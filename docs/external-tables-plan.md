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

## 2. Own resources for the duration of statement execution

- Separate definition resolution from source preparation and execution.
- Retain staged files through execution and result consumption; clean up on errors.
- Avoid duplicate preparation within a statement and sharing across concurrent jobs.
- Persist source definitions, never temporary paths. Specify dry-run behavior before
  introducing source access during analysis.

## 3. Share only the file-reading pieces needed by the first implementation

- Reuse GCS acquisition, format staging and type conversion from load jobs.
- Keep external-table options separate from load options and destination handling.
- Preserve original URI identity for future _FILE_NAME support.

## Before accepting external tables

Choose the first format/schema subset and definition persistence strategy. Verify
REST, DDL and INFORMATION_SCHEMA agree. Specify updates, deletion, copy, time
travel, tabledata.list and Storage Read behavior. Acquire official specification
snapshots and put unsettled BigQuery cases in unverified tests; do not infer
expected values from emulator output.
