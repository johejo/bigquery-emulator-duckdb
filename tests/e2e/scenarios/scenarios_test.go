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
	endpoint := os.Getenv("BQ_EMULATOR_API")
	if endpoint == "" {
		t.Skip("run just e2e")
	}
	project := os.Getenv("BQ_EMULATOR_PROJECT")
	if project == "" {
		project = "test"
	}
	paths, err := filepath.Glob("testdata/*.api.y*ml")
	if err != nil {
		t.Fatal(err)
	}
	if len(paths) == 0 {
		t.Skip("no compatibility scenarios have been migrated yet")
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
					transport := bqscenario.NewEmulatorTransport(endpoint, os.Getenv("BQ_EMULATOR_GRPC"))
					t.Cleanup(func() {
						if err := transport.Close(); err != nil {
							t.Error(err)
						}
					})
					if err := file.Verify(t.Context(), c, project, strings.ToLower(rand.Text()), 2*time.Minute, transport.Call); err != nil {
						t.Fatal(err)
					}
				})
			}
		})
	}
}
