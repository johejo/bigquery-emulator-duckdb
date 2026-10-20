package goclient

import (
	"context"
	"math/big"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

const (
	maxBigNumeric  = "578960446186580977117854925043439539266.34992332820282019728792003956564819967"
	minBigNumeric  = "-578960446186580977117854925043439539266.34992332820282019728792003956564819968"
	tinyBigNumeric = "0.00000000000000000000000000000000000001"
)

func rat(t *testing.T, s string) *big.Rat {
	t.Helper()
	r, ok := new(big.Rat).SetString(s)
	if !ok {
		t.Fatalf("invalid number %q", s)
	}
	return r
}

// BIGNUMERIC keeps its whole range and 38 fractional digits in columns, nested ones included,
// whether written by DML, a streaming insert or a query parameter.
func TestBigNumericColumns(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_bignumeric")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	read := func(sql string, parameters ...bigquery.QueryParameter) *bigquery.RowIterator {
		t.Helper()
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		query.Parameters = parameters
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		return rows
	}

	read("CREATE TABLE t (id INT64, n BIGNUMERIC, a ARRAY<BIGNUMERIC>, s STRUCT<b BIGNUMERIC>)")
	read("INSERT t VALUES (1, BIGNUMERIC '" + maxBigNumeric + "', [BIGNUMERIC '" + minBigNumeric +
		"'], STRUCT(BIGNUMERIC '" + tinyBigNumeric + "'))")
	metadata, err := dataset.Table("t").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	schema := metadata.Schema
	err = dataset.Table("t").Inserter().Put(ctx, &bigquery.ValuesSaver{
		Schema: schema,
		Row: []bigquery.Value{int64(2), rat(t, minBigNumeric), []bigquery.Value{rat(t, maxBigNumeric)},
			[]bigquery.Value{rat(t, "-"+tinyBigNumeric)}},
	})
	if err != nil {
		t.Fatalf("Put: %v", err)
	}
	read("INSERT t (id, n) SELECT 3, @n", bigquery.QueryParameter{Name: "n", Value: &bigquery.QueryParameterValue{
		Type:  bigquery.StandardSQLDataType{TypeKind: "BIGNUMERIC"},
		Value: tinyBigNumeric,
	}})

	want := [][]string{
		{"1", maxBigNumeric, minBigNumeric, tinyBigNumeric},
		{"2", minBigNumeric, maxBigNumeric, "-" + tinyBigNumeric},
		{"3", tinyBigNumeric},
	}
	rows := read("SELECT id, n, a, s.b FROM t ORDER BY id")
	for _, row := range want {
		var got []bigquery.Value
		if err := rows.Next(&got); err != nil {
			t.Fatalf("Next: %v", err)
		}
		if got[0] != mustInt(t, row[0]) || got[1].(*big.Rat).Cmp(rat(t, row[1])) != 0 {
			t.Errorf("got row %v, want %v", got, row)
		}
		if len(row) > 2 {
			array := got[2].([]bigquery.Value)
			if array[0].(*big.Rat).Cmp(rat(t, row[2])) != 0 || got[3].(*big.Rat).Cmp(rat(t, row[3])) != 0 {
				t.Errorf("got row %v, want %v", got, row)
			}
		}
	}
	if err := rows.Next(new([]bigquery.Value)); err != iterator.Done {
		t.Errorf("got another row (%v), want 3", err)
	}

	rows = read("SELECT id FROM t WHERE n = @n", bigquery.QueryParameter{Name: "n",
		Value: &bigquery.QueryParameterValue{
			Type:  bigquery.StandardSQLDataType{TypeKind: "BIGNUMERIC"},
			Value: minBigNumeric,
		}})
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil || row[0] != int64(2) {
		t.Errorf("parameter: got %v (%v), want id 2", row, err)
	}
}

func mustInt(t *testing.T, s string) int64 {
	t.Helper()
	r := rat(t, s)
	if !r.IsInt() {
		t.Fatalf("not an integer: %s", s)
	}
	return r.Num().Int64()
}

// Array functions keep the BIGNUMERIC schema and values exposed by the Go client.
func TestBigNumericArrayFunctions(t *testing.T) {
	t.Parallel()
	rows, err := newClient(t).Query("SELECT ARRAY_LENGTH(a) AS n, ARRAY_REVERSE(a) AS r, " +
		"ARRAY_CONCAT(a, a) AS c FROM (SELECT [BIGNUMERIC '" + maxBigNumeric +
		"', BIGNUMERIC '" + minBigNumeric + "', BIGNUMERIC '" + tinyBigNumeric + "'] AS a)").Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if len(rows.Schema) != 3 || rows.Schema[0].Type != bigquery.IntegerFieldType || rows.Schema[0].Repeated {
		t.Fatalf("unexpected schema: %v", rows.Schema)
	}
	if row[0] != int64(3) {
		t.Errorf("ARRAY_LENGTH: got %v, want 3", row[0])
	}
	for column, want := range [][]string{
		{tinyBigNumeric, minBigNumeric, maxBigNumeric},
		{maxBigNumeric, minBigNumeric, tinyBigNumeric, maxBigNumeric, minBigNumeric, tinyBigNumeric},
	} {
		field := rows.Schema[column+1]
		if field.Type != bigquery.BigNumericFieldType || !field.Repeated {
			t.Fatalf("%s: got schema %v, want repeated BIGNUMERIC", field.Name, field)
		}
		got := row[column+1].([]bigquery.Value)
		if len(got) != len(want) {
			t.Fatalf("%s: got %d elements, want %d", field.Name, len(got), len(want))
		}
		for i, value := range want {
			if got[i].(*big.Rat).Cmp(rat(t, value)) != 0 {
				t.Errorf("%s[%d]: got %v, want %s", field.Name, i, got[i], value)
			}
		}
	}
	if err := rows.Next(new([]bigquery.Value)); err != iterator.Done {
		t.Errorf("got another row (%v), want one", err)
	}
}

// AVG of BIGNUMERIC is BIGNUMERIC, and the statistical aggregates are FLOAT64, as in BigQuery.
func TestBigNumericAggregates(t *testing.T) {
	t.Parallel()
	rows, err := newClient(t).Query("SELECT AVG(x) AS a, STDDEV_POP(x) AS s, CORR(x, x) AS c " +
		"FROM UNNEST([BIGNUMERIC '" + tinyBigNumeric + "', BIGNUMERIC '" + maxBigNumeric + "']) x").Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if len(rows.Schema) != 3 || rows.Schema[0].Type != bigquery.BigNumericFieldType ||
		rows.Schema[1].Type != bigquery.FloatFieldType || rows.Schema[2].Type != bigquery.FloatFieldType {
		t.Fatalf("unexpected schema: %v", rows.Schema)
	}
	// The sum overflows BIGNUMERIC, but the average fits.
	if want := "289480223093290488558927462521719769633.17496166410141009864396001978282409984"; row[0].(*big.Rat).Cmp(rat(t, want)) != 0 {
		t.Errorf("AVG: got %v, want %s", row[0], want)
	}
	if _, ok := row[1].(float64); !ok {
		t.Errorf("STDDEV_POP: got %T, want float64", row[1])
	}
	if _, ok := row[2].(float64); !ok {
		t.Errorf("CORR: got %T, want float64", row[2])
	}
}

// A parsed value reaches the client as BIGNUMERIC, with its full precision.
func TestParseBigNumeric(t *testing.T) {
	t.Parallel()
	query := newClient(t).Query("SELECT PARSE_BIGNUMERIC(@s) AS n")
	query.Parameters = []bigquery.QueryParameter{{Name: "s", Value: maxBigNumeric}}
	rows, err := query.Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if len(rows.Schema) != 1 || rows.Schema[0].Type != bigquery.BigNumericFieldType || rows.Schema[0].Repeated {
		t.Fatalf("unexpected schema: %v", rows.Schema)
	}
	if got := row[0].(*big.Rat); got.Cmp(rat(t, maxBigNumeric)) != 0 {
		t.Errorf("got %v, want %s", got, maxBigNumeric)
	}
	if err := rows.Next(new([]bigquery.Value)); err != iterator.Done {
		t.Errorf("got another row (%v), want one", err)
	}
}
