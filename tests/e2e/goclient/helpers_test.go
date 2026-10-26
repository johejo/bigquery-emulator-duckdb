package goclient

import (
	"context"
	"os"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
	"google.golang.org/api/option"
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
	status, err := waitJob(ctx, job)
	return job, status, err
}

// waitJob preserves the status for tests that inspect job statistics.
func waitJob(ctx context.Context, job *bigquery.Job) (*bigquery.JobStatus, error) {
	status, err := job.Wait(ctx)
	if err != nil {
		return nil, err
	}
	return status, status.Err()
}

// wait runs a query, load, copy or extract job and returns its error.
func wait(ctx context.Context, runner interface {
	Run(context.Context) (*bigquery.Job, error)
}) error {
	job, err := runner.Run(ctx)
	if err != nil {
		return err
	}
	return runJob(ctx, job, nil)
}

// runJob checks a Run result and waits for completion. Tests can keep the job
// alongside the returned error when they need to inspect its output.
func runJob(ctx context.Context, job *bigquery.Job, err error) error {
	if err != nil {
		return err
	}
	_, err = waitJob(ctx, job)
	return err
}

// collectRows distinguishes the end of a result set from a failed read.
func collectRows(rows *bigquery.RowIterator) ([][]bigquery.Value, error) {
	var result [][]bigquery.Value
	for {
		var row []bigquery.Value
		if err := rows.Next(&row); err == iterator.Done {
			return result, nil
		} else if err != nil {
			return nil, err
		}
		result = append(result, row)
	}
}

func readRows(t *testing.T, rows *bigquery.RowIterator) [][]bigquery.Value {
	t.Helper()
	result, err := collectRows(rows)
	if err != nil {
		t.Fatalf("read rows: %v", err)
	}
	return result
}

func newClient(t *testing.T) *bigquery.Client {
	t.Helper()
	project := os.Getenv("BQ_EMULATOR_PROJECT")
	if project == "" {
		project = "test"
	}
	return newClientForProject(t, project)
}

func newClientForProject(t *testing.T, project string) *bigquery.Client {
	t.Helper()
	endpoint := os.Getenv("BQ_EMULATOR_API")
	if endpoint == "" {
		t.Skip("BQ_EMULATOR_API is not set; run just e2e")
	}
	client, err := bigquery.NewClient(context.Background(), project,
		option.WithEndpoint(endpoint), option.WithoutAuthentication())
	if err != nil {
		t.Fatalf("bigquery.NewClient: %v", err)
	}
	t.Cleanup(func() { client.Close() })
	return client
}
