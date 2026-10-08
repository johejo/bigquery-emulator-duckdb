package goclient

import (
	"context"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// Check the Go client's typed decoding and result schema, including nested INTERVALs.
// The values follow BigQuery's data-types#interval_type construction examples.
func TestIntervalResults(t *testing.T) {
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
