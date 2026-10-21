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

// FOR SYSTEM_TIME AS OF reads the rows a table had at a time, whichever kind of write changed
// them since.
// https://cloud.google.com/bigquery/docs/reference/standard-sql/query-syntax#for_system_time_as_of
// https://cloud.google.com/bigquery/docs/time-travel
func TestTimeTravel(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_time_travel")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	mustRun := func(sql string) {
		t.Helper()
		if _, _, err := runScript(t, client, dataset.DatasetID, sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}
	readAt := func(table string, at time.Time) []string {
		t.Helper()
		query := client.Query("SELECT FORMAT('%d:%s', id, name) FROM " + table +
			" FOR SYSTEM_TIME AS OF @ts ORDER BY id")
		query.DefaultDatasetID = dataset.DatasetID
		query.Parameters = []bigquery.QueryParameter{{Name: "ts", Value: at}}
		rows, err := query.Read(ctx)
		if err != nil {
			t.Fatalf("%s at %v: %v", table, at, err)
		}
		got := []string{}
		for {
			var row []bigquery.Value
			if err := rows.Next(&row); err == iterator.Done {
				return got
			} else if err != nil {
				t.Fatal(err)
			}
			got = append(got, row[0].(string))
		}
	}
	mustJob := func(job *bigquery.Job, err error) {
		t.Helper()
		if err := runJob(ctx, job, err); err != nil {
			t.Fatal(err)
		}
	}
	// A time between two jobs, which have committed what they wrote when they finish.
	now := func() time.Time {
		time.Sleep(5 * time.Millisecond)
		defer time.Sleep(5 * time.Millisecond)
		return time.Now()
	}

	mustRun(`CREATE TABLE t (id INT64, name STRING); CREATE TABLE source (id INT64, name STRING);
	  INSERT source (id, name) VALUES (6, 'f')`)
	created := now()
	mustRun(`INSERT t (id, name) VALUES (1, 'a'), (2, 'b')`)
	inserted := now()
	mustRun(`UPDATE t SET name = 'A' WHERE id = 1`)
	updated := now()
	mustRun(`DELETE t WHERE id = 2`)
	deleted := now()
	if err := dataset.Table("t").Inserter().Put(ctx, &bigquery.ValuesSaver{
		Schema: bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType},
			{Name: "name", Type: bigquery.StringFieldType}},
		Row: []bigquery.Value{int64(3), "c"},
	}); err != nil {
		t.Fatal(err)
	}
	streamed := now()
	loader := dataset.Table("t").LoaderFrom(bigquery.NewReaderSource(strings.NewReader("4,d\n")))
	loader.WriteDisposition = bigquery.WriteAppend
	mustJob(loader.Run(ctx))
	loaded := now()
	query := client.Query("SELECT 5 AS id, 'e' AS name")
	query.Dst = dataset.Table("t")
	query.WriteDisposition = bigquery.WriteAppend
	mustJob(query.Run(ctx))
	queried := now()
	copier := dataset.Table("t").CopierFrom(dataset.Table("source"))
	copier.WriteDisposition = bigquery.WriteAppend
	mustJob(copier.Run(ctx))
	copied := now()
	mustRun(`BEGIN TRANSACTION; DELETE t WHERE id = 3; INSERT t (id, name) VALUES (7, 'g');
	  COMMIT TRANSACTION`)
	transacted := now()
	mustRun(`TRUNCATE TABLE t`)

	for _, c := range []struct {
		name string
		at   time.Time
		want []string
	}{
		{"created", created, []string{}},
		{"inserted", inserted, []string{"1:a", "2:b"}},
		{"updated", updated, []string{"1:A", "2:b"}},
		{"deleted", deleted, []string{"1:A"}},
		{"streamed", streamed, []string{"1:A", "3:c"}},
		{"loaded", loaded, []string{"1:A", "3:c", "4:d"}},
		{"queried", queried, []string{"1:A", "3:c", "4:d", "5:e"}},
		{"copied", copied, []string{"1:A", "3:c", "4:d", "5:e", "6:f"}},
		{"transacted", transacted, []string{"1:A", "4:d", "5:e", "6:f", "7:g"}},
		{"truncated", now(), []string{}},
	} {
		if got := readAt("t", c.at); !reflect.DeepEqual(got, c.want) {
			t.Errorf("%s: rows = %v, want %v", c.name, got, c.want)
		}
	}

	// CREATE TABLE COPY and CLONE and CREATE SNAPSHOT TABLE copy the rows a table had then, which a
	// clone and a snapshot record as when they were made.
	// https://cloud.google.com/bigquery/docs/table-snapshots-create
	// https://cloud.google.com/bigquery/docs/table-clones-create
	for _, sql := range []string{
		"CREATE TABLE copied_at COPY t FOR SYSTEM_TIME AS OF @ts",
		"CREATE TABLE cloned_at CLONE t FOR SYSTEM_TIME AS OF @ts",
		"CREATE SNAPSHOT TABLE snapshot_at CLONE t FOR SYSTEM_TIME AS OF @ts",
	} {
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		query.Parameters = []bigquery.QueryParameter{{Name: "ts", Value: updated}}
		mustJob(query.Run(ctx))
	}
	for _, table := range []string{"copied_at", "cloned_at", "snapshot_at"} {
		if got, want := readAt(table, now()), []string{"1:A", "2:b"}; !reflect.DeepEqual(got, want) {
			t.Errorf("%s: rows = %v, want %v", table, got, want)
		}
	}
	at := updated.Truncate(time.Millisecond)
	if clone, err := dataset.Table("cloned_at").Metadata(ctx); err != nil {
		t.Fatal(err)
	} else if clone.CloneDefinition == nil || !clone.CloneDefinition.CloneTime.Equal(at) {
		t.Errorf("cloned_at: clone definition = %+v, want clone time %v", clone.CloneDefinition, at)
	}
	if snapshot, err := dataset.Table("snapshot_at").Metadata(ctx); err != nil {
		t.Fatal(err)
	} else if snapshot.SnapshotDefinition == nil || !snapshot.SnapshotDefinition.SnapshotTime.Equal(at) {
		t.Errorf("snapshot_at: snapshot definition = %+v, want snapshot time %v",
			snapshot.SnapshotDefinition, at)
	}
}

// BigQuery's errors for the times FOR SYSTEM_TIME AS OF cannot read, and the statements that read a
// table at several times, as BigQuery answers.
// https://cloud.google.com/bigquery/docs/reference/standard-sql/query-syntax#for_system_time_as_of
func TestTimeTravelErrorsAndStatements(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_time_travel_errors")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	run := func(sql string, ts time.Time) ([][]bigquery.Value, error) {
		t.Helper()
		query := client.Query(sql)
		query.DefaultDatasetID = dataset.DatasetID
		if strings.Contains(sql, "@ts") {
			query.Parameters = []bigquery.QueryParameter{{Name: "ts", Value: ts}}
		}
		rows, err := query.Read(ctx)
		if err != nil {
			return nil, err
		}
		var got [][]bigquery.Value
		for {
			var row []bigquery.Value
			if err := rows.Next(&row); err == iterator.Done {
				return got, nil
			} else if err != nil {
				return nil, err
			}
			got = append(got, row)
		}
	}
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"CREATE TABLE t (id INT64); INSERT t (id) VALUES (1)"); err != nil {
		t.Fatal(err)
	}
	table := client.Project() + "." + dataset.DatasetID + ".t"
	for sql, want := range map[string]string{
		"SELECT * FROM t FOR SYSTEM_TIME AS OF TIMESTAMP_ADD(CURRENT_TIMESTAMP(), INTERVAL 1 HOUR)": "Query error: 'FOR SYSTEM_TIME AS OF' expression for table '" + table +
			"' evaluates to a TIMESTAMP value in the future: ",
		"SELECT * FROM t FOR SYSTEM_TIME AS OF TIMESTAMP_SUB(CURRENT_TIMESTAMP(), INTERVAL 8 DAY)": "Invalid value: Invalid time travel timestamp ",
		"SELECT * FROM t FOR SYSTEM_TIME AS OF CAST(NULL AS TIMESTAMP)":                            "Query error: 'FOR SYSTEM_TIME AS OF' expression for table '" + table + "' is NULL.",
	} {
		if _, err := run(sql, time.Time{}); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: error = %v, want one containing %q", sql, err, want)
		}
	}

	time.Sleep(5 * time.Millisecond)
	ts := time.Now()
	time.Sleep(5 * time.Millisecond)
	if _, _, err := runScript(t, client, dataset.DatasetID, "INSERT t (id) VALUES (2)"); err != nil {
		t.Fatal(err)
	}
	// A statement reads a table at a time and at the current time.
	got, err := run("SELECT (SELECT COUNT(*) FROM t FOR SYSTEM_TIME AS OF @ts), (SELECT COUNT(*) FROM t)", ts)
	if want := [][]bigquery.Value{{int64(1), int64(2)}}; err != nil || !reflect.DeepEqual(got, want) {
		t.Errorf("two times: rows = %v, err %v, want %v", got, err, want)
	}
	// DML reads its destination at a time.
	if _, err := run("INSERT INTO t SELECT * FROM t FOR SYSTEM_TIME AS OF @ts", ts); err != nil {
		t.Errorf("DML reading its destination at a time: %v", err)
	}
	// A renamed table reads by its new name the rows it had before.
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"CREATE TABLE r (id INT64); INSERT r (id) VALUES (1)"); err != nil {
		t.Fatal(err)
	}
	time.Sleep(5 * time.Millisecond)
	renamed := time.Now()
	time.Sleep(5 * time.Millisecond)
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"INSERT r (id) VALUES (2); ALTER TABLE r RENAME TO r2"); err != nil {
		t.Fatal(err)
	}
	got, err = run("SELECT id FROM r2 FOR SYSTEM_TIME AS OF @ts", renamed)
	if want := [][]bigquery.Value{{int64(1)}}; err != nil || !reflect.DeepEqual(got, want) {
		t.Errorf("renamed: rows = %v, err %v, want %v", got, err, want)
	}
	// A view reads a table at the time its query evaluates.
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"CREATE VIEW v AS SELECT id FROM t FOR SYSTEM_TIME AS OF CURRENT_TIMESTAMP()"); err != nil {
		t.Fatal(err)
	}
	got, err = run("SELECT id FROM v ORDER BY id", time.Time{})
	if want := [][]bigquery.Value{{int64(1)}, {int64(1)}, {int64(2)}}; err != nil || !reflect.DeepEqual(got, want) {
		t.Errorf("view: rows = %v, err %v, want %v", got, err, want)
	}
	// A table function reads a table at a time.
	if _, _, err := runScript(t, client, dataset.DatasetID,
		"CREATE TABLE FUNCTION go_time_travel_errors.f() AS SELECT * FROM go_time_travel_errors.t FOR SYSTEM_TIME AS OF CURRENT_TIMESTAMP()"); err != nil {
		t.Fatal(err)
	}
	got, err = run("SELECT id FROM go_time_travel_errors.f() ORDER BY id", time.Time{})
	if want := [][]bigquery.Value{{int64(1)}, {int64(1)}, {int64(2)}}; err != nil || !reflect.DeepEqual(got, want) {
		t.Errorf("table function: rows = %v, err %v, want %v", got, err, want)
	}
}

// The emulator keeps a table's rows for FOR SYSTEM_TIME AS OF from its creation, and only while its
// columns stay the same, where BigQuery reads a table it replaced and the schema it had; it reads
// only times that do not depend on the time zone.
func TestTimeTravelUnsupported(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_time_travel_unsupported")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	for name, c := range map[string]struct{ write, read string }{
		"replaced": {"CREATE OR REPLACE TABLE replaced (id INT64)", "replaced"},
		"added":    {"ALTER TABLE added ADD COLUMN extra STRING", "added"},
		"dropped":  {"ALTER TABLE dropped DROP COLUMN name", "dropped"},
	} {
		if _, _, err := runScript(t, client, dataset.DatasetID, "CREATE TABLE "+name+
			" (id INT64, name STRING); INSERT "+name+" (id) VALUES (1)"); err != nil {
			t.Fatal(err)
		}
		at := time.Now()
		time.Sleep(5 * time.Millisecond)
		if _, _, err := runScript(t, client, dataset.DatasetID, c.write); err != nil {
			t.Fatal(err)
		}
		query := client.Query("SELECT * FROM " + c.read + " FOR SYSTEM_TIME AS OF @ts")
		query.DefaultDatasetID = dataset.DatasetID
		query.Parameters = []bigquery.QueryParameter{{Name: "ts", Value: at}}
		if _, err := query.Read(ctx); err == nil || !strings.Contains(err.Error(), "does not support") {
			t.Errorf("%s: error = %v, want unsupported", name, err)
		}
	}
	if _, _, err := runScript(t, client, dataset.DatasetID, "CREATE TABLE base (id INT64)"); err != nil {
		t.Fatal(err)
	}
	for _, sql := range []string{
		"CREATE TEMP TABLE s (id INT64); SELECT * FROM s FOR SYSTEM_TIME AS OF CURRENT_TIMESTAMP()",
		"SELECT * FROM base FOR SYSTEM_TIME AS OF CAST(CAST(CURRENT_TIMESTAMP() AS STRING) AS TIMESTAMP)",
	} {
		if _, _, err := runScript(t, client, dataset.DatasetID, sql); err == nil ||
			!strings.Contains(err.Error(), "does not support") {
			t.Errorf("%s: error = %v, want unsupported", sql, err)
		}
	}
}
