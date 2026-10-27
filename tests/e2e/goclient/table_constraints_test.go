package goclient

import (
	"context"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
)

// Primary keys as BigQuery's DDL reference and the Table resource's tableConstraints describe them:
// third_party/bigquery_docs/reference/standard-sql/data-definition-language.md
// third_party/bigquery_docs/reference/rest/v2/tables.md
func TestPrimaryKeys(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := newDataset(t, client, "go_primary_keys")
	run := func(sql string) error {
		_, _, err := runScript(t, client, dataset.DatasetID, sql)
		return err
	}
	primaryKey := func(table string) []string {
		t.Helper()
		metadata, err := dataset.Table(table).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if metadata.TableConstraints == nil || metadata.TableConstraints.PrimaryKey == nil {
			return nil
		}
		return metadata.TableConstraints.PrimaryKey.Columns
	}
	for _, sql := range []string{
		"CREATE TABLE pk_table (x INT64, y INT64, PRIMARY KEY (x, y) NOT ENFORCED)",
		"CREATE TABLE column_key (id STRING PRIMARY KEY NOT ENFORCED, v INT64)",
		"CREATE TABLE fk_table (x INT64, y INT64, i INT64, j INT64, u INT64, v INT64)",
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	if got, want := primaryKey("pk_table"), []string{"x", "y"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("pk_table primary key = %v, want %v", got, want)
	}
	if got, want := primaryKey("column_key"), []string{"id"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("column_key primary key = %v, want %v", got, want)
	}
	if got := primaryKey("fk_table"); got != nil {
		t.Fatalf("fk_table primary key = %v, want none", got)
	}

	// The documentation's ALTER TABLE examples; DROP PRIMARY KEY IF EXISTS has no effect on a table
	// without one.
	for _, sql := range []string{
		"ALTER TABLE pk_table DROP PRIMARY KEY",
		"ALTER TABLE pk_table DROP PRIMARY KEY IF EXISTS",
	} {
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	if got := primaryKey("pk_table"); got != nil {
		t.Fatalf("primary key after DROP PRIMARY KEY = %v, want none", got)
	}
	if err := run("ALTER TABLE pk_table ADD PRIMARY KEY (x,y) NOT ENFORCED"); err != nil {
		t.Fatal(err)
	}
	if got, want := primaryKey("pk_table"), []string{"x", "y"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("primary key after ADD PRIMARY KEY = %v, want %v", got, want)
	}

	// Primary key columns can be neither dropped nor given a default value.
	for _, sql := range []string{
		"ALTER TABLE pk_table DROP COLUMN x",
		"ALTER TABLE pk_table ALTER COLUMN y SET DEFAULT 1",
	} {
		if err := run(sql); err == nil {
			t.Errorf("%s succeeded, want an error", sql)
		}
	}

	table := dataset.Table("rest_key")
	if err := table.Create(ctx, &bigquery.TableMetadata{
		Schema: bigquery.Schema{
			{Name: "a", Type: bigquery.IntegerFieldType},
			{Name: "b", Type: bigquery.StringFieldType},
		},
		TableConstraints: &bigquery.TableConstraints{
			PrimaryKey: &bigquery.PrimaryKey{Columns: []string{"b", "a"}},
		},
	}); err != nil {
		t.Fatal(err)
	}
	if got, want := primaryKey("rest_key"), []string{"b", "a"}; !reflect.DeepEqual(got, want) {
		t.Fatalf("rest_key primary key = %v, want %v", got, want)
	}
}
