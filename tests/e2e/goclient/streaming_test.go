package goclient

import (
	"context"
	"errors"
	"testing"

	"cloud.google.com/go/bigquery"
)

func TestInserterPut(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_streaming")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, &bigquery.DatasetMetadata{}); err != nil {
		t.Fatalf("Dataset.Create: %v", err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	table := dataset.Table("events")
	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType, Required: true},
		{Name: "tags", Type: bigquery.StringFieldType, Repeated: true},
		{Name: "details", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			{Name: "label", Type: bigquery.StringFieldType},
		}},
	}
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatalf("Table.Create: %v", err)
	}
	type details struct {
		Label string `bigquery:"label"`
	}
	type event struct {
		ID      int64    `bigquery:"id"`
		Tags    []string `bigquery:"tags"`
		Details details  `bigquery:"details"`
	}
	if err := table.Inserter().Put(ctx, []*event{{ID: 7, Tags: []string{"a", "b"}, Details: details{Label: "ok"}}}); err != nil {
		t.Fatalf("Inserter.Put: %v", err)
	}
	rows := table.Read(ctx)
	var got struct {
		ID int64
	}
	if err := rows.Next(&got); err != nil {
		t.Fatalf("Next: %v", err)
	}
	if got.ID != 7 {
		t.Errorf("got id %d, want 7", got.ID)
	}
	queryRows, err := client.Query("SELECT id FROM go_streaming.events WHERE id = 7").Read(ctx)
	if err != nil {
		t.Fatalf("Query.Read: %v", err)
	}
	var queried struct{ ID int64 }
	if err := queryRows.Next(&queried); err != nil {
		t.Fatalf("Query.Next: %v", err)
	}
	if queried.ID != 7 {
		t.Errorf("query got id %d, want 7", queried.ID)
	}
}

func TestStreamingInsertOptions(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_streaming_options")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, &bigquery.DatasetMetadata{}); err != nil {
		t.Fatalf("Dataset.Create: %v", err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	table := dataset.Table("events")
	schema := bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType, Required: true}}
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatalf("Table.Create: %v", err)
	}
	rows := []*bigquery.ValuesSaver{
		{Schema: schema, Row: []bigquery.Value{int64(1)}},
		{Schema: schema, Row: []bigquery.Value{"invalid"}},
	}
	inserter := table.Inserter()
	var insertionErrors bigquery.PutMultiError
	if err := inserter.Put(ctx, rows); !errors.As(err, &insertionErrors) || len(insertionErrors) != 1 || insertionErrors[0].RowIndex != 1 {
		t.Fatalf("mixed batch errors: %v, want one error for row 1", err)
	}
	checkCount := func(want int64) {
		t.Helper()
		results, err := client.Query("SELECT COUNT(*) AS n FROM go_streaming_options.events").Read(ctx)
		if err != nil {
			t.Fatalf("count query: %v", err)
		}
		var row struct{ N int64 }
		if err := results.Next(&row); err != nil || row.N != want {
			t.Fatalf("row count: got %+v (%v), want %d", row, err, want)
		}
	}
	checkCount(0)

	inserter.SkipInvalidRows = true
	if err := inserter.Put(ctx, rows); !errors.As(err, &insertionErrors) || len(insertionErrors) != 1 || insertionErrors[0].RowIndex != 1 {
		t.Fatalf("skipped row errors: %v, want one error for row 1", err)
	}
	checkCount(1)

	inserter.IgnoreUnknownValues = true
	if err := inserter.Put(ctx, struct {
		ID    int64  `bigquery:"id"`
		Extra string `bigquery:"extra"`
	}{ID: 2, Extra: "ignored"}); err != nil {
		t.Fatalf("insert with unknown field: %v", err)
	}
	checkCount(2)
}
