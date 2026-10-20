package goclient

import (
	"context"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
)

// An option's VALUE "is a constant expression containing only literals, query parameters, and
// scalar functions", which cannot contain functions such as REPEAT, and expiration_timestamp is a
// TIMESTAMP
// (https://cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language).
// BigQuery's answers settle the rest: a STRING literal converts to the TIMESTAMP and a STRING
// expression does not, a bare script variable is ambiguous, CURRENT_TIMESTAMP() is not after the
// creation time, and the messages.
func TestOptionExpressions(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_option_expressions")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string, parameters ...bigquery.QueryParameter) error {
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		query.Parameters = parameters
		job, err := query.Run(ctx)
		if err != nil {
			return err
		}
		status, err := job.Wait(ctx)
		if err != nil {
			return err
		}
		return status.Err()
	}
	mustRun := func(sql string, parameters ...bigquery.QueryParameter) {
		t.Helper()
		if err := run(sql, parameters...); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	metadataOf := func(name string) *bigquery.TableMetadata {
		t.Helper()
		metadata, err := dataset.Table(name).Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		return metadata
	}
	// BigQuery answered an expiration 41 milliseconds before a day after the creation time.
	expiresWithinADay := func(name string) {
		t.Helper()
		metadata := metadataOf(name)
		if got := metadata.ExpirationTime.Sub(metadata.CreationTime); got > 24*time.Hour || got < 24*time.Hour-time.Minute {
			t.Errorf("%s: expiration = %v, creation = %v; want about a day after creation",
				name, metadata.ExpirationTime, metadata.CreationTime)
		}
	}

	mustRun("CREATE TABLE t (id INT64) OPTIONS (expiration_timestamp = TIMESTAMP_ADD(CURRENT_TIMESTAMP(), INTERVAL 1 DAY), description = CONCAT('a', 'b'))")
	expiresWithinADay("t")
	if got := metadataOf("t").Description; got != "ab" {
		t.Errorf("description = %q, want %q", got, "ab")
	}
	mustRun("BEGIN CREATE TABLE script_table (id INT64) OPTIONS (expiration_timestamp = TIMESTAMP_ADD(CURRENT_TIMESTAMP(), INTERVAL 1 DAY)); END")
	expiresWithinADay("script_table")

	// tables.get reports no creation time for a view, so its expiration is checked against the
	// time it was created in.
	before := time.Now()
	mustRun("CREATE VIEW v OPTIONS (expiration_timestamp = TIMESTAMP_ADD(CURRENT_TIMESTAMP(), INTERVAL 48 HOUR)) AS SELECT 1 AS id")
	after := time.Now()
	if got := metadataOf("v").ExpirationTime; got.Before(before.Add(48*time.Hour).Truncate(time.Millisecond)) || got.After(after.Add(48*time.Hour)) {
		t.Errorf("v: expiration = %v, want 48h after %v to %v", got, before, after)
	}

	mustRun("ALTER TABLE t SET OPTIONS (expiration_timestamp = TIMESTAMP_ADD(TIMESTAMP '2099-01-01 00:00:00 UTC', INTERVAL @hours HOUR))",
		bigquery.QueryParameter{Name: "hours", Value: 2})
	if got, want := metadataOf("t").ExpirationTime, time.Date(2099, 1, 1, 2, 0, 0, 0, time.UTC); !got.Equal(want) {
		t.Errorf("t: expiration = %v, want %v", got, want)
	}

	mustRun("CREATE TABLE string_literal (id INT64) OPTIONS (expiration_timestamp = '2099-01-01 00:00:00 UTC')")
	if got, want := metadataOf("string_literal").ExpirationTime, time.Date(2099, 1, 1, 0, 0, 0, 0, time.UTC); !got.Equal(want) {
		t.Errorf("string_literal: expiration = %v, want %v", got, want)
	}

	// An expression that fails, such as a TIMESTAMP_ADD past the type's range, fails the statement.
	if err := run("CREATE TABLE overflow (id INT64) OPTIONS (expiration_timestamp = TIMESTAMP_ADD(TIMESTAMP '9999-12-31 00:00:00 UTC', INTERVAL 2 DAY))"); err == nil || strings.Contains(err.Error(), "does not support") {
		t.Errorf("overflow: error = %v, want the expression's error", err)
	}

	for _, test := range []struct {
		sql  string
		want string
	}{
		{
			sql:  "CREATE TABLE string_expression (id INT64) OPTIONS (expiration_timestamp = CONCAT('2099-01-01', ' 00:00:00 UTC'))",
			want: "Option expiration_timestamp value has type STRING which cannot be coerced to expected type TIMESTAMP",
		},
		{
			sql:  "DECLARE expiration TIMESTAMP DEFAULT TIMESTAMP '2099-01-01 00:00:00 UTC'; CREATE TABLE variable (id INT64) OPTIONS (expiration_timestamp = expiration)",
			want: "Unable to determine if expiration is a string or expression.  If a string is intended, please enclose it with quotation marks.  If an expression is intended, please enclose it with parentheses.",
		},
		{
			sql:  "CREATE TABLE excluded (id INT64) OPTIONS (description = REPEAT('x', 2))",
			want: "Found unsupported function call 'REPEAT'; failed to evaluate 'description' in table OPTIONS",
		},
	} {
		if err := run(test.sql); err == nil || !strings.Contains(err.Error(), test.want) {
			t.Errorf("%s: error = %v, want one containing %q", test.sql, err, test.want)
		}
	}
}
