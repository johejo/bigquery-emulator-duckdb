// Package emulatorprocess manages isolated emulator processes for tests and probes.
//
// Set EMULATOR_LOG_DIR to a directory to also write each process's stderr to a file of its
// own there, such as for CI to keep the server side of a failing test.
package emulatorprocess

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"strings"
	"sync"
	"syscall"
	"time"
)

const listening = "bigquery-emulator-duckdb listening on "
const grpcListening = "BigQuery Storage gRPC listening on "
const logDirEnv = "EMULATOR_LOG_DIR"

// Process is an emulator whose startup URL has been read from stderr.
// Stop must be called before removing its data directory.
type Process struct {
	URL  string
	GRPC string
	cmd  *exec.Cmd
	log  *startupLog
	done chan struct{}
	err  error // Written before done is closed.
}

type startupLog struct {
	sync.Mutex
	buffer bytes.Buffer
	ready  chan string
	grpc   string
}

func (l *startupLog) Write(data []byte) (int, error) {
	l.Lock()
	defer l.Unlock()
	n, err := l.buffer.Write(data)
	if l.ready != nil {
		for rest := l.buffer.String(); ; {
			line, tail, complete := strings.Cut(rest, "\n")
			if !complete {
				break
			}
			rest = tail
			if endpoint, ok := strings.CutPrefix(line, grpcListening); ok {
				l.grpc = endpoint
			}
			if url, ok := strings.CutPrefix(line, listening); ok {
				l.ready <- url
				l.ready = nil
				break
			}
		}
	}
	return n, err
}

func (l *startupLog) text() string {
	l.Lock()
	defer l.Unlock()
	return l.buffer.String()
}

// openLogFile creates a file for cmd's stderr in EMULATOR_LOG_DIR, headed by the command line
// that identifies the process, or returns nil when the variable is unset.
func openLogFile(cmd *exec.Cmd) (*os.File, error) {
	dir := os.Getenv(logDirEnv)
	if dir == "" {
		return nil, nil
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	file, err := os.CreateTemp(dir, "emulator-*.log")
	if err != nil {
		return nil, err
	}
	if _, err := fmt.Fprintf(file, "# %q\n", cmd.Args); err != nil {
		_ = file.Close()
		return nil, err
	}
	return file, nil
}

// Start runs the serve command and waits for its startup announcement or ctx to expire. The
// caller supplies all server arguments, including --host and --http-port; dir may be empty.
// ctx bounds startup only. The caller owns the process after Start succeeds.
func Start(ctx context.Context, binary, dir string, args ...string) (*Process, error) {
	ready := make(chan string, 1)
	p := &Process{
		// Each isolated process also needs its own Storage listener. Explicit caller
		// arguments later in the list override this default.
		cmd:  exec.Command(binary, append([]string{"serve", "--grpc-port", "0"}, args...)...),
		log:  &startupLog{ready: ready},
		done: make(chan struct{}),
	}
	p.cmd.Dir = dir
	p.cmd.Stderr = p.log
	logFile, err := openLogFile(p.cmd)
	if err != nil {
		return nil, err
	}
	if logFile != nil {
		p.cmd.Stderr = io.MultiWriter(p.log, logFile)
	}
	if err := p.cmd.Start(); err != nil {
		if logFile != nil {
			_ = logFile.Close()
		}
		return nil, err
	}
	go func() {
		// Wait returns after copying all of stderr, so the file is complete when closed.
		p.err = p.cmd.Wait()
		if logFile != nil {
			_ = logFile.Close()
		}
		close(p.done)
	}()
	select {
	case p.URL = <-ready:
		p.log.Lock()
		p.GRPC = p.log.grpc
		p.log.Unlock()
		return p, nil
	case <-p.done:
		return nil, fmt.Errorf("%s exited before listening (%v):\n%s", binary, p.err, p.log.text())
	case <-ctx.Done():
		_ = p.cmd.Process.Kill()
		<-p.done
		return nil, fmt.Errorf("starting %s: %w\n%s", binary, ctx.Err(), p.log.text())
	}
}

// Stop sends SIGTERM so that persistence is checkpointed, waits for a clean exit,
// and kills a process that does not exit within 15 seconds. It may be called again.
func (p *Process) Stop() error {
	select {
	case <-p.done:
	default:
		_ = p.cmd.Process.Signal(syscall.SIGTERM)
		timer := time.NewTimer(15 * time.Second)
		defer timer.Stop()
		select {
		case <-p.done:
		case <-timer.C:
			_ = p.cmd.Process.Kill()
			<-p.done
			return fmt.Errorf("emulator did not stop within 15 seconds:\n%s", p.log.text())
		}
	}
	if p.err != nil {
		return fmt.Errorf("emulator exited: %w\n%s", p.err, p.log.text())
	}
	return nil
}
