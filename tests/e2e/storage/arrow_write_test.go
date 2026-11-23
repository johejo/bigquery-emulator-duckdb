package storage

import (
	"bytes"
	"fmt"
	"math"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/memory"
	"github.com/johejo/bigquery-emulator-duckdb/internal/arrowtest"
	"google.golang.org/grpc/codes"
)

// The Storage Write documentation shows Arrow IPC schemas and batches on the
// default stream with Int64 and Utf8 fields (write-api-streaming.md).
func TestAppendArrow(t *testing.T) {
	t.Parallel()
	rest, _, write := clients(t)
	const dataset = "storage_write_arrow"
	path := table(t, rest, dataset, "id INT64 NOT NULL, name STRING")
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	builder := array.NewRecordBuilder(memory.DefaultAllocator, arrow.NewSchema([]arrow.Field{
		{Name: "name", Type: arrow.BinaryTypes.String, Nullable: true},
		{Name: "id", Type: arrow.PrimitiveTypes.Int64},
	}, nil))
	defer builder.Release()
	encode := func(ids []int64, names []string, valid []bool) ([]byte, []byte) {
		t.Helper()
		builder.Field(0).(*array.StringBuilder).AppendValues(names, valid)
		builder.Field(1).(*array.Int64Builder).AppendValues(ids, nil)
		record := builder.NewRecordBatch()
		defer record.Release()
		schema, batch, err := arrowtest.Encode(record)
		if err != nil {
			t.Fatal(err)
		}
		return schema, batch
	}
	request := func(path string, schema, batch []byte) *storagepb.AppendRowsRequest {
		rows := &storagepb.AppendRowsRequest_ArrowData{Rows: &storagepb.ArrowRecordBatch{SerializedRecordBatch: batch}}
		if schema != nil {
			rows.WriterSchema = &storagepb.ArrowSchema{SerializedSchema: schema}
		}
		return &storagepb.AppendRowsRequest{WriteStream: path, Rows: &storagepb.AppendRowsRequest_ArrowRows{ArrowRows: rows}}
	}
	firstSchema, firstBatch := encode([]int64{math.MinInt64, 0}, []string{"日本語", ""}, []bool{true, false})
	if r := send(t, stream, request(path+"/streams/_default", firstSchema, firstBatch)); r.GetAppendResult() == nil {
		t.Fatalf("first append: %v", r)
	}
	_, nextBatch := encode([]int64{math.MaxInt64}, []string{""}, nil)
	if r := send(t, stream, request("", nil, nextBatch[:len(nextBatch)-1])); codes.Code(r.GetError().GetCode()) != codes.InvalidArgument {
		t.Fatalf("truncated batch: %v", r)
	}
	if r := send(t, stream, request("", nil, nextBatch)); r.GetAppendResult() == nil {
		t.Fatalf("schema reuse: %v", r)
	}
	closeAppend(t, stream)
	wantVisible(t, rest, dataset, math.MinInt64, 0, math.MaxInt64)
	iter, err := rest.Query("SELECT name FROM " + dataset + ".rows ORDER BY id").Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []bigquery.Value{"日本語", nil, ""} {
		var got []bigquery.Value
		if err := iter.Next(&got); err != nil {
			t.Fatal(err)
		}
		if got[0] != want {
			t.Errorf("name = %#v, want %#v", got[0], want)
		}
	}
}

// assertArrowWrite publishes one batch to the default stream and checks the SQL values
// rather than the separate Storage Read Arrow representation.
func assertArrowWrite(t *testing.T, dataset, columns string, fields []arrow.Field, rows [][]string, checks []string) {
	t.Helper()
	rest, _, write := clients(t)
	path := table(t, rest, dataset, columns)
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	builder := array.NewRecordBuilder(memory.DefaultAllocator, arrow.NewSchema(fields, nil))
	defer builder.Release()
	for _, row := range rows {
		for i, value := range row {
			if err := builder.Field(i).AppendValueFromString(value); err != nil {
				t.Fatalf("encode column %s: %v", fields[i].Name, err)
			}
		}
	}
	record := builder.NewRecordBatch()
	defer record.Release()
	schema, batch, err := arrowtest.Encode(record)
	if err != nil {
		t.Fatal(err)
	}
	request := &storagepb.AppendRowsRequest{WriteStream: path + "/streams/_default",
		Rows: &storagepb.AppendRowsRequest_ArrowRows{ArrowRows: &storagepb.AppendRowsRequest_ArrowData{
			WriterSchema: &storagepb.ArrowSchema{SerializedSchema: schema},
			Rows:         &storagepb.ArrowRecordBatch{SerializedRecordBatch: batch},
		}}}
	if r := send(t, stream, request); r.GetAppendResult() == nil {
		t.Fatalf("append: %v", r)
	}
	closeAppend(t, stream)
	var aggregates []string
	for _, check := range checks {
		aggregates = append(aggregates, fmt.Sprintf("COUNTIF(%s) = COUNT(*)", check))
	}
	query := fmt.Sprintf("SELECT COUNT(*) = %d, %s FROM %s.rows", len(rows), strings.Join(aggregates, ", "), dataset)
	it, err := rest.Query(query).Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var got []bigquery.Value
	if err := it.Next(&got); err != nil {
		t.Fatal(err)
	}
	if got[0] != true {
		t.Errorf("wrong row count for %s", dataset)
	}
	for i, check := range checks {
		if got[i+1] != true {
			t.Errorf("published rows did not satisfy %s", check)
		}
	}
}

// The remaining scalar wire types and timestamp parameters are documented in
// supported-data-types.md. Pre-epoch timestamps detect truncation toward epoch.
func TestAppendArrowTypedScalars(t *testing.T) {
	t.Parallel()
	assertArrowWrite(t, "storage_write_arrow_typed", `i INT64, d DATE, di DATE, ds DATE,
		dt DATETIME, dt_min DATETIME, ts TIMESTAMP, n NUMERIC, bn BIGNUMERIC, g GEOGRAPHY, j JSON`, []arrow.Field{
		{Name: "i", Type: arrow.PrimitiveTypes.Uint64},
		{Name: "d", Type: arrow.FixedWidthTypes.Date32},
		{Name: "di", Type: arrow.PrimitiveTypes.Int32},
		{Name: "ds", Type: arrow.BinaryTypes.String},
		{Name: "dt", Type: &arrow.TimestampType{Unit: arrow.Microsecond}},
		{Name: "dt_min", Type: &arrow.TimestampType{Unit: arrow.Microsecond}},
		{Name: "ts", Type: &arrow.TimestampType{Unit: arrow.Microsecond, TimeZone: "UTC"}},
		{Name: "n", Type: &arrow.Decimal128Type{Precision: 38, Scale: 9}},
		{Name: "bn", Type: &arrow.Decimal256Type{Precision: 76, Scale: 38}},
		{Name: "g", Type: arrow.BinaryTypes.String},
		{Name: "j", Type: arrow.BinaryTypes.String},
	}, [][]string{{
		"9223372036854775807", "0001-01-01", "-1", "2020-02-29", "1969-12-31T23:59:59.876543",
		"0001-01-01T00:00:00", "1969-12-31T23:59:59.999999Z", "-123456789012345678901.123456789",
		"0.00000000000000000000000000000000000001", `{"type":"Point","coordinates":[-11,43]}`, "null",
	}}, []string{
		"i = 9223372036854775807", "d = DATE '0001-01-01'", "di = DATE '1969-12-31'", "ds = DATE '2020-02-29'",
		"dt = DATETIME '1969-12-31 23:59:59.876543'", "dt_min = DATETIME '0001-01-01 00:00:00'",
		"ts = TIMESTAMP '1969-12-31 23:59:59.999999+00'",
		"n = NUMERIC '-123456789012345678901.123456789'", "bn = BIGNUMERIC '0.00000000000000000000000000000000000001'",
		"ST_EQUALS(g, ST_GEOGPOINT(-11, 43))", "j IS NOT NULL AND JSON_TYPE(j) = 'null'",
	})
}

// The supported-data-types table specifies the three Arrow interval units and UTF-8.
func TestAppendArrowIntervals(t *testing.T) {
	t.Parallel()
	assertArrowWrite(t, "storage_write_arrow_intervals", "ym INTERVAL, dd INTERVAL, mdn INTERVAL, text INTERVAL", []arrow.Field{
		{Name: "ym", Type: arrow.FixedWidthTypes.MonthInterval},
		{Name: "dd", Type: arrow.FixedWidthTypes.DayTimeInterval},
		{Name: "mdn", Type: arrow.FixedWidthTypes.MonthDayNanoInterval},
		{Name: "text", Type: arrow.BinaryTypes.String},
	}, [][]string{{"-13", `{"Days":2,"Milliseconds":-1723}`, `{"Months":-3,"Days":7,"Nanoseconds":-54321001}`, "2-3 4 5:6:7.890123"}}, []string{
		"EXTRACT(YEAR FROM ym) = -1 AND EXTRACT(MONTH FROM ym) = -1",
		"EXTRACT(DAY FROM dd) = 2 AND EXTRACT(SECOND FROM dd) = -1 AND EXTRACT(MICROSECOND FROM dd) = -723000",
		"EXTRACT(MONTH FROM mdn) = -3 AND EXTRACT(DAY FROM mdn) = 7 AND EXTRACT(MICROSECOND FROM mdn) = -54321",
		"mdn < INTERVAL '-0-3 7 -0:0:0.054321' YEAR TO SECOND",
		"text = INTERVAL '2-3 4 5:6:7.890123' YEAR TO SECOND",
	})
}

// List fields use an empty list for missing values; a null RANGE bound is UNBOUNDED
// (supported-data-types.md). Nested Arrow fields are bound by name, not position.
func TestAppendArrowNested(t *testing.T) {
	t.Parallel()
	item := arrow.StructOf(
		arrow.Field{Name: "tags", Type: arrow.ListOf(arrow.BinaryTypes.String)},
		arrow.Field{Name: "id", Type: arrow.PrimitiveTypes.Int64},
	)
	bounds := func(typ arrow.DataType) *arrow.StructType {
		return arrow.StructOf(arrow.Field{Name: "start", Type: typ, Nullable: true},
			arrow.Field{Name: "end", Type: typ, Nullable: true})
	}
	assertArrowWrite(t, "storage_write_arrow_nested", `items ARRAY<STRUCT<id INT64, tags ARRAY<STRING>>>,
		rd RANGE<DATE>, rdt RANGE<DATETIME>, rts RANGE<TIMESTAMP>`, []arrow.Field{
		{Name: "items", Type: arrow.ListOf(item)},
		{Name: "rd", Type: bounds(arrow.FixedWidthTypes.Date32)},
		{Name: "rdt", Type: bounds(&arrow.TimestampType{Unit: arrow.Microsecond})},
		{Name: "rts", Type: bounds(&arrow.TimestampType{Unit: arrow.Microsecond, TimeZone: "UTC"})},
	}, [][]string{{`[{"tags":["雪",""],"id":8},{"tags":[],"id":9}]`, `{"start":null,"end":"2001-12-31"}`,
		`{"start":"2012-02-29T22:01:02.123456","end":null}`, `{"start":null,"end":null}`}}, []string{
		"ARRAY_LENGTH(items) = 2 AND items[OFFSET(0)].id = 8 AND items[OFFSET(0)].tags[OFFSET(0)] = '雪'",
		"items[OFFSET(0)].tags[OFFSET(1)] = '' AND items[OFFSET(1)].id = 9 AND ARRAY_LENGTH(items[OFFSET(1)].tags) = 0",
		"rd IS NOT NULL AND RANGE_START(rd) IS NULL AND RANGE_END(rd) = DATE '2001-12-31'",
		"RANGE_START(rdt) = DATETIME '2012-02-29 22:01:02.123456' AND RANGE_END(rdt) IS NULL",
		"rts IS NOT NULL AND RANGE_START(rts) IS NULL AND RANGE_END(rts) IS NULL",
	})
}

// The Storage Write type table (supported-data-types.md) maps Boolean to BOOL,
// Binary to BYTES, and both SINGLE and DOUBLE to FLOAT64.
func TestAppendArrowScalarTypes(t *testing.T) {
	t.Parallel()
	rest, _, write := clients(t)
	const dataset = "storage_write_arrow_scalars"
	path := table(t, rest, dataset, "flag BOOL, raw BYTES, single FLOAT64, wide FLOAT64")
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	builder := array.NewRecordBuilder(memory.DefaultAllocator, arrow.NewSchema([]arrow.Field{
		{Name: "flag", Type: arrow.FixedWidthTypes.Boolean, Nullable: true},
		{Name: "raw", Type: arrow.BinaryTypes.Binary, Nullable: true},
		{Name: "single", Type: arrow.PrimitiveTypes.Float32, Nullable: true},
		{Name: "wide", Type: arrow.PrimitiveTypes.Float64, Nullable: true},
	}, nil))
	defer builder.Release()
	builder.Field(0).(*array.BooleanBuilder).AppendValues([]bool{true, false, false}, []bool{true, true, false})
	raw := []byte{0, 0xff, 0x80}
	builder.Field(1).(*array.BinaryBuilder).AppendValues([][]byte{raw, {}, nil}, []bool{true, true, false})
	builder.Field(2).(*array.Float32Builder).AppendValues([]float32{-1.25, 0, 0}, []bool{true, true, false})
	builder.Field(3).(*array.Float64Builder).AppendValues([]float64{1 + math.Ldexp(1, -40), 2.5, 0}, []bool{true, true, false})
	record := builder.NewRecordBatch()
	defer record.Release()
	schema, batch, err := arrowtest.Encode(record)
	if err != nil {
		t.Fatal(err)
	}
	request := &storagepb.AppendRowsRequest{WriteStream: path + "/streams/_default",
		Rows: &storagepb.AppendRowsRequest_ArrowRows{ArrowRows: &storagepb.AppendRowsRequest_ArrowData{
			WriterSchema: &storagepb.ArrowSchema{SerializedSchema: schema},
			Rows:         &storagepb.ArrowRecordBatch{SerializedRecordBatch: batch},
		}}}
	if r := send(t, stream, request); r.GetAppendResult() == nil {
		t.Fatalf("append: %v", r)
	}
	closeAppend(t, stream)
	iter, err := rest.Query("SELECT flag, raw, single, wide FROM " + dataset + ".rows ORDER BY flag DESC NULLS LAST").Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range [][]bigquery.Value{
		{true, raw, -1.25, 1 + math.Ldexp(1, -40)},
		{false, []byte{}, float64(0), 2.5},
		{nil, nil, nil, nil},
	} {
		var got []bigquery.Value
		if err := iter.Next(&got); err != nil {
			t.Fatal(err)
		}
		for i, v := range want {
			if b, ok := v.([]byte); ok {
				if g, ok := got[i].([]byte); !ok || !bytes.Equal(g, b) {
					t.Errorf("column %d = %#v, want %#v", i, got[i], v)
				}
			} else if got[i] != v {
				t.Errorf("column %d = %#v, want %#v", i, got[i], v)
			}
		}
	}
}
