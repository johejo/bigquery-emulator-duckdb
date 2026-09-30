package goclient

import (
	"context"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

func TestAlterTableAddColumn(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_alter")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		job, err := query.Run(ctx)
		if err != nil {
			return err
		}
		status, err := job.Wait(ctx)
		if err != nil {
			return err
		}
		return status.Err()
	}
	for _, sql := range []string{
		"CREATE TABLE users (id INT64)",
		"INSERT users VALUES (1)",
		"ALTER TABLE users ADD COLUMN name STRING",
		"ALTER TABLE users ADD COLUMN IF NOT EXISTS name STRING",
		"ALTER TABLE IF EXISTS missing ADD COLUMN x INT64",
		"ALTER TABLE users ADD COLUMN tags ARRAY<STRING>",
		"ALTER TABLE users ADD COLUMN details STRUCT<score INT64>",
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	table := dataset.Table("users")
	metadata, err := table.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	wantSchema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "name", Type: bigquery.StringFieldType},
		{Name: "tags", Type: bigquery.StringFieldType, Repeated: true},
		{Name: "details", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			{Name: "score", Type: bigquery.IntegerFieldType},
		}},
	}
	if !reflect.DeepEqual(metadata.Schema, wantSchema) {
		t.Fatalf("schema = %#v, want %#v", metadata.Schema, wantSchema)
	}
	var row []bigquery.Value
	if err := table.Read(ctx).Next(&row); err != nil {
		t.Fatal(err)
	}
	if len(row) != 4 || row[0] != int64(1) || row[1] != nil || row[3] != nil {
		t.Fatalf("existing row = %#v; added nullable columns must be NULL", row)
	}
	if tags, ok := row[2].([]bigquery.Value); !ok || len(tags) != 0 {
		t.Fatalf("added repeated column = %#v, want empty array", row[2])
	}
	for _, sql := range []string{
		"ALTER TABLE users ADD COLUMN name INT64",
		"ALTER TABLE users ADD COLUMN partial INT64, ADD COLUMN other STRING",
		"ALTER TABLE users ADD COLUMN partial INT64, DROP COLUMN name",
		"ALTER TABLE users ADD COLUMN defaulted INT64 DEFAULT 5",
		"ALTER TABLE users ADD COLUMN required INT64 NOT NULL",
		"ALTER TABLE missing ADD COLUMN x INT64",
	} {
		if err := run(sql); err == nil {
			t.Fatalf("%s unexpectedly succeeded", sql)
		}
	}
	dryRun := client.Query("ALTER TABLE go_alter.users ADD COLUMN dry_run INT64")
	dryRun.DryRun = true
	if _, err := dryRun.Run(ctx); err != nil {
		t.Fatal(err)
	}
	metadata, err = table.Metadata(ctx)
	if err != nil || !reflect.DeepEqual(metadata.Schema, wantSchema) {
		t.Fatalf("failed ALTER or dry run changed schema: metadata=%+v, error=%v", metadata, err)
	}
	for _, sql := range []string{
		"UPDATE users SET name = 'alice', tags = ['a'], details = STRUCT(7 AS score) WHERE id = 1",
		"INSERT users (id, name) VALUES (2, 'bob')",
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	rows, err := client.Query("SELECT id, name, tags[SAFE_OFFSET(0)], details.score FROM go_alter.users ORDER BY id").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range [][]bigquery.Value{{int64(1), "alice", "a", int64(7)}, {int64(2), "bob", nil, nil}} {
		row = nil
		if err := rows.Next(&row); err != nil || !reflect.DeepEqual(row, want) {
			t.Fatalf("row = %#v, want %#v; error = %v", row, want, err)
		}
	}
	if err := rows.Next(&row); err != iterator.Done {
		t.Fatalf("end of rows: %v", err)
	}
}
