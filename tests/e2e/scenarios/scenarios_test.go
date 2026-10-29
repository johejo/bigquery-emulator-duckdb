package scenarios

import (
	"crypto/rand"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/johejo/bigquery-emulator-duckdb/internal/bqscenario"
)

func TestScenarios(t *testing.T) {
	testScenarios(t, "testdata/*.api.y*ml", false)
}

// As with compliance known errors, a fixed case must be promoted rather than silently passing.
// An execution or cleanup error is not evidence of a known compatibility mismatch.
func TestKnownBugs(t *testing.T) {
	testScenarios(t, "testdata/known_bugs/*.api.y*ml", true)
}

func testScenarios(t *testing.T, pattern string, knownBug bool) {
	t.Helper()
	endpoint := os.Getenv("BQ_EMULATOR_API")
	if endpoint == "" {
		t.Skip("run just e2e")
	}
	project := os.Getenv("BQ_EMULATOR_PROJECT")
	if project == "" {
		project = "test"
	}
	paths, err := filepath.Glob(pattern)
	if err != nil {
		t.Fatal(err)
	}
	if len(paths) == 0 {
		if knownBug {
			t.Skip("no known API compatibility bugs")
		}
		t.Fatal("no API compatibility scenarios found")
	}
	for _, path := range paths {
		file, err := bqscenario.Read(path)
		if err != nil {
			t.Fatal(err)
		}
		t.Run(filepath.Base(path), func(t *testing.T) {
			for _, c := range file.Cases {
				t.Run(c.Name, func(t *testing.T) {
					t.Parallel()
					gcs := os.Getenv("STORAGE_EMULATOR_HOST")
					if gcs != "" && !strings.Contains(gcs, "://") {
						gcs = "http://" + gcs
					}
					transport := bqscenario.NewEmulatorTransport(endpoint, os.Getenv("BQ_EMULATOR_GRPC"), gcs)
					t.Cleanup(func() {
						if err := transport.Close(); err != nil {
							t.Error(err)
						}
					})
					// tests/e2e/gcs seeds the fake GCS server with this bucket.
					err := file.Verify(t.Context(), c, project, strings.ToLower(rand.Text()), "scenarios", 2*time.Minute, transport.Call)
					if knownBug {
						if err == nil {
							t.Fatal("known bug now passes; move this case to testdata/")
						}
						if _, ok := err.(*bqscenario.MismatchError); !ok {
							t.Fatal(err)
						}
						t.Log(err)
					} else if err != nil {
						t.Fatal(err)
					}
				})
			}
		})
	}
}
