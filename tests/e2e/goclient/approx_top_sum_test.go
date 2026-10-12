package goclient

import (
	"context"
	"testing"

	"cloud.google.com/go/bigquery"
)

func TestApproxTopSumParametersAndSchema(t *testing.T) {
	query := newClient(t).Query(`SELECT APPROX_TOP_SUM(x, w, @n) AS top
		FROM UNNEST([STRUCT('x' AS x, 5 AS w), ('x', 1), ('y', 2)])`)
	query.Parameters = []bigquery.QueryParameter{{Name: "n", Value: int64(2)}}
	rows, err := query.Read(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	var row struct {
		Top []struct {
			Value string
			Sum   int64
		}
	}
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	// The sums and field types follow APPROX_TOP_SUM's documented definition.
	if len(row.Top) != 2 || row.Top[0].Value != "x" || row.Top[0].Sum != 6 ||
		row.Top[1].Value != "y" || row.Top[1].Sum != 2 {
		t.Fatalf("got %+v, want [{x 6} {y 2}]", row.Top)
	}
	if len(rows.Schema) != 1 {
		t.Fatalf("got schema %v, want one repeated record", rows.Schema)
	}
	top := rows.Schema[0]
	if !top.Repeated || top.Type != bigquery.RecordFieldType || len(top.Schema) != 2 ||
		top.Schema[0].Name != "value" || top.Schema[0].Type != bigquery.StringFieldType ||
		top.Schema[1].Name != "sum" || top.Schema[1].Type != bigquery.IntegerFieldType {
		t.Fatalf("unexpected top schema: %+v", top)
	}
}

func TestApproxTopSumInvalidCountParameter(t *testing.T) {
	client := newClient(t)
	for _, count := range []bigquery.NullInt64{
		{Int64: 0, Valid: true},
		{Int64: -1, Valid: true},
		{Valid: false},
	} {
		query := client.Query("SELECT APPROX_TOP_SUM(x, 1, @n) FROM UNNEST(@input) x")
		query.Parameters = []bigquery.QueryParameter{
			{Name: "n", Value: count},
			{Name: "input", Value: []int64{1, 2}},
		}
		if _, err := query.Read(context.Background()); err == nil {
			t.Errorf("count %+v: want an error for a NULL or nonpositive count", count)
		}
	}
}
