package serverprocess

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

// Emulator is an emulator whose startup URL has been read from stderr.
// Stop must be called before removing its data directory.
type Emulator struct {
	URL     string
	GRPC    string
	process *process
	log     *startupLog
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

// StartEmulator runs the serve command and waits for its startup announcement or ctx to expire. The
// caller supplies all server arguments, including --host and --http-port; dir may be empty.
// ctx bounds startup only. The caller owns the process after StartEmulator succeeds.
// Set EMULATOR_LOG_DIR to also save each process's stderr to its own file.
func StartEmulator(ctx context.Context, binary, dir string, args ...string) (*Emulator, error) {
	ready := make(chan string, 1)
	p := &Emulator{log: &startupLog{ready: ready}}
	// Each isolated process also needs its own Storage listener. Explicit caller
	// arguments later in the list override this default.
	cmd := exec.Command(binary, append([]string{"serve", "--grpc-port", "0"}, args...)...)
	cmd.Dir = dir
	cmd.Stderr = p.log
	logFile, err := openLogFile(cmd)
	if err != nil {
		return nil, err
	}
	if logFile != nil {
		cmd.Stderr = io.MultiWriter(p.log, logFile)
	}
	p.process, err = startProcess(cmd, func() {
		if logFile != nil {
			_ = logFile.Close()
		}
	})
	if err != nil {
		return nil, err
	}
	select {
	case p.URL = <-ready:
		p.log.Lock()
		p.GRPC = p.log.grpc
		p.log.Unlock()
		return p, nil
	case <-p.process.done:
		return nil, fmt.Errorf("%s exited before listening (%v):\n%s", binary, p.process.err, p.log.text())
	case <-ctx.Done():
		p.process.kill()
		return nil, fmt.Errorf("starting %s: %w\n%s", binary, ctx.Err(), p.log.text())
	}
}

// Stop sends SIGTERM so that persistence is checkpointed, waits for a clean exit,
// and kills a process that does not exit within 15 seconds. It may be called again.
func (p *Emulator) Stop() error {
	if err := p.process.stop(syscall.SIGTERM, false, 15*time.Second); err != nil {
		return fmt.Errorf("stopping emulator: %w\n%s", err, p.log.text())
	}
	if p.process.err != nil {
		return fmt.Errorf("emulator exited: %w\n%s", p.process.err, p.log.text())
	}
	return nil
}
