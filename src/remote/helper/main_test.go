package main

import (
	"bytes"
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

type fakeBridge struct {
	state    []byte
	frame    []byte
	commands []map[string]any
}

func (b *fakeBridge) LastState() []byte { return append([]byte(nil), b.state...) }

func (b *fakeBridge) WaitForFrame(ctx context.Context, lastSeq uint64) ([]byte, uint64, error) {
	if lastSeq == 0 {
		return append([]byte(nil), b.frame...), 1, nil
	}
	return nil, lastSeq, context.Canceled
}

func (b *fakeBridge) RegisterListener() chan []byte { return make(chan []byte, 1) }

func (b *fakeBridge) UnregisterListener(ch chan []byte) { close(ch) }

func (b *fakeBridge) SendCommand(command map[string]any) error {
	b.commands = append(b.commands, command)
	return nil
}

func newTestServer(t *testing.T, bridge *fakeBridge) *Server {
	t.Helper()
	webRoot := t.TempDir()
	if err := os.WriteFile(filepath.Join(webRoot, "index.html"), []byte("<!doctype html><title>Switch Remote</title>"), 0o600); err != nil {
		t.Fatal(err)
	}
	return &Server{token: "secret", bridge: bridge, webRoot: webRoot}
}

func TestIndexRequiresTokenAndServesBundledAsset(t *testing.T) {
	server := newTestServer(t, &fakeBridge{})

	unauthorized := httptest.NewRecorder()
	server.handleIndex(unauthorized, httptest.NewRequest(http.MethodGet, "/", nil))
	if unauthorized.Code != http.StatusUnauthorized {
		t.Fatalf("expected unauthorized without token, got %d", unauthorized.Code)
	}

	ok := httptest.NewRecorder()
	server.handleIndex(ok, httptest.NewRequest(http.MethodGet, "/?token=secret", nil))
	if ok.Code != http.StatusOK {
		t.Fatalf("expected bundled index, got %d", ok.Code)
	}
	if !bytes.Contains(ok.Body.Bytes(), []byte("Switch Remote")) {
		t.Fatal("index response did not include bundled remote UI")
	}
}

func TestCommandPostSendsBridgeCommand(t *testing.T) {
	bridge := &fakeBridge{}
	server := newTestServer(t, bridge)

	body := bytes.NewBufferString(`{"command":"cut"}`)
	response := httptest.NewRecorder()
	server.handleCommand(response, httptest.NewRequest(http.MethodPost, "/api/command?token=secret", body))

	if response.Code != http.StatusOK {
		t.Fatalf("expected command accepted, got %d", response.Code)
	}
	if len(bridge.commands) != 1 || bridge.commands[0]["type"] != "command" || bridge.commands[0]["command"] != "cut" {
		t.Fatalf("unexpected commands: %#v", bridge.commands)
	}
}

func TestEventsStreamInitialState(t *testing.T) {
	bridge := &fakeBridge{state: []byte(`{"enabled":true}`)}
	server := newTestServer(t, bridge)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	request := httptest.NewRequest(http.MethodGet, "/events?token=secret", nil).WithContext(ctx)
	response := httptest.NewRecorder()

	server.handleEvents(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("expected events stream, got %d", response.Code)
	}
	if !bytes.Contains(response.Body.Bytes(), []byte("event: state")) {
		t.Fatal("events stream did not include initial state event")
	}
}

func TestFeedStreamsFrame(t *testing.T) {
	bridge := &fakeBridge{frame: []byte{0xff, 0xd8, 0xff, 0xd9}}
	server := newTestServer(t, bridge)
	request := httptest.NewRequest(http.MethodGet, "/feed.mjpg?token=secret", nil)
	response := httptest.NewRecorder()

	server.handleFeed(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("expected feed stream, got %d", response.Code)
	}
	if !bytes.Contains(response.Body.Bytes(), bridge.frame) {
		t.Fatal("feed stream did not include jpeg frame")
	}
}

func TestResolveWebRootRequiresIndex(t *testing.T) {
	webRoot := t.TempDir()
	if _, err := resolveWebRoot(webRoot); err == nil {
		t.Fatal("expected missing index.html to fail")
	}
	if err := os.WriteFile(filepath.Join(webRoot, "index.html"), []byte("<!doctype html>"), 0o600); err != nil {
		t.Fatal(err)
	}
	resolved, err := resolveWebRoot(webRoot)
	if err != nil {
		t.Fatal(err)
	}
	if resolved != webRoot {
		t.Fatalf("expected %q, got %q", webRoot, resolved)
	}
}

func TestCommandRejectsInvalidJSON(t *testing.T) {
	server := newTestServer(t, &fakeBridge{})
	response := httptest.NewRecorder()

	server.handleCommand(response, httptest.NewRequest(http.MethodPost, "/api/command?token=secret", bytes.NewBufferString("{")))

	if response.Code != http.StatusBadRequest {
		t.Fatalf("expected invalid json rejection, got %d", response.Code)
	}
}
