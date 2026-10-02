package goclient

import (
	"context"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// The schema a table reports is the one it was created with, which DuckDB's column types cannot
// tell: JSON and GEOGRAPHY are stored as text, REQUIRED as NOT NULL and NUMERIC(10, 2) as a
// DECIMAL, and descriptions have no column type at all.
func TestTablesKeepTheirBigQuerySchema(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_table_schema")
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
	schemaOf := func(table string) bigquery.Schema {
		t.Helper()
		metadata, err := dataset.Table(table).Metadata(ctx)
		if err != nil {
			t.Fatalf("%s: %v", table, err)
		}
		return metadata.Schema
	}

	created := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType, Required: true, Description: "the key"},
		{Name: "doc", Type: bigquery.JSONFieldType},
		{Name: "place", Type: bigquery.GeographyFieldType},
		{Name: "price", Type: bigquery.NumericFieldType, Precision: 10, Scale: 2},
		{Name: "big", Type: bigquery.BigNumericFieldType},
		{Name: "code", Type: bigquery.StringFieldType, MaxLength: 8},
		{Name: "tags", Type: bigquery.JSONFieldType, Repeated: true},
		{Name: "detail", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			{Name: "note", Type: bigquery.JSONFieldType},
		}},
	}
	if err := dataset.Table("api").Create(ctx, &bigquery.TableMetadata{Schema: created}); err != nil {
		t.Fatal(err)
	}
	if got := schemaOf("api"); !reflect.DeepEqual(got, created) {
		t.Errorf("tables.insert: got schema\n%v\nwant\n%v", fields(got), fields(created))
	}

	// The emulator does not translate GEOGRAPHY in SQL, so the DDL leaves it out.
	run(`CREATE TABLE ddl (
		id INT64 NOT NULL OPTIONS (description = 'the key'),
		doc JSON,
		price NUMERIC(10, 2),
		big BIGNUMERIC,
		code STRING(8) DEFAULT 'none',
		tags ARRAY<JSON>,
		detail STRUCT<note JSON>)`)
	want := bigquery.Schema{created[0], created[1], created[3], created[4],
		{Name: "code", Type: bigquery.StringFieldType, MaxLength: 8, DefaultValueExpression: "'none'"},
		created[6], created[7]}
	if got := schemaOf("ddl"); !reflect.DeepEqual(got, want) {
		t.Errorf("CREATE TABLE: got schema\n%v\nwant\n%v", fields(got), fields(want))
	}

	// IF NOT EXISTS keeps what is there, metadata included.
	run("CREATE TABLE IF NOT EXISTS ddl (id STRING)")
	run("ALTER TABLE ddl ADD COLUMN IF NOT EXISTS doc STRING")
	if got := schemaOf("ddl"); !reflect.DeepEqual(got, want) {
		t.Errorf("IF NOT EXISTS: got schema\n%v\nwant\n%v", fields(got), fields(want))
	}

	run("ALTER TABLE ddl ADD COLUMN added JSON")
	if got := schemaOf("ddl"); len(got) != 8 || got[7].Type != bigquery.JSONFieldType {
		t.Errorf("ALTER TABLE ADD COLUMN: got schema\n%v\nwant a JSON column last", fields(got))
	}

	run("CREATE TABLE ctas AS SELECT JSON '{\"a\": 1}' AS doc, 1 AS n")
	run("INSERT ddl (id, doc) VALUES (1, JSON '{\"a\": 1}')")
	ctas := bigquery.Schema{
		{Name: "doc", Type: bigquery.JSONFieldType},
		{Name: "n", Type: bigquery.IntegerFieldType},
	}
	if got := schemaOf("ctas"); !reflect.DeepEqual(got, ctas) {
		t.Errorf("CREATE TABLE AS SELECT: got schema\n%v\nwant\n%v", fields(got), fields(ctas))
	}

	// A JSON column is JSON in queries too, so JSON functions apply to it.
	rows, err := client.Query("SELECT JSON_VALUE(doc, '$.a') AS a, doc FROM go_table_schema.ddl").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if row[0] != "1" || rows.Schema[1].Type != bigquery.JSONFieldType {
		t.Errorf("got row %v with schema %v, want a = 1 and doc as JSON", row, fields(rows.Schema))
	}
	if err := rows.Next(&row); err != iterator.Done {
		t.Errorf("got another row %v, want one", row)
	}

	// Queries read GEOGRAPHY as the text it is stored as.
	rows, err = client.Query("SELECT place FROM go_table_schema.api").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if err := rows.Next(&row); err != iterator.Done {
		t.Errorf("got row %v, want none", row)
	}

	// Copies and query results written to a table keep the schema too.
	copier := dataset.Table("copied").CopierFrom(dataset.Table("api"))
	job, err := copier.Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if status, err := job.Wait(ctx); err != nil || status.Err() != nil {
		t.Fatalf("copy: %v %v", err, status.Err())
	}
	if got := schemaOf("copied"); !reflect.DeepEqual(got, created) {
		t.Errorf("copy: got schema\n%v\nwant\n%v", fields(got), fields(created))
	}
	query := client.Query("SELECT doc FROM go_table_schema.ddl")
	query.Dst = dataset.Table("written")
	job, err = query.Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if status, err := job.Wait(ctx); err != nil || status.Err() != nil {
		t.Fatalf("destination: %v %v", err, status.Err())
	}
	if got := schemaOf("written"); len(got) != 1 || got[0].Type != bigquery.JSONFieldType {
		t.Errorf("destination table: got schema %v, want a JSON column", fields(got))
	}
}

func fields(schema bigquery.Schema) []bigquery.FieldSchema {
	var result []bigquery.FieldSchema
	for _, field := range schema {
		result = append(result, *field)
	}
	return result
}
