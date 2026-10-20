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

// NOT NULL applies to fields inside records, including records in arrays. A nullable parent
// record may itself be NULL. LIKE AS SELECT inherits these modes from the source schema.
// https://cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#column_name_and_column_schema
func TestCreateTableAsSelectNestedColumnModes(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_ctas_nested_modes")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}
	columns := `(r STRUCT<x INT64 NOT NULL, s STRUCT<y STRING NOT NULL>>, a ARRAY<STRUCT<x INT64 NOT NULL>>)`
	values := `SELECT STRUCT(1 AS renamed, STRUCT('ok' AS renamed) AS renamed_s), [STRUCT(2 AS renamed)] FROM UNNEST([0])`
	p.mustRun("CREATE TABLE source " + columns)
	p.mustRun("CREATE TABLE declared " + columns + " AS " + values)
	p.mustRun("CREATE TABLE inherited LIKE source AS " + values)
	p.mustRun("CREATE TABLE empty " + columns + " AS " + values + " WHERE FALSE")
	p.mustRun("CREATE TABLE empty_like LIKE source AS " + values + " WHERE FALSE")
	if got, want := p.read("SELECT r.x, r.s.y, a[OFFSET(0)].x FROM declared"), [][]bigquery.Value{{int64(1), "ok", int64(2)}}; !reflect.DeepEqual(got, want) {
		t.Fatalf("CTAS values = %v, want %v", got, want)
	}
	for _, table := range []string{"declared", "inherited", "empty", "empty_like"} {
		t.Run(table, func(t *testing.T) {
			if got, want := p.view(table), p.view("source"); !reflect.DeepEqual(got.Schema, want.Schema) ||
				!reflect.DeepEqual(got.Columns, want.Columns) || !reflect.DeepEqual(got.FieldPaths, want.FieldPaths) {
				t.Fatalf("schema or information schema differs from source: %+v, want %+v", got, want)
			}
			// Both nullable records and empty arrays remain writable after creation.
			p.mustRun("INSERT " + table + " VALUES (NULL, []), (STRUCT(3, NULL), [])")
			p.mustRun("INSERT " + table + " " + values)
			before := p.read("SELECT TO_JSON_STRING(t) FROM " + table + " AS t ORDER BY 1")
			for _, sql := range []string{
				"INSERT " + table + " VALUES (STRUCT(NULL, NULL), [])",
				"INSERT " + table + " VALUES (STRUCT(1, STRUCT(CAST(NULL AS STRING))), [])",
				"INSERT " + table + " VALUES (NULL, [STRUCT(CAST(NULL AS INT64))])",
				"INSERT " + table + " SELECT STRUCT(CAST(NULL AS INT64), NULL), ARRAY<STRUCT<x INT64>>[]",
				"INSERT " + table + " SELECT NULL, [STRUCT(CAST(NULL AS INT64) AS renamed)]",
				"UPDATE " + table + " SET r.x = NULL WHERE r IS NOT NULL",
				"UPDATE " + table + " SET a = [STRUCT(CAST(NULL AS INT64) AS x)] WHERE TRUE",
			} {
				if err := p.run(sql); err == nil {
					t.Fatalf("%s unexpectedly succeeded", sql)
				}
			}
			if got := p.read("SELECT TO_JSON_STRING(t) FROM " + table + " AS t ORDER BY 1"); !reflect.DeepEqual(got, before) {
				t.Fatalf("failed writes changed rows: %v, want %v", got, before)
			}
		})
	}
	// Client streaming and load jobs must enforce the same schema as SQL writes.
	metadata, err := dataset.Table("inherited").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, row := range [][]bigquery.Value{
		{[]bigquery.Value{nil, nil}, []bigquery.Value{}},
		{nil, []bigquery.Value{[]bigquery.Value{nil}}},
	} {
		if err := dataset.Table("inherited").Inserter().Put(ctx, &bigquery.ValuesSaver{Schema: metadata.Schema, Row: row}); err == nil {
			t.Fatal("streaming a NULL required field succeeded")
		}
	}
	for _, data := range []string{`{"r":{"x":null},"a":[]}`, `{"r":null,"a":[{"x":null}]}`} {
		source := bigquery.NewReaderSource(strings.NewReader(data))
		source.SourceFormat = bigquery.JSON
		job, err := dataset.Table("inherited").LoaderFrom(source).Run(ctx)
		if err != nil {
			t.Fatal(err)
		}
		status, err := job.Wait(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if status.Err() == nil {
			t.Fatalf("loading %s succeeded", data)
		}
	}
	for _, clause := range []string{columns, "LIKE source"} {
		for _, values := range []string{
			`SELECT STRUCT(CAST(NULL AS INT64) AS x, CAST(NULL AS STRUCT<y STRING>) AS s), ARRAY<STRUCT<x INT64>>[]`,
			`SELECT STRUCT(1 AS x, STRUCT(CAST(NULL AS STRING) AS y) AS s), ARRAY<STRUCT<x INT64>>[]`,
			`SELECT CAST(NULL AS STRUCT<x INT64, s STRUCT<y STRING>>), [STRUCT(CAST(NULL AS INT64) AS x)]`,
		} {
			for _, target := range []string{"TABLE failed", "OR REPLACE TABLE declared"} {
				before := p.view("declared")
				rows := p.read("SELECT TO_JSON_STRING(t) FROM declared AS t ORDER BY 1")
				if err := p.run("CREATE " + target + " " + clause + " AS " + values); err == nil || !strings.Contains(err.Error(), "cannot be null") {
					t.Fatalf("%s AS %s: want REQUIRED field error, got %v", clause, values, err)
				}
				if _, err := dataset.Table("failed").Metadata(ctx); err == nil {
					t.Fatal("failed CTAS left a table behind")
				}
				if got := p.view("declared"); !reflect.DeepEqual(got, before) {
					t.Fatalf("failed replacement changed metadata: %+v, want %+v", got, before)
				}
				if got := p.read("SELECT TO_JSON_STRING(t) FROM declared AS t ORDER BY 1"); !reflect.DeepEqual(got, rows) {
					t.Fatalf("failed replacement changed rows: %v, want %v", got, rows)
				}
			}
		}
	}
}
