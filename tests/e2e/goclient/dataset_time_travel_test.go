package goclient

import (
	"context"
	"fmt"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
)

// Dataset windows are whole days from 48 through 168 hours, and apply to all tables.
// third_party/bigquery_docs/reference/standard-sql/data-definition-language.md
// third_party/bigquery_docs/reference/rest/v2/datasets.md
// third_party/bigquery_docs/time-travel.md
func TestDatasetTimeTravelWindow(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_time_travel_window")
	_ = dataset.DeleteWithContents(ctx)
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string) error {
		t.Helper()
		defaultDataset := dataset.DatasetID
		if strings.HasPrefix(sql, "CREATE SCHEMA") {
			defaultDataset = ""
		}
		_, _, err := runScript(t, client, defaultDataset, sql)
		return err
	}
	check := func(hours int) {
		t.Helper()
		metadata, err := dataset.Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if want := time.Duration(hours) * time.Hour; metadata.MaxTimeTravel != want {
			t.Fatalf("MaxTimeTravel = %v, want %v", metadata.MaxTimeTravel, want)
		}
	}
	if err := run("CREATE SCHEMA " + dataset.DatasetID + " OPTIONS (max_time_travel_hours = 48)"); err != nil {
		t.Fatal(err)
	}
	check(48)
	if err := run("CREATE TABLE t (id INT64)"); err != nil {
		t.Fatal(err)
	}
	// This is outside the configured window, not merely before this table's creation.
	if err := run("SELECT * FROM t FOR SYSTEM_TIME AS OF TIMESTAMP_SUB(CURRENT_TIMESTAMP(), INTERVAL 3 DAY)"); err == nil || !strings.Contains(err.Error(), "Invalid time travel timestamp") {
		t.Fatalf("outside configured window: %v", err)
	}
	for _, hours := range []int{72, 96, 120, 144, 168} {
		if err := run(fmt.Sprintf("ALTER SCHEMA %s SET OPTIONS (max_time_travel_hours = %d)", dataset.DatasetID, hours)); err != nil {
			t.Fatal(err)
		}
		check(hours)
	}
	if _, err := dataset.Update(ctx, bigquery.DatasetMetadataToUpdate{MaxTimeTravel: 48 * time.Hour}, ""); err != nil {
		t.Fatal(err)
	}
	check(48)
	if err := run("ALTER SCHEMA " + dataset.DatasetID + " SET OPTIONS (description = 'kept')"); err != nil {
		t.Fatal(err)
	}
	check(48)
	for _, hours := range []int{0, 24, 49, 169, -24} {
		if err := run(fmt.Sprintf("ALTER SCHEMA %s SET OPTIONS (max_time_travel_hours = %d)", dataset.DatasetID, hours)); err == nil {
			t.Errorf("accepted invalid window %d", hours)
		}
		check(48)
	}
	if _, err := dataset.Update(ctx, bigquery.DatasetMetadataToUpdate{MaxTimeTravel: 49 * time.Hour}, ""); err == nil {
		t.Error("datasets.patch accepted 49 hours")
	}
	check(48)
	api := client.Dataset("go_time_travel_window_api")
	_ = api.DeleteWithContents(ctx)
	t.Cleanup(func() { _ = api.DeleteWithContents(ctx) })
	if err := api.Create(ctx, &bigquery.DatasetMetadata{MaxTimeTravel: 96 * time.Hour}); err != nil {
		t.Fatal(err)
	}
	metadata, err := api.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.MaxTimeTravel != 96*time.Hour {
		t.Errorf("datasets.insert MaxTimeTravel = %v, want 96h", metadata.MaxTimeTravel)
	}
}
