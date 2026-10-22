// Command ddloptions refreshes the DDL option catalog from BigQuery's DDL reference, which it reads
// from standard input, keeping reviewed samples and templates. A new option has no samples and a
// new option list a context without a template, so both stay Untested until reviewed. It never
// queries BigQuery.
//
//	curl -fsSL REFERENCE_URL | ddloptions CATALOG
package main

import (
	"fmt"
	"io"
	"os"
	"regexp"
	"slices"
	"strings"

	"golang.org/x/net/html"

	"github.com/johejo/bigquery-emulator-duckdb/internal/ddloptions"
)

// optionTables returns the rows after the header of each option table, by the anchor of the
// heading above it.
func optionTables(r io.Reader) (map[string][][]string, error) {
	tables := map[string][][]string{}
	tokenizer := html.NewTokenizer(r)
	heading := ""
	var table [][]string
	var row []string
	var cell *strings.Builder
	for {
		switch tokenizer.Next() {
		case html.ErrorToken:
			if tokenizer.Err() == io.EOF {
				return tables, nil
			}
			return nil, tokenizer.Err()
		case html.StartTagToken:
			token := tokenizer.Token()
			switch token.Data {
			case "h2", "h3", "h4", "h5":
				heading = ""
				for _, a := range token.Attr {
					if a.Key == "id" {
						heading = a.Val
					}
				}
			case "table":
				table = [][]string{}
			case "tr":
				if table != nil {
					row = []string{}
				}
			case "td", "th":
				if row != nil {
					cell = &strings.Builder{}
				}
			}
		case html.TextToken:
			if cell != nil {
				cell.Write(tokenizer.Text())
			}
		case html.EndTagToken:
			name, _ := tokenizer.TagName()
			switch string(name) {
			case "td", "th":
				if cell != nil && row != nil {
					row = append(row, strings.Join(strings.Fields(cell.String()), " "))
					cell = nil
				}
			case "tr":
				if row != nil && table != nil {
					table = append(table, row)
					row = nil
				}
			case "table":
				if len(table) > 0 && len(table[0]) > 0 &&
					slices.Contains([]string{"NAME", "Name", "Options"}, table[0][0]) {
					if _, ok := tables[heading]; ok {
						return nil, fmt.Errorf("duplicate option table: %s", heading)
					}
					tables[heading] = table[1:]
				}
				table = nil
			}
		}
	}
}

var externalType = regexp.MustCompile(
	`^(<ARRAY<STRUCT<STRING, STRING>>>|ARRAY<STRING>|STRING|BOOL|INT64|TIMESTAMP|INTERVAL)`)

var typeAliases = map[string]string{
	"BOOLEAN":              "BOOL",
	"SMALLINT":             "INT64",
	"INTEGER":              "INT64",
	"DOUBLE":               "FLOAT64",
	"JSON-FORMATTEDSTRING": "STRING",
}

// sqlType is the GoogleSQL type the reference's VALUE cell names.
func sqlType(group, name, text string) string {
	if group == "external_table_option_list" {
		if name == "uris" {
			// Bigtable takes STRING; the bigtable_uri_option group keeps that form.
			return "ARRAY<STRING>"
		}
		// The external options table puts the type and description in one cell.
		text = externalType.FindString(text)
	}
	if group == "vector_index_option_list" && name == "lexical_search_columns" {
		// The table says ARRAY; its description says this is an array of column names.
		return "ARRAY<STRING>"
	}
	text = strings.ReplaceAll(strings.ToUpper(text), " ", "")
	if strings.HasPrefix(text, "<") && len(text) > 1 {
		text = text[1 : len(text)-1]
	}
	if alias, ok := typeAliases[text]; ok {
		return alias
	}
	return text
}

// prose holds the options the reference describes in prose instead of NAME/VALUE rows.
var prose = map[string]ddloptions.Option{
	"generated_column_options": {Name: "asynchronous", Type: "BOOL", Values: []string{"TRUE"},
		Source: "create_table_statement"},
	"bigtable_uri_option": {Name: "uris", Type: "STRING", Source: "external_table_option_list"},
}

// refresh replaces the catalog's names and types with the reference's, keeping samples, contexts
// and the options described in prose.
func refresh(catalog *ddloptions.Catalog, r io.Reader) error {
	tables, err := optionTables(r)
	if err != nil {
		return err
	}
	if _, ok := tables["table_option_list"]; !ok {
		return fmt.Errorf("DDL reference contains no recognized option tables")
	}
	previous := func(group, name string) (ddloptions.Option, bool) {
		for _, o := range catalog.Groups[group] {
			if o.Name == name {
				return o, true
			}
		}
		return ddloptions.Option{}, false
	}
	groups := map[string][]ddloptions.Option{}
	for group, rows := range tables {
		options := []ddloptions.Option{}
		for _, row := range rows {
			if len(row) < 2 {
				return fmt.Errorf("%s: a row without a type: %q", group, row)
			}
			name := strings.TrimSuffix(row[0], " (Preview)")
			// The ALTER table repeats this option, but its details say it is create-only.
			if group == "materialized_view_set_options_list" && name == "allow_non_incremental_definition" {
				continue
			}
			option, ok := previous(group, name)
			if !ok {
				option = ddloptions.Option{Name: name}
			}
			option.Type = sqlType(group, name, row[1])
			options = append(options, option)
		}
		groups[group] = options
	}
	for group, definition := range prose {
		option := definition
		if old, ok := previous(group, definition.Name); ok {
			option.Values = old.Values
		}
		groups[group] = []ddloptions.Option{option}
	}
	var removed []string
	for group := range catalog.Groups {
		if _, ok := groups[group]; !ok {
			removed = append(removed, group)
		}
	}
	if len(removed) > 0 {
		slices.Sort(removed)
		return fmt.Errorf("option tables disappeared; review the upstream format: %v", removed)
	}
	var added []string
	for group := range groups {
		if !slices.ContainsFunc(catalog.Contexts, func(c ddloptions.Context) bool { return c.Group == group }) {
			added = append(added, group)
		}
	}
	slices.Sort(added)
	for _, group := range added {
		catalog.Contexts = append(catalog.Contexts,
			ddloptions.Context{Name: strings.ReplaceAll(group, "_", " "), Group: group})
	}
	catalog.Groups = groups
	return nil
}

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: ddloptions CATALOG < REFERENCE_HTML")
		os.Exit(2)
	}
	path := os.Args[1]
	catalog, err := ddloptions.Load(path)
	if err == nil {
		err = refresh(catalog, os.Stdin)
	}
	if err == nil {
		// Saved before validation so that a new option the contexts' restrictions hide can be
		// reviewed in the file.
		err = catalog.Save(path)
	}
	if err == nil {
		err = catalog.Validate()
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
