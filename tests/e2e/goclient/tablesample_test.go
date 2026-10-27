package goclient

import (
	"context"
	"math"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// TABLESAMPLE accepts a numeric parameter in [0, 100]. The endpoints select no rows or all
// rows; intermediate percentages select a subset whose size is nondeterministic.
// third_party/bigquery_docs/table-sampling.md
// BigQuery's answers establish the NULL and infinity messages. On a 16-partition table with
// 65,536 rows, NaN returned all rows in eight trials while 50 PERCENT returned half each time.
func TestTableSampleQueryParameters(t *testing.T) {
	t.Parallel()
	ctx := t.Context()
	client := newClient(t)
	dataset := client.Dataset("go_tablesample_parameters")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(context.Background()) })
	p := datasetQueries{t: t, client: client, dataset: dataset}
	const rowCount int64 = 65536
	p.mustRun("CREATE TABLE t AS SELECT id FROM UNNEST(GENERATE_ARRAY(1, 65536)) AS id")

	for _, mode := range []string{"named", "positional"} {
		t.Run(mode, func(t *testing.T) {
			for _, c := range []struct {
				name          string
				value         any
				count         int64
				fails         bool
				errorContains string
			}{
				{"zero_integer", int64(0), 0, false, ""},
				{"all_integer", int64(100), rowCount, false, ""},
				{"zero_float", 0.0, 0, false, ""},
				{"all_float", 100.0, rowCount, false, ""},
				{"fractional", 12.5, -1, false, ""},
				{"negative_integer", int64(-1), 0, true, ""},
				{"large_integer", int64(101), 0, true, ""},
				{"negative_float", -0.1, 0, true, ""},
				{"large_float", 100.1, 0, true, ""},
				{"null_integer", bigquery.NullInt64{}, 0, true, "Const must be a DOUBLE-compatible constant."},
				{"null_float", bigquery.NullFloat64{}, 0, true, "Const must be a DOUBLE-compatible constant."},
				{"nan", math.NaN(), rowCount, false, ""},
				{"positive_infinity", math.Inf(1), 0, true, "Sampling rate must be between 0 and 100 percent."},
				{"negative_infinity", math.Inf(-1), 0, true, "Sampling rate must be between 0 and 100 percent."},
				{"string", "100", 0, true, ""},
			} {
				t.Run(c.name, func(t *testing.T) {
					placeholder, name := "@percent", "percent"
					if mode == "positional" {
						placeholder, name = "?", ""
					}
					query := client.Query("SELECT COUNT(*) FROM t TABLESAMPLE SYSTEM (" + placeholder + " PERCENT)")
					query.DefaultDatasetID = dataset.DatasetID
					query.Parameters = []bigquery.QueryParameter{{Name: name, Value: c.value}}
					rows, err := query.Read(ctx)
					var got []bigquery.Value
					if err == nil {
						err = rows.Next(&got)
					}
					if c.fails {
						if err == nil {
							t.Fatal("query unexpectedly succeeded")
						}
						if !strings.Contains(err.Error(), c.errorContains) {
							t.Errorf("error = %v, want one containing %q", err, c.errorContains)
						}
						return
					}
					if err != nil {
						t.Fatal(err)
					}
					count := got[0].(int64)
					if c.count >= 0 && count != c.count {
						t.Errorf("count = %d, want %d", count, c.count)
					}
					if count < 0 || count > rowCount {
						t.Errorf("sample count = %d, want a subset of %d rows", count, rowCount)
					}
				})
			}
		})
	}
}
