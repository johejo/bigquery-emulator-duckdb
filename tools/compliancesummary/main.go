// Command compliancesummary summarizes the test logs of `just compliance` as Markdown: how many
// statements pass, fail, or are rejected as unsupported, and which tests fail most. With -results,
// it also writes each statement's outcome, one "NAME<TAB>OUTCOME" line per statement. It fails when
// a shard did not finish, such as one that timed out, since its statements are then missing.
//
//	compliancesummary [-results FILE] LOGDIR
//
// LOGDIR is the test's log directory, bazel-testlogs/compliance_test.
package main

import (
	"bufio"
	"cmp"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"time"
)

const (
	pass        = "pass"
	fail        = "fail"
	unsupported = "unsupported"
)

var (
	shardDir = regexp.MustCompile(`^shard_(\d+)_of_(\d+)$`)
	// GoogleSQL logs every statement it runs as CSV: target, prefix, name, passed, known error,
	// known error mode. A statement that a known error allows, such as one the emulator rejects
	// as unsupported, does not pass.
	csvLine = regexp.MustCompile(`CSV: "","[^"]*","(.*)",(true|false),(?:true|false),[A-Z_]+$`)
	// A failure that no known error allows, listed in the failures summary at the end of a shard.
	failureName = regexp.MustCompile(`^\s*Name: (.*)$`)
	// A code-based statement's name: its function, possibly under SAFE, then its argument types.
	codeName = regexp.MustCompile(`^code:(?:safe_error_mode__safe_)?(.+?)(?:_[A-Z<]|$)`)
)

const (
	failuresStart = "==== Failures Summary"
	failuresEnd   = "==== End Failures Summary"
	reportEnd     = "==== End GOOGLESQL COMPLIANCE REPORT"
)

// shard holds what one shard's log says.
type shard struct {
	passed   map[string]bool // Every statement that ran, and whether it passed.
	failed   []string
	finished bool
}

func readShard(r io.Reader) (shard, error) {
	s := shard{passed: map[string]bool{}}
	scanner := bufio.NewScanner(r)
	scanner.Buffer(nil, 64<<20)
	inFailure := false
	for scanner.Scan() {
		line := scanner.Text()
		switch {
		case strings.HasPrefix(line, failuresEnd):
			inFailure = false
		case strings.HasPrefix(line, failuresStart):
			inFailure = true
		case strings.HasPrefix(line, reportEnd):
			s.finished = true
		case inFailure:
			if m := failureName.FindStringSubmatch(line); m != nil {
				s.failed = append(s.failed, m[1])
			}
		default:
			if m := csvLine.FindStringSubmatch(line); m != nil {
				s.passed[m[1]] = s.passed[m[1]] || m[2] == "true"
			}
		}
	}
	return s, scanner.Err()
}

// latestShards returns the log paths of the shards of the latest run under dir, by shard number,
// with an empty path for a shard that left no log. Logs of earlier runs with another shard count
// stay in the directory, so the run is the one whose log was written last.
func latestShards(dir string) ([]string, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	count := 0
	var latest time.Time
	for _, entry := range entries {
		m := shardDir.FindStringSubmatch(entry.Name())
		if m == nil {
			continue
		}
		info, err := os.Stat(filepath.Join(dir, entry.Name(), "test.log"))
		if err != nil {
			continue
		}
		if n, _ := strconv.Atoi(m[2]); count == 0 || info.ModTime().After(latest) {
			count, latest = n, info.ModTime()
		}
	}
	if count == 0 {
		return nil, fmt.Errorf("no shard logs in %s", dir)
	}
	paths := make([]string, count)
	for i := range paths {
		path := filepath.Join(dir, fmt.Sprintf("shard_%d_of_%d", i+1, count), "test.log")
		if _, err := os.Stat(path); err == nil {
			paths[i] = path
		}
	}
	return paths, nil
}

// group names the test a statement belongs to: the file of a file-based test, or the function of
// a code-based one, with or without SAFE.
func group(name string) string {
	if m := codeName.FindStringSubmatch(name); m != nil {
		return "code:" + m[1]
	}
	file, _, _ := strings.Cut(name, ":")
	return file
}

type summary struct {
	outcomes   map[string]string
	unfinished []int // Shard numbers, from 1.
	shards     int
}

func summarize(paths []string) (summary, error) {
	s := summary{outcomes: map[string]string{}, shards: len(paths)}
	for i, path := range paths {
		if path == "" {
			s.unfinished = append(s.unfinished, i+1)
			continue
		}
		file, err := os.Open(path)
		if err != nil {
			return s, err
		}
		result, err := readShard(file)
		file.Close()
		if err != nil {
			return s, fmt.Errorf("%s: %w", path, err)
		}
		if !result.finished {
			s.unfinished = append(s.unfinished, i+1)
		}
		for name, passed := range result.passed {
			if passed {
				s.outcomes[name] = pass
			} else if _, ok := s.outcomes[name]; !ok {
				s.outcomes[name] = unsupported
			}
		}
		for _, name := range result.failed {
			s.outcomes[name] = fail
		}
	}
	return s, nil
}

func (s summary) writeMarkdown(w io.Writer) {
	counts := map[string]int{}
	groups := map[string]int{}
	for name, outcome := range s.outcomes {
		counts[outcome]++
		if outcome == fail {
			groups[group(name)]++
		}
	}
	fmt.Fprintln(w, "## GoogleSQL compliance")
	fmt.Fprintln(w)
	if len(s.unfinished) > 0 {
		fmt.Fprintf(w, "**Shards that did not finish:** %v of %d. Their statements are missing below.\n\n",
			s.unfinished, s.shards)
	}
	fmt.Fprintln(w, "| Statements | Count |")
	fmt.Fprintln(w, "|---|---:|")
	fmt.Fprintf(w, "| Pass | %d |\n", counts[pass])
	fmt.Fprintf(w, "| Fail | %d |\n", counts[fail])
	fmt.Fprintf(w, "| Unsupported | %d |\n", counts[unsupported])
	fmt.Fprintf(w, "| Total | %d |\n", len(s.outcomes))
	if len(groups) == 0 {
		return
	}
	names := make([]string, 0, len(groups))
	for name := range groups {
		names = append(names, name)
	}
	slices.SortFunc(names, func(a, b string) int {
		return cmp.Or(cmp.Compare(groups[b], groups[a]), cmp.Compare(a, b))
	})
	const top = 20
	fmt.Fprintln(w)
	fmt.Fprintf(w, "Tests with the most failures, top %d of %d:\n\n", min(top, len(names)), len(names))
	fmt.Fprintln(w, "| Test | Failures |")
	fmt.Fprintln(w, "|---|---:|")
	for _, name := range names[:min(top, len(names))] {
		fmt.Fprintf(w, "| `%s` | %d |\n", name, groups[name])
	}
}

func (s summary) writeResults(w io.Writer) error {
	names := make([]string, 0, len(s.outcomes))
	for name := range s.outcomes {
		names = append(names, name)
	}
	slices.Sort(names)
	buffered := bufio.NewWriter(w)
	for _, name := range names {
		fmt.Fprintf(buffered, "%s\t%s\n", name, s.outcomes[name])
	}
	return buffered.Flush()
}

func main() {
	results := flag.String("results", "", "write each statement's outcome to this file")
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: compliancesummary [-results FILE] LOGDIR")
		os.Exit(2)
	}
	paths, err := latestShards(flag.Arg(0))
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	s, err := summarize(paths)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	s.writeMarkdown(os.Stdout)
	if *results != "" {
		file, err := os.Create(*results)
		if err == nil {
			err = s.writeResults(file)
			if closeErr := file.Close(); err == nil {
				err = closeErr
			}
		}
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
	if len(s.unfinished) > 0 {
		os.Exit(1)
	}
}
