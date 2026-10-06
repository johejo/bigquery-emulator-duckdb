package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// A BIGNUMERIC(P, S) column rounds what is written to it half away from zero to S fractional
// digits and rejects a value with more than P - S integer digits, as NUMERIC(P, S) does, nested
// fields included, whatever writes it. The values follow BigQuery's documented examples for
// NUMERIC(5, 2); BIGNUMERIC(76, 38) and BIGNUMERIC(40, 2) allow 38 integer digits.
func TestBigNumericPrecisionAndScale(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_bignumeric_precision")
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
	mustRun := func(sql string) {
		t.Helper()
		if err := run(sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	// The values of the query's single column as text.
	values := func(sql string) []string {
		t.Helper()
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		var got []string
		for {
			var row []bigquery.Value
			err := rows.Next(&row)
			if err == iterator.Done {
				return got
			}
			if err != nil {
				t.Fatalf("%s: %v", sql, err)
			}
			text, _ := row[0].(string)
			got = append(got, text)
		}
	}
	check := func(sql string, want ...string) {
		t.Helper()
		if got := values(sql); !reflect.DeepEqual(got, want) {
			t.Errorf("%s: got %q, want %q", sql, got, want)
		}
	}

	mustRun(`CREATE TABLE t (
		id INT64,
		n BIGNUMERIC(5, 2),
		a ARRAY<STRUCT<n BIGNUMERIC(5, 2)>>,
		w BIGNUMERIC(76, 38),
		d BIGNUMERIC(5, 2) DEFAULT 1.125)`)
	metadata, err := dataset.Table("t").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	want := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "n", Type: bigquery.BigNumericFieldType, Precision: 5, Scale: 2},
		{Name: "a", Type: bigquery.RecordFieldType, Repeated: true, Schema: bigquery.Schema{
			{Name: "n", Type: bigquery.BigNumericFieldType, Precision: 5, Scale: 2},
		}},
		{Name: "w", Type: bigquery.BigNumericFieldType, Precision: 76, Scale: 38},
		{Name: "d", Type: bigquery.BigNumericFieldType, Precision: 5, Scale: 2, DefaultValueExpression: "1.125"},
	}
	if !reflect.DeepEqual(metadata.Schema, want) {
		t.Errorf("got schema\n%v\nwant\n%v", fields(metadata.Schema), fields(want))
	}

	mustRun("INSERT t (id, n, a) VALUES (1, 1.125, [STRUCT(BIGNUMERIC '-1.125')])")
	check("SELECT CAST(n AS STRING) FROM t", "1.13")
	check("SELECT CAST(a[0].n AS STRING) FROM t", "-1.13")
	check("SELECT CAST(d AS STRING) FROM t", "1.13")

	// Writing a column of another precision and scale rounds again; so do updates.
	mustRun("CREATE TABLE e (n BIGNUMERIC(4, 1))")
	mustRun("INSERT e SELECT n FROM t")
	check("SELECT CAST(n AS STRING) FROM e", "1.1")
	mustRun("UPDATE t SET n = n / 3 WHERE TRUE")
	check("SELECT CAST(n AS STRING) FROM t", "0.38")
	mustRun("CREATE TABLE c (n BIGNUMERIC(5, 2)) AS SELECT BIGNUMERIC '1.125' AS n")
	check("SELECT CAST(n AS STRING) FROM c", "1.13")

	for _, sql := range []string{
		"INSERT t (n) VALUES (1111)",
		"INSERT t (a) VALUES ([STRUCT(BIGNUMERIC '1111')])",
		"INSERT t (w) VALUES (BIGNUMERIC '1e38')",
		"UPDATE t SET n = 1000 WHERE TRUE",
	} {
		if err := run(sql); err == nil {
			t.Errorf("%s: got no error, want a value out of range", sql)
		}
	}

	// Tables the API creates, streaming inserts and loads round and check values too.
	schema := bigquery.Schema{{Name: "n", Type: bigquery.BigNumericFieldType, Precision: 40, Scale: 2}}
	api := dataset.Table("api")
	if err := api.Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatal(err)
	}
	put := func(value string) error {
		return api.Inserter().Put(ctx, &bigquery.ValuesSaver{Schema: schema, Row: []bigquery.Value{rat(t, value)}})
	}
	if err := put("12345678901234567890123456789012345678.125"); err != nil {
		t.Fatalf("Put: %v", err)
	}
	if err := put("123456789012345678901234567890123456789"); err == nil {
		t.Error("Put: got no error, want 39 integer digits out of range of BIGNUMERIC(40, 2)")
	}
	source := bigquery.NewReaderSource(strings.NewReader("{\"n\": -1.125}\n"))
	source.SourceFormat = bigquery.JSON
	job, err := api.LoaderFrom(source).Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if status, err := job.Wait(ctx); err != nil || status.Err() != nil {
		t.Fatalf("load: %v %v", err, status.Err())
	}
	check("SELECT CAST(n AS STRING) FROM api ORDER BY n",
		"-1.13", "12345678901234567890123456789012345678.13")
}
