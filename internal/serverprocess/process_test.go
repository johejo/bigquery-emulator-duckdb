package serverprocess

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"slices"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestServerProcessHelper(t *testing.T) {
	separator := slices.Index(os.Args, "--")
	if separator < 0 {
		return
	}
	mode := os.Args[separator+1]
	stopped := make(chan os.Signal, 1)
	if mode == "ignore" {
		signal.Ignore(syscall.SIGTERM)
	} else {
		signal.Notify(stopped, syscall.SIGTERM, syscall.SIGINT)
	}
	fmt.Println("ready")
	if mode == "exit" {
		os.Exit(7)
	}
	if mode == "ignore" {
		time.Sleep(time.Minute)
		os.Exit(0)
	}
	<-stopped
	os.Exit(0)
}

func startTestProcess(t *testing.T, mode string, group bool) *process {
	t.Helper()
	cmd := exec.Command(os.Args[0], "-test.run=^TestServerProcessHelper$", "--", mode)
	if group {
		cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	}
	reader, writer, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = reader.Close() })
	cmd.Stdout = writer
	p, err := startProcess(cmd, nil)
	_ = writer.Close()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(p.kill)
	ready := make(chan string, 1)
	go func() {
		line, _ := bufio.NewReader(reader).ReadString('\n')
		ready <- line
	}()
	select {
	case line := <-ready:
		if line != "ready\n" {
			t.Fatalf("startup output = %q", line)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("helper did not start")
	}
	return p
}

func TestServerStop(t *testing.T) {
	for _, tc := range []struct {
		name   string
		signal syscall.Signal
		group  bool
	}{
		{"terminate", syscall.SIGTERM, false},
		{"interrupt", syscall.SIGINT, false},
		{"group", syscall.SIGTERM, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			p := startTestProcess(t, "wait", tc.group)
			for range 2 {
				if err := p.stop(tc.signal, tc.group, 10*time.Second); err != nil {
					t.Fatal(err)
				}
			}
		})
	}
}

func TestServerStopTimeout(t *testing.T) {
	t.Parallel()
	p := startTestProcess(t, "ignore", false)
	if err := p.stop(syscall.SIGTERM, false, 100*time.Millisecond); err == nil || !strings.Contains(err.Error(), "did not stop") {
		t.Fatalf("stop = %v", err)
	}
	if err := p.stop(syscall.SIGTERM, false, time.Second); err != nil {
		t.Fatal(err)
	}
}

func TestServerPoll(t *testing.T) {
	t.Run("ready", func(t *testing.T) {
		t.Parallel()
		p := startTestProcess(t, "wait", false)
		ctx, cancel := context.WithTimeout(t.Context(), 10*time.Second)
		defer cancel()
		attempts := 0
		if err := p.poll(ctx, func(context.Context) bool {
			attempts++
			return attempts == 2
		}); err != nil {
			t.Fatal(err)
		}
	})
	t.Run("canceled", func(t *testing.T) {
		t.Parallel()
		p := startTestProcess(t, "wait", false)
		ctx, cancel := context.WithCancel(t.Context())
		cancel()
		if err := p.poll(ctx, func(context.Context) bool { return false }); !errors.Is(err, context.Canceled) {
			t.Fatalf("poll = %v", err)
		}
	})
	t.Run("exited", func(t *testing.T) {
		t.Parallel()
		p := startTestProcess(t, "exit", false)
		ctx, cancel := context.WithTimeout(t.Context(), 10*time.Second)
		defer cancel()
		if err := p.poll(ctx, func(context.Context) bool { return false }); err == nil || !strings.Contains(err.Error(), "exit status 7") {
			t.Fatalf("poll = %v", err)
		}
	})
}
