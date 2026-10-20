package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// Check the Go client's typed decoding and result schema, including nested INTERVALs.
// The values follow BigQuery's data-types#interval_type construction examples.
func TestIntervalResults(t *testing.T) {
	t.Parallel()
	rows, err := newClient(t).Query(`SELECT
		INTERVAL '8 -20 17' MONTH TO HOUR AS i,
		EXTRACT(HOUR FROM INTERVAL 1500 MINUTE) AS h,
		[INTERVAL 1 YEAR] AS a,
		STRUCT(INTERVAL 90 SECOND AS v) AS s`).Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var got []bigquery.Value
	if err := rows.Next(&got); err != nil {
		t.Fatal(err)
	}
	want := []bigquery.Value{
		&bigquery.IntervalValue{Months: 8, Days: -20, Hours: 17},
		int64(25),
		[]bigquery.Value{&bigquery.IntervalValue{Years: 1}},
		[]bigquery.Value{&bigquery.IntervalValue{Minutes: 1, Seconds: 30}},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("values = %#v, want %#v", got, want)
	}
	wantSchema := bigquery.Schema{
		{Name: "i", Type: bigquery.IntervalFieldType},
		{Name: "h", Type: bigquery.IntegerFieldType},
		{Name: "a", Type: bigquery.IntervalFieldType, Repeated: true},
		{Name: "s", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			{Name: "v", Type: bigquery.IntervalFieldType},
		}},
	}
	if !reflect.DeepEqual(rows.Schema, wantSchema) {
		t.Fatalf("schema = %#v, want %#v", rows.Schema, wantSchema)
	}
	if err := rows.Next(&got); err != iterator.Done {
		t.Fatalf("next = %v, want iterator.Done", err)
	}
}

// INTERVAL query parameters retain their independent month, day and microsecond parts.
// EXTRACT follows https://cloud.google.com/bigquery/docs/reference/standard-sql/interval_functions#extract.
func TestIntervalParameters(t *testing.T) {
	t.Parallel()
	query := newClient(t).Query(`SELECT @i AS i, EXTRACT(MONTH FROM @i) AS m,
		EXTRACT(MICROSECOND FROM @i) AS us, EXTRACT(DAY FROM @i) AS d, @a AS a`)
	value := &bigquery.IntervalValue{Years: 1, Months: 2, Days: -3, Hours: 4, Minutes: 5, Seconds: 6,
		SubSecondNanos: 789999000}
	query.Parameters = []bigquery.QueryParameter{
		{Name: "i", Value: value},
		{Name: "a", Value: []*bigquery.IntervalValue{{Days: -1}}},
	}
	rows, err := query.Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var got []bigquery.Value
	if err := rows.Next(&got); err != nil {
		t.Fatal(err)
	}
	want := []bigquery.Value{value, int64(2), int64(789999), int64(-3),
		[]bigquery.Value{&bigquery.IntervalValue{Days: -1}}}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("values = %#v, want %#v", got, want)
	}
}

// A table with INTERVAL columns, also REPEATED and in a RECORD, keeps its schema, takes
// tabledata.insertAll rows in the canonical form, and reads back each value's month, day and time
// parts.
func TestIntervalColumns(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_interval_columns")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "i", Type: bigquery.IntervalFieldType},
		{Name: "a", Type: bigquery.IntervalFieldType, Repeated: true},
		{Name: "s", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			{Name: "v", Type: bigquery.IntervalFieldType},
		}},
	}
	table := dataset.Table("intervals")
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatal(err)
	}
	metadata, err := table.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(metadata.Schema, schema) {
		t.Fatalf("schema = %v, want %v", fields(metadata.Schema), fields(schema))
	}
	// The client would marshal an IntervalValue as a JSON object, so rows carry the canonical form.
	value := &bigquery.IntervalValue{Years: 1, Months: 2, Days: -3, Hours: 4, Minutes: 5, Seconds: 6,
		SubSecondNanos: 789000000}
	rows := []*bigquery.ValuesSaver{
		{Schema: schema, Row: []bigquery.Value{1, value.String(), []bigquery.Value{"0-1 0 0:0:0", "0-0 30 0:0:0"},
			[]bigquery.Value{"0-0 0 25:0:0"}}},
		{Schema: schema, Row: []bigquery.Value{2, "0-0 -5 0:0:0", []bigquery.Value{}, nil}},
	}
	if err := table.Inserter().Put(ctx, rows); err != nil {
		t.Fatal(err)
	}
	query := client.Query("SELECT id, i, a, s FROM intervals ORDER BY id")
	query.DefaultDatasetID = dataset.DatasetID
	it, err := query.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	want := [][]bigquery.Value{
		{int64(1), value,
			[]bigquery.Value{&bigquery.IntervalValue{Months: 1}, &bigquery.IntervalValue{Days: 30}},
			[]bigquery.Value{&bigquery.IntervalValue{Hours: 25}}},
		{int64(2), &bigquery.IntervalValue{Days: -5}, []bigquery.Value(nil), nil},
	}
	for i, w := range want {
		var got []bigquery.Value
		if err := it.Next(&got); err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(got, w) {
			t.Fatalf("row %d = %#v, want %#v", i, got, w)
		}
	}
	var extra []bigquery.Value
	if err := it.Next(&extra); err != iterator.Done {
		t.Fatalf("next = %v, want iterator.Done", err)
	}
}

// A CSV load reads an INTERVAL in the form Y-M D H:M:S[.F]; the values are those of BigQuery's
// CSV loading documentation:
// https://docs.cloud.google.com/bigquery/docs/loading-data-cloud-storage-csv#data_types
// Loads from other formats, whose forms BigQuery does not document, are unsupported.
func TestIntervalLoad(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_interval_load")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "i", Type: bigquery.IntervalFieldType},
	}
	load := func(table string, format bigquery.DataFormat, data string) error {
		source := bigquery.NewReaderSource(strings.NewReader(data))
		source.SourceFormat = format
		source.Schema = schema
		job, err := dataset.Table(table).LoaderFrom(source).Run(ctx)
		if err != nil {
			return err
		}
		status, err := job.Wait(ctx)
		if err != nil {
			return err
		}
		return status.Err()
	}
	if err := load("csv", bigquery.CSV, "1,10-6 0 0:0:0\n2,0-0 -5 0:0:0\n3,0-0 0 0:0:1.25\n4,\n"); err != nil {
		t.Fatal(err)
	}
	query := client.Query("SELECT i FROM csv ORDER BY id")
	query.DefaultDatasetID = dataset.DatasetID
	rows, err := query.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	want := []bigquery.Value{
		&bigquery.IntervalValue{Years: 10, Months: 6},
		&bigquery.IntervalValue{Days: -5},
		&bigquery.IntervalValue{Seconds: 1, SubSecondNanos: 250000000},
		nil,
	}
	for i, w := range want {
		var got []bigquery.Value
		if err := rows.Next(&got); err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(got, []bigquery.Value{w}) {
			t.Fatalf("row %d = %#v, want %#v", i, got, w)
		}
	}
	err = load("json", bigquery.JSON, `{"id": 1, "i": "10-6 0 0:0:0"}`+"\n")
	if err == nil || !strings.Contains(err.Error(), "loading INTERVAL columns from NEWLINE_DELIMITED_JSON") {
		t.Fatalf("JSON load error = %v, want unsupported", err)
	}
}
