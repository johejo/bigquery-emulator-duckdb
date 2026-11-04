package storage

import (
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"google.golang.org/api/iterator"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"
)

// The Storage v1 proto specifies updated_schema on schema updates and writer_schema
// whenever the input schema changes (google/cloud/bigquery/storage/v1/storage.proto).
// Added nullable columns are NULL for existing rows:
// third_party/bigquery_docs/reference/standard-sql/data-definition-language.md
func TestWriteSchemaAddition(t *testing.T) {
	t.Parallel()
	for _, kind := range []storagepb.WriteStream_Type{
		storagepb.WriteStream_TYPE_UNSPECIFIED, // Default stream.
		storagepb.WriteStream_COMMITTED,
		storagepb.WriteStream_BUFFERED,
		storagepb.WriteStream_PENDING,
	} {
		t.Run(kind.String(), func(t *testing.T) {
			rest, _, write := clients(t)
			dataset := "storage_schema_" + kind.String()
			path := table(t, rest, dataset, "id INT64, name STRING")
			name := path + "/streams/_default"
			if kind != storagepb.WriteStream_TYPE_UNSPECIFIED {
				name = writeStream(t, write, path, kind).Name
			}
			stream, err := write.AppendRows(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			if response := send(t, stream, appendRequest(name, true, nil, row(1, "before"))); response.GetError() != nil {
				t.Fatal(response)
			}
			query(t, rest, "ALTER TABLE "+dataset+".rows ADD COLUMN extra STRING")
			query(t, rest, "ALTER TABLE "+dataset+".rows ADD COLUMN tags ARRAY<STRING>")
			response := send(t, stream, appendRequest("", false, nil, row(2, "old descriptor")))
			if response.GetError() != nil || len(response.GetUpdatedSchema().GetFields()) != 4 || response.GetUpdatedSchema().GetFields()[2].GetName() != "extra" {
				t.Fatalf("schema notification: %v", response)
			}
			request := appendRequest("", true, nil)
			request.GetProtoRows().WriterSchema.ProtoDescriptor.Field = append(writerSchema().Field,
				&descriptorpb.FieldDescriptorProto{Name: proto.String("extra"), Number: proto.Int32(3), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()})
			data := protowire.AppendTag(row(3, "new descriptor"), 3, protowire.BytesType)
			request.GetProtoRows().Rows.SerializedRows = [][]byte{protowire.AppendString(data, "added")}
			if response := send(t, stream, request); response.GetError() != nil {
				t.Fatal(response)
			}
			closeAppend(t, stream)
			// Publication must also work if the schema changes after rows were staged.
			query(t, rest, "ALTER TABLE "+dataset+".rows ADD COLUMN later STRING")
			switch kind {
			case storagepb.WriteStream_BUFFERED:
				if _, err := write.FlushRows(t.Context(), &storagepb.FlushRowsRequest{WriteStream: name}); err != nil {
					t.Fatal(err)
				}
			case storagepb.WriteStream_PENDING:
				if _, err := write.FinalizeWriteStream(t.Context(), &storagepb.FinalizeWriteStreamRequest{Name: name}); err != nil {
					t.Fatal(err)
				}
				response, err := write.BatchCommitWriteStreams(t.Context(), &storagepb.BatchCommitWriteStreamsRequest{Parent: path, WriteStreams: []string{name}})
				if err != nil || len(response.GetStreamErrors()) != 0 {
					t.Fatalf("commit: %v, %v", response, err)
				}
			}
			rows, err := rest.Query("SELECT id, extra, later, ARRAY_LENGTH(tags) FROM " + dataset + ".rows ORDER BY id").Read(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			for id := int64(1); id <= 3; id++ {
				var values []bigquery.Value
				if err := rows.Next(&values); err != nil {
					t.Fatal(err)
				}
				var extra bigquery.Value
				if id == 3 {
					extra = "added"
				}
				if !reflect.DeepEqual(values, []bigquery.Value{id, extra, nil, int64(0)}) {
					t.Fatalf("row %d: %v", id, values)
				}
			}
			var values []bigquery.Value
			if err := rows.Next(&values); err != iterator.Done {
				t.Fatalf("extra row: %v, %v", values, err)
			}
		})
	}
}

// tables.patch can relax a REQUIRED column without replacing the table.
// third_party/bigquery_docs/reference/rest/v2/tables.md
func TestWriteSchemaRelaxation(t *testing.T) {
	t.Parallel()
	rest, _, write := clients(t)
	const dataset = "storage_schema_relaxation"
	path := table(t, rest, dataset, "id INT64, name STRING NOT NULL")
	name := writeStream(t, write, path, storagepb.WriteStream_COMMITTED).Name
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if response := send(t, stream, appendRequest(name, true, nil, row(1, "required"))); response.GetError() != nil {
		t.Fatal(response)
	}
	if _, err := rest.Dataset(dataset).Table("rows").Update(t.Context(), bigquery.TableMetadataToUpdate{
		Schema: bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}, {Name: "name", Type: bigquery.StringFieldType}},
	}, ""); err != nil {
		t.Fatal(err)
	}
	data := protowire.AppendVarint(protowire.AppendTag(nil, 1, protowire.VarintType), 2)
	response := send(t, stream, appendRequest("", false, nil, data))
	if response.GetError() != nil || len(response.GetUpdatedSchema().GetFields()) != 2 || response.GetUpdatedSchema().GetFields()[1].GetMode() != storagepb.TableFieldSchema_NULLABLE {
		t.Fatalf("relaxed schema: %v", response)
	}
	closeAppend(t, stream)
	// A fresh connection on the same explicit stream must also use the current schema.
	stream, err = write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	data = protowire.AppendVarint(protowire.AppendTag(nil, 1, protowire.VarintType), 3)
	if response := send(t, stream, appendRequest(name, true, nil, data)); response.GetError() != nil {
		t.Fatal(response)
	}
	closeAppend(t, stream)
	rows, err := rest.Query("SELECT COUNT(*) FROM " + dataset + ".rows WHERE name IS NULL").Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var values []bigquery.Value
	if err := rows.Next(&values); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(values, []bigquery.Value{int64(2)}) {
		t.Fatalf("relaxed rows: %v", values)
	}
}

// tables.patch can add fields within a RECORD while preserving its existing fields.
// third_party/bigquery_docs/reference/rest/v2/tables.md
func TestWriteNestedSchemaAddition(t *testing.T) {
	t.Parallel()
	rest, _, write := clients(t)
	const dataset = "storage_schema_nested"
	path := table(t, rest, dataset, "id INT64, name STRING, rec STRUCT<value STRING>")
	name := writeStream(t, write, path, storagepb.WriteStream_BUFFERED).Name
	descriptor := writerSchema()
	descriptor.NestedType = []*descriptorpb.DescriptorProto{{Name: proto.String("Record"), Field: []*descriptorpb.FieldDescriptorProto{
		{Name: proto.String("value"), Number: proto.Int32(1), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()},
	}}}
	descriptor.Field = append(descriptor.Field, &descriptorpb.FieldDescriptorProto{Name: proto.String("rec"), Number: proto.Int32(3), Type: descriptorpb.FieldDescriptorProto_TYPE_MESSAGE.Enum(), TypeName: proto.String(".Row.Record"), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()})
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	appendRow := func(id int64, updated bool) *storagepb.AppendRowsResponse {
		request := appendRequest(name, true, nil)
		request.GetProtoRows().WriterSchema.ProtoDescriptor = descriptor
		nested := protowire.AppendString(protowire.AppendTag(nil, 1, protowire.BytesType), "value")
		if updated {
			nested = protowire.AppendString(protowire.AppendTag(nested, 2, protowire.BytesType), "added")
		}
		data := protowire.AppendBytes(protowire.AppendTag(row(id, "nested"), 3, protowire.BytesType), nested)
		request.GetProtoRows().Rows.SerializedRows = [][]byte{data}
		return send(t, stream, request)
	}
	if response := appendRow(1, false); response.GetError() != nil {
		t.Fatal(response)
	}
	if _, err := rest.Dataset(dataset).Table("rows").Update(t.Context(), bigquery.TableMetadataToUpdate{
		Schema: bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}, {Name: "name", Type: bigquery.StringFieldType},
			{Name: "rec", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{{Name: "value", Type: bigquery.StringFieldType}, {Name: "added", Type: bigquery.StringFieldType}, {Name: "tags", Type: bigquery.StringFieldType, Repeated: true}}}},
	}, ""); err != nil {
		t.Fatal(err)
	}
	if response := appendRow(2, false); response.GetError() != nil || len(response.GetUpdatedSchema().GetFields()) != 3 || len(response.GetUpdatedSchema().GetFields()[2].GetFields()) != 3 {
		t.Fatalf("nested update: %v", response)
	}
	descriptor.NestedType[0].Field = append(descriptor.NestedType[0].Field, &descriptorpb.FieldDescriptorProto{Name: proto.String("added"), Number: proto.Int32(2), Type: descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(), Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum()})
	if response := appendRow(3, true); response.GetError() != nil {
		t.Fatal(response)
	}
	closeAppend(t, stream)
	if _, err := write.FlushRows(t.Context(), &storagepb.FlushRowsRequest{WriteStream: name}); err != nil {
		t.Fatal(err)
	}
	rows, err := rest.Query("SELECT rec.value, rec.added, ARRAY_LENGTH(rec.tags) FROM " + dataset + ".rows ORDER BY id").Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	for _, added := range []bigquery.Value{nil, nil, "added"} {
		var values []bigquery.Value
		if err := rows.Next(&values); err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(values, []bigquery.Value{"value", added, int64(0)}) {
			t.Fatalf("nested row: %v", values)
		}
	}
}
