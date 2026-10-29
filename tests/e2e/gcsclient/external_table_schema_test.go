package gcsclient

import (
	"errors"
	"math/big"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"cloud.google.com/go/civil"
	"github.com/apache/arrow-go/v18/parquet"
	"github.com/apache/arrow-go/v18/parquet/file"
	"github.com/apache/arrow-go/v18/parquet/schema"
	"google.golang.org/api/googleapi"
	"google.golang.org/api/iterator"
)

// A column of a Parquet fixture: its node, its values as the slice its physical type's writer
// takes, and the definition and repetition levels of an optional or repeated column.
type parquetColumn struct {
	node        schema.Node
	values      any
	levels      []int16
	repetitions []int16
}

func logicalNode(t *testing.T, name string, repetition parquet.Repetition, logical schema.LogicalType, physical parquet.Type, length int) schema.Node {
	t.Helper()
	node, err := schema.NewPrimitiveNodeLogical(name, repetition, logical, physical, length, -1)
	if err != nil {
		t.Fatal(err)
	}
	return node
}

func parquetColumns(t *testing.T, columns ...parquetColumn) []byte {
	t.Helper()
	var fields schema.FieldList
	for _, column := range columns {
		fields = append(fields, column.node)
	}
	return externalParquetFile(t, fields, func(group file.SerialRowGroupWriter) {
		for _, column := range columns {
			chunk, err := group.NextColumn()
			if err != nil {
				t.Fatal(err)
			}
			switch writer := chunk.(type) {
			case *file.BooleanColumnChunkWriter:
				_, err = writer.WriteBatch(column.values.([]bool), column.levels, column.repetitions)
			case *file.Int32ColumnChunkWriter:
				_, err = writer.WriteBatch(column.values.([]int32), column.levels, column.repetitions)
			case *file.Int64ColumnChunkWriter:
				_, err = writer.WriteBatch(column.values.([]int64), column.levels, column.repetitions)
			case *file.Int96ColumnChunkWriter:
				_, err = writer.WriteBatch(column.values.([]parquet.Int96), column.levels, column.repetitions)
			case *file.Float32ColumnChunkWriter:
				_, err = writer.WriteBatch(column.values.([]float32), column.levels, column.repetitions)
			case *file.Float64ColumnChunkWriter:
				_, err = writer.WriteBatch(column.values.([]float64), column.levels, column.repetitions)
			case *file.ByteArrayColumnChunkWriter:
				_, err = writer.WriteBatch(column.values.([]parquet.ByteArray), column.levels, column.repetitions)
			case *file.FixedLenByteArrayColumnChunkWriter:
				_, err = writer.WriteBatch(column.values.([]parquet.FixedLenByteArray), column.levels, column.repetitions)
			default:
				t.Fatalf("no writer for %T", chunk)
			}
			if err != nil {
				t.Fatal(err)
			}
		}
	})
}

// objects serves each named object of the bucket "bucket" and lists them, counting downloads.
func objects(t *testing.T, files map[string][]byte) *atomic.Int32 {
	t.Helper()
	var downloads atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/storage/v1/b/bucket/o" {
			w.Header().Set("Content-Type", "application/json")
			var items []string
			for name := range files {
				if strings.HasPrefix(name, r.URL.Query().Get("prefix")) {
					items = append(items, `{"name":"`+name+`"}`)
				}
			}
			_, _ = w.Write([]byte(`{"items":[` + strings.Join(items, ",") + `]}`))
			return
		}
		contents, ok := files[strings.TrimPrefix(r.URL.Path, "/storage/v1/b/bucket/o/")]
		if !ok {
			http.NotFound(w, r)
			return
		}
		downloads.Add(1)
		_, _ = w.Write(contents)
	}))
	t.Cleanup(server.Close)
	t.Setenv("STORAGE_EMULATOR_HOST", server.URL)
	return &downloads
}

func externalSchema(t *testing.T, table *bigquery.Table) map[string]bigquery.FieldType {
	t.Helper()
	metadata, err := table.Metadata(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	types := map[string]bigquery.FieldType{}
	for _, field := range metadata.Schema {
		types[field.Name] = field.Type
	}
	return types
}

func externalRow(t *testing.T, client *bigquery.Client, query string) []bigquery.Value {
	t.Helper()
	it, err := client.Query(query).Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := it.Next(&row); err != nil {
		t.Fatal(err)
	}
	if err := it.Next(&[]bigquery.Value{}); err != iterator.Done {
		t.Fatalf("extra row: %v", err)
	}
	return row
}

// Parquet types convert as the Parquet conversions of
// [Loading Parquet data](third_party/bigquery_docs/loading-data-cloud-storage-parquet.md)
// list them. Whether a REQUIRED Parquet column is detected as REQUIRED is an unverified case.
func TestExternalParquetDetectedTypes(t *testing.T) {
	configure(t)
	required, optional := parquet.Repetitions.Required, parquet.Repetitions.Optional
	stamp := time.Date(2024, 1, 2, 3, 4, 5, 6000, time.UTC)
	// 2024-01-02 is Julian day 2460312.
	int96 := parquet.NewInt96([3]uint32{0, 0, 2460312})
	int96.SetNanoSeconds(int64(3*time.Hour + 4*time.Minute + 5*time.Second + 6*time.Microsecond))
	contents := parquetColumns(t,
		parquetColumn{node: schema.NewInt32Node("i32", required, -1), values: []int32{-2}},
		parquetColumn{node: logicalNode(t, "u8", required, schema.NewIntLogicalType(8, false), parquet.Types.Int32, -1), values: []int32{255}},
		parquetColumn{node: schema.NewInt64Node("i64", optional, -1), values: []int64{1 << 40}, levels: []int16{1}},
		parquetColumn{node: schema.NewBooleanNode("flag", required, -1), values: []bool{true}},
		parquetColumn{node: logicalNode(t, "d", required, schema.DateLogicalType{}, parquet.Types.Int32, -1), values: []int32{int32(stamp.Unix() / 86400)}},
		parquetColumn{node: logicalNode(t, "t_ms", required, schema.NewTimeLogicalType(true, schema.TimeUnitMillis), parquet.Types.Int32, -1), values: []int32{45296789}},
		parquetColumn{node: logicalNode(t, "t_us", required, schema.NewTimeLogicalType(false, schema.TimeUnitMicros), parquet.Types.Int64, -1), values: []int64{3723000004}},
		parquetColumn{node: logicalNode(t, "ts_ms", required, schema.NewTimestampLogicalType(true, schema.TimeUnitMillis), parquet.Types.Int64, -1), values: []int64{stamp.UnixMilli()}},
		parquetColumn{node: logicalNode(t, "ts_us", required, schema.NewTimestampLogicalType(true, schema.TimeUnitMicros), parquet.Types.Int64, -1), values: []int64{stamp.UnixMicro()}},
		parquetColumn{node: schema.NewInt96Node("ts_96", required, -1), values: []parquet.Int96{int96}},
		parquetColumn{node: schema.NewFloat32Node("f32", required, -1), values: []float32{1.5}},
		parquetColumn{node: schema.NewFloat64Node("f64", required, -1), values: []float64{-0.25}},
		parquetColumn{node: logicalNode(t, "s", required, schema.StringLogicalType{}, parquet.Types.ByteArray, -1), values: []parquet.ByteArray{[]byte("日本語")}},
		parquetColumn{node: schema.NewByteArrayNode("b", required, -1), values: []parquet.ByteArray{{0, 255}}},
		parquetColumn{node: schema.NewFixedLenByteArrayNode("fixed", required, 2, -1), values: []parquet.FixedLenByteArray{{1, 2}}},
		parquetColumn{node: logicalNode(t, "n32", required, schema.NewDecimalLogicalType(9, 2), parquet.Types.Int32, -1), values: []int32{-12345}},
		parquetColumn{node: logicalNode(t, "n64", required, schema.NewDecimalLogicalType(38, 9), parquet.Types.FixedLenByteArray, 16),
			values: []parquet.FixedLenByteArray{append(make([]byte, 12), 0x49, 0x96, 0x02, 0xd2)}},
	)
	downloads := objects(t, map[string][]byte{"types.parquet": contents})
	client := start(t)
	if err := externalDDL(t.Context(), client, `CREATE EXTERNAL TABLE gcs.types
		OPTIONS (format = 'PARQUET', uris = ['gs://bucket/types.parquet'])`); err != nil {
		t.Fatal(err)
	}
	if got := downloads.Load(); got != 1 {
		t.Fatalf("creation downloads = %d, want 1", got)
	}
	want := map[string]bigquery.FieldType{
		"i32": bigquery.IntegerFieldType, "u8": bigquery.IntegerFieldType, "i64": bigquery.IntegerFieldType,
		"flag": bigquery.BooleanFieldType, "d": bigquery.DateFieldType,
		"t_ms": bigquery.TimeFieldType, "t_us": bigquery.TimeFieldType,
		"ts_ms": bigquery.TimestampFieldType, "ts_us": bigquery.TimestampFieldType, "ts_96": bigquery.TimestampFieldType,
		"f32": bigquery.FloatFieldType, "f64": bigquery.FloatFieldType,
		"s": bigquery.StringFieldType, "b": bigquery.BytesFieldType, "fixed": bigquery.BytesFieldType,
		"n32": bigquery.NumericFieldType, "n64": bigquery.NumericFieldType,
	}
	if got := externalSchema(t, client.Dataset("gcs").Table("types")); !reflect.DeepEqual(got, want) {
		t.Fatalf("detected schema = %v, want %v", got, want)
	}
	row := externalRow(t, client, `SELECT i32, u8, i64, flag, d, t_ms, t_us, ts_ms, ts_us, ts_96, f32, f64,
		s, b, fixed, n32, n64 FROM gcs.types`)
	expected := []bigquery.Value{
		int64(-2), int64(255), int64(1 << 40), true, civil.Date{Year: 2024, Month: 1, Day: 2},
		civil.Time{Hour: 12, Minute: 34, Second: 56, Nanosecond: 789000000},
		civil.Time{Hour: 1, Minute: 2, Second: 3, Nanosecond: 4000},
		stamp.Truncate(time.Millisecond), stamp, stamp, 1.5, -0.25, "日本語", []byte{0, 255}, []byte{1, 2},
		big.NewRat(-12345, 100), big.NewRat(1234567890, 1000000000),
	}
	for i := range expected {
		if rat, ok := expected[i].(*big.Rat); ok {
			if got, ok := row[i].(*big.Rat); !ok || got.Cmp(rat) != 0 {
				t.Errorf("column %d = %v, want %v", i, row[i], rat)
			}
		} else if timestamp, ok := expected[i].(time.Time); ok {
			if got, ok := row[i].(time.Time); !ok || !got.Equal(timestamp) {
				t.Errorf("column %d = %v, want %v", i, row[i], timestamp)
			}
		} else if !reflect.DeepEqual(row[i], expected[i]) {
			t.Errorf("column %d = %#v, want %#v", i, row[i], expected[i])
		}
	}

	// An explicit schema takes the types BigQuery detects, without coercions.
	if err := externalDDL(t.Context(), client, `CREATE EXTERNAL TABLE gcs.declared (i32 INT64, d DATE, n32 NUMERIC)
		OPTIONS (format = 'PARQUET', uris = ['gs://bucket/types.parquet'])`); err != nil {
		t.Fatal(err)
	}
	if got := externalRow(t, client, "SELECT i32, d FROM gcs.declared"); !reflect.DeepEqual(got, []bigquery.Value{int64(-2), civil.Date{Year: 2024, Month: 1, Day: 2}}) {
		t.Fatalf("declared row = %v", got)
	}
	if err := externalDDL(t.Context(), client, `CREATE EXTERNAL TABLE gcs.coerced (d STRING)
		OPTIONS (format = 'PARQUET', uris = ['gs://bucket/types.parquet'])`); err != nil {
		t.Fatal(err)
	}
	if _, err := client.Query("SELECT d FROM gcs.coerced").Read(t.Context()); err == nil {
		t.Fatal("DATE column read as STRING")
	}
}

// Through tables.insert, a table without a schema takes the one its sources have, as does
// autodetect, which BigQuery's API documents.
func TestExternalParquetDetectedThroughAPI(t *testing.T) {
	configure(t)
	contents := parquetColumns(t, parquetColumn{node: schema.NewInt64Node("id", parquet.Repetitions.Optional, -1), values: []int64{7}, levels: []int16{1}})
	objects(t, map[string][]byte{"data.parquet": contents})
	client := start(t)
	for _, autodetect := range []bool{false, true} {
		table := client.Dataset("gcs").Table(map[bool]string{false: "implicit", true: "autodetect"}[autodetect])
		config := &bigquery.ExternalDataConfig{SourceFormat: bigquery.Parquet, SourceURIs: []string{"gs://bucket/data.parquet"}, AutoDetect: autodetect}
		if err := table.Create(t.Context(), &bigquery.TableMetadata{ExternalDataConfig: config}); err != nil {
			t.Fatal(err)
		}
		if got := externalSchema(t, table); !reflect.DeepEqual(got, map[string]bigquery.FieldType{"id": bigquery.IntegerFieldType}) {
			t.Fatalf("autodetect=%v schema = %v", autodetect, got)
		}
		externalRows(t, client, "SELECT id FROM gcs."+table.TableID, []int64{7})
	}
}

// BigQuery takes the schema of the alphabetically last file, by its whole path.
func TestExternalParquetDetectsLastFile(t *testing.T) {
	configure(t)
	column := func(name string) []byte {
		return parquetColumns(t, parquetColumn{node: schema.NewInt64Node(name, parquet.Repetitions.Optional, -1), values: []int64{1}, levels: []int16{1}})
	}
	objects(t, map[string][]byte{"00/a.parquet": column("a"), "00/z.parquet": column("z"), "01/b.parquet": column("b")})
	client := start(t)
	if err := externalDDL(t.Context(), client, `CREATE EXTERNAL TABLE gcs.last
		OPTIONS (format = 'PARQUET', uris = ['gs://bucket/01/*', 'gs://bucket/00/*'])`); err != nil {
		t.Fatal(err)
	}
	if got := externalSchema(t, client.Dataset("gcs").Table("last")); !reflect.DeepEqual(got, map[string]bigquery.FieldType{"b": bigquery.IntegerFieldType}) {
		t.Fatalf("schema = %v", got)
	}
}

// Columns whose type BigQuery does not convert, or converts by options the emulator does not
// take, fail rather than take another type.
func TestExternalParquetUndetectedTypes(t *testing.T) {
	configure(t)
	required := parquet.Repetitions.Required
	group, err := schema.NewGroupNode("g", required, schema.FieldList{schema.NewInt64Node("x", required, -1)}, -1)
	if err != nil {
		t.Fatal(err)
	}
	files := map[string][]byte{
		"enum.parquet":     parquetColumns(t, parquetColumn{node: logicalNode(t, "e", required, schema.EnumLogicalType{}, parquet.Types.ByteArray, -1), values: []parquet.ByteArray{[]byte("A")}}),
		"nanos.parquet":    parquetColumns(t, parquetColumn{node: logicalNode(t, "ts", required, schema.NewTimestampLogicalType(true, schema.TimeUnitNanos), parquet.Types.Int64, -1), values: []int64{1}}),
		"wide.parquet":     parquetColumns(t, parquetColumn{node: logicalNode(t, "n", required, schema.NewDecimalLogicalType(18, 10), parquet.Types.Int64, -1), values: []int64{1}}),
		"repeated.parquet": parquetColumns(t, parquetColumn{node: schema.NewInt64Node("r", parquet.Repetitions.Repeated, -1), values: []int64{1}, levels: []int16{1}, repetitions: []int16{0}}),
		"group.parquet": externalParquetFile(t, schema.FieldList{group}, func(rows file.SerialRowGroupWriter) {
			chunk, err := rows.NextColumn()
			if err != nil {
				t.Fatal(err)
			}
			if _, err := chunk.(*file.Int64ColumnChunkWriter).WriteBatch([]int64{1}, nil, nil); err != nil {
				t.Fatal(err)
			}
		}),
	}
	objects(t, files)
	client := start(t)
	for name := range files {
		err := externalDDL(t.Context(), client, `CREATE EXTERNAL TABLE gcs.t
			OPTIONS (format = 'PARQUET', uris = ['gs://bucket/`+name+`'])`)
		if err == nil || !strings.Contains(err.Error(), "detecting the type") {
			t.Errorf("%s: %v", name, err)
		}
	}
	if _, err := client.Dataset("gcs").Table("t").Metadata(t.Context()); !isNotFound(err) {
		t.Fatalf("table after failed detection: %v", err)
	}
}

func isNotFound(err error) bool {
	var apiErr *googleapi.Error
	return errors.As(err, &apiErr) && apiErr.Code == http.StatusNotFound
}

// IF NOT EXISTS keeps an existing table without reading the sources, and OR REPLACE replaces an
// external table, detecting the schema of its new sources.
func TestExternalParquetCreateModes(t *testing.T) {
	configure(t)
	column := func(name string) []byte {
		return parquetColumns(t, parquetColumn{node: schema.NewInt64Node(name, parquet.Repetitions.Optional, -1), values: []int64{1}, levels: []int16{1}})
	}
	downloads := objects(t, map[string][]byte{"a.parquet": column("a"), "b.parquet": column("b")})
	client := start(t)
	ctx := t.Context()
	for _, sql := range []string{
		"CREATE EXTERNAL TABLE gcs.t OPTIONS (format = 'PARQUET', uris = ['gs://bucket/a.parquet'])",
		"CREATE EXTERNAL TABLE IF NOT EXISTS gcs.t OPTIONS (format = 'PARQUET', uris = ['gs://bucket/b.parquet'])",
	} {
		if err := externalDDL(ctx, client, sql); err != nil {
			t.Fatal(err)
		}
	}
	if got := downloads.Load(); got != 1 {
		t.Fatalf("downloads = %d, want 1", got)
	}
	table := client.Dataset("gcs").Table("t")
	if got := externalSchema(t, table); !reflect.DeepEqual(got, map[string]bigquery.FieldType{"a": bigquery.IntegerFieldType}) {
		t.Fatalf("schema after IF NOT EXISTS = %v", got)
	}
	if err := externalDDL(ctx, client, "CREATE OR REPLACE EXTERNAL TABLE gcs.t OPTIONS (format = 'PARQUET', uris = ['gs://bucket/b.parquet'])"); err != nil {
		t.Fatal(err)
	}
	if got := externalSchema(t, table); !reflect.DeepEqual(got, map[string]bigquery.FieldType{"b": bigquery.IntegerFieldType}) {
		t.Fatalf("schema after OR REPLACE = %v", got)
	}
	if err := externalDDL(ctx, client, "CREATE EXTERNAL TABLE gcs.t OPTIONS (format = 'PARQUET', uris = ['gs://bucket/a.parquet'])"); err == nil {
		t.Fatalf("duplicate creation: %v", err)
	}
}

// DROP EXTERNAL TABLE drops only an external table, as the
// [DDL reference](third_party/bigquery_docs/reference/standard-sql/data-definition-language.md)
// states.
func TestDropExternalTable(t *testing.T) {
	configure(t)
	client := start(t)
	ctx := t.Context()
	for _, sql := range []string{
		"CREATE EXTERNAL TABLE gcs.external (id INT64) OPTIONS (format = 'PARQUET', uris = ['gs://bucket/data.parquet'])",
		"CREATE TABLE gcs.native (id INT64)",
		"CREATE VIEW gcs.v AS SELECT 1 AS id",
	} {
		if err := externalDDL(ctx, client, sql); err != nil {
			t.Fatal(err)
		}
	}
	for _, name := range []string{"native", "v"} {
		err := externalDDL(ctx, client, "DROP EXTERNAL TABLE gcs."+name)
		if err == nil || !strings.Contains(err.Error(), "An external table was expected") {
			t.Errorf("DROP EXTERNAL TABLE of %s: %v", name, err)
		}
		if _, err := client.Dataset("gcs").Table(name).Metadata(ctx); err != nil {
			t.Errorf("%s after rejected drop: %v", name, err)
		}
	}
	if err := externalDDL(ctx, client, "DROP EXTERNAL TABLE gcs.external"); err != nil {
		t.Fatal(err)
	}
	if _, err := client.Dataset("gcs").Table("external").Metadata(ctx); !isNotFound(err) {
		t.Fatalf("external table after drop: %v", err)
	}
	if err := externalDDL(ctx, client, "DROP EXTERNAL TABLE gcs.external"); err == nil {
		t.Fatal("DROP EXTERNAL TABLE of a missing table succeeded")
	}
	if err := externalDDL(ctx, client, "DROP EXTERNAL TABLE IF EXISTS gcs.external"); err != nil {
		t.Fatal(err)
	}
}
