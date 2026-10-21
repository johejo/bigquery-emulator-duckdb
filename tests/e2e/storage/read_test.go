package storage

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"math/big"
	"os"
	"reflect"
	"slices"
	"sort"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"cloud.google.com/go/civil"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/ipc"
	"github.com/linkedin/goavro/v2"
	"google.golang.org/api/iterator"
	"google.golang.org/api/option"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
)

func clients(t *testing.T) (*bigquery.Client, storagepb.BigQueryReadClient, storagepb.BigQueryWriteClient) {
	t.Helper()
	endpoint, grpcEndpoint := os.Getenv("BQ_EMULATOR_API"), os.Getenv("BQ_EMULATOR_GRPC")
	if endpoint == "" || grpcEndpoint == "" {
		t.Skip("run just e2e")
	}
	rest, err := bigquery.NewClient(t.Context(), "test", option.WithEndpoint(endpoint), option.WithoutAuthentication())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = rest.Close() })
	conn, err := grpc.NewClient(grpcEndpoint, grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithDefaultCallOptions(grpc.MaxCallRecvMsgSize(129*1024*1024)))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	return rest, storagepb.NewBigQueryReadClient(conn), storagepb.NewBigQueryWriteClient(conn)
}

func query(t *testing.T, client *bigquery.Client, sql string) {
	t.Helper()
	job, err := client.Query(sql).Run(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	state, err := job.Wait(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if err := state.Err(); err != nil {
		t.Fatalf("%s: %v", sql, err)
	}
}

func table(t *testing.T, client *bigquery.Client, dataset, columns string) string {
	t.Helper()
	ds := client.Dataset(dataset)
	if err := ds.Create(t.Context(), nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ds.DeleteWithContents(context.Background()) })
	query(t, client, "CREATE TABLE "+dataset+".rows ("+columns+")")
	return "projects/test/datasets/" + dataset + "/tables/rows"
}

func session(t *testing.T, read storagepb.BigQueryReadClient, table string, format storagepb.DataFormat,
	options *storagepb.ReadSession_TableReadOptions, count int32) *storagepb.ReadSession {
	t.Helper()
	result, err := read.CreateReadSession(t.Context(), &storagepb.CreateReadSessionRequest{
		Parent: "projects/test", MaxStreamCount: count,
		ReadSession: &storagepb.ReadSession{Table: table, DataFormat: format, ReadOptions: options},
	})
	if err != nil {
		t.Fatal(err)
	}
	if result.ExpireTime.AsTime().Before(time.Now().Add(5 * time.Hour)) {
		t.Fatal("session expires too early")
	}
	return result
}

func responses(t *testing.T, read storagepb.BigQueryReadClient, name string, offset int64) []*storagepb.ReadRowsResponse {
	t.Helper()
	stream, err := read.ReadRows(t.Context(), &storagepb.ReadRowsRequest{ReadStream: name, Offset: offset})
	if err != nil {
		t.Fatal(err)
	}
	var result []*storagepb.ReadRowsResponse
	for {
		response, err := stream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		result = append(result, response)
	}
	return result
}

func arrowIDs(t *testing.T, read storagepb.BigQueryReadClient, s *storagepb.ReadSession, name string, offset int64) []int64 {
	t.Helper()
	var ids []int64
	blocks := responses(t, read, name, offset)
	for i, block := range blocks {
		if i == 0 && !bytes.Equal(block.GetArrowSchema().GetSerializedSchema(), s.GetArrowSchema().GetSerializedSchema()) {
			t.Fatal("first response must contain the session schema")
		}
		data := append(bytes.Clone(s.GetArrowSchema().GetSerializedSchema()), block.GetArrowRecordBatch().GetSerializedRecordBatch()...)
		reader, err := ipc.NewReader(bytes.NewReader(data))
		if err != nil {
			t.Fatal(err)
		}
		var n int64
		for reader.Next() {
			record := reader.RecordBatch()
			column := record.Column(0).(*array.Int64)
			for j := 0; j < column.Len(); j++ {
				ids = append(ids, column.Value(j))
			}
			n += record.NumRows()
		}
		if err := reader.Err(); err != nil {
			t.Fatal(err)
		}
		reader.Release()
		if n != block.RowCount {
			t.Fatalf("decoded %d rows, response says %d", n, block.RowCount)
		}
	}
	return ids
}

// The Storage API documents snapshot isolation, disjoint read streams, schema order and
// offset resumption. Expectations here follow those guarantees and the rows we insert.
func TestReadSnapshotProjectionSplitAndOffset(t *testing.T) {
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_snapshot", "id INT64, rec STRUCT<a INT64, b STRING>, records ARRAY<STRUCT<a INT64, b STRING>>")
	query(t, rest, "INSERT INTO storage_read_snapshot.rows SELECT i, STRUCT(i AS a, 'keep' AS b), [STRUCT(i AS a, 'keep' AS b)] FROM UNNEST(GENERATE_ARRAY(1, 9)) AS i")
	s := session(t, read, path, storagepb.DataFormat_ARROW, &storagepb.ReadSession_TableReadOptions{
		SelectedFields: []string{"records.b", "rec.b", "id"}, RowRestriction: "id >= 2",
	}, 3)
	query(t, rest, "DELETE FROM storage_read_snapshot.rows WHERE TRUE")
	var all []int64
	for _, stream := range s.Streams {
		all = append(all, arrowIDs(t, read, s, stream.Name, 0)...)
	}
	sort.Slice(all, func(i, j int) bool { return all[i] < all[j] })
	if !reflect.DeepEqual(all, []int64{2, 3, 4, 5, 6, 7, 8, 9}) {
		t.Fatalf("snapshot: %v", all)
	}
	original := arrowIDs(t, read, s, s.Streams[0].Name, 0)
	resumed := arrowIDs(t, read, s, s.Streams[0].Name, 1)
	if !reflect.DeepEqual(resumed, original[1:]) {
		t.Fatalf("resume: %v, original %v", resumed, original)
	}
	split, err := read.SplitReadStream(t.Context(), &storagepb.SplitReadStreamRequest{Name: s.Streams[0].Name, Fraction: 0.5})
	if err != nil {
		t.Fatal(err)
	}
	children := append(arrowIDs(t, read, s, split.PrimaryStream.Name, 0), arrowIDs(t, read, s, split.RemainderStream.Name, 0)...)
	if !reflect.DeepEqual(children, original) {
		t.Fatalf("split: %v, original %v", children, original)
	}
	first := responses(t, read, s.Streams[0].Name, 0)[0]
	data := append(bytes.Clone(s.GetArrowSchema().SerializedSchema), first.GetArrowRecordBatch().SerializedRecordBatch...)
	reader, err := ipc.NewReader(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Release()
	if !reader.Next() {
		t.Fatal(reader.Err())
	}
	fields := reader.Schema().Fields()
	if fields[0].Name != "id" || fields[1].Name != "rec" || fields[2].Name != "records" {
		t.Fatalf("field order: %v", fields)
	}
	rec := reader.RecordBatch().Column(1).(*array.Struct)
	if rec.NumField() != 1 || rec.Field(0).(*array.String).Value(0) != "keep" {
		t.Fatal("nested projection lost STRUCT shape")
	}
	records := reader.RecordBatch().Column(2).(*array.List).ListValues().(*array.Struct)
	if records.NumField() != 1 || records.Field(0).(*array.String).Value(0) != "keep" {
		t.Fatal("nested repeated projection lost STRUCT shape")
	}
}

// A projection keeps a NULL STRUCT NULL, the order of repeated elements, and returns a NULL
// array as an empty one, as BigQuery stores it.
func TestReadArrowProjectionNullsAndOrder(t *testing.T) {
	t.Parallel()
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_projection_nulls", `id INT64, rec STRUCT<a INT64, b STRING>,
		records ARRAY<STRUCT<a INT64, nested STRUCT<x INT64, y STRING>>>`)
	query(t, rest, `INSERT INTO storage_read_projection_nulls.rows VALUES
		(1, NULL, NULL),
		(2, STRUCT(2, 'b'), [STRUCT(3 AS a, STRUCT(30 AS x, 'p' AS y) AS nested), STRUCT(1, NULL), STRUCT(2, STRUCT(20, 'q'))])`)
	s := session(t, read, path, storagepb.DataFormat_ARROW, &storagepb.ReadSession_TableReadOptions{
		SelectedFields: []string{"rec.b", "records.nested.y", "id"},
	}, 1)
	block := responses(t, read, s.Streams[0].Name, 0)[0]
	data := append(bytes.Clone(s.GetArrowSchema().SerializedSchema), block.GetArrowRecordBatch().SerializedRecordBatch...)
	reader, err := ipc.NewReader(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Release()
	if !reader.Next() {
		t.Fatal(reader.Err())
	}
	r := reader.RecordBatch()
	got := map[int64]string{}
	for row := 0; row < int(r.NumRows()); row++ {
		rec := r.Column(1).(*array.Struct)
		text := "rec=null"
		if !rec.IsNull(row) {
			text = "rec=" + rec.Field(0).(*array.String).Value(row)
		}
		records := r.Column(2).(*array.List)
		if records.IsNull(row) {
			t.Fatalf("row %d: NULL array", row)
		}
		start, end := records.ValueOffsets(row)
		elements := records.ListValues().(*array.Struct)
		inner := elements.Field(0).(*array.Struct)
		text += " records="
		for i := int(start); i < int(end); i++ {
			if elements.NumField() != 1 || inner.NumField() != 1 {
				t.Fatal("nested projection lost STRUCT shape")
			}
			if inner.IsNull(i) {
				text += "[null]"
			} else {
				text += "[" + inner.Field(0).(*array.String).Value(i) + "]"
			}
		}
		got[r.Column(0).(*array.Int64).Value(row)] = text
	}
	want := map[int64]string{1: "rec=null records=", 2: "rec=b records=[p][null][q]"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
}

func union(value any) any {
	if m, ok := value.(map[string]any); ok && len(m) == 1 {
		for _, value := range m {
			return value
		}
	}
	return value
}

func TestReadAvroTypes(t *testing.T) {
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_avro", "id INT64, name STRING, payload BYTES, n NUMERIC, bn BIGNUMERIC, tags ARRAY<STRING>, rec STRUCT<a INT64, b STRING>, ts TIMESTAMP, d DATE, tm TIME, dt DATETIME")
	query(t, rest, `INSERT INTO storage_read_avro.rows VALUES (-1, '日本語', b'\x00\xff', NUMERIC '-123456789.123456789', BIGNUMERIC '-12345678901234567890123456789012345678.12345678901234567890123456789012345678', ['a','b'], STRUCT(1,'nested'), TIMESTAMP '1969-12-31 23:59:59.999999+00', DATE '1969-12-31', TIME '23:59:59.999999', DATETIME '1969-12-31 23:59:59.999999'), (2, NULL, b'', 0, 0, [], NULL, NULL, NULL, NULL, NULL)`)
	s := session(t, read, path, storagepb.DataFormat_AVRO, nil, 1)
	// The official Storage schema mapping annotates DATETIME as string/datetime.
	var schema struct {
		Fields []struct {
			Name string
			Type json.RawMessage
		}
	}
	if err := json.Unmarshal([]byte(s.GetAvroSchema().Schema), &schema); err != nil {
		t.Fatal(err)
	}
	for _, field := range schema.Fields {
		if field.Name == "dt" {
			var branches []json.RawMessage
			if err := json.Unmarshal(field.Type, &branches); err != nil {
				t.Fatal(err)
			}
			var kind struct{ Type, LogicalType string }
			if err := json.Unmarshal(branches[1], &kind); err != nil {
				t.Fatal(err)
			}
			if kind.Type != "string" || kind.LogicalType != "datetime" {
				t.Fatalf("DATETIME schema: %s", branches[1])
			}
		}
	}
	codec, err := goavro.NewCodec(s.GetAvroSchema().Schema)
	if err != nil {
		t.Fatal(err)
	}
	values := make(map[int64]map[string]any)
	for _, block := range responses(t, read, s.Streams[0].Name, 0) {
		data := block.GetAvroRows().SerializedBinaryRows
		for range block.RowCount {
			value, remaining, err := codec.NativeFromBinary(data)
			if err != nil {
				t.Fatal(err)
			}
			row := value.(map[string]any)
			values[union(row["id"]).(int64)] = row
			data = remaining
		}
		if len(data) != 0 {
			t.Fatal("unexpected trailing Avro bytes")
		}
	}
	row := values[-1]
	if union(row["name"]) != "日本語" || !bytes.Equal(union(row["payload"]).([]byte), []byte{0, 255}) {
		t.Fatalf("text/bytes: %v", row)
	}
	for name, want := range map[string]string{"n": "-123456789.123456789", "bn": "-12345678901234567890123456789012345678.12345678901234567890123456789012345678"} {
		expected, _ := new(big.Rat).SetString(want)
		if union(row[name]).(*big.Rat).Cmp(expected) != 0 {
			t.Fatalf("%s: %v", name, row[name])
		}
	}
	if union(row["ts"]).(time.Time).UnixMicro() != -1 || union(row["d"]).(time.Time).Format("2006-01-02") != "1969-12-31" || union(row["dt"]) != "1969-12-31 23:59:59.999999" {
		t.Fatalf("temporal values: %v", row)
	}
	if values[2]["name"] != nil || values[2]["rec"] != nil || len(values[2]["tags"].([]any)) != 0 {
		t.Fatalf("NULL/empty: %v", values[2])
	}
}

func TestGoClientStorageRead(t *testing.T) {
	rest, _, _ := clients(t)
	table(t, rest, "storage_read_goclient", "id INT64, n NUMERIC, bn BIGNUMERIC, ts TIMESTAMP, d DATE, tm TIME, dt DATETIME, rec STRUCT<a INT64, b STRING>, tags ARRAY<STRING>")
	query(t, rest, `INSERT INTO storage_read_goclient.rows VALUES (1, NUMERIC '-1.000000001', BIGNUMERIC '0.00000000000000000000000000000000000001', TIMESTAMP '1969-12-31 23:59:59.999999+00', DATE '1969-12-31', TIME '23:59:59.999999', DATETIME '1969-12-31 23:59:59.999999', STRUCT(1, 'nested'), ['a','b'])`)
	if err := rest.EnableStorageReadClient(t.Context(), option.WithEndpoint(os.Getenv("BQ_EMULATOR_GRPC")), option.WithoutAuthentication(), option.WithGRPCDialOption(grpc.WithTransportCredentials(insecure.NewCredentials()))); err != nil {
		t.Fatal(err)
	}
	rows := rest.Dataset("storage_read_goclient").Table("rows").Read(t.Context())
	var value []bigquery.Value
	if err := rows.Next(&value); err != nil {
		t.Fatal(err)
	}
	if value[0] != int64(1) || value[1].(*big.Rat).FloatString(9) != "-1.000000001" || value[2].(*big.Rat).FloatString(38) != "0.00000000000000000000000000000000000001" || value[3].(time.Time).UnixMicro() != -1 {
		t.Fatalf("values: %v", value)
	}
	date := civil.Date{Year: 1969, Month: time.December, Day: 31}
	tm := civil.Time{Hour: 23, Minute: 59, Second: 59, Nanosecond: 999999000}
	if value[4] != date || value[5] != tm || value[6] != (civil.DateTime{Date: date, Time: tm}) {
		t.Fatalf("civil times: %v", value)
	}
	if err := rows.Next(&value); err != iterator.Done {
		t.Fatalf("end: %v", err)
	}
}

func TestReadEmptyAndMultipleBatches(t *testing.T) {
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_batches", "id INT64")
	empty := session(t, read, path, storagepb.DataFormat_ARROW, nil, 1)
	if ids := arrowIDs(t, read, empty, empty.Streams[0].Name, 0); len(ids) != 0 {
		t.Fatal(ids)
	}
	query(t, rest, "INSERT INTO storage_read_batches.rows SELECT i FROM UNNEST(GENERATE_ARRAY(1, 2050)) AS i")
	s := session(t, read, path, storagepb.DataFormat_ARROW, nil, 1)
	ids := arrowIDs(t, read, s, s.Streams[0].Name, 0)
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	if len(ids) != 2050 {
		t.Fatalf("row count: %d", len(ids))
	}
	for i, id := range ids {
		if id != int64(i+1) {
			t.Fatalf("row %d: %d", i, id)
		}
	}
	// Resume across both IPC response boundaries and DuckDB vector boundaries.
	original := arrowIDs(t, read, s, s.Streams[0].Name, 0)
	for _, offset := range []int64{1, 1023, 2047, 2050} {
		got := arrowIDs(t, read, s, s.Streams[0].Name, offset)
		if !slices.Equal(got, original[offset:]) {
			t.Fatalf("offset %d: got %v, want %v", offset, got, original[offset:])
		}
	}
}

// Storage Read's documented Arrow types and snapshot isolation determine the expected values:
// https://docs.cloud.google.com/bigquery/docs/reference/storage#arrow_schema_details
func TestReadArrowSnapshotTypesAndConcurrentResume(t *testing.T) {
	t.Parallel()
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_arrow_snapshot_types", `id INT64 NOT NULL, s STRING,
		b BYTES, flag BOOL, f FLOAT64, n NUMERIC, d DATE, tm TIME, dt DATETIME, ts TIMESTAMP,
		rec STRUCT<x INT64, s STRING>`)
	query(t, rest, `INSERT INTO storage_read_arrow_snapshot_types.rows
		SELECT i, IF(MOD(i,5)=0, NULL, CONCAT('日本語', CAST(i AS STRING))), b'\x00\xff',
			MOD(i,2)=0,
			CASE i WHEN 1 THEN CAST('NaN' AS FLOAT64) WHEN 2 THEN CAST('inf' AS FLOAT64)
				WHEN 3 THEN CAST('-inf' AS FLOAT64) WHEN 4 THEN -0.0 ELSE CAST(i AS FLOAT64)/2 END,
			IF(MOD(i,11)=0, NULL, NUMERIC '-1.000000001'),
			CASE i WHEN 1 THEN DATE '0001-01-01' WHEN 4103 THEN DATE '9999-12-31' ELSE DATE '1969-12-31' END,
			TIME '23:59:59.999999', DATETIME '1969-12-31 23:59:59.999999',
			TIMESTAMP '1969-12-31 23:59:59.999999+00',
			IF(MOD(i,7)=0, NULL, STRUCT(i AS x, CAST(i AS STRING) AS s))
		FROM UNNEST(GENERATE_ARRAY(1,4103)) AS i`)
	s := session(t, read, path, storagepb.DataFormat_ARROW, nil, 3)
	query(t, rest, "DELETE FROM storage_read_arrow_snapshot_types.rows WHERE TRUE")
	// Concurrent readers share the immutable snapshot, including its nested variable-width data.
	for _, offset := range []int64{0, 17, 1023} {
		t.Run(fmt.Sprint(offset), func(t *testing.T) {
			t.Parallel()
			var total int64
			seen := make(map[int64]bool)
			for _, stream := range s.Streams {
				for _, block := range responses(t, read, stream.Name, offset) {
					data := append(bytes.Clone(s.GetArrowSchema().SerializedSchema), block.GetArrowRecordBatch().SerializedRecordBatch...)
					reader, err := ipc.NewReader(bytes.NewReader(data))
					if err != nil {
						t.Fatal(err)
					}
					if reader.Schema().Field(0).Nullable || !reader.Schema().Field(1).Nullable {
						t.Fatal("lost required/nullable field modes")
					}
					for reader.Next() {
						r := reader.RecordBatch()
						for row := 0; row < int(r.NumRows()); row++ {
							id := r.Column(0).(*array.Int64).Value(row)
							if id < 1 || id > 4103 || seen[id] {
								t.Fatalf("invalid or duplicate id: %d", id)
							}
							seen[id] = true
							wantFloat := float64(id) / 2
							switch id {
							case 1:
								wantFloat = math.NaN()
							case 2:
								wantFloat = math.Inf(1)
							case 3:
								wantFloat = math.Inf(-1)
							case 4:
								wantFloat = math.Copysign(0, -1)
							}
							f := r.Column(4).(*array.Float64).Value(row)
							if !(math.IsNaN(f) && math.IsNaN(wantFloat)) && (f != wantFloat || math.Signbit(f) != math.Signbit(wantFloat)) {
								t.Fatalf("float at id %d: got %v, want %v", id, f, wantFloat)
							}
							wantDate := int64(-1)
							if id == 1 {
								wantDate = -719162
							} else if id == 4103 {
								wantDate = 2932896
							}
							number := r.Column(5).(*array.Decimal128)
							if number.IsNull(row) != (id%11 == 0) || (!number.IsNull(row) && number.Value(row).BigInt().String() != "-1000000001") {
								t.Fatalf("numeric at id %d: %v", id, number)
							}
							text := r.Column(1).(*array.String)
							if text.IsNull(row) != (id%5 == 0) || (!text.IsNull(row) && text.Value(row) != fmt.Sprint("日本語", id)) {
								t.Fatalf("string at id %d: %v", id, text)
							}
							if !bytes.Equal(r.Column(2).(*array.Binary).Value(row), []byte{0, 255}) ||
								r.Column(3).(*array.Boolean).Value(row) != (id%2 == 0) ||
								int64(r.Column(6).(*array.Date32).Value(row)) != wantDate ||
								r.Column(7).(*array.Time64).Value(row) != 86399999999 ||
								r.Column(8).(*array.Timestamp).Value(row) != -1 ||
								r.Column(9).(*array.Timestamp).Value(row) != -1 {
								t.Fatalf("scalar value at id %d", id)
							}
							rec := r.Column(10).(*array.Struct)
							if rec.IsNull(row) != (id%7 == 0) || (!rec.IsNull(row) &&
								(rec.Field(0).(*array.Int64).Value(row) != id || rec.Field(1).(*array.String).Value(row) != fmt.Sprint(id))) {
								t.Fatalf("struct at id %d: %v", id, rec)
							}
						}
						total += r.NumRows()
					}
					if err := reader.Err(); err != nil {
						t.Fatal(err)
					}
					reader.Release()
				}
			}
			if want := 4103 - int64(len(s.Streams))*offset; total != want {
				t.Fatalf("read %d rows, want %d", total, want)
			}
		})
	}
}

func TestReadRejectsInvalidAndUnsupportedOptions(t *testing.T) {
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_errors", "id INT64")
	for _, tc := range []struct {
		name    string
		options *storagepb.ReadSession_TableReadOptions
		code    codes.Code
	}{
		{"unknown", &storagepb.ReadSession_TableReadOptions{SelectedFields: []string{"absent"}}, codes.InvalidArgument},
		{"injection", &storagepb.ReadSession_TableReadOptions{RowRestriction: "TRUE); DELETE FROM storage_read_errors.rows; --"}, codes.InvalidArgument},
		{"subquery", &storagepb.ReadSession_TableReadOptions{RowRestriction: "id IN (SELECT 1)"}, codes.Unimplemented},
		{"compression", &storagepb.ReadSession_TableReadOptions{OutputFormatSerializationOptions: &storagepb.ReadSession_TableReadOptions_ArrowSerializationOptions{ArrowSerializationOptions: &storagepb.ArrowSerializationOptions{BufferCompression: storagepb.ArrowSerializationOptions_LZ4_FRAME}}}, codes.Unimplemented},
	} {
		t.Run(tc.name, func(t *testing.T) {
			_, err := read.CreateReadSession(t.Context(), &storagepb.CreateReadSessionRequest{Parent: "projects/test", ReadSession: &storagepb.ReadSession{Table: path, DataFormat: storagepb.DataFormat_ARROW, ReadOptions: tc.options}})
			if status.Code(err) != tc.code {
				t.Fatalf("got %v, want %v", err, tc.code)
			}
		})
	}
	s := session(t, read, path, storagepb.DataFormat_ARROW, nil, 0)
	if got := arrowIDs(t, read, s, s.Streams[0].Name, 0); len(got) != 0 {
		t.Fatal(got)
	}
	stream, err := read.ReadRows(t.Context(), &storagepb.ReadRowsRequest{ReadStream: s.Streams[0].Name, Offset: -1})
	if err == nil {
		_, err = stream.Recv()
	}
	if status.Code(err) != codes.OutOfRange {
		t.Fatal(err)
	}
}

// The Storage Read API gives a RANGE as a STRUCT of start and end in Arrow, whose NULL field is an
// unbounded end, which the Go client reads as a RangeValue:
// https://docs.cloud.google.com/bigquery/docs/reference/storage#arrow_schema_details
func TestGoClientStorageReadRange(t *testing.T) {
	rest, _, _ := clients(t)
	table(t, rest, "storage_read_range", "id INT64, d RANGE<DATE>, dt RANGE<DATETIME>, ts RANGE<TIMESTAMP>")
	query(t, rest, `INSERT INTO storage_read_range.rows VALUES
		(1, RANGE<DATE> '[2020-01-01, 2021-01-01)', RANGE<DATETIME> '[2020-01-01 12:00:00, UNBOUNDED)',
		 RANGE<TIMESTAMP> '[UNBOUNDED, 1970-01-01 00:00:00.000001+00)'),
		(2, NULL, NULL, NULL)`)
	if err := rest.EnableStorageReadClient(t.Context(), option.WithEndpoint(os.Getenv("BQ_EMULATOR_GRPC")), option.WithoutAuthentication(), option.WithGRPCDialOption(grpc.WithTransportCredentials(insecure.NewCredentials()))); err != nil {
		t.Fatal(err)
	}
	rows := rest.Dataset("storage_read_range").Table("rows").Read(t.Context())
	got := map[int64][]bigquery.Value{}
	for {
		var value []bigquery.Value
		err := rows.Next(&value)
		if err == iterator.Done {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		got[value[0].(int64)] = value[1:]
	}
	date := func(year int, month time.Month, day int) civil.Date {
		return civil.Date{Year: year, Month: month, Day: day}
	}
	want := map[int64][]bigquery.Value{
		1: {&bigquery.RangeValue{Start: date(2020, 1, 1), End: date(2021, 1, 1)},
			&bigquery.RangeValue{Start: civil.DateTime{Date: date(2020, 1, 1), Time: civil.Time{Hour: 12}}},
			&bigquery.RangeValue{End: time.UnixMicro(1).UTC()}},
		2: {nil, nil, nil},
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("values = %v, want %v", got, want)
	}
}

// In Avro, the first RANGE of an element type defines the record google.sqlType.RANGE_T, of
// nullable start and end fields, and later ones name it:
// https://docs.cloud.google.com/bigquery/docs/reference/storage#avro_schema_details
func TestReadAvroRange(t *testing.T) {
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_avro_range", "id INT64, d1 RANGE<DATE>, d2 RANGE<DATE>")
	query(t, rest, `INSERT INTO storage_read_avro_range.rows VALUES
		(1, RANGE<DATE> '[1969-12-31, UNBOUNDED)', RANGE<DATE> '[UNBOUNDED, 1970-01-02)')`)
	s := session(t, read, path, storagepb.DataFormat_AVRO, nil, 1)
	var schema struct {
		Fields []struct {
			Name string
			Type []json.RawMessage
		}
	}
	if err := json.Unmarshal([]byte(s.GetAvroSchema().Schema), &schema); err != nil {
		t.Fatal(err)
	}
	var record struct {
		Type, Name, Namespace string
		SQLType               string `json:"sqlType"`
		Fields                []struct {
			Name string
			Type []json.RawMessage
		}
	}
	if err := json.Unmarshal(schema.Fields[1].Type[1], &record); err != nil {
		t.Fatal(err)
	}
	if record.Type != "record" || record.Name != "RANGE_DATE" || record.Namespace != "google.sqlType" ||
		record.SQLType != "RANGE" ||
		len(record.Fields) != 2 || record.Fields[0].Name != "start" || record.Fields[1].Name != "end" ||
		string(record.Fields[0].Type[0]) != `"null"` || string(record.Fields[1].Type[0]) != `"null"` {
		t.Fatalf("first RANGE<DATE> schema: %s", schema.Fields[1].Type[1])
	}
	if reference := string(schema.Fields[2].Type[1]); reference != `"google.sqlType.RANGE_DATE"` {
		t.Fatalf("second RANGE<DATE> schema: %s", reference)
	}
	codec, err := goavro.NewCodec(s.GetAvroSchema().Schema)
	if err != nil {
		t.Fatal(err)
	}
	blocks := responses(t, read, s.Streams[0].Name, 0)
	value, remaining, err := codec.NativeFromBinary(blocks[0].GetAvroRows().SerializedBinaryRows)
	if err != nil {
		t.Fatal(err)
	}
	if len(remaining) != 0 {
		t.Fatal("unexpected trailing Avro bytes")
	}
	row := value.(map[string]any)
	day := func(value any) string { return union(value).(time.Time).Format("2006-01-02") }
	d1 := union(row["d1"]).(map[string]any)
	d2 := union(row["d2"]).(map[string]any)
	if day(d1["start"]) != "1969-12-31" || d1["end"] != nil || d2["start"] != nil || day(d2["end"]) != "1970-01-02" {
		t.Fatalf("RANGE values: %v", row)
	}
}
