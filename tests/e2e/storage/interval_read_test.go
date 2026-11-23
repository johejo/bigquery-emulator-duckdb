package storage

import (
	"bytes"
	"encoding/json"
	"os"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/ipc"
	"github.com/linkedin/goavro/v2"
)

// Decode recorded BigQuery responses and emulator responses with the same clients. Compare
// the decoded schemas and values rather than requiring identical IPC or Avro serialization.
func TestReadIntervals(t *testing.T) {
	t.Parallel()
	rest, read, _ := clients(t)
	data, err := os.ReadFile("testdata/storage_read_interval.api.answers.json")
	if err != nil {
		t.Fatal(err)
	}
	var observations struct {
		Cases map[string]struct {
			Steps struct {
				Session struct {
					ArrowSchema []byte
					AvroSchema  string
				}
				Read struct {
					Messages []struct {
						ArrowRecordBatch struct{ SerializedRecordBatch []byte }
						AvroRows         struct{ SerializedBinaryRows []byte }
					}
				}
			}
		}
	}
	if err := json.Unmarshal(data, &observations); err != nil {
		t.Fatal(err)
	}
	const interval = "INTERVAL '-0-3 7 -0:0:0.054321' YEAR TO SECOND"
	for _, test := range []struct{ name, columns, row string }{
		{"scalar", "value INTERVAL, missing INTERVAL", interval + ", NULL"},
		{"nested", "items ARRAY<INTERVAL>, empty_items ARRAY<INTERVAL>, rec STRUCT<value INTERVAL, missing INTERVAL>",
			"[" + interval + ", INTERVAL 25 HOUR], ARRAY<INTERVAL>[], STRUCT(" + interval + ", CAST(NULL AS INTERVAL))"},
	} {
		dataset := "storage_read_interval_" + test.name
		path := table(t, rest, dataset, test.columns)
		query(t, rest, "INSERT INTO "+dataset+".rows VALUES ("+test.row+")")
		for _, format := range []storagepb.DataFormat{storagepb.DataFormat_ARROW, storagepb.DataFormat_AVRO} {
			caseName := strings.ToLower(format.String()) + "_" + test.name
			t.Run(caseName, func(t *testing.T) {
				want := observations.Cases[caseName].Steps
				if len(want.Read.Messages) != 1 {
					t.Fatal("missing BigQuery observation")
				}
				msg := want.Read.Messages[0]
				s := session(t, read, path, format, nil, 1)
				blocks := responses(t, read, s.Streams[0].Name, 0)
				if len(blocks) != 1 || blocks[0].GetRowCount() != 1 {
					t.Fatalf("expected one row in one block, got %v", blocks)
				}
				if format == storagepb.DataFormat_ARROW {
					expected := intervalArrowReader(t, want.Session.ArrowSchema, msg.ArrowRecordBatch.SerializedRecordBatch)
					actual := intervalArrowReader(t, s.GetArrowSchema().GetSerializedSchema(), blocks[0].GetArrowRecordBatch().GetSerializedRecordBatch())
					if !actual.Schema().Equal(expected.Schema()) {
						t.Fatalf("Arrow schema = %v, want %v", actual.Schema(), expected.Schema())
					}
					if !array.RecordEqual(actual.RecordBatch(), expected.RecordBatch()) {
						t.Errorf("Arrow batch = %v, want %v", actual.RecordBatch(), expected.RecordBatch())
					}
					return
				}
				expected := intervalAvroRow(t, want.Session.AvroSchema, msg.AvroRows.SerializedBinaryRows)
				actual := intervalAvroRow(t, s.GetAvroSchema().GetSchema(), blocks[0].GetAvroRows().GetSerializedBinaryRows())
				if !reflect.DeepEqual(actual, expected) {
					t.Errorf("Avro row = %#v, want %#v", actual, expected)
				}
			})
		}
	}
}

func intervalArrowReader(t *testing.T, schema, batch []byte) *ipc.Reader {
	t.Helper()
	r, err := ipc.NewReader(bytes.NewReader(append(bytes.Clone(schema), batch...)))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(r.Release)
	if !r.Next() {
		t.Fatalf("no Arrow batch: %v", r.Err())
	}
	return r
}

func intervalAvroRow(t *testing.T, schema string, rows []byte) map[string]any {
	t.Helper()
	codec, err := goavro.NewCodec(schema)
	if err != nil {
		t.Fatal(err)
	}
	value, trailing, err := codec.NativeFromBinary(rows)
	if err != nil || len(trailing) != 0 {
		t.Fatalf("Avro decoding: %v, trailing bytes: %d", err, len(trailing))
	}
	row := value.(map[string]any)
	if rec, ok := row["rec"]; ok {
		// Ignore the synthetic name of the enclosing STRUCT, not the INTERVAL record name.
		row["rec"] = union(rec)
	}
	return row
}
