package goclient

import (
	"context"
	"reflect"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/civil"
	"google.golang.org/api/iterator"
)

// Check the Go client's typed decoding and result schema of each RANGE element type, including
// unbounded ends and an array of RANGEs; the client decodes no RANGE in a STRUCT. The values
// follow BigQuery's range-functions RANGE examples.
func TestRangeResults(t *testing.T) {
	rows, err := newClient(t).Query(`SELECT
		RANGE(DATE '2022-12-01', DATE '2022-12-31') AS d,
		RANGE(DATETIME '2022-10-01 14:53:27', DATETIME '2022-10-01 16:00:00') AS dt,
		RANGE(TIMESTAMP '2022-10-01 14:53:27 America/Los_Angeles',
		      TIMESTAMP '2022-10-01 16:00:00 America/Los_Angeles') AS ts,
		RANGE(NULL, DATE '2022-12-31') AS u,
		[RANGE(DATE '2022-10-01', NULL), RANGE<DATE> '[UNBOUNDED, UNBOUNDED)'] AS a`).Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var got []bigquery.Value
	if err := rows.Next(&got); err != nil {
		t.Fatal(err)
	}
	want := []bigquery.Value{
		&bigquery.RangeValue{Start: civil.Date{Year: 2022, Month: 12, Day: 1},
			End: civil.Date{Year: 2022, Month: 12, Day: 31}},
		&bigquery.RangeValue{
			Start: civil.DateTime{Date: civil.Date{Year: 2022, Month: 10, Day: 1},
				Time: civil.Time{Hour: 14, Minute: 53, Second: 27}},
			End: civil.DateTime{Date: civil.Date{Year: 2022, Month: 10, Day: 1},
				Time: civil.Time{Hour: 16}}},
		&bigquery.RangeValue{Start: time.Date(2022, 10, 1, 21, 53, 27, 0, time.UTC),
			End: time.Date(2022, 10, 1, 23, 0, 0, 0, time.UTC)},
		&bigquery.RangeValue{End: civil.Date{Year: 2022, Month: 12, Day: 31}},
		[]bigquery.Value{&bigquery.RangeValue{Start: civil.Date{Year: 2022, Month: 10, Day: 1}},
			&bigquery.RangeValue{}},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("values = %#v, want %#v", got, want)
	}
	rangeOf := func(element bigquery.FieldType) *bigquery.RangeElementType {
		return &bigquery.RangeElementType{Type: element}
	}
	wantSchema := bigquery.Schema{
		{Name: "d", Type: bigquery.RangeFieldType, RangeElementType: rangeOf(bigquery.DateFieldType)},
		{Name: "dt", Type: bigquery.RangeFieldType,
			RangeElementType: rangeOf(bigquery.DateTimeFieldType)},
		{Name: "ts", Type: bigquery.RangeFieldType,
			RangeElementType: rangeOf(bigquery.TimestampFieldType)},
		{Name: "u", Type: bigquery.RangeFieldType, RangeElementType: rangeOf(bigquery.DateFieldType)},
		{Name: "a", Type: bigquery.RangeFieldType, Repeated: true,
			RangeElementType: rangeOf(bigquery.DateFieldType)},
	}
	if !reflect.DeepEqual(rows.Schema, wantSchema) {
		t.Fatalf("schema = %#v, want %#v", rows.Schema, wantSchema)
	}
	if err := rows.Next(&got); err != iterator.Done {
		t.Fatalf("next = %v, want iterator.Done", err)
	}
}

// RANGE query parameters, which the client sends as a rangeValue whose missing bound is unbounded.
func TestRangeParameters(t *testing.T) {
	query := newClient(t).Query(`SELECT RANGE_START(@d) AS s, RANGE_END(@d) AS e,
		RANGE_CONTAINS(@ts, TIMESTAMP '2024-04-11 00:00:00+00') AS c`)
	query.Parameters = []bigquery.QueryParameter{
		{Name: "d", Value: &bigquery.RangeValue{Start: civil.Date{Year: 2024, Month: 4, Day: 11}}},
		{Name: "ts", Value: &bigquery.RangeValue{
			Start: time.Date(2024, 4, 10, 0, 0, 0, 0, time.UTC),
			End:   time.Date(2024, 4, 12, 0, 0, 0, 0, time.UTC)}},
	}
	rows, err := query.Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var got []bigquery.Value
	if err := rows.Next(&got); err != nil {
		t.Fatal(err)
	}
	want := []bigquery.Value{civil.Date{Year: 2024, Month: 4, Day: 11}, nil, true}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("values = %#v, want %#v", got, want)
	}
}

// A table with RANGE columns keeps its schema, takes rows from tabledata.insertAll as objects of
// start and end, and reads them back.
func TestRangeColumns(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_range_columns")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "d", Type: bigquery.RangeFieldType,
			RangeElementType: &bigquery.RangeElementType{Type: bigquery.DateFieldType}},
		{Name: "ts", Type: bigquery.RangeFieldType,
			RangeElementType: &bigquery.RangeElementType{Type: bigquery.TimestampFieldType}},
	}
	table := dataset.Table("ranges")
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
	type row struct {
		ID int64                `bigquery:"id"`
		D  *bigquery.RangeValue `bigquery:"d"`
		TS *bigquery.RangeValue `bigquery:"ts"`
	}
	inserted := []*row{
		{ID: 1, D: &bigquery.RangeValue{Start: civil.Date{Year: 2010, Month: 1, Day: 10},
			End: civil.Date{Year: 2010, Month: 3, Day: 10}},
			TS: &bigquery.RangeValue{Start: time.Date(2020, 1, 1, 0, 0, 0, 0, time.UTC)}},
		{ID: 2, D: &bigquery.RangeValue{End: civil.Date{Year: 2020, Month: 9, Day: 20}}},
	}
	if err := table.Inserter().Put(ctx, inserted); err != nil {
		t.Fatal(err)
	}
	query := client.Query(`SELECT id, d, ts FROM ranges
		WHERE RANGE_OVERLAPS(d, RANGE<DATE> '[2010-02-01, 2010-02-02)') OR id = 2 ORDER BY d`)
	query.DefaultDatasetID = dataset.DatasetID
	rows, err := query.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var got []*row
	for {
		var r row
		err := rows.Next(&r)
		if err == iterator.Done {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		got = append(got, &r)
	}
	// An unbounded start sorts first.
	want := []*row{inserted[1], inserted[0]}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("rows = %v, want %v", got, want)
	}
}
