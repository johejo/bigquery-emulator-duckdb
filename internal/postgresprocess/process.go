// Package postgresprocess runs a throwaway PostgreSQL server for EXTERNAL_QUERY in tests and
// probes. initdb, postgres and pg_isready must be on PATH.
package postgresprocess

import (
	"context"
	"errors"
	"fmt"
	"net"
	"os/exec"
	"syscall"
	"time"
)

// Process is an empty PostgreSQL server that listens only on TCP on the loopback address.
type Process struct {
	// Conninfo is a libpq connection string for the database postgres as the user postgres.
	Conninfo string
	cmd      *exec.Cmd
	done     chan struct{}
	err      error // Written before done is closed.
}

// Start creates a database cluster in dir, which must not exist or be empty, and starts a server
// on it, returning once it accepts connections or ctx ends.
func Start(ctx context.Context, dir string) (*Process, error) {
	initdb := exec.CommandContext(ctx, "initdb", "--pgdata", dir, "--username", "postgres",
		"--auth", "trust", "--encoding", "UTF8", "--locale", "C")
	if output, err := initdb.CombinedOutput(); err != nil {
		return nil, fmt.Errorf("initdb: %w\n%s", err, output)
	}
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	port := listener.Addr().(*net.TCPAddr).Port
	if err := listener.Close(); err != nil {
		return nil, err
	}
	// An empty socket directory keeps the server off Unix sockets, whose paths would exceed
	// the length limit under a long temporary directory.
	p := &Process{
		Conninfo: fmt.Sprintf("host=127.0.0.1 port=%d user=postgres dbname=postgres", port),
		cmd: exec.Command("postgres", "-D", dir, "-p", fmt.Sprint(port), "-k", "",
			"-c", "listen_addresses=127.0.0.1"),
		done: make(chan struct{}),
	}
	if err := p.cmd.Start(); err != nil {
		return nil, err
	}
	go func() {
		p.err = p.cmd.Wait()
		close(p.done)
	}()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		ready := exec.CommandContext(ctx, "pg_isready", "--host", "127.0.0.1", "--port", fmt.Sprint(port))
		if ready.Run() == nil {
			return p, nil
		}
		select {
		case <-ctx.Done():
			return nil, errors.Join(fmt.Errorf("waiting for PostgreSQL: %w", ctx.Err()), p.Stop())
		case <-p.done:
			return nil, fmt.Errorf("PostgreSQL exited before accepting connections: %v", p.err)
		case <-ticker.C:
		}
	}
}

// Stop shuts the server down without waiting for its clients to disconnect.
func (p *Process) Stop() error {
	select {
	case <-p.done:
		return nil
	default:
	}
	if err := p.cmd.Process.Signal(syscall.SIGINT); err != nil {
		return err
	}
	<-p.done
	return nil
}
