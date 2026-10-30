# Support-table catalogs

`tools/restprobe` runs the probes behind the generated support tables, and `just docs`
regenerates them; each `*_preamble.md` here opens its page and says what the page's statuses
mean. This page describes the catalogs that the function type, collation and DDL option probes
read, and how to extend them.

## Function types

`//:function_probe` enumerates GoogleSQL signatures and builds calls using
`tools/function_probe_hints.txt`; `restprobe functions` aggregates their outcomes per
function. That is separate from the specification-driven type coverage in
[function-types.md](../../docs/function-types.md).

`tools/functioncoverage` is a Go generator for reviewed classes of type tables and prose domains
in the saved BigQuery documentation. `just function-types-update` generates
`tools/function_types.json`, a reviewable list of concrete argument and result types with local
source pages. It extracts unary INPUT/OUTPUT tables and binary input matrices from mathematical
and aggregate function pages, with explicit rules for synonyms and optional arguments. Unknown
table shapes or types fail generation. `domains.go` expands reviewed STRING/BYTES, numeric,
orderable, array-element and arbitrary-value domains, checking that their source sections exist.
These prose rules require review when the specification changes; they are not inferred from
emulator registrations.

To expand coverage, add a reviewed extraction or domain rule and its source snapshot, regenerate
the catalog, and review the concrete signatures. Keep function-specific valid samples in the
catalog's generation rules and recursive typed samples in `internal/functioncoverage`.

The JSON catalog stays a single file; the report has an overview in `docs/function-types.md` and
detail pages under `docs/function-types/`, grouped by specification page. `just docs-function-types`
regenerates this report set alone. `restprobe -check function-types` checks it without modifying
files, including detecting obsolete domain pages, which regeneration removes; `just docs-check`
runs it.

## Collation

`tools/collationcoverage` lists the sections of the saved function, expression and operator
references that say they support collation; `just collation-operations-update` writes them to
`tools/collation_operations.json`. `//:function_probe` calls each listed function with a collated
argument in every signature GoogleSQL marks as using the operation collation, and fails for a
listed function no call collates, so that the documentation and GoogleSQL's signatures cannot
drift apart unnoticed. A function whose collated call the emulator rejects is Partial in
[functions.md](../../docs/functions.md). The other listed operations, and where a collation can be
assigned, are the Collation section of `tools/sql_features.txt`.

## DDL options

`tools/ddl_options.json` is a snapshot of the option lists in BigQuery's DDL reference. `groups`
maps each list's section anchor to its options: a name, a GoogleSQL type and SQL sample `values`.
Each of `contexts` names a statement that takes a group's options, with a `template` holding
`{option}` once, `setup` statements, and `only` or `exclude` to keep the options a language or
form takes. `implementation` names the DDL handlers' context in `src/ddl_option_types.cc`.

`internal/ddloptions` checks the catalog's structure and builds one statement per sample, which
`restprobe options` runs. `tests/ddl_options_catalog_test.cc` parses each type and sample with
the emulator's analyzer settings, and compares types with the handlers' definitions, which must
all appear in the catalog.

`just update-ddl-options` refreshes names and types from the reference's snapshot under
`third_party/bigquery_docs/` with `tools/ddloptions`, keeping samples and contexts. A new option
has no samples, and a new list a context without a template, so both stay Untested until someone
adds samples that meet the reference's constraints.
