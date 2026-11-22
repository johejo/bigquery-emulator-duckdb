package storage

import (
	"math"
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
