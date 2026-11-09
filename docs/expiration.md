# Expiration and history

This page describes coordination in the [runtime](architecture.md). Client-visible limits are
in [Options and expiration](compatibility.md#options-and-expiration),
[Partition expiration](compatibility.md#partition-expiration) and
[Time travel](compatibility.md#time-travel). It describes the current implementation, not a
plan for extending unsupported operations.

## Coordination and identity

Metadata mutations and response reads share the
[expiration mutex](../src/expiration_mutex.h). DML only keeps expired-relation drops out while
it runs, allowing concurrent DML. Expiration drops recheck metadata inside their transaction.
Catalog and row-write locks use non-recursive Abseil mutexes. SQL and script entry points
acquire statement coordination before opening a transaction snapshot; mutations pass held
catalog locks to nested reads and sweeps. When both expiration and table history coordination
are required, expiration precedes history.

[table_history.cc](../src/table_history.cc) retains rows before each write for time travel in
project `main` tables keyed by dataset, table and creation time. Writes outside a transaction
hold a mutex so each version reflects the previous committed write. A transaction dates its
versions when it commits. Partition policy history instead uses storage OIDs within a project:
creation timestamps have only millisecond precision and are not sufficient replacement tokens.

## Creation policy

[expiration_policy.cc](../src/expiration_policy.cc) resolves ordinary native table creation
without reading the clock or mutating metadata. Table and partition defaults are resolved
together: a dataset partition default suppresses its table default only for time-partitioned
tables; explicit table deadlines survive. Request layers distinguish omission from removal
before resolution. Copy, clone, snapshot, LIKE and replacement use separate inheritance paths.

The creation/removal combinations and independently observed expectations live in
[policy scenarios](../tests/e2e/scenarios/testdata/partition_expiration_policy.api.yaml) and
[DDL scenarios](../tests/e2e/scenarios/testdata/partition_expiration_ddl.api.yaml), with adjacent
BigQuery answer sidecars. They are the source for edge-case expectations, rather than a second
matrix maintained in this document.

## Partition visibility

Partition expiration retains raw rows and applies temporal policy filtering instead of deleting
rows. Removing or extending a policy can expose them again; ordinary write history alone cannot
reproduce that. Policy-only changes do not copy whole tables, and repeated reads create no
history. [History scenarios](../tests/e2e/scenarios/testdata/partition_expiration_history.api.yaml)
and [DML scenarios](../tests/e2e/scenarios/testdata/partition_expiration_dml.api.yaml) record the
verified workflows. They do not establish natural deadline passage without a policy change.
The runtime rejects those unsettled reads as described in the compatibility guide.

[partition_policy_history.cc](../src/partition_policy_history.cc) stages the transaction's final
duration or removal and publishes it at a caller-supplied commit cutoff, atomically with table
metadata. Historical lookup selects the policy effective at the requested microsecond.
[partitioning.h](../src/partitioning.h) supplies UTC boundaries and an expiration predicate
for an explicit evaluation time, excluding special NULL and out-of-range partitions.

[partition_policy_read.cc](../src/partition_policy_read.cc) composes raw row history, policy
filtering and execution-time storage identity validation. Discovery, translation and execution
must use the same active backend transaction: otherwise a concurrent write could create the
first historical row version after resolution, leaving a historical query reading current rows.
A duration fetched separately from execution could likewise mix old policy and new rows.

Ordinary queries, each script statement outside an explicit transaction, DML target selection,
`tabledata.list` and row counts use one evaluation cutoff and snapshot. Time travel resolves
raw row history before applying the historical policy. `tabledata.list` counts unpaginated rows
in that same snapshot. Policy history rejects reads of its own staged policy rather than
defining unverified explicit-transaction visibility. Other unsupported entry points remain
rejected rather than bypassing policy filtering.
