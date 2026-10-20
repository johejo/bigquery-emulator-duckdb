package goclient

import (
	"context"
	"reflect"
	"strconv"
	"strings"
	"testing"
	"time"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// tableView is what clients see of a table: its resource from tables.get, and its columns,
// defaults and column descriptions from INFORMATION_SCHEMA.
type tableView struct {
	Schema       bigquery.Schema
	Description  string
	Name         string
	Labels       map[string]string
	CreationTime time.Time
	NumRows      uint64
	Columns      [][]bigquery.Value
	FieldPaths   [][]bigquery.Value
}

type creationPaths struct {
	t       *testing.T
	client  *bigquery.Client
	dataset *bigquery.Dataset
}

func (p creationPaths) run(sql string) error {
	p.t.Helper()
	_, _, err := runScript(p.t, p.client, p.dataset.DatasetID, sql)
	return err
}

func (p creationPaths) mustRun(sql string) {
	p.t.Helper()
	if err := p.run(sql); err != nil {
		p.t.Fatalf("%s: %v", sql, err)
	}
}

func (p creationPaths) wait(job *bigquery.Job, err error) error {
	p.t.Helper()
	if err != nil {
		return err
	}
	status, err := job.Wait(context.Background())
	if err != nil {
		return err
	}
	return status.Err()
}

func (p creationPaths) query(sql string, table string, disposition bigquery.TableWriteDisposition) error {
	p.t.Helper()
	query := p.client.Query(sql)
	query.Dst = p.dataset.Table(table)
	query.WriteDisposition = disposition
	return p.wait(query.Run(context.Background()))
}

func (p creationPaths) load(table, rows string, schema bigquery.Schema,
	disposition bigquery.TableWriteDisposition) error {
	p.t.Helper()
	source := bigquery.NewReaderSource(strings.NewReader(rows))
	source.SourceFormat = bigquery.JSON
	source.Schema = schema
	loader := p.dataset.Table(table).LoaderFrom(source)
	loader.WriteDisposition = disposition
	return p.wait(loader.Run(context.Background()))
}

func (p creationPaths) copy(table string, disposition bigquery.TableWriteDisposition,
	sources ...string) error {
	p.t.Helper()
	var tables []*bigquery.Table
	for _, source := range sources {
		tables = append(tables, p.dataset.Table(source))
	}
	copier := p.dataset.Table(table).CopierFrom(tables...)
	copier.WriteDisposition = disposition
	return p.wait(copier.Run(context.Background()))
}

func (p creationPaths) read(sql string, parameters ...bigquery.QueryParameter) [][]bigquery.Value {
	p.t.Helper()
	query := p.client.Query(sql)
	query.DefaultDatasetID = p.dataset.DatasetID
	query.Parameters = parameters
	rows, err := query.Read(context.Background())
	if err != nil {
		p.t.Fatalf("%s: %v", sql, err)
	}
	var result [][]bigquery.Value
	for {
		var row []bigquery.Value
		if err := rows.Next(&row); err == iterator.Done {
			return result
		} else if err != nil {
			p.t.Fatalf("%s: %v", sql, err)
		}
		result = append(result, row)
	}
}

func (p creationPaths) view(table string) tableView {
	p.t.Helper()
	metadata, err := p.dataset.Table(table).Metadata(context.Background())
	if err != nil {
		p.t.Fatalf("%s: %v", table, err)
	}
	name := bigquery.QueryParameter{Name: "table", Value: table}
	// INFORMATION_SCHEMA.TABLES reports the creation time tables.get does.
	created := p.read("SELECT creation_time FROM INFORMATION_SCHEMA.TABLES WHERE table_name = @table", name)
	if len(created) != 1 || len(created[0]) != 1 || !metadata.CreationTime.Equal(timeValue(created[0][0])) {
		p.t.Errorf("%s: INFORMATION_SCHEMA.TABLES creation_time = %v, want %v", table, created, metadata.CreationTime)
	}
	// BigQuery documents a column without a default as NULL; IFNULL reads either spelling the
	// same. Whether a REPEATED column is nullable is not documented, so arrays are left out.
	return tableView{
		Schema:       metadata.Schema,
		Description:  metadata.Description,
		Name:         metadata.Name,
		Labels:       metadata.Labels,
		CreationTime: metadata.CreationTime,
		NumRows:      metadata.NumRows,
		Columns: p.read(`SELECT column_name, is_nullable, data_type, IFNULL(column_default, 'NULL')
		  FROM INFORMATION_SCHEMA.COLUMNS WHERE table_name = @table AND data_type NOT LIKE 'ARRAY%'
		  ORDER BY ordinal_position`, name),
		FieldPaths: p.read(`SELECT field_path, IFNULL(description, '')
		  FROM INFORMATION_SCHEMA.COLUMN_FIELD_PATHS WHERE table_name = @table ORDER BY field_path`, name),
	}
}

// `value` as a time, or the zero time for NULL.
func timeValue(value bigquery.Value) time.Time {
	t, _ := value.(time.Time)
	return t
}

// The columns and field paths INFORMATION_SCHEMA reports for the top-level INT64 and STRING
// columns of `schema`, by the definitions of COLUMNS and COLUMN_FIELD_PATHS.
func informationSchemaOf(schema bigquery.Schema) (columns, paths [][]bigquery.Value) {
	for _, field := range schema {
		paths = append(paths, []bigquery.Value{field.Name, field.Description})
		if field.Repeated {
			continue
		}
		nullable, def := "YES", "NULL"
		if field.Required {
			nullable = "NO"
		}
		if field.DefaultValueExpression != "" {
			def = field.DefaultValueExpression
		}
		types := map[bigquery.FieldType]string{bigquery.IntegerFieldType: "INT64", bigquery.StringFieldType: "STRING"}
		columns = append(columns, []bigquery.Value{field.Name, nullable, types[field.Type], def})
	}
	return columns, paths
}

// Tables created by tables.insert, DDL, and query, load and copy jobs each take the schema,
// defaults and metadata that path gives them, and tables.get and INFORMATION_SCHEMA agree on it.
func TestTableCreationPaths(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_table_creation_paths")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}

	// A string literal default keeps its quotes in defaultValueExpression
	// (https://cloud.google.com/bigquery/docs/default-values).
	declared := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType, Required: true, Description: "key"},
		{Name: "name", Type: bigquery.StringFieldType, DefaultValueExpression: "'none'"},
		{Name: "tags", Type: bigquery.StringFieldType, Repeated: true},
	}
	loaded := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType, Required: true, Description: "key"},
		{Name: "name", Type: bigquery.StringFieldType},
		{Name: "tags", Type: bigquery.StringFieldType, Repeated: true},
	}
	queried := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType},
		{Name: "name", Type: bigquery.StringFieldType},
		{Name: "tags", Type: bigquery.StringFieldType, Repeated: true},
	}
	labels := map[string]string{"env": "dev"}
	for i, c := range []struct {
		name   string
		create func() error
		schema bigquery.Schema
		// The description, friendly name and labels, which only tables.insert and DDL set; nil
		// for a copy, since BigQuery does not document which of them a copy job carries over.
		metadata *bigquery.TableMetadata
		omitted  bigquery.Value // name of a row inserted without it.
	}{
		{"rest", func() error {
			return dataset.Table("rest").Create(ctx, &bigquery.TableMetadata{
				Schema: declared, Description: "the table", Name: "Paths", Labels: labels,
			})
		}, declared, &bigquery.TableMetadata{Description: "the table", Name: "Paths", Labels: labels}, "none"},
		{"ddl", func() error {
			return p.run(`CREATE TABLE ddl (
			  id INT64 NOT NULL OPTIONS (description = 'key'), name STRING DEFAULT 'none', tags ARRAY<STRING>)
			  OPTIONS (description = 'the table', friendly_name = 'Paths', labels = [('env', 'dev')])`)
		}, declared, &bigquery.TableMetadata{Description: "the table", Name: "Paths", Labels: labels}, "none"},
		// A query writes its result's schema, whose columns are nullable.
		{"query", func() error {
			return p.query("SELECT 0 AS id, 'a' AS name, ['x'] AS tags", "query", bigquery.WriteEmpty)
		}, queried, &bigquery.TableMetadata{}, nil},
		{"load", func() error {
			return p.load("load", `{"id": 0, "name": "a", "tags": ["x"]}`, loaded, bigquery.WriteEmpty)
		}, loaded, &bigquery.TableMetadata{}, nil},
		// A copy is a full copy of its source's schema.
		{"copy", func() error { return p.copy("copy", bigquery.WriteEmpty, "load") }, loaded, nil, nil},
		{"copy_defaults", func() error { return p.copy("copy_defaults", bigquery.WriteEmpty, "ddl") }, declared, nil, "none"},
	} {
		t.Run(c.name, func(t *testing.T) {
			p := creationPaths{t: t, client: client, dataset: dataset}
			if err := c.create(); err != nil {
				t.Fatal(err)
			}
			got := p.view(c.name)
			if !reflect.DeepEqual(got.Schema, c.schema) {
				t.Errorf("schema = %+v, want %+v", got.Schema, c.schema)
			}
			columns, paths := informationSchemaOf(c.schema)
			if !reflect.DeepEqual(got.Columns, columns) || !reflect.DeepEqual(got.FieldPaths, paths) {
				t.Errorf("INFORMATION_SCHEMA columns, field paths = %v, %v; want %v, %v",
					got.Columns, got.FieldPaths, columns, paths)
			}
			if want := c.metadata; want != nil && (got.Description != want.Description || got.Name != want.Name ||
				len(got.Labels) != len(want.Labels) || len(want.Labels) != 0 && !reflect.DeepEqual(got.Labels, want.Labels)) {
				t.Errorf("description, friendly name, labels = %q, %q, %v", got.Description, got.Name, got.Labels)
			}
			// Each path inserts an id of its own, so the copy does not see the load's.
			id := strconv.Itoa(10 + i)
			p.mustRun("INSERT " + c.name + " (id) VALUES (" + id + ")")
			want := [][]bigquery.Value{{c.omitted, int64(0)}}
			if rows := p.read("SELECT name, ARRAY_LENGTH(tags) FROM " + c.name + " WHERE id = " + id); !reflect.DeepEqual(rows, want) {
				t.Errorf("row inserted without name and tags = %v, want %v", rows, want)
			}
			if c.schema[0].Required {
				if err := p.run("INSERT " + c.name + " (name) VALUES ('no id')"); err == nil {
					t.Error("inserted a row without its REQUIRED id")
				}
			}
		})
	}

	// A default must have the column's type.
	if err := dataset.Table("bad_default").Create(ctx, &bigquery.TableMetadata{Schema: bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType, DefaultValueExpression: "'one'"},
	}}); err == nil {
		t.Error("tables.insert accepted a STRING default for an INT64 column")
	}

	// A job or statement that fails to create a table leaves none behind.
	for name, create := range map[string]func() error{
		"failed_ddl":   func() error { return p.run("CREATE TABLE failed_ddl AS SELECT ERROR('boom') AS id") },
		"failed_query": func() error { return p.query("SELECT ERROR('boom') AS id", "failed_query", bigquery.WriteEmpty) },
		"failed_load": func() error {
			return p.load("failed_load", `{"id": null}`, loaded[:1], bigquery.WriteTruncate)
		},
	} {
		if err := create(); err == nil {
			t.Errorf("%s: succeeded", name)
		}
		if _, err := dataset.Table(name).Metadata(ctx); err == nil {
			t.Errorf("%s: left a table behind", name)
		}
	}
}

// Writes into an existing table keep or replace its schema, defaults and metadata as their
// disposition says, and omitted columns take their defaults.
func TestWritesToExistingTables(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_writes_to_existing_tables")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}
	p.mustRun(`CREATE TABLE target (id INT64 OPTIONS (description = 'key'), name STRING DEFAULT 'none', tags ARRAY<STRING>)
	  OPTIONS (description = 'the table', labels = [('env', 'dev')])`)
	p.mustRun("CREATE TABLE narrow (id INT64, tags ARRAY<STRING>)")
	p.mustRun("INSERT narrow (id) VALUES (9)")
	created := p.view("target")
	// Appends change the number of rows and nothing else clients see of the table.
	unchanged := func(step string) {
		t.Helper()
		got := p.view("target")
		got.NumRows = created.NumRows
		if !reflect.DeepEqual(got, created) {
			t.Errorf("%s: table = %+v, want %+v", step, got, created)
		}
	}
	names := func() [][]bigquery.Value {
		t.Helper()
		return p.read("SELECT id, name FROM target ORDER BY id")
	}

	// "If the query omits a column with a default value, the default value is assigned", and a
	// load assigns defaults to columns its data does not have but keeps its NULLs
	// (https://cloud.google.com/bigquery/docs/default-values).
	if err := p.query("SELECT 1 AS id", "target", bigquery.WriteAppend); err != nil {
		t.Fatal(err)
	}
	unchanged("query append")
	if err := p.load("target", `{"id": 2}`, bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType}},
		bigquery.WriteAppend); err != nil {
		t.Fatal(err)
	}
	if err := p.load("target", `{"id": 3, "name": null}`, bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType}, {Name: "name", Type: bigquery.StringFieldType},
	}, bigquery.WriteAppend); err != nil {
		t.Fatal(err)
	}
	if got, want := names(), [][]bigquery.Value{{int64(1), "none"}, {int64(2), "none"}, {int64(3), nil}}; !reflect.DeepEqual(got, want) {
		t.Fatalf("appended rows = %v, want %v", got, want)
	}

	// A copy cannot append to a table whose additional columns have defaults
	// (https://cloud.google.com/bigquery/docs/default-values#limitations).
	if err := p.copy("target", bigquery.WriteAppend, "narrow"); err == nil {
		t.Error("copy appended to a table whose additional column has a default")
	}
	if got := names(); len(got) != 3 {
		t.Errorf("rows after a rejected copy = %v", got)
	}
	unchanged("rejected copy")

	// WRITE_TRUNCATE_DATA "overwrites the data, but keeps the constraints and schema of the
	// existing table", while WRITE_TRUNCATE "uses the schema from the query result".
	if err := p.query("SELECT 4 AS id", "target", bigquery.WriteTruncateData); err != nil {
		t.Fatal(err)
	}
	if got, want := names(), [][]bigquery.Value{{int64(4), "none"}}; !reflect.DeepEqual(got, want) {
		t.Errorf("rows after WRITE_TRUNCATE_DATA = %v, want %v", got, want)
	}
	unchanged("WRITE_TRUNCATE_DATA")
	if err := p.query("SELECT 5 AS id, 'five' AS name", "target", bigquery.WriteTruncate); err != nil {
		t.Fatal(err)
	}
	replaced := bigquery.Schema{
		{Name: "id", Type: bigquery.IntegerFieldType}, {Name: "name", Type: bigquery.StringFieldType},
	}
	got := p.view("target")
	columns, paths := informationSchemaOf(replaced)
	if !reflect.DeepEqual(got.Schema, replaced) || !reflect.DeepEqual(got.Columns, columns) ||
		!reflect.DeepEqual(got.FieldPaths, paths) {
		t.Errorf("after WRITE_TRUNCATE: schema = %+v, columns = %v, field paths = %v", got.Schema, got.Columns, got.FieldPaths)
	}
	if rows, want := names(), [][]bigquery.Value{{int64(5), "five"}}; !reflect.DeepEqual(rows, want) {
		t.Errorf("rows after WRITE_TRUNCATE = %v, want %v", rows, want)
	}
}

// A replacement that fails, by any path, leaves the table it would have replaced as it was:
// its rows, schema, defaults, metadata and creation time.
func TestFailedReplacementsKeepTheTable(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_failed_replacements")
	_ = dataset.DeleteWithContents(ctx)
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}
	p.mustRun(`CREATE TABLE keep (id INT64 NOT NULL OPTIONS (description = 'key'), name STRING DEFAULT 'none')
	  OPTIONS (description = 'the table', friendly_name = 'Keep', labels = [('env', 'dev')])`)
	p.mustRun("INSERT keep (id) VALUES (1)")
	p.mustRun("CREATE TABLE other (id INT64)")
	before := p.view("keep")
	required := bigquery.Schema{{Name: "id", Type: bigquery.IntegerFieldType, Required: true}}
	for _, c := range []struct {
		name    string
		replace func() error
	}{
		{"CREATE TABLE", func() error { return p.run("CREATE TABLE keep (id INT64)") }},
		{"CREATE OR REPLACE TABLE AS SELECT", func() error {
			return p.run("CREATE OR REPLACE TABLE keep AS SELECT ERROR('boom') AS id")
		}},
		{"CREATE OR REPLACE TABLE with a violated constraint", func() error {
			return p.run("CREATE OR REPLACE TABLE keep (id INT64 NOT NULL) AS SELECT CAST(NULL AS INT64) AS id")
		}},
		{"tables.insert", func() error {
			return dataset.Table("keep").Create(ctx, &bigquery.TableMetadata{Schema: required})
		}},
		{"query WRITE_TRUNCATE", func() error {
			return p.query("SELECT ERROR('boom') AS id", "keep", bigquery.WriteTruncate)
		}},
		{"query WRITE_EMPTY", func() error { return p.query("SELECT 2 AS id", "keep", bigquery.WriteEmpty) }},
		{"load WRITE_TRUNCATE", func() error {
			return p.load("keep", `{"id": null}`, required, bigquery.WriteTruncate)
		}},
		// Source tables must have identical schemas.
		{"copy WRITE_TRUNCATE", func() error { return p.copy("keep", bigquery.WriteTruncate, "keep", "other") }},
		{"copy WRITE_EMPTY", func() error { return p.copy("keep", bigquery.WriteEmpty, "other") }},
	} {
		if err := c.replace(); err == nil {
			t.Fatalf("%s: succeeded", c.name)
		}
		if after := p.view("keep"); !reflect.DeepEqual(after, before) {
			t.Fatalf("%s: table = %+v, want %+v", c.name, after, before)
		}
		if rows := p.read("SELECT id, name FROM keep"); !reflect.DeepEqual(rows, [][]bigquery.Value{{int64(1), "none"}}) {
			t.Fatalf("%s: rows = %v", c.name, rows)
		}
	}
	p.mustRun("INSERT keep (id) VALUES (2)")
	if rows := p.read("SELECT name FROM keep WHERE id = 2"); !reflect.DeepEqual(rows, [][]bigquery.Value{{"none"}}) {
		t.Errorf("default after failed replacements: %v", rows)
	}
}
