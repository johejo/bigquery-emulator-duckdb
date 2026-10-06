package goclient

import (
	"context"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// runScript runs sql with dataset as its default dataset and waits for the job to finish.
func runScript(t *testing.T, client *bigquery.Client, dataset, sql string) (*bigquery.Job,
	*bigquery.JobStatus, error) {
	t.Helper()
	ctx := context.Background()
	query := client.Query(sql)
	query.DefaultDatasetID = dataset
	job, err := query.Run(ctx)
	if err != nil {
		return nil, nil, err
	}
	status, err := job.Wait(ctx)
	if err != nil {
		return job, nil, err
	}
	return job, status, status.Err()
}

func TestMultiStatementQueryReturnsItsLastResult(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_scripts")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })

	job, status, err := runScript(t, client, dataset.DatasetID, `
DECLARE n INT64 DEFAULT 3;
CREATE TABLE t (id INT64, tags ARRAY<STRING>);
WHILE n > 0 DO
  INSERT t (id, tags) VALUES (n, ['a']);
  SET n = n - 1;
END WHILE;
SELECT COUNT(*) AS c, SUM(id) AS s FROM t;`)
	if err != nil {
		t.Fatal(err)
	}
	statistics := status.Statistics.Details.(*bigquery.QueryStatistics)
	if statistics.StatementType != "SCRIPT" {
		t.Errorf("got statement type %q, want SCRIPT", statistics.StatementType)
	}
	rows, err := job.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row struct{ C, S int64 }
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if row.C != 3 || row.S != 6 {
		t.Errorf("got count %d and sum %d, want 3 and 6", row.C, row.S)
	}
	if err := rows.Next(&row); err != iterator.Done {
		t.Errorf("got another row or an error: %v", err)
	}
}

func TestMultiStatementQueryFailsWithTheFailedStatement(t *testing.T) {
	client := newClient(t)
	_, _, err := runScript(t, client, "", "SELECT 1;\nSELECT 1/0;")
	if err == nil || !strings.Contains(err.Error(), "division by zero") {
		t.Errorf("got %v, want a division by zero", err)
	}
}

func TestMultiStatementQueryFailsWhenAVariableBreaksItsTypeParameters(t *testing.T) {
	client := newClient(t)
	_, _, err := runScript(t, client, "", "DECLARE x STRING(3) DEFAULT 'abcd'; SELECT x")
	if err == nil {
		t.Error("got no error, want STRING(3) to fail to take 'abcd'")
	}
}

// The emulator rejects what it does not run, even inside an exception handler, which would
// otherwise handle the rejection as an error of the script.
func TestMultiStatementQueryRejectsUnsupportedForms(t *testing.T) {
	client := newClient(t)
	for _, sql := range []string{
		"EXECUTE IMMEDIATE 'SELECT 1'; SELECT 2",
		"CALL d.p(); SELECT 1",
		"SET @@time_zone = 'Asia/Tokyo'; SELECT CURRENT_DATE()",
		"BEGIN BEGIN TRANSACTION; COMMIT TRANSACTION; EXCEPTION WHEN ERROR THEN SELECT 1; END",
	} {
		_, _, err := runScript(t, client, "", sql)
		if err == nil || !strings.Contains(err.Error(), "The emulator does not support") {
			t.Errorf("%s: got %v, want an unsupported error", sql, err)
		}
	}
}

func TestMultiStatementQueryRejectsJobOptionsOfASingleStatement(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)

	dryRun := client.Query("SELECT 1; SELECT 2")
	dryRun.DryRun = true
	job, err := dryRun.Run(ctx)
	if err == nil {
		err = job.LastStatus().Err()
	}
	if err == nil || !strings.Contains(err.Error(), "The emulator does not support dry runs") {
		t.Errorf("got %v, want dry runs of a multi-statement query to be unsupported", err)
	}

	destination := client.Query("SELECT 1; SELECT 2")
	destination.Dst = client.Dataset("go_scripts_destination").Table("t")
	job, err = destination.Run(ctx)
	if err == nil {
		var status *bigquery.JobStatus
		if status, err = job.Wait(ctx); err == nil {
			err = status.Err()
		}
	}
	if err == nil || !strings.Contains(err.Error(), "multi-statement") {
		t.Errorf("got %v, want a multi-statement query to have no destination table", err)
	}
}

func TestChildJobsOfAMultiStatementQueryAreUnsupported(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	job, _, err := runScript(t, client, "", "SELECT 1; SELECT 2")
	if err != nil {
		t.Fatal(err)
	}
	_, err = job.Children(ctx).Next()
	if err == nil || !strings.Contains(err.Error(), "The emulator does not support child jobs") {
		t.Errorf("got %v, want child jobs to be unsupported", err)
	}
}

// Temporary tables follow
// https://cloud.google.com/bigquery/docs/multi-statement-queries#temporary_tables.
func TestTemporaryTablesLastForTheirMultiStatementQuery(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_temporary_tables")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"CREATE TABLE t AS SELECT 1 AS a"); err != nil {
		t.Fatal(err)
	}

	// Once the temporary table is dropped, its name refers to the default dataset's table again.
	job, _, err := runScript(t, client, dataset.DatasetID, `
CREATE TEMP TABLE t AS SELECT 2 AS a;
DROP TABLE t;
SELECT a FROM t;`)
	if err != nil {
		t.Fatal(err)
	}
	rows, err := job.Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row struct{ A int64 }
	if err := rows.Next(&row); err != nil || row.A != 1 {
		t.Errorf("got %d and %v, want the default dataset's row 1", row.A, err)
	}

	// _SESSION names only temporary tables.
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"SELECT 1; SELECT a FROM _SESSION.t"); err == nil {
		t.Error("_SESSION.t read a table that is not temporary")
	}

	// A temporary table is gone once the multi-statement query that created it ends, and the
	// default dataset never lists it.
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"CREATE TEMP TABLE n AS SELECT 1 AS x; SELECT x FROM n"); err != nil {
		t.Fatal(err)
	}
	if _, _, err := runScript(t, client, dataset.DatasetID, "SELECT 1; SELECT x FROM n"); err == nil {
		t.Error("a temporary table outlived its multi-statement query")
	}
	tables := dataset.Tables(ctx)
	var names []string
	for {
		table, err := tables.Next()
		if err == iterator.Done {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		names = append(names, table.TableID)
	}
	if len(names) != 1 || names[0] != "t" {
		t.Errorf("got tables %v, want only t", names)
	}
}

func TestTemporaryTablesRejectedForms(t *testing.T) {
	client := newClient(t)
	for _, sql := range []string{
		// Only a multi-statement query creates temporary tables.
		"CREATE TEMP TABLE n (x INT64)",
		// A temporary table takes no project or dataset qualifier.
		"CREATE TEMP TABLE d.n (x INT64); SELECT 1",
	} {
		if _, _, err := runScript(t, client, "", sql); err == nil {
			t.Errorf("%s: succeeded, want an error", sql)
		}
	}
	// A view would outlive the temporary table it reads.
	sql := "CREATE TEMP TABLE n AS SELECT 1 AS x; CREATE VIEW go_scripts_temporary_view.v AS SELECT x FROM n"
	_, _, err := runScript(t, client, "", sql)
	if err == nil || !strings.Contains(err.Error(), "The emulator does not support") {
		t.Errorf("%s: got %v, want an unsupported error", sql, err)
	}
}
