package goclient

import (
	"context"
	"reflect"
	"strings"
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
		_, _, err := runScript(t, client, dataset.DatasetID, sql)
		return err
	}
	for _, sql := range []string{
		"CREATE TABLE users (id INT64)",
		"INSERT users VALUES (1)",
		"ALTER TABLE users ADD COLUMN name STRING",
		"ALTER TABLE users ADD COLUMN IF NOT EXISTS name STRING",
		"ALTER TABLE IF EXISTS missing ADD COLUMN x INT64",
		"ALTER TABLE users ADD COLUMN tags ARRAY<STRING>",
		"ALTER TABLE users ADD COLUMN details STRUCT<score INT64>",
		"ALTER TABLE users ADD COLUMN IF NOT EXISTS name STRING, ADD COLUMN score FLOAT64",
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
		{Name: "score", Type: bigquery.FloatFieldType},
	}
	if !reflect.DeepEqual(metadata.Schema, wantSchema) {
		t.Fatalf("schema = %#v, want %#v", metadata.Schema, wantSchema)
	}
	var row []bigquery.Value
	if err := table.Read(ctx).Next(&row); err != nil {
		t.Fatal(err)
	}
	if len(row) != 5 || row[0] != int64(1) || row[1] != nil || row[3] != nil || row[4] != nil {
		t.Fatalf("existing row = %#v; added nullable columns must be NULL", row)
	}
	if tags, ok := row[2].([]bigquery.Value); !ok || len(tags) != 0 {
		t.Fatalf("added repeated column = %#v, want empty array", row[2])
	}
	for _, sql := range []string{
		"ALTER TABLE users ADD COLUMN name INT64",
		"ALTER TABLE users ADD COLUMN partial INT64, ADD COLUMN name INT64",
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

func TestAlterTableDropColumnAndRename(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_alter_drop")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		_, _, err := runScript(t, client, dataset.DatasetID, sql)
		return err
	}
	for _, sql := range []string{
		`CREATE TABLE events (id INT64, day DATE, name STRING, note STRING, extra STRING)
		 PARTITION BY day CLUSTER BY name OPTIONS (description = 'events')`,
		"INSERT events (id, day, name, note, extra) VALUES (1, DATE '2024-01-01', 'a', 'n', 'x')",
		"ALTER TABLE events DROP COLUMN note, DROP COLUMN IF EXISTS missing, DROP COLUMN IF EXISTS EXTRA",
		"ALTER TABLE IF EXISTS missing DROP COLUMN id",
		"ALTER TABLE events RENAME TO renamed",
		"ALTER TABLE IF EXISTS missing RENAME TO other",
		"CREATE TABLE taken (id INT64)",
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	if _, err := dataset.Table("events").Metadata(ctx); err == nil {
		t.Fatal("the renamed table is still there under its old name")
	}
	wantSchema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "day", Type: bigquery.DateFieldType},
		{Name: "name", Type: bigquery.StringFieldType},
	}
	check := func() {
		t.Helper()
		metadata, err := dataset.Table("renamed").Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(metadata.Schema, wantSchema) {
			t.Fatalf("schema = %#v, want %#v", metadata.Schema, wantSchema)
		}
		if metadata.Description != "events" ||
			!reflect.DeepEqual(metadata.TimePartitioning, &bigquery.TimePartitioning{Type: bigquery.DayPartitioningType, Field: "day"}) ||
			!reflect.DeepEqual(metadata.Clustering, &bigquery.Clustering{Fields: []string{"name"}}) {
			t.Fatalf("description = %q, partitioning = %+v, clustering = %+v", metadata.Description,
				metadata.TimePartitioning, metadata.Clustering)
		}
	}
	check()
	rows, err := client.Query("SELECT id, name FROM go_alter_drop.renamed").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil || !reflect.DeepEqual(row, []bigquery.Value{int64(1), "a"}) {
		t.Fatalf("row = %#v, error = %v", row, err)
	}

	for _, sql := range []string{
		"ALTER TABLE renamed DROP COLUMN day",
		"ALTER TABLE renamed DROP COLUMN name",
		"ALTER TABLE renamed DROP COLUMN id, DROP COLUMN name",
		"ALTER TABLE renamed DROP COLUMN missing",
		"ALTER TABLE renamed RENAME TO taken",
		"ALTER TABLE renamed RENAME TO go_alter_drop.other",
		"ALTER TABLE missing DROP COLUMN id",
	} {
		if err := run(sql); err == nil {
			t.Fatalf("%s unexpectedly succeeded", sql)
		}
	}
	check()
}

func TestAlterTableSetOptions(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_alter_options")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		_, _, err := runScript(t, client, dataset.DatasetID, sql)
		return err
	}
	type metadata struct {
		Description, Name string
		Labels            map[string]string
	}
	check := func(want metadata) {
		t.Helper()
		got, err := dataset.Table("t").Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if g := (metadata{got.Description, got.Name, got.Labels}); !reflect.DeepEqual(g, want) {
			t.Fatalf("got %+v, want %+v", g, want)
		}
		if !reflect.DeepEqual(got.TimePartitioning, &bigquery.TimePartitioning{Type: bigquery.DayPartitioningType, Field: "day"}) {
			t.Fatalf("partitioning = %+v", got.TimePartitioning)
		}
	}
	if err := run(`CREATE TABLE t (id INT64, day DATE) PARTITION BY day
		OPTIONS (description = 'old', friendly_name = 'Old', labels = [('env', 'dev'), ('team', 'data')])`); err != nil {
		t.Fatal(err)
	}
	// Each option set replaces the table's value, labels included, and leaves the others.
	if err := run("ALTER TABLE t SET OPTIONS (description = 'new', labels = [('env', 'prod')])"); err != nil {
		t.Fatal(err)
	}
	check(metadata{"new", "Old", map[string]string{"env": "prod"}})
	// NULL clears an option.
	if err := run("ALTER TABLE t SET OPTIONS (description = NULL, friendly_name = NULL)"); err != nil {
		t.Fatal(err)
	}
	check(metadata{"", "", map[string]string{"env": "prod"}})
	if err := run("ALTER TABLE IF EXISTS missing SET OPTIONS (description = 'x')"); err != nil {
		t.Fatal(err)
	}

	for sql, want := range map[string]string{
		"ALTER TABLE t SET OPTIONS (expiration_timestamp = TIMESTAMP '2030-01-01 00:00:00 UTC')": "ALTER TABLE option expiration_timestamp",
		"ALTER TABLE t SET OPTIONS (require_partition_filter = TRUE)":                            "ALTER TABLE option require_partition_filter",
		"ALTER TABLE t SET OPTIONS (labels = [('Env', 'prod')])":                                 "must start with a lowercase letter",
	} {
		if err := run(sql); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", sql, err, want)
		}
	}
	check(metadata{"", "", map[string]string{"env": "prod"}})
}

func TestAlterTableAlterColumnSetOptions(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_alter_column_options")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		_, _, err := runScript(t, client, dataset.DatasetID, sql)
		return err
	}
	check := func(want bigquery.Schema) {
		t.Helper()
		got, err := dataset.Table("t").Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(got.Schema, want) {
			t.Fatalf("schema = %#v, want %#v", got.Schema, want)
		}
	}
	for _, sql := range []string{
		`CREATE TABLE t (id INT64 NOT NULL OPTIONS (description = 'old'), name STRING,
		 s STRUCT<a INT64 OPTIONS (description = 'field')>)`,
		"INSERT t (id, name) VALUES (1, 'a')",
		"ALTER TABLE t ALTER COLUMN name SET OPTIONS (description = 'name'), ALTER COLUMN S SET OPTIONS (description = 'record')",
		"ALTER TABLE t ALTER COLUMN id SET OPTIONS (description = NULL)",
		"ALTER TABLE t ALTER COLUMN IF EXISTS missing SET OPTIONS (description = 'x')",
		"ALTER TABLE IF EXISTS missing ALTER COLUMN id SET OPTIONS (description = 'x')",
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	want := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType, Required: true},
		{Name: "name", Type: bigquery.StringFieldType, Description: "name"},
		{Name: "s", Type: bigquery.RecordFieldType, Description: "record", Schema: bigquery.Schema{
			{Name: "a", Type: bigquery.IntegerFieldType, Description: "field"},
		}},
	}
	check(want)

	for sql, want := range map[string]string{
		"ALTER TABLE t ALTER COLUMN id SET OPTIONS (rounding_mode = 'ROUND_HALF_EVEN')":                                         "column option rounding_mode",
		"ALTER TABLE t ALTER COLUMN name SET OPTIONS (description = 'x'), DROP COLUMN id":                                       "different kinds of actions",
		"ALTER TABLE t ALTER COLUMN name SET OPTIONS (description = 'x'), ALTER COLUMN missing SET OPTIONS (description = 'x')": "missing",
		"ALTER TABLE missing ALTER COLUMN id SET OPTIONS (description = 'x')":                                                   "missing",
	} {
		if err := run(sql); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", sql, err, want)
		}
	}
	check(want)
	rows, err := client.Query("SELECT id, name FROM go_alter_column_options.t").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil || !reflect.DeepEqual(row, []bigquery.Value{int64(1), "a"}) {
		t.Fatalf("row = %#v, error = %v", row, err)
	}
}
