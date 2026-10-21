package main

import (
	"context"
	"net/http"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

// Exercise the complete workflows locally, including upload, job polling and gRPC streams. All
// observations go into temporary copies and are discarded: emulator results are never BigQuery
// answers. Compatibility expectations belong in the client tests, not in this tool test.
func TestScenarioFixturesAgainstEmulator(t *testing.T) {
	t.Parallel()
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("run just e2e")
	}
	startup, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	process, err := emulatorprocess.Start(startup, binary, "", "--host", "127.0.0.1", "--port", "0", "--project", `{"projectId":"test"}`)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = process.Stop() })
	transport := newScenarioTransport("")
	transport.restEndpoint = process.URL
	transport.grpcEndpoint = process.GRPC
	transport.grpcOptions = []grpc.DialOption{grpc.WithTransportCredentials(insecure.NewCredentials())}
	t.Cleanup(func() { _ = transport.Close() })
	paths, err := filepath.Glob("testdata/*.api.json")
	if err != nil {
		t.Fatal(err)
	}
	for _, source := range paths {
		data, err := os.ReadFile(source)
		if err != nil {
			t.Fatal(err)
		}
		path := filepath.Join(t.TempDir(), filepath.Base(source))
		if err := os.WriteFile(path, data, 0o600); err != nil {
			t.Fatal(err)
		}
		if err := fillScenarios(t.Context(), path, "test", "smoke", 20*time.Second, transport.call); err != nil {
			t.Fatal(err)
		}
		file, err := readScenarios(path)
		if err != nil {
			t.Fatal(err)
		}
		for _, c := range file.Cases {
			if !answered(c.Answer) {
				t.Errorf("%s/%s: no observation", source, c.Name)
			}
		}
	}
	response, err := transport.rest(t.Context(), restRequest{Method: http.MethodGet, Path: "/bigquery/v2/projects/test/datasets"}, nil)
	if err != nil || response.Failed {
		t.Fatalf("list datasets: %+v, %v", response, err)
	}
	datasets, present, err := pointer(response.Value, "/body/datasets")
	if err != nil {
		t.Fatal(err)
	}
	if present && len(datasets.([]any)) != 0 {
		t.Fatalf("scenario cleanup left datasets: %v", datasets)
	}
}
