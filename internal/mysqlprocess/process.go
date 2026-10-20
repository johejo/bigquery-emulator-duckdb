// Package mysqlprocess runs a throwaway MySQL server for EXTERNAL_QUERY tests and probes.
// mysqld and mysql must be on PATH.
package mysqlprocess

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

// Process is an empty MySQL database, accessible only on the loopback address.
type Process struct {
	// Conninfo includes the mysql: discriminator used by --connection.
	Conninfo  string
	cmd       *exec.Cmd
	done      chan struct{}
	err       error // Written before done is closed.
	socketDir string
}

// Start initializes dir and starts a server, returning once its database is ready or ctx ends.
func Start(ctx context.Context, dir string) (*Process, error) {
	if err := os.MkdirAll(dir, 0700); err != nil {
		return nil, err
	}
	initialize := exec.CommandContext(ctx, "mysqld", "--no-defaults", "--initialize-insecure", "--datadir="+dir)
	if output, err := initialize.CombinedOutput(); err != nil {
		return nil, fmt.Errorf("initializing MySQL: %w\n%s", err, output)
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	port := listener.Addr().(*net.TCPAddr).Port
	if err := listener.Close(); err != nil {
		return nil, err
	}
	// macOS temporary directories can exceed the Unix socket path limit.
	socketDir, err := os.MkdirTemp("/tmp", "bigquery-mysql-")
	if err != nil {
		return nil, err
	}
	p := &Process{
		Conninfo: fmt.Sprintf("mysql:host=127.0.0.1 port=%d user=root database=external_query ssl_mode=disabled", port),
		cmd: exec.Command("mysqld", "--no-defaults", "--datadir="+dir, "--bind-address=127.0.0.1",
			fmt.Sprintf("--port=%d", port), "--socket="+filepath.Join(socketDir, "mysql.sock"),
			"--mysqlx=OFF", "--default-time-zone=+09:00", "--log-error="+filepath.Join(dir, "server.log")),
		done: make(chan struct{}), socketDir: socketDir,
	}
	if err := p.cmd.Start(); err != nil {
		_ = os.RemoveAll(socketDir)
		return nil, err
	}
	go func() { p.err = p.cmd.Wait(); close(p.done) }()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		ready := exec.CommandContext(ctx, "mysql", "--no-defaults", "--protocol=TCP", "--host=127.0.0.1",
			fmt.Sprintf("--port=%d", port), "--user=root", "--execute=CREATE DATABASE IF NOT EXISTS external_query")
		if ready.Run() == nil {
			return p, nil
		}
		select {
		case <-ctx.Done():
			return nil, errors.Join(fmt.Errorf("waiting for MySQL: %w", ctx.Err()), p.Stop())
		case <-p.done:
			_ = os.RemoveAll(socketDir)
			log, _ := os.ReadFile(filepath.Join(dir, "server.log"))
			return nil, fmt.Errorf("MySQL exited before accepting connections: %v\n%s", p.err, log)
		case <-ticker.C:
		}
	}
}

// Execute runs fixture SQL through the server's Unix socket.
func (p *Process) Execute(ctx context.Context, sql string) error {
	cmd := exec.CommandContext(ctx, "mysql", "--no-defaults", "--user=root",
		"--socket="+filepath.Join(p.socketDir, "mysql.sock"), "external_query")
	cmd.Stdin = strings.NewReader(sql)
	var output bytes.Buffer
	cmd.Stdout, cmd.Stderr = &output, &output
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("MySQL fixture: %w\n%s", err, output.String())
	}
	return nil
}

// Stop shuts down the server and removes its Unix socket directory.
func (p *Process) Stop() error {
	defer os.RemoveAll(p.socketDir)
	select {
	case <-p.done:
		return nil
	default:
	}
	if err := p.cmd.Process.Signal(syscall.SIGTERM); err != nil {
		return err
	}
	<-p.done
	return nil
}
