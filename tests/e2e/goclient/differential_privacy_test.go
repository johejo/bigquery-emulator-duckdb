package goclient

import (
	"reflect"
	"testing"
)

// The documentation's "Remove noise" example, ordered for comparison: an epsilon of 1e20 removes
// the noise. Each professor contributes to at most two groups, so max_groups_contributed=2 keeps
// every row, and AVG averages each professor's average.
// third_party/bigquery_docs/reference/standard-sql/query-syntax.md
func TestDifferentialPrivacyWithoutNoise(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	p := datasetQueries{t: t, client: client, dataset: newDataset(t, client, "go_differential_privacy")}
	p.mustRun(`CREATE TABLE professors AS (
  SELECT 101 AS id, "pencil" AS item, 24 AS quantity UNION ALL
  SELECT 123, "pen", 16 UNION ALL
  SELECT 123, "pencil", 10 UNION ALL
  SELECT 123, "pencil", 38 UNION ALL
  SELECT 101, "pen", 19 UNION ALL
  SELECT 101, "pen", 23 UNION ALL
  SELECT 130, "scissors", 8 UNION ALL
  SELECT 150, "pencil", 72)`)
	got := p.read(`SELECT
  WITH DIFFERENTIAL_PRIVACY
    OPTIONS(epsilon=1e20, delta=.01, max_groups_contributed=2, privacy_unit_column=id)
    item,
    AVG(quantity, contribution_bounds_per_group => (0,100)) AS average_quantity
FROM professors
GROUP BY item
ORDER BY item`)
	want := [][]any{{"pen", 18.5}, {"pencil", 40.0}, {"scissors", 8.0}}
	if len(got) != len(want) {
		t.Fatalf("rows = %v, want %v", got, want)
	}
	for i := range want {
		if !reflect.DeepEqual([]any{got[i][0], got[i][1]}, want[i]) {
			t.Errorf("row %d = %v, want %v", i, got[i], want[i])
		}
	}
}
