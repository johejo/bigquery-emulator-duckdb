package goclient

import (
	"cmp"
	"encoding/hex"
	"errors"
	"reflect"
	"slices"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/memory"
	"google.golang.org/api/iterator"
)

// BigQuery's WKT for the full polygon, as checked in scalars/geography_full.txt.
const fullGeographyWkt = "GEOMETRYCOLLECTION(POLYGON((0 0, 120 0, -120 0, 0 0)), " +
	"POLYGON((0 0, -120 0, 120 0, 0 0)))"

// The WKB of POINT(2 4), from ST_GEOGFROM's documentation, and of POLYGON((0 0, 0 1, 1 0, 0 0)),
// whose shell runs clockwise.
const (
	pointWkb     = "010100000000000000000000400000000000001040"
	clockwiseWkb = "01030000000100000004000000000000000000000000000000000000000000000000000000000000000000f03f" +
		"000000000000f03f000000000000000000000000000000000000000000000000"
)

// A table with GEOGRAPHY columns, also REPEATED and in a RECORD, takes WKT, GeoJSON and WKB from
// tabledata.insertAll and loads, and GEOGRAPHY values from queries and DML. tabledata.list
// returns each as WKT, and queries read the columns as GEOGRAPHY. The WKT and coordinates are
// those of scalars/geography.txt and scalars/geography_linestring.txt.
func TestGeographyColumns(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := newDataset(t, client, "go_geography_columns")
	p := datasetQueries{t: t, client: client, dataset: dataset}
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
	// Each row with a value that is neither WKT nor GeoJSON fails; a WKT polygon whose shell runs
	// clockwise is unsupported, since BigQuery takes it as the region on its left:
	// https://cloud.google.com/bigquery/docs/geospatial-data#polygon_orientation
	var rowErrors bigquery.PutMultiError
	inserter.SkipInvalidRows = true
	err := inserter.Put(ctx, []*bigquery.ValuesSaver{
		row(int64(2), "POINT(1", nil, nil),
		row(int64(8), "POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))", nil, nil),
	})
	if !errors.As(err, &rowErrors) || len(rowErrors) != 2 ||
		!strings.Contains(rowErrors[1].Error(), "whose shell runs clockwise") {
		t.Fatalf("invalid rows: %v, want an error for each, for a clockwise shell", err)
	}

	// Both also take GeoJSON geometries and hex-encoded WKB, whose polygons are oriented as WKT's
	// are; POINT(2 4) is ST_GEOGFROM's example:
	// https://cloud.google.com/bigquery/docs/geospatial-data#loading_data
	// https://cloud.google.com/bigquery/docs/reference/standard-sql/geography_functions#st_geogfrom
	if err := inserter.Put(ctx, []*bigquery.ValuesSaver{
		row(int64(3), `{"type": "Point", "coordinates": [1, 2]}`, nil, nil),
		row(int64(13), pointWkb, nil, nil),
	}); err != nil {
		t.Fatal(err)
	}
	err = inserter.Put(ctx, []*bigquery.ValuesSaver{row(int64(8), clockwiseWkb, nil, nil)})
	if !errors.As(err, &rowErrors) || len(rowErrors) != 1 ||
		!strings.Contains(rowErrors[0].Error(), "whose shell runs clockwise") {
		t.Fatalf("clockwise WKB: %v, want an error for a clockwise shell", err)
	}
	if err := p.load("places", `{"id": 4, "g": "POINT(5 6)", "r": {"g": null}}`+"\n"+
		`{"id": 12, "g": "{\"type\": \"Point\", \"coordinates\": [3, 4]}"}`+"\n"+
		`{"id": 14, "g": "`+pointWkb+`"}`+"\n", schema,
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
		{int64(3), "POINT(1 2)", empty, nil},
		{int64(4), "POINT(5 6)", empty, []bigquery.Value{nil}},
		{int64(5), "POINT(170 10)", empty, nil},
		{int64(6), "POINT(12 13)", []bigquery.Value{"POINT(5 6)"}, nil},
		{int64(12), "POINT(3 4)", empty, nil},
		{int64(13), "POINT(2 4)", empty, nil},
		{int64(14), "POINT(2 4)", empty, nil},
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

	// BigQuery writes the full polygon as these two hemispheres; both that WKT and FULLGLOBE
	// must retain the full region through ingestion, storage and query destinations.
	if err := inserter.Put(ctx, []*bigquery.ValuesSaver{
		row(int64(9), "fullglobe", []bigquery.Value{fullGeographyWkt}, []bigquery.Value{"fullglobe"}),
	}); err != nil {
		t.Fatal(err)
	}
	if err := p.load("places", `{"id": 10, "g": "`+fullGeographyWkt+`"}`+"\n", schema,
		bigquery.WriteAppend); err != nil {
		t.Fatal(err)
	}
	if err := p.query("SELECT 11 AS id, ST_GEOGFROMTEXT('fullglobe') AS g",
		"places", bigquery.WriteAppend); err != nil {
		t.Fatal(err)
	}
	if got, want := p.read(`SELECT id, ST_ISCLOSED(g), ST_CONTAINS(g, ST_GEOGPOINT(0, -90))
		FROM places WHERE id BETWEEN 9 AND 11 ORDER BY id`), [][]bigquery.Value{
		{int64(9), true, true}, {int64(10), true, true}, {int64(11), true, true},
	}; !reflect.DeepEqual(got, want) {
		t.Errorf("stored full polygons = %v, want %v", got, want)
	}
	if got, want := p.read(`SELECT ST_ISCLOSED(gs[OFFSET(0)]), ST_ISCLOSED(r.g)
		FROM places WHERE id = 9`), [][]bigquery.Value{{true, true}}; !reflect.DeepEqual(got, want) {
		t.Errorf("nested full polygons = %v, want %v", got, want)
	}
}

// A Parquet load reads a binary column as WKB into a GEOGRAPHY field, also REPEATED:
// https://cloud.google.com/bigquery/docs/geospatial-data#loading_data
func TestGeographyParquetWkb(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := newDataset(t, client, "go_geography_parquet_wkb")

	point, err := hex.DecodeString(pointWkb)
	if err != nil {
		t.Fatal(err)
	}
	allocator := memory.NewGoAllocator()
	ids := array.NewInt64Builder(allocator)
	ids.AppendValues([]int64{1, 2}, nil)
	values := array.NewBinaryBuilder(allocator, arrow.BinaryTypes.Binary)
	values.Append(point)
	values.AppendNull()
	lists := array.NewListBuilder(allocator, arrow.BinaryTypes.Binary)
	elements := lists.ValueBuilder().(*array.BinaryBuilder)
	lists.Append(true)
	elements.Append(point)
	lists.Append(true)
	source := writeParquet(t, []arrow.Field{
		{Name: "id", Type: arrow.PrimitiveTypes.Int64},
		{Name: "g", Type: arrow.BinaryTypes.Binary, Nullable: true},
		{Name: "gs", Type: arrow.ListOf(arrow.BinaryTypes.Binary), Nullable: true},
	}, []arrow.Array{ids.NewArray(), values.NewArray(), lists.NewArray()})
	schema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "g", Type: bigquery.GeographyFieldType},
		{Name: "gs", Type: bigquery.GeographyFieldType, Repeated: true},
	}
	if err := loadParquet(ctx, dataset.Table("places"), source, schema); err != nil {
		t.Fatalf("load: %v", err)
	}
	p := datasetQueries{t: t, client: client, dataset: dataset}
	if got, want := p.read(`SELECT id, ST_ASTEXT(g), ARRAY(SELECT ST_ASTEXT(e) FROM UNNEST(gs) AS e)
		FROM places ORDER BY id`), [][]bigquery.Value{
		{int64(1), "POINT(2 4)", []bigquery.Value{"POINT(2 4)"}},
		{int64(2), nil, []bigquery.Value(nil)},
	}; !reflect.DeepEqual(got, want) {
		t.Errorf("loaded = %v, want %v", got, want)
	}
}
