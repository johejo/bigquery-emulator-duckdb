package main

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func fixture(t *testing.T, files map[string]string) *repo {
	t.Helper()
	r := &repo{root: t.TempDir(), client: http.DefaultClient, key: "test-secret"}
	files["tools/bigquerydocs/exceptions.json"] = "[]\n"
	for name, body := range files {
		if err := r.write(name, []byte(body)); err != nil {
			t.Fatal(err)
		}
	}
	for _, args := range [][]string{{"init", "-q"}, {"add", "."}} {
		cmd := exec.Command("git", args...)
		cmd.Dir = r.root
		if b, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("git: %s: %v", b, err)
		}
	}
	if err := r.load(); err != nil {
		t.Fatal(err)
	}
	return r
}
func serve(t *testing.T, r *repo, handler http.HandlerFunc) {
	t.Helper()
	s := httptest.NewServer(handler)
	t.Cleanup(s.Close)
	r.endpoint = s.URL
	r.client = s.Client()
}
func answer(w http.ResponseWriter, u, body string) {
	_ = json.NewEncoder(w).Encode(document{Name: "documents/" + strings.TrimPrefix(u, "https://"), URI: u, Title: "Reference", Content: body})
}
func contents(t *testing.T, r *repo, name string) string {
	t.Helper()
	b, err := r.read(name)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

func TestSyncLifecycle(t *testing.T) {
	t.Parallel()
	u := origin + "/reference/standard-sql/string_functions"
	raw := "https://cloud.google.com/bigquery/docs/reference/standard-sql/string_functions"
	r := fixture(t, map[string]string{
		"docs/example.md":                       "[CONCAT](" + raw + "#concat) and [LENGTH](" + u + "?hl=ja#length).\n",
		"src/example.cc":                        "// " + raw + "#concat.\n",
		"tools/restprobe/functions_preamble.md": "[CONCAT](" + raw + "#concat)\n",
		"cases/example.api.yaml":                "description: >-\n  See " + raw + "#concat.\nvars:\n  a: b\n",
	})
	calls := 0
	serve(t, r, func(w http.ResponseWriter, req *http.Request) {
		calls++
		if req.Header.Get("X-Goog-Api-Key") != "test-secret" {
			t.Error("missing authentication header")
		}
		if req.URL.Path != "/v1/documents/docs.cloud.google.com/bigquery/docs/reference/standard-sql/string_functions" || req.URL.Query().Get("view") != "DOCUMENT_VIEW_CONTENT" {
			t.Errorf("request: %s", req.URL)
		}
		answer(w, u, "# String functions\n\n## CONCAT {#concat}\n\n## LENGTH {#length}\n")
	})
	if err := r.run(context.Background(), "sync", io.Discard); err != nil {
		t.Fatal(err)
	}
	if calls != 1 {
		t.Fatalf("calls=%d, want 1", calls)
	}
	localPath := "../" + filename(u)
	if got := contents(t, r, "docs/example.md"); got != "[CONCAT]("+localPath+") and [LENGTH]("+localPath+").\n" {
		t.Fatal(got)
	}
	if got := contents(t, r, "src/example.cc"); got != "// "+filename(u)+".\n" {
		t.Fatal(got)
	}
	if got := contents(t, r, "tools/restprobe/functions_preamble.md"); got != "[CONCAT]("+localPath+")\n" {
		t.Fatal(got)
	}
	if got := contents(t, r, "cases/example.api.yaml"); !strings.Contains(got, filename(u)+".") {
		t.Fatal(got)
	}
	before := contents(t, r, store+"/manifest.json")
	if err := r.run(context.Background(), "sync", io.Discard); err != nil {
		t.Fatal(err)
	}
	if calls != 1 {
		t.Fatal("sync fetched an existing snapshot")
	}
	if err := r.run(context.Background(), "check", io.Discard); err != nil {
		t.Fatal(err)
	}
	if calls != 1 {
		t.Fatal("check used network")
	}
	if err := r.run(context.Background(), "refresh", io.Discard); err != nil {
		t.Fatal(err)
	}
	if calls != 2 {
		t.Fatalf("refresh calls=%d", calls)
	}
	if after := contents(t, r, store+"/manifest.json"); after != before {
		t.Fatal("unchanged refresh changed manifest")
	}
	// Report generation must not undo localization, and unsnapshotted links stay remote.
	rendered, err := r.render("docs/functions.md", []byte("[CONCAT]("+raw+"#concat) [missing]("+origin+"/missing)"))
	if err != nil {
		t.Fatal(err)
	}
	if string(rendered) != "[CONCAT]("+localPath+") [missing]("+origin+"/missing)" {
		t.Fatal(string(rendered))
	}
	// Reloading the manifest must preserve the offline workflow.
	if err := r.load(); err != nil {
		t.Fatal(err)
	}
	if err := r.run(context.Background(), "check", io.Discard); err != nil {
		t.Fatal(err)
	}
}

func TestPartialFailureAndExceptions(t *testing.T) {
	t.Parallel()
	a, b := origin+"/a", origin+"/b"
	r := fixture(t, map[string]string{
		"README.md":         "[a](" + a + ") [b](" + b + ")\n",
		"src/errors.cc":     "return \"" + a + "\";\n// " + a + "\n",
		"tests/answers.txt": "=> error: " + a + "\n# " + a + "\n",
	})
	r.exceptions = []exception{{"src/errors.cc", "return ", "Client error text"}, {"tests/answers.txt", "=> error:", "Recorded expected error"}}
	calls := 0
	serve(t, r, func(w http.ResponseWriter, req *http.Request) {
		calls++
		if strings.HasSuffix(req.URL.Path, "/b") {
			http.Error(w, "not in corpus", 404)
			return
		}
		answer(w, a, "# A\n")
	})
	err := r.run(context.Background(), "sync", io.Discard)
	if err == nil || !strings.Contains(err.Error(), "HTTP 404") {
		t.Fatalf("%v", err)
	}
	if calls != 2 {
		t.Fatalf("calls=%d", calls)
	}
	if got := contents(t, r, "README.md"); got != "[a]("+filename(a)+") [b]("+b+")\n" {
		t.Fatal(got)
	}
	if got := contents(t, r, "src/errors.cc"); got != "return \""+a+"\";\n// "+filename(a)+"\n" {
		t.Fatal(got)
	}
	if got := contents(t, r, "tests/answers.txt"); got != "=> error: "+a+"\n# "+filename(a)+"\n" {
		t.Fatal(got)
	}
	if _, err := r.read(filename(b)); !os.IsNotExist(err) {
		t.Fatalf("failed fetch created snapshot: %v", err)
	}
}
func TestRejectUnexpectedDocument(t *testing.T) {
	t.Parallel()
	for _, mode := range []string{"empty", "wrong-uri", "wrong-name", "invalid-json", "unauthorized"} {
		t.Run(mode, func(t *testing.T) {
			t.Parallel()
			u := origin + "/a"
			r := fixture(t, map[string]string{"README.md": u + "\n"})
			serve(t, r, func(w http.ResponseWriter, req *http.Request) {
				switch mode {
				case "empty":
					answer(w, u, "")
				case "wrong-uri":
					answer(w, origin+"/b", "# B")
				case "wrong-name":
					_ = json.NewEncoder(w).Encode(document{URI: u, Content: "# A", Name: "documents/wrong"})
				case "invalid-json":
					fmt.Fprint(w, "invalid")
				case "unauthorized":
					http.Error(w, "test-secret", http.StatusUnauthorized)
				}
			})
			err := r.run(context.Background(), "sync", io.Discard)
			if err == nil {
				t.Fatal("unexpected success")
			}
			if strings.Contains(err.Error(), "test-secret") {
				t.Fatal("error leaked API key")
			}
			if got := contents(t, r, "README.md"); got != u+"\n" {
				t.Fatal("citation changed after failed fetch")
			}
			if _, err := r.read(filename(u)); !os.IsNotExist(err) {
				t.Fatalf("snapshot exists: %v", err)
			}
		})
	}
}
func TestCheckAndScanAreOffline(t *testing.T) {
	t.Parallel()
	u := origin + "/a"
	r := fixture(t, map[string]string{"README.md": u + "\n", "code.go": "var link = \"" + u + "\"\n"})
	serve(t, r, func(w http.ResponseWriter, req *http.Request) { t.Error("unexpected network access") })
	var out bytes.Buffer
	if err := r.run(context.Background(), "scan", &out); err == nil || !strings.Contains(err.Error(), "unclassified URL") {
		t.Fatalf("%v", err)
	}
	if !strings.Contains(out.String(), "missing "+u) || !strings.Contains(out.String(), "README.md") {
		t.Fatal(out.String())
	}
	if err := r.run(context.Background(), "check", io.Discard); err == nil || !strings.Contains(err.Error(), "missing snapshot") {
		t.Fatalf("%v", err)
	}
}
func TestRelativeFunctionIndex(t *testing.T) {
	t.Parallel()
	r := fixture(t, map[string]string{"tools/bigquery_functions.txt": "# Header\nABS mathematical_functions#abs\nACOS mathematical_functions#acos\n"})
	refs, problems, err := r.scan()
	if err != nil || len(problems) > 0 {
		t.Fatalf("%v %v", err, problems)
	}
	if len(refs) != 2 || refs[0].url != origin+"/reference/standard-sql/mathematical_functions" || refs[0].fragment != "#abs" {
		t.Fatalf("%+v", refs)
	}
}
func TestCorruptSnapshotAndWrongLocalPath(t *testing.T) {
	t.Parallel()
	u := origin + "/a"
	r := fixture(t, map[string]string{"docs/a.md": u + "\n"})
	serve(t, r, func(w http.ResponseWriter, req *http.Request) { answer(w, u, "# A") })
	if err := r.run(context.Background(), "sync", io.Discard); err != nil {
		t.Fatal(err)
	}
	if err := r.write("docs/a.md", []byte("[a]("+filename(u)+")\n")); err != nil {
		t.Fatal(err)
	}
	if err := r.run(context.Background(), "check", io.Discard); err == nil || !strings.Contains(err.Error(), "incorrect local path") {
		t.Fatalf("%v", err)
	}
	if err := r.write(filename(u), []byte("changed")); err != nil {
		t.Fatal(err)
	}
	if err := r.run(context.Background(), "check", io.Discard); err == nil || !strings.Contains(err.Error(), "hash mismatch") {
		t.Fatalf("%v", err)
	}
	if _, err := r.render("docs/a.md", []byte(u)); err == nil {
		t.Fatal("render accepted corrupt snapshot")
	}
}
func TestCanonical(t *testing.T) {
	t.Parallel()
	for _, raw := range []string{"https://cloud.google.com/bigquery/docs/a?hl=ja#b", "https://docs.cloud.google.com/bigquery/docs/a/#b"} {
		u, f, err := canonical(raw)
		if err != nil || u != origin+"/a" || f != "#b" {
			t.Fatalf("%s => %s %s %v", raw, u, f, err)
		}
	}
	for _, raw := range []string{origin + "/a?unknown=value", origin + "/../escape", origin + "/%2e%2e/escape", "https://example.org/bigquery/docs/a", "https://cloud.google.com/bigquery/docs-other"} {
		if _, _, err := canonical(raw); err == nil {
			t.Fatalf("accepted %s", raw)
		}
	}
}
func TestManifestPathValidation(t *testing.T) {
	t.Parallel()
	r := fixture(t, map[string]string{})
	m := manifest{Pages: []page{{URL: origin + "/a", File: "../../escape"}}}
	b, err := json.Marshal(m)
	if err != nil {
		t.Fatal(err)
	}
	if err := r.write(store+"/manifest.json", b); err != nil {
		t.Fatal(err)
	}
	if err := r.load(); err == nil {
		t.Fatal("accepted unsafe manifest path")
	}
}
func TestIgnoreUntrackedAndSnapshotLinks(t *testing.T) {
	t.Parallel()
	r := fixture(t, map[string]string{store + "/a.md": origin + "/nested\n"})
	if err := r.write("untracked.md", []byte(origin+"/untracked")); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(filepath.Join(r.root, store, "a.md"), filepath.Join(r.root, "link.md")); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command("git", "add", "link.md")
	cmd.Dir = r.root
	if err := cmd.Run(); err != nil {
		t.Fatal(err)
	}
	refs, problems, err := r.scan()
	if err != nil || len(refs) != 0 || len(problems) != 0 {
		t.Fatalf("%+v %+v %v", refs, problems, err)
	}
}
