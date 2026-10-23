package goclient

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"
)

// The table that the cases in testdata/scalars/*.txt read as `t` from their default dataset, and
// a dataset whose default rounding mode is ROUND_HALF_EVEN, which cases create tables in.
var scalarFixture = []string{
	"CREATE SCHEMA IF NOT EXISTS scalar_graph",
	"CREATE TABLE scalar_graph.Person (id INT64, name STRING)",
	"INSERT scalar_graph.Person VALUES (1, 'Alex'), (2, 'Dana'), (3, 'Lee')",
	"CREATE TABLE scalar_graph.Knows (id INT64, src INT64, dst INT64)",
	"INSERT scalar_graph.Knows VALUES (10, 1, 2), (11, 2, 1), (12, 1, 1), (13, 1, 2)",
	"CREATE TABLE scalar_graph.Account (id INT64, kind STRING)",
	"INSERT scalar_graph.Account VALUES (7, 'checking')",
	"CREATE TABLE scalar_graph.Owns (id INT64, account_id INT64)",
	"INSERT scalar_graph.Owns VALUES (1, 7)",
	"CREATE PROPERTY GRAPH scalar_graph.FinGraph NODE TABLES (scalar_graph.Person KEY (id), scalar_graph.Account KEY (id)) " +
		"EDGE TABLES (scalar_graph.Knows KEY (id) SOURCE KEY (src) REFERENCES Person (id) " +
		"DESTINATION KEY (dst) REFERENCES Person (id), scalar_graph.Owns KEY (id, account_id) " +
		"SOURCE KEY (id) REFERENCES Person (id) DESTINATION KEY (account_id) REFERENCES Account (id))",
	"CREATE PROPERTY GRAPH scalar_graph.HiddenKeys NODE TABLES (scalar_graph.Person KEY (id) NO PROPERTIES) " +
		"EDGE TABLES (scalar_graph.Knows KEY (id) SOURCE KEY (src) REFERENCES Person (id) " +
		"DESTINATION KEY (dst) REFERENCES Person (id) NO PROPERTIES)",
	"CREATE TABLE scalar_graph.Compound (tenant STRING, id INT64)",
	"INSERT scalar_graph.Compound VALUES ('a', 1), ('a', 2), ('b', 1)",
	"CREATE TABLE scalar_graph.CompoundEdge (id STRING, src_tenant STRING, src_id INT64, dst_tenant STRING, dst_id INT64)",
	"INSERT scalar_graph.CompoundEdge VALUES ('x', 'a', 1, 'a', 2), ('y', 'a', 1, 'b', 1), ('loop', 'b', 1, 'b', 1)",
	"CREATE PROPERTY GRAPH scalar_graph.CompoundGraph NODE TABLES (scalar_graph.Compound KEY (tenant, id)) " +
		"EDGE TABLES (scalar_graph.CompoundEdge KEY (id) SOURCE KEY (src_tenant, src_id) REFERENCES Compound (tenant, id) " +
		"DESTINATION KEY (dst_tenant, dst_id) REFERENCES Compound (tenant, id) " +
		"PROPERTIES (id AS edge_id, src_tenant AS origin, dst_tenant AS target))",
	"CREATE PROPERTY GRAPH scalar_graph.AliasedNodes NODE TABLES (scalar_graph.Person AS Authors KEY (id) LABEL Author, " +
		"scalar_graph.Person AS Readers KEY (id) LABEL Reader) " +
		"EDGE TABLES (scalar_graph.Knows KEY (id) SOURCE KEY (src) REFERENCES Authors (id) " +
		"DESTINATION KEY (dst) REFERENCES Readers (id))",
	"CREATE SCHEMA IF NOT EXISTS scalars",
	"CREATE SCHEMA IF NOT EXISTS scalars_rounding_even OPTIONS (default_rounding_mode = 'ROUND_HALF_EVEN')",
	"CREATE OR REPLACE TABLE scalars.t (a INT64, b STRING, raw BYTES)",
	"INSERT INTO scalars.t VALUES (3, 'あ', FROM_HEX('00ff')), (1, 'x', FROM_HEX('61')), " +
		"(2, 'yy', NULL), (NULL, NULL, NULL)",
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

// Retire idle connections before cpp-httplib's five-second keep-alive timeout.
// Otherwise a POST can race the server closing a reused connection and fail with
// EOF; retrying queries is unsafe because they can have side effects.
var queryHTTPClient = func() *http.Client {
	transport := http.DefaultTransport.(*http.Transport).Clone()
	transport.IdleConnTimeout = time.Second
	return &http.Client{Transport: transport}
}()

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
	response, err := queryHTTPClient.Post(url, "application/json", bytes.NewReader(body))
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
	t.Parallel()
	endpoint := os.Getenv("BQ_EMULATOR_API")
	if endpoint == "" {
		t.Skip("BQ_EMULATOR_API is not set; run just e2e")
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
		for _, c := range readQueryCases(t, path) {
			t.Run(fmt.Sprintf("%s:%d", filepath.Base(c.path), c.line), func(t *testing.T) {
				if filepath.Base(path) == "external_query_spanner.txt" && os.Getenv("E2E_SPANNER_DATABASE") == "" {
					t.Skip("Spanner emulator is unavailable on this platform; set SPANNER_EMULATOR_HOST and E2E_SPANNER_DATABASE")
				}
				t.Parallel()
				response, err := runQuery(endpoint, project, "scalars", c.sql)
				queryFailed := response.Error != nil || len(response.Errors) > 0
				if c.knownBug {
					matches := err == nil && len(response.Rows) == 1 && len(response.Rows[0].F) == 1 &&
						reflect.DeepEqual(response.Rows[0].F[0].V, c.want)
					if c.wantError != nil {
						matches = queryFailed && c.matchesError(err)
					}
					if matches {
						t.Errorf("%s:%d: %s: known bug is fixed; write the case with => instead of !>",
							c.path, c.line, c.sql)
					}
					return
				}
				if c.wantError != nil {
					if !queryFailed || !c.matchesError(err) {
						t.Errorf("%s:%d: %s: got error %v, want a query error with message %q", c.path, c.line, c.sql, err, *c.wantError)
					}
					return
				}
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
