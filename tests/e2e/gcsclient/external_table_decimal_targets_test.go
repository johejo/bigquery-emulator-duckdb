package gcsclient

import (
	"fmt"
	"math/big"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"github.com/apache/arrow-go/v18/parquet"
	"github.com/apache/arrow-go/v18/parquet/schema"
)

// decimal_target_types selects the type a detected DECIMAL takes, by range rather than by order,
// as decimalTargetTypes in [tables](third_party/bigquery_docs/reference/rest/v2/tables.md)
// describes it: NUMERIC before BIGNUMERIC before STRING, each where it holds the precision and
// scale. It applies to DECIMALs in RECORDs and REPEATED columns too, as
// [Loading Parquet data](third_party/bigquery_docs/loading-data-cloud-storage-parquet.md)
// describes the conversion of each DECIMAL.
func TestExternalParquetDecimalTargetTypes(t *testing.T) {
	configure(t)
	required := parquet.Repetitions.Required
	// DECIMAL(9, 2), which NUMERIC holds.
	small := func(name string, repetition parquet.Repetition) schema.Node {
		return logicalNode(t, name, repetition, schema.NewDecimalLogicalType(9, 2), parquet.Types.Int32, -1)
	}
	// DECIMAL(40, 2), which NUMERIC does not hold.
	wide := func(name string) schema.Node {
		return logicalNode(t, name, required, schema.NewDecimalLogicalType(40, 2), parquet.Types.FixedLenByteArray, 17)
	}
	wideValue := []parquet.FixedLenByteArray{append(make([]byte, 15), 0x30, 0x39)} // 123.45
	record := func(name string, field schema.Node) schema.Node {
		return schema.MustGroup(schema.NewGroupNode(name, required, schema.FieldList{field}, -1))
	}
	objects(t, map[string][]byte{"decimals.parquet": parquetColumns(t,
		parquetColumn{node: small("small", required), values: []int32{-12345}},
		parquetColumn{node: wide("wide"), values: wideValue},
		parquetColumn{node: record("r", small("n", required)), values: []int32{-12345}},
		parquetColumn{node: record("rw", wide("n")), values: wideValue},
		parquetColumn{node: small("list", parquet.Repetitions.Repeated), values: []int32{-12345, 100}, levels: []int16{1, 1}, repetitions: []int16{0, 1}},
	)})
	client := start(t)
	for _, tc := range []struct {
		name        string
		targets     []bigquery.DecimalTargetType
		small, wide bigquery.FieldType
	}{
		{"bignumeric", []bigquery.DecimalTargetType{bigquery.BigNumericTargetType}, bigquery.BigNumericFieldType, bigquery.BigNumericFieldType},
		{"string", []bigquery.DecimalTargetType{bigquery.StringTargetType}, bigquery.StringFieldType, bigquery.StringFieldType},
		{"numeric_first", []bigquery.DecimalTargetType{bigquery.StringTargetType, bigquery.BigNumericTargetType, bigquery.NumericTargetType}, bigquery.NumericFieldType, bigquery.BigNumericFieldType},
		{"numeric_string", []bigquery.DecimalTargetType{bigquery.StringTargetType, bigquery.NumericTargetType}, bigquery.NumericFieldType, bigquery.StringFieldType},
	} {
		for _, api := range []bool{false, true} {
			name := fmt.Sprintf("decimals_%s_%t", tc.name, api)
			table := client.Dataset("gcs").Table(name)
			if api {
				if err := table.Create(t.Context(), &bigquery.TableMetadata{ExternalDataConfig: &bigquery.ExternalDataConfig{
					SourceFormat:       bigquery.Parquet,
					SourceURIs:         []string{"gs://bucket/decimals.parquet"},
					DecimalTargetTypes: tc.targets,
				}}); err != nil {
					t.Fatal(err)
				}
			} else {
				var targets []string
				for _, target := range tc.targets {
					targets = append(targets, "'"+string(target)+"'")
				}
				if err := externalDDL(t.Context(), client, "CREATE EXTERNAL TABLE gcs."+name+
					" OPTIONS (format = 'PARQUET', uris = ['gs://bucket/decimals.parquet'], decimal_target_types = ["+
					strings.Join(targets, ", ")+"])"); err != nil {
					t.Fatal(err)
				}
			}
			metadata, err := table.Metadata(t.Context())
			if err != nil {
				t.Fatal(err)
			}
			if !reflect.DeepEqual(metadata.ExternalDataConfig.DecimalTargetTypes, tc.targets) {
				t.Errorf("%s: decimalTargetTypes = %v, want %v", name, metadata.ExternalDataConfig.DecimalTargetTypes, tc.targets)
			}
			types := map[string]bigquery.FieldType{}
			for _, field := range metadata.Schema {
				typ := field.Type
				if typ == bigquery.RecordFieldType {
					typ = field.Schema[0].Type
				}
				types[field.Name] = typ
			}
			columns := []string{"small", "wide", "r", "rw", "list"}
			want := map[string]bigquery.FieldType{"small": tc.small, "wide": tc.wide, "r": tc.small, "rw": tc.wide, "list": tc.small}
			if !reflect.DeepEqual(types, want) {
				t.Fatalf("%s: schema = %v, want %v", name, types, want)
			}
			row := externalRow(t, client, "SELECT small, wide, r.n, rw.n, list[OFFSET(0)], list[OFFSET(1)] FROM gcs."+name)
			for i, value := range []string{"-123.45", "123.45", "-123.45", "123.45", "-123.45", "1"} {
				column := columns[min(i, len(columns)-1)]
				if want[column] == bigquery.StringFieldType {
					if row[i] != value {
						t.Errorf("%s: %s = %#v, want %q", name, column, row[i], value)
					}
					continue
				}
				expected, _ := new(big.Rat).SetString(value)
				if got, ok := row[i].(*big.Rat); !ok || got.Cmp(expected) != 0 {
					t.Errorf("%s: %s = %v, want %s", name, column, row[i], value)
				}
			}
		}
	}
}
