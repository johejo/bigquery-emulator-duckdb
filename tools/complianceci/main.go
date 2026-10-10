// Command complianceci packages Bazel's compliance test for CI and runs one matrix job's shards.
// Usage: complianceci package OUTPUT_DIR | complianceci run BUNDLE_DIR LOG_DIR GROUP GROUPS
package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

type testConfig struct {
	Args   []string `json:"args"`
	Shards int      `json:"shards"`
}

// Read resolved arguments rather than duplicating BUILD.bazel's flags. Reject a different
// launcher or per-shard flags instead of silently discarding them.
func configFromActions(data []byte) (testConfig, error) {
	var query struct {
		Actions []struct {
			Arguments []string `json:"arguments"`
		} `json:"actions"`
	}
	if err := json.Unmarshal(data, &query); err != nil {
		return testConfig{}, err
	}
	if len(query.Actions) == 0 {
		return testConfig{}, errors.New("no TestRunner actions")
	}
	args := query.Actions[0].Arguments
	if len(args) < 2 || !slices.Equal(args[:2], []string{"external/bazel_tools/tools/test/test-setup.sh", "./compliance_test"}) {
		return testConfig{}, fmt.Errorf("unexpected Bazel launcher: %v", args)
	}
	for _, action := range query.Actions {
		if !slices.Equal(action.Arguments, args) {
			return testConfig{}, errors.New("TestRunner arguments differ between shards")
		}
	}
	return testConfig{Args: args[2:], Shards: len(query.Actions)}, nil
}

func command(ctx context.Context, name string, args ...string) *exec.Cmd {
	cmd := exec.CommandContext(ctx, name, args...)
	cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	// Stop the entire process group on interruption, including timeout's test process.
	cmd.Cancel = func() error { return syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL) }
	cmd.WaitDelay = 30 * time.Second
	return cmd
}

func packageTests(ctx context.Context, output string) error {
	output, err := filepath.Abs(output)
	if err != nil {
		return err
	}
	summary := filepath.Join(output, "summary")
	if err := os.MkdirAll(summary, 0o755); err != nil {
		return err
	}
	cmd := command(ctx, "bazelisk", "aquery", `mnemonic("TestRunner", //:compliance_test)`, "--output=jsonproto", "--include_artifacts=false")
	cmd.Stdout = nil
	data, err := cmd.Output()
	if err != nil {
		return err
	}
	config, err := configFromActions(data)
	if err != nil {
		return err
	}
	data, err = json.Marshal(config)
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(output, "config.json"), data, 0o644); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(summary, "shard-count"), []byte(strconv.Itoa(config.Shards)+"\n"), 0o644); err != nil {
		return err
	}
	// Bundle this already-built Go executable; shard jobs do not compile or invoke Go.
	executable, err := os.Executable()
	if err != nil {
		return err
	}
	data, err = os.ReadFile(executable)
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(output, "complianceci"), data, 0o755); err != nil {
		return err
	}
	// Dereference Bazel's symlinks and omit the absolute-path manifest. Keep tar's established
	// handling of runfiles instead of implementing an archiver here.
	if err := command(ctx, "tar", "--dereference", "--exclude=compliance_test.runfiles/MANIFEST", "-czf", filepath.Join(output, "compliance.tar.gz"), "-C", "bazel-bin", "compliance_test", "compliance_test.runfiles", "-C", output, "config.json", "complianceci").Run(); err != nil {
		return err
	}
	return command(ctx, "go", "build", "-o", filepath.Join(summary, "compliance-summary"), "./tools/compliancesummary").Run()
}

func runShard(ctx context.Context, bundle, logs string, config testConfig, index int) error {
	dir := filepath.Join(logs, fmt.Sprintf("shard_%d_of_%d", index+1, config.Shards))
	if err := os.MkdirAll(filepath.Join(dir, "tmp"), 0o755); err != nil {
		return err
	}
	log, err := os.Create(filepath.Join(dir, "test.log"))
	if err != nil {
		return err
	}
	defer log.Close()
	runfiles := filepath.Join(bundle, "compliance_test.runfiles")
	status := filepath.Join(dir, "shard-status")
	// Retain GNU timeout's one-hour eternal timeout and 30-second termination grace period.
	args := append([]string{"--kill-after=30s", "3600", filepath.Join(bundle, "compliance_test")}, config.Args...)
	cmd := command(ctx, "timeout", args...)
	cmd.Dir = filepath.Join(runfiles, "_main")
	cmd.Env = slices.DeleteFunc(os.Environ(), func(s string) bool {
		return strings.HasPrefix(s, "RUNFILES_MANIFEST_FILE=") || strings.HasPrefix(s, "RUNFILES_MANIFEST_ONLY=")
	})
	cmd.Env = append(cmd.Env,
		"TEST_SRCDIR="+runfiles, "RUNFILES_DIR="+runfiles, "TEST_WORKSPACE=_main", "TEST_TARGET=//:compliance_test", "BAZEL_TEST=1",
		"TEST_TMPDIR="+filepath.Join(dir, "tmp"),
		"GTEST_TOTAL_SHARDS="+strconv.Itoa(config.Shards), "TEST_TOTAL_SHARDS="+strconv.Itoa(config.Shards),
		"GTEST_SHARD_INDEX="+strconv.Itoa(index), "TEST_SHARD_INDEX="+strconv.Itoa(index),
		"GTEST_SHARD_STATUS_FILE="+status, "TEST_SHARD_STATUS_FILE="+status)
	cmd.Stdout, cmd.Stderr = log, log
	runErr := cmd.Run()
	_, statusErr := os.Stat(status)
	if statusErr != nil {
		statusErr = fmt.Errorf("shard did not acknowledge sharding: %w", statusErr)
	}
	return errors.Join(runErr, statusErr)
}

// Each worker runs every groups-th shard. Failures do not stop other shards; cancellation does.
func runShards(ctx context.Context, bundle, logs string, config testConfig, group, groups, parallelism int) error {
	if config.Shards <= 0 || groups <= 0 || group < 0 || group >= groups || parallelism <= 0 {
		return errors.New("invalid shard count, group, or parallelism")
	}
	var mu sync.Mutex
	var failures []error
	var wg sync.WaitGroup
	slots := make(chan struct{}, parallelism)
loop:
	for index := group; index < config.Shards; index += groups {
		select {
		case slots <- struct{}{}:
		case <-ctx.Done():
			break loop
		}
		if ctx.Err() != nil {
			<-slots
			break
		}
		wg.Go(func() {
			defer func() { <-slots }()
			fmt.Printf("Starting shard %d of %d\n", index+1, config.Shards)
			if err := runShard(ctx, bundle, logs, config, index); err != nil {
				mu.Lock()
				failures = append(failures, fmt.Errorf("shard %d: %w", index+1, err))
				mu.Unlock()
			}
		})
	}
	wg.Wait()
	return errors.Join(append(failures, ctx.Err())...)
}

func run(ctx context.Context, args []string) error {
	if len(args) == 2 && args[0] == "package" {
		return packageTests(ctx, args[1])
	}
	if len(args) != 5 || args[0] != "run" {
		return errors.New("usage: complianceci package OUTPUT_DIR | complianceci run BUNDLE_DIR LOG_DIR GROUP GROUPS")
	}
	bundle, err := filepath.Abs(args[1])
	if err != nil {
		return err
	}
	logs, err := filepath.Abs(args[2])
	if err != nil {
		return err
	}
	group, err := strconv.Atoi(args[3])
	if err != nil {
		return err
	}
	groups, err := strconv.Atoi(args[4])
	if err != nil {
		return err
	}
	data, err := os.ReadFile(filepath.Join(bundle, "config.json"))
	if err != nil {
		return err
	}
	var config testConfig
	if err := json.Unmarshal(data, &config); err != nil {
		return err
	}
	return runShards(ctx, bundle, logs, config, group, groups, runtime.GOMAXPROCS(0))
}

func main() {
	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()
	if err := run(ctx, os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
