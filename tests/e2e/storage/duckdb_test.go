package storage

import (
	"bytes"
	"context"
	"math"
	"math/big"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"slices"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery/storage/apiv1/storagepb"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/ipc"
	"github.com/johejo/bigquery-emulator-duckdb/internal/emulatorprocess"
	"github.com/linkedin/goavro/v2"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
)

// External DuckDB physical types still use the documented Storage Read wire types:
// third_party/bigquery_docs/reference/storage.md
// Expected values come from the fixture's literal values.
func TestReadDuckDBPhysicalTypes(t *testing.T) {
	t.Parallel()
	binary := os.Getenv("BQ_EMULATOR_BINARY")
	if binary == "" {
		t.Skip("run just e2e")
	}
	dir := t.TempDir()
	cmd := exec.CommandContext(t.Context(), "duckdb", filepath.Join(dir, "test.duckdb"))
	cmd.Stdin = strings.NewReader(`CREATE SCHEMA physical;
		CREATE TYPE label AS ENUM ('日本語', 'other');
		CREATE TABLE physical.rows AS SELECT i::BIGINT AS id, i::SMALLINT AS small,
			CASE i%4 WHEN 0 THEN 'NaN'::FLOAT WHEN 1 THEN 'Infinity'::FLOAT
				WHEN 2 THEN '-Infinity'::FLOAT ELSE '-0.0'::FLOAT END AS f,
			(CASE WHEN i%3=0 THEN NULL ELSE [12, -34] END)::INTEGER[2] AS fixed,
			TIMESTAMP_NS '1969-12-31 23:59:59.999999000' AS dt,
			CASE WHEN i%5=0 THEN NULL ELSE {'n': -12.34::DECIMAL(4,2),
				'b': '\x00\xFF'::BLOB, 'tags': ['日本語'::label]} END AS rec,
			CASE WHEN i%2=0 THEN NULL ELSE [1::SMALLINT, -2::SMALLINT] END AS items,
			'12345678-1234-5678-9abc-123456789abc'::UUID AS uuid
		FROM range(2050) t(i);
		CREATE TABLE physical.invalid AS SELECT
			{'i': 1::SMALLINT, 'dt': TIMESTAMP 'infinity'} AS rec;`)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("DuckDB setup: %v: %s", err, output)
	}
	startup, cancel := context.WithTimeout(t.Context(), 15*time.Second)
	defer cancel()
	p, err := emulatorprocess.Start(startup, binary, "", "--host", "127.0.0.1", "--port", "0",
		"--data-dir", dir, `--project={"projectId":"test"}`)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := p.Stop(); err != nil {
			t.Error(err)
		}
	})
	conn, err := grpc.NewClient(p.GRPC, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	read := storagepb.NewBigQueryReadClient(conn)
	for _, format := range []storagepb.DataFormat{storagepb.DataFormat_ARROW, storagepb.DataFormat_AVRO} {
		t.Run(format.String(), func(t *testing.T) {
			s := session(t, read, "projects/test/datasets/physical/tables/rows", format, nil, 1)
			var original []int64
			for _, offset := range []int64{0, 1023, 2047, 2050} {
				var ids []int64
				check := func(id, small int64, f float64, fixed []any, dt string, rec any, items []any, uuid string) {
					t.Helper()
					ids = append(ids, id)
					if small != id || dt != "1969-12-31 23:59:59.999999" || uuid != "12345678-1234-5678-9abc-123456789abc" {
						t.Fatalf("scalar values at %d: %d, %s, %s", id, small, dt, uuid)
					}
					if (id%4 == 0 && !math.IsNaN(f)) || (id%4 == 1 && !math.IsInf(f, 1)) ||
						(id%4 == 2 && !math.IsInf(f, -1)) || (id%4 == 3 && (f != 0 || !math.Signbit(f))) {
						t.Fatalf("float at %d: %v", id, f)
					}
					if id%3 == 0 {
						if len(fixed) != 0 {
							t.Fatalf("NULL fixed array at %d: %v", id, fixed)
						}
					} else if !reflect.DeepEqual(fixed, []any{int64(12), int64(-34)}) {
						t.Fatalf("fixed array at %d: %v", id, fixed)
					}
					if id%5 == 0 {
						if rec != nil {
							t.Fatalf("NULL struct at %d: %v", id, rec)
						}
					} else if !reflect.DeepEqual(rec, []any{"-12.34", []byte{0, 255}, []any{"日本語"}}) {
						t.Fatalf("struct at %d: %v", id, rec)
					}
					if id%2 == 0 {
						if len(items) != 0 {
							t.Fatalf("NULL list at %d: %v", id, items)
						}
					} else if !reflect.DeepEqual(items, []any{int64(1), int64(-2)}) {
						t.Fatalf("list at %d: %v", id, items)
					}
				}
				for _, block := range responses(t, read, s.Streams[0].Name, offset) {
					if format == storagepb.DataFormat_ARROW {
						data := append(bytes.Clone(s.GetArrowSchema().SerializedSchema), block.GetArrowRecordBatch().SerializedRecordBatch...)
						r, err := ipc.NewReader(bytes.NewReader(data))
						if err != nil {
							t.Fatal(err)
						}
						for r.Next() {
							batch := r.RecordBatch()
							list := func(column *array.List, row int) []any {
								if column.IsNull(row) {
									t.Fatal("NULL repeated value")
								}
								var values []any
								begin, end := column.ValueOffsets(row)
								for i := begin; i < end; i++ {
									if column.ListValues().IsNull(int(i)) {
										values = append(values, nil)
									} else {
										switch a := column.ListValues().(type) {
										case *array.Int64:
											values = append(values, a.Value(int(i)))
										case *array.String:
											values = append(values, a.Value(int(i)))
										}
									}
								}
								return values
							}
							for row := 0; row < int(batch.NumRows()); row++ {
								var rec any
								a := batch.Column(5).(*array.Struct)
								if !a.IsNull(row) {
									rec = []any{new(big.Rat).SetFrac(a.Field(0).(*array.Decimal128).Value(row).BigInt(), big.NewInt(1000000000)).FloatString(2),
										a.Field(1).(*array.Binary).Value(row), list(a.Field(2).(*array.List), row)}
								}
								check(batch.Column(0).(*array.Int64).Value(row), batch.Column(1).(*array.Int64).Value(row),
									batch.Column(2).(*array.Float64).Value(row), list(batch.Column(3).(*array.List), row),
									time.UnixMicro(int64(batch.Column(4).(*array.Timestamp).Value(row))).UTC().Format("2006-01-02 15:04:05.999999"),
									rec, list(batch.Column(6).(*array.List), row), batch.Column(7).(*array.String).Value(row))
							}
						}
						if err := r.Err(); err != nil {
							t.Fatal(err)
						}
						r.Release()
					} else {
						codec, err := goavro.NewCodec(s.GetAvroSchema().Schema)
						if err != nil {
							t.Fatal(err)
						}
						data := block.GetAvroRows().SerializedBinaryRows
						for range block.RowCount {
							value, remaining, err := codec.NativeFromBinary(data)
							if err != nil {
								t.Fatal(err)
							}
							data = remaining
							v := value.(map[string]any)
							var rec any
							if a := union(v["rec"]); a != nil {
								m := a.(map[string]any)
								rec = []any{union(m["n"]).(*big.Rat).FloatString(2), union(m["b"]), m["tags"]}
							}
							items := v["items"].([]any)
							for i := range items {
								items[i] = union(items[i])
							}
							check(union(v["id"]).(int64), union(v["small"]).(int64), union(v["f"]).(float64),
								v["fixed"].([]any), union(v["dt"]).(string), rec, items, union(v["uuid"]).(string))
						}
						if len(data) != 0 {
							t.Fatal("trailing Avro bytes")
						}
					}
				}
				if offset == 0 {
					original = ids
				} else if !slices.Equal(ids, original[offset:]) {
					t.Fatalf("offset %d: resumed rows differ", offset)
				}
				if len(ids) != 2050-int(offset) {
					t.Fatalf("offset %d: got %d rows", offset, len(ids))
				}
			}
			invalid := session(t, read, "projects/test/datasets/physical/tables/invalid", format, nil, 1)
			stream, err := read.ReadRows(t.Context(), &storagepb.ReadRowsRequest{ReadStream: invalid.Streams[0].Name})
			if err == nil {
				_, err = stream.Recv()
			}
			if status.Code(err) != codes.InvalidArgument {
				t.Fatalf("infinite DATETIME: %v", err)
			}
		})
	}
}
