package goclient

import (
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// NULL parameter errors come from GoogleSQL's analytic_window_frames and
// analytic_numeric_range_window_frames compliance tests. Script variables are not valid
// window frame offsets in BigQuery; use parameters to exercise NULL boundary validation.
func TestWindowRangeNullParameters(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	for _, c := range []struct {
		name, order, frame string
		value              any
	}{
		{"start_preceding", "x", "@n PRECEDING AND CURRENT ROW", bigquery.NullInt64{}},
		{"end_preceding", "x", "2 PRECEDING AND @n PRECEDING", bigquery.NullInt64{}},
		{"start_following", "x", "@n FOLLOWING AND 2 FOLLOWING", bigquery.NullInt64{}},
		{"end_following", "x", "CURRENT ROW AND @n FOLLOWING", bigquery.NullInt64{}},
		{"numeric", "CAST(x AS NUMERIC)", "@n PRECEDING AND CURRENT ROW", &bigquery.QueryParameterValue{
			Type: bigquery.StandardSQLDataType{TypeKind: "NUMERIC"}, Value: bigquery.NullString{},
		}},
		{"float", "CAST(x AS FLOAT64)", "CURRENT ROW AND @n FOLLOWING", bigquery.NullFloat64{}},
	} {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			query := client.Query("SELECT COUNT(*) OVER (ORDER BY " + c.order + " RANGE BETWEEN " + c.frame + ") FROM UNNEST([1]) AS x")
			query.Parameters = []bigquery.QueryParameter{{Name: "n", Value: c.value}}
			rows, err := query.Read(t.Context())
			if err == nil {
				var row []bigquery.Value
				err = rows.Next(&row)
			}
			const want = "The boundary offset value cannot be null"
			if err == nil || !strings.Contains(err.Error(), want) {
				t.Fatalf("error = %v, want one containing %q", err, want)
			}
		})
	}
}
