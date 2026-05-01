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
	"sync"
	"syscall"
	"time"
)

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
	token  string
	bridge *Bridge
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

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	_, _ = io.WriteString(w, indexHTML)
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
	flag.Parse()

	if *controlPort == 0 || *token == "" {
		log.Fatal("missing required flags")
	}

	conn, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", *controlPort))
	if err != nil {
		log.Fatalf("unable to connect to plugin bridge: %v", err)
	}
	defer conn.Close()

	bridge := NewBridge(conn)
	server := &Server{
		token:  *token,
		bridge: bridge,
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
		log.Printf("listening on http://0.0.0.0:%d", *httpPort)
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

const indexHTML = `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Switcher Remote</title>
  <style>
    :root {
      color-scheme: dark;
      --bg: #020617;
      --panel: rgba(15, 23, 42, 0.92);
      --border: rgba(148, 163, 184, 0.24);
      --text: #f8fafc;
      --muted: #94a3b8;
      --green: #22c55e;
      --red: #ef4444;
      --orange: #f97316;
      --white: #f8fafc;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background:
        radial-gradient(circle at top left, rgba(34, 197, 94, 0.18), transparent 28%),
        radial-gradient(circle at top right, rgba(239, 68, 68, 0.18), transparent 24%),
        linear-gradient(180deg, #020617 0%, #0f172a 100%);
      color: var(--text);
      font: 500 15px/1.4 "SF Pro Display", "Segoe UI", sans-serif;
    }
    .shell {
      width: min(1480px, calc(100vw - 32px));
      margin: 0 auto;
      padding: 20px 0 32px;
    }
    .toolbar {
      display: grid;
      grid-template-columns: 1fr auto;
      gap: 16px;
      align-items: center;
      padding: 16px 18px;
      border: 1px solid var(--border);
      border-radius: 20px;
      background: var(--panel);
      backdrop-filter: blur(18px);
      box-shadow: 0 18px 54px rgba(2, 6, 23, 0.42);
    }
    .headline {
      display: flex;
      flex-wrap: wrap;
      gap: 8px 18px;
      align-items: baseline;
    }
    .headline h1 {
      margin: 0;
      font-size: clamp(20px, 2.5vw, 32px);
      letter-spacing: -0.03em;
    }
    .meta {
      color: var(--muted);
      font-size: 13px;
      display: flex;
      gap: 12px;
      flex-wrap: wrap;
    }
    .actions {
      display: flex;
      gap: 10px;
      align-items: center;
      flex-wrap: wrap;
      justify-content: flex-end;
    }
    button {
      border: 1px solid transparent;
      border-radius: 999px;
      padding: 12px 18px;
      font: inherit;
      font-weight: 700;
      color: var(--text);
      cursor: pointer;
      transition: transform 120ms ease, opacity 120ms ease, border-color 120ms ease;
    }
    button:disabled {
      opacity: 0.45;
      cursor: not-allowed;
      transform: none;
    }
    button:hover:not(:disabled) {
      transform: translateY(-1px);
    }
    .cut { background: linear-gradient(135deg, #ef4444, #b91c1c); }
    .auto { background: linear-gradient(135deg, #22c55e, #166534); }
    .ghost {
      background: transparent;
      border-color: var(--border);
    }
    .stage-card {
      margin-top: 18px;
      border: 1px solid var(--border);
      border-radius: 24px;
      overflow: hidden;
      background: rgba(2, 6, 23, 0.92);
      box-shadow: 0 18px 54px rgba(2, 6, 23, 0.42);
    }
    .stage {
      position: relative;
      aspect-ratio: 16 / 9;
      background: #020617;
    }
    .stage img {
      display: block;
      width: 100%;
      height: 100%;
      object-fit: cover;
      background: #020617;
    }
    .overlay {
      position: absolute;
      inset: 0;
      pointer-events: none;
    }
    .tile-button {
      position: absolute;
      pointer-events: auto;
      border-radius: 16px;
      background: transparent;
      border: 2px solid rgba(148, 163, 184, 0.18);
      box-shadow: inset 0 0 0 1px rgba(255,255,255,0.02);
    }
    .tile-button.selected { border-color: var(--white); }
    .tile-button.preview { border-color: var(--green); }
    .tile-button.program { border-color: var(--red); }
    .tile-button.program.preview { border-color: var(--orange); }
    .tile-button.empty { cursor: default; }
    .status-strip {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 0;
      border-top: 1px solid var(--border);
      background: rgba(15, 23, 42, 0.92);
    }
    .status-item {
      padding: 14px 18px;
      border-right: 1px solid var(--border);
    }
    .status-item:last-child { border-right: none; }
    .status-item strong {
      display: block;
      margin-bottom: 2px;
      font-size: 12px;
      letter-spacing: 0.08em;
      text-transform: uppercase;
      color: var(--muted);
    }
    .status-item span {
      display: block;
      font-size: 15px;
      word-break: break-word;
    }
    .empty {
      display: flex;
      align-items: center;
      justify-content: center;
      min-height: 100vh;
      color: var(--muted);
    }
    @media (max-width: 900px) {
      .toolbar {
        grid-template-columns: 1fr;
      }
      .actions {
        justify-content: stretch;
      }
      .actions button {
        flex: 1;
      }
      .status-strip {
        grid-template-columns: 1fr;
      }
      .status-item {
        border-right: none;
        border-bottom: 1px solid var(--border);
      }
      .status-item:last-child {
        border-bottom: none;
      }
    }
  </style>
</head>
<body>
  <div class="shell">
    <div class="toolbar">
      <div class="headline">
        <h1>Switcher Remote</h1>
        <div class="meta">
          <span id="connection">Connecting...</span>
          <span id="transition">Transition: --</span>
          <span id="mode">Mode: --</span>
        </div>
      </div>
      <div class="actions">
        <button class="ghost" id="refresh">Reconnect</button>
        <button class="cut" id="cut">CUT</button>
        <button class="auto" id="auto">AUTO</button>
      </div>
    </div>
    <div class="stage-card">
      <div class="stage">
        <img id="feed" alt="Switcher remote feed">
        <div id="overlay" class="overlay"></div>
      </div>
      <div class="status-strip">
        <div class="status-item">
          <strong>Status</strong>
          <span id="status">Waiting for state…</span>
        </div>
        <div class="status-item">
          <strong>Selection</strong>
          <span id="selection">None</span>
        </div>
        <div class="status-item">
          <strong>Program</strong>
          <span id="program">None</span>
        </div>
      </div>
    </div>
  </div>
  <script>
    const params = new URLSearchParams(window.location.search);
    const token = params.get('token') || '';
    const feed = document.getElementById('feed');
    const overlay = document.getElementById('overlay');
    const cutButton = document.getElementById('cut');
    const autoButton = document.getElementById('auto');
    const reconnectButton = document.getElementById('refresh');
    const statusEl = document.getElementById('status');
    const selectionEl = document.getElementById('selection');
    const programEl = document.getElementById('program');
    const connectionEl = document.getElementById('connection');
    const transitionEl = document.getElementById('transition');
    const modeEl = document.getElementById('mode');

    let currentState = null;
    let eventSource = null;

    function command(command, extra = {}) {
      return fetch('/api/command?token=' + encodeURIComponent(token), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ command, ...extra }),
      });
    }

    function slotText(slotIndex) {
      if (!currentState || slotIndex == null || slotIndex < 0) return 'None';
      const slot = (currentState.slots || []).find((entry) => entry.index === slotIndex);
      return slot ? slot.title : 'None';
    }

    function renderState(state) {
      currentState = state;
      statusEl.textContent = state.status || 'Remote unavailable';
      selectionEl.textContent = slotText(state.selectedSlotIndex);
      programEl.textContent = slotText(state.programSlotIndex);
      transitionEl.textContent = 'Transition: ' + (state.transitionDurationMs ?? '--') + ' ms';
      modeEl.textContent = 'Mode: ' + (state.previewProgramMode ? 'Studio' : 'Direct');
      cutButton.disabled = !state.enabled || state.selectedSlotIndex == null || state.selectedSlotIndex < 0;
      autoButton.disabled = !state.enabled || state.selectedSlotIndex == null || state.selectedSlotIndex < 0;
      overlay.replaceChildren();

      for (const slot of state.slots || []) {
        const rect = slot.rect;
        if (!rect) continue;
        const button = document.createElement('button');
        button.type = 'button';
        button.className = 'tile-button' +
          (slot.selected ? ' selected' : '') +
          (slot.preview ? ' preview' : '') +
          (slot.program ? ' program' : '') +
          (!slot.hasSource ? ' empty' : '');
        button.style.left = (rect.x * 100) + '%';
        button.style.top = (rect.y * 100) + '%';
        button.style.width = (rect.width * 100) + '%';
        button.style.height = (rect.height * 100) + '%';
        button.title = slot.title || ('Slot ' + (slot.index + 1));
        button.disabled = !slot.hasSource;
        if (slot.hasSource) {
          button.addEventListener('click', () => command('select_preview_slot', { slotIndex: slot.index }));
        }
        overlay.appendChild(button);
      }
    }

    function connectEvents() {
      if (eventSource) eventSource.close();
      connectionEl.textContent = 'Connecting…';
      eventSource = new EventSource('/events?token=' + encodeURIComponent(token));
      eventSource.addEventListener('state', (event) => {
        connectionEl.textContent = 'Connected';
        renderState(JSON.parse(event.data));
      });
      eventSource.onerror = () => {
        connectionEl.textContent = 'Disconnected, retrying…';
      };
    }

    reconnectButton.addEventListener('click', () => {
      connectEvents();
      feed.src = '/feed.mjpg?token=' + encodeURIComponent(token) + '&t=' + Date.now();
    });
    cutButton.addEventListener('click', () => command('cut'));
    autoButton.addEventListener('click', () => command('auto'));

    feed.src = '/feed.mjpg?token=' + encodeURIComponent(token);
    connectEvents();
  </script>
</body>
</html>`
