package goclient

import (
	"cmp"
	"context"
	"errors"
	"reflect"
	"slices"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// A table with GEOGRAPHY columns, also REPEATED and in a RECORD, takes WKT from
// tabledata.insertAll and loads, and GEOGRAPHY values from queries and DML. tabledata.list
// returns each as WKT, and queries read the columns as GEOGRAPHY. The WKT and coordinates are
// those of scalars/geography.txt and scalars/geography_linestring.txt.
func TestGeographyColumns(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_geography_columns")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}
	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "g", Type: bigquery.GeographyFieldType},
		{Name: "gs", Type: bigquery.GeographyFieldType, Repeated: true},
		{Name: "r", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{
			{Name: "g", Type: bigquery.GeographyFieldType},
		}},
	}
	table := dataset.Table("places")
	if err := table.Create(ctx, &bigquery.TableMetadata{Schema: schema}); err != nil {
		t.Fatal(err)
	}
	row := func(values ...bigquery.Value) *bigquery.ValuesSaver {
		return &bigquery.ValuesSaver{Schema: schema, Row: values}
	}
	inserter := table.Inserter()
	if err := inserter.Put(ctx, []*bigquery.ValuesSaver{row(int64(1), "POINT(-122.35 47.62)",
		[]bigquery.Value{"LINESTRING(0 0, 132 0)"}, []bigquery.Value{"POINT(10 10)"})}); err != nil {
		t.Fatal(err)
	}
	// Each row with a value that is not WKT fails; GeoJSON, which BigQuery takes, is unsupported,
	// as is a polygon whose shell runs clockwise, which BigQuery takes as the region on its left:
	// https://cloud.google.com/bigquery/docs/geospatial-data#polygon_orientation
	var rowErrors bigquery.PutMultiError
	inserter.SkipInvalidRows = true
	err := inserter.Put(ctx, []*bigquery.ValuesSaver{
		row(int64(2), "POINT(1", nil, nil),
		row(int64(3), `{"type": "Point", "coordinates": [1, 2]}`, nil, nil),
		row(int64(8), "POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))", nil, nil),
	})
	if !errors.As(err, &rowErrors) || len(rowErrors) != 3 ||
		!strings.Contains(rowErrors[1].Error(), "does not support GeoJSON") ||
		!strings.Contains(rowErrors[2].Error(), "whose shell runs clockwise") {
		t.Fatalf("invalid rows: %v, want an error for each, for GeoJSON and a clockwise shell", err)
	}

	if err := p.load("places", `{"id": 4, "g": "POINT(5 6)", "r": {"g": null}}`+"\n", schema,
		bigquery.WriteAppend); err != nil {
		t.Fatal(err)
	}
	p.mustRun("INSERT places (id, g) VALUES (5, ST_GEOGPOINT(170, 10))")
	if err := p.query("SELECT 6 AS id, ST_GEOGPOINT(12, 13) AS g, [ST_GEOGPOINT(5, 6)] AS gs",
		"places", bigquery.WriteAppend); err != nil {
		t.Fatal(err)
	}

	listed := table.Read(ctx)
	var got [][]bigquery.Value
	for {
		var values []bigquery.Value
		if err := listed.Next(&values); err == iterator.Done {
			break
		} else if err != nil {
			t.Fatal(err)
		}
		got = append(got, values)
	}
	slices.SortFunc(got, func(a, b []bigquery.Value) int { return cmp.Compare(a[0].(int64), b[0].(int64)) })
	// The client reads an empty array as nil.
	var empty []bigquery.Value
	want := [][]bigquery.Value{
		{int64(1), "POINT(-122.35 47.62)", []bigquery.Value{"LINESTRING(0 0, 132 0)"},
			[]bigquery.Value{"POINT(10 10)"}},
		{int64(4), "POINT(5 6)", empty, []bigquery.Value{nil}},
		{int64(5), "POINT(170 10)", empty, nil},
		{int64(6), "POINT(12 13)", []bigquery.Value{"POINT(5 6)"}, nil},
	}
	if !reflect.DeepEqual(got, want) {
		t.Errorf("tabledata.list = %v, want %v", got, want)
	}

	if got, want := p.read(`SELECT ST_X(g), ST_Y(g), ST_ASTEXT(gs[OFFSET(0)]), ST_ASTEXT(r.g)
		FROM places WHERE id = 1`), [][]bigquery.Value{
		{-122.35, 47.62, "LINESTRING(0 0, 132 0)", "POINT(10 10)"},
	}; !reflect.DeepEqual(got, want) {
		t.Errorf("query = %v, want %v", got, want)
	}

	// A counterclockwise shell encloses the square degree on its left.
	inserter.SkipInvalidRows = false
	if err := inserter.Put(ctx, []*bigquery.ValuesSaver{
		row(int64(7), "POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))", nil, nil),
	}); err != nil {
		t.Fatal(err)
	}
	if got, want := p.read("SELECT ST_AREA(g) < 1e11 FROM places WHERE id = 7"),
		[][]bigquery.Value{{true}}; !reflect.DeepEqual(got, want) {
		t.Errorf("polygon query = %v, want %v", got, want)
	}
}
