package goclient

import (
	"context"
	"testing"
	"time"
)

// Keep SDK timestamp decoding and local-clock bounds in Go. Clone definitions,
// inheritance, defaults, independence and replacement are in create_table_copy.api.yaml.
func TestCloneTimeDecoding(t *testing.T) {
	t.Parallel()
	ctx := context.Background()
	client := newClient(t)
	dataset := client.Dataset("go_clone_time_decoding")
	if err := dataset.Create(ctx, nil); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	p := creationPaths{t: t, client: client, dataset: dataset}
	p.mustRun("CREATE TABLE source (id INT64)")
	before := time.Now().Add(-time.Second)
	p.mustRun("CREATE TABLE cloned CLONE source")
	after := time.Now().Add(time.Second)
	metadata, err := dataset.Table("cloned").Metadata(ctx)
	if err != nil {
		t.Fatal(err)
	}
	if metadata.CloneDefinition == nil {
		t.Fatal("missing clone definition")
	}
	cloneTime := metadata.CloneDefinition.CloneTime
	if cloneTime.Before(before) || cloneTime.After(after) {
		t.Fatalf("clone time = %v, want between %v and %v", cloneTime, before, after)
	}
}
