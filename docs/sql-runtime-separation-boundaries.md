# SQL runtime separation: boundary inventory

This is the step 1 inventory for [the separation plan](sql-runtime-separation-plan.md),
inspected at `7774306d0b6b1bb839fa9e6f0cdf30bfaefcaaf4` on Darwin arm64.
It describes existing ownership and the intended allocation, not an implemented runtime.
No production behavior, driver semantics or known-error entries change in this step.

## Dependency baseline

Run `just sql-runtime-deps` to print the complete `allpaths` subgraphs from
`//:compliance_test` to `//:gcs`, `@google_cloud_cpp//:storage` and, separately,
`@google_cloud_cpp//:spanner`. Graph factoring is disabled so every target/edge is visible.
This uses Bazel's dependency graph, including `implementation_deps`, rather than source
include searches. `query` conservatively includes selectable branches; it is not evidence
about the inputs of a particular configured link action.

The complete GCS subgraph has these outgoing edges (prefix `//:` omitted):

| Target | Dependencies on paths to GCS |
|---|---|
| `compliance_test` | `emulator` |
| `emulator` | `ddl_write`, `duckdb_table_source`, `object_ref`, `duckdb_catalog`, `gcs`, `table_comments`, `file_read` |
| `ddl_write` | `table_comments`, `duckdb_catalog` |
| `duckdb_table_source` | `external_table`, `table_comments`, `gcs`, `duckdb_catalog` |
| `duckdb_catalog` | `table_comments` |
| `external_table` | `gcs`, `file_read` |
| `file_read` | `gcs` |
| `object_ref` | `gcs` |
| `table_comments` | `gcs` |

The Storage subgraph adds exactly `gcs -> @google_cloud_cpp//:storage` to those
edges. In particular, removing `Emulator::gcs_client_` does not sever the metadata
paths. `table_comments.cc::TableCommentStatement` calls `FindGcsWildcard`
to validate source URIs; it needs pure validation, not an acquisition client.

The separate Spanner subgraph has these outgoing edges:

| Target | Dependencies on paths to Spanner |
|---|---|
| `compliance_test` | `emulator`, `result_value` |
| `emulator` | `script`, `backend`, `table_history`, `ddl_write`, `result_value`, `duckdb_table_source`, `duckdb_catalog` |
| `script` | `backend`, `result_value` |
| `result_value` | `backend` |
| `table_history` | `backend` |
| `ddl_write` | `duckdb_catalog` |
| `duckdb_catalog` | `backend` |
| `duckdb_table_source` | `spanner_query`, `backend`, `table_history`, `duckdb_catalog` |
| `backend` | `backend_functions` |
| `backend_functions` | `spanner_query` |
| `spanner_query` | `@google_cloud_cpp//:spanner` |

`Backend::Database` registers PostgreSQL and MySQL scanners, and
`RegisterBackendFunctions` registers Spanner unconditionally. Moving table resolution
alone cannot isolate these adapters. `BackendInitializer` is an existing composition
boundary for application registration; service-independent builtin registration stays shared.

## Implemented boundaries after steps 2–3

The dependency tables above are the original baseline, not the current graph.
`table_comments` now uses `gcs_uri` for pure URI validation; acquisition uses the same
implementation, with unchanged parsing rules and errors. Nearby metadata/schema/catalog
helpers have no remaining direct acquisition includes.

`duckdb_table_source` accepts an optional owned `ExternalTableProvider` and keeps the
single native catalog/analyzer path. `service_table_provider`, constructed by
`Emulator::NewTableSource`, implements GCS reads and external connection discovery.
It borrows the application's one `ExternalConnections` registry, preserving its lock
and shared-backend registration outside session transactions. Its deferred GCS reads
own copies of the metadata, schema and client rather than borrowing the provider.
Native sources without a provider reject external operations explicitly.

`service_backend` composes Spanner registration through `BackendInitializer` and
PostgreSQL/MySQL scanner loading through `DatabaseInitializer`. Native backend defaults
and common builtin function registration no longer depend on concrete connectors.
The application still constructs these adapters for production, including shell use.
ObjectRef acquisition remains in `Emulator` until step 4.

The following check now passes:

```sh
just sql-runtime-deps-check //:backend //:duckdb_table_source \\
  //:duckdb_catalog //:table_comments
bazelisk query 'somepath(set(//:backend //:duckdb_table_source), set(//:external_query //:mysql_query //:postgres_scanner //:spanner_query))'
```

The second command returns an empty graph, checking connector isolation separately
from Storage isolation. Compliance still depends on the application, so its final
assertion is not enabled yet. Ownership/missing-provider tests live in
`tests/duckdb_table_source_test.cc`; production behavior remains covered by the existing
client tests below, without copying their SQL into unit tests.

Validation on Darwin arm64: `just fmt` and `just check` passed, including the existing
external-table, external-query, ObjectRef, load/extract, shell and Storage client tests.
The focused `array_functions_test` compliance run also passed; its 173 per-statement
outcomes match `sql-runtime-separation-baseline.tsv` exactly (153 pass, 6 known failure,
14 unsupported). Full compliance and final runtime/compliance link isolation remain
for the driver extraction steps.

## Implemented shared runtime (step 4)

`//:sql_runtime` now owns the backend, project registry/aliases, native catalog APIs,
query analysis/translation/execution, scripts, destination writes and all SQL locks and
counters. The `sql_runtime_*.cc` sources are the moved implementations, not a second
executor. `Emulator` inherits the native APIs and wraps runtime execution with the one
application job registry. Its `emulator_jobs.cc` retains load/copy/extract orchestration;
the native write operation it calls is `SqlRuntime::WriteDestination`.

Only the facade constructs GCS-backed ObjectRef callbacks, the external-table provider
factory and connector initializers. Runtime defaults install none, and its assembled
function catalog omits `OBJ.FETCH_METADATA` when the backend has no fetch callback.
ObjectRef validation remains registered. Provider preparations keep their existing owned
state and statement-resource lifetime. No fake service client is installed.

`just sql-runtime-deps-check //:sql_runtime` passes, and `just check` now includes that
assertion. `just sql-runtime-deps //:sql_runtime` also reports no paths to GCS, Storage
or Spanner. The new runtime test covers concurrent script namespace/session isolation,
consumption of results after runtime destruction and missing ObjectRef fetch capability.
`just fmt` and `just check` pass, including C++ tests, end-to-end tests and generated-doc
checks. The full `just compliance` run (32 shards) and `just compliance-summary` also pass
with the facade using the extracted executor. Compliance still links `:emulator`; its
driver switch, before/after comparison and actual link input inspection remain step 5. The allocation table below is the original inventory;
its runtime/application ownership is now implemented, while driver migration is pending.

## Responsibility and mutable-state allocation

The table preserves pre-extraction names and source locations. “Runtime” means the
shared SQL runtime; “application” means the production facade/services. Each state item
gets one owner, not a copy in both.

| Existing implementation/state | Intended owner and boundary |
|---|---|
| `Emulator::backend_`; `Backend::Database` | Runtime owns the database. `Backend` keeps connection creation, DDL serialization and result conversion; application-specific connector registration moves outward. |
| `projects_`, `project_ids_`, `data_dir_`, startup attachment/registry restoration in `emulator.cc` | Runtime owns native projects, aliases and persistence. Both query and API catalog calls resolve through the same registry. |
| `excluded_function_ids_`, `declarations_`, `has_session_user_`, geography/function options | Runtime configuration, supplied by composition. Preserve the driver's GoogleSQL options independently of production BigQuery corrections. |
| `Translate`, `NewTableSource`, `RunStatement`, `Execute`, `Prepare`, `InferLocation`, `UseTargets` in `emulator_query.cc` | Runtime, with a single analyzer/catalog/translator path and per-statement resources. No runtime-to-job callback. |
| Dataset/table/routine APIs in `emulator_{datasets,tables,routines}.cc`; DDL in `emulator_ddl.cc` | Runtime native catalog operations, including defaults, metadata, expiration and read-only restrictions. API handlers and load/copy destinations must delegate rather than write around them. |
| `expiration_mutex_` | Runtime; recursive locking covers expiry and mutations plus their response lookups. Keep the same lock across API and SQL mutations. |
| `history_mutexes_mutex_`, `history_mutexes_`, `KeepVersion`, `HistoryCreationTime` | Runtime; per-table write/history serialization, including job destinations. Transactional history uses the session and before-commit actions, not a second connection. |
| `graph_mutex_` | Runtime; serializes property-graph changes because DuckPGQ updates all connection registries. |
| `next_script_number_` | Runtime; unique temporary database names, shared across all query callers. |
| `ScriptHostImpl` in `emulator_query.cc`, executor in `script.cc` | Runtime. Script execution owns a `Backend::NewSession()` connection. Analysis/catalog reads and statements see that session's uncommitted state. Preserve transaction controls and cleanup on the same connection. |
| `Backend::Session::{connection,in_transaction,before_commit}`, database DDL mutex | Remain owned by `Backend`, not the application or a second compliance executor. |
| `JobLocation` and table-source location cache | Per execution, not global runtime state. Application supplies the requested location; runtime infers/references datasets, DDL targets, regions and external connection locations. Application records the resolved location in `Job`. |
| `QueryResult`, `TableRead`, `StatementResources`, `StorageSnapshot` | Runtime result/resource ownership. Deferred preparations and staged files must survive execution and result consumption; rows keep resources and Storage snapshots keep a session plus resources. A provider must retain its own required state for deferred work, not borrow a short-lived table source. |
| `WriteDestination` in `emulator_jobs.cc`; insert and Storage row APIs in `emulator_tabledata.cc` | Native write/disposition/transaction logic belongs to runtime even where currently placed in the jobs file. Share schema checks, history, numeric conversion and expiration with SQL; application supplies decoded load rows or query/copy requests. |
| `mutex_`, `jobs_`, `running_jobs_`, `next_job_number_`, `RunJob`, job CRUD | Application; lock covers job registration/bookkeeping, not the SQL execution body. Keep duplicate-ID handling, dry-run behavior, timestamps and error recording outside runtime. Query execution returns its result/error and statement/DDL metadata without storing a REST job. |
| `RunLoad`, `RunExtract`, file staging/upload and format orchestration in `emulator_jobs.cc` | Application services, using runtime catalog/read/write operations. Copy orchestration stays application; native snapshot/clone/write semantics remain runtime operations. |
| `gcs_client_`, `storage_endpoint()`, constructor ObjectRef fetch callback | Application. Separate `FetchObjectMetadata` acquisition from service-independent ObjectRef validation/operations. Callback lifetime must not exceed the client/connection configuration it captures. |
| `connections_` (`conninfo`, `attached`, mutex and shared backend reference) | Application-composed external provider. Keep one synchronized attachment registry; discovery references connection location and registers outside a script transaction through the shared database. Do not copy this registry into each table source. |
| `DuckDbTableSource` native lookup, aliases, location cache, history and write restrictions | Runtime native table source. GCS `ReadQuery` preparation and `DescribeExternalQuery` adapter dispatch move to supplied capabilities, not an alternate catalog. Missing providers explicitly reject external operations, never read the native placeholder table. |

The move must preserve lock scope and acquisition order at existing call sites. In
particular, API mutation responses currently hold expiration protection, script DDL uses
its session, and result resources can outlive translation. This inventory does not
introduce a new global query lock or prescribe a generic plugin API.

## Compliance driver surface and baseline

`tests/compliance/test_driver.cc::EmulatorTestDriver` uses:

- Construction/reset with in-memory project registration, GoogleSQL declarations,
  empty exclusions and uncorrected function options; then `CreateDataset`.
- `CreateTable` with filtered/annotated schemas. Fixture rows are converted to a typed
  parameter and inserted through `RunQuery`, not directly into DuckDB.
- `RunQuery` for property-graph definitions during fixture preparation.
- `RunQuery` for compared SELECTs with parameters, a default dataset and
  `null_arrays = true`. `JobStatus` and `ResultValue` convert errors/results back to
  GoogleSQL. Non-query comparisons are rejected before mutation.

The future runtime API must cover these operations without job registration. Preserve
parameter conversion, schema collations, reference compatibility options and result
conversion; do not change the oracle as part of switching the driver.

A focused pre-extraction baseline was run at the revision/platform above:

```sh
just compliance --test_sharding_strategy=disabled \
  --test_arg=--gtest_filter=FilebasedTest/ComplianceFilebasedTests.FilebasedTest/array_functions_test \
  --test_output=errors
```

Result: one GoogleTest ran and passed; 173 distinct statement outcomes: 153 pass,
0 fail, 6 known failure, 14 unsupported. The per-statement comparison baseline is
[sql-runtime-separation-baseline.tsv](sql-runtime-separation-baseline.tsv), produced
by `tools/compliancesummary -results`, not copied into SQL expected-value tests.
This subset covers array/NULL/collation behavior and fixture preparation; it is not
a substitute for the full `just compliance` run required when switching the driver.

To reproduce the summary without accidentally reading old 32-shard logs, copy the
unsharded log to a fresh directory in the summary tool's shard layout:

```sh
baseline=$(mktemp -d)
mkdir "$baseline/shard_1_of_1"
cp bazel-testlogs/compliance_test/test.log "$baseline/shard_1_of_1/test.log"
go run ./tools/compliancesummary -results /tmp/sql-runtime-results.tsv "$baseline"
diff -u docs/sql-runtime-separation-baseline.tsv /tmp/sql-runtime-results.tsv
```

Compare every changed outcome, including newly passing known failures. Do not relax
known-error files to make the extraction pass. Broaden to the full suite in step 5.

## Existing client coverage to retain

These are existing coverage locations, not claims that they were all rerun in step 1.
Use `just e2e` for the orchestrated environment (fake GCS and configured connections).
Do not duplicate these queries in C++ tests.

| Capability | Existing coverage |
|---|---|
| GCS acquisition, URI validation and wildcards | `tests/e2e/gcs_client.yml`, `tests/e2e/gcsclient/`, `tests/e2e/goclient/load_test.go` |
| Load jobs, uploads and compressed files | `tests/e2e/load.yml`, `tests/e2e/goclient/{load,json_load,parquet_lists,bignumeric_parquet}_test.go` |
| Extract formats, compression, wildcards and errors | `tests/e2e/extract.yml`, `tests/e2e/goclient/extract_test.go` |
| External-table metadata and reading | `tests/e2e/scenarios/testdata/external_table.api.yaml` and its recorded answers; `geoparquet*.api.yaml` |
| ObjectRef pure forms, fetching, missing connections and errors | `tests/e2e/goclient/testdata/{scalars,errors}/objectref.txt`; fixtures under `tests/e2e/gcs/objectref/` |
| External database queries and provider configuration | `tests/e2e/goclient/testdata/scalars/external_query*.txt`, `tools/e2e/main.go` |
| Jobs, native mutations, scripts and history | `tests/e2e/{query,destination_table,copy,rest_jobs}.yml`, `tests/e2e/goclient/{jobs,time_travel,snapshot_table}_test.go` |

## Dependency assertions and activation

`just sql-runtime-deps-check //:TARGET ...` fails if **any transitive path** from a
root reaches `:emulator` (application facade), `:service_backend`,
`:service_table_provider` (application composition), `:gcs`, `:file_read` (acquisition),
`:extract`, or the Storage SDK. The roots default to `//:compliance_test`. Query
errors also fail the command; they are not interpreted as absence of dependencies.
It deliberately fails on today's compliance target. The compliance assertion remains
opt-in; `just check` now runs the assertion for `//:sql_runtime`.

The shared runtime assertion is active. Apply it also to `//:compliance_test` after step 5. Keep `:backend`,
`:duckdb_table_source`, `:duckdb_catalog` and `:table_comments` as useful diagnostic
roots while extracting; do not permanently assert the whole server is service-free.
In step 6 add the compliance root alongside the runtime on the normal `just check` path.
If load/extract orchestration gains new targets, add those specific targets to the
forbidden set then. This is a narrow architecture check, not a frozen allowlist of
all third-party libraries. Spanner remains separately reported, not silently counted
as Storage or evidence that all cloud SDKs have disappeared.

For the configured build, also inspect actual linking in step 5:

```sh
bazelisk cquery 'deps(//:compliance_test)' --output=label
bazelisk aquery 'mnemonic("CppLink", //:compliance_test)' --output=text
```

Check link inputs/arguments (including parameter files when used) as well as the
target graph for Storage libraries. The current step establishes repeatable graph
checks; it does not claim the existing executable is Storage-free.
