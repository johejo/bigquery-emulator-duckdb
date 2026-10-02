package goclient

import (
	"context"
	"reflect"
	"testing"

	"cloud.google.com/go/bigquery"
	"google.golang.org/api/iterator"
)

// The iterators of datasets.list and tables.list send maxResults and follow nextPageToken.
// A page token names the last entry of its page, so deleting an entry the iterator has already
// returned does not skip the next one. Datasets whose names start with an underscore are listed
// only with ListHidden.
func TestListPages(t *testing.T) {
	ctx := context.Background()
	// A project of its own, so that other tests' datasets do not show up.
	client := newClientForProject(t, "go-list-pages")
	for _, id := range []string{"c", "a", "_hidden", "b"} {
		dataset := client.Dataset(id)
		_ = dataset.DeleteWithContents(ctx)
		if err := dataset.Create(ctx, nil); err != nil {
			t.Fatal(err)
		}
		t.Cleanup(func() { _ = dataset.DeleteWithContents(ctx) })
	}

	datasetIDs := listDatasetIDs(t, client.Datasets(ctx), func(dataset *bigquery.Dataset) {
		if dataset.DatasetID == "a" {
			if err := dataset.Delete(ctx); err != nil {
				t.Fatal(err)
			}
		}
	})
	if want := []string{"a", "b", "c"}; !reflect.DeepEqual(datasetIDs, want) {
		t.Errorf("datasets: got %v, want %v", datasetIDs, want)
	}
	all := client.Datasets(ctx)
	all.ListHidden = true
	if got, want := listDatasetIDs(t, all, nil), []string{"_hidden", "b", "c"}; !reflect.DeepEqual(got, want) {
		t.Errorf("datasets with ListHidden: got %v, want %v", got, want)
	}

	dataset := client.Dataset("b")
	for _, id := range []string{"t1", "t2", "t3"} {
		if err := dataset.Table(id).Create(ctx, &bigquery.TableMetadata{
			Schema: bigquery.Schema{{Name: "x", Type: bigquery.IntegerFieldType}},
		}); err != nil {
			t.Fatal(err)
		}
	}
	var pages [][]string
	pager := iterator.NewPager(dataset.Tables(ctx), 2, "")
	for {
		var tables []*bigquery.Table
		token, err := pager.NextPage(&tables)
		if err != nil {
			t.Fatal(err)
		}
		var ids []string
		for _, table := range tables {
			ids = append(ids, table.TableID)
		}
		pages = append(pages, ids)
		if token == "" {
			break
		}
	}
	if want := [][]string{{"t1", "t2"}, {"t3"}}; !reflect.DeepEqual(pages, want) {
		t.Errorf("table pages: got %v, want %v", pages, want)
	}
}

// listDatasetIDs reads `datasets` one dataset per page, calling `visit`, if given, on each.
func listDatasetIDs(t *testing.T, datasets *bigquery.DatasetIterator,
	visit func(*bigquery.Dataset)) []string {
	t.Helper()
	datasets.PageInfo().MaxSize = 1
	var ids []string
	for {
		dataset, err := datasets.Next()
		if err == iterator.Done {
			return ids
		}
		if err != nil {
			t.Fatal(err)
		}
		ids = append(ids, dataset.DatasetID)
		if visit != nil {
			visit(dataset)
		}
	}
}
