package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// STRING(L) limits Unicode characters; BYTES(L) limits bytes. Parameters apply to writes,
// including fields in STRUCTs and ARRAYs, and remain in the table schema.
// https://cloud.google.com/bigquery/docs/reference/standard-sql/data-types#parameterized_data_types
func TestColumnLengthWrites(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_column_length_writes")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{datasetQueries{t: t, client: client, dataset: dataset}}
	columns := `(s STRING(3) NOT NULL, b BYTES(3), r STRUCT<label STRING(3)>, a ARRAY<STRING(3)>)`
	values := `SELECT '日本語' AS s, b'abc' AS b, STRUCT('abc' AS renamed) AS r, ['abc', ''] AS a FROM UNNEST([0])`
	p.mustRun("CREATE TABLE defaulted (s STRING(3) DEFAULT 'abc')")
	p.mustRun("INSERT defaulted VALUES (DEFAULT)")
	if got, want := p.read("SELECT s FROM defaulted"), [][]bigquery.Value{{"abc"}}; !reflect.DeepEqual(got, want) {
		t.Fatalf("default = %v, want %v", got, want)
	}
	if err := dataset.Table("invalid_default").Create(ctx, &bigquery.TableMetadata{Schema: bigquery.Schema{
		{Name: "s", Type: bigquery.StringFieldType, MaxLength: 3, DefaultValueExpression: "'abcd'"},
	}}); err == nil || !strings.Contains(err.Error(), "maximum length 3") {
		t.Fatalf("tables.insert with an overlong default: %v", err)
	}
	p.mustRun("CREATE TABLE source " + columns)
	p.mustRun("CREATE TABLE declared " + columns + " AS " + values)
	p.mustRun("CREATE TABLE inherited LIKE source AS " + values)
	p.mustRun("CREATE TABLE empty LIKE source AS " + values + " WHERE FALSE")
	if got, want := p.read("SELECT s, TO_HEX(b), r.label, ARRAY_LENGTH(a) FROM declared"), [][]bigquery.Value{{"日本語", "616263", "abc", int64(2)}}; !reflect.DeepEqual(got, want) {
		t.Fatalf("CTAS values = %v, want %v", got, want)
	}
	invalid := []string{
		`SELECT 'abcd' AS s, b'abc' AS b, STRUCT('abc' AS label) AS r, ['abc'] AS a`,
		`SELECT 'abc' AS s, b'abcd' AS b, STRUCT('abc' AS label) AS r, ['abc'] AS a`,
		`SELECT 'abc' AS s, b'abc' AS b, STRUCT('abcd' AS label) AS r, ['abc'] AS a`,
		`SELECT 'abc' AS s, b'abc' AS b, STRUCT('abc' AS label) AS r, ['abcd'] AS a`,
	}
	for _, table := range []string{"declared", "inherited", "empty"} {
		got, want := p.view(table), p.view("source")
		if !reflect.DeepEqual(got.Schema, want.Schema) || !reflect.DeepEqual(got.Columns, want.Columns) || !reflect.DeepEqual(got.FieldPaths, want.FieldPaths) {
			t.Fatalf("%s: schema differs from source: %+v, want %+v", table, got, want)
		}
		if table == "empty" && got.NumRows != 0 {
			t.Fatalf("empty SELECT produced %d rows", got.NumRows)
		}
		p.mustRun("INSERT " + table + " VALUES ('', b'', NULL, []), ('abc', NULL, STRUCT(NULL), NULL)")
		p.mustRun("INSERT " + table + " " + values)
		before := p.read("SELECT TO_JSON_STRING(t) FROM " + table + " AS t ORDER BY 1")
		for _, query := range invalid {
			if err := p.run("INSERT " + table + " " + query); err == nil || !strings.Contains(err.Error(), "maximum length 3") {
				t.Fatalf("%s: invalid INSERT SELECT error = %v", table, err)
			}
		}
		for _, sql := range []string{
			"INSERT " + table + " VALUES ('abcd', b'', NULL, [])",
			"INSERT " + table + " VALUES ('abc', b'abcd', NULL, [])",
			"UPDATE " + table + " SET r.label = 'abcd' WHERE r IS NOT NULL",
			"UPDATE " + table + " SET a = ['abcd'] WHERE TRUE",
			"UPDATE " + table + " SET s = 'abcd' WHERE TRUE",
			"UPDATE " + table + " SET r = STRUCT('abcd' AS label) WHERE TRUE",
			"MERGE " + table + " AS t USING (SELECT 1 AS id) AS q ON TRUE WHEN MATCHED THEN UPDATE SET s = 'abcd'",
			"MERGE " + table + " AS t USING (SELECT 1 AS id) AS q ON FALSE WHEN NOT MATCHED THEN INSERT (s) VALUES ('abcd')",
		} {
			err := p.run(sql)
			if err == nil || !strings.Contains(err.Error(), "maximum length 3") {
				t.Fatalf("%s: error = %v", sql, err)
			}
			if strings.HasPrefix(sql, "UPDATE ") || strings.Contains(sql, "WHEN MATCHED") {
				if strings.Contains(err.Error(), "Field ") {
					t.Fatalf("%s: assignment error must not name a column: %v", sql, err)
				}
			}
		}
		if err := p.run("INSERT " + table + " VALUES (NULL, NULL, NULL, [])"); err == nil {
			t.Fatal("adding length validation stopped enforcing REQUIRED")
		}
		if got := p.read("SELECT TO_JSON_STRING(t) FROM " + table + " AS t ORDER BY 1"); !reflect.DeepEqual(got, before) {
			t.Fatalf("failed writes changed rows: %v, want %v", got, before)
		}
	}
	for _, clause := range []string{columns, "LIKE source"} {
		for _, query := range invalid {
			before := p.view("declared")
			rows := p.read("SELECT TO_JSON_STRING(t) FROM declared AS t ORDER BY 1")
			for _, target := range []string{"TABLE failed", "OR REPLACE TABLE declared"} {
				if err := p.run("CREATE " + target + " " + clause + " AS " + query); err == nil || !strings.Contains(err.Error(), "maximum length 3") {
					t.Fatalf("%s AS %s: error = %v", clause, query, err)
				}
			}
			if _, err := dataset.Table("failed").Metadata(ctx); err == nil {
				t.Fatal("failed CTAS left a table behind")
			}
			if got := p.view("declared"); !reflect.DeepEqual(got, before) {
				t.Fatal("failed replacement changed metadata")
			}
			if got := p.read("SELECT TO_JSON_STRING(t) FROM declared AS t ORDER BY 1"); !reflect.DeepEqual(got, rows) {
				t.Fatal("failed replacement changed rows")
			}
		}
	}
	metadata, err := dataset.Table("inherited").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	row := func(s string) *bigquery.ValuesSaver {
		return &bigquery.ValuesSaver{Schema: metadata.Schema, Row: []bigquery.Value{s, []byte("abc"), nil, []bigquery.Value{}}}
	}
	if err := dataset.Table("inherited").Inserter().Put(ctx, row("abc")); err != nil {
		t.Fatal(err)
	}
	if err := dataset.Table("inherited").Inserter().Put(ctx, row("abcd")); err == nil {
		t.Fatal("streaming an overlong value succeeded")
	}
	wait := func(job *bigquery.Job, err error, wantError bool) {
		t.Helper()
		if err == nil {
			var status *bigquery.JobStatus
			status, err = job.Wait(ctx)
			if err == nil {
				err = status.Err()
			}
		}
		if wantError {
			if err == nil || !strings.Contains(err.Error(), "maximum length 3") {
				t.Fatalf("job error = %v, want length constraint error", err)
			}
		} else if err != nil {
			t.Fatal(err)
		}
	}
	before := p.read("SELECT TO_JSON_STRING(t) FROM inherited AS t ORDER BY 1")
	q := client.Query(invalid[0])
	q.Dst = dataset.Table("inherited")
	q.WriteDisposition = bigquery.WriteTruncateData
	job, err := q.Run(ctx)
	wait(job, err, true)
	if got := p.read("SELECT TO_JSON_STRING(t) FROM inherited AS t ORDER BY 1"); !reflect.DeepEqual(got, before) {
		t.Fatal("failed WRITE_TRUNCATE_DATA changed rows")
	}
	for _, s := range []string{"abc", "abcd"} {
		q := client.Query("SELECT '" + s + "' AS s, b'abc' AS b, STRUCT('abc' AS label) AS r, ['abc'] AS a")
		q.Dst = dataset.Table("inherited")
		q.WriteDisposition = bigquery.WriteAppend
		job, err := q.Run(ctx)
		wait(job, err, s == "abcd")
		data := bigquery.NewReaderSource(strings.NewReader(`{"s":"` + s + `","b":"YWJj","r":{"label":"abc"},"a":["abc"]}`))
		data.SourceFormat = bigquery.JSON
		loader := dataset.Table("inherited").LoaderFrom(data)
		loader.WriteDisposition = bigquery.WriteAppend
		job, err = loader.Run(ctx)
		wait(job, err, s == "abcd")
	}
}

// Base64 text is decoded before applying BYTES(L), including nested JSON fields.
// https://cloud.google.com/bigquery/docs/loading-data-cloud-storage-json#details_of_loading_json_data
// https://cloud.google.com/bigquery/docs/loading-data-cloud-storage-csv#data_types
func TestColumnLengthLoadBytes(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_column_length_load_bytes")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := datasetQueries{t: t, client: client, dataset: dataset}
	p.mustRun("CREATE TABLE json_bytes (b BYTES(3), r STRUCT<b BYTES(3)>, a ARRAY<BYTES(3)>)")
	p.mustRun("CREATE TABLE csv_bytes (b BYTES(3))")
	load := func(table, data string, format bigquery.DataFormat, fails bool) {
		t.Helper()
		source := bigquery.NewReaderSource(strings.NewReader(data))
		source.SourceFormat = format
		loader := dataset.Table(table).LoaderFrom(source)
		loader.WriteDisposition = bigquery.WriteAppend
		job, err := loader.Run(ctx)
		if err != nil {
			t.Fatal(err)
		}
		status, err := job.Wait(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if (status.Err() != nil) != fails {
			t.Fatalf("loading %s: error = %v, want error %v", data, status.Err(), fails)
		}
		if fails && !strings.Contains(status.Err().Error(), "maximum length 3") {
			t.Fatalf("loading %s: want a length error, got %v", data, status.Err())
		}
	}
	load("json_bytes", `{"b":"AP8B","r":{"b":"YWJj"},"a":["","YWJj"]}`, bigquery.JSON, false)
	if got, want := p.read("SELECT TO_HEX(b), TO_HEX(r.b), TO_HEX(a[OFFSET(0)]), TO_HEX(a[OFFSET(1)]) FROM json_bytes"), [][]bigquery.Value{{"00ff01", "616263", "", "616263"}}; !reflect.DeepEqual(got, want) {
		t.Fatalf("decoded bytes = %v, want %v", got, want)
	}
	for _, data := range []string{`{"b":"YWJjZA=="}`, `{"r":{"b":"YWJjZA=="}}`, `{"a":["YWJjZA=="]}`} {
		load("json_bytes", data, bigquery.JSON, true)
	}
	load("csv_bytes", "AP8B\n", bigquery.CSV, false)
	load("csv_bytes", "YWJjZA==\n", bigquery.CSV, true)
	if got, want := p.read("SELECT TO_HEX(b) FROM csv_bytes"), [][]bigquery.Value{{"00ff01"}}; !reflect.DeepEqual(got, want) {
		t.Fatalf("decoded CSV bytes = %v, want %v", got, want)
	}
}
