package goclient

import (
	"bufio"
	"encoding/json"
	"os"
	"strings"
	"testing"
)

type queryCase struct {
	path          string
	line          int
	sql           string
	want          any
	wantError     *string
	errorContains bool
	// A known bug: want is BigQuery's answer, which the emulator does not give yet.
	knownBug bool
}

// readQueryCases reads cases written as one query, possibly over several lines, followed by
// "=> " and the JSON of the cell's "v" as jobs.query returns it: a string, or null for NULL.
// A known bug is written with "!> " instead, followed by BigQuery's answer; the case passes while
// the emulator answers otherwise, and fails once it is fixed, to be turned into a "=> " case.
// Blank lines and lines starting with # between cases are ignored.
// "=> error:" expects a query error, optionally followed by BigQuery's exact message.
// "=> error contains:" matches a substring, useful when the client adds job context.
func readQueryCases(t *testing.T, path string) []queryCase {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer file.Close()

	var cases []queryCase
	var sql []string
	start := 0
	scanner := bufio.NewScanner(file)
	for line := 1; scanner.Scan(); line++ {
		text := scanner.Text()
		switch {
		case strings.HasPrefix(text, "=> "), strings.HasPrefix(text, "!> "):
			if len(sql) == 0 {
				t.Fatalf("%s:%d: expected value without a query", path, line)
			}
			var want any
			var wantError *string
			errorContains := strings.HasPrefix(text[len("=> "):], "error contains:")
			if errorContains {
				message := strings.TrimSpace(strings.TrimPrefix(text[len("=> "):], "error contains:"))
				if message == "" {
					t.Fatalf("%s:%d: empty error substring", path, line)
				}
				wantError = &message
			} else if strings.HasPrefix(text[len("=> "):], "error:") {
				message := strings.TrimSpace(strings.TrimPrefix(text[len("=> "):], "error:"))
				wantError = &message
			} else if err := json.Unmarshal([]byte(text[len("=> "):]), &want); err != nil {
				t.Fatalf("%s:%d: %v", path, line, err)
			}
			cases = append(cases, queryCase{path: path, line: start, sql: strings.Join(sql, "\n"),
				want: want, wantError: wantError, errorContains: errorContains, knownBug: strings.HasPrefix(text, "!> ")})
			sql = nil
		case len(sql) > 0:
			sql = append(sql, text)
		case text == "" || strings.HasPrefix(text, "#"):
		default:
			start = line
			sql = append(sql, text)
		}
	}
	if err := scanner.Err(); err != nil {
		t.Fatalf("Scan: %v", err)
	}
	if len(sql) > 0 {
		t.Fatalf("%s:%d: query without an expected value", path, start)
	}
	if len(cases) == 0 {
		t.Fatalf("%s: no cases", path)
	}
	return cases
}

// matchesError preserves exact REST messages while allowing client-added context.
func (c queryCase) matchesError(err error) bool {
	if err == nil || c.wantError == nil {
		return false
	}
	if c.errorContains {
		return strings.Contains(err.Error(), *c.wantError)
	}
	return *c.wantError == "" || err.Error() == *c.wantError
}
