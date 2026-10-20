// Package spannerprocess runs the standalone Spanner emulator for federated-query E2E tests.
package spannerprocess

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"syscall"
	"time"
)

// Process owns an isolated emulator, with one empty GoogleSQL database.
type Process struct {
	Host     string
	Database string
	cmd      *exec.Cmd
	done     chan struct{}
	err      error
}

// Start runs the standalone Spanner emulator on free loopback ports.
// SPANNER_EMULATOR_GATEWAY selects gateway_main, as the Nix development shell does.
func Start(ctx context.Context) (*Process, error) {
	grpc, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	defer grpc.Close()
	rest, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	defer rest.Close()
	host := grpc.Addr().String()
	restHost := rest.Addr().String()
	grpcPort := grpc.Addr().(*net.TCPAddr).Port
	restPort := rest.Addr().(*net.TCPAddr).Port
	p := &Process{Host: host, Database: "projects/test/instances/test/databases/test", done: make(chan struct{})}
	gateway := os.Getenv("SPANNER_EMULATOR_GATEWAY")
	if gateway == "" {
		gateway = "gateway_main"
	}
	p.cmd = exec.Command(gateway, "--hostname=127.0.0.1", fmt.Sprintf("--grpc_port=%d", grpcPort), fmt.Sprintf("--http_port=%d", restPort))
	p.cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	p.cmd.Stderr = os.Stderr
	if err := grpc.Close(); err != nil {
		return nil, err
	}
	if err := rest.Close(); err != nil {
		return nil, err
	}
	if err := p.cmd.Start(); err != nil {
		return nil, err
	}
	go func() { p.err = p.cmd.Wait(); close(p.done) }()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		request, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://"+restHost+"/v1/projects/test/instances", nil)
		if err != nil {
			return nil, errors.Join(err, p.Stop())
		}
		response, err := http.DefaultClient.Do(request)
		if err == nil {
			response.Body.Close()
			if response.StatusCode == http.StatusOK {
				break
			}
		}
		select {
		case <-ctx.Done():
			return nil, errors.Join(fmt.Errorf("waiting for Spanner emulator: %w", ctx.Err()), p.Stop())
		case <-p.done:
			return nil, fmt.Errorf("spanner emulator exited before accepting connections: %v", p.err)
		case <-ticker.C:
		}
	}
	for _, operation := range []struct{ path, body string }{
		{"/v1/projects/test/instances", `{"instanceId":"test","instance":{"config":"emulator-config","displayName":"test","nodeCount":1}}`},
		{"/v1/projects/test/instances/test/databases", `{"createStatement":"CREATE DATABASE test"}`},
	} {
		request, err := http.NewRequestWithContext(ctx, http.MethodPost, "http://"+restHost+operation.path, bytes.NewBufferString(operation.body))
		if err != nil {
			return nil, errors.Join(err, p.Stop())
		}
		request.Header.Set("Content-Type", "application/json")
		response, err := http.DefaultClient.Do(request)
		if err != nil {
			return nil, errors.Join(err, p.Stop())
		}
		body, readErr := io.ReadAll(response.Body)
		response.Body.Close()
		if readErr != nil || response.StatusCode != http.StatusOK {
			return nil, errors.Join(fmt.Errorf("initialize Spanner emulator: HTTP %d: %s", response.StatusCode, body), readErr, p.Stop())
		}
	}
	return p, nil
}

// Stop shuts down the gateway and its emulator child processes.
func (p *Process) Stop() error {
	select {
	case <-p.done:
		return nil
	default:
	}
	if err := syscall.Kill(-p.cmd.Process.Pid, syscall.SIGTERM); err != nil {
		return err
	}
	<-p.done
	return nil
}
