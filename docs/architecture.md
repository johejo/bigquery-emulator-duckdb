# Architecture

SQL follows `Server` → `Emulator` → `SqlRuntime` → GoogleSQL analysis → translation →
DuckDB execution. REST metadata operations and Storage RPCs use the same runtime and native
catalog through the application facade.

This page describes ownership and dependency boundaries. See [representations](representations.md),
[UDF runtimes](udf-runtimes.md) and [expiration coordination](expiration.md) for cross-cutting
invariants. Client-visible behavior belongs in [Query and API limits](compatibility.md) and
[Storage API](storage-api.md).

## Runtime and application ownership

[`SqlRuntime`](../src/sql_runtime.h) owns the database, native catalog and query execution.
Scripts use its backend sessions, so analysis and execution see the same transaction. Results
and snapshots retain statement resources independently of job bookkeeping.

[`Emulator`](../src/emulator.h) adds jobs and external services. Destination writes belong to
the runtime, not the job registry: executing SQL does not require registering a job. A bare
runtime has no external providers or connectors and rejects operations that require them.
The compliance driver uses this boundary; `just check` verifies that `//:sql_runtime` and
`//:compliance_test` do not transitively depend on the facade or service SDKs.

## Server

[`Server`](../src/server.h) owns REST and Storage gRPC transports; its `Emulator` must outlive
it. REST routes come from the bundled discovery document. The transport validates parameters;
handlers reject unsupported behavior.

## Frontend

[GoogleSQL](https://github.com/google/googlesql) resolves names and types independently of
DuckDB. Start at [analyzer.cc](../src/analyzer.cc) and [catalog.cc](../src/catalog.cc).

Analyzer declaration policy is separate from builtin exclusions and backend compatibility
options. Normal execution adds BigQuery declarations; compliance uses GoogleSQL builtins.
Statement-local catalogs carry this choice through scripts, routine bodies and views.

GoogleSQL's `ScriptExecutor` manages control flow and variables through
[`ScriptHost`](../src/script.h), without depending on the application facade. Persistent
routines resolve dependencies in their owning project and dataset, excluding job-local objects.

## Translator

[translator.cc](../src/translator.cc) and [src/translator/](../src/translator/) turn the
resolved AST into DuckDB SQL. The translator consumes a read-only
[function catalog](../src/function_catalog/catalog.h); it does not select feature adapters.
Each registration couples translation rules with type-use restrictions, so a physical encoding
such as a graph STRUCT cannot accidentally enable ordinary STRUCT operations.

Registry composition lives in [emulator_functions.cc](../src/emulator_functions.cc).
The emulator and function probe use the same composition. Executable implementations are
registered separately by [backend_functions.cc](../src/backend_functions.cc). Neither the
translator nor the common catalog depends on feature adapters' registration targets.

The registry reads resolved AST details only through
[resolved_access.h](../src/function_catalog/resolved_access.h), keeping heavy AST headers and
translator dependencies out of feature interfaces. See
[AGENTS.md](../AGENTS.md#implementing-functions) for backend selection and registration rules.

## Backend and external services

The backend uses DuckDB's C++ API for execution and the V1 C API for scalar and aggregate
registration. [`duckdb_capi`](../src/duckdb_capi.cc) borrows handles without transferring
ownership; this bridge depends on the pinned DuckDB version's handle representation.

Feature libraries separate semantics from engine integration: `googlesql_<feature>` owns
validation, computation and errors, while its `_duckdb` adapter handles vectors and registration.
The `googlesql_` prefix denotes compatible semantics, not necessarily upstream implementation.

Known BigQuery differences are registration-time
[`functions_duckdb::Options`](../src/googlesql_functions_duckdb/options.h), defaulting to
GoogleSQL behavior; [`BigQueryFunctionOptions()`](../src/function_options.h) enables them.
This lets compliance exercise the same implementations without BigQuery-specific behavior.

[`DuckDbTableSource`](../src/duckdb_table_source.cc) resolves native tables. The application
supplies an [`ExternalTableProvider`](../src/external_table_provider.h) for external reads;
without one, reads fail rather than exposing native placeholder tables. Analysis does not stage
GCS files. `StatementResources` retains staged files until results or Storage snapshots are
consumed. Connector initializers and ObjectRef callbacks likewise keep service dependencies out
of the native backend.

## Metadata and routines

Native catalog operations depend on neither `Emulator` nor the parser. They use the supplied
backend so reads include uncommitted script changes.

Table and column comments store BigQuery metadata; dataset metadata uses
`main.emulator_datasets` because DuckDB cannot comment on schemas. Catalog changes and metadata
writes share a transaction. Views and routines retain GoogleSQL definitions in comments rather
than reconstructing them from translated SQL. Materialized views also retain the base table's
creation identity, so replacing the base invalidates them.
