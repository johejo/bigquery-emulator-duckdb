package storage

import (
	"bytes"
	"context"
	"io"
	"math/big"
	"os"
	"reflect"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"cloud.google.com/go/bigquery/storage/managedwriter"
	"cloud.google.com/go/civil"
	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
	"google.golang.org/api/iterator"
	"google.golang.org/api/option"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/known/wrapperspb"
)

func writerSchema() *descriptorpb.DescriptorProto {
	return &descriptorpb.DescriptorProto{Name: proto.String("Row"), Field: []*descriptorpb.FieldDescriptorProto{
		{Name: proto.String("id"), Number: proto.Int32(1), Type: descriptorpb.FieldDescriptorProto_TYPE_INT64.Enum(), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()},
		{Name: proto.String("name"), Number: proto.Int32(2), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()},
	}}
}

func TestOmittedRepeatedAndRequiredNestedFields(t *testing.T) {
	rest, _, write := clients(t)
	ds := rest.Dataset("storage_write_fields")
	if err := ds.Create(t.Context(), nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ds.DeleteWithContents(context.Background()) })
	if err := ds.Table("rows").Create(t.Context(), &bigquery.TableMetadata{Schema: bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "name", Type: bigquery.StringFieldType},
		{Name: "tags", Type: bigquery.StringFieldType, Repeated: true},
		{Name: "rec", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{{Name: "value", Type: bigquery.StringFieldType, Required: true}}},
	}}); err != nil {
		t.Fatal(err)
	}
	name := "projects/test/datasets/storage_write_fields/tables/rows/streams/_default"
	descriptor := writerSchema()
	descriptor.NestedType = []*descriptorpb.DescriptorProto{{Name: proto.String("Record"), Field: []*descriptorpb.FieldDescriptorProto{{Name: proto.String("value"), Number: proto.Int32(1), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()}}}}
	descriptor.Field = append(descriptor.Field, &descriptorpb.FieldDescriptorProto{Name: proto.String("rec"), Number: proto.Int32(3), Type: descriptorpb.FieldDescriptorProto_TYPE_MESSAGE.Enum(), TypeName: proto.String(".Row.Record"), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()})
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	request := appendRequest(name, true, nil, row(1, "valid"))
	request.GetProtoRows().WriterSchema.ProtoDescriptor = descriptor
	if response := send(t, stream, request); response.GetError() != nil {
		t.Fatal(response)
	}
	bad := protowire.AppendTag(row(2, "bad"), 3, protowire.BytesType)
	bad = protowire.AppendBytes(bad, nil) // A present record must contain its required child.
	if response := send(t, stream, appendRequest("", false, nil, bad)); response.GetError().GetCode() != int32(codes.InvalidArgument) {
		t.Fatal(response)
	}
	if response := send(t, stream, appendRequest("", false, nil, row(3, string([]byte{0xff})))); response.GetError().GetCode() != int32(codes.InvalidArgument) {
		t.Fatal(response)
	}
	closeAppend(t, stream)
	wantVisible(t, rest, "storage_write_fields", 1)
	rows, err := rest.Query("SELECT ARRAY_LENGTH(tags), rec IS NULL FROM storage_write_fields.rows").Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var values []bigquery.Value
	if err := rows.Next(&values); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(values, []bigquery.Value{int64(0), true}) {
		t.Fatalf("missing fields: %v", values)
	}
}

func TestProtobufTypes(t *testing.T) {
	rest, _, write := clients(t)
	path := table(t, rest, "storage_write_types", "id INT64, name STRING, n NUMERIC, bn BIGNUMERIC, d DATE, tm TIME, dt DATETIME, ts TIMESTAMP, payload BYTES, enabled BOOL, tags ARRAY<STRING>")
	descriptor := writerSchema()
	for i, field := range []struct {
		name string
		kind descriptorpb.FieldDescriptorProto_Type
	}{
		{"n", descriptorpb.FieldDescriptorProto_TYPE_BYTES}, {"bn", descriptorpb.FieldDescriptorProto_TYPE_BYTES},
		{"d", descriptorpb.FieldDescriptorProto_TYPE_INT32}, {"tm", descriptorpb.FieldDescriptorProto_TYPE_INT64},
		{"dt", descriptorpb.FieldDescriptorProto_TYPE_STRING}, {"ts", descriptorpb.FieldDescriptorProto_TYPE_INT64},
		{"payload", descriptorpb.FieldDescriptorProto_TYPE_BYTES}, {"enabled", descriptorpb.FieldDescriptorProto_TYPE_BOOL},
		{"tags", descriptorpb.FieldDescriptorProto_TYPE_STRING},
	} {
		label := descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL
		if field.name == "tags" {
			label = descriptorpb.FieldDescriptorProto_LABEL_REPEATED
		}
		descriptor.Field = append(descriptor.Field, &descriptorpb.FieldDescriptorProto{Name: proto.String(field.name), Number: proto.Int32(int32(i + 3)), Type: field.kind.Enum(), Label: label.Enum()})
	}
	data := row(1, "日本語")
	bytesField := func(number protowire.Number, value []byte) {
		data = protowire.AppendTag(data, number, protowire.BytesType)
		data = protowire.AppendBytes(data, value)
	}
	intField := func(number protowire.Number, value int64) {
		data = protowire.AppendTag(data, number, protowire.VarintType)
		data = protowire.AppendVarint(data, uint64(value))
	}
	// Storage decimals are signed little-endian integers scaled by 10^9 and 10^38.
	bytesField(3, []byte{0, 0xca, 0x9a, 0x3b})
	bytesField(4, []byte{1})
	intField(5, -1)
	intField(6, 1<<20) // Packed civil seconds occupy the bits above the 20-bit microseconds.
	bytesField(7, []byte("1969-12-31 23:59:59.999999"))
	intField(8, -1)
	bytesField(9, []byte{0, 255})
	intField(10, 1)
	bytesField(11, []byte("a"))
	bytesField(11, []byte("日本語"))
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	request := appendRequest(path+"/streams/_default", true, nil, data)
	request.GetProtoRows().WriterSchema.ProtoDescriptor = descriptor
	if response := send(t, stream, request); response.GetError() != nil {
		t.Fatal(response)
	}
	invalid := protowire.AppendTag(nil, 7, protowire.BytesType)
	invalid = protowire.AppendString(invalid, "1969-12-31 24:00:00")
	if response := send(t, stream, appendRequest("", false, nil, invalid)); response.GetError().GetCode() != int32(codes.InvalidArgument) {
		t.Fatal(response)
	}
	closeAppend(t, stream)
	rows, err := rest.Query("SELECT n, bn, d, tm, dt, ts, payload, enabled, tags FROM storage_write_types.rows").Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var values []bigquery.Value
	if err := rows.Next(&values); err != nil {
		t.Fatal(err)
	}
	date := civil.Date{Year: 1969, Month: time.December, Day: 31}
	if values[0].(*big.Rat).FloatString(9) != "1.000000000" || values[1].(*big.Rat).FloatString(38) != "0.00000000000000000000000000000000000001" || values[2] != date || values[3] != (civil.Time{Second: 1}) || values[4] != (civil.DateTime{Date: date, Time: civil.Time{Hour: 23, Minute: 59, Second: 59, Nanosecond: 999999000}}) || values[5].(time.Time).UnixMicro() != -1 || !bytes.Equal(values[6].([]byte), []byte{0, 255}) || values[7] != true || !reflect.DeepEqual(values[8], []bigquery.Value{"a", "日本語"}) {
		t.Fatalf("protobuf values: %v", values)
	}
}

func TestReconnectAndDefaultDestinationSwitch(t *testing.T) {
	rest, _, write := clients(t)
	first := table(t, rest, "storage_write_reconnect", "id INT64, name STRING")
	second := table(t, rest, "storage_write_switch", "id INT64, name STRING")
	state := writeStream(t, write, first, storagepb.WriteStream_COMMITTED)
	for id := int64(0); id < 2; id++ {
		stream, err := write.AppendRows(t.Context())
		if err != nil {
			t.Fatal(err)
		}
		response := send(t, stream, appendRequest(state.Name, true, &id, row(id, "reconnect")))
		if response.GetError() != nil || response.GetAppendResult().GetOffset().GetValue() != id {
			t.Fatal(response)
		}
		closeAppend(t, stream)
	}
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{first, second} {
		if response := send(t, stream, appendRequest(path+"/streams/_default", true, nil, row(2, "switch"))); response.GetError() != nil {
			t.Fatal(response)
		}
	}
	closeAppend(t, stream)
	wantVisible(t, rest, "storage_write_reconnect", 0, 1, 2)
	wantVisible(t, rest, "storage_write_switch", 2)
}

func TestStorageStateAfterRestart(t *testing.T) {
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("run just e2e")
	}
	dataDir := t.TempDir()
	start := func() (*emulatorprocess.Process, *bigquery.Client, storagepb.BigQueryWriteClient) {
		t.Helper()
		ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
		defer cancel()
		p, err := emulatorprocess.Start(ctx, binary, "", "--host", "127.0.0.1", "--port", "0", "--data-dir", dataDir, "--project", `{"projectId":"test"}`)
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = p.Stop() })
		rest, err := bigquery.NewClient(t.Context(), "test", option.WithEndpoint(p.URL), option.WithoutAuthentication())
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = rest.Close() })
		conn, err := grpc.NewClient(p.GRPC, grpc.WithTransportCredentials(insecure.NewCredentials()))
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = conn.Close() })
		return p, rest, storagepb.NewBigQueryWriteClient(conn)
	}
	p, rest, write := start()
	if err := rest.Dataset("storage_restart").Create(t.Context(), nil); err != nil {
		t.Fatal(err)
	}
	query(t, rest, "CREATE TABLE storage_restart.rows (id INT64, name STRING)")
	path := "projects/test/datasets/storage_restart/tables/rows"
	var names []string
	for i, kind := range []storagepb.WriteStream_Type{storagepb.WriteStream_COMMITTED, storagepb.WriteStream_BUFFERED, storagepb.WriteStream_PENDING} {
		state := writeStream(t, write, path, kind)
		names = append(names, state.Name)
		stream, err := write.AppendRows(t.Context())
		if err != nil {
			t.Fatal(err)
		}
		if response := send(t, stream, appendRequest(state.Name, true, nil, row(int64(i), "restart"))); response.GetError() != nil {
			t.Fatal(response)
		}
		closeAppend(t, stream)
	}
	if err := p.Stop(); err != nil {
		t.Fatal(err)
	}
	_, rest, write = start()
	wantVisible(t, rest, "storage_restart", 0)
	for _, name := range names {
		if _, err := write.GetWriteStream(t.Context(), &storagepb.GetWriteStreamRequest{Name: name}); status.Code(err) != codes.NotFound {
			t.Fatalf("stream after restart: %v", err)
		}
	}
}

func row(id int64, name string) []byte {
	data := protowire.AppendTag(nil, 1, protowire.VarintType)
	data = protowire.AppendVarint(data, uint64(id))
	data = protowire.AppendTag(data, 2, protowire.BytesType)
	return protowire.AppendString(data, name)
}

func appendRequest(name string, descriptor bool, offset *int64, data ...[]byte) *storagepb.AppendRowsRequest {
	input := &storagepb.AppendRowsRequest_ProtoData{Rows: &storagepb.ProtoRows{SerializedRows: data}}
	if descriptor {
		input.WriterSchema = &storagepb.ProtoSchema{ProtoDescriptor: writerSchema()}
	}
	request := &storagepb.AppendRowsRequest{WriteStream: name, Rows: &storagepb.AppendRowsRequest_ProtoRows{ProtoRows: input}}
	if offset != nil {
		request.Offset = wrapperspb.Int64(*offset)
	}
	return request
}

func send(t *testing.T, stream storagepb.BigQueryWrite_AppendRowsClient, request *storagepb.AppendRowsRequest) *storagepb.AppendRowsResponse {
	t.Helper()
	if err := stream.Send(request); err != nil {
		t.Fatal(err)
	}
	response, err := stream.Recv()
	if err != nil {
		t.Fatal(err)
	}
	return response
}

func closeAppend(t *testing.T, stream storagepb.BigQueryWrite_AppendRowsClient) {
	t.Helper()
	if err := stream.CloseSend(); err != nil {
		t.Fatal(err)
	}
	if _, err := stream.Recv(); err != io.EOF {
		t.Fatalf("AppendRows end: %v", err)
	}
}

func visibleIDs(t *testing.T, rest *bigquery.Client, dataset string) []int64 {
	t.Helper()
	rows, err := rest.Query("SELECT id FROM " + dataset + ".rows ORDER BY id").Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	result := []int64{}
	for {
		var row []bigquery.Value
		err := rows.Next(&row)
		if err == iterator.Done {
			return result
		}
		if err != nil {
			t.Fatal(err)
		}
		result = append(result, row[0].(int64))
	}
}

func wantVisible(t *testing.T, rest *bigquery.Client, dataset string, want ...int64) {
	t.Helper()
	got := visibleIDs(t, rest, dataset)
	if !reflect.DeepEqual(got, append([]int64{}, want...)) {
		t.Fatalf("visible rows: %v, want %v", got, want)
	}
}

func writeStream(t *testing.T, write storagepb.BigQueryWriteClient, path string, kind storagepb.WriteStream_Type) *storagepb.WriteStream {
	t.Helper()
	result, err := write.CreateWriteStream(t.Context(), &storagepb.CreateWriteStreamRequest{Parent: path, WriteStream: &storagepb.WriteStream{Type: kind}})
	if err != nil {
		t.Fatal(err)
	}
	if result.Type != kind || result.TableSchema == nil || result.CreateTime == nil {
		t.Fatalf("stream metadata: %v", result)
	}
	if kind == storagepb.WriteStream_COMMITTED && !result.CreateTime.AsTime().Equal(result.CommitTime.AsTime()) {
		t.Fatal("committed stream create/commit times differ")
	}
	return result
}

func TestDefaultStreamAndAtomicBadBatch(t *testing.T) {
	rest, _, write := clients(t)
	path := table(t, rest, "storage_write_default", "id INT64 NOT NULL, name STRING")
	name := path + "/streams/_default"
	metadata, err := write.GetWriteStream(t.Context(), &storagepb.GetWriteStreamRequest{Name: name, View: storagepb.WriteStreamView_FULL})
	if err != nil {
		t.Fatal(err)
	}
	tableMetadata, err := rest.Dataset("storage_write_default").Table("rows").Metadata(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Type != storagepb.WriteStream_COMMITTED || !metadata.CreateTime.AsTime().Equal(tableMetadata.CreationTime) {
		t.Fatalf("default metadata: %v, table created %v", metadata, tableMetadata.CreationTime)
	}
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	response := send(t, stream, appendRequest(name, true, nil, row(1, "日本語"), row(2, "")))
	if response.GetError() != nil || response.GetAppendResult() == nil || response.GetAppendResult().Offset != nil {
		t.Fatalf("default append: %v", response)
	}
	response = send(t, stream, appendRequest("", false, nil, row(3, "valid"), []byte{}))
	if response.GetError().GetCode() != int32(codes.InvalidArgument) || len(response.RowErrors) != 1 || response.RowErrors[0].Index != 1 {
		t.Fatalf("bad batch: %v", response)
	}
	wantVisible(t, rest, "storage_write_default", 1, 2)
	zero := int64(0)
	response = send(t, stream, appendRequest("", false, &zero, row(3, "valid")))
	if response.GetError().GetCode() != int32(codes.InvalidArgument) {
		t.Fatalf("default offset: %v", response)
	}
	response = send(t, stream, appendRequest("", false, nil, row(3, "valid")))
	if response.GetError() != nil {
		t.Fatal(response)
	}
	closeAppend(t, stream)
	wantVisible(t, rest, "storage_write_default", 1, 2, 3)
	_, err = write.FinalizeWriteStream(t.Context(), &storagepb.FinalizeWriteStreamRequest{Name: name})
	if status.Code(err) != codes.InvalidArgument {
		t.Fatal(err)
	}
}

// Visibility, offset errors and finalize/commit behavior are specified in storage.proto and
// https://cloud.google.com/bigquery/docs/write-api-streaming and write-api-batch.
func TestExplicitStreamVisibilityOffsetsAndFinalize(t *testing.T) {
	for _, kind := range []storagepb.WriteStream_Type{storagepb.WriteStream_COMMITTED, storagepb.WriteStream_BUFFERED, storagepb.WriteStream_PENDING} {
		t.Run(kind.String(), func(t *testing.T) {
			rest, _, write := clients(t)
			dataset := "storage_write_" + kind.String()
			path := table(t, rest, dataset, "id INT64 NOT NULL, name STRING")
			state := writeStream(t, write, path, kind)
			stream, err := write.AppendRows(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			zero, two, five := int64(0), int64(2), int64(5)
			response := send(t, stream, appendRequest(state.Name, true, &zero, row(1, "a"), row(2, "b")))
			if response.GetError() != nil || response.GetAppendResult().GetOffset().GetValue() != 0 {
				t.Fatal(response)
			}
			response = send(t, stream, appendRequest("", false, &zero, row(9, "duplicate")))
			if response.GetError().GetCode() != int32(codes.AlreadyExists) {
				t.Fatalf("duplicate: %v", response)
			}
			response = send(t, stream, appendRequest("", false, &five, row(9, "gap")))
			if response.GetError().GetCode() != int32(codes.OutOfRange) {
				t.Fatalf("gap: %v", response)
			}
			response = send(t, stream, appendRequest("", false, &two, row(3, "c")))
			if response.GetError() != nil || response.GetAppendResult().GetOffset().GetValue() != 2 {
				t.Fatal(response)
			}
			if kind == storagepb.WriteStream_COMMITTED {
				wantVisible(t, rest, dataset, 1, 2, 3)
			} else {
				wantVisible(t, rest, dataset)
			}
			basic, err := write.GetWriteStream(t.Context(), &storagepb.GetWriteStreamRequest{Name: state.Name})
			if err != nil {
				t.Fatal(err)
			}
			if basic.TableSchema != nil {
				t.Fatal("BASIC includes schema")
			}
			if kind == storagepb.WriteStream_BUFFERED {
				for range 2 {
					flushed, err := write.FlushRows(t.Context(), &storagepb.FlushRowsRequest{WriteStream: state.Name, Offset: wrapperspb.Int64(0)})
					if err != nil || flushed.Offset != 0 {
						t.Fatalf("flush: %v, %v", flushed, err)
					}
					wantVisible(t, rest, dataset, 1)
				}
				flushed, err := write.FlushRows(t.Context(), &storagepb.FlushRowsRequest{WriteStream: state.Name})
				if err != nil || flushed.Offset != 2 {
					t.Fatalf("flush all: %v, %v", flushed, err)
				}
				wantVisible(t, rest, dataset, 1, 2, 3)
			}
			for range 2 {
				finalized, err := write.FinalizeWriteStream(t.Context(), &storagepb.FinalizeWriteStreamRequest{Name: state.Name})
				if err != nil || finalized.RowCount != 3 {
					t.Fatalf("finalize: %v, %v", finalized, err)
				}
			}
			response = send(t, stream, appendRequest("", false, nil, row(4, "late")))
			if response.GetError().GetCode() != int32(codes.FailedPrecondition) {
				t.Fatalf("append after finalize: %v", response)
			}
			closeAppend(t, stream)
			if kind == storagepb.WriteStream_PENDING {
				wantVisible(t, rest, dataset)
				committed, err := write.BatchCommitWriteStreams(t.Context(), &storagepb.BatchCommitWriteStreamsRequest{Parent: path, WriteStreams: []string{state.Name}})
				if err != nil || committed.CommitTime == nil || len(committed.StreamErrors) != 0 {
					t.Fatalf("commit: %v, %v", committed, err)
				}
				wantVisible(t, rest, dataset, 1, 2, 3)
			}
		})
	}
}

func TestPendingBatchCommitIsAtomic(t *testing.T) {
	rest, _, write := clients(t)
	path := table(t, rest, "storage_write_atomic", "id INT64 NOT NULL, name STRING")
	var names []string
	for id := int64(1); id <= 2; id++ {
		state := writeStream(t, write, path, storagepb.WriteStream_PENDING)
		names = append(names, state.Name)
		stream, err := write.AppendRows(t.Context())
		if err != nil {
			t.Fatal(err)
		}
		if response := send(t, stream, appendRequest(state.Name, true, nil, row(id, "pending"))); response.GetError() != nil {
			t.Fatal(response)
		}
		closeAppend(t, stream)
		if id == 1 {
			if _, err := write.FinalizeWriteStream(t.Context(), &storagepb.FinalizeWriteStreamRequest{Name: state.Name}); err != nil {
				t.Fatal(err)
			}
		}
	}
	request := &storagepb.BatchCommitWriteStreamsRequest{Parent: path, WriteStreams: names}
	failed, err := write.BatchCommitWriteStreams(t.Context(), request)
	if err != nil || failed.CommitTime != nil || len(failed.StreamErrors) != 1 {
		t.Fatalf("commit before finalize: %v, %v", failed, err)
	}
	wantVisible(t, rest, "storage_write_atomic")
	if _, err := write.FinalizeWriteStream(t.Context(), &storagepb.FinalizeWriteStreamRequest{Name: names[1]}); err != nil {
		t.Fatal(err)
	}
	committed, err := write.BatchCommitWriteStreams(t.Context(), request)
	if err != nil || committed.CommitTime == nil || len(committed.StreamErrors) != 0 {
		t.Fatalf("commit: %v, %v", committed, err)
	}
	wantVisible(t, rest, "storage_write_atomic", 1, 2)
	again, err := write.BatchCommitWriteStreams(t.Context(), request)
	if err != nil || again.CommitTime != nil || len(again.StreamErrors) != 2 {
		t.Fatalf("duplicate commit: %v, %v", again, err)
	}
	wantVisible(t, rest, "storage_write_atomic", 1, 2)
}

func TestManagedWriter(t *testing.T) {
	rest, _, _ := clients(t)
	path := table(t, rest, "storage_managed_writer", "id INT64 NOT NULL, name STRING")
	ctx, cancel := context.WithTimeout(t.Context(), 30*time.Second)
	defer cancel()
	client, err := managedwriter.NewClient(ctx, "test", option.WithEndpoint(os.Getenv("BQ_EMULATOR_GRPC")), option.WithoutAuthentication(), option.WithGRPCDialOption(grpc.WithTransportCredentials(insecure.NewCredentials())))
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	stream, err := client.NewManagedStream(ctx, managedwriter.WithDestinationTable(path), managedwriter.WithType(managedwriter.CommittedStream), managedwriter.WithSchemaDescriptor(writerSchema()))
	if err != nil {
		t.Fatal(err)
	}
	defer stream.Close()
	for id := int64(0); id < 3; id++ {
		result, err := stream.AppendRows(ctx, [][]byte{row(id, "managed")}, managedwriter.WithOffset(id))
		if err != nil {
			t.Fatal(err)
		}
		offset, err := result.GetResult(ctx)
		if err != nil || offset != id {
			t.Fatalf("managed append: %d, %v", offset, err)
		}
	}
	count, err := stream.Finalize(ctx)
	if err != nil || count != 3 {
		t.Fatalf("managed finalize: %d, %v", count, err)
	}
	wantVisible(t, rest, "storage_managed_writer", 0, 1, 2)
}

func TestDroppedTableInvalidatesOldStream(t *testing.T) {
	rest, _, write := clients(t)
	path := table(t, rest, "storage_write_recreate", "id INT64, name STRING")
	state := writeStream(t, write, path, storagepb.WriteStream_COMMITTED)
	query(t, rest, "CREATE OR REPLACE TABLE storage_write_recreate.rows (id INT64, name STRING)")
	_, err := write.GetWriteStream(t.Context(), &storagepb.GetWriteStreamRequest{Name: state.Name})
	if status.Code(err) != codes.NotFound {
		t.Fatal(err)
	}
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	response := send(t, stream, appendRequest(state.Name, true, nil, row(1, "old")))
	if response.GetError().GetCode() != int32(codes.NotFound) {
		t.Fatal(response)
	}
	response = send(t, stream, appendRequest(path+"/streams/_default", true, nil, row(2, "new")))
	if response.GetError() != nil {
		t.Fatal(response)
	}
	closeAppend(t, stream)
	wantVisible(t, rest, "storage_write_recreate", 2)
}
