# SQL runtime and external-service separation plan

Status: step 1 completed; runtime extraction (steps 2–6) remains proposed.
See the [boundary inventory and dependency checks](sql-runtime-separation-boundaries.md)
for dependency paths, responsibility allocation, client coverage and the recorded
compliance baseline.

## Goal

Run GoogleSQL compliance tests through the same SQL execution path as production,
without linking GCS acquisition, load/extract orchestration or the Cloud Storage SDK.
Separate responsibilities rather than substitute a fake `GcsClient` behind an
interface. Production behavior and supported forms must remain unchanged.

The shared runtime includes analysis, translation, execution, scripts, query DDL/DML,
native table/catalog management, result conversion and statement resource ownership.
The application composes that runtime with API/job orchestration and external-service
features. Compliance composes the same runtime without those features.

Names below describe responsibilities, not final class or target names. Keep each
step buildable and independently reviewable; do not create a second SQL executor.

## Current coupling

- `compliance_test` uses `Emulator` for dataset/table preparation and `RunQuery`.
- `Emulator` owns `GcsClient`, configures ObjectRef metadata-fetch callbacks and
  creates a `DuckDbTableSource` with that client.
- `emulator_jobs.cc` mixes job orchestration with GCS-backed load/extract work.
- `DuckDbTableSource` combines native catalog access, GCS external-table reads and
  external database connections. `ReadQuery` calls `GcsExternalRead` directly.
- `table_comments.cc` depends on GCS for source-URI validation via
  `FindGcsWildcard`; moving only the client will leave this dependency behind.
- `Backend` and `DuckDbTableSource` also depend on external database adapters.
  Spanner provides another google-cloud-cpp dependency, distinct from Storage.

Inspect the full Bazel dependency graph before choosing the final target boundaries.
Removing `:gcs` from one direct dependency list is not sufficient.

## Intended dependency direction

```text
compliance driver ---> shared SQL runtime ---> native catalog / Backend
                              ^
                              |
server / shell ---> application composition
                      |       |
                      |       +--> shared SQL runtime
                      +--> load / extract services
                      +--> GCS external-table provider
                      +--> ObjectRef metadata-fetch integration
                      +--> external database providers
                                  |
                                  +--> service SDKs / connectors
```

Runtime extension points express SQL capabilities, such as preparing a table read
or resolving an external query, not a generic wrapper around Storage SDK methods.
Use existing `TableSource`, `TableRead`, `StatementResources`, function catalog and
`BackendInitializer` boundaries where they fit. Add abstractions only for concrete
call sites; avoid a general plugin framework.

## 1. Establish boundaries and dependency checks

- Record all dependency paths from `//:compliance_test` to `//:gcs` and
  `@google_cloud_cpp//:storage`, including metadata-validation paths.
- Inventory driver calls and the state shared by query execution, catalog mutations,
  scripts, jobs and external services. Identify ownership of locks, sessions,
  transactions, project aliases and job locations before moving it.
- Record a representative compliance baseline and the existing client tests for
  load/extract, external tables and ObjectRef. Do not duplicate their SQL in C++ tests.
- Choose narrow Bazel dependency assertions for the intended core target and final
  compliance target. Check transitive dependencies, not generated SQL or rule names.

Completion: an explicit allocation of existing responsibilities and a reproducible
way to detect accidental Storage dependencies. Final dependency assertions become
mandatory once the separation is complete, not while intermediate steps still link GCS.

## 2. Separate pure metadata validation from GCS access

- Move URI parsing/wildcard validation used by metadata handling into a small,
  SDK-independent implementation. Both metadata validation and GCS acquisition use it.
- Preserve validation rules and client-visible errors. Do not introduce a header-only
  target to hide a dependency cycle.
- Audit nearby metadata and schema helpers for other accidental service dependencies;
  keep unrelated cleanup out of this change.

Completion: native metadata/catalog helpers no longer depend on `:gcs` merely to
validate or store external-table metadata. Existing client tests remain unchanged.

## 3. Separate native table resolution from external providers

- Keep native schema/metadata lookup, project aliases, location tracking, time travel,
  numeric-storage conversion and write restrictions in the native table source.
- Move GCS external-read preparation and external connection discovery/registration
  to application-composed providers. The native implementation must not construct
  service clients or call concrete service adapters.
- Preserve a single catalog/analyzer path: providers supply capabilities to it rather
  than reimplementing catalog behavior in a compliance-specific table source.
- Keep analysis free of file staging. Deferred preparations retain their provider
  state and resources through execution and result/snapshot consumption, as today.
- Missing providers must reject attempted external operations explicitly. Never fall
  back to reading an external table's native placeholder relation.
- Review external connector setup in `Backend` as well as `DuckDbTableSource`.
  Move application-specific registration outward where needed to keep the runtime
  boundary honest; distinguish this work from unrelated connector refactoring.

Completion: the native table source can be constructed without GCS or connection
adapters; production composition still supports all currently supported external reads.

## 4. Extract the shared SQL runtime and move application orchestration outward

- Extract runtime ownership and the existing query execution path from `Emulator`.
  Include native dataset/table setup needed by the driver, rather than making the
  driver insert directly into DuckDB or bypass catalog mutation rules.
- Keep query analysis, translation, scripts, DDL/DML, results and errors shared.
  Preserve query options, compatibility options, session behavior, expiration,
  transaction boundaries and location checks. Assign each piece of mutable state a
  single owner rather than copying it into the application facade.
- Keep REST job bookkeeping and load/extract orchestration in the application layer.
  The application delegates query execution to the shared runtime and uses its native
  catalog operations for loads and extracts, without reverse runtime-to-job dependencies.
- Move GCS-backed ObjectRef fetch composition out of the runtime constructor. Keep
  service-independent ObjectRef validation/operations available. Align function
  availability and backend registration for the service-dependent forms.
- Retain `Emulator` as an application facade where useful. Do not force transports
  to adopt runtime internals, and do not move service construction into runtime defaults.

Completion: the runtime target has no dependency on GCS, load/extract orchestration
or application composition. Server and shell delegate SQL to this shared target.
Steps 3 and 4 may be split into smaller patches where existing ownership requires it.

## 5. Switch compliance to the shared runtime

- Prepare datasets/tables and execute queries through the shared public runtime API.
  Preserve parameter conversion, GoogleSQL reference compatibility options and result
  decoding; this is not a change to the compliance oracle.
- Do not link the application facade or install GCS/external-service providers.
  Do not provide a fake Storage implementation simply to satisfy construction.
- Compare results against the baseline. Investigate every newly passing or failing
  known-error entry; do not mask regressions by broadening known errors or filters.
- Enforce absence of dependency paths from compliance to `:gcs`, the Storage SDK and
  load/extract orchestration. Confirm actual link inputs as well as target dependencies.
  If another SDK remains, identify its path separately rather than claiming all
  google-cloud-cpp dependencies have disappeared.

Completion: compliance and production use the same runtime implementation, while
compliance has no Storage dependency and production retains external-service behavior.

## 6. Validate and document the resulting architecture

- Run `just fmt` and `just check` before committing, plus `just compliance` for the
  driver/runtime change. Use focused existing tests during intermediate steps.
- Preserve end-to-end coverage of query jobs, scripts, query DDL/DML, native catalog
  APIs, load/extract, GCS external tables, ObjectRef and external database queries.
  Add cases only for uncovered client-visible behavior, using independent expected
  values; unsettled BigQuery cases belong under `testdata/unverified/`.
- Use C++ tests for ownership, injected preparation failures and concurrent lifetimes
  that cannot be checked reliably end to end, not for which provider was selected.
- Add a dependency-architecture check to the normal check path, so future metadata
  helpers or default constructors cannot silently reintroduce Storage into compliance.
- Update `architecture.md` to describe the implemented ownership and composition.
  Regenerate support tables only if their inputs change; this refactor should not
  change advertised support or add compatibility exceptions.

## Non-goals and risks

- No new external-table formats, API fields or SQL features.
- No compliance-only preprocessor branches, alternate definitions of the same class,
  linker-selected stubs, or duplicate implementations of query execution.
- No promise to eliminate every SDK in one patch. Storage is the first measurable
  goal; other concrete adapters must still stay outside the native runtime boundary.
- The main risks are state/lock ownership during extraction, scripts using a different
  backend connection, deferred reads outliving their provider and accidentally making
  production and compliance register different service-independent SQL semantics.
- Provider absence is a composition choice for compliance, not permission for the
  production emulator to accept and ignore supported external operations.
