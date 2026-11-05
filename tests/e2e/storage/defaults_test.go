package storage

import (
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"google.golang.org/api/iterator"
	"google.golang.org/grpc/codes"
	"google.golang.org/protobuf/encoding/protowire"
)

// A missing required field must still fail under NULL_VALUE; DEFAULT_VALUE supplies the
// column default. Explicit values in the same rejected batch must not be inserted.
// third_party/bigquery_docs/default-values.md
func TestWriteRequiredColumnDefault(t *testing.T) {
	t.Parallel()
	rest, _, write := clients(t)
	path := table(t, rest, "storage_required_default", "id INT64, name STRING DEFAULT 'required' NOT NULL")
	stream, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	missing := protowire.AppendVarint(protowire.AppendTag(nil, 1, protowire.VarintType), 1)
	request := appendRequest(path+"/streams/_default", true, nil, row(2, "rejected"), missing)
	if response := send(t, stream, request); response.GetError().GetCode() != int32(codes.InvalidArgument) || len(response.GetRowErrors()) != 1 || response.GetRowErrors()[0].GetIndex() != 1 {
		t.Fatal(response)
	}
	request = appendRequest("", false, nil, missing)
	request.MissingValueInterpretations = map[string]storagepb.AppendRowsRequest_MissingValueInterpretation{"name": storagepb.AppendRowsRequest_DEFAULT_VALUE}
	if response := send(t, stream, request); response.GetError() != nil {
		t.Fatal(response)
	}
	closeAppend(t, stream)
	rows, err := rest.Query("SELECT id, name FROM storage_required_default.rows").Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var values []bigquery.Value
	if err := rows.Next(&values); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(values, []bigquery.Value{int64(1), "required"}) {
		t.Fatal(values)
	}
	if err := rows.Next(&values); err != iterator.Done {
		t.Fatalf("extra row: %v, %v", values, err)
	}
}

// Expected values follow the schema-omission examples and interpretation settings in
// third_party/bigquery_docs/default-values.md. Connection/request setting lifetimes are
// specified by google/cloud/bigquery/storage/v1/storage.proto.
func TestWriteColumnDefaults(t *testing.T) {
	t.Parallel()
	for _, kind := range []storagepb.WriteStream_Type{
		storagepb.WriteStream_TYPE_UNSPECIFIED,
		storagepb.WriteStream_COMMITTED,
		storagepb.WriteStream_BUFFERED,
		storagepb.WriteStream_PENDING,
	} {
		t.Run(kind.String(), func(t *testing.T) {
			t.Parallel()
			rest, _, write := clients(t)
			dataset := "storage_defaults_" + kind.String()
			path := table(t, rest, dataset, "id INT64, name STRING DEFAULT 'default_name', extra STRING DEFAULT 'default_extra', required INT64 DEFAULT 42 NOT NULL, tags ARRAY<STRING> DEFAULT ['a', 'b'], rec STRUCT<n INT64, s STRING> DEFAULT (7, 'nested'), payload BYTES DEFAULT b'abc', ts TIMESTAMP DEFAULT TIMESTAMP '2020-01-02 03:04:05.123456+00', n NUMERIC DEFAULT 1.23")
			name := path + "/streams/_default"
			if kind != storagepb.WriteStream_TYPE_UNSPECIFIED {
				name = writeStream(t, write, path, kind).Name
			}
			stream, err := write.AppendRows(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			missingName := func(id uint64) []byte {
				return protowire.AppendVarint(protowire.AppendTag(nil, 1, protowire.VarintType), id)
			}
			appendRow := func(request *storagepb.AppendRowsRequest) {
				t.Helper()
				if response := send(t, stream, request); response.GetError() != nil {
					t.Fatal(response)
				}
			}
			appendRow(appendRequest(name, true, nil, missingName(1))) // Schema field missing from data: NULL.
			request := appendRequest("", false, nil, missingName(2))
			request.MissingValueInterpretations = map[string]storagepb.AppendRowsRequest_MissingValueInterpretation{"name": storagepb.AppendRowsRequest_DEFAULT_VALUE}
			appendRow(request)
			appendRow(appendRequest("", false, nil, missingName(3))) // The map does not persist.
			request = appendRequest("", false, nil, missingName(4))
			request.DefaultMissingValueInterpretation = storagepb.AppendRowsRequest_DEFAULT_VALUE
			appendRow(request)
			appendRow(appendRequest("", false, nil, missingName(5))) // The connection default persists.
			request = appendRequest("", false, nil, missingName(6))
			request.MissingValueInterpretations = map[string]storagepb.AppendRowsRequest_MissingValueInterpretation{"name": storagepb.AppendRowsRequest_NULL_VALUE}
			appendRow(request)
			appendRow(appendRequest("", false, nil, row(7, ""))) // Present empty string is not missing.
			request = appendRequest("", false, nil, missingName(8))
			request.DefaultMissingValueInterpretation = storagepb.AppendRowsRequest_NULL_VALUE
			appendRow(request)
			// Defaults are fixed when appended, not re-evaluated when staged rows publish.
			query(t, rest, "ALTER TABLE "+dataset+".rows ALTER COLUMN extra SET DEFAULT 'changed'")
			appendRow(appendRequest("", false, nil, row(9, "explicit")))
			closeAppend(t, stream)
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
			rows, err := rest.Query("SELECT id, name, extra, required, TO_JSON_STRING(tags), rec.n, rec.s, CAST(payload AS STRING), FORMAT_TIMESTAMP('%F %H:%M:%E6S', ts, 'UTC'), CAST(n AS STRING) FROM " + dataset + ".rows ORDER BY id").Read(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			for i, wantName := range []bigquery.Value{nil, "default_name", nil, "default_name", "default_name", nil, "", nil, "explicit"} {
				var values []bigquery.Value
				if err := rows.Next(&values); err != nil {
					t.Fatal(err)
				}
				extra := "default_extra"
				if i == 8 {
					extra = "changed"
				}
				want := []bigquery.Value{int64(i + 1), wantName, extra, int64(42), `["a","b"]`, int64(7), "nested", "abc", "2020-01-02 03:04:05.123456", "1.23"}
				if !reflect.DeepEqual(values, want) {
					t.Fatalf("row %d: got %v, want %v", i+1, values, want)
				}
			}
			var values []bigquery.Value
			if err := rows.Next(&values); err != iterator.Done {
				t.Fatalf("extra row: %v, %v", values, err)
			}
		})
	}
}
