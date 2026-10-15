package goclient

import (
	"context"
	"fmt"
	"reflect"
	"strings"
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

// The rows and both session sets are from BigQuery's RANGE_SESSIONIZE examples:
// https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/range-functions#range_sessionize
func TestRangeSessionize(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_range_sessionize")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	for _, sql := range []string{
		"CREATE TABLE " + dataset.DatasetID + ".ranges (emp_id INT64, dept_id INT64, duration RANGE<DATE>)",
		"INSERT " + dataset.DatasetID + `.ranges VALUES
    (10, 1000, RANGE<DATE> '[2010-01-10, 2010-03-10)'),
    (10, 2000, RANGE<DATE> '[2010-03-10, 2010-07-15)'),
    (10, 2000, RANGE<DATE> '[2010-06-15, 2010-08-18)'),
    (20, 2000, RANGE<DATE> '[2010-03-10, 2010-07-20)'),
    (20, 1000, RANGE<DATE> '[2020-05-10, 2020-09-20)')`,
	} {
		if _, err := client.Query(sql).Read(ctx); err != nil {
			t.Fatal(err)
		}
	}
	dateRange := func(start, end string) *bigquery.RangeValue {
		s, err := civil.ParseDate(start)
		if err != nil {
			t.Fatal(err)
		}
		e, err := civil.ParseDate(end)
		if err != nil {
			t.Fatal(err)
		}
		return &bigquery.RangeValue{Start: s, End: e}
	}
	durations := []*bigquery.RangeValue{
		dateRange("2010-01-10", "2010-03-10"), dateRange("2010-03-10", "2010-07-15"),
		dateRange("2010-06-15", "2010-08-18"), dateRange("2010-03-10", "2010-07-20"),
		dateRange("2020-05-10", "2020-09-20"),
	}
	for _, option := range []string{"", ", 'MEETS'", ", 'OVERLAPS'"} {
		t.Run(option, func(t *testing.T) {
			query := client.Query(fmt.Sprintf(`SELECT * FROM RANGE_SESSIONIZE(TABLE %s.ranges,
     'duration', ['emp_id']%s) ORDER BY emp_id, duration`, dataset.DatasetID, option))
			rows, err := query.Read(ctx)
			if err != nil {
				t.Fatal(err)
			}
			wantSchema := bigquery.Schema{
				{Name: "emp_id", Type: bigquery.IntegerFieldType}, {Name: "dept_id", Type: bigquery.IntegerFieldType},
				{Name: "duration", Type: bigquery.RangeFieldType, RangeElementType: &bigquery.RangeElementType{Type: bigquery.DateFieldType}},
				{Name: "session_range", Type: bigquery.RangeFieldType, RangeElementType: &bigquery.RangeElementType{Type: bigquery.DateFieldType}},
			}
			sessions := []*bigquery.RangeValue{
				dateRange("2010-01-10", "2010-08-18"), dateRange("2010-01-10", "2010-08-18"),
				dateRange("2010-01-10", "2010-08-18"), durations[3], durations[4],
			}
			if option == ", 'OVERLAPS'" {
				sessions[0] = durations[0]
				sessions[1] = dateRange("2010-03-10", "2010-08-18")
				sessions[2] = sessions[1]
			}
			for i := range durations {
				var got []bigquery.Value
				if err := rows.Next(&got); err != nil {
					t.Fatal(err)
				}
				emp := []int64{10, 10, 10, 20, 20}[i]
				dept := []int64{1000, 2000, 2000, 2000, 1000}[i]
				want := []bigquery.Value{emp, dept, durations[i], sessions[i]}
				if !reflect.DeepEqual(got, want) {
					t.Fatalf("row %d = %v, want %v", i, got, want)
				}
			}
			if !reflect.DeepEqual(rows.Schema, wantSchema) {
				t.Fatalf("schema = %v, want %v", fields(rows.Schema), fields(wantSchema))
			}
			var extra []bigquery.Value
			if err := rows.Next(&extra); err != iterator.Done {
				t.Fatalf("next = %v, want iterator.Done", err)
			}
		})
	}
}

// Session ranges preserve the element type that the input RANGE resolves to.
func TestRangeSessionizeElementTypes(t *testing.T) {
	for _, c := range []struct {
		element bigquery.FieldType
		sql     string
		want    *bigquery.RangeValue
	}{
		{bigquery.DateTimeFieldType,
			"RANGE(DATETIME '2022-10-01 14:53:27', DATETIME '2022-10-01 16:00:00')",
			&bigquery.RangeValue{
				Start: civil.DateTime{Date: civil.Date{Year: 2022, Month: 10, Day: 1}, Time: civil.Time{Hour: 14, Minute: 53, Second: 27}},
				End:   civil.DateTime{Date: civil.Date{Year: 2022, Month: 10, Day: 1}, Time: civil.Time{Hour: 16}},
			}},
		{bigquery.TimestampFieldType,
			"RANGE(TIMESTAMP '2022-10-01 14:53:27 America/Los_Angeles', TIMESTAMP '2022-10-01 16:00:00 America/Los_Angeles')",
			&bigquery.RangeValue{Start: time.Date(2022, 10, 1, 21, 53, 27, 0, time.UTC), End: time.Date(2022, 10, 1, 23, 0, 0, 0, time.UTC)}},
	} {
		t.Run(string(c.element), func(t *testing.T) {
			query := newClient(t).Query("WITH input AS (SELECT " + c.sql + " AS r) SELECT session_range FROM RANGE_SESSIONIZE(TABLE input, 'r', [])")
			rows, err := query.Read(context.Background())
			if err != nil {
				t.Fatal(err)
			}
			wantSchema := bigquery.Schema{{Name: "session_range", Type: bigquery.RangeFieldType, RangeElementType: &bigquery.RangeElementType{Type: c.element}}}
			var got []bigquery.Value
			if err := rows.Next(&got); err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(rows.Schema, wantSchema) {
				t.Fatalf("schema = %v, want %v", fields(rows.Schema), fields(wantSchema))
			}
			if want := []bigquery.Value{c.want}; !reflect.DeepEqual(got, want) {
				t.Fatalf("values = %v, want %v", got, want)
			}
		})
	}
}

// A CSV load writes a RANGE as "[start, end)", quoted for its comma, where NULL and UNBOUNDED are
// unbounded ends; the values are those of BigQuery's CSV loading documentation:
// https://docs.cloud.google.com/bigquery/docs/loading-data-cloud-storage-csv#data_types
func TestRangeLoadCSV(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_range_load_csv")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	rangeOf := func(element bigquery.FieldType) *bigquery.RangeElementType {
		return &bigquery.RangeElementType{Type: element}
	}
	source := bigquery.NewReaderSource(strings.NewReader(`1,"[2020-01-01, 2021-01-01)","[2020-01-01 12:00:00, 2020-01-02 00:00:00)","[2020-01-01 00:00:00+00, UNBOUNDED)"
2,"[UNBOUNDED, 2021-01-01)",,
3,"[2020-03-01, NULL)",,
4,"[UNBOUNDED, UNBOUNDED)",,
`))
	source.SourceFormat = bigquery.CSV
	source.Schema = bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "d", Type: bigquery.RangeFieldType, RangeElementType: rangeOf(bigquery.DateFieldType)},
		{Name: "dt", Type: bigquery.RangeFieldType, RangeElementType: rangeOf(bigquery.DateTimeFieldType)},
		{Name: "ts", Type: bigquery.RangeFieldType, RangeElementType: rangeOf(bigquery.TimestampFieldType)},
	}
	job, err := dataset.Table("ranges").LoaderFrom(source).Run(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if status, err := job.Wait(ctx); err != nil || status.Err() != nil {
		t.Fatalf("load: %v %v", err, status.Err())
	}
	query := client.Query("SELECT d, dt, ts FROM ranges ORDER BY id")
	query.DefaultDatasetID = dataset.DatasetID
	rows, err := query.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	date := func(year int, month time.Month, day int) civil.Date {
		return civil.Date{Year: year, Month: month, Day: day}
	}
	want := [][]bigquery.Value{
		{&bigquery.RangeValue{Start: date(2020, 1, 1), End: date(2021, 1, 1)},
			&bigquery.RangeValue{Start: civil.DateTime{Date: date(2020, 1, 1), Time: civil.Time{Hour: 12}},
				End: civil.DateTime{Date: date(2020, 1, 2)}},
			&bigquery.RangeValue{Start: time.Date(2020, 1, 1, 0, 0, 0, 0, time.UTC)}},
		{&bigquery.RangeValue{End: date(2021, 1, 1)}, nil, nil},
		{&bigquery.RangeValue{Start: date(2020, 3, 1)}, nil, nil},
		{&bigquery.RangeValue{}, nil, nil},
	}
	for i, values := range want {
		var got []bigquery.Value
		if err := rows.Next(&got); err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(got, values) {
			t.Fatalf("row %d = %v, want %v", i, got, values)
		}
	}
	if err := rows.Next(&[]bigquery.Value{}); err != iterator.Done {
		t.Fatalf("next = %v, want iterator.Done", err)
	}
}
