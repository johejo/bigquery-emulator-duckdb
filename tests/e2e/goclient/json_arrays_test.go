package goclient

import (
	"context"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// Check typed client decoding and schema across rows. Values follow the JSON
// functions' documented flattening and SQL-to-JSON encoding rules.
func TestJsonArrayFunctionResults(t *testing.T) {
	rows, err := newClient(t).Query(`SELECT
		JSON_FLATTEN(JSON_ARRAY([n], JSON 'null')) AS flattened,
		JSON_ARRAY_APPEND(JSON '[]', '$', STRUCT(n AS n)) AS appended,
		JSON_ARRAY_INSERT(JSON '[]', '$[0]', STRUCT(n AS n)) AS inserted
		FROM UNNEST([1, 2]) AS n ORDER BY n`).Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	wantRows := [][]bigquery.Value{
		{[]bigquery.Value{"1", "null"}, `[{"n":1}]`, `[{"n":1}]`},
		{[]bigquery.Value{"2", "null"}, `[{"n":2}]`, `[{"n":2}]`},
	}
	for _, want := range wantRows {
		var got []bigquery.Value
		if err := rows.Next(&got); err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("values = %#v, want %#v", got, want)
		}
	}
	wantSchema := bigquery.Schema{
		{Name: "flattened", Type: bigquery.JSONFieldType, Repeated: true},
		{Name: "appended", Type: bigquery.JSONFieldType},
		{Name: "inserted", Type: bigquery.JSONFieldType},
	}
	if !reflect.DeepEqual(rows.Schema, wantSchema) {
		t.Fatalf("schema = %#v, want %#v", rows.Schema, wantSchema)
	}
	var got []bigquery.Value
	if err := rows.Next(&got); err != iterator.Done {
		t.Fatalf("next = %v, want iterator.Done", err)
	}
}
