package server

import (
	"context"
	"encoding/json"
	"errors"
	"net"
	"net/http"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/johejo/bigquery-emulator-duckdb/internal/serverprocess"
)

func binary(t *testing.T) string {
	t.Helper()
	path := os.Getenv("BQ_EMULATOR_BINARY")
	if path == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set; run just e2e")
	}
	return path
}

func TestInvalidPort(t *testing.T) {
	for _, port := range []string{"-1", "65536", "999999999999999999", "abc", "1x"} {
		t.Run(port, func(t *testing.T) {
			rejected(t, port, 2)
		})
	}
}

func TestInvalidGRPCPort(t *testing.T) {
	for _, port := range []string{"-1", "65536", "999999999999999999", "abc", "1x"} {
		t.Run(port, func(t *testing.T) { rejectedListener(t, "--grpc-port", port, 2) })
	}
}

func TestOccupiedGRPCPort(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	rejectedListener(t, "--grpc-port", strconv.Itoa(listener.Addr().(*net.TCPAddr).Port), 1)
}

func TestOccupiedPort(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	rejected(t, strconv.Itoa(listener.Addr().(*net.TCPAddr).Port), 1)
}

func rejected(t *testing.T, port string, code int) {
	t.Helper()
	rejectedListener(t, "--http-port", port, code)
}

func rejectedListener(t *testing.T, flag, port string, code int) {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	output, err := exec.CommandContext(ctx, binary(t), "serve", "--host", "127.0.0.1", "--http-port", "0", "--grpc-port", "0", flag, port).CombinedOutput()
	var exit *exec.ExitError
	if ctx.Err() != nil || !errors.As(err, &exit) || exit.ExitCode() != code || strings.Contains(string(output), "listening on") {
		t.Fatalf("expected exit %d before listening: %v\n%s", code, err, output)
	}
}

// The startup announcement lets callers immediately shut down, including before any request.
func TestShutdownAfterAnnouncement(t *testing.T) {
	for range 10 {
		ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
		p, err := serverprocess.StartEmulator(ctx, binary(t), "", "--host", "127.0.0.1", "--http-port", "0")
		cancel()
		if err != nil {
			t.Fatal(err)
		}
		if err := p.Stop(); err != nil {
			t.Fatal(err)
		}
	}
}

func start(t *testing.T, args ...string) *serverprocess.Emulator {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	p, err := serverprocess.StartEmulator(ctx, binary(t), "", append([]string{"--http-port", "0"}, args...)...)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := p.Stop(); err != nil {
			t.Error(err)
		}
	})
	return p
}

// Clients such as test suites open many connections at once; with a short listen backlog the
// kernel resets the ones that do not fit instead of queueing them.
func TestBurstOfConnections(t *testing.T) {
	p := start(t, "--host", "127.0.0.1")
	const clients = 100
	begin := make(chan struct{})
	errs := make(chan error, clients)
	var workers sync.WaitGroup
	for range clients {
		workers.Go(func() {
			client := &http.Client{Transport: &http.Transport{}}
			defer client.CloseIdleConnections()
			<-begin
			response, err := client.Get(p.URL + "/bigquery/v2/projects")
			if err != nil {
				errs <- err
				return
			}
			_ = response.Body.Close()
			if response.StatusCode != http.StatusOK {
				errs <- errors.New(response.Status)
			}
		})
	}
	close(begin)
	workers.Wait()
	close(errs)
	for err := range errs {
		t.Error(err)
	}
}

// URLs the emulator returns, such as the discovery document's rootUrl, bracket an IPv6 host and
// name the loopback address for the wildcard one.
func TestIPv6Host(t *testing.T) {
	listener, err := net.Listen("tcp6", "[::1]:0")
	if err != nil {
		t.Skipf("IPv6 loopback is unavailable: %v", err)
	}
	_ = listener.Close()
	for _, host := range []string{"::", "::1"} {
		t.Run(host, func(t *testing.T) {
			p := start(t, "--host", host)
			if !strings.HasPrefix(p.URL, "http://[::1]:") {
				t.Fatalf("announced %s", p.URL)
			}
			response, err := http.Get(p.URL + "/$discovery/rest")
			if err != nil {
				t.Fatal(err)
			}
			defer response.Body.Close()
			var document struct {
				RootURL string `json:"rootUrl"`
			}
			if err := json.NewDecoder(response.Body).Decode(&document); err != nil {
				t.Fatal(err)
			}
			if document.RootURL != p.URL+"/" {
				t.Fatalf("rootUrl = %q, want %q", document.RootURL, p.URL+"/")
			}
		})
	}
}
