package storage

import (
	"context"
	"os"
	"reflect"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"github.com/johejo/bigquery-emulator-duckdb/internal/serverprocess"
	"google.golang.org/api/iterator"
	"google.golang.org/api/option"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/types/descriptorpb"
)

func changeRow(id int64, name, sequence string) []byte {
	data := row(id, name)
	data = protowire.AppendTag(data, 3, protowire.BytesType)
	data = protowire.AppendString(data, "UPSERT")
	data = protowire.AppendTag(data, 4, protowire.BytesType)
	return protowire.AppendString(data, sequence)
}

func changeRequest(name string, data ...[]byte) *storagepb.AppendRowsRequest {
	descriptor := writerSchema()
	for i, field := range []string{"_CHANGE_TYPE", "_CHANGE_SEQUENCE_NUMBER"} {
		descriptor.Field = append(descriptor.Field, &descriptorpb.FieldDescriptorProto{
			Name: proto.String(field), Number: proto.Int32(int32(3 + i)),
			Type:  descriptorpb.FieldDescriptorProto_TYPE_STRING.Enum(),
			Label: descriptorpb.FieldDescriptorProto_LABEL_OPTIONAL.Enum(),
		})
	}
	return &storagepb.AppendRowsRequest{WriteStream: name, Rows: &storagepb.AppendRowsRequest_ProtoRows{
		ProtoRows: &storagepb.AppendRowsRequest_ProtoData{
			WriterSchema: &storagepb.ProtoSchema{ProtoDescriptor: descriptor},
			Rows:         &storagepb.ProtoRows{SerializedRows: data},
		},
	}}
}

// The sequence numbers that order change data capture rows,
// third_party/bigquery_docs/change-data-capture.md, outlast the process with --data-dir, and
// belong to the table rather than its name.
func TestChangeSequencesAfterRestart(t *testing.T) {
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("run just e2e")
	}
	dataDir := t.TempDir()
	start := func() (*serverprocess.Emulator, *bigquery.Client, storagepb.BigQueryWriteClient) {
		t.Helper()
		ctx, cancel := context.WithTimeout(t.Context(), 15*time.Second)
		defer cancel()
		p, err := serverprocess.StartEmulator(ctx, binary, "", "--host", "127.0.0.1", "--http-port", "0", "--data-dir", dataDir, "--project", `{"projectId":"test"}`)
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
	const stream = "projects/test/datasets/storage_cdc/tables/rows/streams/_default"
	upsert := func(write storagepb.BigQueryWriteClient, name, sequence string) {
		t.Helper()
		append, err := write.AppendRows(t.Context())
		if err != nil {
			t.Fatal(err)
		}
		if response := send(t, append, changeRequest(stream, changeRow(1, name, sequence))); response.GetError() != nil {
			t.Fatal(response)
		}
		closeAppend(t, append)
	}
	names := func(rest *bigquery.Client, want ...string) {
		t.Helper()
		rows, err := rest.Query("SELECT name FROM storage_cdc.rows ORDER BY id").Read(t.Context())
		if err != nil {
			t.Fatal(err)
		}
		got := []string{}
		for {
			var row []bigquery.Value
			err := rows.Next(&row)
			if err == iterator.Done {
				break
			}
			if err != nil {
				t.Fatal(err)
			}
			got = append(got, row[0].(string))
		}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("names: %v, want %v", got, want)
		}
	}
	const table = "CREATE TABLE storage_cdc.rows (id INT64 PRIMARY KEY NOT ENFORCED, name STRING)"

	p, rest, write := start()
	if err := rest.Dataset("storage_cdc").Create(t.Context(), nil); err != nil {
		t.Fatal(err)
	}
	query(t, rest, table)
	upsert(write, "kept", "5")
	if err := p.Stop(); err != nil {
		t.Fatal(err)
	}
	_, rest, write = start()
	upsert(write, "older", "4")
	names(rest, "kept")

	query(t, rest, "DROP TABLE storage_cdc.rows")
	query(t, rest, table)
	upsert(write, "recreated", "4")
	names(rest, "recreated")
}
