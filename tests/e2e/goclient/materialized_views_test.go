package goclient

import (
	"context"
	"reflect"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// Materialized views expose their definition and always include base-table changes when
// max_staleness is unset, even when the cached result has not been refreshed.
// third_party/bigquery_docs/materialized-views-use.md
// third_party/bigquery_docs/materialized-views-create.md
func TestMaterializedViewLifecycle(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_materialized_views")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	mustRun := func(sql string) *bigquery.QueryStatistics {
		t.Helper()
		_, status, err := runScript(t, client, dataset.DatasetID, sql)
		if err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
		return status.Statistics.Details.(*bigquery.QueryStatistics)
	}
	mustRun("CREATE TABLE source (category STRING, amount INT64)")
	mustRun("INSERT source VALUES ('a', 1), ('a', 2), ('b', 4), ('ignored', NULL)")
	definition := "SELECT category, COUNT(*) AS n, SUM(amount) AS total FROM go_materialized_views.source WHERE amount IS NOT NULL GROUP BY category"
	stats := mustRun("CREATE MATERIALIZED VIEW mv OPTIONS (description = 'totals', labels = [('kind', 'aggregate')]) AS " + definition)
	if stats.StatementType != "CREATE_MATERIALIZED_VIEW" || stats.DDLTargetTable == nil || stats.DDLTargetTable.TableID != "mv" {
		t.Fatalf("CREATE statistics = %+v", stats)
	}
	view := dataset.Table("mv")
	metadata, err := view.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.Type != bigquery.MaterializedView || metadata.MaterializedView == nil ||
		metadata.MaterializedView.Query != definition || metadata.ViewQuery != "" ||
		metadata.Description != "totals" || metadata.Labels["kind"] != "aggregate" {
		t.Fatalf("MV metadata = %+v", metadata)
	}
	if len(metadata.Schema) != 3 || metadata.Schema[1].Type != bigquery.IntegerFieldType || metadata.Schema[2].Type != bigquery.IntegerFieldType {
		t.Fatalf("MV schema = %+v", metadata.Schema)
	}
	// BigQuery observation: tabledata.list rejects even an MV whose cache is empty.
	var listedRow []bigquery.Value
	if err := view.Read(ctx).Next(&listedRow); err == nil || !strings.Contains(err.Error(), "Cannot list a table of type MATERIALIZED_VIEW.") {
		t.Fatalf("tabledata.list = %v, want MATERIALIZED_VIEW rejection", err)
	}
	// A metadata-only update must preserve the object kind, definition and creation time.
	updated, err := view.Update(ctx, bigquery.TableMetadataToUpdate{Description: "updated"}, "")
	if err != nil {
		t.Fatal(err)
	}
	if updated.Type != metadata.Type || updated.MaterializedView == nil ||
		updated.MaterializedView.Query != definition || !updated.CreationTime.Equal(metadata.CreationTime) {
		t.Fatalf("updated metadata = %+v", updated)
	}
	checkRows := func(want [][]bigquery.Value) {
		t.Helper()
		rows, err := client.Query("SELECT category, n, total FROM go_materialized_views.mv ORDER BY category").Read(ctx)
		if err != nil {
			t.Fatal(err)
		}
		var got [][]bigquery.Value
		for {
			var row []bigquery.Value
			if err := rows.Next(&row); err == iterator.Done {
				break
			} else if err != nil {
				t.Fatal(err)
			}
			got = append(got, row)
		}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("MV rows = %v, want %v", got, want)
		}
	}
	checkRows([][]bigquery.Value{{"a", int64(2), int64(3)}, {"b", int64(1), int64(4)}})
	mustRun("INSERT source VALUES ('b', 8)")
	mustRun("UPDATE source SET amount = 3 WHERE amount = 1")
	mustRun("DELETE source WHERE amount = 4")
	checkRows([][]bigquery.Value{{"a", int64(2), int64(5)}, {"b", int64(1), int64(8)}})
	mustRun("CREATE MATERIALIZED VIEW IF NOT EXISTS mv AS SELECT amount FROM go_materialized_views.source")
	checkRows([][]bigquery.Value{{"a", int64(2), int64(5)}, {"b", int64(1), int64(8)}})

	rows, err := client.Query("SELECT table_type, ddl FROM go_materialized_views.INFORMATION_SCHEMA.TABLES WHERE table_name = 'mv'").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if row[0] != "MATERIALIZED VIEW" || !strings.HasPrefix(row[1].(string), "CREATE MATERIALIZED VIEW `") {
		t.Fatalf("INFORMATION_SCHEMA.TABLES = %v", row)
	}
	found := false
	tables := dataset.Tables(ctx)
	for {
		table, err := tables.Next()
		if err == iterator.Done {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		if table.TableID == "mv" {
			found = true
		}
	}
	if !found {
		t.Fatal("MV missing from tables.list")
	}

	// The API uses the same validator and metadata representation as DDL.
	apiView := dataset.Table("api_mv")
	if err := apiView.Create(ctx, &bigquery.TableMetadata{
		MaterializedView: &bigquery.MaterializedViewDefinition{Query: definition, EnableRefresh: true},
	}); err != nil {
		t.Fatal(err)
	}
	apiMetadata, err := apiView.Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if apiMetadata.Type != bigquery.MaterializedView || apiMetadata.MaterializedView == nil ||
		apiMetadata.MaterializedView.Query != definition || !apiMetadata.MaterializedView.EnableRefresh ||
		apiMetadata.MaterializedView.RefreshInterval != 30*time.Minute {
		t.Fatalf("API MV metadata = %+v", apiMetadata)
	}
	if err := apiView.Delete(ctx); err != nil {
		t.Fatal(err)
	}
	// Deletion/recreation changes the base table's identity; the old MV cannot read it.
	// third_party/bigquery_docs/materialized-views-intro.md
	mustRun("DROP TABLE source")
	if _, _, err := runScript(t, client, dataset.DatasetID, "SELECT * FROM mv"); err == nil {
		t.Fatal("MV over a deleted base table was readable")
	}
	mustRun("CREATE TABLE source (category STRING, amount INT64)")
	if _, _, err := runScript(t, client, dataset.DatasetID, "SELECT * FROM mv"); err == nil {
		t.Fatal("MV over a recreated base table was readable")
	}
	stats = mustRun("DROP MATERIALIZED VIEW mv")
	if stats.StatementType != "DROP_MATERIALIZED_VIEW" {
		t.Fatalf("DROP statement type = %s", stats.StatementType)
	}
	if _, err := view.Metadata(ctx); err == nil {
		t.Fatal("dropped MV still exists")
	}
	mustRun("DROP MATERIALIZED VIEW IF EXISTS mv")
}

// BigQuery observations: MVs are absent from VIEWS and SELECT * expands added base columns.
func TestMaterializedViewSchemaExpansion(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_materialized_view_expansion")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	mustRun := func(sql string) {
		t.Helper()
		if _, _, err := runScript(t, client, dataset.DatasetID, sql); err != nil {
			t.Fatal(err)
		}
	}
	mustRun("CREATE TABLE source (id INT64); INSERT source VALUES (1); CREATE MATERIALIZED VIEW mv AS SELECT * FROM source")
	rows, err := client.Query("SELECT table_name FROM go_materialized_view_expansion.INFORMATION_SCHEMA.VIEWS").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	var row []bigquery.Value
	if err := rows.Next(&row); err != iterator.Done {
		t.Fatalf("INFORMATION_SCHEMA.VIEWS = %v, %v; want no rows", row, err)
	}
	mustRun("ALTER TABLE source ADD COLUMN extra STRING")
	rows, err = client.Query("SELECT * FROM go_materialized_view_expansion.mv").Read(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if err := rows.Next(&row); err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(row, []bigquery.Value{int64(1), nil}) {
		t.Fatalf("MV after ADD COLUMN = %v, want [1 NULL]", row)
	}
	if err := rows.Next(&row); err != iterator.Done {
		t.Fatalf("extra MV row = %v, %v", row, err)
	}
}

// These forms are forbidden by BigQuery's incremental MV definition restrictions.
// third_party/bigquery_docs/materialized-views-create.md
func TestMaterializedViewRejectsInvalidDefinitions(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_materialized_view_errors")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"CREATE TABLE source (id INT64, word STRING COLLATE 'und:ci'); CREATE FUNCTION go_materialized_view_errors.udf(x INT64) AS (x + 1)"); err != nil {
		t.Fatal(err)
	}
	for _, query := range []string{
		"SELECT RAND() AS x FROM source",
		"SELECT CURRENT_TIMESTAMP() AS x FROM source",
		"SELECT go_materialized_view_errors.udf(id) AS x FROM source",
		"SELECT a.id FROM source AS a JOIN source AS b ON a.id = b.id",
		"SELECT id, ROW_NUMBER() OVER () AS n FROM source",
		"SELECT COUNT(*) / 10 AS n FROM source",
		"SELECT id, COUNT(*) AS n FROM source GROUP BY id HAVING COUNT(*) > 1",
		"SELECT STRING_AGG(CAST(id AS STRING)) AS s FROM source",
		"SELECT id FROM source FOR SYSTEM_TIME AS OF CURRENT_TIMESTAMP()",
		// Collated aggregate sort keys are forbidden even though MV columns may be collated.
		// third_party/bigquery_docs/reference/standard-sql/collation-concepts.md
		"SELECT ARRAY_AGG(id ORDER BY word) AS ids FROM source",
	} {
		if _, _, err := runScript(t, client, dataset.DatasetID, "CREATE MATERIALIZED VIEW invalid AS "+query); err == nil {
			t.Errorf("accepted invalid MV definition: %s", query)
		}
	}
	if _, err := dataset.Table("invalid").Metadata(ctx); err == nil {
		t.Fatal("failed creation left an MV behind")
	}
}
