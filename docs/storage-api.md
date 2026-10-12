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
INTERVAL and RANGE, parameterized decimals, Arrow JSON/GEOGRAPHY, historical snapshots,
sampling, compression and custom serialization options are unsupported.

## Write limitations

Protobuf scalar fields, STRUCTs and repeated fields are supported. Column defaults, parameterized
columns, custom protobuf annotations, JSON/GEOGRAPHY input and schema evolution are unsupported.

Create a new stream after changing a table schema. Default streams established before a schema
change require restarting the process. Tables persisted without creation metadata cannot return
default-stream creation metadata.

Decimal writer strings must fit NUMERIC's scale of 9 or BIGNUMERIC's scale of 38 exactly.

## Lifetime and persistence

Storage session and stream state, offsets, and unpublished BUFFERED/PENDING rows are
discarded at shutdown, including with `--data-dir`. Published rows persist normally. Storage stream
expiration and production quotas are not emulated.
