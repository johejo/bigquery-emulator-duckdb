package serverprocess

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
)

// MySQL is an empty MySQL database, accessible only on the loopback address.
type MySQL struct {
	// Conninfo includes the mysql: discriminator used by --connection.
	Conninfo  string
	process   *process
	socketDir string
}

// StartMySQL initializes dir and starts a server, returning once its database is ready or ctx ends.
// mysqld and mysql must be on PATH.
func StartMySQL(ctx context.Context, dir string) (*MySQL, error) {
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
	p := &MySQL{
		Conninfo:  fmt.Sprintf("mysql:host=127.0.0.1 port=%d user=root database=external_query ssl_mode=disabled", port),
		socketDir: socketDir,
	}
	cmd := exec.Command("mysqld", "--no-defaults", "--datadir="+dir, "--bind-address=127.0.0.1",
		fmt.Sprintf("--port=%d", port), "--socket="+filepath.Join(socketDir, "mysql.sock"),
		"--mysqlx=OFF", "--default-time-zone=+09:00", "--log-error="+filepath.Join(dir, "server.log"))
	p.process, err = startProcess(cmd, nil)
	if err != nil {
		_ = os.RemoveAll(socketDir)
		return nil, err
	}
	if err := p.process.poll(ctx, func(ctx context.Context) bool {
		ready := exec.CommandContext(ctx, "mysql", "--no-defaults", "--protocol=TCP", "--host=127.0.0.1",
			fmt.Sprintf("--port=%d", port), "--user=root", "--execute=CREATE DATABASE IF NOT EXISTS external_query")
		return ready.Run() == nil
	}); err != nil {
		stopErr := p.Stop()
		log, _ := os.ReadFile(filepath.Join(dir, "server.log"))
		return nil, errors.Join(fmt.Errorf("waiting for MySQL: %w\n%s", err, log), stopErr)
	}
	return p, nil
}

// Execute runs fixture SQL through the server's Unix socket.
func (p *MySQL) Execute(ctx context.Context, sql string) error {
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
func (p *MySQL) Stop() error {
	defer os.RemoveAll(p.socketDir)
	return p.process.stop(syscall.SIGTERM, false, 0)
}
