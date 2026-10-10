package goclient

import (
	"context"
	"os"
	"testing"

	"cloud.google.com/go/bigquery"
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
	status, err := job.Wait(ctx)
	if err != nil {
		return job, nil, err
	}
	return job, status, status.Err()
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
