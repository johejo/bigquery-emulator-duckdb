package goclient

import (
	"math"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

func TestWindowRangeSpecialParameters(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	// Exact diagnostics recorded from BigQuery. Go's NaN and infinity values must reach
	// window boundary validation as FLOAT64 query parameters, not as script variables.
	for _, c := range []struct {
		name, sql, want string
		value           float64
	}{
		{
			"nan",
			"SELECT COUNT(*) OVER (ORDER BY x RANGE BETWEEN CURRENT ROW AND @offset_value FOLLOWING) FROM UNNEST([1.0]) AS x",
			"Window frame offset for PRECEDING or FOLLOWING must be non-negative",
			math.NaN(),
		},
		{
			"infinity",
			"SELECT COUNT(*) OVER (ORDER BY x RANGE BETWEEN @offset_value PRECEDING AND CURRENT ROW) FROM UNNEST([1.0, CAST('inf' AS FLOAT64)]) AS x",
			"Offset value in OVER(RANGE ... <offset> PRECEDING|FOLLOWING) cannot be infinity when there exists an infinity order key making the window boundary value NaN.",
			math.Inf(1),
		},
	} {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			query := client.Query(c.sql)
			query.Parameters = []bigquery.QueryParameter{{Name: "offset_value", Value: c.value}}
			rows, err := query.Read(t.Context())
			if err == nil {
				var row []bigquery.Value
				err = rows.Next(&row)
			}
			if err == nil || !strings.Contains(err.Error(), c.want) {
				t.Fatalf("error = %v, want one containing %q", err, c.want)
			}
		})
	}
}

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
