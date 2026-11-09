# Expiration and history

These invariants coordinate metadata, row history and partition visibility. Client-visible
limits belong in [Options and expiration](compatibility.md#options-and-expiration),
[Partition expiration](compatibility.md#partition-expiration) and
[Time travel](compatibility.md#time-travel).

## Coordination and identity

The [expiration mutex](../src/expiration_mutex.h) serializes metadata mutations and response
reads; DML only excludes expired-relation drops, allowing concurrent DML. Coordination must
precede opening a transaction snapshot. Nested operations reuse held catalog locks; when both
expiration and history locks are needed, expiration comes first. Drops recheck metadata inside
their transaction.

[Row history](../src/table_history.cc) identifies tables by dataset, name and creation time,
and dates transactional versions at commit. [Partition policy history](../src/partition_policy_history.cc)
uses storage OIDs instead: millisecond creation timestamps cannot reliably distinguish replacements.

## Creation policy

[expiration_policy.cc](../src/expiration_policy.cc) resolves defaults without clock reads or
metadata writes. Request layers must preserve omission versus removal. Creation, replacement
and copying have different inheritance rules; the authoritative edge cases are the
[policy](../tests/e2e/scenarios/testdata/partition_expiration_policy.api.yaml) and
[DDL](../tests/e2e/scenarios/testdata/partition_expiration_ddl.api.yaml) scenarios with BigQuery
answer sidecars, not a duplicate matrix here.

## Partition visibility

Partition expiration filters retained raw rows instead of deleting them, because removing or
extending a policy can expose them again. Policy-only changes therefore need policy history,
not whole-table row copies. The transaction's final policy is published atomically with metadata
at commit.

[partition_policy_read.cc](../src/partition_policy_read.cc) combines row history, policy and
storage identity. Discovery, translation and execution must share a backend transaction:
otherwise a concurrent first write could make a historical query read current rows, or a policy
change could combine old policy with new rows. Counts and paginated data likewise share one
snapshot and evaluation cutoff. Time travel selects raw row history before applying historical
policy.

[History](../tests/e2e/scenarios/testdata/partition_expiration_history.api.yaml) and
[DML](../tests/e2e/scenarios/testdata/partition_expiration_dml.api.yaml) scenarios verify policy
changes, not natural deadline passage or staged-policy visibility. Those unsettled reads are
rejected rather than assigned inferred semantics.
