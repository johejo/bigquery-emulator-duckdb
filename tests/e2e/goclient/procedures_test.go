package goclient

import (
	"context"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// Metadata follows CREATE PROCEDURE and the REST Routine resource documentation.
// Scalar results are covered in testdata/scalars/procedures.txt.
func TestSqlProcedureMetadata(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_procedures")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) *bigquery.JobStatus {
		t.Helper()
		_, status, err := runScript(t, client, "", sql)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		return status
	}
	status := run(`CREATE PROCEDURE go_procedures.p(IN x INT64)
        OPTIONS(strict_mode=FALSE, description='procedure description') BEGIN SELECT x; END`)
	stats := status.Statistics.Details.(*bigquery.QueryStatistics)
	if stats.StatementType != "CREATE_PROCEDURE" || stats.DDLTargetRoutine == nil ||
		stats.DDLTargetRoutine.DatasetID != dataset.DatasetID || stats.DDLTargetRoutine.RoutineID != "p" {
		t.Errorf("unexpected create statistics: %+v", stats)
	}
	m, err := dataset.Routine("p").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if m.Type != "PROCEDURE" || m.Language != "SQL" || m.Description != "procedure description" ||
		!strings.Contains(m.Body, "SELECT x") || m.CreationTime.IsZero() || m.LastModifiedTime.IsZero() ||
		len(m.Arguments) != 1 || m.Arguments[0].Name != "x" || m.Arguments[0].Mode != "IN" ||
		m.Arguments[0].DataType.TypeKind != "INT64" || m.ReturnType != nil {
		t.Errorf("unexpected procedure metadata: %+v", m)
	}
	status = run("CALL go_procedures.p(3)")
	if got := status.Statistics.Details.(*bigquery.QueryStatistics).StatementType; got != "CALL" {
		t.Errorf("got statement type %q, want CALL", got)
	}
	run("CREATE PROCEDURE IF NOT EXISTS go_procedures.p() OPTIONS(strict_mode=FALSE) BEGIN SELECT 1; END")
	m, err = dataset.Routine("p").Metadata(ctx)
	if err != nil || len(m.Arguments) != 1 {
		t.Fatalf("IF NOT EXISTS replaced the procedure: %v, %v", m, err)
	}
	run("CREATE OR REPLACE PROCEDURE go_procedures.p() OPTIONS(strict_mode=FALSE) BEGIN SELECT 1; END")
	for _, sql := range []string{
		"SELECT go_procedures.p()",
		"DROP FUNCTION go_procedures.p",
		"CREATE OR REPLACE FUNCTION go_procedures.p() AS (1)",
	} {
		if _, _, err := runScript(t, client, "", sql); err == nil {
			t.Errorf("%s: got no error", sql)
		}
	}
	status = run("DROP PROCEDURE go_procedures.p")
	if got := status.Statistics.Details.(*bigquery.QueryStatistics).StatementType; got != "DROP_PROCEDURE" {
		t.Errorf("got statement type %q, want DROP_PROCEDURE", got)
	}
	run("DROP PROCEDURE IF EXISTS go_procedures.p")
	if _, err := dataset.Routine("p").Metadata(ctx); err == nil {
		t.Error("dropped procedure is still visible")
	}
}
