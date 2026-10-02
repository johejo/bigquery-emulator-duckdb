package goclient

import (
	"context"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
)

// BigQuery stores a missing ARRAY as an empty one, so a row that leaves out a REPEATED column
// reads it back as [], however the table was created.
func TestOmittedRepeatedColumnsAreEmpty(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_repeated_default")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) {
		t.Helper()
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		job, err := query.Run(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		status, err := job.Wait(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		if err := status.Err(); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	lengths := func(sql string) [][]bigquery.Value {
		t.Helper()
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		var result [][]bigquery.Value
		for {
			var row []bigquery.Value
			if err := rows.Next(&row); err != nil {
				break
			}
			result = append(result, row)
		}
		return result
	}

	if err := dataset.Table("api").Create(ctx, &bigquery.TableMetadata{Schema: bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "tags", Type: bigquery.StringFieldType, Repeated: true},
		{Name: "items", Type: bigquery.RecordFieldType, Repeated: true, Schema: bigquery.Schema{
			{Name: "sku", Type: bigquery.StringFieldType},
		}},
	}}); err != nil {
		t.Fatal(err)
	}
	run("CREATE TABLE ddl (id INT64, tags ARRAY<STRING>, items ARRAY<STRUCT<sku STRING>>)")
	run("CREATE TABLE ctas AS SELECT 0 AS id, ['a'] AS tags, [STRUCT('b' AS sku)] AS items")
	destination := client.Query("SELECT 0 AS id, ['a'] AS tags, [STRUCT('b' AS sku)] AS items")
	destination.Dst = dataset.Table("destination")
	job, err := destination.Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if status, err := job.Wait(ctx); err != nil || status.Err() != nil {
		t.Fatalf("destination: %v %v", err, status.Err())
	}
	for _, table := range []string{"api", "ddl", "ctas", "destination"} {
		run("INSERT " + table + " (id) VALUES (1)")
		got := lengths("SELECT ARRAY_LENGTH(tags), ARRAY_LENGTH(items) FROM " + table + " WHERE id = 1")
		if want := [][]bigquery.Value{{int64(0), int64(0)}}; !reflect.DeepEqual(got, want) {
			t.Errorf("%s: got array lengths %v, want %v", table, got, want)
		}
	}

	// Rows that exist when a REPEATED column is added read it as empty too.
	run("ALTER TABLE ddl ADD COLUMN added ARRAY<INT64>")
	run("INSERT ddl (id) VALUES (2)")
	got := lengths("SELECT id, ARRAY_LENGTH(added) FROM ddl ORDER BY id")
	if want := [][]bigquery.Value{{int64(1), int64(0)}, {int64(2), int64(0)}}; !reflect.DeepEqual(got, want) {
		t.Errorf("ADD COLUMN: got %v, want %v", got, want)
	}

	// A declared default wins.
	run("CREATE TABLE defaulted (id INT64, tags ARRAY<STRING> DEFAULT ['x'])")
	run("INSERT defaulted (id) VALUES (1)")
	got = lengths("SELECT ARRAY_LENGTH(tags) FROM defaulted")
	if want := [][]bigquery.Value{{int64(1)}}; !reflect.DeepEqual(got, want) {
		t.Errorf("declared default: got %v, want %v", got, want)
	}
}
