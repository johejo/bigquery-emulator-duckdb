package goclient

import (
	"context"
	"testing"

	"cloud.google.com/go/bigquery"
)

func TestJobStatisticsDescribeTheStatement(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_statement_type")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	for _, tc := range []struct {
		sql           string
		statementType string
		ddlTarget     string
	}{
		{"CREATE TABLE t AS SELECT 1 AS id", "CREATE_TABLE_AS_SELECT", "t"},
		{"CREATE TABLE u (id INT64)", "CREATE_TABLE", "u"},
		{"ALTER TABLE u ADD COLUMN name STRING", "ALTER_TABLE", "u"},
		{"ALTER TABLE u SET OPTIONS (description = 'users')", "ALTER_TABLE", "u"},
		{"ALTER SCHEMA go_statement_type SET OPTIONS (description = 'types')", "ALTER_SCHEMA", ""},
		{"CREATE VIEW v AS SELECT id FROM t", "CREATE_VIEW", "v"},
		{"INSERT u (id) VALUES (1)", "INSERT", ""},
		{"TRUNCATE TABLE u", "TRUNCATE_TABLE", ""},
		{"WITH x AS (SELECT 1 AS n) SELECT n FROM x", "SELECT", ""},
		{"DROP VIEW v", "DROP_VIEW", "v"},
		{"DROP TABLE u", "DROP_TABLE", "u"},
	} {
		query := client.Query(tc.sql)
		query.DefaultDatasetID = dataset.DatasetID
		job, err := query.Run(ctx)
		if err != nil {
			t.Fatalf("%s: %v", tc.sql, err)
		}
		status, err := job.Wait(ctx)
		if err != nil {
			t.Fatalf("%s: %v", tc.sql, err)
		}
		if err := status.Err(); err != nil {
			t.Fatalf("%s: %v", tc.sql, err)
		}
		statistics := status.Statistics.Details.(*bigquery.QueryStatistics)
		if statistics.StatementType != tc.statementType {
			t.Errorf("%s: got statement type %q, want %q", tc.sql, statistics.StatementType,
				tc.statementType)
		}
		target := statistics.DDLTargetTable
		switch {
		case tc.ddlTarget == "" && target != nil:
			t.Errorf("%s: got DDL target %v, want none", tc.sql, target)
		case tc.ddlTarget != "" && (target == nil || target.ProjectID != client.Project() ||
			target.DatasetID != dataset.DatasetID || target.TableID != tc.ddlTarget):
			t.Errorf("%s: got DDL target %v, want %s.%s.%s", tc.sql, target, client.Project(),
				dataset.DatasetID, tc.ddlTarget)
		}
	}
}
