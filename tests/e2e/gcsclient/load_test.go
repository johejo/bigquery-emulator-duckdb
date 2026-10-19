package gcsclient

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
	"google.golang.org/api/iterator"
	"google.golang.org/api/option"
)

// These tests exercise loads through the public client while a controlled Storage
// server supplies permissions, pagination and failures that fixtures cannot express.
func start(t *testing.T) *bigquery.Client {
	t.Helper()
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set; run just e2e")
	}
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	p, err := emulatorprocess.Start(ctx, binary, "", "--host", "127.0.0.1", "--port", "0", `--project={"projectId":"test"}`)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := p.Stop(); err != nil {
			t.Error(err)
		}
	})
	client, err := bigquery.NewClient(t.Context(), "test", option.WithEndpoint(p.URL+"/bigquery/v2/"), option.WithoutAuthentication())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := client.Close(); err != nil {
			t.Error(err)
		}
	})
	if err := client.Dataset("gcs").Create(t.Context(), nil); err != nil {
		t.Fatal(err)
	}
	return client
}

func configure(t *testing.T) {
	t.Helper()
	if os.Getenv("BQ_EMULATOR_BINARY") == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set; run just e2e")
	}
	for _, name := range []string{"STORAGE_EMULATOR_HOST", "CLOUD_STORAGE_EMULATOR_ENDPOINT", "CLOUD_STORAGE_TESTBENCH_ENDPOINT", "GOOGLE_APPLICATION_CREDENTIALS"} {
		old, exists := os.LookupEnv(name)
		t.Cleanup(func() {
			if exists {
				_ = os.Setenv(name, old)
			} else {
				_ = os.Unsetenv(name)
			}
		})
		if err := os.Unsetenv(name); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("GOOGLE_APPLICATION_CREDENTIALS", filepath.Join(t.TempDir(), "missing.json"))
}

func load(t *testing.T, client *bigquery.Client, uri string, want []int64) {
	t.Helper()
	ctx, cancel := context.WithTimeout(t.Context(), 30*time.Second)
	defer cancel()
	source := bigquery.NewGCSReference(uri)
	source.SourceFormat = bigquery.JSON
	source.Schema = bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}}
	job, err := client.Dataset("gcs").Table("rows").LoaderFrom(source).Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	status, err := job.Wait(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if err := status.Err(); err != nil {
		t.Fatal(err)
	}
	rows, err := client.Query("SELECT id FROM gcs.rows ORDER BY id").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var got []int64
	for {
		var row struct {
			ID int64 `bigquery:"id"`
		}
		err := rows.Next(&row)
		if err == iterator.Done {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		got = append(got, row.ID)
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("rows = %v, want %v", got, want)
	}
}

func TestEndpointConfiguration(t *testing.T) {
	for _, name := range []string{"host", "prefix", "sdk", "testbench", "sdk_precedence"} {
		t.Run(name, func(t *testing.T) {
			configure(t)
			prefix := ""
			if name == "prefix" {
				prefix = "/prefix"
			}
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path != prefix+"/storage/v1/b/bucket/o/nested/a.json" || r.URL.Query().Get("alt") != "media" {
					http.Error(w, "unexpected request", http.StatusNotFound)
					return
				}
				fmt.Fprintln(w, `{"id":1}`)
			}))
			defer server.Close()
			switch name {
			case "host":
				t.Setenv("STORAGE_EMULATOR_HOST", strings.TrimPrefix(server.URL, "http://"))
			case "prefix":
				t.Setenv("STORAGE_EMULATOR_HOST", server.URL+prefix+"///")
			case "sdk":
				t.Setenv("STORAGE_EMULATOR_HOST", "http://127.0.0.1:1")
				t.Setenv("CLOUD_STORAGE_EMULATOR_ENDPOINT", server.URL)
			case "testbench":
				t.Setenv("STORAGE_EMULATOR_HOST", "http://127.0.0.1:1")
				t.Setenv("CLOUD_STORAGE_TESTBENCH_ENDPOINT", server.URL)
			case "sdk_precedence":
				t.Setenv("CLOUD_STORAGE_TESTBENCH_ENDPOINT", "http://127.0.0.1:1")
				t.Setenv("CLOUD_STORAGE_EMULATOR_ENDPOINT", server.URL)
			}
			load(t, start(t), "gs://bucket/nested/a.json", []int64{1})
		})
	}
}

func TestEmulatorDoesNotUseCredentials(t *testing.T) {
	for _, name := range []string{"STORAGE_EMULATOR_HOST", "CLOUD_STORAGE_EMULATOR_ENDPOINT", "CLOUD_STORAGE_TESTBENCH_ENDPOINT"} {
		t.Run(name, func(t *testing.T) {
			configure(t)
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.URL.Path == "/token" || r.Header.Get("Authorization") != "" {
					t.Error("emulator attempted authentication")
					http.Error(w, "authentication is not available", http.StatusForbidden)
					return
				}
				fmt.Fprintln(w, `{"id":1}`)
			}))
			defer server.Close()
			dir := t.TempDir()
			subject := filepath.Join(dir, "subject")
			if err := os.WriteFile(subject, []byte("subject-token"), 0600); err != nil {
				t.Fatal(err)
			}
			credentials := filepath.Join(dir, "credentials.json")
			adc := fmt.Sprintf(`{"type":"external_account","audience":"//iam.googleapis.com/projects/123/locations/global/workloadIdentityPools/pool/providers/provider","subject_token_type":"urn:ietf:params:oauth:token-type:jwt","token_url":%q,"credential_source":{"file":%q}}`, server.URL+"/token", subject)
			if err := os.WriteFile(credentials, []byte(adc), 0600); err != nil {
				t.Fatal(err)
			}
			t.Setenv("GOOGLE_APPLICATION_CREDENTIALS", credentials)
			t.Setenv(name, server.URL)
			load(t, start(t), "gs://bucket/nested/a.json", []int64{1})
		})
	}
}

func TestLoadWithoutListPermission(t *testing.T) {
	configure(t)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/storage/v1/b/bucket/o/nested/a.json" {
			http.Error(w, "listing is forbidden", http.StatusForbidden)
			return
		}
		fmt.Fprintln(w, `{"id":1}`)
	}))
	defer server.Close()
	t.Setenv("STORAGE_EMULATOR_HOST", server.URL)
	load(t, start(t), "gs://bucket/nested/a.json", []int64{1})
}

func TestLoadRetriesTransientFailure(t *testing.T) {
	configure(t)
	var requests atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if requests.Add(1) == 1 {
			http.Error(w, "temporary failure", http.StatusServiceUnavailable)
			return
		}
		fmt.Fprintln(w, `{"id":1}`)
	}))
	defer server.Close()
	t.Setenv("STORAGE_EMULATOR_HOST", server.URL)
	load(t, start(t), "gs://bucket/nested/a.json", []int64{1})
}

func TestLoadAllListingPages(t *testing.T) {
	configure(t)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch r.URL.Path {
		case "/storage/v1/b/bucket/o":
			if r.URL.Query().Get("prefix") != "nested/a" {
				http.Error(w, "wrong prefix", http.StatusBadRequest)
				return
			}
			switch r.URL.Query().Get("pageToken") {
			case "":
				fmt.Fprintln(w, `{"items":[{"name":"nested/a2.json"}],"nextPageToken":"next"}`)
			case "next":
				fmt.Fprintln(w, `{"items":[{"name":"nested/a1.json"}]}`)
			default:
				http.Error(w, "wrong page token", http.StatusBadRequest)
			}
		case "/storage/v1/b/bucket/o/nested/a1.json":
			fmt.Fprintln(w, `{"id":1}`)
		case "/storage/v1/b/bucket/o/nested/a2.json":
			fmt.Fprintln(w, `{"id":2}`)
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	t.Setenv("STORAGE_EMULATOR_HOST", server.URL)
	load(t, start(t), "gs://bucket/nested/a*.json", []int64{1, 2})
}

func TestLoadsReuseConnections(t *testing.T) {
	configure(t)
	var connections atomic.Int32
	server := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintln(w, `{"id":1}`)
	}))
	server.Config.ConnState = func(_ net.Conn, state http.ConnState) {
		if state == http.StateNew {
			connections.Add(1)
		}
	}
	server.Start()
	defer server.Close()
	t.Setenv("STORAGE_EMULATOR_HOST", server.URL)
	client := start(t)
	load(t, client, "gs://bucket/nested/a.json", []int64{1})
	load(t, client, "gs://bucket/nested/a.json", []int64{1, 1})
	if got := connections.Load(); got != 1 {
		t.Errorf("connections = %d, want 1", got)
	}
}
