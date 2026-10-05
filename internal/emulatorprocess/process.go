// Package emulatorprocess manages isolated emulator processes for tests and probes.
package emulatorprocess

import (
	"bytes"
	"context"
	"fmt"
	"os/exec"
	"strings"
	"sync"
	"syscall"
	"time"
)

const listening = "bigquery-emulator-duckdb listening on "

// Process is an emulator whose startup URL has been read from stderr.
// Stop must be called before removing its data directory.
type Process struct {
	URL  string
	cmd  *exec.Cmd
	log  *startupLog
	done chan struct{}
	err  error // Written before done is closed.
}

type startupLog struct {
	sync.Mutex
	buffer bytes.Buffer
	ready  chan string
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

// Start waits for the startup announcement or ctx to expire. The caller supplies
// all server arguments, including --host and --port; dir may be empty.
// ctx bounds startup only. The caller owns the process after Start succeeds.
func Start(ctx context.Context, binary, dir string, args ...string) (*Process, error) {
	ready := make(chan string, 1)
	p := &Process{
		cmd:  exec.Command(binary, args...),
		log:  &startupLog{ready: ready},
		done: make(chan struct{}),
	}
	p.cmd.Dir = dir
	p.cmd.Stderr = p.log
	if err := p.cmd.Start(); err != nil {
		return nil, err
	}
	go func() {
		p.err = p.cmd.Wait()
		close(p.done)
	}()
	select {
	case p.URL = <-ready:
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
