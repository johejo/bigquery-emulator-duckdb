package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestValidateSQLWithoutCredentials(t *testing.T) {
	t.Setenv("PATH", t.TempDir())
	for _, content := range []string{
		"", "# comments only\n",
		"SELECT 1\n?>\n",
		"SELECT\n  1\n\n# comment\n?> \"1\"\nSELECT 2\n?> error: invalid query\n",
		"SELECT @p\nparam> {\"name\":\"p\"}\n?>\n",
		"SELECT NULL\n?> null\nSELECT [1]\n?> [{\"v\":\"1\"}]\n",
		"CREATE TEMP TABLE t AS SELECT 1;\nSELECT COUNT(*) FROM t\n?>\n",
	} {
		t.Run(content, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "case.txt")
			if err := os.WriteFile(path, []byte(content), 0o600); err != nil {
				t.Fatal(err)
			}
			if err := run("unused", path, "", true, time.Second); err != nil {
				t.Fatal(err)
			}
			got, err := os.ReadFile(path)
			if err != nil || string(got) != content {
				t.Fatalf("validation changed the file: %q, %v", got, err)
			}
		})
	}
}

func TestRejectMalformedSQLCases(t *testing.T) {
	t.Setenv("PATH", t.TempDir())
	for _, tc := range []struct {
		content string
		want    string
	}{
		{"# comment\nSELECT 1\n", ":2: query is missing its ?> answer marker"},
		{"SELECT 1\n?>\nSELECT 2\n", ":3: query is missing its ?> answer marker"},
		{"?>\n", ":1: answer marker has no query"},
		{"SELECT 1\n?>\n?>\n", ":3: answer marker has no query"},
		{"SELECT 1\n?>broken\n", ":2: expected ?>"},
		{"SELECT 1\n?> \n", ":2: answer must be JSON"},
		{"SELECT 1\n?> invalid\n", ":2: answer must be JSON"},
		{"SELECT 1\n?> error: \n", ":2: error answer must have a message"},
		{"param> {}\n", ":1: parameter has no query"},
		{"SELECT @p\nparam>{}\n?>\n", ":2: expected param>"},
		{"SELECT @p\nparam> bad\n?>\n", ":2: parameter must be a JSON object"},
		{"SELECT @p\nparam> null\n?>\n", ":2: parameter must be a JSON object"},
		{"SELECT @p\nparam> []\n?>\n", ":2: parameter must be a JSON object"},
		{"SELECT @p\nparam> {}\nSELECT 2\n?>\n", ":3: expected answer marker after parameters"},
	} {
		t.Run(tc.content, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "case.txt")
			if err := os.WriteFile(path, []byte(tc.content), 0o600); err != nil {
				t.Fatal(err)
			}
			// Both validation and collection must reject the file before invoking gcloud.
			for _, validate := range []bool{true, false} {
				if err := run("unused", path, "", validate, time.Second); err == nil || !strings.Contains(err.Error(), path+tc.want) {
					t.Fatalf("validate=%v: got %v, want %s", validate, err, path+tc.want)
				}
			}
			if err := fillFile(path, func(string, []json.RawMessage) (string, error) {
				t.Fatal("malformed file sent a query")
				return "", nil
			}); err == nil {
				t.Fatal("fillFile accepted malformed cases")
			}
			got, err := os.ReadFile(path)
			if err != nil || string(got) != tc.content {
				t.Fatalf("malformed file was changed: %q, %v", got, err)
			}
		})
	}
}
