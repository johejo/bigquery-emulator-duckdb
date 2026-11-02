package goclient

import (
	"testing"

	"cloud.google.com/go/bigquery"
)

func TestJobStatisticsDescribeTheStatement(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	dataset := newDataset(t, client, "go_statement_type")

	for _, tc := range []struct {
		sql           string
		statementType string
		ddlTarget     string
	}{
		{"CREATE TABLE t AS SELECT 1 AS id", "CREATE_TABLE_AS_SELECT", "t"},
		{"CREATE TABLE u (id INT64)", "CREATE_TABLE", "u"},
		{"CREATE TABLE w LIKE u AS SELECT id FROM t", "CREATE_TABLE_AS_SELECT", "w"},
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
		_, status, err := runScript(t, client, dataset.DatasetID, tc.sql)
		if err != nil {
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
