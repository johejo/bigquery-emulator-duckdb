package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"testing"
	"time"
)

func TestConfigFromActions(t *testing.T) {
	for _, tc := range []struct {
		name, input string
		want        testConfig
		fail        bool
	}{
		{name: "resolved", input: `{"actions":[{"arguments":["external/bazel_tools/tools/test/test-setup.sh","./compliance_test","--pattern=with spaces"]},{"arguments":["external/bazel_tools/tools/test/test-setup.sh","./compliance_test","--pattern=with spaces"]}]}`, want: testConfig{Args: []string{"--pattern=with spaces"}, Shards: 2}},
		{name: "empty", input: `{"actions":[]}`, fail: true},
		{name: "invalid JSON", input: `{`, fail: true},
		{name: "new launcher", input: `{"actions":[{"arguments":["wrapper","./compliance_test"]}]}`, fail: true},
		{name: "different arguments", input: `{"actions":[{"arguments":["external/bazel_tools/tools/test/test-setup.sh","./compliance_test","first"]},{"arguments":["external/bazel_tools/tools/test/test-setup.sh","./compliance_test","second"]}]}`, fail: true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, err := configFromActions([]byte(tc.input))
			if (err != nil) != tc.fail {
				t.Fatalf("configFromActions() = %v, %v", got, err)
			}
			if !tc.fail && !reflect.DeepEqual(got, tc.want) {
				t.Fatalf("got %v, want %v", got, tc.want)
			}
		})
	}
}

func writeFile(t *testing.T, path, content string, mode os.FileMode) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), mode); err != nil {
		t.Fatal(err)
	}
}

func TestPackageRelocation(t *testing.T) {
	if version, err := exec.Command("tar", "--version").Output(); err != nil || !strings.Contains(string(version), "GNU tar") {
		t.Skip("CI packaging requires GNU tar")
	}
	root := t.TempDir()
	workspace := filepath.Join(root, "workspace")
	bin := filepath.Join(root, "bin")
	writeFile(t, filepath.Join(bin, "bazelisk"), `#!/usr/bin/env bash
printf '%s' '{"actions":[{"arguments":["external/bazel_tools/tools/test/test-setup.sh","./compliance_test","--pattern=with spaces"]}]}'
`, 0o755)
	writeFile(t, filepath.Join(bin, "go"), "#!/usr/bin/env bash\ntouch \"$3\"\n", 0o755)
	t.Setenv("PATH", bin+string(os.PathListSeparator)+os.Getenv("PATH"))
	writeFile(t, filepath.Join(workspace, "bazel-bin/compliance_test"), "test executable", 0o755)
	source := filepath.Join(workspace, "source")
	writeFile(t, source, "runfile contents", 0o644)
	runfiles := filepath.Join(workspace, "bazel-bin/compliance_test.runfiles")
	writeFile(t, filepath.Join(runfiles, "MANIFEST"), "stale absolute paths", 0o644)
	if err := os.MkdirAll(filepath.Join(runfiles, "_main"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(source, filepath.Join(runfiles, "_main/data")); err != nil {
		t.Fatal(err)
	}
	t.Chdir(workspace)
	output := filepath.Join(root, "output")
	if err := packageTests(t.Context(), output); err != nil {
		t.Fatal(err)
	}
	relocated := filepath.Join(root, "relocated")
	if err := os.Mkdir(relocated, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := exec.Command("tar", "-xzf", filepath.Join(output, "compliance.tar.gz"), "-C", relocated).Run(); err != nil {
		t.Fatal(err)
	}
	t.Chdir(root)
	if err := os.RemoveAll(workspace); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(relocated, "compliance_test.runfiles/_main/data"))
	if err != nil || string(data) != "runfile contents" {
		t.Fatalf("relocated runfile = %q, %v", data, err)
	}
	if _, err := os.Stat(filepath.Join(relocated, "compliance_test.runfiles/MANIFEST")); !os.IsNotExist(err) {
		t.Fatalf("manifest was not excluded: %v", err)
	}
	data, err = os.ReadFile(filepath.Join(relocated, "config.json"))
	if err != nil {
		t.Fatal(err)
	}
	var config testConfig
	if err := json.Unmarshal(data, &config); err != nil {
		t.Fatal(err)
	}
	if config.Shards != 1 || !reflect.DeepEqual(config.Args, []string{"--pattern=with spaces"}) {
		t.Fatalf("unexpected config: %v", config)
	}
	info, err := os.Stat(filepath.Join(relocated, "complianceci"))
	if err != nil || info.Mode()&0o111 == 0 {
		t.Fatalf("runner is not executable: %v", err)
	}
	data, err = os.ReadFile(filepath.Join(output, "summary/shard-count"))
	if err != nil || string(data) != "1\n" {
		t.Fatalf("shard count = %q, %v", data, err)
	}
}

// A separate instance of this test binary stands in for the C++ executable. It validates the
// environment and cwd supplied to each shard, and emits logs even on a failing run.
func TestShardHelper(t *testing.T) {
	if os.Getenv("COMPLIANCE_HELPER") != "1" {
		return
	}
	index := os.Getenv("GTEST_SHARD_INDEX")
	runfiles := os.Getenv("TEST_SRCDIR")
	cwd, err := os.Getwd()
	if err != nil || cwd != filepath.Join(runfiles, "_main") || os.Getenv("RUNFILES_DIR") != runfiles || os.Getenv("TEST_SHARD_INDEX") != index || os.Getenv("GTEST_TOTAL_SHARDS") != "6" || os.Getenv("TEST_TOTAL_SHARDS") != "6" {
		os.Exit(2)
	}
	if os.Getenv("RUNFILES_MANIFEST_FILE") != "" || os.Getenv("RUNFILES_MANIFEST_ONLY") != "" {
		os.Exit(2)
	}
	if os.Args[len(os.Args)-1] != "--pattern=with spaces" {
		os.Exit(2)
	}
	if _, err := os.Stat(os.Getenv("TEST_TMPDIR")); err != nil {
		os.Exit(2)
	}
	if os.Getenv("MISSING_STATUS") != index {
		if err := os.WriteFile(os.Getenv("GTEST_SHARD_STATUS_FILE"), nil, 0o644); err != nil {
			os.Exit(2)
		}
	}
	fmt.Printf("shard %s\n", index)
	if os.Getenv("HANG_SHARD") == index {
		time.Sleep(time.Minute)
	}
	if os.Getenv("FAIL_SHARD") == index {
		os.Exit(1)
	}
	os.Exit(0)
}

func helperBundle(t *testing.T) (string, testConfig) {
	t.Helper()
	if _, err := exec.LookPath("timeout"); err != nil {
		t.Skip("GNU timeout unavailable")
	}
	t.Setenv("COMPLIANCE_HELPER", "1")
	t.Setenv("RUNFILES_MANIFEST_FILE", "/stale/build/path")
	t.Setenv("RUNFILES_MANIFEST_ONLY", "1")
	bundle := t.TempDir()
	if err := os.MkdirAll(filepath.Join(bundle, "compliance_test.runfiles/_main"), 0o755); err != nil {
		t.Fatal(err)
	}
	executable, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(executable, filepath.Join(bundle, "compliance_test")); err != nil {
		t.Fatal(err)
	}
	return bundle, testConfig{Shards: 6, Args: []string{"-test.run=^TestShardHelper$", "--", "--pattern=with spaces"}}
}

func TestRunShards(t *testing.T) {
	for _, mode := range []string{"success", "failure", "no acknowledgement"} {
		t.Run(mode, func(t *testing.T) {
			bundle, config := helperBundle(t)
			if mode == "failure" {
				t.Setenv("FAIL_SHARD", "2")
			}
			if mode == "no acknowledgement" {
				t.Setenv("MISSING_STATUS", "2")
			}
			logs := t.TempDir()
			err := runShards(t.Context(), bundle, logs, config, 0, 2, 2)
			if (err == nil) != (mode == "success") {
				t.Fatalf("unexpected result: %v", err)
			}
			if mode == "no acknowledgement" && !strings.Contains(err.Error(), "did not acknowledge") {
				t.Fatal(err)
			}
			dirs, err := os.ReadDir(logs)
			if err != nil || len(dirs) != 3 {
				t.Fatalf("expected all three assigned shards, got %v, %v", dirs, err)
			}
			for _, index := range []int{0, 2, 4} {
				data, err := os.ReadFile(filepath.Join(logs, fmt.Sprintf("shard_%d_of_6/test.log", index+1)))
				if err != nil || string(data) != "shard "+strconv.Itoa(index)+"\n" {
					t.Fatalf("shard %d log = %q, %v", index, data, err)
				}
			}
		})
	}
}

func TestCancellation(t *testing.T) {
	bundle, config := helperBundle(t)
	t.Setenv("HANG_SHARD", "0")
	ctx, cancel := context.WithTimeout(t.Context(), time.Second)
	defer cancel()
	start := time.Now()
	if err := runShards(ctx, bundle, t.TempDir(), config, 0, 2, 1); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("expected deadline cancellation, got %v", err)
	}
	if time.Since(start) > 5*time.Second {
		t.Fatal("cancellation did not stop the test process")
	}
}
