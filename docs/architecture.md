# Architecture

SQL follows `Server` → `Emulator` → `SqlRuntime` → GoogleSQL analysis → translation →
DuckDB execution. REST metadata operations and Storage RPCs use the same runtime and native
catalog through the application facade.

This page maps the main components and their dependency boundaries. Detailed representations
are in [Value and result representations](representations.md), worker execution in
[UDF runtimes](udf-runtimes.md), and expiration coordination in
[Expiration and history](expiration.md). Client-visible behavior belongs in
[Query and API limits](compatibility.md) and the [Storage API guide](storage-api.md).

## Runtime and application ownership

[`SqlRuntime`](../src/sql_runtime.h) owns the backend database, project registrations and
aliases, function configuration, native catalog operations and the single query execution
path in [sql_runtime_query.cc](../src/sql_runtime_query.cc). It also owns expiration, graph
and per-table history locks and the script namespace counter. Scripts use a backend session
of this runtime; analysis and statements see that session's transaction. Results and snapshots
retain statement resources independently of job bookkeeping.

[`Emulator`](../src/emulator.h) is the application facade. Its runtime base supplies native
API methods without duplicating state. [emulator_jobs.cc](../src/emulator_jobs.cc) records
jobs, errors, times and locations, and orchestrates load, copy and extract. Destination writes,
including dispositions, metadata and history, live in
[sql_runtime_destination.cc](../src/sql_runtime_destination.cc), not the job registry.
`SqlRuntime::ExecuteQuery` updates statement metadata without registering a job or converting
exceptions into job failures.

The facade owns GCS and the external connection registry and supplies table-provider factories,
ObjectRef callbacks and connector initializers. A bare runtime installs no external services:
it omits ObjectRef metadata fetch without a callback and rejects external table operations
without a provider. Service-independent ObjectRef operations remain available.

The compliance driver constructs the runtime directly and prepares fixtures through its native
catalog and SQL APIs. It installs no external services or UDF runtimes. `just check` verifies
that `//:sql_runtime` and `//:compliance_test` have no transitive dependency on the facade,
load acquisition, extraction or Cloud Storage SDK.

## Server

[`Server`](../src/server.h) owns the [REST](../src/server/rest.cc) and
[Storage gRPC](../src/server/storage.cc) transports; the `Emulator` must outlive it.
[REST routes](../src/server/routes.cc) come from the bundled discovery document. The transport
validates parameter names and types, and handlers reject unsupported behavior.

## Frontend

[GoogleSQL](https://github.com/google/googlesql) resolves names and types into an AST,
independently of DuckDB execution.

| Component | Responsibility |
|---|---|
| [analyzer.cc](../src/analyzer.cc) | Statement analysis and result schemas |
| [catalog.cc](../src/catalog.cc), [table_path.cc](../src/table_path.cc) | Table and graph resolution through `TableSource`, statement-local `EXTERNAL_QUERY`, default project and dataset |
| [builtin_catalog.cc](../src/builtin_catalog.cc), [analyzer_functions](../src/analyzer_functions/) | Cached builtin catalogs and BigQuery-specific declarations, argument constraints and TVF schemas |
| [type_mapping.cc](../src/type_mapping.cc), [duckdb_types.cc](../src/duckdb_types.cc) | REST/GoogleSQL/DuckDB type conversion; reading backend result and storage types |
| [information_schema.cc](../src/information_schema.cc) | Metadata rows computed during analysis and carried into the query |

`FunctionDeclarations` selects analyzer declarations independently of builtin exclusions and
backend compatibility options. Normal execution adds BigQuery declarations; compliance selects
GoogleSQL builtins only, without `EXTERNAL_QUERY`. The selection follows scripts, routine bodies
and views through their statement-local catalogs.

For scripts, GoogleSQL's `ScriptExecutor` manages control flow and variables.
[script.cc](../src/script.cc) executes statements through `ScriptHost`, isolating it from
emulator internals. [procedure_validation.cc](../src/procedure_validation.cc) validates strict
procedures without executing SQL. It stops at control statements, DDL, CALL and dynamic SQL,
following BigQuery; constant dynamic SQL is evaluated by GoogleSQL and checked for syntax.
Its catalog resolves references in the procedure's project, without job-local objects.

## Translator

[translator.cc](../src/translator.cc) and [src/translator/](../src/translator/) turn the
resolved AST into DuckDB SQL. A statement-wide `Context` and per-scan `Scope` track columns
and visibility. Internal column IDs avoid alias collisions. Translation preserves differences
such as NULL ordering and STRUCT comparison semantics.

The translator receives a read-only [function catalog](../src/function_catalog/catalog.h).
The catalog owns registry entries and rule expansion, but does not select feature adapters.
Each registration keeps its translation and `TypeUse` attributes together; overload rules
restrict arguments. These attributes reject unsupported uses of physical representations such
as graph STRUCTs or BIGNUMERIC. GEOGRAPHY support is a type attribute, independent of the
backend reported by the function support table.

The builtin tables are `functions_template.cc`, `functions_backend.cc` and
`functions_aggregate.cc` under [src/translator/](../src/translator/).
[Static definitions](../src/function_catalog/static_definitions.h) own overload arrays beside
names and attributes in a `constexpr` tuple; views are taken only after it reaches static
storage. Registration copies the definitions into the catalog.

[resolved_access](../src/function_catalog/resolved_access.h) owns `ScalarCall`'s AST accessors
for arguments, types and literals. Feature handlers use these without depending on the
translator or exposing resolved AST definitions in headers. Shared
[comparison helpers](../src/function_catalog/comparison.h) serve STRUCT subqueries and handlers.

[emulator_functions.cc](../src/emulator_functions.cc) assembles builtin definitions and feature
providers for each choice of compatibility options. It also registers COLLATE's handler,
keeping the translator independent of the collation library. The emulator and function probe
use this same composition. [backend_functions.cc](../src/backend_functions.cc) registers
executable implementations at the application layer. Neither the translator nor the common
catalog depends on feature adapters' registration targets.

Adapters can share definitions between SQL rule construction and backend registration, as
geography and trigonometric functions do. See [AGENTS.md](../AGENTS.md#implementing-functions)
for backend selection and registration, and [representations.md](representations.md) for
collation, graph and partition-result translation.

## Backend and external services

The backend owns DuckDB databases, connections, prepared statements and result chunks through
the C++ API. It executes SQL, loads static extensions, registers catalog functions and exports
Arrow arrays. Scalar and aggregate registration still use the V1 C API.
[`duckdb_capi`](../src/duckdb_capi.cc) borrows connection handles without transferring ownership;
this boundary depends on the pinned DuckDB version's handle representation. DuckDB C++ headers
stay in implementation and internal vector helper files.

[googlesql_functions](../src/googlesql_functions/) owns scalar compatibility logic without
DuckDB dependencies. [googlesql_functions_duckdb](../src/googlesql_functions_duckdb/) registers
those functions and upstream GoogleSQL functions, converts vectors and storage representations,
and manages aggregate callbacks. Feature libraries follow the same split:
`googlesql_<feature>` owns validation, computation and errors; its `_duckdb` adapter handles
registration and representations. Shared helpers live in [duckdb_functions](../src/duckdb_functions/).
The `googlesql_` prefix denotes compatible semantics, including emulator implementations.

Known BigQuery differences are flags that default to GoogleSQL behavior. Adapters read
[`functions_duckdb::Options`](../src/googlesql_functions_duckdb/options.h) at registration and
pass only each function's flags to its owned callback context.
[`BigQueryFunctionOptions()`](../src/function_options.h) selects the differences by default.
Its `query` field carries [`QueryRestrictions`](../src/function_catalog/query_restrictions.h)
through the catalog to the translator. [Development](development.md#forms-only-googlesql-accepts)
documents forms available only in GoogleSQL mode.

[`DuckDbTableSource`](../src/duckdb_table_source.cc) owns native resolution, aliases, locations,
time travel, numeric conversion and write restrictions. The application's
[`ExternalTableProvider`](../src/external_table_provider.h), implemented by
[service_table_provider](../src/service_table_provider.cc), prepares GCS and Bigtable reads and
registers external connections. Without a provider, external reads fail rather than reading
native placeholder tables.

GCS definitions retain metadata, schema and client by value. Analysis does not stage files;
`StatementResources` keeps staged files alive through result or Storage snapshot consumption.
[`gcs_uri`](../src/gcs_uri.cc) and [`bigtable_options`](../src/bigtable_options.cc) validate
metadata without service SDK dependencies. [bigtable_external](../src/bigtable_external.cc)
discovers families and reads current cells through google-cloud-cpp.

`EXTERNAL_QUERY` uses a DuckDB table macro for each connection. PostgreSQL's
[libpq path](../src/external_query.cc) and MySQL's
[MariaDB Connector/C path](../src/mysql_query.cc) prepare during analysis, then use linked
scanner extensions. [Spanner](../src/spanner_query.cc) gets metadata with a `PLAN` RPC and streams
rows into a table function with one reader and bounded chunks.
[service_backend](../src/service_backend.cc) supplies connector registration through backend
initializers. ObjectRef similarly receives lookup and fetch callbacks;
[object_ref.cc](../src/object_ref.cc) owns GCS fetches and
[connection_id.cc](../src/connection_id.cc) normalizes connection IDs. A native `Backend`
registers no external connectors by default.

## Metadata and routines

Resource operations live in `sql_runtime_datasets.cc`, `sql_runtime_tables.cc` and
`sql_runtime_routines.cc`; `sql_runtime_tabledata.cc` handles rows and `sql_runtime_ddl.cc`
handles DDL metadata. [duckdb_catalog.cc](../src/duckdb_catalog.cc) reads metadata and removes
expired relations using the supplied backend, including uncommitted script changes. These
native components depend on neither `Emulator` nor the parser.

Table and column comments store BigQuery metadata. Dataset metadata lives in
`main.emulator_datasets`, since DuckDB cannot comment on schemas. Catalog changes and metadata
writes share a transaction. [Expiration and history](expiration.md) describes coordination,
policy visibility and retained row versions.

Logical views are native DuckDB views whose comments preserve GoogleSQL definitions and result
schemas. Materialized views use the same execution path, with an object type and base-table
creation identity in the comment; recreating the base invalidates them. Persistent SQL UDFs,
table functions and procedures use macro comments. `RoutineCatalog` resolves definitions in
their owning project and dataset, shares dependency state and detects recursion, keeping
job-local objects out of persistent dependencies.
