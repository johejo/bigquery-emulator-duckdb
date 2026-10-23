# Architecture

SQL follows the path `Server` → GoogleSQL analysis → translation → DuckDB execution.
REST metadata operations and Storage RPCs share the same `Emulator` and catalog.

A query result keeps its rows as the DuckDB chunks of the statement's result, which a job keeps
for `jobs.getQueryResults`. A row is encoded as BigQuery's JSON cells only when a response reads
it, so a page encodes only its rows; the emulator's own reads, such as script variables and
extracts, read values from the vectors. Columns whose values can fail to
encode, such as an INTERVAL or BIGNUMERIC out of range, are encoded once when the statement
runs, so that the statement fails rather than a later read.

A Storage Read session keeps its rows as the DuckDB vectors of one query, which selects the
requested fields in GoogleSQL, on a backend session the snapshot owns so that Arrow's connection
properties remain valid. Each response copies its rows of the columns whose DuckDB Arrow layout
matches the BigQuery wire type into a private chunk, exports them through the Arrow C Data
interface. A NULL array becomes an empty one, a BIGNUMERIC's stored bytes become a decimal256,
and infinite RANGE bounds become NULL. Columns with other physical layouts are appended to Arrow
arrays directly from their typed vectors. The resulting arrays are serialized as Arrow IPC with
Apache nanoarrow or encoded as Avro binary rows. Both formats use the table metadata to encode the public schema.

## Server

[`Server`](../src/server.h) owns the [REST](../src/server/rest.cc) and
[Storage gRPC](../src/server/storage.cc) transports; the `Emulator` must outlive it.
[REST routes](../src/server/routes.cc) come from the bundled BigQuery discovery document.
The transport validates parameter names and types, and handlers reject unsupported behavior.

## Frontend

[GoogleSQL](https://github.com/google/googlesql) resolves names and types into an AST, independently
of DuckDB execution. The main entry points are:

- [analyzer.cc](../src/analyzer.cc): statement analysis and query result schemas.
- [catalog.cc](../src/catalog.cc): tables loaded through `TableSource`, built-in functions and
  routines; [table_path.cc](../src/table_path.cc) applies the default project and dataset.
- [type_mapping.cc](../src/type_mapping.cc): REST schema, GoogleSQL and DuckDB type conversions.
- [information_schema.cc](../src/information_schema.cc): metadata rows computed during analysis
  and carried into the translated query. Dataset, table and routine views have separate
  sources, with column definitions beside their row builders.

For scripts, GoogleSQL's `ScriptExecutor` manages control flow and variables.
[script.cc](../src/script.cc) analyzes and executes each statement through `ScriptHost`, which
isolates it from emulator internals. Strict procedure definitions use
[procedure_validation.cc](../src/procedure_validation.cc) to analyze statements and expressions
with typed variables and their constant values, without executing SQL statements. Validation
stops at control statements, DDL, CALL and dynamic SQL, following BigQuery. Constant dynamic SQL
is evaluated by GoogleSQL and checked for syntax. Its routine catalog resolves references in
the procedure's own project, without job-local tables or functions.

## Translator

[translator.cc](../src/translator.cc) and [src/translator/](../src/translator/) turn the resolved
AST into DuckDB SQL. A statement-wide `Context` and per-scan `Scope` track columns and visibility.
Internal column IDs avoid alias collisions; translation preserves GoogleSQL semantics where
DuckDB differs, including NULL ordering and STRUCT comparisons.

[functions.cc](../src/translator/functions.cc) assembles function rules. Calls use DuckDB,
GoogleSQL backend functions or S2 according to their semantics. See
[AGENTS.md](../AGENTS.md#implementing-functions) for backend selection and registration.

Graph definitions use [DuckPGQ](https://github.com/cwida/duckpgq-extension), linked
statically beside DuckDB. The graph translator turns resolved fixed paths into
joins on element tables and endpoint keys, and leaves property expressions and filters to the
ordinary SQL translator. Undirected edges match either orientation with OR over the same edge
row, preserving its properties and emitting self-loops once. Each path applies its own mode
before paths are joined by shared variables' identities. Private graph names encode the project,
dataset and graph name, since DuckPGQ's registry is shared across catalogs. Each project's
`main._property_graphs` table keeps canonical GoogleSQL DDL for catalog resolution
and native DDL for restoring DuckPGQ's registry on startup.
Query intermediates keep graph elements as structs containing a private identity from the
element table and its keys, labels, and a nested property struct. Fixed paths contain separate
node and edge lists, with property types widened to the resolved path's element types. Graph
functions use this representation; it is never returned as an ordinary STRUCT to clients.

## Backend

DuckDB executes translated SQL through its C++ API. Each project is a catalog and each dataset a
schema. Results are encoded in BigQuery's REST row format.
[src/backend_functions/](../src/backend_functions/) exposes GoogleSQL implementations through
DuckDB functions, uses [S2 Geometry](https://s2geometry.io/) for GEOGRAPHY, and keeps the
sketches of `HLL_COUNT` and `APPROX_COUNT_DISTINCT` with a
[port of zetasketch](../src/zetasketch/README.md), whose sketches are BigQuery's.

BIGNUMERIC query values are BIGNUM integers in units of 10^-38; stored columns use the
`bq_bignumeric_units` alias or a precision-and-scale alias. `Backend::NumericReadQuery` converts
external integer and decimal columns to those units at the table boundary. Catalog scans, REST
row reads, copies and extracts share it; writes that retain external numeric storage are rejected.

`EXTERNAL_QUERY` exposes each connection through a DuckDB table macro:

- [MySQL](../src/mysql_query.cc): MariaDB Connector/C prepares the query during analysis; the
  linked [mysql_scanner extension](../third_party/duckdb_mysql/README.md) reads the rows.
- [PostgreSQL](../src/external_query.cc): libpq prepares the query during analysis; the linked
  [postgres_scanner extension](../third_party/duckdb_postgres/README.md) reads the rows.
- [Spanner](../src/spanner_query.cc): a `PLAN` RPC supplies result metadata; google-cloud-cpp
  streams rows into a DuckDB table function with one reader and bounded chunks.

## Metadata and routines

[duckdb_catalog.cc](../src/duckdb_catalog.cc) reads metadata and removes expired relations using
the supplied backend, including uncommitted script changes.
[duckdb_table_source.cc](../src/duckdb_table_source.cc) adapts it to `TableSource`, tracks job
locations and registers external connection macros. Neither depends on `Emulator` or the parser.

Resource operations live in `emulator_datasets.cc`, `emulator_tables.cc` and `emulator_routines.cc`;
`emulator_tabledata.cc` handles rows, and `emulator_ddl.cc` handles query DDL metadata.
Mutations and their response reads share the expiration mutex; expiration drops recheck metadata
within their transaction.

Table and column comments store BigQuery metadata. Dataset metadata lives in
`main.emulator_datasets` because DuckDB cannot comment on schemas. Catalog changes and their
metadata writes share a transaction.

[table_history.cc](../src/table_history.cc) keeps a table's rows before each write for `FOR
SYSTEM_TIME AS OF`, in tables of the project's `main` schema keyed by dataset, table and creation
time. Writes outside a transaction hold a mutex so that each kept version is what the previous
write committed; a transaction dates its versions when it commits.

Logical views are native DuckDB views whose comments preserve the original GoogleSQL definition
and result schema. Materialized views use the same execution path, with their own object type
and base-table creation identity recorded in the comment. Reads compute current results, and
recreating the base table invalidates its materialized views. Persistent SQL UDFs, table functions
and procedures use macro comments.
For analysis, `RoutineCatalog` resolves view and routine definitions against their owning project
and dataset, shares their dependency state and detects recursion. This lets queries use current
definitions while keeping job-local objects out of persistent dependencies.

## DDL option probes

`tools/ddl_options.json` is a snapshot of the option lists in BigQuery's DDL reference. `groups`
maps each list's section anchor to its options: a name, a GoogleSQL type and SQL sample `values`.
Each of `contexts` names a statement that takes a group's options, with a `template` holding
`{option}` once, `setup` statements, and `only` or `exclude` to keep the options a language or
form takes. `implementation` names the DDL handlers' context in `src/ddl_option_types.cc`.

`internal/ddloptions` checks the catalog's structure and builds one statement per sample, which
`tools/restprobe options` runs on fresh emulators to generate `docs/options.md`.
`tests/ddl_options_catalog_test.cc` parses each type and sample with the emulator's analyzer
settings, and compares types with the handlers' definitions, which must all appear in the catalog.

`just update-ddl-options` refreshes names and types from the reference with `tools/ddloptions`,
keeping samples and contexts. A new option has no samples, and a new list a context without a
template, so both stay Untested until someone adds samples that meet the reference's constraints.

See [compatibility.md](compatibility.md) for query and API limits and
[storage-api.md](storage-api.md) for Storage client behavior.

### Python UDF workers

Python routine definitions are stored in the same macro comments as SQL routines, with their
runtime and entry point. The catalog exposes them as volatile functions, and the translator
calls typed `bq_py_*` scalar functions. Each DuckDB local function state starts a worker lazily,
reuses its compiled functions across rows, and destroys it at query cleanup. Independent
execution states use separate processes and Python globals.

The `python_udf_worker` executable links the rules_python CPython 3.11 runtime; the main
emulator does not link or initialize libpython. The worker uses the CPython C API for compilation,
argument conversion and calls. A private Unix socket carries JSON records, with hex for BYTES
and strings for floating-point values so nonfinite values survive transport. Standard input,
output and error point to the null device, separately from the protocol socket. The parent polls
nonblocking I/O against a call deadline and kills the worker process group on cleanup, then
waits for its child. On Linux, address space limits apply before Python initializes. On macOS,
where Darwin rejects `RLIMIT_AS`, the parent samples the worker's resident memory while polling.
