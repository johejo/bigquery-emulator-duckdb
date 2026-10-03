package goclient

import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

// The table that the cases in testdata/scalars/*.txt read as `t` from their default dataset.
var scalarFixture = []string{
	"CREATE SCHEMA IF NOT EXISTS scalars",
	"CREATE OR REPLACE TABLE scalars.t (a INT64, b STRING, raw BYTES)",
	"INSERT INTO scalars.t VALUES (3, 'あ', FROM_HEX('00ff')), (1, 'x', FROM_HEX('61')), " +
		"(2, 'yy', NULL), (NULL, NULL, NULL)",
}

type scalarCase struct {
	path string
	line int
	sql  string
	want any
}

// readScalarCases reads cases written as one query, possibly over several lines, followed by
// "=> " and the JSON of the cell's "v" as jobs.query returns it: a string, or null for NULL.
// Blank lines and lines starting with # between cases are ignored.
func readScalarCases(t *testing.T, path string) []scalarCase {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer file.Close()

	var cases []scalarCase
	var sql []string
	start := 0
	scanner := bufio.NewScanner(file)
	for line := 1; scanner.Scan(); line++ {
		text := scanner.Text()
		switch {
		case strings.HasPrefix(text, "=> "):
			if len(sql) == 0 {
				t.Fatalf("%s:%d: expected value without a query", path, line)
			}
			var want any
			if err := json.Unmarshal([]byte(text[len("=> "):]), &want); err != nil {
				t.Fatalf("%s:%d: %v", path, line, err)
			}
			cases = append(cases, scalarCase{path: path, line: start, sql: strings.Join(sql, "\n"), want: want})
			sql = nil
		case len(sql) > 0:
			sql = append(sql, text)
		case text == "" || strings.HasPrefix(text, "#"):
		default:
			start = line
			sql = append(sql, text)
		}
	}
	if err := scanner.Err(); err != nil {
		t.Fatalf("Scan: %v", err)
	}
	if len(sql) > 0 {
		t.Fatalf("%s:%d: query without an expected value", path, start)
	}
	return cases
}

type queryResponse struct {
	Rows []struct {
		F []struct {
			V any `json:"v"`
		} `json:"f"`
	} `json:"rows"`
	Errors []struct {
		Message string `json:"message"`
	} `json:"errors"`
	Error *struct {
		Message string `json:"message"`
	} `json:"error"`
}

// runQuery calls jobs.query directly so that the cell comes back exactly as the REST API
// encodes it, rather than converted by a client library. An empty dataset means no default.
func runQuery(endpoint, project, dataset, sql string) (queryResponse, error) {
	request := map[string]any{"query": sql, "useLegacySql": false}
	if dataset != "" {
		request["defaultDataset"] = map[string]string{"projectId": project, "datasetId": dataset}
	}
	body, err := json.Marshal(request)
	if err != nil {
		return queryResponse{}, err
	}
	url := fmt.Sprintf("%s/bigquery/v2/projects/%s/queries", endpoint, project)
	response, err := http.Post(url, "application/json", bytes.NewReader(body))
	if err != nil {
		return queryResponse{}, err
	}
	defer response.Body.Close()
	var decoded queryResponse
	if err := json.NewDecoder(response.Body).Decode(&decoded); err != nil {
		return queryResponse{}, err
	}
	if decoded.Error != nil {
		return decoded, fmt.Errorf("%s", decoded.Error.Message)
	}
	if len(decoded.Errors) > 0 {
		return decoded, fmt.Errorf("%s", decoded.Errors[0].Message)
	}
	return decoded, nil
}

// TestScalars runs each query of testdata/scalars/*.txt and compares its one cell. These are the
// many small cases where a BigQuery function or operator must answer as BigQuery does.
func TestScalars(t *testing.T) {
	endpoint := os.Getenv("BQ_EMULATOR_API")
	if endpoint == "" {
		t.Skip("BQ_EMULATOR_API is not set; start the emulator with tests/e2e/run.sh")
	}
	project := os.Getenv("BQ_EMULATOR_PROJECT")
	if project == "" {
		project = "test"
	}
	for _, sql := range scalarFixture {
		if _, err := runQuery(endpoint, project, "", sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}

	paths, err := filepath.Glob("testdata/scalars/*.txt")
	if err != nil || len(paths) == 0 {
		t.Fatalf("no cases in testdata/scalars: %v", err)
	}
	for _, path := range paths {
		for _, c := range readScalarCases(t, path) {
			t.Run(fmt.Sprintf("%s:%d", filepath.Base(c.path), c.line), func(t *testing.T) {
				t.Parallel()
				response, err := runQuery(endpoint, project, "scalars", c.sql)
				if err != nil {
					t.Fatalf("%s:%d: %s: %v", c.path, c.line, c.sql, err)
				}
				if len(response.Rows) != 1 || len(response.Rows[0].F) != 1 {
					t.Fatalf("%s:%d: %s: got %d rows, want one cell", c.path, c.line, c.sql,
						len(response.Rows))
				}
				if got := response.Rows[0].F[0].V; !reflect.DeepEqual(got, c.want) {
					t.Errorf("%s:%d: %s: got %#v, want %#v", c.path, c.line, c.sql, got, c.want)
				}
			})
		}
	}
}
