package main

import (
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"
)

// A shard log as GoogleSQL writes it, cut down to the lines the summary reads.
const finishedLog = `I1006 sql_test_base.cc:906] CSV: "","code:LE","code:LE_INT64_<1>_INT64_<2>",true,true,ALLOW_UNIMPLEMENTED
I1006 sql_test_base.cc:906] CSV: "","code:Least","code:Least_ARRAY<INT64>_<1, NULL>",false,true,ALLOW_UNIMPLEMENTED
I1006 sql_test_base.cc:906] CSV: "","graph_test","graph_test:match",false,true,ALLOW_UNIMPLEMENTED
I1006 sql_test_base.cc:906] CSV: "","code:GE","code:GE_INT64_<1>_INT64_<2>",false,true,ALLOW_ERROR_OR_WRONG_ANSWER
I1006 sql_test_base.cc:906] CSV: "","code:GT","code:GT_INT64_<1>_INT64_<2>",true,true,ALLOW_ERROR_OR_WRONG_ANSWER
  Location: FILE-compliance_test_cases.cc-LINE-875
      Name: code:Least_ARRAY<INT64>_<1, NULL>
==== GOOGLESQL COMPLIANCE REPORT
[  PASSED  ] 0 statements.
[  FAILED  ] 1 statements.
==== End GOOGLESQL COMPLIANCE REPORT
==== Failures Summary #1
   Statement: SELECT Least(@p0) AS ColA
    Name: code:Least_ARRAY<INT64>_<1, NULL>
==== End Failures Summary #1
`

// A shard that timed out: its statements ran without a report.
const unfinishedLog = `I1006 sql_test_base.cc:906] CSV: "","code:safe_error_mode__safe.generate_array","code:safe_error_mode__safe_generate_array_INT64_<1>",true,true,ALLOW_UNIMPLEMENTED
`

func writeLog(t *testing.T, dir, shard, content string, modTime time.Time) {
	t.Helper()
	path := filepath.Join(dir, shard, "test.log")
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Chtimes(path, modTime, modTime); err != nil {
		t.Fatal(err)
	}
}

func TestSummarize(t *testing.T) {
	dir := t.TempDir()
	now := time.Now()
	// A log of an earlier run with another shard count is ignored.
	writeLog(t, dir, "shard_1_of_2", unfinishedLog, now.Add(-time.Hour))
	writeLog(t, dir, "shard_1_of_3", finishedLog, now)
	writeLog(t, dir, "shard_2_of_3", unfinishedLog, now)

	paths, err := latestShards(dir)
	if err != nil {
		t.Fatal(err)
	}
	s, err := summarize(paths)
	if err != nil {
		t.Fatal(err)
	}
	want := map[string]string{
		"code:LE_INT64_<1>_INT64_<2>":                         pass,
		"code:Least_ARRAY<INT64>_<1, NULL>":                   fail,
		"graph_test:match":                                    unsupported,
		"code:GE_INT64_<1>_INT64_<2>":                         known,
		"code:GT_INT64_<1>_INT64_<2>":                         pass,
		"code:safe_error_mode__safe_generate_array_INT64_<1>": pass,
	}
	if !reflect.DeepEqual(s.outcomes, want) {
		t.Errorf("outcomes = %v, want %v", s.outcomes, want)
	}
	if want := []string{"code:GT_INT64_<1>_INT64_<2>"}; !reflect.DeepEqual(s.removable, want) {
		t.Errorf("removable = %v, want %v", s.removable, want)
	}
	// Shard 2 has no report, and shard 3 left no log.
	if want := []int{2, 3}; !reflect.DeepEqual(s.unfinished, want) {
		t.Errorf("unfinished = %v, want %v", s.unfinished, want)
	}

	var markdown strings.Builder
	s.writeMarkdown(&markdown)
	for _, line := range []string{
		"**Shards that did not finish:** [2 3] of 3.",
		"| Pass | 3 |", "| Fail | 1 |", "| Known failure | 1 |", "| Unsupported | 1 |",
		"| Total | 6 |", "| `code:Least` | 1 |", "| `code:GE` | 1 |",
		"**Failures:** 1. Fix them", "- `code:Least_ARRAY<INT64>_<1, NULL>`",
		"**Known failures that pass:** 1. Remove them", "- `code:GT_INT64_<1>_INT64_<2>`",
	} {
		if !strings.Contains(markdown.String(), line) {
			t.Errorf("Markdown lacks %q:\n%s", line, markdown.String())
		}
	}

	var results strings.Builder
	if err := s.writeResults(&results); err != nil {
		t.Fatal(err)
	}
	if got, want := results.String(), "code:GE_INT64_<1>_INT64_<2>\tknown\n"+
		"code:GT_INT64_<1>_INT64_<2>\tpass\n"+
		"code:LE_INT64_<1>_INT64_<2>\tpass\n"+
		"code:Least_ARRAY<INT64>_<1, NULL>\tfail\n"+
		"code:safe_error_mode__safe_generate_array_INT64_<1>\tpass\n"+
		"graph_test:match\tunsupported\n"; got != want {
		t.Errorf("results = %q, want %q", got, want)
	}
}

func TestGroup(t *testing.T) {
	for name, want := range map[string]string{
		"code:LE_ARRAY<BIGNUMERIC>_<NULL>":                     "code:LE",
		"code:safe_error_mode__safe_generate_array_INT64_<1>":  "code:generate_array",
		"code:date_add_DATE_<2001-01-01>":                      "code:date_add",
		"additional_date_time_functions_test:months_between_x": "additional_date_time_functions_test",
	} {
		if got := group(name); got != want {
			t.Errorf("group(%q) = %q, want %q", name, got, want)
		}
	}
}
