package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// Column schemas declare REQUIRED mode, but queries do not propagate it:
// https://cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#column_name_and_column_schema
func TestCreateTableAsSelectColumnModes(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_ctas_column_modes")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}
	p.mustRun(`CREATE TABLE source (id INT64 NOT NULL, name STRING)`)
	p.mustRun(`INSERT source VALUES (1, 'one')`)
	p.mustRun(`CREATE TABLE inferred AS SELECT * FROM source`)
	p.mustRun(`CREATE TABLE declared (id INT64 NOT NULL OPTIONS (description = 'key'), name STRING)
	  AS SELECT * FROM source`)
	p.mustRun(`CREATE TABLE empty (id INT64 NOT NULL) AS SELECT id FROM source WHERE FALSE`)
	for _, c := range []struct {
		table  string
		schema bigquery.Schema
	}{
		{"inferred", bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}, {Name: "name", Type: bigquery.StringFieldType}}},
		{"declared", bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType, Required: true, Description: "key"}, {Name: "name", Type: bigquery.StringFieldType}}},
		{"empty", bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType, Required: true}}},
	} {
		got := p.view(c.table)
		columns, paths := informationSchemaOf(c.schema)
		if !reflect.DeepEqual(got.Schema, c.schema) || !reflect.DeepEqual(got.Columns, columns) || !reflect.DeepEqual(got.FieldPaths, paths) {
			t.Errorf("%s: schema = %+v, columns = %v, field paths = %v", c.table, got.Schema, got.Columns, got.FieldPaths)
		}
	}
	p.mustRun(`INSERT inferred VALUES (NULL, NULL)`)
	p.mustRun(`INSERT declared (id) VALUES (2)`)
	for _, sql := range []string{
		`CREATE TABLE failed (id INT64 NOT NULL) AS SELECT CAST(NULL AS INT64) AS id`,
		`INSERT declared (name) VALUES ('missing')`,
		`INSERT declared VALUES (NULL, 'null')`,
		`UPDATE declared SET id = NULL WHERE TRUE`,
		`INSERT empty VALUES (NULL)`,
	} {
		if err := p.run(sql); err == nil {
			t.Fatalf("%s unexpectedly succeeded", sql)
		}
	}
	if _, err := dataset.Table("failed").Metadata(ctx); err == nil {
		t.Fatal("failed CTAS left a table behind")
	}
	if got, want := p.read("SELECT id, name FROM declared ORDER BY id"), [][]bigquery.Value{{int64(1), "one"}, {int64(2), nil}}; !reflect.DeepEqual(got, want) {
		t.Fatalf("rows = %v, want %v", got, want)
	}
}

// BigQuery rejects ADD COLUMN with a default or top-level REQUIRED mode, even for an empty
// table. Defaults must be set after adding the column:
// https://cloud.google.com/bigquery/docs/default-values#limitations
// https://cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#alter_table_add_column_statement
func TestAlterTableAddColumnModes(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_add_column_modes")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}
	for _, table := range []string{"empty", "populated"} {
		p.mustRun("CREATE TABLE " + table + " (id INT64)")
	}
	p.mustRun("INSERT populated VALUES (1)")
	for _, table := range []string{"empty", "populated"} {
		before := p.view(table)
		for _, column := range []string{"n INT64 DEFAULT 5", "n INT64 NOT NULL", "n INT64 DEFAULT 5 NOT NULL"} {
			for _, prefix := range []string{"", "ADD COLUMN earlier INT64, "} {
				sql := "ALTER TABLE " + table + " " + prefix + "ADD COLUMN " + column
				err := p.run(sql)
				if err == nil || strings.Contains(err.Error(), "The emulator does not support") {
					t.Fatalf("%s: want an invalid statement error, got %v", sql, err)
				}
				if after := p.view(table); !reflect.DeepEqual(after, before) {
					t.Fatalf("%s changed the table: %+v", sql, after)
				}
			}
		}
	}
	p.mustRun("ALTER TABLE populated ADD COLUMN n INT64")
	p.mustRun("ALTER TABLE populated ALTER COLUMN n SET DEFAULT 5")
	p.mustRun("INSERT populated (id) VALUES (2)")
	if got, want := p.read("SELECT id, n FROM populated ORDER BY id"), [][]bigquery.Value{{int64(1), nil}, {int64(2), int64(5)}}; !reflect.DeepEqual(got, want) {
		t.Fatalf("rows = %v, want %v", got, want)
	}
}
