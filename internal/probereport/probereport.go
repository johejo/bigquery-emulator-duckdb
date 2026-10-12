// Package probereport turns the outcomes of probes into the Markdown support tables that
// tools/restprobe and tools/storageprobe generate.
package probereport

import (
	"fmt"
	"io"
	"sort"
	"strings"
	"unicode"
)

// Outcome is how one probe of a form ended.
type Outcome int

const (
	Runs Outcome = iota
	Broken
	Unsupported
	Untested
)

// Probe is the outcome of one probe, with the feature it rejected as unsupported or why it failed.
type Probe struct {
	Outcome Outcome
	Detail  string
}

// Status is Supported, Partial, Unsupported, Broken or Untested, from how many probes had each
// outcome.
func Status(counts map[Outcome]int) string {
	tested := counts[Runs] + counts[Unsupported] + counts[Broken]
	switch {
	case counts[Broken] > 0:
		return "Broken"
	case tested == 0:
		return "Untested"
	case counts[Runs] == tested:
		return "Supported"
	case counts[Runs] == 0:
		return "Unsupported"
	}
	return "Partial"
}

// Notes builds a row's Notes cell from its handwritten note and what its probes found. The forms
// rejected as unsupported are listed only when others run, since the status says the rest; the
// failures of a Broken or Untested row say what to fix.
func Notes(note, st string, rejected, failures map[string]bool) string {
	var parts []string
	if note != "" {
		parts = append(parts, note)
	}
	switch {
	case st == "Partial" && len(rejected) > 0:
		parts = append(parts, "Unsupported: "+strings.Join(SortedNames(rejected), "; ")+".")
	case st == "Broken" || st == "Untested":
		parts = append(parts, strings.Join(SortedNames(failures), "; "))
	}
	return Escape(strings.Join(parts, " "))
}

// Escape escapes the pipes that would end a Markdown table cell.
func Escape(text string) string { return strings.ReplaceAll(text, "|", `\|`) }

// statuses lists the statuses in the order summaries show them.
var statuses = []string{"Supported", "Partial", "Unsupported", "Broken", "Untested"}

// Tally counts the rows of a section by status.
type Tally struct {
	Title  string
	Counts map[string]int
}

// WriteSummary prints a table of how many rows of each section have each status. Each section has
// a row of its own, so changes to different sections do not conflict.
func WriteSummary(w io.Writer, heading string, tallies []Tally) {
	var columns []string
	for _, st := range statuses {
		for _, t := range tallies {
			if t.Counts[st] > 0 {
				columns = append(columns, st)
				break
			}
		}
	}
	fmt.Fprintf(w, "| %s | %s |\n|%s\n", heading, strings.Join(columns, " | "),
		strings.Repeat(" --- |", len(columns)+1))
	for _, t := range tallies {
		fmt.Fprintf(w, "| [%s](#%s) |", t.Title, anchor(t.Title))
		for _, st := range columns {
			fmt.Fprintf(w, " %d |", t.Counts[st])
		}
		fmt.Fprintln(w)
	}
}

// anchor is the fragment GitHub links a Markdown heading with.
func anchor(heading string) string {
	var b strings.Builder
	for _, r := range strings.ToLower(heading) {
		switch {
		case r == ' ':
			b.WriteRune('-')
		case r == '-' || r == '_' || unicode.IsLetter(r) || unicode.IsDigit(r):
			b.WriteRune(r)
		}
	}
	return b.String()
}

// SortedNames returns the keys of entries in order.
func SortedNames[V any](entries map[string]V) []string {
	names := make([]string, 0, len(entries))
	for name := range entries {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}
