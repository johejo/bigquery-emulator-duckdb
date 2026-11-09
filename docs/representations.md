# Value and result representations

These encodings bridge GoogleSQL and DuckDB where their native types or ownership models
differ. See [architecture](architecture.md) for component boundaries and
[Query and API limits](compatibility.md) for client-visible limits.

## Catalog names

Each project is a DuckDB catalog and each dataset a schema. Names containing ASCII uppercase
letters or starting with `_bq_` use reversible `_bq_`-prefixed UTF-8 hex encoding, preserving
case distinctions despite DuckDB's identifier matching. Logical case-insensitive resolution
happens before encoding; history uses the resolved identity.

Catalog reads use [dedicated functions](../src/catalog_functions.h) rather than
`duckdb_tables()` or `information_schema`: those compute storage information and DDL for every
relation and cannot push filters down. Logical-name searches must decode before filtering.

## Query results and Storage Read

[`ResultChunks`](../src/result_chunks.h) retains chunks and database lifetime independently
of jobs. Chunks are recursively flattened before sharing so concurrent readers never normalize
shared buffers. Values that can fail REST encoding are validated during execution, not lazily
during pagination.

Storage snapshots also retain their backend session to keep Arrow connection properties valid.
Exports use private chunks or arrays rather than mutating shared results. Table metadata defines
the public schema; physical layouts alone cannot determine it.

## BIGNUMERIC and TIMESTAMP

BIGNUMERIC uses BIGNUM integers in units of 10^-38. Stored columns carry a type alias;
`Backend::NumericReadQuery` converts external numeric storage at table boundaries. Writes
retaining external numeric storage are rejected.

BigQuery mode uses `TIMESTAMPTZ`. GoogleSQL's nanosecond mode uses signed Unix nanoseconds in
`HUGEINT`, aliased as `bq_timestamp_nanos`: DuckDB's `TIMESTAMP_NS` cannot cover years 0001–9999.
The [shared codec](../src/timestamp_nanos.h) validates that range. Scalar calls requiring
microseconds reject remainders instead of truncating. Query and write boundaries recursively
restore aliases from resolved types because integer operations can discard them; a bare
`HUGEINT` must not be inferred as a timestamp.

## Collation

GoogleSQL records operation collations in the resolved AST. The translator reads them only
through [collation.h](../src/translator/collation.h), never inferring them from argument types.
Comparison, ordering and grouping use ICU sort-key BLOBs; substring matching requires
GoogleSQL's collated implementations. See [AGENTS.md](../AGENTS.md#collation) for extension rules.

COLLATE validates literal specifications during translation so pruning cannot skip validation.
Column and result annotations remain metadata, separate from the physical STRING value.

## Graphs and partition-result analytics

Graph elements use private STRUCTs carrying identity, keys, labels and properties; they must
not escape as ordinary client-visible STRUCTs. Private graph names include project and dataset
because [DuckPGQ](https://github.com/cwida/duckpgq-extension) shares its registry across catalogs.
`main._property_graphs` retains both GoogleSQL DDL for resolution and native DDL for restoration.

Analytic rules with `partition_results`, such as `ST_CLUSTERDBSCAN`, compute an array once per
partition and join elements back by materialized row identity, avoiding repeated computation
per row.

## Geography and sketches

GEOGRAPHY BLOBs, aliased as `bq_geography`, hold exact S2 vertices and an encoded S2ShapeIndex.
Distance functions reuse the index; predicates and set operations snap inputs together and
build their own. The [semantic library](../src/googlesql_geography/) has no DuckDB dependency.

HLL's wire format and hashing are described in the [zetasketch port](../src/zetasketch/README.md).

### KLL sketches

The [KLL adapter](../src/kll_duckdb/kll.cc) uses BigQuery's protobuf envelope, not Apache
DataSketches' preamble. [tools/kll](../tools/kll/README.md) records observed fields and
independently verified sketches.

Input count and retained mass are distinct: extraction ranks use retained mass, including the
sampler, and exact integer arithmetic. INIT and MERGE use randomized pair compactors with
`64 * precision` capacity per level and separately retained exact extrema. This trades space
for a simple error argument, not reproduction of BigQuery's capacity formula or random sequence.
[Recorded answers](../tools/kll/writer.answers.txt) establish that BigQuery can read and merge
the output; [client-visible costs](compatibility.md#approximate-aggregates) remain a limitation.

Precision follows [recorded merge answers](../tools/kll/merge_precision.answers.txt), rather
than simply taking the minimum input precision. Typed merge functions use the same merge and
extraction paths so they cannot disagree with MERGE_PARTIAL followed by EXTRACT.
