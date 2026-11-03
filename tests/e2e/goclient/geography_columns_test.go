package goclient

import (
	"cmp"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/memory"
	"github.com/apache/arrow-go/v18/parquet"
	"github.com/apache/arrow-go/v18/parquet/file"
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
	// Invalid text fails; a clockwise WKT shell encloses the region on its left:
	// third_party/bigquery_docs/geospatial-data.md
	var rowErrors bigquery.PutMultiError
	inserter.SkipInvalidRows = true
	err := inserter.Put(ctx, []*bigquery.ValuesSaver{
		row(int64(2), "POINT(1", nil, nil),
		row(int64(8), "POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))", nil, nil),
	})
	if !errors.As(err, &rowErrors) || len(rowErrors) != 1 || rowErrors[0].RowIndex != 0 {
		t.Fatalf("invalid rows: %v, want an error only for the malformed point", err)
	}

	// Both also take GeoJSON geometries and hex-encoded WKB, whose polygons are oriented as WKT's
	// are; POINT(2 4) is ST_GEOGFROM's example:
	// third_party/bigquery_docs/geospatial-data.md
	// third_party/bigquery_docs/reference/standard-sql/geography_functions.md
	if err := inserter.Put(ctx, []*bigquery.ValuesSaver{
		row(int64(3), `{"type": "Point", "coordinates": [1, 2]}`, nil, nil),
		row(int64(13), pointWkb, nil, nil),
	}); err != nil {
		t.Fatal(err)
	}
	if err := inserter.Put(ctx, []*bigquery.ValuesSaver{row(int64(15), clockwiseWkb, nil, nil)}); err != nil {
		t.Fatal(err)
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
		{int64(8), "POLYGON((0 0, 0 1, 1 1, 1 0, 0 0))", empty, nil},
		{int64(12), "POINT(3 4)", empty, nil},
		{int64(13), "POINT(2 4)", empty, nil},
		{int64(14), "POINT(2 4)", empty, nil},
		{int64(15), "POLYGON((0 0, 0 1, 1 0, 0 0))", empty, nil},
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
// third_party/bigquery_docs/geospatial-data.md
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

// A Parquet extract writes GEOGRAPHY columns as BYTE_ARRAY with GeoParquet metadata whose edges
// are spherical, and a load of the file reads them back into GEOGRAPHY columns by default:
// third_party/bigquery_docs/exporting-data.md
// third_party/bigquery_docs/geospatial-data.md
// The long constant-latitude line stays a single great-circle arc only with spherical edges.
func TestGeographyParquetExtract(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := newDataset(t, client, "go_geography_parquet_extract")
	p := datasetQueries{t: t, client: client, dataset: dataset}
	p.mustRun(`CREATE TABLE places AS SELECT * FROM UNNEST([
	  STRUCT(1 AS id, ST_GEOGPOINT(2, 4) AS g, ST_GEOGPOINT(5, 6) AS h),
	  (2, ST_GEOGFROMTEXT('LINESTRING(-60 60, 60 60)'), NULL),
	  (3, ST_GEOGFROMTEXT('POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))'), NULL),
	  (4, NULL, NULL)])`)

	extracted := filepath.Join(t.TempDir(), "places.parquet")
	destination := bigquery.NewGCSReference(extracted)
	destination.DestinationFormat = bigquery.Parquet
	if err := wait(ctx, dataset.Table("places").ExtractorTo(destination)); err != nil {
		t.Fatalf("extract: %v", err)
	}
	reader, err := file.OpenParquetFile(extracted, false)
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	columns := reader.MetaData().Schema
	for _, name := range []string{"g", "h"} {
		column := columns.Column(columns.ColumnIndexByName(name))
		if column.PhysicalType() != parquet.Types.ByteArray {
			t.Errorf("%s is %s, want BYTE_ARRAY", name, column.PhysicalType())
		}
	}
	geo := reader.MetaData().KeyValueMetadata().FindValue("geo")
	if geo == nil {
		t.Fatal("no GeoParquet metadata")
	}
	var metadata struct {
		Columns map[string]struct {
			Encoding string `json:"encoding"`
			Edges    string `json:"edges"`
		} `json:"columns"`
	}
	if err := json.Unmarshal([]byte(*geo), &metadata); err != nil {
		t.Fatalf("GeoParquet metadata %s: %v", *geo, err)
	}
	for _, name := range []string{"g", "h"} {
		if column, ok := metadata.Columns[name]; !ok || column.Encoding != "WKB" || column.Edges != "spherical" {
			t.Errorf("GeoParquet metadata %s: want column %s in WKB with spherical edges", *geo, name)
		}
	}
	if _, ok := metadata.Columns["id"]; ok || len(metadata.Columns) != 2 {
		t.Errorf("GeoParquet metadata %s: want only columns g and h", *geo)
	}

	if err := loadParquet(ctx, dataset.Table("loaded"), extracted, nil); err != nil {
		t.Fatalf("load: %v", err)
	}
	table, err := dataset.Table("loaded").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, field := range table.Schema {
		if (field.Name == "g" || field.Name == "h") && field.Type != bigquery.GeographyFieldType {
			t.Errorf("loaded %s as %s, want GEOGRAPHY", field.Name, field.Type)
		}
	}
	if got, want := p.read(`SELECT id, IFNULL(ST_EQUALS(l.g, p.g), l.g IS NULL AND p.g IS NULL),
		IFNULL(ST_EQUALS(l.h, p.h), l.h IS NULL AND p.h IS NULL)
		FROM places AS p JOIN loaded AS l USING (id) ORDER BY id`), [][]bigquery.Value{
		{int64(1), true, true},
		{int64(2), true, true},
		{int64(3), true, true},
		{int64(4), true, true},
	}; !reflect.DeepEqual(got, want) {
		t.Errorf("loaded = %v, want each value as extracted", got)
	}
}

// CSV and JSON extracts write each GEOGRAPHY, also REPEATED and in a RECORD, as the WKT that
// tabledata.list returns, and a Parquet extract fails on a REPEATED GEOGRAPHY and on a GEOGRAPHY
// in a RECORD. The expected values were recorded on BigQuery in project johejo-sandbox, runs
// xte2ufb5rlbgmlndmar3ujkli3 and h4punfth2ppvhtlpxwcv4kt4e6, from cases of a scenario since
// resolved.
func TestGeographyTextExtract(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := newDataset(t, client, "go_geography_text_extract")
	p := datasetQueries{t: t, client: client, dataset: dataset}
	p.mustRun(`CREATE TABLE places AS SELECT * FROM UNNEST([
	  STRUCT(1 AS id, ST_GEOGPOINT(-122.35, 47.62) AS g),
	  (2, ST_GEOGFROMTEXT('LINESTRING(0 0, 132 0)')),
	  (3, ST_GEOGFROMTEXT('POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))')),
	  (4, ST_GEOGFROMTEXT('MULTIPOINT(1 2, 3 4)')),
	  (5, ST_GEOGFROMTEXT('GEOMETRYCOLLECTION(POINT(5 5), LINESTRING(0 0, 1 1))')),
	  (6, ST_GEOGFROMTEXT('POINT EMPTY')),
	  (7, NULL)])`)
	p.mustRun(`CREATE TABLE nested AS SELECT 1 AS id, [ST_GEOGPOINT(1, 2), ST_GEOGPOINT(3, 4)] AS gs,
	  STRUCT(ST_GEOGPOINT(5, 6) AS g) AS r`)
	extract := func(table string, format bigquery.DataFormat) ([]string, error) {
		path := filepath.Join(t.TempDir(), table)
		destination := bigquery.NewGCSReference(path)
		destination.DestinationFormat = format
		if err := wait(ctx, dataset.Table(table).ExtractorTo(destination)); err != nil {
			return nil, err
		}
		contents, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		// BigQuery's lines were observed sorted.
		lines := strings.Split(strings.TrimSuffix(string(contents), "\n"), "\n")
		slices.Sort(lines)
		return lines, nil
	}

	for _, c := range []struct {
		table  string
		format bigquery.DataFormat
		want   []string
	}{
		{"places", bigquery.CSV, []string{
			`1,POINT(-122.35 47.62)`,
			`2,"LINESTRING(0 0, 132 0)"`,
			`3,"POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))"`,
			`4,"MULTIPOINT(1 2, 3 4)"`,
			`5,"GEOMETRYCOLLECTION(POINT(5 5), LINESTRING(0 0, 1 1))"`,
			`6,GEOMETRYCOLLECTION EMPTY`,
			`7,`,
			`id,g`,
		}},
		{"places", bigquery.JSON, []string{
			`{"id":"1","g":"POINT(-122.35 47.62)"}`,
			`{"id":"2","g":"LINESTRING(0 0, 132 0)"}`,
			`{"id":"3","g":"POLYGON((0 0, 1 0, 1 1, 0 1, 0 0))"}`,
			`{"id":"4","g":"MULTIPOINT(1 2, 3 4)"}`,
			`{"id":"5","g":"GEOMETRYCOLLECTION(POINT(5 5), LINESTRING(0 0, 1 1))"}`,
			`{"id":"6","g":"GEOMETRYCOLLECTION EMPTY"}`,
			`{"id":"7"}`,
		}},
		{"nested", bigquery.JSON, []string{
			`{"id":"1","gs":["POINT(1 2)","POINT(3 4)"],"r":{"g":"POINT(5 6)"}}`,
		}},
	} {
		got, err := extract(c.table, c.format)
		if err != nil {
			t.Errorf("%s to %s: %v", c.table, c.format, err)
		} else if !reflect.DeepEqual(got, c.want) {
			t.Errorf("%s to %s: got %q, want %q", c.table, c.format, got, c.want)
		}
	}

	// The first column GeoParquet cannot describe fails the extract.
	p.mustRun("CREATE TABLE record AS SELECT 1 AS id, STRUCT(ST_GEOGPOINT(5, 6) AS g) AS r")
	for table, column := range map[string]string{"nested": "Repeated column 'gs'", "record": "Nested column 'r.g'"} {
		message := "Error while reading data, error message: " + column + " with GEOGRAPHY " +
			"type cannot be exported to Parquet. GeoParquet only supports non-repeated root level columns"
		if _, err := extract(table, bigquery.Parquet); err == nil || !strings.Contains(err.Error(), message) {
			t.Errorf("%s to Parquet: got %v, want an error containing %q", table, err, message)
		}
	}
}
