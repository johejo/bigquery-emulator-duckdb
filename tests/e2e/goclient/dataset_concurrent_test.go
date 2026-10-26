package goclient

import (
	"context"
	"fmt"
	"sync"
	"testing"
)

// IF NOT EXISTS must be a no-op when another job creates the dataset first,
// rather than exposing a catalog write conflict before the next statement runs.
func TestConcurrentCreateSchemaIfNotExists(t *testing.T) {
	t.Parallel()
	client := newClient(t)
	ctx := t.Context()
	for round := range 8 {
		dataset := client.Dataset(fmt.Sprintf("go_concurrent_create_schema_%d", round))
		_ = dataset.DeleteWithContents(ctx)
		t.Cleanup(func() { _ = dataset.DeleteWithContents(context.Background()) })
		const workers = 32
		start := make(chan struct{})
		errors := make(chan error, workers)
		var group sync.WaitGroup
		for range workers {
			group.Go(func() {
				<-start
				sql := fmt.Sprintf("CREATE SCHEMA IF NOT EXISTS %s OPTIONS (description = 'concurrent creation'); SELECT 1", dataset.DatasetID)
				if err := wait(ctx, client.Query(sql)); err != nil {
					errors <- err
				}
			})
		}
		close(start)
		group.Wait()
		close(errors)
		for err := range errors {
			t.Errorf("round %d: %v", round, err)
		}
		metadata, err := dataset.Metadata(ctx)
		if err != nil {
			t.Fatal(err)
		}
		if metadata.Description != "concurrent creation" {
			t.Errorf("round %d: description = %q, want concurrent creation", round, metadata.Description)
		}
	}
}
