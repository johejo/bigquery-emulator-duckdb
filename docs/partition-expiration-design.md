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
[dataset documentation](../third_party/bigquery_docs/updating-datasets.md)
specifies that default partition expiration takes precedence over default table
expiration for newly created partitioned tables when both dataset defaults exist.
Later dataset-default changes do not update existing tables.

### Creation policy resolution matrix

Here, `T` is an explicitly supplied table deadline, `P` an explicitly supplied
partition duration, `DT` the dataset default table duration and `DP` the dataset
default partition duration. A table deadline inherited from `DT` is its creation
time plus `DT`; a partition duration is not relative to table creation.

The [dataset resource specification](../third_party/bigquery_docs/reference/rest/v2/datasets.md)
and the dataset documentation above establish these ordinary creation cases:

| New native table | Table expiration input | Partition expiration input | Dataset defaults | Effective table deadline | Effective partition duration |
| --- | --- | --- | --- | --- | --- |
| Not partitioned | omitted | not applicable | `DT`, with or without `DP` | creation time + `DT` | none |
| Time-partitioned | omitted | omitted | neither | none | none |
| Time-partitioned | omitted | omitted | `DT` only | creation time + `DT` | none |
| Time-partitioned | omitted | omitted | `DP` only | none | `DP` |
| Time-partitioned | omitted | omitted | `DT` and `DP` | none | `DP` |
| Time-partitioned | omitted | `P` | `DP`, with or without `DT` | none | `P` |

The recorded BigQuery answers settle the remaining tested combinations; default
suppression depends on the **dataset** partition default, not just the effective
partition duration:

| Input combination | Observed result | Answer case |
| --- | --- | --- |
| Explicit `T` with `DP` | Keep `T` and inherit `DP` | REST `create_explicit_table_with_partition_default` |
| Explicit `P` with `DT`, without `DP` | Keep `P` and inherit creation time + `DT` | REST `create_explicit_partition_with_table_default` |
| Null duration on creation with both defaults | Inherit `DP`, without a table deadline, in both REST and DDL | REST `create_null_with_both_defaults`; DDL `create_null` |
| Zero duration on creation | Reject; not an inheritance or removal spelling | REST `create_zero_with_both_defaults`; DDL `create_zero` |
| Omitted nested duration on PATCH | Retain the stored duration | REST `patch_omits_duration` |
| Null nested duration on PATCH | Remove expiration without reapplying `DP` | REST `patch_null_duration` |
| Null duration in ALTER TABLE | Remove expiration without reapplying `DP` | DDL `alter_null` |
| Zero duration on PATCH or ALTER TABLE | Reject; duration must be positive | REST `patch_zero_duration`; DDL `alter_zero` |
| Null whole `timePartitioning` on PATCH | Reject conversion to a non-partitioned table | REST `patch_null_partitioning` |

The cases are in
[`partition_expiration_policy.api.yaml`](../tests/e2e/scenarios/testdata/known_bugs/partition_expiration_policy.api.yaml)
and
[`partition_expiration_ddl.api.yaml`](../tests/e2e/scenarios/testdata/known_bugs/partition_expiration_ddl.api.yaml).
Their adjacent answer sidecars preserve the BigQuery observations, including
the creation and expiration timestamps that establish the inherited table
lifetime. Null is an explicit JSON null, not a stand-in for omission. Record the
interpretation in the request layer before resolving effective metadata; do not
add a persisted tri-state policy merely to remember how a table was created.

`CREATE TABLE LIKE`, `CREATE TABLE COPY`, replacement, job destinations, clone and
snapshot creation are separate inheritance paths. This matrix does not authorize
using ordinary defaults for them. Existing tables retain their effective policy
when a dataset default changes or is removed; `change_dataset_default` observes
both existing and newly created tables through INFORMATION_SCHEMA.

## Shared partition semantics

The shared `partitioning.h/.cc` component provides:

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
describes `__NULL__` and `__UNPARTITIONED__`. The recorded DATE case
`special_partitions_then_remove` shows that NULL and out-of-range values remain
visible while expired ordinary partitions are hidden. The helper excludes these
special values from its expiration predicate. The case is evidence for this
policy-change workflow, not for every retention duration or input type.

Pass evaluation time into this component instead of reading the clock internally.
This gives each operation a consistent cutoff and allows deterministic boundary
tests.

Do not add a persistent partition catalog solely for expiration. The existing
partition column and table metadata are sufficient to identify ordinary expired
rows. Revisit that choice only if independently required partition state cannot be
derived from them.

## Runtime-owned policy visibility and eventual cleanup

Partition expiration cannot initially be implemented as destructive cleanup. In
the recorded policy-change cases, removing or extending expiration makes the
previously hidden rows visible again, including a row inserted into an
already-expired partition. Deleting those rows from the current table and keeping
them only in ordinary write history cannot reproduce that result.

Use temporal policy evaluation as the foundation for the observed workflows:

```text
Table expiration     -> drop the relation
Partition expiration -> select visible rows under the policy at the read time
Policy change        -> retain old policy and atomically publish the new policy
```

Keep row-write history separate from policy history. Current reads evaluate the
current policy; historical reads obtain the row version and policy effective at
the requested timestamp. A policy-only change must not copy an entire table.
Repeated reads with no writes or policy changes must not create history records.

Keep physically retained rows available for policy removal/extension and for time
travel. Do not introduce a purge until independently verified retention behavior
settles when these rows can no longer become visible. Any eventual cleanup is a
row mutation belonging in the runtime, where history and write coordination are
available, not in the catalog's relation-drop helper. It must revalidate identity
and policy within the deletion transaction and preserve all still-readable row
and policy versions.

Define a single lock order across expiration, metadata changes and ordinary
writes. In particular, do not acquire the existing expiration and history mutexes
in opposite orders on different paths. The implementation must also account for
script transactions and use the backend that sees their uncommitted changes.

## Time travel: policy evaluation and remaining boundaries

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

The recorded policy-change workflows require temporal policy evaluation rather
than irreversible logical removal at the first observed expiration. Policy
history must include changes and removal, rather than only the current table
comment. This choice does not yet settle natural deadline passage or eventual
physical retention.

The saved time-travel specification establishes retention and restoration from
before expiration; it does not establish that a lazy emulator may date
expiration at its eventual cleanup commit. In particular,
keeping a whole-table version with `valid_to = cleanup_commit` and treating it as
the historical answer would make the 11:00 read above expose expired rows without
independent evidence that BigQuery does so.

[`partition_expiration_history.api.yaml`](../tests/e2e/scenarios/testdata/known_bugs/partition_expiration_history.api.yaml)
records verified expectations from its adjacent BigQuery answer sidecar:

- `shorten_then_remove` and `shorten_then_extend` read snapshots before and after a
  policy change whose new deadline is already past, then read current rows after
  removing or extending that policy. The original row is visible before the
  change, hidden at the later timestamp and in the current read while expiration
  is active, then visible again after removal or extension.
- `insert_into_expired_partition_then_remove` shows the inserted row hidden while
  expiration is active and at the historical timestamp after insertion, but
  visible in the current read after policy removal.
- `special_partitions_then_remove` observes NULL, both ends of the ordinary date
  range and values outside that range, before and after the policy change. CTAS
  avoids conflating the pre-1970 DML restriction with special-partition behavior.

These cases do **not** answer natural deadline passage with no policy change, nor
visibility inside an existing transaction or Storage Read snapshot. A natural
expiration case must establish that rows were present before a future deadline,
avoid reads until after that deadline, then read historical timestamps on both
sides of it. A shortening-policy case alone is not evidence for that timeline.
Do not infer the missing answers from GoogleSQL's reference implementation or
from the time it took an API request to finish.

Policy history must be dated at the policy change as well as at row writes:
shortening a duration at 12:00 must not simply rewrite the policy used at 11:00.
Likewise, retained raw rows and visible rows are different inputs to DML: do not
let an UPDATE or DELETE operate on rows a client cannot see without evidence of
BigQuery doing so. That interaction needs independently answered cases before
policy filtering can be enabled for DML.

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

### Lifecycle boundary matrix

The following maps existing entry points, not implemented partition cleanup. In
each row, the future lifecycle operation needs one explicit evaluation cutoff;
metadata discovery and subsequent execution must not independently read the
clock and accidentally use different policies or table identities.

| Path | Current backend and transaction boundary | Required expiration integration |
| --- | --- | --- |
| SQL analysis and ordinary SELECT | `NewTableSource` uses the supplied session, otherwise `backend_`; currently sweeps table expiration before analysis | Cleanup and scan must agree on identity and cutoff, including wildcards and INFORMATION_SCHEMA. Dry-run analysis must not add a row mutation. |
| SQL DML | `RunStatement` uses the script session or `backend_`; `KeepVersion` uses the caller's transaction when present | Evaluate both existing rows and newly written expired rows, with history and DML under the same write coordination. |
| Script transaction | The session backend sees uncommitted writes; `KeepVersion` registers commit finalization there | Never clean through `backend_` in place of the session. Define visibility at statement boundaries and commit, and preserve rollback behavior. |
| `tabledata.list` | `GetTable` followed by a separate SELECT on `backend_` | A cleanup in `GetTable` alone leaves a race before SELECT. Coordinate the lifecycle operation with the actual read. |
| `tables.get` row count | `GetTable` sweeps then reads metadata and count on `backend_` | Count must reflect expiration under the same identity and cutoff; do not copy history when no rows expire. |
| Storage Read creation | `ReadStorageTable` gets metadata, translates, materializes with `ExecuteStorageRead`, then rechecks schema and OID | Apply expiration before materialization without changing an already created snapshot. A schema/OID recheck alone cannot detect policy changes. |
| Query/load/copy destination publication | `WriteDestination` uses `GetTable` and executes a transaction containing destination writes; append history is protected by `HistoryMutex` | Resolve creation versus inheritance separately; expire appended rows in the publication transaction rather than only during initial metadata lookup. |
| Copy/extract source | Job orchestration resolves source metadata and then reads source data | Apply source expiration to the source read, not merely destination creation or source metadata lookup. |
| `tabledata.insertAll` | `InsertTableData` gets metadata and keeps history before executing inserts on `backend_` | Coordinate policy revalidation, insertion, expiration and history; preserve partial-row error behavior. |
| Storage Write publication | `WriteStorageRows` opens a new session transaction and checks OID/schema there; publication takes `HistoryMutex` | Read policy and expire rows in that transaction. Validation-only appends roll back and must not publish expiration effects. |
| DDL/REST policy changes | DDL alteration uses `ExecuteDdl`; REST `UpdateTable` holds `expiration_mutex_` | Policy change, resulting cleanup and temporal history must agree atomically. Dataset-default changes affect future creation only. |

### Lock and identity constraints

The existing expiration mutex is recursive; history has one non-recursive mutex
per table. Some creation paths already hold the expiration mutex before entering
code that can take a history mutex. Any combined lifecycle implementation must
use **expiration mutex before table history mutex**, never the reverse. Calls to
`GetTable` or `NewTableSource` from inside a history-locked operation must either
avoid expiration explicitly or be moved before lock acquisition. Merely replacing
all `ExpireTables` calls with a history-writing sweep is unsafe.

`HistoryMutex()` currently also calls `SweepHistory(backend_, project)`. It is not
a pure lock lookup, even when the caller has a different session. Account for that
catalog mutation before using it inside a lifecycle transaction. Transactions
that already hold a DuckDB snapshot while waiting for a mutex also need conflict
handling or retry; acquiring the mutex does not refresh their snapshot.

Revalidation must compare the table's storage OID and policy, not just its name.
A creation timestamp is a history namespace, not a sufficient replacement token:
it has millisecond precision. The existing table-drop sweep rechecks comments;
row cleanup needs its own transactional identity and policy checks before keeping
history or deleting anything. Never commit a pre-cleanup version separately and
then delete from a potentially different relation.

Existing read snapshots and transaction isolation need explicit treatment;
cleanup must not accidentally redefine their visibility through a wall-clock
filter.

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
