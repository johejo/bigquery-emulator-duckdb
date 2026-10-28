package goclient

import (
	"compress/gzip"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"strings"
	"testing"
)

func TestHTTPResponseCompression(t *testing.T) {
	t.Parallel()
	endpoint := os.Getenv("BQ_EMULATOR_API")
	if endpoint == "" {
		t.Skip("BQ_EMULATOR_API is not set; run just e2e")
	}
	project := os.Getenv("BQ_EMULATOR_PROJECT")
	if project == "" {
		project = "test"
	}
	transport := http.DefaultTransport.(*http.Transport).Clone()
	// Inspect the wire encoding and decode it ourselves, not through Go's automatic gzip support.
	transport.DisableCompression = true
	t.Cleanup(transport.CloseIdleConnections)
	client := &http.Client{Transport: transport}

	for _, encoding := range []string{"identity", "gzip"} {
		t.Run(encoding, func(t *testing.T) {
			t.Parallel()
			request, err := http.NewRequestWithContext(t.Context(), http.MethodPost,
				endpoint+"/bigquery/v2/projects/"+project+"/queries",
				strings.NewReader(`{"query":"SELECT 1","useLegacySql":false}`))
			if err != nil {
				t.Fatal(err)
			}
			request.Header.Set("Content-Type", "application/json")
			request.Header.Set("Accept-Encoding", encoding)
			request.Header.Set("User-Agent", "emulator-e2e (gzip)")
			response, err := client.Do(request)
			if err != nil {
				t.Fatal(err)
			}
			defer response.Body.Close()
			if response.StatusCode != http.StatusOK {
				t.Fatalf("status = %s", response.Status)
			}
			wantEncoding := ""
			if encoding == "gzip" {
				wantEncoding = "gzip"
			}
			if got := response.Header.Get("Content-Encoding"); got != wantEncoding {
				t.Fatalf("Content-Encoding = %q, want %q", got, wantEncoding)
			}
			var body io.Reader = response.Body
			if encoding == "gzip" {
				reader, err := gzip.NewReader(body)
				if err != nil {
					t.Fatal(err)
				}
				defer reader.Close()
				body = reader
			}
			// Read through EOF to validate the gzip checksum as well as the JSON payload.
			data, err := io.ReadAll(body)
			if err != nil {
				t.Fatal(err)
			}
			var result queryResponse
			if err := json.Unmarshal(data, &result); err != nil {
				t.Fatal(err)
			}
			if len(result.Rows) != 1 || len(result.Rows[0].F) != 1 || result.Rows[0].F[0].V != "1" {
				t.Fatalf("unexpected query result: %s", data)
			}
		})
	}
}
