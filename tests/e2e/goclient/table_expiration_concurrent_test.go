package goclient

import (
	"context"
	"fmt"
	"net/http"
	"os"
	"sync"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	bqapi "google.golang.org/api/bigquery/v2"
	"google.golang.org/api/googleapi"
	"google.golang.org/api/option"
)

// Other runbooks query separate datasets, but their expiration sweeps are global.
// Successful mutations must still return their resource when those sweeps run concurrently.
func TestTableExpirationConcurrentMutations(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := client.Dataset("go_expiration_mutations")
	queries := client.Dataset("go_expiration_queries")
	for _, ds := range []string{dataset.DatasetID, queries.DatasetID} {
		d := client.Dataset(ds)
		_ = d.DeleteWithContents(ctx)
		if err := d.Create(ctx, nil); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = d.DeleteWithContents(context.Background()) })
	}
	service, err := bqapi.NewService(ctx,
		option.WithEndpoint(os.Getenv("BQ_EMULATOR_API")+"/bigquery/v2/"),
		option.WithoutAuthentication())
	if err != nil {
		t.Fatal(err)
	}

	sweepCtx, cancel := context.WithCancel(ctx)
	var workers sync.WaitGroup
	ready := make(chan struct{}, 2)
	errors := make(chan error, 2)
	t.Cleanup(func() {
		cancel()
		workers.Wait()
		close(errors)
		for err := range errors {
			t.Errorf("concurrent query: %v", err)
		}
	})
	for range 2 {
		workers.Go(func() {
			started := false
			for sweepCtx.Err() == nil {
				query := client.Query("SELECT 1")
				query.DefaultDatasetID = queries.DatasetID
				if _, err := query.Read(sweepCtx); err != nil {
					if sweepCtx.Err() == nil {
						errors <- err
					}
					return
				}
				if !started {
					ready <- struct{}{}
					started = true
				}
			}
		})
	}
	for range 2 {
		select {
		case <-ready:
		case err := <-errors:
			t.Fatal(err)
		case <-ctx.Done():
			t.Fatal(ctx.Err())
		}
	}

	// tables.update is PUT; the higher-level client's Table.Update only sends PATCH.
	for i := range 16 {
		for _, kind := range []string{"table", "view"} {
			for _, method := range []string{"insert", "patch", "update"} {
				name := fmt.Sprintf("%s_%s_%d", kind, method, i)
				body := &bqapi.Table{TableReference: &bqapi.TableReference{
					ProjectId: dataset.ProjectID, DatasetId: dataset.DatasetID, TableId: name,
				}}
				if kind == "view" {
					body.View = &bqapi.ViewDefinition{Query: "SELECT 1 AS id", UseLegacySql: false,
						ForceSendFields: []string{"UseLegacySql"}}
				} else {
					body.Schema = &bqapi.TableSchema{Fields: []*bqapi.TableFieldSchema{{Name: "id", Type: "INTEGER"}}}
				}
				if method != "insert" {
					if _, err := service.Tables.Insert(dataset.ProjectID, dataset.DatasetID, body).Context(ctx).Do(); err != nil {
						t.Fatal(err)
					}
				}
				body.ExpirationTime = 1
				var got *bqapi.Table
				var err error
				switch method {
				case "insert":
					got, err = service.Tables.Insert(dataset.ProjectID, dataset.DatasetID, body).Context(ctx).Do()
				case "patch":
					got, err = service.Tables.Patch(dataset.ProjectID, dataset.DatasetID, name, body).Context(ctx).Do()
				case "update":
					got, err = service.Tables.Update(dataset.ProjectID, dataset.DatasetID, name, body).Context(ctx).Do()
				}
				if err != nil {
					t.Fatalf("%s: %v", name, err)
				}
				if got.TableReference.TableId != name || got.ExpirationTime != body.ExpirationTime {
					t.Fatalf("%s: got table %s, expiration %d", name, got.TableReference.TableId, got.ExpirationTime)
				}
			}
		}
	}
}

// Readers arriving together at an expired table must all see absence, rather than one of them
// failing while another deletes it.
func TestTableExpirationConcurrentReads(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := client.Dataset("go_expiration_reads")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(context.Background()) })
	table := dataset.Table("t")
	if err := table.Create(ctx, &bigquery.TableMetadata{
		Schema:         bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}},
		ExpirationTime: time.Date(2000, 1, 1, 0, 0, 0, 0, time.UTC),
	}); err != nil {
		t.Fatal(err)
	}
	const readers = 32
	start := make(chan struct{})
	errors := make(chan error, readers)
	var workers sync.WaitGroup
	for range readers {
		workers.Go(func() {
			<-start
			_, err := table.Metadata(ctx)
			if apiError, ok := err.(*googleapi.Error); !ok || apiError.Code != http.StatusNotFound {
				errors <- fmt.Errorf("metadata of an expired table: %v", err)
			}
		})
	}
	close(start)
	workers.Wait()
	close(errors)
	for err := range errors {
		t.Error(err)
	}
}
