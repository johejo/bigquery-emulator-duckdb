package main

import (
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
)

// probeParquet is a Parquet file of one row, an OPTIONAL INT64 column x of 1, written by arrow-go
// v18.8.0, so that probes of external tables, which read their sources, can run.
const probeParquet = "UEFSMRUAFRwVHCwVAhUAFQYVBhwAAAACAAAAAgEBAAAAAAAAABUEGSxIBnNjaGVtYRUCABUEJQIYAXgAFgIZHBkcJggcFQQZJQAGGRgBeBUAFgIWQhZCJghJHBUAFQAVAgAAABZCFgImCBZCFAAAKBBhcnJvdy1nbyB2MTguOC4wGRwcAAAAYgAAAFBBUjE="

// serveStorage serves the Cloud Storage JSON API with every object of every bucket probeParquet,
// and a listing of one object under the requested prefix, and points emulators started after it
// at the server. It is never closed; the process exits after probing.
func serveStorage() error {
	contents, err := base64.StdEncoding.DecodeString(probeParquet)
	if err != nil {
		return err
	}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		path, ok := strings.CutPrefix(r.URL.Path, "/storage/v1/b/")
		if !ok {
			http.NotFound(w, r)
			return
		}
		if _, object, ok := strings.Cut(path, "/o/"); ok && object != "" {
			_, _ = w.Write(contents)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{"items": []map[string]string{{"name": r.URL.Query().Get("prefix") + "probe.parquet"}}})
	}))
	return os.Setenv("STORAGE_EMULATOR_HOST", server.URL)
}
