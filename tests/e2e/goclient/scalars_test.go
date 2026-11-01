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
	runScalarCases(t, "testdata/scalars", "scalars")
}

// TestGraphScalars runs graph cases with their own fixture, independently of other scalars.
func TestGraphScalars(t *testing.T) {
	t.Parallel()
	runScalarCases(t, "testdata/scalars/graph", "")
}

func runScalarCases(t *testing.T, directory, dataset string) {
	t.Helper()
	endpoint := os.Getenv("BQ_EMULATOR_API")
	if endpoint == "" {
		t.Skip("BQ_EMULATOR_API is not set; run just e2e")
	}
	project := os.Getenv("BQ_EMULATOR_PROJECT")
	if project == "" {
		project = "test"
	}
	setupPath := filepath.Join(directory, "setup.sql")
	setup, err := os.ReadFile(setupPath)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runQuery(endpoint, project, "", string(setup)); err != nil {
		t.Fatalf("%s: %v", setupPath, err)
	}

	paths, err := filepath.Glob(filepath.Join(directory, "*.txt"))
	if err != nil || len(paths) == 0 {
		t.Fatalf("no cases in %s: %v", directory, err)
	}
	for _, path := range paths {
		for _, c := range readQueryCases(t, path) {
			t.Run(fmt.Sprintf("%s:%d", filepath.Base(c.path), c.line), func(t *testing.T) {
				if filepath.Base(path) == "external_query_spanner.txt" && os.Getenv("E2E_SPANNER_DATABASE") == "" {
					t.Skip("Spanner emulator is unavailable on this platform; set SPANNER_EMULATOR_HOST and E2E_SPANNER_DATABASE")
				}
				t.Parallel()
				response, err := runQuery(endpoint, project, dataset, c.sql)
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
