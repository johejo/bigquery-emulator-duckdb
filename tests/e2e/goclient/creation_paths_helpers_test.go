package goclient

import (
	"context"
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
