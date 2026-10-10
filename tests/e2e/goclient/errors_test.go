package goclient

import (
	"fmt"
	"path/filepath"
	"testing"
)

// SQL-only rejections use the Go client's Run/Wait path; request options and API lifecycle
// checks stay in their own Go tests. Each query is independent and has no default dataset.
func TestQueryErrors(t *testing.T) {
	paths, err := filepath.Glob("testdata/errors/*.txt")
	if err != nil || len(paths) == 0 {
		t.Fatalf("no cases in testdata/errors: %v", err)
	}
	for _, path := range paths {
		for _, c := range readQueryCases(t, path) {
			if c.wantError == nil || c.knownBug {
				t.Fatalf("%s:%d: expected => error: or => error contains:", c.path, c.line)
			}
			t.Run(fmt.Sprintf("%s:%d", filepath.Base(c.path), c.line), func(t *testing.T) {
				client := newClient(t)
				_, _, err := runScript(t, client, "", c.sql)
				if !c.matchesError(err) {
					t.Errorf("%s: got %v, want error %q (contains: %v)", c.sql, err, *c.wantError, c.errorContains)
				}
			})
		}
	}
}
