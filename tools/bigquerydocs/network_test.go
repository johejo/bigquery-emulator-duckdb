package main

import (
	"context"
	"io"
	"net/http"
	"testing"
)

func TestRetryAndFailedRefresh(t *testing.T) {
	t.Parallel()
	u := origin + "/a"
	r := fixture(t, map[string]string{"README.md": u + "\n"})
	calls := 0
	fail := false
	serve(t, r, func(w http.ResponseWriter, req *http.Request) {
		calls++
		if fail {
			http.Error(w, "not found", http.StatusNotFound)
			return
		}
		if calls == 1 {
			http.Error(w, "retry", http.StatusServiceUnavailable)
			return
		}
		answer(w, u, "# A\n")
	})
	if err := r.run(context.Background(), "sync", io.Discard); err != nil {
		t.Fatal(err)
	}
	if calls != 2 {
		t.Fatalf("calls=%d", calls)
	}
	before := contents(t, r, store+"/manifest.json")
	fail = true
	if err := r.run(context.Background(), "refresh", io.Discard); err == nil {
		t.Fatal("refresh unexpectedly succeeded")
	}
	if contents(t, r, filename(u)) != "# A\n" || contents(t, r, store+"/manifest.json") != before {
		t.Fatal("failed refresh damaged existing snapshot")
	}
	if err := r.run(context.Background(), "check", io.Discard); err != nil {
		t.Fatal(err)
	}
}

func TestCancelledRetry(t *testing.T) {
	t.Parallel()
	r := fixture(t, map[string]string{})
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	serve(t, r, func(w http.ResponseWriter, req *http.Request) {
		cancel()
		http.Error(w, "retry", http.StatusTooManyRequests)
	})
	if _, err := r.fetch(ctx, origin+"/a"); err == nil {
		t.Fatal("cancelled fetch succeeded")
	}
}

func TestInlineCommentClassification(t *testing.T) {
	t.Parallel()
	u := origin + "/a"
	for _, tc := range []struct {
		line string
		want bool
	}{
		{"int x = 0; // See " + u, true},
		{"var x = \"not a comment //\" // " + u, true},
		{"var x = \"" + u + "\"", false},
		{"var x = \"" + u + "\" // " + u, false},
	} {
		if got := citationLine("src/example.cc", tc.line); got != tc.want {
			t.Errorf("%q: got %v, want %v", tc.line, got, tc.want)
		}
	}
}
