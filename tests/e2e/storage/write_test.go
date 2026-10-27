package storage

import (
	"bytes"
	"context"
	"io"
	"math/big"
	"os"
	"reflect"
	"strings"
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

// GEOGRAPHY fields are protobuf strings of WKT or GeoJSON, at any depth.
// third_party/bigquery_docs/supported-data-types.md
func TestGeography(t *testing.T) {
	rest, _, write := clients(t)
	path := table(t, rest, "storage_write_geography", "id INT64, name STRING, g GEOGRAPHY, gs ARRAY<GEOGRAPHY>")
	descriptor := writerSchema()
	descriptor.Field = append(descriptor.Field,
		&descriptorpb.FieldDescriptorProto{Name: proto.String("g"), Number: proto.Int32(3), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()},
		&descriptorpb.FieldDescriptorProto{Name: proto.String("gs"), Number: proto.Int32(4), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(), Label: descriptorpb.FieldDescriptorProto_LABEL_REPEATED.Enum()})
	geographies := func(id int64, values ...string) []byte {
		data := row(id, "geography")
		for i, value := range values {
			number := protowire.Number(min(i+3, 4))
			data = protowire.AppendString(protowire.AppendTag(data, number, protowire.BytesType), value)
		}
		return data
	}
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	request := appendRequest(path+"/streams/_default", true, nil,
		geographies(1, "POINT(1 2)", `{"type": "Point", "coordinates": [3, 4]}`, "LINESTRING(0 0, 1 1)"))
	request.GetProtoRows().WriterSchema.ProtoDescriptor = descriptor
	if response := send(t, stream, request); response.GetError() != nil {
		t.Fatal(response)
	}
	response := send(t, stream, appendRequest("", false, nil, geographies(2, "POINT(5 6)"), geographies(3, "POINT(1)")))
	if response.GetError().GetCode() != int32(codes.InvalidArgument) || len(response.RowErrors) != 1 || response.RowErrors[0].Index != 1 {
		t.Fatalf("invalid geography: %v", response)
	}
	closeAppend(t, stream)
	wantVisible(t, rest, "storage_write_geography", 1)
	rows, err := rest.Query(`SELECT ST_ASTEXT(g), ARRAY(SELECT ST_ASTEXT(x) FROM UNNEST(gs) AS x WITH OFFSET ORDER BY offset)
	 FROM storage_write_geography.rows`).Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var values []bigquery.Value
	if err := rows.Next(&values); err != nil {
		t.Fatal(err)
	}
	if want := []bigquery.Value{"POINT(1 2)", []bigquery.Value{"POINT(3 4)", "LINESTRING(0 0, 1 1)"}}; !reflect.DeepEqual(values, want) {
		t.Fatalf("geographies = %v, want %v", values, want)
	}
}

// Column rounding modes apply when values are written, including nested fields. The ties are
// BigQuery's DDL rounding_mode examples; precision overflow must reject the entire append.
// third_party/bigquery_docs/reference/standard-sql/data-definition-language.md
func TestRoundingModes(t *testing.T) {
	t.Parallel()
	for _, kind := range []storagepb.WriteStream_Type{
		storagepb.WriteStream_TYPE_UNSPECIFIED, // The default stream.
		storagepb.WriteStream_COMMITTED, storagepb.WriteStream_BUFFERED, storagepb.WriteStream_PENDING,
	} {
		t.Run(kind.String(), func(t *testing.T) {
			t.Parallel()
			rest, _, write := clients(t)
			dataset := "storage_rounding_" + kind.String()
			path := table(t, rest, dataset, `id INT64, name STRING,
			 n NUMERIC(5, 1) OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'),
			 bn BIGNUMERIC(10, 0) OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'),
			 away NUMERIC(5, 1),
			 a ARRAY<NUMERIC(5, 1)> OPTIONS (rounding_mode = 'ROUND_HALF_EVEN'),
			 s STRUCT<n NUMERIC(5, 1) OPTIONS (rounding_mode = 'ROUND_HALF_EVEN')>`)
			name := path + "/streams/_default"
			if kind != storagepb.WriteStream_TYPE_UNSPECIFIED {
				name = writeStream(t, write, path, kind).Name
			}
			descriptor := writerSchema()
			for i, field := range []string{"n", "bn", "away", "a"} {
				label := descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL
				if field == "a" {
					label = descriptorpb.FieldDescriptorProto_LABEL_REPEATED
				}
				descriptor.Field = append(descriptor.Field, &descriptorpb.FieldDescriptorProto{
					Name: proto.String(field), Number: proto.Int32(int32(i + 3)),
					Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(), Label: label.Enum(),
				})
			}
			descriptor.NestedType = []*descriptorpb.DescriptorProto{{Name: proto.String("Record"), Field: []*descriptorpb.FieldDescriptorProto{
				{Name: proto.String("n"), Number: proto.Int32(1), Type: descriptorpb.FieldDescriptorProto_TYPE_BYTES.Enum(), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()},
			}}}
			descriptor.Field = append(descriptor.Field, &descriptorpb.FieldDescriptorProto{
				Name: proto.String("s"), Number: proto.Int32(7), Type: descriptorpb.FieldDescriptorProto_TYPE_MESSAGE.Enum(),
				TypeName: proto.String(".Row.Record"), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(),
			})
			data := row(1, "ties")
			for i, value := range []string{"2.25", "2.5", "2.25", "2.25", "-2.25", "2.35"} {
				number := protowire.Number(i + 3)
				if number > 6 {
					number = 6
				}
				data = protowire.AppendString(protowire.AppendTag(data, number, protowire.BytesType), value)
			}
			// NUMERIC 2.25 as a signed little-endian integer scaled by 10^9.
			nested := protowire.AppendBytes(protowire.AppendTag(nil, 1, protowire.BytesType), []byte{0x80, 0x46, 0x1c, 0x86, 0})
			data = protowire.AppendBytes(protowire.AppendTag(data, 7, protowire.BytesType), nested)
			stream, err := write.AppendRows(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			request := appendRequest(name, true, nil, data, row(2, "missing"))
			request.GetProtoRows().WriterSchema.ProtoDescriptor = descriptor
			if response := send(t, stream, request); response.GetError() != nil {
				t.Fatal(response)
			}
			for _, field := range []protowire.Number{3, 4, 6} {
				value := "9999.95"
				if field == 4 {
					value = "9999999999.5"
				}
				bad := protowire.AppendString(protowire.AppendTag(row(4, "overflow"), field, protowire.BytesType), value)
				response := send(t, stream, appendRequest("", false, nil, row(3, "rolled back"), bad))
				if response.GetError().GetCode() != int32(codes.InvalidArgument) || len(response.RowErrors) != 1 || response.RowErrors[0].Index != 1 {
					t.Fatalf("overflow batch: %v", response)
				}
			}
			closeAppend(t, stream)
			if kind == storagepb.WriteStream_BUFFERED {
				if _, err := write.FlushRows(t.Context(), &storagepb.FlushRowsRequest{WriteStream: name}); err != nil {
					t.Fatal(err)
				}
			}
			if kind == storagepb.WriteStream_PENDING {
				if _, err := write.FinalizeWriteStream(t.Context(), &storagepb.FinalizeWriteStreamRequest{Name: name}); err != nil {
					t.Fatal(err)
				}
				response, err := write.BatchCommitWriteStreams(t.Context(), &storagepb.BatchCommitWriteStreamsRequest{Parent: path, WriteStreams: []string{name}})
				if err != nil || len(response.GetStreamErrors()) != 0 {
					t.Fatalf("commit: %v, %v", response, err)
				}
			}
			wantVisible(t, rest, dataset, 1, 2)
			rows, err := rest.Query(`SELECT CAST(n AS STRING), CAST(bn AS STRING), CAST(away AS STRING),
			 ARRAY_TO_STRING(ARRAY(SELECT CAST(x AS STRING) FROM UNNEST(a) AS x WITH OFFSET ORDER BY offset), ','),
			 CAST(s.n AS STRING) FROM ` + dataset + `.rows ORDER BY id`).Read(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			for _, want := range [][]bigquery.Value{{"2.2", "2", "2.3", "2.2,-2.2,2.4", "2.2"}, {nil, nil, nil, "", nil}} {
				var got []bigquery.Value
				if err := rows.Next(&got); err != nil {
					t.Fatal(err)
				}
				if !reflect.DeepEqual(got, want) {
					t.Errorf("rounded values = %v, want %v", got, want)
				}
			}
		})
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

// FOR SYSTEM_TIME AS OF reads the rows a table had before an append committed.
// third_party/bigquery_docs/time-travel.md
func TestTimeTravelBeforeAppend(t *testing.T) {
	rest, _, write := clients(t)
	path := table(t, rest, "storage_write_time_travel", "id INT64, name STRING")
	appendRow := func(id int64) {
		t.Helper()
		stream, err := write.AppendRows(t.Context())
		if err != nil {
			t.Fatal(err)
		}
		if response := send(t, stream, appendRequest(path+"/streams/_default", true, nil, row(id, "x"))); response.GetError() != nil {
			t.Fatal(response)
		}
		closeAppend(t, stream)
	}
	appendRow(1)
	time.Sleep(5 * time.Millisecond)
	before := time.Now()
	time.Sleep(5 * time.Millisecond)
	appendRow(2)
	query := rest.Query("SELECT ARRAY_AGG(id ORDER BY id) FROM storage_write_time_travel.rows FOR SYSTEM_TIME AS OF @ts")
	query.Parameters = []bigquery.QueryParameter{{Name: "ts", Value: before}}
	rows, err := query.Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var values []bigquery.Value
	if err := rows.Next(&values); err != nil {
		t.Fatal(err)
	}
	if got := values[0]; !reflect.DeepEqual(got, []bigquery.Value{int64(1)}) {
		t.Errorf("rows before the append: %v, want [1]", got)
	}
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

// An AppendRows connection a client leaves open must not keep the emulator from shutting down.
func TestShutdownWithIdleAppendConnection(t *testing.T) {
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("run just e2e")
	}
	ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	p, err := emulatorprocess.Start(ctx, binary, "", "--host", "127.0.0.1", "--port", "0", "--project", `{"projectId":"test"}`)
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
	if err := rest.Dataset("storage_shutdown").Create(t.Context(), nil); err != nil {
		t.Fatal(err)
	}
	query(t, rest, "CREATE TABLE storage_shutdown.rows (id INT64, name STRING)")
	stream, err := storagepb.NewBigQueryWriteClient(conn).AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if response := send(t, stream, appendRequest("projects/test/datasets/storage_shutdown/tables/rows/streams/_default", true, nil, row(1, "idle"))); response.GetError() != nil {
		t.Fatal(response)
	}
	if err := p.Stop(); err != nil {
		t.Fatal(err)
	}
	if _, err := stream.Recv(); err == nil || err == io.EOF {
		t.Fatalf("AppendRows after shutdown: %v", err)
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
// third_party/bigquery_docs/write-api-streaming.md and write-api-batch.
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

// A write stream belongs to the table it was created for: once the table is dropped and another
// takes its name, by any path, the stream is gone and the default stream writes to the new table.
// Changing the table's rows or metadata, or failing to replace it, keeps the stream.
func TestStreamsFollowTableIdentity(t *testing.T) {
	wait := func(t *testing.T, job *bigquery.Job, err error) error {
		t.Helper()
		if err != nil {
			return err
		}
		state, err := job.Wait(t.Context())
		if err != nil {
			return err
		}
		return state.Err()
	}
	run := func(t *testing.T, rest *bigquery.Client, sql string) error {
		t.Helper()
		job, err := rest.Query(sql).Run(t.Context())
		return wait(t, job, err)
	}
	queryTo := func(t *testing.T, rest *bigquery.Client, table *bigquery.Table, sql string,
		disposition bigquery.TableWriteDisposition) error {
		t.Helper()
		query := rest.Query(sql)
		query.Dst = table
		query.WriteDisposition = disposition
		job, err := query.Run(t.Context())
		return wait(t, job, err)
	}
	loadTo := func(t *testing.T, table *bigquery.Table, rows string, schema bigquery.Schema,
		disposition bigquery.TableWriteDisposition) error {
		t.Helper()
		source := bigquery.NewReaderSource(strings.NewReader(rows))
		source.SourceFormat = bigquery.JSON
		source.Schema = schema
		loader := table.LoaderFrom(source)
		loader.WriteDisposition = disposition
		job, err := loader.Run(t.Context())
		return wait(t, job, err)
	}
	schema := bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}, {Name: "name", Type: bigquery.StringFieldType}}
	for _, c := range []struct {
		name string
		// change runs after the stream wrote row 1, and returns the rows the table holds then.
		change    func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64
		recreated bool
	}{
		{"create_or_replace", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			query(t, rest, "CREATE OR REPLACE TABLE "+table.DatasetID+".rows (id INT64, name STRING)")
			return nil
		}, true},
		{"drop_and_create", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			query(t, rest, "DROP TABLE "+table.DatasetID+".rows")
			query(t, rest, "CREATE TABLE "+table.DatasetID+".rows (id INT64, name STRING)")
			return nil
		}, true},
		{"tables_delete_and_insert", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			if err := table.Delete(t.Context()); err != nil {
				t.Fatal(err)
			}
			if err := table.Create(t.Context(), &bigquery.TableMetadata{Schema: schema}); err != nil {
				t.Fatal(err)
			}
			return nil
		}, true},
		{"query_job", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			if err := table.Delete(t.Context()); err != nil {
				t.Fatal(err)
			}
			if err := queryTo(t, rest, table, "SELECT 0 AS id, 'query' AS name", bigquery.WriteEmpty); err != nil {
				t.Fatal(err)
			}
			return []int64{0}
		}, true},
		{"load_job", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			if err := table.Delete(t.Context()); err != nil {
				t.Fatal(err)
			}
			if err := loadTo(t, table, `{"id": 0, "name": "load"}`, schema, bigquery.WriteEmpty); err != nil {
				t.Fatal(err)
			}
			return []int64{0}
		}, true},
		{"copy_job", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			source := rest.Dataset(table.DatasetID).Table("source")
			if err := table.Delete(t.Context()); err != nil {
				t.Fatal(err)
			}
			query(t, rest, "CREATE TABLE "+table.DatasetID+".source AS SELECT 0 AS id, 'copy' AS name")
			job, err := table.CopierFrom(source).Run(t.Context())
			if err := wait(t, job, err); err != nil {
				t.Fatal(err)
			}
			return []int64{0}
		}, true},
		{"tables_patch", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			if _, err := table.Update(t.Context(), bigquery.TableMetadataToUpdate{Description: "patched"}, ""); err != nil {
				t.Fatal(err)
			}
			return []int64{1}
		}, false},
		{"truncate_table", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			query(t, rest, "TRUNCATE TABLE "+table.DatasetID+".rows")
			return nil
		}, false},
		{"query_append", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			if err := queryTo(t, rest, table, "SELECT 0 AS id, 'query' AS name", bigquery.WriteAppend); err != nil {
				t.Fatal(err)
			}
			return []int64{0, 1}
		}, false},
		{"query_truncate_data", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			if err := queryTo(t, rest, table, "SELECT 0 AS id, 'query' AS name", bigquery.WriteTruncateData); err != nil {
				t.Fatal(err)
			}
			return []int64{0}
		}, false},
		{"failed_create_or_replace", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			if err := run(t, rest, "CREATE OR REPLACE TABLE "+table.DatasetID+".rows AS SELECT ERROR('boom') AS id, '' AS name"); err == nil {
				t.Fatal("replacement succeeded")
			}
			return []int64{1}
		}, false},
		{"failed_query_truncate", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			if err := queryTo(t, rest, table, "SELECT ERROR('boom') AS id, '' AS name", bigquery.WriteTruncate); err == nil {
				t.Fatal("replacement succeeded")
			}
			return []int64{1}
		}, false},
		{"failed_load_truncate", func(t *testing.T, rest *bigquery.Client, table *bigquery.Table) []int64 {
			required := bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType, Required: true}, schema[1]}
			if err := loadTo(t, table, `{"id": null}`, required, bigquery.WriteTruncate); err == nil {
				t.Fatal("replacement succeeded")
			}
			return []int64{1}
		}, false},
	} {
		t.Run(c.name, func(t *testing.T) {
			rest, _, write := clients(t)
			dataset := "storage_identity_" + c.name
			path := table(t, rest, dataset, "id INT64, name STRING")
			state := writeStream(t, write, path, storagepb.WriteStream_COMMITTED)
			stream, err := write.AppendRows(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			if response := send(t, stream, appendRequest(state.Name, true, nil, row(1, "before"))); response.GetError() != nil {
				t.Fatal(response)
			}
			closeAppend(t, stream)
			rows := c.change(t, rest, rest.Dataset(dataset).Table("rows"))

			_, err = write.GetWriteStream(t.Context(), &storagepb.GetWriteStreamRequest{Name: state.Name})
			stream, err2 := write.AppendRows(t.Context())
			if err2 != nil {
				t.Fatal(err2)
			}
			response := send(t, stream, appendRequest(state.Name, true, nil, row(2, "old stream")))
			if c.recreated {
				if status.Code(err) != codes.NotFound || response.GetError().GetCode() != int32(codes.NotFound) {
					t.Fatalf("old stream on a recreated table: get = %v, append = %v", err, response)
				}
			} else {
				if err != nil || response.GetError() != nil {
					t.Fatalf("old stream on the same table: get = %v, append = %v", err, response)
				}
				rows = append(rows, 2)
			}
			closeAppend(t, stream)
			stream, err = write.AppendRows(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			if response := send(t, stream, appendRequest(path+"/streams/_default", true, nil, row(3, "default"))); response.GetError() != nil {
				t.Fatal(response)
			}
			closeAppend(t, stream)
			wantVisible(t, rest, dataset, append(rows, 3)...)
		})
	}
}

// A table snapshot is read-only, so the Storage Write API cannot write to it.
// third_party/bigquery_docs/table-snapshots-intro.md
func TestWriteToTableSnapshot(t *testing.T) {
	rest, _, write := clients(t)
	path := table(t, rest, "storage_write_snapshot", "id INT64 NOT NULL, name STRING")
	query(t, rest, "CREATE SNAPSHOT TABLE storage_write_snapshot.snap CLONE storage_write_snapshot.rows")
	snapshot := strings.TrimSuffix(path, "/rows") + "/snap"
	_, err := write.CreateWriteStream(t.Context(), &storagepb.CreateWriteStreamRequest{
		Parent: snapshot, WriteStream: &storagepb.WriteStream{Type: storagepb.WriteStream_COMMITTED},
	})
	// BigQuery names the table by its project's number, which the test project registers.
	if status.Code(err) != codes.InvalidArgument || !strings.Contains(status.Convert(err).Message(),
		":storage_write_snapshot.snap with type SNAPSHOT. Allowed type: TABLE, BIGLAKE with streaming. Entity: "+snapshot) {
		t.Errorf("CreateWriteStream on a table snapshot: %v", err)
	}
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if err := stream.Send(appendRequest(snapshot+"/streams/_default", true, nil, row(1, "a"))); err != nil {
		t.Fatal(err)
	}
	_, err = stream.Recv()
	if status.Code(err) != codes.InvalidArgument || !strings.Contains(status.Convert(err).Message(),
		":storage_write_snapshot.snap` is non-appendable Entity: "+snapshot+"/streams/_default") {
		t.Errorf("default stream append to a table snapshot: %v", err)
	}
}
