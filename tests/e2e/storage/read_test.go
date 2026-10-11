package storage

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"math/big"
	"os"
	"reflect"
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
