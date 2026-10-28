package serverprocess

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
)

// Spanner owns an isolated emulator, with one empty GoogleSQL database.
type Spanner struct {
	Host     string
	Database string
	process  *process
}

// StartSpanner runs the standalone Spanner emulator on free loopback ports.
// SPANNER_EMULATOR_GATEWAY selects gateway_main, as the Nix development shell does.
func StartSpanner(ctx context.Context) (*Spanner, error) {
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
	p := &Spanner{Host: host, Database: "projects/test/instances/test/databases/test"}
	gateway := os.Getenv("SPANNER_EMULATOR_GATEWAY")
	if gateway == "" {
		gateway = "gateway_main"
	}
	cmd := exec.Command(gateway, "--hostname=127.0.0.1", fmt.Sprintf("--grpc_port=%d", grpcPort), fmt.Sprintf("--http_port=%d", restPort))
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	cmd.Stderr = os.Stderr
	if err := grpc.Close(); err != nil {
		return nil, err
	}
	if err := rest.Close(); err != nil {
		return nil, err
	}
	p.process, err = startProcess(cmd, nil)
	if err != nil {
		return nil, err
	}
	if err := p.process.poll(ctx, func(ctx context.Context) bool {
		request, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://"+restHost+"/v1/projects/test/instances", nil)
		if err != nil {
			return false
		}
		response, err := http.DefaultClient.Do(request)
		if err != nil {
			return false
		}
		response.Body.Close()
		return response.StatusCode == http.StatusOK
	}); err != nil {
		return nil, errors.Join(fmt.Errorf("waiting for Spanner emulator: %w", err), p.Stop())
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
func (p *Spanner) Stop() error {
	return p.process.stop(syscall.SIGTERM, true, 0)
}
