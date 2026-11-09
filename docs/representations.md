# Value and result representations

This page describes internal representations used by the [runtime](architecture.md).
Client-visible type and encoding limits are in [Query and API limits](compatibility.md).

## Catalog names

Each project is a DuckDB catalog and each dataset a schema. Dataset schemas and table/view
names containing ASCII uppercase letters or starting with `_bq_` are encoded as reversible
`_bq_`-prefixed UTF-8 hex names. Other names stay as written. This preserves case distinctions
without collisions despite DuckDB's identifier matching. Logical resolution applies the
dataset's `is_case_insensitive` setting before physical encoding; history keys use that
resolved, case-sensitive identity.

Catalog reads use `bq_schemas()`, `bq_relations()` and `bq_column_comments()` from
[catalog_functions.h](../src/catalog_functions.h). They look up physical names or read only
names and comments, avoiding `duckdb_tables()` and `information_schema`, which compute storage
information and DDL for every relation and cannot push filters down. Logical-name searches
decode before filtering or returning names. DuckDB's native schemas and internal `main`
tables keep native names.

## Query results and Storage Read

[`ResultChunks`](../src/result_chunks.h) owns the statement's DuckDB chunks and database
lifetime; [`vector_value`](../src/vector_value.h) reads physical values. Both are independent
of GoogleSQL. A job retains the chunks for `jobs.getQueryResults`.
[`ResultRows`](../src/result_rows.h) encodes only the rows a response reads as REST JSON cells.
Values that can fail encoding, such as out-of-range INTERVAL or BIGNUMERIC, are validated when
the statement runs so errors cannot first appear during pagination. Internal consumers read
vectors directly; [`ReadGoogleSqlValue`](../src/result_googlesql.h) converts for scripts and
compliance. Encoders use GoogleSQL where validation or formatting requires it.

Chunks are recursively flattened before sharing, so concurrent readers never normalize shared
buffers. Typed readers use C++ vectors and logical types; generic restoration and formatting
use `Vector::GetValue` and `Value::ToString`.

A Storage Read snapshot owns a backend session and the vectors of one query selecting the
requested fields in GoogleSQL. Retaining the session keeps Arrow connection properties valid.
Each response copies compatible-layout columns into a private chunk and exports through the
Arrow C Data interface. NULL arrays become empty, BIGNUMERIC bytes become decimal256, and
infinite RANGE bounds become NULL. Other layouts are appended directly to Arrow arrays from
typed vectors. Apache nanoarrow serializes Arrow IPC; Avro uses binary row encoding. Both
formats use table metadata for the public schema.

## BIGNUMERIC and TIMESTAMP

BIGNUMERIC query values are BIGNUM integers in units of 10^-38. Stored columns use
`bq_bignumeric_units` or a precision-and-scale alias. `Backend::NumericReadQuery` converts
external integer and decimal columns at the table boundary. Catalog scans, REST reads, copies
and extracts share it; writes retaining external numeric storage are rejected.

`TimestampPrecision` travels with the function catalog, analyzer, parameter translation and
result conversion. BigQuery mode uses `TIMESTAMPTZ`; GoogleSQL mode uses signed Unix nanoseconds
in `HUGEINT`, aliased as `bq_timestamp_nanos`, since `TIMESTAMP_NS` cannot cover years 0001–9999.
The [shared codec](../src/timestamp_nanos.h) checks that range, parses and formats through
GoogleSQL, and decodes vectors. The [DuckDB adapter](../src/googlesql_functions_duckdb/timestamp.h)
registers the alias and formatting/exact microsecond conversion UDFs.

Carrying and comparing functions operate on integers directly. Other scalar calls cross the
microsecond adapter, which rejects remainders rather than truncating; timestamp RANGE bounds
retain microseconds. Query and write boundaries recursively restore aliases from resolved
types, since integer operations can discard them. A bare `HUGEINT` is not inferred as a timestamp.

## Collation

GoogleSQL propagates [collations](../third_party/bigquery_docs/reference/standard-sql/collation-concepts.md)
as annotations and records each operation's collation in the AST.
[collation.h](../src/translator/collation.h) is the translator's only reader of both.
Comparison, ordering and grouping use `bq_collation_key` BLOBs, the ICU sort keys of GoogleSQL's
collator. `kComparesCollationKeys` keys a function's STRING arguments; `ORDER BY` keys its items.
Searching or matching within strings needs GoogleSQL's collated implementations instead.
Other operations reject non-empty collations, including set operations, whose collation the
AST does not record.

`FieldSchema::collation` becomes the catalog column's annotation. View columns take their
query's annotations; result and new-table schemas record output annotations.
[googlesql_collation](../src/googlesql_collation/) owns collators and the `bigquery_collations`
option restricting specifications to BigQuery's. Its adapter registers keys and specification
checks. COLLATE checks literal specifications during translation, before pruning can skip
execution. BigQuery mode takes only `und:ci` and the empty specification.

## Graphs and partition-result analytics

Graph definitions use statically linked [DuckPGQ](https://github.com/cwida/duckpgq-extension).
The translator turns fixed paths into joins on element tables and endpoint keys; ordinary SQL
translation handles properties and filters. Multi-table node paths union the common property
set with table-specific identities. Undirected edges match either orientation over one edge
row, emitting self-loops once. Each path applies its mode before shared-identity joins.

Private graph names encode project, dataset and graph name because DuckPGQ's registry is shared
across catalogs. `main._property_graphs` stores canonical GoogleSQL DDL for resolution and native
DDL for restoring the registry. Intermediate graph elements are structs of private identity,
keys, labels and nested properties. Edges retain source/destination identities. Fixed paths
contain separate node and edge lists widened to the resolved types. These structs never become
ordinary client-visible STRUCT results.

Analytic rules with `partition_results`, such as `ST_CLUSTERDBSCAN`, return one array element
per input row. Translation materializes row identities, computes each partition once, expands
the result array and joins it back by identity, avoiding repeated clustering per row.

## Geography and sketches

[googlesql_geography](../src/googlesql_geography/) owns typed S2 operations and WKT parsing and
formatting without DuckDB. Its [adapter](../src/googlesql_geography_duckdb/) converts arguments
and results. Values are BLOBs, stored under `bq_geography`, holding exact S2 vertices and an
encoded S2ShapeIndex. Distance functions query the index without rebuilding it; predicates and
set operations snap inputs together and build their own. Callbacks own compatibility options
independently for each database.

HLL uses the [zetasketch port](../src/zetasketch/README.md); the HLL library supplies BigQuery
input hashing, type identifiers, precision and merge rules. Text analysis keeps tokenization,
document counts and scores in its library; its adapter encodes the intermediate window
dictionary and caches parsed analyzers and dictionaries.

### KLL sketches

[src/kll_duckdb/](../src/kll_duckdb/kll.cc) reads and writes a protobuf envelope compatible with
BigQuery, not Apache DataSketches' preamble. [tools/kll/](../tools/kll/README.md) keeps observed
fields, fixed-sketch answers and validation cases. The reader decodes INT64 exactly, includes
sampler mass, normalizes ranks by retained mass rather than input count, and preserves extrema.
Integer rank arithmetic avoids FLOAT64 rounding. Wire reads, varints, shifts and accumulated
weights are bounded; ambiguous duplicate singular fields, unsupported wire forms and versions
are rejected. Absent encoding versions and versions 1/2 are accepted; unknown varint and
length-delimited fields are skipped.

INIT and MERGE share unbiased randomized pair compactors with `64 * precision` capacity per
level and separately stored exact extrema. Weighted inserts split weights into powers of two;
merges import retained weights without expanding to rows. Input count and retained mass stay
separate. Precision starts with the first non-empty sketch and falls only for a compacted
lower-precision input, following [recorded merge answers](../tools/kll/merge_precision.answers.txt).
The fixed capacity trades space for a simple error argument; it does not reproduce BigQuery's
capacity formula, thresholds or random sequence. BigQuery can read and merge the output
([writer answers](../tools/kll/writer.answers.txt)); see [client-visible costs](compatibility.md#approximate-aggregates).

Typed MERGE and MERGE_POINT merge like MERGE_PARTIAL and then use the EXTRACT reader.
Translator handlers and aggregate rules enforce constant quantile-count and phi restrictions
missing from GoogleSQL signatures. The backend validates ranges before propagating NULL sketches.
