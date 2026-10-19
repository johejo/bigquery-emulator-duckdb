# Architecture

SQL requests pass through the server, frontend, translator and backend. REST metadata operations
and Storage RPCs share the same emulator.

## Server

[`Server`](../src/server.h) manages the [REST](../src/server/rest.cc) and
[Storage gRPC](../src/server/storage.cc) transports. The `Emulator` must outlive it.
`Start()` waits for listeners to become ready; `Stop()` requests shutdown; `Wait()` joins serving
threads. Destruction also stops and joins them. Instances are single-use, and the CLI announces
endpoints only after startup succeeds.

REST uses [cpp-httplib](https://github.com/yhirose/cpp-httplib) and
[nlohmann/json](https://github.com/nlohmann/json).
[src/server/routes.cc](../src/server/routes.cc) builds routes from the bundled
[BigQuery v2 discovery document](../third_party/bigquery/discovery.json), accepting both
`/bigquery/v2` and root paths. It validates query parameter names and types; handlers reject
valid parameters they cannot implement.

## Frontend

[GoogleSQL](https://github.com/google/googlesql) parses and analyzes queries into a resolved AST
with names bound and expressions typed. The frontend is independent of DuckDB execution details.

- [catalog.cc](../src/catalog.cc) loads tables lazily through `TableSource`, and
  [table_path.cc](../src/table_path.cc) completes table paths from the default project and
  dataset. Functions and types come from GoogleSQL's built-ins plus BigQuery functions it lacks.
  [table_source.h](../src/table_source.h) gains a method with most metadata features, so it is a
  Bazel target of its own: only what implements or calls it is rebuilt when it changes.
- [type_mapping.cc](../src/type_mapping.cc) maps TableFieldSchema, GoogleSQL and DuckDB types.
  DuckDB column, parameter and expression types are derived from GoogleSQL types.
- [information_schema.cc](../src/information_schema.cc) computes view rows during analysis;
  the translated query carries those rows to DuckDB. Each view is an entry of `kViews`: its
  name, the qualifiers that scope it, its columns and the function that builds its rows from the
  datasets the qualifier selects, through `TableSource`, which counts PARTITIONS' rows too.
- [analyzer.cc](../src/analyzer.cc) uses the parser's language options, reports analysis failures
  as `invalidQuery`, and supplies query result schemas from resolved output columns.

For multi-statement queries, GoogleSQL's `ScriptExecutor` manages control flow and variables.
[script.cc](../src/script.cc) analyzes and translates each statement or expression it reaches,
exposing script variables as catalog constants and decoding expression results from DuckDB rows.
It reaches the emulator only through `ScriptHost`, so it does not depend on the emulator's
internals.

## Translator

[translator.cc](../src/translator.cc) translates the resolved AST into DuckDB SQL. Scans become
derived tables with columns named by resolved IDs; output names are applied at the boundary to
avoid alias collisions. Translation accounts for differences such as NULL ordering and result types.

The sources in [src/translator/](../src/translator/) share a statement-wide `Context` and a
`Scope` describing what each scan can see. A subquery names enclosing columns without a
qualifier; an aggregate scan repeats those its aggregates read as columns of its input, since DuckDB
would evaluate an aggregate that reads only enclosing columns in the enclosing query.
`DuckDbStructFieldNames` maps anonymous or duplicate
STRUCT names to positional internal names. Construction, casts and field access use that mapping;
JSON functions receive the original names separately so they preserve JSON keys. Comparisons of
STRUCTs access fields by position instead, since DuckDB's composite equality takes NULL fields as
equal. DuckDB has no STRUCT without fields, so a `STRUCT<>` is a STRUCT whose one field,
`$empty`, is TRUE; translation lets only JSON functions and ARRAY constructors see it. User-visible
limits are documented in [compatibility.md](compatibility.md#struct-values).

[functions.cc](../src/translator/functions.cc) assembles DuckDB names, SQL templates, GoogleSQL
backend calls, handlers, and aggregate/window rules. Templates use `$n` for argument n, `#n`
for a date-part string, and `!n` for a rule-defined error; for example, `DATE_DIFF(a, b, DAY)`
becomes `date_diff('day', b, a)`. See [AGENTS.md](../AGENTS.md#implementing-functions) for backend
selection and compatibility requirements.

## Backend

[DuckDB](https://duckdb.org/) executes translated SQL through its C++ API. Each project is a
catalog and each dataset a schema. Results are encoded in BigQuery's `{"f": [{"v": ...}]}` format.
[src/backend_functions/](../src/backend_functions/) registers functions using GoogleSQL's
implementations. Most register a GoogleSQL function, or a small adapter of one, through `Apply` in
[apply.h](../src/backend_functions/apply.h), which reads its arguments and writes its result by
their C++ types. GEOGRAPHY functions, which GoogleSQL does not implement, use the
[S2 Geometry](https://s2geometry.io/) library over the S2 points and loops that a GEOGRAPHY holds in queries, written as text. GCS load jobs download objects with
[google-cloud-cpp](https://github.com/googleapis/google-cloud-cpp) to temporary files for DuckDB.

`EXTERNAL_QUERY` reads PostgreSQL through DuckDB's postgres_scanner extension, which is built
from source against the C++ headers of the DuckDB release in use and linked into the binary (see
[third_party/duckdb_postgres](../third_party/duckdb_postgres/README.md)). The catalog prepares the
query with libpq while the statement is analyzed, maps the result's PostgreSQL types to BigQuery's
in [external_query.cc](../src/external_query.cc), and attaches the database to DuckDB once; the
translator reads it with `postgres_query` and converts each column.

Both backends expose a per-connection DuckDB table macro to the translator. Spanner's `PLAN`
RPC supplies result metadata without running the query; the public google-cloud-cpp RowStream
API does not expose this metadata. Execution uses google-cloud-cpp's Spanner client to stream
rows into a DuckDB C API table function, with one reader and bounded chunks. Dates and
timestamps use DuckDB's native types; decimal and JSON text are cast to their resolved types.

Logical views store translated SQL and explicit GoogleSQL output types in native DuckDB views.
For query analysis, `RoutineCatalog` resolves their stored GoogleSQL definitions anew against
the view's project and dataset, and supplies SQL views for GoogleSQL to inline. Persistent SQL
UDFs and table functions resolve in the same shared dependency state, without job-local objects;
the state owns their analyses and catalog objects and detects recursive dependencies. Table
functions use the same macro-comment storage as persistent scalar functions and procedures.
View comments hold the original query and result schema as JSON. Creation and metadata writes
share a transaction, preserving the previous definition on replacement failure.

[duckdb_catalog.cc](../src/duckdb_catalog.cc) reads recorded metadata and sweeps expired
relations through the supplied backend, including a script session's uncommitted changes.
[duckdb_table_source.cc](../src/duckdb_table_source.cc) adapts those reads to `TableSource`,
records job locations and registers external connection macros. Both are separate Bazel libraries with
private implementation dependencies to limit the headers exposed to callers and the scope of
clang-tidy cache invalidation. Neither depends on `Emulator` or GoogleSQL's parser.

The emulator's resource operations live in `emulator_datasets.cc`, `emulator_tables.cc` and
`emulator_routines.cc`; `emulator_tabledata.cc` handles row reads and writes, and
`emulator_ddl.cc` handles catalog checks and metadata for query DDL. The shared expiration mutex
still covers mutations through their response reads, and each expiration drop rechecks the
relation's comment in its transaction.

Column comments hold TableFieldSchema metadata; table comments hold descriptions, friendly names,
labels, partitioning and clustering. View comments also hold this metadata alongside the query.
Dataset metadata lives in the project's `main.emulator_datasets` table because DuckDB cannot
comment on schemas. Schema creation or deletion and its metadata update share a transaction.
