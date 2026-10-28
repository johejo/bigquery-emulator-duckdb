package serverprocess

import (
	"context"
	"errors"
	"fmt"
	"net"
	"os/exec"
	"syscall"
)

// Postgres is an empty PostgreSQL server that listens only on TCP on the loopback address.
type Postgres struct {
	// Conninfo is a libpq connection string for the database postgres as the user postgres.
	Conninfo string
	process  *process
}

// StartPostgres creates a database cluster in dir, which must not exist or be empty, and starts
// a server on it, returning once it accepts connections or ctx ends.
// initdb, postgres and pg_isready must be on PATH.
func StartPostgres(ctx context.Context, dir string) (*Postgres, error) {
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
	p := &Postgres{Conninfo: fmt.Sprintf("host=127.0.0.1 port=%d user=postgres dbname=postgres", port)}
	cmd := exec.Command("postgres", "-D", dir, "-p", fmt.Sprint(port), "-k", "",
		"-c", "listen_addresses=127.0.0.1")
	p.process, err = startProcess(cmd, nil)
	if err != nil {
		return nil, err
	}
	if err := p.process.poll(ctx, func(ctx context.Context) bool {
		ready := exec.CommandContext(ctx, "pg_isready", "--host", "127.0.0.1", "--port", fmt.Sprint(port))
		return ready.Run() == nil
	}); err != nil {
		return nil, errors.Join(fmt.Errorf("waiting for PostgreSQL: %w", err), p.Stop())
	}
	return p, nil
}

// Stop shuts the server down without waiting for its clients to disconnect.
func (p *Postgres) Stop() error {
	return p.process.stop(syscall.SIGINT, false, 0)
}
