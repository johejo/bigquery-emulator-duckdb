// Package goclient checks the emulator from the point of view of the Go BigQuery client
// library, which drives the REST API differently from the bq command-line tool: it runs
// parameterised queries, polls jobs and asks for timestamps as epoch microseconds.
package goclient

import (
	"cloud.google.com/go/bigquery"
	"context"
	"testing"
	"time"
)

func TestDatasetTableAndRows(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	created := time.Date(2024, 3, 4, 5, 6, 7, 0, time.UTC)

	dataset := client.Dataset("go_e2e")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, &bigquery.DatasetMetadata{}); err != nil {
		t.Fatalf("Dataset.Create: %v", err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	table := dataset.Table("users")
	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "name", Type: bigquery.StringFieldType},
		{Name: "created", Type: bigquery.TimestampFieldType},
	}
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatalf("Table.Create: %v", err)
	}
	metadata, err := table.Metadata(ctx)
	if err != nil {
		t.Fatalf("Table.Metadata: %v", err)
	}
	if len(metadata.Schema) != 3 || metadata.Schema[2].Type != bigquery.TimestampFieldType {
		t.Errorf("got schema %v, want the three columns the table was created with",
			metadata.Schema)
	}

	// A DML statement runs as a job, which the client polls until it is done.
	insert := client.Query("INSERT INTO go_e2e.users VALUES (@id, @name, @created)")
	insert.Parameters = []bigquery.QueryParameter{
		{Name: "id", Value: int64(1)},
		{Name: "name", Value: "alice"},
		{Name: "created", Value: created},
	}
	job, err := insert.Run(ctx)
	if err != nil {
		t.Fatalf("Run: %v", err)
	}
	status, err := job.Wait(ctx)
	if err != nil {
		t.Fatalf("Wait: %v", err)
	}
	if err := status.Err(); err != nil {
		t.Fatalf("insert job: %v", err)
	}

	var row struct {
		ID      int64
		Name    string
		Created time.Time
	}
	rows := table.Read(ctx)
	if err := rows.Next(&row); err != nil {
		t.Fatalf("Next: %v", err)
	}
	if row.ID != 1 || row.Name != "alice" || !row.Created.Equal(created) {
		t.Errorf("got row %+v, want {1 alice %s}", row, created)
	}
}
