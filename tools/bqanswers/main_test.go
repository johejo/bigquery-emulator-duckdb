package main

import (
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestFillAnswers(t *testing.T) {
	for _, directory := range []bool{false, true} {
		t.Run(map[bool]string{false: "file", true: "directory"}[directory], func(t *testing.T) {
			dir := t.TempDir()
			contents := map[string]string{
				"a.txt":     "# comment\nSELECT\n  1\n?>\n\nSELECT 2\n?> \"2\"\n",
				"b.txt":     "SELECT 3\n?>\n",
				"README.md": "instructions\n",
			}
			for name, content := range contents {
				if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o644); err != nil {
					t.Fatal(err)
				}
			}
			path := filepath.Join(dir, "a.txt")
			wantQueries := []string{"SELECT\n  1"}
			contents["a.txt"] = strings.Replace(contents["a.txt"], "?>\n", "?> \"answer\"\n", 1)
			if directory {
				path = dir
				wantQueries = append(wantQueries, "SELECT 3")
				contents["b.txt"] = "SELECT 3\n?> \"answer\"\n"
			}
			var queries []string
			if err := fillAnswers(path, func(sql string) (string, error) {
				queries = append(queries, sql)
				return `"answer"`, nil
			}); err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(queries, wantQueries) {
				t.Fatalf("queries = %q, want %q", queries, wantQueries)
			}
			for name, want := range contents {
				got, err := os.ReadFile(filepath.Join(dir, name))
				if err != nil {
					t.Fatal(err)
				}
				if string(got) != want {
					t.Errorf("%s = %q, want %q", name, got, want)
				}
			}
		})
	}
}

func TestFillAnswersContinuesAfterFailure(t *testing.T) {
	dir := t.TempDir()
	for name, content := range map[string]string{
		"a.txt": "SELECT 1\n?>\nSELECT 2\n?>\n",
		"b.txt": "SELECT 3\n?>\n",
	} {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	failure := errors.New("query failed")
	err := fillAnswers(dir, func(sql string) (string, error) {
		if sql == "SELECT 1" {
			return "", failure
		}
		return `"answer"`, nil
	})
	if !errors.Is(err, failure) || !strings.Contains(err.Error(), "a.txt:2:") {
		t.Fatalf("error = %v, want query failure with file and line", err)
	}
	for name, want := range map[string]string{
		"a.txt": "SELECT 1\n?>\nSELECT 2\n?> \"answer\"\n",
		"b.txt": "SELECT 3\n?> \"answer\"\n",
	} {
		got, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			t.Fatal(err)
		}
		if string(got) != want {
			t.Errorf("%s = %q, want %q", name, got, want)
		}
	}
}
