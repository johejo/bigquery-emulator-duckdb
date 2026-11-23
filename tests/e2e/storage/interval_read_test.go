package storage

import (
	"bytes"
	"encoding/json"
	"os"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/ipc"
	"github.com/apache/arrow-go/v18/arrow/memory"
	"github.com/johejo/bigquery-emulator-duckdb/internal/arrowtest"
	"github.com/linkedin/goavro/v2"
	"google.golang.org/grpc/status"
)

type intervalObservation struct {
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
			Error struct {
				Code  string
				Error struct{ Message string }
			}
		}
	}
}

func intervalObservations(t *testing.T, path string) map[string]intervalObservation {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var observations struct {
		Cases map[string]intervalObservation
	}
	if err := json.Unmarshal(data, &observations); err != nil {
		t.Fatal(err)
	}
	return observations.Cases
}

// Decode recorded BigQuery responses and emulator responses with the same clients. Compare
// the decoded schemas and values rather than requiring identical IPC or Avro serialization.
func TestReadIntervals(t *testing.T) {
	t.Parallel()
	rest, read, _ := clients(t)
	observations := intervalObservations(t, "testdata/storage_read_interval.api.answers.json")
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
				want := observations[caseName].Steps
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

func TestReadIntervalPrecision(t *testing.T) {
	t.Parallel()
	rest, read, write := clients(t)
	observations := intervalObservations(t, "testdata/storage_read_interval_precision.api.answers.json")
	path := table(t, rest, "storage_read_interval_precision", "value INTERVAL")
	builder := array.NewRecordBuilder(memory.DefaultAllocator, arrow.NewSchema([]arrow.Field{
		{Name: "value", Type: arrow.FixedWidthTypes.MonthDayNanoInterval},
	}, nil))
	defer builder.Release()
	builder.Field(0).(*array.MonthDayNanoIntervalBuilder).Append(arrow.MonthDayNanoInterval{Nanoseconds: 1001})
	record := builder.NewRecordBatch()
	defer record.Release()
	schema, batch, err := arrowtest.Encode(record)
	if err != nil {
		t.Fatal(err)
	}
	writer, err := write.AppendRows(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	if r := send(t, writer, &storagepb.AppendRowsRequest{WriteStream: path + "/streams/_default",
		Rows: &storagepb.AppendRowsRequest_ArrowRows{ArrowRows: &storagepb.AppendRowsRequest_ArrowData{
			WriterSchema: &storagepb.ArrowSchema{SerializedSchema: schema},
			Rows:         &storagepb.ArrowRecordBatch{SerializedRecordBatch: batch},
		}},
	}); r.GetAppendResult() == nil {
		t.Fatalf("append: %v", r)
	}
	closeAppend(t, writer)
	for _, format := range []storagepb.DataFormat{storagepb.DataFormat_ARROW, storagepb.DataFormat_AVRO} {
		caseName := strings.ToLower(format.String()) + "_nanos"
		t.Run(caseName, func(t *testing.T) {
			want := observations[caseName].Steps
			// The recording's first response contains the appended value; the other row is NULL.
			if len(want.Read.Messages) != 2 {
				t.Fatal("missing BigQuery observation")
			}
			msg := want.Read.Messages[0]
			s := session(t, read, path, format, nil, 1)
			blocks := responses(t, read, s.Streams[0].Name, 0)
			if len(blocks) != 1 || blocks[0].GetRowCount() != 1 {
				t.Fatalf("expected one appended row, got %v", blocks)
			}
			if format == storagepb.DataFormat_ARROW {
				expected := intervalArrowReader(t, want.Session.ArrowSchema, msg.ArrowRecordBatch.SerializedRecordBatch)
				actual := intervalArrowReader(t, s.GetArrowSchema().GetSerializedSchema(), blocks[0].GetArrowRecordBatch().GetSerializedRecordBatch())
				if !array.RecordEqual(actual.RecordBatch(), expected.RecordBatch()) {
					t.Errorf("Arrow batch = %v, want %v", actual.RecordBatch(), expected.RecordBatch())
				}
			} else {
				expected := intervalAvroRow(t, want.Session.AvroSchema, msg.AvroRows.SerializedBinaryRows)
				actual := intervalAvroRow(t, s.GetAvroSchema().GetSchema(), blocks[0].GetAvroRows().GetSerializedBinaryRows())
				if !reflect.DeepEqual(actual, expected) {
					t.Errorf("Avro row = %#v, want %#v", actual, expected)
				}
			}
		})
	}
	t.Run("arrow_wide", func(t *testing.T) {
		const dataset = "storage_read_interval_wide"
		path := table(t, rest, dataset, "wide INTERVAL")
		query(t, rest, "INSERT INTO "+dataset+".rows VALUES (INTERVAL 3000000 HOUR)")
		s := session(t, read, path, storagepb.DataFormat_ARROW, nil, 1)
		rows, err := read.ReadRows(t.Context(), &storagepb.ReadRowsRequest{ReadStream: s.Streams[0].Name})
		if err != nil {
			t.Fatal(err)
		}
		_, err = rows.Recv()
		want := observations["arrow_wide"].Steps.Read.Error
		_, detail, ok := strings.Cut(want.Error.Message, "': ")
		if !ok || detail == "" || want.Code == "" {
			t.Fatal("missing BigQuery overflow observation")
		}
		if status.Code(err).String() != want.Code || !strings.HasSuffix(status.Convert(err).Message(), detail) {
			t.Fatalf("ReadRows error = %v, want %s with detail %q", err, want.Code, detail)
		}
	})
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
