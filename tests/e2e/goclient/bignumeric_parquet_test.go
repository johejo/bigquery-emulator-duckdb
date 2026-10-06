package goclient

import (
	"context"
	"math/big"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"github.com/apache/arrow/go/v15/arrow"
	"github.com/apache/arrow/go/v15/arrow/array"
	"github.com/apache/arrow/go/v15/arrow/decimal128"
	"github.com/apache/arrow/go/v15/arrow/decimal256"
	"github.com/apache/arrow/go/v15/arrow/memory"
	"github.com/apache/arrow/go/v15/parquet/file"
	"github.com/apache/arrow/go/v15/parquet/pqarrow"
	"github.com/apache/arrow/go/v15/parquet/schema"
	"google.golang.org/api/iterator"
)

// BigQuery's Parquet export writes BIGNUMERIC as DECIMAL(76, 38).
var bigNumericParquet = &arrow.Decimal256Type{Precision: 76, Scale: 38}

// unscaled is the integer of `value` in units of 10^-scale.
func unscaled(t *testing.T, value string, scale int32) *big.Int {
	t.Helper()
	r := rat(t, value)
	r.Mul(r, new(big.Rat).SetInt(new(big.Int).Exp(big.NewInt(10), big.NewInt(int64(scale)), nil)))
	if !r.IsInt() {
		t.Fatalf("%s has more than %d fractional digits", value, scale)
	}
	return r.Num()
}

// decimal is the 256-bit two's complement of `value` in units of 10^-scale. Unlike
// decimal256.FromBigInt, it takes the smallest BIGNUMERIC, -2^255 units.
func decimal(t *testing.T, value string, scale int32) decimal256.Num {
	t.Helper()
	bits := new(big.Int).Lsh(big.NewInt(1), 256)
	n := new(big.Int).Mod(unscaled(t, value, scale), bits)
	mask := new(big.Int).SetUint64(^uint64(0))
	var words [4]uint64
	for i := range words {
		words[i] = new(big.Int).And(new(big.Int).Rsh(n, uint(64*i)), mask).Uint64()
	}
	return decimal256.New(words[3], words[2], words[1], words[0])
}

// writeParquet writes the columns `fields` of one record to a Parquet file and returns its path.
func writeParquet(t *testing.T, fields []arrow.Field, columns []arrow.Array) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "source.parquet")
	output, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	s := arrow.NewSchema(fields, nil)
	writer, err := pqarrow.NewFileWriter(s, output, nil, pqarrow.DefaultWriterProps())
	if err != nil {
		t.Fatal(err)
	}
	record := array.NewRecord(s, columns, int64(columns[0].Len()))
	defer record.Release()
	if err := writer.Write(record); err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	return path
}

func runJob(ctx context.Context, job interface {
	Wait(context.Context) (*bigquery.JobStatus, error)
}, err error) error {
	if err != nil {
		return err
	}
	status, err := job.Wait(ctx)
	if err != nil {
		return err
	}
	return status.Err()
}

func loadParquet(ctx context.Context, table *bigquery.Table, path string, s bigquery.Schema,
	targets ...bigquery.DecimalTargetType) error {
	source := bigquery.NewGCSReference(path)
	source.SourceFormat = bigquery.Parquet
	source.Schema = s
	loader := table.LoaderFrom(source)
	loader.DecimalTargetTypes = targets
	job, err := loader.Run(ctx)
	return runJob(ctx, job, err)
}

// A Parquet DECIMAL loads into BIGNUMERIC exactly, at any depth, whether it is as wide as
// BigQuery's export writes or narrower; and a table's BIGNUMERIC columns export as DECIMAL(76,
// 38), which loads back into the same values.
func TestBigNumericParquet(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_bignumeric_parquet")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	allocator := memory.NewGoAllocator()
	wide := array.NewDecimal256Builder(allocator, bigNumericParquet)
	for _, value := range []string{maxBigNumeric, minBigNumeric} {
		wide.Append(decimal(t, value, 38))
	}
	wide.AppendNull()
	structType := arrow.StructOf(arrow.Field{Name: "b", Type: bigNumericParquet, Nullable: true})
	structs := array.NewStructBuilder(allocator, structType)
	inner := structs.FieldBuilder(0).(*array.Decimal256Builder)
	structs.Append(true)
	inner.Append(decimal(t, tinyBigNumeric, 38))
	structs.Append(true)
	inner.AppendNull()
	structs.AppendNull()
	lists := array.NewListBuilder(allocator, bigNumericParquet)
	elements := lists.ValueBuilder().(*array.Decimal256Builder)
	lists.Append(true)
	elements.Append(decimal(t, "-"+tinyBigNumeric, 38))
	elements.Append(decimal(t, "1.5", 38))
	lists.Append(true)
	lists.AppendNull()
	// A scale other than BIGNUMERIC's.
	scaledType := &arrow.Decimal256Type{Precision: 50, Scale: 10}
	scaled := array.NewDecimal256Builder(allocator, scaledType)
	for _, value := range []string{"12345678901234567890123456789012345678.0123456789", "-0.0000000001"} {
		scaled.Append(decimal(t, value, 10))
	}
	scaled.AppendNull()
	// DuckDB reads a DECIMAL of up to 38 digits and an integer as numbers of their own.
	narrowType := &arrow.Decimal128Type{Precision: 10, Scale: 2}
	narrow := array.NewDecimal128Builder(allocator, narrowType)
	narrow.Append(decimal128.FromBigInt(unscaled(t, "-12345678.90", 2)))
	narrow.Append(decimal128.FromBigInt(unscaled(t, "0.01", 2)))
	narrow.AppendNull()
	integers := array.NewInt64Builder(allocator)
	integers.AppendValues([]int64{9223372036854775807, -9223372036854775808, 0}, []bool{true, true, false})
	ids := array.NewInt64Builder(allocator)
	ids.AppendValues([]int64{1, 2, 3}, nil)
	source := writeParquet(t, []arrow.Field{
		{Name: "id", Type: arrow.PrimitiveTypes.Int64},
		{Name: "n", Type: bigNumericParquet, Nullable: true},
		{Name: "s", Type: structType, Nullable: true},
		{Name: "a", Type: arrow.ListOf(bigNumericParquet), Nullable: true},
		{Name: "w", Type: scaledType, Nullable: true},
		{Name: "d", Type: narrowType, Nullable: true},
		{Name: "i", Type: arrow.PrimitiveTypes.Int64, Nullable: true},
	}, []arrow.Array{ids.NewArray(), wide.NewArray(), structs.NewArray(), lists.NewArray(),
		scaled.NewArray(), narrow.NewArray(), integers.NewArray()})

	bigNumeric := func(name string) *bigquery.FieldSchema {
		return &bigquery.FieldSchema{Name: name, Type: bigquery.BigNumericFieldType}
	}
	tableSchema := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		bigNumeric("n"),
		{Name: "s", Type: bigquery.RecordFieldType, Schema: bigquery.Schema{bigNumeric("b")}},
		{Name: "a", Type: bigquery.BigNumericFieldType, Repeated: true},
		bigNumeric("w"),
		bigNumeric("d"),
		bigNumeric("i"),
	}
	if err := loadParquet(ctx, dataset.Table("loaded"), source, tableSchema); err != nil {
		t.Fatalf("load: %v", err)
	}
	want := [][]string{
		{maxBigNumeric, tinyBigNumeric, "-" + tinyBigNumeric + ",1.5",
			"12345678901234567890123456789012345678.0123456789", "-12345678.9", "9223372036854775807"},
		{minBigNumeric, "NULL", "", "-0.0000000001", "0.01", "-9223372036854775808"},
		{"NULL", "NULL", "", "NULL", "NULL", "NULL"},
	}
	check := func(table string) {
		t.Helper()
		query := client.Query("SELECT n, s.b, a, w, d, i FROM " + table + " ORDER BY id")
		query.DefaultDatasetID = dataset.DatasetID
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", table, err)
		}
		for _, row := range want {
			var got []bigquery.Value
			if err := rows.Next(&got); err != nil {
				t.Fatalf("%s: %v", table, err)
			}
			for i, value := range row {
				if i == 2 {
					var elements []string
					values, _ := got[i].([]bigquery.Value)
					for _, element := range values {
						elements = append(elements, element.(*big.Rat).FloatString(38))
					}
					var wantElements []string
					for _, element := range strings.Split(value, ",") {
						if element != "" {
							wantElements = append(wantElements, rat(t, element).FloatString(38))
						}
					}
					if strings.Join(elements, ",") != strings.Join(wantElements, ",") {
						t.Errorf("%s: a: got %v, want %v", table, elements, wantElements)
					}
				} else if value == "NULL" {
					if got[i] != nil {
						t.Errorf("%s: column %d: got %v, want NULL", table, i, got[i])
					}
				} else if r, ok := got[i].(*big.Rat); !ok || r.Cmp(rat(t, value)) != 0 {
					t.Errorf("%s: column %d: got %v, want %s", table, i, got[i], value)
				}
			}
		}
		if err := rows.Next(new([]bigquery.Value)); err != iterator.Done {
			t.Errorf("%s: got another row (%v), want 3", table, err)
		}
	}
	check("loaded")

	// Schema detection picks BIGNUMERIC for a DECIMAL that NUMERIC cannot hold only when
	// decimalTargetTypes lists it.
	detected := dataset.Table("detected")
	if err := loadParquet(ctx, detected, source, nil, bigquery.NumericTargetType,
		bigquery.BigNumericTargetType); err != nil {
		t.Fatalf("load with detection: %v", err)
	}
	metadata, err := detected.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	for _, field := range metadata.Schema {
		if (field.Name == "n" || field.Name == "w") && field.Type != bigquery.BigNumericFieldType {
			t.Errorf("detected %s as %s, want BIGNUMERIC", field.Name, field.Type)
		}
	}

	extracted := filepath.Join(t.TempDir(), "extracted.parquet")
	destination := bigquery.NewGCSReference(extracted)
	destination.DestinationFormat = bigquery.Parquet
	extractor := dataset.Table("loaded").ExtractorTo(destination)
	job, err := extractor.Run(ctx)
	if err := runJob(ctx, job, err); err != nil {
		t.Fatalf("extract: %v", err)
	}
	reader, err := file.OpenParquetFile(extracted, false)
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	columns := reader.MetaData().Schema
	for i := 0; i < columns.NumColumns(); i++ {
		column := columns.Column(i)
		if column.Name() == "id" {
			continue
		}
		decimal, ok := column.LogicalType().(*schema.DecimalLogicalType)
		if !ok || decimal.Precision() != 76 || decimal.Scale() != 38 {
			t.Errorf("%s: got %v, want DECIMAL(76, 38)", column.Path(), column.LogicalType())
		}
	}
	if err := loadParquet(ctx, dataset.Table("reloaded"), extracted, tableSchema); err != nil {
		t.Fatalf("reload: %v", err)
	}
	check("reloaded")
}

// A load that would change a value or BigQuery would treat differently fails.
func TestBigNumericParquetErrors(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_bignumeric_parquet_errors")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	allocator := memory.NewGoAllocator()
	// 10^75 units of 1 is out of BIGNUMERIC's range.
	integral := &arrow.Decimal256Type{Precision: 76, Scale: 0}
	overflow := array.NewDecimal256Builder(allocator, integral)
	overflow.Append(decimal256.FromBigInt(new(big.Int).Exp(big.NewInt(10), big.NewInt(75), nil)))
	overflowing := writeParquet(t, []arrow.Field{{Name: "n", Type: integral, Nullable: true}},
		[]arrow.Array{overflow.NewArray()})
	wide := array.NewDecimal256Builder(allocator, bigNumericParquet)
	wide.Append(decimal(t, "1.5", 38))
	source := writeParquet(t, []arrow.Field{{Name: "n", Type: bigNumericParquet, Nullable: true}},
		[]arrow.Array{wide.NewArray()})

	for name, load := range map[string]func() error{
		"overflow": func() error {
			return loadParquet(ctx, dataset.Table("overflow"), overflowing,
				bigquery.Schema{{Name: "n", Type: bigquery.BigNumericFieldType}})
		},
		// The default decimalTargetTypes picks NUMERIC, which cannot hold every value.
		"detection": func() error { return loadParquet(ctx, dataset.Table("detection"), source, nil) },
		"numeric": func() error {
			return loadParquet(ctx, dataset.Table("numeric"), source,
				bigquery.Schema{{Name: "n", Type: bigquery.NumericFieldType}})
		},
	} {
		if err := load(); err == nil {
			t.Errorf("%s: load succeeded, want an error", name)
		}
	}
}
