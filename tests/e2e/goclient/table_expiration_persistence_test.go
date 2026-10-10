package goclient

import (
	"context"
	"os"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
	"google.golang.org/api/googleapi"
	"google.golang.org/api/option"
)

func TestTableExpirationPersistence(t *testing.T) {
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("BQ_EMULATOR_BINARY is not set; run just e2e")
	}
	ctx := t.Context()
	dir := t.TempDir()
	start := func() (*emulatorprocess.Process, *bigquery.Client) {
		t.Helper()
		startup, cancel := context.WithTimeout(ctx, 15*time.Second)
		defer cancel()
		p, err := emulatorprocess.Start(startup, binary, "", "--host", "127.0.0.1", "--port", "0",
			"--data-dir", dir, `--project={"projectId":"expiration"}`)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() {
			if err := p.Stop(); err != nil {
				t.Error(err)
			}
		})
		client, err := bigquery.NewClient(ctx, "expiration", option.WithEndpoint(p.URL), option.WithoutAuthentication())
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = client.Close() })
		return p, client
	}
	p, client := start()
	dataset := client.Dataset("ds")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	future := time.Date(2099, 1, 1, 0, 0, 0, 0, time.UTC)
	deadline := time.Now().Add(2 * time.Second).Truncate(time.Millisecond)
	schema := bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}}
	for name, expiration := range map[string]time.Time{"future": future, "expired": deadline} {
		if err := dataset.Table(name).Create(ctx, &bigquery.TableMetadata{Schema: schema, ExpirationTime: expiration}); err != nil {
			t.Fatal(err)
		}
	}
	if err := p.Stop(); err != nil {
		t.Fatal(err)
	}
	time.Sleep(time.Until(deadline.Add(time.Millisecond)))
	p, client = start()
	dataset = client.Dataset("ds")
	got, err := dataset.Table("future").Metadata(ctx)
	if err != nil || !got.ExpirationTime.Equal(future) {
		t.Fatalf("restored expiration: got %v, %v", got, err)
	}
	_, err = dataset.Table("expired").Metadata(ctx)
	apiError, ok := err.(*googleapi.Error)
	if !ok || apiError.Code != 404 {
		t.Fatalf("restored expired table: got %v, want 404", err)
	}
	if err := p.Stop(); err != nil {
		t.Fatal(err)
	}
	_, client = start()
	if err := client.Dataset("ds").Table("expired").Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatalf("expiration deletion did not persist: %v", err)
	}
}
