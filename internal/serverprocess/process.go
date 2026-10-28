// Package serverprocess manages isolated servers for tests and probes.
package serverprocess

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"syscall"
	"time"
)

// process owns the sole Wait call and publishes its result by closing done.
// A startup context does not control the lifetime of a successfully started server.
type process struct {
	cmd  *exec.Cmd
	done chan struct{}
	err  error // Written before done is closed.
}

// afterWait releases output resources after exec has finished copying output.
// It is also called if the command cannot start.
func startProcess(cmd *exec.Cmd, afterWait func()) (*process, error) {
	if err := cmd.Start(); err != nil {
		if afterWait != nil {
			afterWait()
		}
		return nil, err
	}
	p := &process{cmd: cmd, done: make(chan struct{})}
	go func() {
		p.err = cmd.Wait()
		if afterWait != nil {
			afterWait()
		}
		close(p.done)
	}()
	return p, nil
}

func (p *process) kill() {
	_ = p.cmd.Process.Kill()
	<-p.done
}

// stop preserves each server's shutdown signal and whether it owns a process group.
// A zero timeout waits indefinitely. Exit status is interpreted by the caller.
func (p *process) stop(signal syscall.Signal, group bool, timeout time.Duration) error {
	select {
	case <-p.done:
		return nil
	default:
	}
	var err error
	if group {
		err = syscall.Kill(-p.cmd.Process.Pid, signal)
	} else {
		err = p.cmd.Process.Signal(signal)
	}
	if err != nil && !errors.Is(err, os.ErrProcessDone) {
		return err
	}
	if timeout == 0 {
		<-p.done
		return nil
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case <-p.done:
		return nil
	case <-timer.C:
		if group {
			_ = syscall.Kill(-p.cmd.Process.Pid, syscall.SIGKILL)
			<-p.done
		} else {
			p.kill()
		}
		return fmt.Errorf("process did not stop within %s", timeout)
	}
}

// poll waits for a service-specific readiness check, startup cancellation or exit.
// A failed check is retried; the service supplies diagnostics if startup fails.
func (p *process) poll(ctx context.Context, check func(context.Context) bool) error {
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		if check(ctx) {
			return nil
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-p.done:
			return fmt.Errorf("process exited before accepting connections: %v", p.err)
		case <-ticker.C:
		}
	}
}
