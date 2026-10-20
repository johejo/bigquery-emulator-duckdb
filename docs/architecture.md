# Architecture

SQL follows the path `Server` → GoogleSQL analysis → translation → DuckDB execution.
REST metadata operations and Storage RPCs share the same `Emulator` and catalog.

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
isolates it from emulator internals.

## Translator

[translator.cc](../src/translator.cc) and [src/translator/](../src/translator/) turn the resolved
AST into DuckDB SQL. A statement-wide `Context` and per-scan `Scope` track columns and visibility.
Internal column IDs avoid alias collisions; translation preserves GoogleSQL semantics where
DuckDB differs, including NULL ordering and STRUCT comparisons.

[functions.cc](../src/translator/functions.cc) assembles function rules. Calls use DuckDB,
GoogleSQL backend functions or S2 according to their semantics. See
[AGENTS.md](../AGENTS.md#implementing-functions) for backend selection and registration.

## Backend

DuckDB executes translated SQL through its C++ API. Each project is a catalog and each dataset a
schema. Results are encoded in BigQuery's REST row format.
[src/backend_functions/](../src/backend_functions/) exposes GoogleSQL implementations through
DuckDB functions and uses [S2 Geometry](https://s2geometry.io/) for GEOGRAPHY.

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

Logical views are native DuckDB views whose comments preserve the original GoogleSQL definition
and result schema. Persistent SQL UDFs, table functions and procedures use macro comments.
For analysis, `RoutineCatalog` resolves view and routine definitions against their owning project
and dataset, shares their dependency state and detects recursion. This lets queries use current
definitions while keeping job-local objects out of persistent dependencies.

See [compatibility.md](compatibility.md) for query and API limits and
[storage-api.md](storage-api.md) for Storage client behavior.
