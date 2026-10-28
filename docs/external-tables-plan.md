# External table foundation

Scope: prepare for ordinary GCS external tables, not EXTERNAL_QUERY or BigLake.
Keep CREATE EXTERNAL TABLE and REST externalDataConfiguration unsupported until a
complete, client-visible subset can be implemented. Do not materialize external
sources into ordinary tables at creation time.

## 1. Separate reads from write restrictions (implemented)

- Expose a table's optional read query independently from its row-write restriction.
- Keep numeric-storage conversion inside the DuckDB adapter, not in the generic
  catalog interface. Preserve existing numeric-storage and snapshot behavior,
  including errors and explicit replacement with native storage.
- Share row-write restrictions between DML and API write paths. Metadata changes
  and table replacement remain separate operations, not a blanket read-only flag.
- Existing numeric-storage and snapshot client tests cover preserved behavior;
  added numeric-storage DELETE, TRUNCATE and MERGE rejection cases.
- Validation: `just fmt` and `just check` passed.

## 2. Own resources for the duration of statement execution (implemented)

- Resolve schemas during analysis without preparing sources. `TableRead` separates
  fixed read SQL from a self-contained preparation that runs during translation,
  after analysis, with a statement-owned `TemporaryFiles` instance.
- `StatementResources` caches prepared reads by resolved table reference within one
  statement, never across statements or concurrent jobs. Translated SQL retains its
  owner through execution; result-row copies and Storage Read snapshots retain it
  through consumption. Script statements and scalar expressions get separate owners.
- Preparation, translation and execution failures release the staged files when
  their execution scope unwinds. C++ tests inject staged sources to check cleanup,
  repeated reads, concurrent statements and result/snapshot lifetimes without
  accepting external tables through DDL or REST.
- Resource-backed reads in dry runs are explicitly unsupported before preparation
  or DuckDB binding, which can itself access files. Fixed read queries, including
  numeric-storage conversion, keep their existing dry-run behavior. Actual external
  table dry-run semantics still need specification before accepting those tables.
- Translations of stored definitions have no execution resource owner: a view that
  would need staging is rejected before preparation. Definitions stored as original
  GoogleSQL, such as table functions, do not prepare their reads at creation. Neither
  can capture temporary source paths; external-table definitions themselves still
  need a persistence strategy before they can be accepted.
- Validation: `just fmt` and `just check` passed, including C++ tests, end-to-end
  tests and generated support-table checks.

## 3. Share only the file-reading pieces needed by the first implementation (implemented)

- `file_read` shares GCS acquisition, CSV/JSON/Parquet staging, schema inference
  adjustments and type conversion with load jobs. It does not prepare or execute SQL;
  callers own the `TemporaryFiles` used during staging.
- Reader settings are independent of load-job options. Load jobs map validated options
  to reader settings and retain destination handling, schema updates and rounding
  policies in `emulator_jobs.cc`. External-table options remain unimplemented and must
  be validated separately before using these readers.
- Each staged source keeps its concrete original URI after wildcard expansion alongside
  its current local path. JSON numeric rewriting and Parquet metadata rewriting replace
  only the path, preserving source identity for future _FILE_NAME support.
- GCS staging tests cover source identity and caller-owned cleanup, including JSON
  rewriting. Statement-resource tests exercise the shared CSV reader; existing load-job
  end-to-end tests cover preserved format and conversion behavior.
- Validation: `just fmt` and `just check` passed.

## Before accepting external tables

Choose the first format/schema subset and definition persistence strategy. Verify
REST, DDL and INFORMATION_SCHEMA agree. Specify updates, deletion, copy, time
travel, tabledata.list and Storage Read behavior. Acquire official specification
snapshots and put unsettled BigQuery cases in unverified tests; do not infer
expected values from emulator output.
