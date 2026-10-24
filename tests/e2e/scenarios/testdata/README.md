# API compatibility scenarios

Put verified `*.api.yaml` (or `*.api.yml`) cases here. Each case requires case-level or step-level `expect` values. Case expectations map
step names to the objects selected by `observe`; step expectations compare that step's selection.
See [the scenario format](../../../../tools/bqanswers/README.md).

`just e2e` runs cases in parallel. Use `${run}` and `${case}` in dataset names and
provide cleanup. A test that lists a whole project needs an isolated project;
keep such tests in Go until the scenario has a suitable isolated fixture.

Keep SQL returning one cell in `goclient/testdata/scalars`. Keep SDK type conversion,
iterators, managedwriter, binary decoding, concurrency and process lifecycle tests
in Go. Expected values must have an independent source, as required by AGENTS.md.
Do not copy the emulator's output into `expect` or automatically promote every
collected answer into an assertion. Deliberate emulator limitations can be tested
here, but must not be collected as expected BigQuery behavior.

Migrated cases include:

- `ddl_statistics.api.yaml`: statement types and DDL target tables, with independent
  fixtures for each statement and a shared execution template.
- `label_order.api.yaml`: label ordering after REST patches, preserving the recorded
  BigQuery expectations and the sequence of updates.

- `table_metadata.api.yaml` and `dataset_metadata.api.yaml` replace the table,
  partitioning and dataset metadata lifecycle tests, including creation-time agreement
  with INFORMATION_SCHEMA and rejected operations. Optional scalar expectations list
  the representations the Go SDK previously decoded to the same value.

- `table_update.api.yaml` replaces schema and view updates, including nested field
  additions, REQUIRED relaxation, existing rows, all six rejected schema changes,
  and preservation of the view definition after a rejected replacement. `$schema`
  compares the same schema properties the Go SDK previously exposed.

- `jobs.api.yaml` replaces creation/default semantics, destination metadata and
  failures, and cancellation/deletion/ID reuse. Go retains timestamp decoding and
  clock bounds, Query.Read with a destination, and SDK pagination.
- `create_table_like.api.yaml` and `create_table_copy.api.yaml` replace metadata,
  constraints, defaults, row independence, clone definitions and rejected
  replacements. Go retains typed DATE/NUMERIC/array/record reads and clone-time
  decoding against local-clock bounds.

- `rounding_modes.api.yaml` replaces all ten rounding workflow tests: column/DML
  modes, query/load/streaming writes, dataset and table inheritance, LIKE/COPY/CLONE,
  copy jobs, unparameterized values, PATCH/PUT, create/truncate load combinations,
  nested numeric/string input and overflow atomicity. Go retains PutMultiError decoding.
- `table_creation_paths.api.yaml` replaces all three creation/write/replacement tests.
  Every metadata snapshot still checks schema, creation time, columns and field paths
  against INFORMATION_SCHEMA; failed replacements also preserve rows and defaults.
  Shared Go helpers remain for the other SDK tests that use them.

Query jobs are polled to completion; job errors are included in observations so an
HTTP-success response cannot hide a failed query. New cases should likewise observe
job completion and errors rather than assume that submitting a job finishes it.
