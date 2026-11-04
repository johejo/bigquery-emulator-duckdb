package storage

import (
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"google.golang.org/grpc/codes"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"
)

// JSON columns take protobuf strings. Canonicalization keeps the first duplicate key,
// preserves signed/unsigned 64-bit integers, and permits at most 500 nested levels.
// third_party/bigquery_docs/supported-data-types.md
// third_party/bigquery_docs/reference/standard-sql/data-types.md
// JSON null is distinct from SQL NULL:
// third_party/bigquery_docs/reference/standard-sql/json_functions.md
func TestJSONWrite(t *testing.T) {
	t.Parallel()
	for _, kind := range []storagepb.WriteStream_Type{
		storagepb.WriteStream_TYPE_UNSPECIFIED, storagepb.WriteStream_COMMITTED,
		storagepb.WriteStream_BUFFERED, storagepb.WriteStream_PENDING,
	} {
		t.Run(kind.String(), func(t *testing.T) {
			t.Parallel()
			rest, _, write := clients(t)
			dataset := "storage_json_" + kind.String()
			path := table(t, rest, dataset, "id INT64, name STRING, j JSON, js ARRAY<JSON>, s STRUCT<j JSON>")
			name := path + "/streams/_default"
			if kind != storagepb.WriteStream_TYPE_UNSPECIFIED {
				name = writeStream(t, write, path, kind).Name
			}
			stringField := func(name string, number int32, repeated bool) *descriptorpb.FieldDescriptorProto {
				label := descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL
				if repeated {
					label = descriptorpb.FieldDescriptorProto_LABEL_REPEATED
				}
				return &descriptorpb.FieldDescriptorProto{Name: proto.String(name), Number: proto.Int32(number), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(), Label: label.Enum()}
			}
			descriptor := writerSchema()
			descriptor.Field = append(descriptor.Field, stringField("j", 3, false), stringField("js", 4, true),
				&descriptorpb.FieldDescriptorProto{Name: proto.String("s"), Number: proto.Int32(5), Type: descriptorpb.FieldDescriptorProto_TYPE_MESSAGE.Enum(), TypeName: proto.String(".Row.Record"), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()})
			descriptor.NestedType = []*descriptorpb.DescriptorProto{{Name: proto.String("Record"), Field: []*descriptorpb.FieldDescriptorProto{stringField("j", 1, false)}}}
			textField := func(data []byte, number protowire.Number, text string) []byte {
				return protowire.AppendString(protowire.AppendTag(data, number, protowire.BytesType), text)
			}
			data := textField(row(1, "object"), 3, `{"a":1,"a":2,"min":-9223372036854775808,"max":18446744073709551615,"text":"日本語"}`)
			for _, text := range []string{"null", `""`, "[]", "{}", "true", "1.25"} {
				data = textField(data, 4, text)
			}
			nested := textField(nil, 1, `[1,null,"日本語"]`)
			data = protowire.AppendBytes(protowire.AppendTag(data, 5, protowire.BytesType), nested)
			stream, err := write.AppendRows(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			request := appendRequest(name, true, nil, data, textField(row(2, "null"), 3, "null"), row(3, "missing"), textField(row(4, "depth"), 3, strings.Repeat("[", 500)+"0"+strings.Repeat("]", 500)))
			request.GetProtoRows().WriterSchema.ProtoDescriptor = descriptor
			if response := send(t, stream, request); response.GetError() != nil {
				t.Fatal(response)
			}
			// Invalid JSON at each location rejects the whole append, including its valid row.
			for _, bad := range [][]byte{
				textField(row(6, "bad"), 3, "{"),
				textField(row(6, "empty"), 3, ""),
				textField(row(6, "bad array"), 4, "[1,]"),
				protowire.AppendBytes(protowire.AppendTag(row(6, "bad record"), 5, protowire.BytesType), textField(nil, 1, "not json")),
				textField(row(6, "too deep"), 3, strings.Repeat("[", 501)+"0"+strings.Repeat("]", 501)),
			} {
				response := send(t, stream, appendRequest("", false, nil, row(5, "rolled back"), bad))
				if response.GetError().GetCode() != int32(codes.InvalidArgument) || len(response.RowErrors) != 1 || response.RowErrors[0].Index != 1 {
					t.Fatalf("invalid JSON batch: %v", response)
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
			wantVisible(t, rest, dataset, 1, 2, 3, 4)
			rows, err := rest.Query(`SELECT JSON_VALUE(j.a), JSON_VALUE(j.min), JSON_VALUE(j.max), JSON_VALUE(j.text),
    ARRAY(SELECT JSON_TYPE(x) FROM UNNEST(js) AS x WITH OFFSET ORDER BY offset),
    JSON_VALUE(s.j[2]) FROM ` + dataset + `.rows WHERE id = 1`).Read(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			var got []bigquery.Value
			if err := rows.Next(&got); err != nil {
				t.Fatal(err)
			}
			want := []bigquery.Value{"1", "-9223372036854775808", "18446744073709551615", "日本語", []bigquery.Value{"null", "string", "array", "object", "boolean", "number"}, "日本語"}
			if !reflect.DeepEqual(got, want) {
				t.Fatalf("JSON values = %v, want %v", got, want)
			}
			rows, err = rest.Query(`SELECT j IS NULL, JSON_TYPE(j), ARRAY_LENGTH(js), s IS NULL FROM ` + dataset + `.rows WHERE id IN (2,3) ORDER BY id`).Read(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			for _, want := range [][]bigquery.Value{{false, "null", int64(0), true}, {true, nil, int64(0), true}} {
				var got []bigquery.Value
				if err := rows.Next(&got); err != nil {
					t.Fatal(err)
				}
				if !reflect.DeepEqual(got, want) {
					t.Fatalf("JSON nulls = %v, want %v", got, want)
				}
			}
		})
	}
}
