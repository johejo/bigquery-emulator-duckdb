# Using the BigQuery Storage API

See the [README](../README.md#bigquery-storage-api-grpc) for connection options and the generated
[RPC support table](storage.md) for supported forms.

## Writing rows

Use `storage/managedwriter.NewClient` or the generated Storage v1 client with a self-contained
protobuf writer descriptor and serialized rows. Arrow input is unsupported.

The default stream and COMMITTED streams publish immediately. BUFFERED streams publish through
`FlushRows`; PENDING streams publish atomically through `BatchCommitWriteStreams` after
finalization. Explicit streams retain offsets across connections within the process.

## Reading rows

Read sessions materialize a snapshot in memory and expire after six hours. Arrow and Avro
output support scalar types, STRUCTs and arrays, including decimals and temporal types.
A RANGE column is a struct of `start` and `end`, which is null where unbounded, as in BigQuery.
INTERVAL columns, parameterized decimals, and JSON columns in Arrow are unsupported. The [support table](storage.md) lists the request options the emulator rejects.

## Write limitations

Protobuf scalar fields, STRUCTs and repeated fields are supported. GEOGRAPHY fields take strings
as `tabledata.insertAll` does; see [GEOGRAPHY values](compatibility.md#geography-values).
Parameterized STRING and BYTES columns, custom protobuf annotations and INTERVAL input are
unsupported.

Existing streams support adding nullable or repeated columns and relaxing REQUIRED columns to
NULLABLE, including within records. Open connections report changes through `updated_schema`,
and accept a replacement protobuf writer descriptor. Older descriptors can continue writing.
Unpublished BUFFERED and PENDING rows remain publishable after these changes and after column
defaults change; their default values are fixed at append time. Column removal, renaming and type
changes are unsupported on existing streams.

Tables persisted without creation metadata cannot return default-stream creation metadata.

Decimal writer strings must fit NUMERIC's scale of 9 or BIGNUMERIC's scale of 38 exactly.

## Lifetime and persistence

Storage session and stream state, offsets, and unpublished BUFFERED/PENDING rows are
discarded at shutdown, including with `--data-dir`. Published rows persist normally. Storage stream
expiration and production quotas are not emulated.
