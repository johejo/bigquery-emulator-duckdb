package main

import (
	"reflect"
	"strings"
	"testing"

	"github.com/johejo/bigquery-emulator-duckdb/internal/ddloptions"
)

// ddl is the DDL reference's URL, which the snapshot's links keep.
const ddl = "https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language"

const reference = "## `CREATE TABLE` statement\n\n" +
	"- [`table_option_list`](" + ddl + "#table_option_list) : options.\n\n" +
	"### `table_option_list`\n\n" +
	"<table>\n<tbody>\n<tr class=\"header\">\n<th><code>NAME</code></th>\n<th><code>VALUE</code></th>\n<th>Details</th>\n</tr>\n" +
	"<tr><td><code>description</code></td><td><p><code>STRING</code></p></td><td>Description</td></tr>\n" +
	"<tr><td>new_option</td><td>BOOLEAN</td><td>A new option</td></tr>\n</tbody>\n</table>\n\n" +
	"## `CREATE NEW` statement\n\n" +
	"- [`option_list`](" + ddl + "#new_option_list) : options.\n\n" +
	"### `option_list`\n\n" +
	"| `NAME`   | `VALUE`                         | Details |\n" +
	"|----------|---------------------------------|---------|\n" +
	"| `labels` | `ARRAY<STRUCT<STRING, STRING>>` | [Labels](https://example.com) |\n"

func catalog() *ddloptions.Catalog {
	return &ddloptions.Catalog{
		Groups: map[string][]ddloptions.Option{
			"table_option_list": {{Name: "description", Type: "STRING", Values: []string{"'reviewed'"}}},
		},
		Contexts: []ddloptions.Context{{Name: "CREATE TABLE", Group: "table_option_list", Template: "CREATE TABLE n OPTIONS ({option})"}},
	}
}

func refreshed(t *testing.T, c *ddloptions.Catalog) *ddloptions.Catalog {
	t.Helper()
	if err := refresh(c, strings.NewReader(reference)); err != nil {
		t.Fatal(err)
	}
	return c
}

func TestPreservesReviewedSamplesAndTemplates(t *testing.T) {
	t.Parallel()
	c := refreshed(t, catalog())
	if got := c.Groups["table_option_list"][0].Values; !reflect.DeepEqual(got, []string{"'reviewed'"}) {
		t.Errorf("values = %q", got)
	}
	if c.Contexts[0].Template != "CREATE TABLE n OPTIONS ({option})" {
		t.Errorf("template = %q", c.Contexts[0].Template)
	}
}

func TestNewOptionsAndGroupsHaveNoGuessedSamplesOrTemplates(t *testing.T) {
	t.Parallel()
	c := refreshed(t, catalog())
	if got := c.Groups["table_option_list"][1]; !reflect.DeepEqual(got, ddloptions.Option{Name: "new_option", Type: "BOOL"}) {
		t.Errorf("new option = %+v", got)
	}
	added := c.Contexts[len(c.Contexts)-1]
	if added.Group != "new_option_list" || added.Template != "" {
		t.Errorf("new context = %+v", added)
	}
	if got := c.Groups["new_option_list"][0].Type; got != "ARRAY<STRUCT<STRING,STRING>>" {
		t.Errorf("type = %s", got)
	}
	if err := c.Validate(); err != nil {
		t.Error(err)
	}
}

func TestRejectsChangedUpstreamFormat(t *testing.T) {
	t.Parallel()
	err := refresh(catalog(), strings.NewReader("# Unavailable\n"))
	if err == nil || !strings.Contains(err.Error(), "no recognized option tables") {
		t.Errorf("refresh() = %v", err)
	}
}

func TestAnchorsWithoutLinksAreHeadings(t *testing.T) {
	t.Parallel()
	tables, err := optionTables(strings.NewReader("### `column_option_list`\n\n| Name | Value |\n|--|--|\n| `a` | `INT64` |\n"))
	if err != nil {
		t.Fatal(err)
	}
	if got := tables["column_option_list"]; !reflect.DeepEqual(got, [][]string{{"a", "INT64"}}) {
		t.Errorf("tables = %q", tables)
	}
}

func TestRejectsDisappearedTables(t *testing.T) {
	t.Parallel()
	c := catalog()
	c.Groups["missing_list"] = nil
	err := refresh(c, strings.NewReader(reference))
	if err == nil || !strings.Contains(err.Error(), "option tables disappeared") {
		t.Errorf("refresh() = %v", err)
	}
}

func TestRefreshIsIdempotent(t *testing.T) {
	t.Parallel()
	once := refreshed(t, catalog())
	twice := refreshed(t, refreshed(t, catalog()))
	if !reflect.DeepEqual(once, twice) {
		t.Errorf("once = %+v, twice = %+v", once, twice)
	}
}
