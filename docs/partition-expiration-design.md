# Partition expiration foundation design

This document is a design proposal, not a description of supported behavior.

## Scope

Start with time-unit column partitioning on native tables: DATE, DATETIME and
TIMESTAMP columns, using the type and unit combinations already accepted by the
emulator. Support table-level expiration and dataset defaults together.

Ingestion-time partitioning remains unsupported. Integer-range partitioning and
external tables are not targets for partition expiration. Copy, clone and snapshot
creation need their own inheritance rules rather than the ordinary table-creation
path. In particular, the [snapshot documentation](../third_party/bigquery_docs/table-snapshots-intro.md)
describes different partition expiration inheritance behavior.

Keep the existing flat DuckDB tables. Physical partitioning is not required to
implement partition expiration.

## Existing components

- [`table_metadata.h`](../src/table_metadata.h) records a time partition's column
  and unit, table expiration and dataset default table expiration.
- [`duckdb_catalog.cc`](../src/duckdb_catalog.cc) lazily drops expired relations.
  `ExpireTables()` rechecks their comments within the drop transaction to protect
  concurrent expiration updates and table replacement.
- [`table_history.cc`](../src/table_history.cc) preserves rows before writes and
  dates versions at commit. It does not distinguish a logical expiration time
  from the commit time of delayed cleanup.

Adding a `DELETE` to the existing table-expiration sweep is therefore insufficient:
partition expiration also needs row-history and concurrent-write coordination.

## Metadata and creation-time policy resolution

Store durations in milliseconds, matching REST:

```cpp
// TimePartitioning
std::optional<int64_t> expiration_ms;

// DatasetMetadata
std::optional<int64_t> default_partition_expiration_ms;
```

Keep partition expiration inside `TimePartitioning`, without duplicating it in
`TableMetadata`.

Request interpretation must distinguish omission, an explicit value and explicit
removal wherever the input interface supports those forms. A stored optional value
alone cannot represent both an omitted setting that inherits a dataset default and
an explicit request for no expiration. Resolve that distinction before persisting
the table's effective metadata. Confirm the exact clearing forms separately for
DDL and REST.

Resolve default table expiration and default partition expiration together rather
than applying `ExpirationWithDefault()` independently. The
[dataset documentation](../third_party/bigquery_docs/updating-datasets.md#update-default-partition-expiration-times)
specifies that default partition expiration takes precedence over default table
expiration for newly created partitioned tables when both dataset defaults exist.
Later dataset-default changes do not update existing tables.

Define a resolution matrix covering explicit table settings, explicit removal,
dataset defaults and table-creation variants before implementing this path.

## Shared partition semantics

Introduce a small `partitioning.h/.cc` component responsible for:

- Validating partition column types and units.
- Computing the UTC boundary of the partition containing a value.
- Distinguishing ordinary partitions from special partitions.
- Building an expiration predicate for an explicitly supplied evaluation time.

The ordinary-partition rule is:

```text
partition_start + expiration_ms <= evaluation_time
```

Implement the comparison without overflow at supported date and duration limits.
TIMESTAMP boundaries must not depend on the session timezone; DATETIME values must
not undergo timezone conversion.

Do not treat NULL or out-of-range values as ordinary dated partitions. The
[partitioning documentation](../third_party/bigquery_docs/partitioned-tables.md)
describes `__NULL__` and `__UNPARTITIONED__`; their expiration behavior must be
settled independently rather than inferred from a convenient SQL predicate.
Unsettled cases belong in the end-to-end unverified inputs.

Pass evaluation time into this component instead of reading the clock internally.
This gives each operation a consistent cutoff and allows deterministic boundary
tests.

Do not add a persistent partition catalog solely for expiration. The existing
partition column and table metadata are sufficient to identify ordinary expired
rows. Revisit that choice only if independently required partition state cannot be
derived from them.

## Runtime-owned expiration mutations

Partition cleanup is a row mutation. Its orchestration belongs in the runtime,
where history and write coordination are available, rather than in a catalog
helper that only knows how to drop relations.

The expiration entry point should distinguish:

```text
Table expiration     -> drop the relation
Partition expiration -> revalidate, preserve history, delete expired rows
```

A partition cleanup transaction must:

1. Revalidate the table's identity and current expiration policy.
2. Check whether any rows need deletion.
3. Preserve the history required by the selected temporal model.
4. Delete expired rows.
5. Finalize history timestamps consistently with that model.

Do not copy the whole table into history on every read when no rows expire. A
concurrent policy extension, removal or table replacement must not leave cleanup
acting on stale metadata.

Define a single lock order across expiration, metadata changes and ordinary
writes. In particular, do not acquire the existing expiration and history mutexes
in opposite orders on different paths. The implementation must also account for
script transactions and use the backend that sees their uncommitted changes.

## Time travel: decision required before cleanup implementation

Consider:

```text
Partition expiration deadline: 10:00
Next access and cleanup:       12:00
Historical read:               AS OF 11:00
```

Reusing the current write-history mechanism would record the rows as present until
the cleanup commit at 12:00. Establish whether that is the correct observable
behavior before adopting it. Do not silently equate a partition's logical deadline
with the time of delayed physical deletion.

The [time-travel documentation](../third_party/bigquery_docs/time-travel.md)
specifies retention of data from expired partitions. Filtering all historical
reads using the current expiration policy would lose that behavior.

Two possible foundations need evaluation:

- **Logical validity in history.** Represent the effective removal time in the
  history model. A single cleanup may encounter multiple partitions with different
  deadlines, which a single table-wide version timestamp cannot directly express.
- **Temporal policy evaluation.** Read historical rows under the policy effective
  at the requested time. This requires policy history, including changes and
  removal, rather than only the current table comment.

Neither approach is selected yet. Clarify the observable behavior using saved
specifications and independently verified cases. Put cases without a settled
answer in `tests/e2e/goclient/testdata/unverified/`; reference implementation output
is not an expected BigQuery answer.

## Operation boundaries

A filter attached only to SQL scans is insufficient. Expiration can affect rows
and metadata exposed by other interfaces. Map these paths to the shared lifecycle
entry point:

- SQL execution, including DML.
- `tabledata.list`.
- Storage Read session creation.
- Row counts returned by `tables.get`.
- Copy and extract jobs.
- Load, streaming inserts and Storage Write publication.
- Changes to expiration policy.

For each path, identify its backend, transaction, evaluation cutoff and lock
ownership. Cover both writes into already-expired partitions and policy changes
that make existing partitions expire. Existing read snapshots and transaction
isolation need explicit treatment; cleanup must not accidentally redefine their
visibility through a wall-clock filter.

## Design decisions to settle first

Before enabling an accepted DDL option or API field, complete:

1. **Policy resolution matrix:** omission, explicit settings, removal, dataset
   defaults and interaction with table expiration.
2. **Lifecycle boundary matrix:** where every SQL/API path evaluates expiration,
   on which backend and in which transaction.
3. **Temporal model:** how logical deadlines, delayed cleanup and historical policy
   changes affect time travel.

Prioritize the temporal model. Metadata additions are small, but choosing cleanup
semantics first could require rewriting both deletion and history handling.

Until behavior is implemented for an input form, keep rejecting it as unsupported
rather than accepting and storing a setting that does not expire data.
