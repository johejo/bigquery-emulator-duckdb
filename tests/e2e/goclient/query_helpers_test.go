package goclient

import (
	"strings"
	"testing"

	"cloud.google.com/go/bigquery"
)

// datasetQueries runs queries and write jobs within a test's dataset.
type datasetQueries struct {
	t       *testing.T
	client  *bigquery.Client
	dataset *bigquery.Dataset
}

func (p datasetQueries) run(sql string) error {
	p.t.Helper()
	_, _, err := runScript(p.t, p.client, p.dataset.DatasetID, sql)
	return err
}

func (p datasetQueries) mustRun(sql string) {
	p.t.Helper()
	if err := p.run(sql); err != nil {
		p.t.Fatalf("%s: %v", sql, err)
	}
}

func (p datasetQueries) query(sql string, table string, disposition bigquery.TableWriteDisposition) error {
	p.t.Helper()
	query := p.client.Query(sql)
	query.Dst = p.dataset.Table(table)
	query.WriteDisposition = disposition
	return wait(p.t.Context(), query)
}

func (p datasetQueries) load(table, rows string, schema bigquery.Schema,
	disposition bigquery.TableWriteDisposition) error {
	p.t.Helper()
	source := bigquery.NewReaderSource(strings.NewReader(rows))
	source.SourceFormat = bigquery.JSON
	source.Schema = schema
	loader := p.dataset.Table(table).LoaderFrom(source)
	loader.WriteDisposition = disposition
	return wait(p.t.Context(), loader)
}

func (p datasetQueries) copy(table string, disposition bigquery.TableWriteDisposition,
	sources ...string) error {
	p.t.Helper()
	var tables []*bigquery.Table
	for _, source := range sources {
		tables = append(tables, p.dataset.Table(source))
	}
	copier := p.dataset.Table(table).CopierFrom(tables...)
	copier.WriteDisposition = disposition
	return wait(p.t.Context(), copier)
}

func (p datasetQueries) read(sql string, parameters ...bigquery.QueryParameter) [][]bigquery.Value {
	p.t.Helper()
	query := p.client.Query(sql)
	query.DefaultDatasetID = p.dataset.DatasetID
	query.Parameters = parameters
	rows, err := query.Read(p.t.Context())
	if err != nil {
		p.t.Fatalf("%s: %v", sql, err)
	}
	return readRows(p.t, rows)
}
