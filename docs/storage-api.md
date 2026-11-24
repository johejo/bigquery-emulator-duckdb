# Using the BigQuery Storage API

See the [README](../README.md#bigquery-storage-api-grpc) for connection options and the generated
[RPC support table](storage.md) for supported forms.

## Writing rows

Use `storage/managedwriter.NewClient` or the generated Storage v1 client with a self-contained
protobuf writer descriptor and serialized rows, or an Arrow IPC writer schema and record batch.
Arrow writes accept uncompressed batches with scalar, list, STRUCT, and RANGE fields. See
[Write limitations](#write-limitations) for the differences from BigQuery.

The default stream and COMMITTED streams publish immediately. BUFFERED streams publish through
`FlushRows`; PENDING streams publish atomically through `BatchCommitWriteStreams` after
finalization. Explicit streams retain offsets across connections within the process.

## Reading rows

Read sessions materialize a snapshot in memory and expire after six hours. Arrow and Avro
output support scalar types, STRUCTs and arrays, including decimals and temporal types.
A RANGE column is a struct of `start` and `end`, which is null where unbounded, as in BigQuery.
If `snapshot_time` is set, the read uses the same history and has the same limits as
[`FOR SYSTEM_TIME AS OF`](compatibility.md#time-travel).

Storage Read rounds INTERVAL time parts down to whole microseconds. Arrow uses `MONTH_DAY_NANO`
and returns `OUT_OF_RANGE` if the time part does not fit in signed 64-bit nanoseconds. Avro uses
a `google.sqlType.INTERVAL` record of months, days, and microseconds. The [support table](storage.md)
lists unsupported request options.

## Write limitations

Protobuf scalar fields, STRUCTs and repeated fields are supported. GEOGRAPHY fields take strings
as `tabledata.insertAll` does; see [GEOGRAPHY values](compatibility.md#geography-values).
Protobuf accepts INTERVAL as a string in canonical `Y-M D H:M:S[.F]` format, with up to six
fractional digits; `google.protobuf.Duration` and custom annotations are unsupported.

As in BigQuery, protobuf rejects an entire batch if a value exceeds its `STRING(L)` or
`BYTES(L)` limit. Arrow writes preserve over-limit values without truncating them.

Arrow accepts the documented [Storage Write Arrow mappings](../third_party/bigquery_docs/supported-data-types.md),
as well as signed `Int64` for INT64 and `Time64(MICROSECOND)` for TIME. The
`MONTH_DAY_NANO` encoding preserves INTERVAL values to nanosecond precision. Compressed
batches, dictionary encoding, field metadata, and missing-value interpretation options are
unsupported.

Existing streams support adding nullable or repeated columns and relaxing REQUIRED columns to
NULLABLE, including within records. Open connections report changes through `updated_schema`,
and accept a replacement writer schema. Older writer schemas can continue writing.
Unpublished BUFFERED and PENDING rows remain publishable after these changes and after column
defaults change; their default values are fixed at append time. Column removal, renaming and type
changes are unsupported on existing streams.

Tables persisted without creation metadata cannot return default-stream creation metadata.

Decimal input must fit NUMERIC's scale of 9 or BIGNUMERIC's scale of 38 exactly.

## Change data capture

Protobuf rows with a `_CHANGE_TYPE` field apply
[change data capture](../third_party/bigquery_docs/change-data-capture.md). On the default
stream, the first row of a connection decides whether it writes inserts, as rows with `INSERT` or
no `_CHANGE_TYPE` do, or `UPSERT` and `DELETE` rows, which need a table with a primary key;
explicit streams write only inserts. The emulator applies each change when it appends it, so
queries always see every change, as BigQuery's do without `max_staleness`; that table option
remains unsupported. An `UPSERT` gives every row with its key its values, or inserts a row when
none has it, and `_CHANGE_SEQUENCE_NUMBER` values order changes to the same key. Sequence numbers
persist with `--data-dir`, and those of a deleted key last two days.

A table that `UPSERT` and `DELETE` rows changed reports when they last did as its
`upsert_stream_apply_watermark`, and rejects `UPDATE`, `DELETE` and `MERGE` statements; `INSERT`
and `TRUNCATE TABLE` still run. BigQuery accepts the others again once it has applied every change
and no upsert stream is active, which the emulator cannot tell, so it rejects them for as long as
the table exists.

These forms are unsupported until BigQuery's behavior is verified:

- `_CHANGE_TYPE` values that are not `INSERT`, `UPSERT` or `DELETE` after an `INSERT` first row,
  writer schemas with `_CHANGE_TYPE` on a table without a primary key unless the first row is
  an `INSERT` on the default stream, and invalid sequence numbers on `INSERT` rows.
- For a `NULL` primary key value: `UPSERT` rows when DML inserted a row with it, `DELETE` rows
  after an `UPSERT` row inserted one, and sequence numbers on later changes to it.
- `TRUNCATE TABLE` on a table that changes with sequence numbers or `NULL` primary key values
  changed.
- Collated primary key columns, and primary keys of more than 16 columns.

## Lifetime and persistence

Storage session and stream state, offsets, and unpublished BUFFERED/PENDING rows are
discarded at shutdown, including with `--data-dir`. Published rows persist normally. Storage stream
expiration and production quotas are not emulated.
