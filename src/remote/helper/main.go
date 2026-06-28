package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"sync"
	"syscall"
	"time"
)

type RemoteBridge interface {
	LastState() []byte
	WaitForFrame(ctx context.Context, lastSeq uint64) ([]byte, uint64, error)
	RegisterListener() chan []byte
	UnregisterListener(ch chan []byte)
	SendCommand(command map[string]any) error
}

type Bridge struct {
	conn        net.Conn
	connMu      sync.Mutex
	stateMu     sync.RWMutex
	lastState   []byte
	frameMu     sync.Mutex
	lastFrame   []byte
	frameSeq    uint64
	listenersMu sync.Mutex
	listeners   map[chan []byte]struct{}
}

func NewBridge(conn net.Conn) *Bridge {
	b := &Bridge{
		conn:      conn,
		listeners: make(map[chan []byte]struct{}),
	}
	return b
}

func (b *Bridge) Run() error {
	for {
		msg, payload, err := readPacket(b.conn)
		if err != nil {
			return err
		}

		msgType, _ := msg["type"].(string)
		switch msgType {
		case "state":
			if state, ok := msg["state"]; ok {
				data, err := json.Marshal(state)
				if err == nil {
					b.stateMu.Lock()
					b.lastState = data
					b.stateMu.Unlock()
					b.broadcast(data)
				}
			}
		case "frame":
			if len(payload) == 0 {
				continue
			}
			b.frameMu.Lock()
			b.lastFrame = append(b.lastFrame[:0], payload...)
			b.frameSeq++
			b.frameMu.Unlock()
		}
	}
}

func (b *Bridge) LastState() []byte {
	b.stateMu.RLock()
	defer b.stateMu.RUnlock()
	if len(b.lastState) == 0 {
		return nil
	}
	return append([]byte(nil), b.lastState...)
}

func (b *Bridge) WaitForFrame(ctx context.Context, lastSeq uint64) ([]byte, uint64, error) {
	for {
		b.frameMu.Lock()
		if b.frameSeq != lastSeq {
			frame := append([]byte(nil), b.lastFrame...)
			seq := b.frameSeq
			b.frameMu.Unlock()
			return frame, seq, nil
		}
		b.frameMu.Unlock()

		select {
		case <-ctx.Done():
			return nil, lastSeq, ctx.Err()
		case <-time.After(100 * time.Millisecond):
		}
	}
}

func (b *Bridge) RegisterListener() chan []byte {
	ch := make(chan []byte, 8)
	b.listenersMu.Lock()
	b.listeners[ch] = struct{}{}
	b.listenersMu.Unlock()
	return ch
}

func (b *Bridge) UnregisterListener(ch chan []byte) {
	b.listenersMu.Lock()
	delete(b.listeners, ch)
	b.listenersMu.Unlock()
	close(ch)
}

func (b *Bridge) broadcast(state []byte) {
	b.listenersMu.Lock()
	defer b.listenersMu.Unlock()
	for ch := range b.listeners {
		select {
		case ch <- state:
		default:
		}
	}
}

func (b *Bridge) SendCommand(command map[string]any) error {
	b.connMu.Lock()
	defer b.connMu.Unlock()
	return writePacket(b.conn, command, nil)
}

type Server struct {
	token   string
	bridge  RemoteBridge
	webRoot string
}

func (s *Server) authenticate(w http.ResponseWriter, r *http.Request) bool {
	if r.URL.Query().Get("token") != s.token {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return false
	}
	return true
}

func (s *Server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if !s.authenticate(w, r) {
		return
	}

	http.ServeFile(w, r, filepath.Join(s.webRoot, "index.html"))
}

func (s *Server) handleEvents(w http.ResponseWriter, r *http.Request) {
	if !s.authenticate(w, r) {
		return
	}

	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")

	ch := s.bridge.RegisterListener()
	defer s.bridge.UnregisterListener(ch)

	if state := s.bridge.LastState(); len(state) > 0 {
		_, _ = fmt.Fprintf(w, "event: state\ndata: %s\n\n", state)
		flusher.Flush()
	}

	heartbeat := time.NewTicker(15 * time.Second)
	defer heartbeat.Stop()

	for {
		select {
		case <-r.Context().Done():
			return
		case state := <-ch:
			_, _ = fmt.Fprintf(w, "event: state\ndata: %s\n\n", state)
			flusher.Flush()
		case <-heartbeat.C:
			_, _ = io.WriteString(w, ":\n\n")
			flusher.Flush()
		}
	}
}

func (s *Server) handleFeed(w http.ResponseWriter, r *http.Request) {
	if !s.authenticate(w, r) {
		return
	}

	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}

	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("Content-Type", "multipart/x-mixed-replace; boundary=frame")

	var seq uint64
	for {
		select {
		case <-r.Context().Done():
			return
		default:
		}

		frame, nextSeq, err := s.bridge.WaitForFrame(r.Context(), seq)
		if err != nil {
			return
		}
		seq = nextSeq
		if len(frame) == 0 {
			continue
		}

		if _, err := io.WriteString(w, "--frame\r\nContent-Type: image/jpeg\r\n\r\n"); err != nil {
			return
		}
		if _, err := w.Write(frame); err != nil {
			return
		}
		if _, err := io.WriteString(w, "\r\n"); err != nil {
			return
		}
		flusher.Flush()
	}
}

func (s *Server) handleCommand(w http.ResponseWriter, r *http.Request) {
	if !s.authenticate(w, r) {
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var body map[string]any
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		http.Error(w, "invalid json", http.StatusBadRequest)
		return
	}

	body["type"] = "command"
	if err := s.bridge.SendCommand(body); err != nil {
		http.Error(w, "bridge unavailable", http.StatusBadGateway)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]any{"ok": true})
}

func readPacket(r io.Reader) (map[string]any, []byte, error) {
	var header [8]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return nil, nil, err
	}

	jsonSize := binary.BigEndian.Uint32(header[0:4])
	payloadSize := binary.BigEndian.Uint32(header[4:8])

	jsonBytes := make([]byte, jsonSize)
	if _, err := io.ReadFull(r, jsonBytes); err != nil {
		return nil, nil, err
	}

	payload := make([]byte, payloadSize)
	if payloadSize > 0 {
		if _, err := io.ReadFull(r, payload); err != nil {
			return nil, nil, err
		}
	}

	var msg map[string]any
	if err := json.Unmarshal(jsonBytes, &msg); err != nil {
		return nil, nil, err
	}

	return msg, payload, nil
}

func writePacket(w io.Writer, msg map[string]any, payload []byte) error {
	jsonBytes, err := json.Marshal(msg)
	if err != nil {
		return err
	}

	header := make([]byte, 8)
	binary.BigEndian.PutUint32(header[0:4], uint32(len(jsonBytes)))
	binary.BigEndian.PutUint32(header[4:8], uint32(len(payload)))

	packet := bytes.NewBuffer(make([]byte, 0, 8+len(jsonBytes)+len(payload)))
	packet.Write(header)
	packet.Write(jsonBytes)
	packet.Write(payload)

	_, err = w.Write(packet.Bytes())
	return err
}

func main() {
	controlPort := flag.Int("control-port", 0, "local control port")
	httpPort := flag.Int("http-port", 8899, "HTTP port")
	token := flag.String("token", "", "access token")
	webRoot := flag.String("web-root", "", "path to Switch Remote web assets")
	flag.Parse()

	if *controlPort == 0 || *token == "" {
		log.Fatal("missing required flags")
	}

	resolvedWebRoot, err := resolveWebRoot(*webRoot)
	if err != nil {
		log.Fatalf("remote web assets unavailable: %v", err)
	}

	conn, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", *controlPort))
	if err != nil {
		log.Fatalf("unable to connect to plugin bridge: %v", err)
	}
	defer conn.Close()

	bridge := NewBridge(conn)
	server := &Server{
		token:   *token,
		bridge:  bridge,
		webRoot: resolvedWebRoot,
	}

	go func() {
		if err := bridge.Run(); err != nil && !errors.Is(err, io.EOF) {
			log.Printf("bridge terminated: %v", err)
		}
		_ = conn.Close()
		os.Exit(1)
	}()

	mux := http.NewServeMux()
	mux.HandleFunc("/", server.handleIndex)
	mux.HandleFunc("/events", server.handleEvents)
	mux.HandleFunc("/feed.mjpg", server.handleFeed)
	mux.HandleFunc("/api/command", server.handleCommand)

	httpServer := &http.Server{
		Addr:              fmt.Sprintf(":%d", *httpPort),
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}

	go func() {
		log.Printf("listening on 0.0.0.0:%d", *httpPort)
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Printf("http server failed: %v", err)
			_ = conn.Close()
			os.Exit(1)
		}
	}()

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	<-sigCh

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_ = httpServer.Shutdown(ctx)
}

func resolveWebRoot(configured string) (string, error) {
	root := configured
	if root == "" {
		exe, err := os.Executable()
		if err != nil {
			return "", err
		}
		root = filepath.Join(filepath.Dir(exe), "web")
	}

	indexPath := filepath.Join(root, "index.html")
	info, err := os.Stat(indexPath)
	if err != nil {
		return "", err
	}
	if info.IsDir() {
		return "", fmt.Errorf("%s is a directory", indexPath)
	}
	return root, nil
}
