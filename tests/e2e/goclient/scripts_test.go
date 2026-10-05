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

// The emulator rejects what it does not run, even inside an exception handler, which would
// otherwise handle the rejection as an error of the script.
func TestMultiStatementQueryRejectsUnsupportedForms(t *testing.T) {
	client := newClient(t)
	for _, sql := range []string{
		"EXECUTE IMMEDIATE 'SELECT 1'; SELECT 2",
		"CALL d.p(); SELECT 1",
		"SET @@time_zone = 'Asia/Tokyo'; SELECT CURRENT_DATE()",
		"DECLARE s STRING(3); SELECT s",
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
