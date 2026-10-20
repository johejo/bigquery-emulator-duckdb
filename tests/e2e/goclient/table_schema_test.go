package goclient

import (
	"context"
	"reflect"
	"strings"
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

	run(`CREATE TABLE ddl (
		id INT64 NOT NULL OPTIONS (description = 'the key'),
		doc JSON,
		place GEOGRAPHY,
		price NUMERIC(10, 2),
		big BIGNUMERIC,
		code STRING(8) DEFAULT 'none',
		tags ARRAY<JSON>,
		detail STRUCT<note JSON>)`)
	want := bigquery.Schema{created[0], created[1], created[2], created[3], created[4],
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
	if got := schemaOf("ddl"); len(got) != 9 || got[8].Type != bigquery.JSONFieldType {
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

	// A GEOGRAPHY column is GEOGRAPHY in queries too.
	rows, err = client.Query("SELECT place FROM go_table_schema.api").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if err := rows.Next(&row); err != iterator.Done {
		t.Errorf("got row %v, want none", row)
	}
	if len(rows.Schema) != 1 || rows.Schema[0].Type != bigquery.GeographyFieldType {
		t.Errorf("got schema %v, want a GEOGRAPHY column", fields(rows.Schema))
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

// A table created through the API rounds values to the scale its schema gives and rejects those
// beyond its precision, as one created by DDL does, nested fields included. The values are
// BigQuery's documented examples for NUMERIC(5, 2).
func TestTablesInsertAppliesPrecisionAndScale(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_table_precision")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		t.Helper()
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

	schema := bigquery.Schema{
		{Name: "price", Type: bigquery.NumericFieldType, Precision: 5, Scale: 2},
		{Name: "items", Type: bigquery.RecordFieldType, Repeated: true, Schema: bigquery.Schema{
			{Name: "price", Type: bigquery.NumericFieldType, Precision: 5, Scale: 2},
		}},
	}
	if err := dataset.Table("t").Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatal(err)
	}
	if err := run("CREATE TABLE ddl (price NUMERIC(5, 2), items ARRAY<STRUCT<price NUMERIC(5, 2)>>)"); err != nil {
		t.Fatal(err)
	}
	for _, table := range []string{"t", "ddl"} {
		if err := run("INSERT " + table + " VALUES (1.125, [STRUCT(NUMERIC '1.125')])"); err != nil {
			t.Fatal(err)
		}
		query := client.Query("SELECT CAST(price AS STRING), CAST(items[0].price AS STRING) FROM " + table)
		query.DefaultDatasetID = dataset.DatasetID
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		var row []bigquery.Value
		if err := rows.Next(&row); err != nil {
			t.Fatal(err)
		}
		if row[0] != "1.13" || row[1] != "1.13" {
			t.Errorf("%s: got %v, want 1.125 rounded to 1.13", table, row)
		}
		for _, sql := range []string{"INSERT " + table + " (price) VALUES (1111)", "INSERT " + table + " (items) VALUES ([STRUCT(NUMERIC '1111')])"} {
			if err := run(sql); err == nil {
				t.Errorf("%s: got no error, want 1111 out of range of NUMERIC(5, 2)", sql)
			}
		}
	}

	// Parameters outside their range are invalid.
	for want, field := range map[string]*bigquery.FieldSchema{
		"In BIGNUMERIC(P, 2), P must be between 2 and 40": {Name: "n", Type: bigquery.BigNumericFieldType, Precision: 41, Scale: 2},
		"In NUMERIC(P, 2), P must be between 2 and 31":    {Name: "n", Type: bigquery.NumericFieldType, Precision: 40, Scale: 2},
	} {
		err := dataset.Table("invalid").Create(ctx, &bigquery.TableMetadata{Schema: bigquery.Schema{field}})
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("got %v, want %q", err, want)
		}
	}
}

// tables.insert and tables.patch reject field names that differ only in case, at any depth, as
// BigQuery does; its error names the field by its path from the top of the schema.
// https://cloud.google.com/bigquery/docs/schemas#column_names
func TestTableSchemasRejectFieldNamesDifferingInCase(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_table_schema_case")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	a := &bigquery.FieldSchema{Name: "a", Type: bigquery.StringFieldType}
	upperA := &bigquery.FieldSchema{Name: "A", Type: bigquery.IntegerFieldType}
	record := func(name string, repeated bool, children ...*bigquery.FieldSchema) *bigquery.FieldSchema {
		return &bigquery.FieldSchema{Name: name, Type: bigquery.RecordFieldType, Repeated: repeated, Schema: children}
	}
	check := func(operation string, err error, want string) {
		t.Helper()
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: got %v, want %q", operation, err, want)
		}
	}

	for _, c := range []struct {
		schema bigquery.Schema
		want   string
	}{
		{bigquery.Schema{a, upperA}, "Field A already exists in schema"},
		{bigquery.Schema{record("r", false, a, upperA)}, "Field r.A already exists in schema"},
		{bigquery.Schema{record("r", true, record("x", false, a, upperA))}, "Field r.x.A already exists in schema"},
	} {
		err := dataset.Table("invalid").Create(ctx, &bigquery.TableMetadata{Schema: c.schema})
		check("tables.insert", err, c.want)
	}

	table := dataset.Table("patched")
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: bigquery.Schema{a, record("r", false, a)}}); err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		schema bigquery.Schema
		want   string
	}{
		{bigquery.Schema{a, record("r", false, a), upperA}, "Field A already exists in schema"},
		{bigquery.Schema{a, record("r", false, a, upperA)}, "Field r.A already exists in schema"},
	} {
		_, err := table.Update(ctx, bigquery.TableMetadataToUpdate{Schema: c.schema}, "")
		check("tables.patch", err, c.want)
	}
}

func fields(schema bigquery.Schema) []bigquery.FieldSchema {
	var result []bigquery.FieldSchema
	for _, field := range schema {
		result = append(result, *field)
	}
	return result
}
