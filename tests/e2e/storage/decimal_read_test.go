package storage

import (
	"bytes"
	"math/big"
	"testing"

	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/ipc"
	"github.com/linkedin/goavro/v2"
)

func TestReadParameterizedDecimals(t *testing.T) {
	t.Parallel()
	rest, read, _ := clients(t)
	path := table(t, rest, "storage_read_decimal_params", "n NUMERIC(8, 2), b BIGNUMERIC(50, 16), i NUMERIC(5)")
	query(t, rest, `INSERT INTO storage_read_decimal_params.rows VALUES
		(NUMERIC '-123.45', BIGNUMERIC '1234567890123456789012345678901234.0123456789012345', NUMERIC '12345')`)
	const bigValue = "1234567890123456789012345678901234.0123456789012345"
	for _, format := range []storagepb.DataFormat{storagepb.DataFormat_ARROW, storagepb.DataFormat_AVRO} {
		t.Run(format.String(), func(t *testing.T) {
			s := session(t, read, path, format, nil, 1)
			blocks := responses(t, read, s.Streams[0].Name, 0)
			if len(blocks) != 1 || blocks[0].GetRowCount() != 1 {
				t.Fatalf("expected one row in one block, got %v", blocks)
			}
			if format == storagepb.DataFormat_ARROW {
				data := append(bytes.Clone(s.GetArrowSchema().GetSerializedSchema()), blocks[0].GetArrowRecordBatch().GetSerializedRecordBatch()...)
				r, err := ipc.NewReader(bytes.NewReader(data))
				if err != nil {
					t.Fatal(err)
				}
				defer r.Release()
				if !r.Next() {
					t.Fatalf("no record batch: %v", r.Err())
				}
				wantTypes := []arrow.DataType{&arrow.Decimal128Type{Precision: 8, Scale: 2}, &arrow.Decimal256Type{Precision: 50, Scale: 16}, &arrow.Decimal128Type{Precision: 5, Scale: 0}}
				for i, want := range []string{"-123.45", bigValue, "12345"} {
					col := r.RecordBatch().Column(i)
					if !arrow.TypeEqual(col.DataType(), wantTypes[i]) || col.ValueStr(0) != want {
						t.Errorf("column %d: got %s %s, want %s %s", i, col.DataType(), col.ValueStr(0), wantTypes[i], want)
					}
				}
				return
			}
			codec, err := goavro.NewCodec(s.GetAvroSchema().GetSchema())
			if err != nil {
				t.Fatal(err)
			}
			decoded, trailing, err := codec.NativeFromBinary(blocks[0].GetAvroRows().GetSerializedBinaryRows())
			if err != nil || len(trailing) != 0 {
				t.Fatalf("Avro decoding: %v, trailing bytes: %d", err, len(trailing))
			}
			row := decoded.(map[string]any)
			for name, want := range map[string]string{"n": "-123.45", "b": bigValue, "i": "12345"} {
				expected, _ := new(big.Rat).SetString(want)
				got := union(row[name]).(*big.Rat)
				if got.Cmp(expected) != 0 {
					t.Errorf("%s = %s, want %s", name, got, want)
				}
			}
		})
	}
}
