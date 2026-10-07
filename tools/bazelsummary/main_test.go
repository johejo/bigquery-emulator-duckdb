package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestReport(t *testing.T) {
	// BEP emits newline-delimited events, string-encoded int64s, and base64 log contents.
	input := `{"progress":{"stdout":"ignored"}}
{"buildToolLogs":{"log":[{"name":"elapsed time","contents":"MTIuMzQwMDAw"},{"name":"critical path","contents":"Q3JpdGljYWwgUGF0aDogNC41NnMsIFJlbW90ZQ=="}]}}
{"buildMetrics":{"timingMetrics":{"wallTimeInMs":"12000","analysisPhaseTimeInMs":"1000","executionPhaseTimeInMs":"11000"},"actionSummary":{"runnerCount":[{"name":"total","count":100},{"name":"disk cache hit","count":80},{"name":"internal","count":10},{"name":"processwrapper-sandbox","count":10}]}}}`
	r, err := readReport(strings.NewReader(input))
	if err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	writeReport(&out, r)
	for _, expected := range []string{
		"| Elapsed | 12.340 |", "| Critical path | 4.560 |", "| Wall time | 12.000 |",
		"| Analysis | 1.000 |", "| Execution | 11.000 |", "| disk cache hit | 80 |",
		"| internal | 10 |", "| processwrapper-sandbox | 10 |", "| total | 100 |",
	} {
		if !strings.Contains(out.String(), expected) {
			t.Errorf("missing %q in:\n%s", expected, &out)
		}
	}
}

func TestReportMissingAndTruncated(t *testing.T) {
	for _, tc := range []struct {
		name, input, want string
	}{
		{"no metrics", `{}`, "Bazel did not emit build metrics."},
		{"no counts", `{"buildMetrics":{}}`, "Bazel did not emit process/cache counts."},
		{"zero defaults", `{"buildMetrics":{"actionSummary":{"runnerCount":[{"name":"internal","count":1}]}}}`, "| disk cache hit | 0 |"},
		{"truncated", `{"buildMetrics":{}}` + "\n" + `{"progress":`, "The JSON report is incomplete;"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			r, err := readReport(strings.NewReader(tc.input))
			if err != nil {
				t.Fatal(err)
			}
			var out bytes.Buffer
			writeReport(&out, r)
			if !strings.Contains(out.String(), tc.want) {
				t.Fatalf("missing %q in:\n%s", tc.want, &out)
			}
		})
	}
}

func TestReportMalformed(t *testing.T) {
	if _, err := readReport(strings.NewReader(`{"buildMetrics":invalid}`)); err == nil {
		t.Fatal("expected a malformed JSON error")
	}
}

func TestSummarizeDirectory(t *testing.T) {
	dir := t.TempDir()
	for name, input := range map[string]string{
		"build.json": `{"finished":{"exitCode":{"name":"SUCCESS"}}}` + "\n" + `{"buildMetrics":{}}`,
		"test.json":  `{"finished":{"exitCode":{"name":"TESTS_FAILED","code":3}}}` + "\n" + `{"buildMetrics":{}}`,
		"other.txt":  "ignored",
	} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(input), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	var out bytes.Buffer
	if err := summarize(&out, dir); err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"### Bazel: build", "### Bazel: test", "Bazel exit status: SUCCESS (0)", "Bazel exit status: TESTS_FAILED (3)"} {
		if !strings.Contains(out.String(), want) {
			t.Errorf("missing %q in:\n%s", want, &out)
		}
	}
	if strings.Contains(out.String(), "other") {
		t.Fatal("included a non-JSON file")
	}
}

func TestSummarizeEmptyDirectory(t *testing.T) {
	var out bytes.Buffer
	if err := summarize(&out, t.TempDir()); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out.String(), "No Bazel JSON reports were emitted.") {
		t.Fatalf("unexpected output: %s", &out)
	}
}
