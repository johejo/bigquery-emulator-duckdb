package goclient

import (
	"context"
	"fmt"
	"reflect"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// Expected errors, row counts and final rows were recorded on BigQuery with bqanswers.
func TestRequirePartitionFilterDML(t *testing.T) {
	t.Parallel()
	for _, c := range []struct {
		name           string
		sql            string
		sourceRequired bool
		errorTable     string
		affected       int64
		rows           []string
	}{
		{name: "update_nonpartition_filter", sql: "UPDATE target SET v = v + 10 WHERE id = 1", sourceRequired: false, errorTable: "target", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "update_partition_filter", sql: "UPDATE target SET v = v + 10 WHERE day = DATE '2025-01-01'", sourceRequired: false, errorTable: "", affected: 1, rows: []string{"2025-01-01,2025-01-02,1,20", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "update_false", sql: "UPDATE target SET v = v + 10 WHERE FALSE", sourceRequired: false, errorTable: "", affected: 0, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "update_function_filter", sql: "UPDATE target SET v = v + 10 WHERE DATE_ADD(day, INTERVAL 1 DAY) = DATE '2025-01-02'", sourceRequired: false, errorTable: "", affected: 1, rows: []string{"2025-01-01,2025-01-02,1,20", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "update_mixed_or", sql: "UPDATE target SET v = v + 10 WHERE day = DATE '2025-01-01' OR id = 2", sourceRequired: false, errorTable: "target", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "update_partition_key", sql: "UPDATE target SET day = DATE '2025-01-03' WHERE day = DATE '2025-01-01'", sourceRequired: false, errorTable: "", affected: 1, rows: []string{"2025-01-03,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "update_from_source", sql: "UPDATE target AS t SET v = s.v FROM source AS s WHERE t.id = s.id AND t.day = DATE '2025-01-01'", sourceRequired: false, errorTable: "", affected: 1, rows: []string{"2025-01-01,2025-01-02,1,100", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "update_required_source_unfiltered", sql: "UPDATE target AS t SET v = s.v FROM source AS s WHERE t.id = s.id AND t.day = DATE '2025-01-01'", sourceRequired: true, errorTable: "source", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "update_both_filtered", sql: "UPDATE target AS t SET v = s.v FROM source AS s WHERE t.id = s.id AND t.day = DATE '2025-01-01' AND s.day = DATE '2025-01-01'", sourceRequired: true, errorTable: "", affected: 1, rows: []string{"2025-01-01,2025-01-02,1,100", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "update_partition_equality", sql: "UPDATE target AS t SET v = s.v FROM source AS s WHERE t.id = s.id AND t.day = s.day AND s.day = DATE '2025-01-01'", sourceRequired: true, errorTable: "", affected: 1, rows: []string{"2025-01-01,2025-01-02,1,100", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "delete_nonpartition_filter", sql: "DELETE FROM target WHERE id = 1", sourceRequired: false, errorTable: "target", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "delete_partition_filter", sql: "DELETE FROM target WHERE day = DATE '2025-01-01'", sourceRequired: false, errorTable: "", affected: 1, rows: []string{"2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "delete_true", sql: "DELETE FROM target WHERE TRUE", sourceRequired: false, errorTable: "target", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "delete_false", sql: "DELETE FROM target WHERE FALSE", sourceRequired: false, errorTable: "", affected: 0, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "delete_null_partition", sql: "DELETE FROM target WHERE day IS NULL", sourceRequired: false, errorTable: "", affected: 1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20"}},
		{name: "delete_all_partitions", sql: "DELETE FROM target WHERE day IS NULL OR day >= DATE '0001-01-01'", sourceRequired: false, errorTable: "", affected: 3, rows: []string{}},
		{name: "delete_mixed_or", sql: "DELETE FROM target WHERE day = DATE '2025-01-01' OR id = 2", sourceRequired: false, errorTable: "target", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "delete_scalar_literal", sql: "DELETE FROM target WHERE day = (SELECT DATE '2025-01-01')", sourceRequired: false, errorTable: "", affected: 1, rows: []string{"2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "delete_source_bound", sql: "DELETE FROM target WHERE day = (SELECT MIN(day) FROM source)", sourceRequired: false, errorTable: "target", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_no_partition_filter", sql: "MERGE target AS t USING source AS s ON t.id = s.id WHEN MATCHED THEN UPDATE SET v = s.v", sourceRequired: false, errorTable: "target", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_on_target_filter", sql: "MERGE target AS t USING source AS s ON t.id = s.id AND t.day = DATE '2025-01-01' WHEN MATCHED THEN UPDATE SET v = s.v", sourceRequired: false, errorTable: "", affected: 1, rows: []string{"2025-01-01,2025-01-02,1,100", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_when_target_filter", sql: "MERGE target AS t USING source AS s ON t.id = s.id WHEN MATCHED AND t.day = DATE '2025-01-01' THEN UPDATE SET v = s.v", sourceRequired: false, errorTable: "", affected: 1, rows: []string{"2025-01-01,2025-01-02,1,100", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_unfiltered_when_branch", sql: "MERGE target AS t USING source AS s ON t.id = s.id WHEN MATCHED AND t.day = DATE '2025-01-01' THEN UPDATE SET v = s.v WHEN MATCHED AND t.id = 2 THEN DELETE", sourceRequired: false, errorTable: "target", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_partition_equality", sql: "MERGE target AS t USING (SELECT * FROM source WHERE day = DATE '2025-01-01') AS s ON t.id = s.id AND t.day = s.day WHEN MATCHED THEN UPDATE SET v = s.v", sourceRequired: true, errorTable: "", affected: 1, rows: []string{"2025-01-01,2025-01-02,1,100", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_required_source_unfiltered", sql: "MERGE target AS t USING source AS s ON t.id = s.id AND t.day = DATE '2025-01-01' WHEN MATCHED THEN UPDATE SET v = s.v", sourceRequired: true, errorTable: "source", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_both_filtered", sql: "MERGE target AS t USING (SELECT * FROM source WHERE day = DATE '2025-01-01') AS s ON t.id = s.id AND t.day = DATE '2025-01-01' WHEN MATCHED THEN UPDATE SET v = s.v", sourceRequired: true, errorTable: "", affected: 1, rows: []string{"2025-01-01,2025-01-02,1,100", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_insert_only", sql: "MERGE target AS t USING source AS s ON t.id = s.id WHEN NOT MATCHED THEN INSERT (day, other_day, id, v) VALUES (s.day, s.day, s.id, s.v)", sourceRequired: false, errorTable: "target", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_false_insert_only", sql: "MERGE target AS t USING source AS s ON FALSE WHEN NOT MATCHED THEN INSERT (day, other_day, id, v) VALUES (s.day, s.day, s.id, s.v)", sourceRequired: false, errorTable: "", affected: 2, rows: []string{"2025-01-01,2025-01-01,1,100", "2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30", "2025-01-03,2025-01-03,4,400"}},
		{name: "merge_false_replace_partition", sql: "MERGE target AS t USING source AS s ON FALSE WHEN NOT MATCHED BY SOURCE AND t.day = DATE '2025-01-01' THEN DELETE WHEN NOT MATCHED AND s.day = DATE '2025-01-01' THEN INSERT (day, other_day, id, v) VALUES (s.day, s.day, s.id, s.v)", sourceRequired: true, errorTable: "", affected: 2, rows: []string{"2025-01-01,2025-01-01,1,100", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_false_unfiltered_delete", sql: "MERGE target AS t USING source AS s ON FALSE WHEN NOT MATCHED BY SOURCE THEN DELETE WHEN NOT MATCHED AND s.day = DATE '2025-01-01' THEN INSERT (day, other_day, id, v) VALUES (s.day, s.day, s.id, s.v)", sourceRequired: true, errorTable: "target", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_on_filter_unmatched_delete", sql: "MERGE target AS t USING source AS s ON t.id = s.id AND t.day = DATE '2025-01-01' WHEN MATCHED THEN UPDATE SET v = s.v WHEN NOT MATCHED BY SOURCE THEN DELETE", sourceRequired: false, errorTable: "target", affected: -1, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "merge_when_unmatched_delete", sql: "MERGE target AS t USING source AS s ON t.id = s.id WHEN NOT MATCHED BY SOURCE AND t.day = DATE '2025-01-01' THEN DELETE", sourceRequired: false, errorTable: "", affected: 0, rows: []string{"2025-01-01,2025-01-02,1,10", "2025-01-02,2025-01-01,2,20", "NULL,NULL,3,30"}},
		{name: "truncate", sql: "TRUNCATE TABLE target", sourceRequired: false, errorTable: "", affected: 3, rows: []string{}},
		{name: "truncate_transaction", sql: "BEGIN TRANSACTION; TRUNCATE TABLE target; COMMIT TRANSACTION", sourceRequired: false, errorTable: "", affected: -1, rows: []string{}},
	} {
		t.Run(c.name, func(t *testing.T) {
			t.Parallel()
			ctx := context.Background()
			client := newClient(t)
			dataset := client.Dataset("go_rpf_dml_" + c.name)
			if err := dataset.Create(ctx, nil); err != nil {
				t.Fatal(err)
			}
			t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
			p := creationPaths{t: t, client: client, dataset: dataset}
			for _, table := range []string{"target", "source"} {
				required := table == "target" || c.sourceRequired
				p.mustRun(fmt.Sprintf("CREATE TABLE %s (day DATE, other_day DATE, id INT64, v INT64) PARTITION BY day OPTIONS (require_partition_filter = %t)", table, required))
			}
			p.mustRun("INSERT INTO target VALUES (DATE '2025-01-01', DATE '2025-01-02', 1, 10), (DATE '2025-01-02', DATE '2025-01-01', 2, 20), (NULL, NULL, 3, 30)")
			p.mustRun("INSERT INTO source VALUES (DATE '2025-01-01', DATE '2025-01-01', 1, 100), (DATE '2025-01-03', DATE '2025-01-03', 4, 400)")
			_, status, err := runScript(t, client, dataset.DatasetID, c.sql)
			if c.errorTable != "" {
				want := fmt.Sprintf("Cannot query over table '%s.%s.%s' without a filter over column(s) 'day' that can be used for partition elimination", client.Project(), dataset.DatasetID, c.errorTable)
				if err == nil || !strings.Contains(err.Error(), want) {
					t.Fatalf("error = %v, want %s", err, want)
				}
			} else {
				if err != nil {
					t.Fatal(err)
				}
				if c.affected >= 0 {
					stats, ok := status.Statistics.Details.(*bigquery.QueryStatistics)
					if !ok || stats.NumDMLAffectedRows != c.affected {
						t.Errorf("statistics = %+v, want %d affected rows", status.Statistics.Details, c.affected)
					}
				}
			}
			got := []string{}
			for _, row := range p.read("SELECT day, other_day, id, v FROM target WHERE day IS NULL OR day >= DATE '0001-01-01' ORDER BY id, day, other_day, v") {
				cells := make([]string, len(row))
				for i, value := range row {
					cells[i] = "NULL"
					if value != nil {
						cells[i] = fmt.Sprint(value)
					}
				}
				got = append(got, strings.Join(cells, ","))
			}
			if !reflect.DeepEqual(got, c.rows) {
				t.Errorf("rows = %v, want %v", got, c.rows)
			}
			metadata, err := dataset.Table("target").Metadata(ctx)
			if err != nil {
				t.Fatal(err)
			}
			if !metadata.RequirePartitionFilter || metadata.TimePartitioning == nil || !metadata.TimePartitioning.RequirePartitionFilter {
				t.Errorf("partition metadata = %+v", metadata)
			}
		})
	}
}
