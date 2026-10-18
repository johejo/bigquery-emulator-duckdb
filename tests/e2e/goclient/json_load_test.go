package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// A NEWLINE_DELIMITED_JSON load writes NULL to a NULLABLE field that rows omit or give as null,
// even when no row of the files has a value for it, including BIGNUMERIC fields.
func TestJSONLoadNullColumns(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_json_load_nulls")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "s", Type: bigquery.StringFieldType},
		{Name: "b", Type: bigquery.BigNumericFieldType},
		{Name: "r", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			{Name: "n", Type: bigquery.NumericFieldType},
			{Name: "m", Type: bigquery.BigNumericFieldType},
		}},
	}
	table := dataset.Table("t")
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatal(err)
	}
	source := bigquery.NewReaderSource(strings.NewReader(`{"id": 1, "b": null}
{"id": 2, "r": {"n": 1.5, "m": null}}
`))
	source.SourceFormat = bigquery.JSON
	if err := wait(ctx, table.LoaderFrom(source)); err != nil {
		t.Fatalf("load: %v", err)
	}
	rows, err := client.Query(`SELECT id, s, CAST(b AS STRING), CAST(r.n AS STRING), CAST(r.m AS STRING)
		FROM go_json_load_nulls.t ORDER BY id`).Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var got [][]bigquery.Value
	for {
		var row []bigquery.Value
		if err := rows.Next(&row); err != nil {
			break
		}
		got = append(got, row)
	}
	want := [][]bigquery.Value{
		{int64(1), nil, nil, nil, nil},
		{int64(2), nil, nil, "1.5", nil},
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("rows = %v, want %v", got, want)
	}
}
