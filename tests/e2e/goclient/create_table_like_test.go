package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// CREATE TABLE LIKE copies only the metadata of the source table: its schema, partitioning,
// clustering and options, which the statement's own clauses replace.
func TestCreateTableLike(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_create_table_like")
	_ = dataset.DeleteWithContents(ctx)
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
		`CREATE TABLE source (
		   id INT64 NOT NULL OPTIONS (description = 'key'),
		   day DATE,
		   amount NUMERIC(10, 2),
		   tags ARRAY<STRING>,
		   point STRUCT<x INT64 OPTIONS (description = 'across')>)
		 PARTITION BY day CLUSTER BY id
		 OPTIONS (description = 'the source', friendly_name = 'Source', labels = [('env', 'dev')])`,
		`INSERT source (id, day) VALUES (1, DATE '2024-01-02')`,
		`CREATE TABLE copy LIKE source`,
		`CREATE TABLE custom LIKE source PARTITION BY DATE_TRUNC(day, MONTH) CLUSTER BY day
		 OPTIONS (description = 'custom')`,
		// The new table has no relationship to the source table after creation.
		`ALTER TABLE source ADD COLUMN later STRING`,
		`CREATE VIEW v AS SELECT id FROM source`,
		`CREATE TABLE defaulted (a INT64 DEFAULT 1)`,
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	source, err := dataset.Table("source").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}

	copied, err := dataset.Table("copy").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if got, want := copied.Schema, source.Schema[:len(source.Schema)-1]; !reflect.DeepEqual(got, want) {
		t.Errorf("schema = %+v, want %+v", got, want)
	}
	if copied.Description != "the source" || copied.Name != "Source" ||
		!reflect.DeepEqual(copied.Labels, map[string]string{"env": "dev"}) {
		t.Errorf("description, name, labels = %q, %q, %v", copied.Description, copied.Name, copied.Labels)
	}
	if got, want := copied.TimePartitioning, (&bigquery.TimePartitioning{Type: bigquery.DayPartitioningType, Field: "day"}); !reflect.DeepEqual(got, want) {
		t.Errorf("time partitioning = %+v, want %+v", got, want)
	}
	if got, want := copied.Clustering, (&bigquery.Clustering{Fields: []string{"id"}}); !reflect.DeepEqual(got, want) {
		t.Errorf("clustering = %+v, want %+v", got, want)
	}
	rows := dataset.Table("copy").Read(ctx)
	var row []bigquery.Value
	if err := rows.Next(&row); err != iterator.Done {
		t.Errorf("copy has a row %v, err %v", row, err)
	}
	// NOT NULL is copied with the schema.
	if err := run("INSERT copy (day) VALUES (DATE '2024-01-02')"); err == nil {
		t.Error("inserting NULL into a REQUIRED column of the copy succeeded")
	}

	custom, err := dataset.Table("custom").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if custom.Description != "custom" || custom.Name != "Source" ||
		!reflect.DeepEqual(custom.Labels, map[string]string{"env": "dev"}) {
		t.Errorf("description, name, labels = %q, %q, %v", custom.Description, custom.Name, custom.Labels)
	}
	if got, want := custom.TimePartitioning, (&bigquery.TimePartitioning{Type: bigquery.MonthPartitioningType, Field: "day"}); !reflect.DeepEqual(got, want) {
		t.Errorf("time partitioning = %+v, want %+v", got, want)
	}
	if got, want := custom.Clustering, (&bigquery.Clustering{Fields: []string{"day"}}); !reflect.DeepEqual(got, want) {
		t.Errorf("clustering = %+v, want %+v", got, want)
	}

	for sql, want := range map[string]string{
		"CREATE TABLE n LIKE v":                          "CREATE TABLE LIKE a view",
		"CREATE TABLE n LIKE defaulted":                  "CREATE TABLE LIKE a table with column defaults",
		"CREATE TABLE n LIKE copy AS SELECT * FROM copy": "CREATE TABLE LIKE AS SELECT",
	} {
		if err := run(sql); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", sql, err, want)
		}
	}
}
