package goclient

import (
	"context"
	"math/big"
	"strings"
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
		"', NULL], STRUCT(BIGNUMERIC '" + tinyBigNumeric + "'))")
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

// Out of range results fail with GoogleSQL's messages, as in BigQuery.
func TestBigNumericErrors(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	for sql, want := range map[string]string{
		"SELECT x + 1 FROM UNNEST([BIGNUMERIC '" + maxBigNumeric + "']) x": "BIGNUMERIC overflow: " +
			maxBigNumeric + " + 1",
		"SELECT SUM(x) FROM UNNEST([BIGNUMERIC '" + maxBigNumeric + "', 1]) x": "BIGNUMERIC overflow: SUM",
		"SELECT CAST(x AS BIGNUMERIC) FROM UNNEST(['x']) x":                    "Invalid BIGNUMERIC value: x",
		"SELECT x * 2 FROM UNNEST([BIGNUMERIC '" + maxBigNumeric + "']) x": "BIGNUMERIC overflow: " +
			maxBigNumeric + " * 2",
		"SELECT x / 0 FROM UNNEST([BIGNUMERIC '1']) x":     "division by zero: 1 / 0",
		"SELECT MOD(x, 0) FROM UNNEST([BIGNUMERIC '1']) x": "division by zero: MOD(1, 0)",
		"SELECT CAST(x AS INT64) FROM UNNEST([BIGNUMERIC '1e20']) x":           "int64 out of range: 100000000000000000000",
	} {
		job, err := client.Query(sql).Run(ctx)
		if err == nil {
			var status *bigquery.JobStatus
			if status, err = job.Wait(ctx); err == nil {
				err = status.Err()
			}
		}
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: got error %v, want %q", sql, err, want)
		}
	}
}
