package server

import (
	"context"
	"errors"
	"net"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
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
	rejectedListener(t, "--port", port, code)
}

func rejectedListener(t *testing.T, flag, port string, code int) {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	output, err := exec.CommandContext(ctx, binary(t), "--host", "127.0.0.1", "--port", "0", "--grpc-port", "0", flag, port).CombinedOutput()
	var exit *exec.ExitError
	if ctx.Err() != nil || !errors.As(err, &exit) || exit.ExitCode() != code || strings.Contains(string(output), "listening on") {
		t.Fatalf("expected exit %d before listening: %v\n%s", code, err, output)
	}
}

// The startup announcement lets callers immediately shut down, including before any request.
func TestShutdownAfterAnnouncement(t *testing.T) {
	for range 10 {
		ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
		p, err := emulatorprocess.Start(ctx, binary(t), "", "--host", "127.0.0.1", "--port", "0")
		cancel()
		if err != nil {
			t.Fatal(err)
		}
		if err := p.Stop(); err != nil {
			t.Fatal(err)
		}
	}
}
