package main

import (
	"context"
	"io"
	"net/http"
	"strings"
	"testing"
)

func TestRemoveExistingLocalAnchors(t *testing.T) {
	t.Parallel()
	u := origin + "/reference/standard-sql/string_functions"
	snapshot := filename(u)
	r := fixture(t, map[string]string{
		"docs/example.md":                       "[CONCAT](../" + snapshot + "#concat) and [LENGTH](../" + snapshot + "#length).\n",
		"src/example.cc":                        "// See " + snapshot + "#concat.\n",
		"cases/example.api.yaml":                "description: >-\n  See " + snapshot + "#length; other text.\nvars:\n  a: b\n",
		"tools/restprobe/functions_preamble.md": "[CONCAT](../" + snapshot + "#concat)\n",
		"tools/bigquery_functions.txt":          "ABS mathematical_functions#abs\n",
	})
	body := "# String functions\n\n## CONCAT\n\n[CONCAT](" + u + "#concat)\n"
	if err := r.write(snapshot, []byte(body)); err != nil {
		t.Fatal(err)
	}
	mathURL := origin + "/reference/standard-sql/mathematical_functions"
	if err := r.write(filename(mathURL), []byte("# Mathematical functions\n")); err != nil {
		t.Fatal(err)
	}
	r.manifest.Pages = []page{
		{URL: u, File: snapshot, SHA256: digest([]byte(body))},
		{URL: mathURL, File: filename(mathURL), SHA256: digest([]byte("# Mathematical functions\n"))},
	}
	if err := r.save(); err != nil {
		t.Fatal(err)
	}
	manifestBefore := contents(t, r, store+"/manifest.json")
	serve(t, r, func(w http.ResponseWriter, req *http.Request) { t.Error("anchor migration must be offline") })
	if err := r.run(context.Background(), "check", io.Discard); err == nil || !strings.Contains(err.Error(), "without an anchor") {
		t.Fatalf("%v", err)
	}
	if err := r.run(context.Background(), "sync", io.Discard); err != nil {
		t.Fatal(err)
	}
	for file, want := range map[string]string{
		"docs/example.md":                       "[CONCAT](../" + snapshot + ") and [LENGTH](../" + snapshot + ").\n",
		"src/example.cc":                        "// See " + snapshot + ".\n",
		"cases/example.api.yaml":                "description: >-\n  See " + snapshot + "; other text.\nvars:\n  a: b\n",
		"tools/restprobe/functions_preamble.md": "[CONCAT](../" + snapshot + ")\n",
		"tools/bigquery_functions.txt":          "ABS mathematical_functions#abs\n",
		snapshot:                                body,
		store + "/manifest.json":                manifestBefore,
	} {
		if got := contents(t, r, file); got != want {
			t.Errorf("%s: got %q, want %q", file, got, want)
		}
	}
	if err := r.run(context.Background(), "check", io.Discard); err != nil {
		t.Fatal(err)
	}
	if err := r.run(context.Background(), "sync", io.Discard); err != nil {
		t.Fatal(err)
	}
	rendered, err := r.render("docs/example.md", []byte("[CONCAT](../"+snapshot+"#concat) [remote]("+u+"#length)"))
	if err != nil {
		t.Fatal(err)
	}
	if want := "[CONCAT](../" + snapshot + ") [remote](../" + snapshot + ")"; string(rendered) != want {
		t.Fatalf("rendered %q, want %q", rendered, want)
	}
}
