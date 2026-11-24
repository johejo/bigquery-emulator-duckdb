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

Protobuf rows with a `_CHANGE_TYPE` field on the default stream apply
[change data capture](../third_party/bigquery_docs/change-data-capture.md) to a table with a
primary key. The emulator applies each change when it appends it, so queries always see every
change, as BigQuery's do without `max_staleness`; that table option remains unsupported, and
`upsert_stream_apply_watermark` is always null. An `UPSERT` replaces the key's row, and
`_CHANGE_SEQUENCE_NUMBER` values order changes to the same key. Sequence numbers persist with
`--data-dir`, and those of a deleted key last two days.

These forms are unsupported until BigQuery's behavior is verified:

- `_CHANGE_TYPE` values other than `UPSERT` and `DELETE`, including `INSERT`, a missing value
  and other letter cases, and connections that mix change rows with other rows.
- Rows without a primary key value, and changes to a key that several rows share.
- Comparing sequence numbers that differ only in trailing zero sections, such as `A` and `A/0`,
  and empty sequence numbers.
- Collated primary key columns, and primary keys of more than 16 columns.

## Lifetime and persistence

Storage session and stream state, offsets, and unpublished BUFFERED/PENDING rows are
discarded at shutdown, including with `--data-dir`. Published rows persist normally. Storage stream
expiration and production quotas are not emulated.
