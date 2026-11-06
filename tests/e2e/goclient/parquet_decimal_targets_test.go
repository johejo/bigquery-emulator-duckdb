package goclient

import (
	"math/big"
	"testing"

	"cloud.google.com/go/bigquery"
	"github.com/apache/arrow-go/v18/arrow"
	"github.com/apache/arrow-go/v18/arrow/array"
	"github.com/apache/arrow-go/v18/arrow/memory"
	"google.golang.org/api/iterator"
)

// STRING supports all DECIMAL precisions, including values outside BIGNUMERIC's
// range, and the conversion also applies inside records and lists.
func TestParquetDecimalStringNested(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	dataset := newDataset(t, client, "go_parquet_decimal_string_nested")
	typ := &arrow.Decimal256Type{Precision: 76, Scale: 0}
	recordType := arrow.StructOf(arrow.Field{Name: "n", Type: typ, Nullable: true})
	allocator := memory.NewGoAllocator()
	records := array.NewStructBuilder(allocator, recordType)
	defer records.Release()
	records.Append(true)
	// Beyond BIGNUMERIC's partially supported 39th integer digit.
	const want = "-999999999999999999999999999999999999999"
	records.FieldBuilder(0).(*array.Decimal256Builder).Append(decimal(t, want, 0))
	lists := array.NewListBuilder(allocator, typ)
	defer lists.Release()
	lists.Append(true)
	lists.ValueBuilder().(*array.Decimal256Builder).Append(decimal(t, want, 0))
	recordValues, listValues := records.NewArray(), lists.NewArray()
	defer recordValues.Release()
	defer listValues.Release()
	source := writeParquet(t, []arrow.Field{
		{Name: "s", Type: recordType, Nullable: true},
		{Name: "a", Type: arrow.ListOf(typ), Nullable: true},
	}, []arrow.Array{recordValues, listValues})
	table := dataset.Table("nested")
	if err := loadParquet(t.Context(), table, source, nil, bigquery.StringTargetType); err != nil {
		t.Fatal(err)
	}
	query := client.Query("SELECT s.n, a[OFFSET(0)] FROM `" + table.ProjectID + "." + table.DatasetID + "." + table.TableID + "`")
	rows, err := query.Read(t.Context())
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	for i, value := range row {
		if value != want {
			t.Errorf("column %d: got %v, want %s", i, value, want)
		}
	}
}

// JobConfigurationLoad forbids duplicate target types and only admits the three
// documented enum values, even when the destination schema is explicitly supplied.
func TestParquetDecimalInvalidTargets(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	dataset := newDataset(t, client, "go_parquet_decimal_invalid_targets")
	typ := &arrow.Decimal256Type{Precision: 10, Scale: 0}
	builder := array.NewDecimal256Builder(memory.NewGoAllocator(), typ)
	defer builder.Release()
	builder.Append(decimal(t, "1", 0))
	values := builder.NewArray()
	defer values.Release()
	source := writeParquet(t, []arrow.Field{{Name: "n", Type: typ, Nullable: true}}, []arrow.Array{values})
	for name, targets := range map[string][]bigquery.DecimalTargetType{
		"duplicate": {bigquery.NumericTargetType, bigquery.NumericTargetType},
		"unknown":   {bigquery.DecimalTargetType("INVALID")},
	} {
		t.Run(name, func(t *testing.T) {
			if err := loadParquet(t.Context(), dataset.Table(name), source,
				bigquery.Schema{{Name: "n", Type: bigquery.NumericFieldType}}, targets...); err == nil {
				t.Fatal("load succeeded, want an error")
			}
		})
	}
}

// Target selection follows the examples and precedence in
// third_party/bigquery_docs/reference/rest/v2/Job.md, including decimals DuckDB
// can read natively but NUMERIC cannot represent at their declared scale.
func TestParquetDecimalTargets(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	dataset := newDataset(t, client, "go_parquet_decimal_targets")
	for _, tc := range []struct {
		name             string
		precision, scale int32
		targets          []bigquery.DecimalTargetType
		want             bigquery.FieldType
	}{
		{"numeric", 38, 9, []bigquery.DecimalTargetType{bigquery.BigNumericTargetType, bigquery.NumericTargetType}, bigquery.NumericFieldType},
		{"integer_digits", 39, 9, []bigquery.DecimalTargetType{bigquery.NumericTargetType, bigquery.BigNumericTargetType}, bigquery.BigNumericFieldType},
		{"fractional_digits", 38, 10, []bigquery.DecimalTargetType{bigquery.NumericTargetType, bigquery.BigNumericTargetType}, bigquery.BigNumericFieldType},
		{"wide", 76, 38, []bigquery.DecimalTargetType{bigquery.NumericTargetType, bigquery.BigNumericTargetType}, bigquery.BigNumericFieldType},
		{"numeric_fallback", 76, 38, []bigquery.DecimalTargetType{bigquery.NumericTargetType}, bigquery.NumericFieldType},
		{"bignumeric_fallback", 76, 39, []bigquery.DecimalTargetType{bigquery.BigNumericTargetType}, bigquery.BigNumericFieldType},
		{"bignumeric_only", 10, 2, []bigquery.DecimalTargetType{bigquery.BigNumericTargetType}, bigquery.BigNumericFieldType},
		{"string_only", 10, 0, []bigquery.DecimalTargetType{bigquery.StringTargetType}, bigquery.StringFieldType},
		{"string_wide", 76, 0, []bigquery.DecimalTargetType{bigquery.StringTargetType}, bigquery.StringFieldType},
		{"string_fallback", 76, 0, []bigquery.DecimalTargetType{bigquery.BigNumericTargetType, bigquery.StringTargetType}, bigquery.StringFieldType},
		{"numeric_before_string", 10, 0, []bigquery.DecimalTargetType{bigquery.StringTargetType, bigquery.NumericTargetType}, bigquery.NumericFieldType},
	} {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()
			typ := &arrow.Decimal256Type{Precision: tc.precision, Scale: tc.scale}
			builder := array.NewDecimal256Builder(memory.NewGoAllocator(), typ)
			defer builder.Release()
			for _, value := range []string{"-123", "0", "123"} {
				builder.Append(decimal(t, value, tc.scale))
			}
			builder.AppendNull()
			values := builder.NewArray()
			defer values.Release()
			source := writeParquet(t, []arrow.Field{{Name: "n", Type: typ, Nullable: true}}, []arrow.Array{values})
			table := dataset.Table(tc.name)
			if err := loadParquet(t.Context(), table, source, nil, tc.targets...); err != nil {
				t.Fatal(err)
			}
			metadata, err := table.Metadata(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			if len(metadata.Schema) != 1 || metadata.Schema[0].Type != tc.want {
				t.Fatalf("schema = %v, want %s", metadata.Schema, tc.want)
			}
			query := client.Query("SELECT n FROM `" + table.ProjectID + "." + table.DatasetID + "." + table.TableID + "` ORDER BY n NULLS LAST")
			rows, err := query.Read(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			var row []bigquery.Value
			for _, want := range []string{"-123", "0", "123"} {
				if err := rows.Next(&row); err != nil {
					t.Fatal(err)
				}
				if tc.want == bigquery.StringFieldType {
					if row[0] != want {
						t.Errorf("got %v, want %s", row[0], want)
					}
				} else if value, ok := row[0].(*big.Rat); !ok || value.Cmp(rat(t, want)) != 0 {
					t.Errorf("got %v, want %s", row[0], want)
				}
			}
			if err := rows.Next(&row); err != nil {
				t.Fatal(err)
			}
			if row[0] != nil {
				t.Errorf("got %v, want NULL", row[0])
			}
			if err := rows.Next(&row); err != iterator.Done {
				t.Errorf("got another row (%v), want end of results", err)
			}
		})
	}
}
