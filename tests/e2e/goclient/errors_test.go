package goclient

import (
	"context"
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// Operators and casts fail where BigQuery's do, with its messages.
func TestOperatorAndCastErrors(t *testing.T) {
	ctx := context.Background()
	client := newClient(t)
	for sql, want := range map[string]string{
		`SELECT x LIKE 'a\\' FROM UNNEST(['a']) x`:                    "LIKE pattern ends with a backslash",
		"SELECT CAST(x AS DATE) FROM UNNEST(['2009/02/13']) x":        "Invalid date: '2009/02/13'",
		"SELECT CAST(x AS TIMESTAMP) FROM UNNEST(['2009-02-13 x']) x": "Invalid timestamp: '2009-02-13 x'",
	} {
		job, err := client.Query(sql).Run(ctx)
		if err == nil {
			var status *bigquery.JobStatus
			if status, err = job.Wait(ctx); err == nil {
				err = status.Err()
			}
		}
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: got error %v, want %q", sql, err, want)
		}
	}
}
