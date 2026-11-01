# Query and API limits

This page records emulator-specific limits and behavior that the generated [SQL](sql.md),
[DDL option](options.md), [function](functions.md) and [REST](api.md) support tables cannot
show. For supported features, use BigQuery's documentation for their normal semantics. The tables check execution, not
agreement with BigQuery's results or metadata.

Deliberate compatibility exceptions are listed in the [README](../README.md#compatibility).
Storage API usage and limits are in its [guide](storage-api.md).

## Jobs

Jobs finish before `jobs.insert` or `jobs.query` returns. Consequently, `jobs.cancel` returns the
finished job, and `jobs.list` returns no jobs for a `stateFilter` of `pending` or `running`.
Unsupported SQL constructs fail as `invalidQuery`, naming the construct.

Multi-statement queries create no child jobs: `jobs.list` omits them and rejects `parentJobId`.

### Locations

Copies across dataset locations are unsupported. To retrieve, cancel or delete a job outside the
`US` and `EU` multi-regions, supply its location to `jobs.get`, `jobs.getQueryResults`,
`jobs.cancel` or `jobs.delete`.

### Loads, extracts and copies

Loads and extracts also accept local paths. Their limits are:

- GeoParquet requires binary WKB geometry columns in CRS84 (the default when `crs` is omitted).
  Other encodings, coordinate reference systems and coordinate epochs are unsupported, as are
  loads combining files with differing GeoParquet metadata or explicitly mapping geometry to a
  non-GEOGRAPHY field. Planar polygons with an edge along a pole are unsupported.
- Parquet DECIMAL values wider than 38 digits require BIGNUMERIC; schema detection selects it
  only when `decimalTargetTypes` includes it.
- Parquet files with differing schemas cannot load if one contains a nullable LIST or a
  single-field group that DuckDB reads as its field, including a LIST without
  `parquetOptions.enableListInference`.
- Query and load jobs cannot partition or cluster their destination table. Loads reject
  `destinationTableProperties`, and replacement of an existing table with `autodetect` for
  formats other than CSV.
- CSV schema detection, which a load also performs when neither it nor its destination gives a
  schema, takes one file, and rejects header names with a form feed or vertical tab.
- Extracts write the whole table to one file and reject INTERVAL, RANGE and GEOGRAPHY columns.
  BIGNUMERIC Parquet output uses `DECIMAL(76, 38)` with `BYTE_ARRAY` rather than
  `FIXED_LEN_BYTE_ARRAY`.
- A copy job's `destinationExpirationTime` is accepted only when creating its destination.
- Copy jobs with `operationType` `SNAPSHOT`, `RESTORE` or `CLONE` take one source table without a
  default rounding mode and create their destination with `WRITE_EMPTY`; a `RESTORE` can also
  replace a table with `WRITE_TRUNCATE`, and a `CLONE` cannot copy a table snapshot.

## Datasets

BigQuery Omni locations, default partition expiration and access controls are unsupported.
`datasets.patch` and `datasets.update` reject changes to the dataset's location.

`INFORMATION_SCHEMA.SCHEMATA` returns NULL last modified times and NULL options other than the
location. Datasets created before the emulator recorded creation times have no `creationTime`
in `datasets.get`, and NULL creation times in `SCHEMATA`.

## Tables

### External tables

GCS external tables support Parquet through `CREATE EXTERNAL TABLE` or `tables.insert`. Sources
must use `gs://` URIs; multiple URIs and a single wildcard per URI are supported. A table created
without a schema takes the one BigQuery detects from the alphabetically last source file when it
is created, so creation reads the sources. Columns and their fields must be of the Parquet types
that the [Parquet conversions](../third_party/bigquery_docs/loading-data-cloud-storage-parquet.md)
convert to INT64, BOOL, FLOAT64, STRING, BYTES, DATE, TIME or TIMESTAMP, a DECIMAL that NUMERIC
holds, or a group, which is a RECORD; a repeated field is REPEATED. By default a LIST or MAP
group is a RECORD of its repeated field. DDL `enable_list_inference` and REST
`parquetOptions.enableListInference` can unwrap LIST groups into REPEATED fields. Unlike loads,
external reads preserve NULL LIST elements. An explicit schema must give each column and field
the type detected for it; coercions, ENUM, GeoParquet, column name conversions and the
options that select other types are unsupported. Every source file must have each REQUIRED and
REPEATED column and field of the schema.

CSV, JSON, Bigtable, BigLake connections, Hive partitioning, metadata caching,
Configuration updates, `ALTER TABLE`, stored views over external tables and time
travel are unsupported. Query dry runs and stored definitions reject external reads rather
than accessing GCS during analysis. `tabledata.list`, Storage Read, extracts and copy jobs do
not read external tables; use a query instead. Writes to external tables are rejected.

### Resource tags

DDL `tags` and REST `resourceTags` share stored metadata for datasets, tables and views,
including materialized views and table snapshots. Tags do not enforce access controls or
require Cloud Resource Manager tag definitions; see the [IAM exception](../README.md#compatibility).

Incremental additions (`+=`), schema tag removals (`-=`), empty tag patches on tagged
datasets, NULL dataset tags, NULL `resourceTags` in full REST updates, and copying tagged
tables through copy jobs are unsupported until their metadata behavior is verified. Use DDL `SET OPTIONS (tags = ...)` or a full REST update to replace tags.

BigQuery returns no tag option row after clearing tags, although its
[tag documentation](../third_party/bigquery_docs/tags.md) says that untagged resources
return an empty array. The recorded BigQuery cases retain the project and run that established
this behavior.

### Options and expiration

[options.md](options.md) lists the DDL options the emulator accepts. Partition expiration is
unsupported.

DDL option values accept literals and constant expressions using query parameters and supported
scalar functions. Other expressions, such as a parenthesized script variable, are unsupported.

Datasets, tables and views created before DDL label order was recorded list labels in key order until
`SET OPTIONS (labels = ...)` replaces them.

Expired tables and views are deleted when the catalog is read, including before query analysis,
except by statements in a transaction, which can still read a table that expires while the
transaction is open.
Tables and views created before the emulator recorded creation times have no `creationTime` in
`tables.get`, and NULL creation times in `INFORMATION_SCHEMA.TABLES`.

### Partitioning and clustering

Ingestion-time partitioning (`_PARTITIONTIME`, `_PARTITIONDATE`) and partition expiration are
unsupported. `INFORMATION_SCHEMA.PARTITIONS` counts rows when queried. Storage byte counts are 0,
including `tables.get`'s `numBytes`; partition `last_modified_time` is NULL and storage tier is
always `ACTIVE`.

For `require_partition_filter`, transforms of temporal partition keys are limited to `DATE_ADD`,
`DATE_SUB`, `EXTRACT(YEAR FROM ...)`, `DATE_TRUNC`, `DATE` and `TIMESTAMP_TRUNC`, with constant
arguments besides the key. Projections propagate filters through direct column renames only. Outer
predicates do not propagate through aggregation, LIMIT or other scan boundaries.
The option cannot be enabled on an unpartitioned table or view.

### Column defaults

Parameterized columns accept literal defaults only. `tables.update` and `tables.patch` cannot
add or change defaults, and the Storage Write API cannot write to a table with defaults.

### Primary and foreign keys

Tables keep their primary keys, which `tables.get` reports, but
`INFORMATION_SCHEMA.TABLE_CONSTRAINTS` and `KEY_COLUMN_USAGE` are unsupported. Foreign keys in
`CREATE TABLE` and `tableConstraints` are accepted and dropped, and `ALTER TABLE ADD FOREIGN KEY`
and `DROP CONSTRAINT` are unsupported.

`tableConstraints` on materialized views are unsupported until BigQuery's behavior is verified.

### ALTER TABLE

Supported actions are `ADD COLUMN`, `DROP COLUMN`, `RENAME TO`, `SET OPTIONS`, `ADD PRIMARY KEY`,
`DROP PRIMARY KEY`, and `ALTER COLUMN` with `SET OPTIONS`, `SET DEFAULT` or `DROP DEFAULT`. Other actions are unsupported. Column options
have the limits described under [Options and expiration](#options-and-expiration). See also
[External tables](#external-tables).

### Copies and clones

For `CREATE TABLE LIKE`, `COPY` and `CLONE`, see the [SQL support table](sql.md) for rejected
forms.

### Table snapshots

`FOR SYSTEM_TIME AS OF` has the limits of [time travel](#time-travel). `ALTER TABLE` actions other than `SET OPTIONS`, `ADD COLUMN` and `DROP COLUMN` are unsupported on a
snapshot rather than rejected with BigQuery's message.

### Time travel

`FOR SYSTEM_TIME AS OF` reads the rows a table had within BigQuery's default seven-day time travel
window, which datasets cannot change. It is unsupported:

- before the table was created, including a table it replaced by `CREATE OR REPLACE TABLE` or a
  job's `WRITE_TRUNCATE`, before `ALTER TABLE ADD COLUMN` or `DROP COLUMN`, and before
  `tables.patch` or `tables.update` changed its columns;
- on temporary tables;
- for times other than a query parameter, a script variable, or `CURRENT_TIMESTAMP()` with
  `TIMESTAMP_ADD` or `TIMESTAMP_SUB`, since BigQuery evaluates the time in America/Los_Angeles.

### Schema changes

`tables.patch` and `tables.update` change schema, view definition, description, friendly name,
labels, clustering, default rounding mode and expiration; other fields set to other than their
defaults are unsupported. Partitioning and
a view's schema cannot change. For resource tags, see [Resource tags](#resource-tags).
`tables.update` keeps a schema or view definition omitted from the request.

### Views

Temporary, recursive and value-table views, and views that read temporary tables, are unsupported.

Queries resolve views using the current definitions of persistent SQL UDFs and table functions.
A changed UDF return type is reflected in query results, while the stored view schema stays
unchanged.

### Materialized views

Materialized views are limited to a single managed base table, projections, filters and
`COUNT`, `COUNTIF`, `SUM`, `MIN`, `MAX` and `AVG`, with aggregate results used directly as outputs.
Distinct aggregates, grouping sets, nested aggregation, joins, subqueries, CTEs, ordering, limits,
view and snapshot references, and explicit output column lists are unsupported.

Partitioning, clustering, replicas and `ALTER MATERIALIZED VIEW` are unsupported, and so are the
options [options.md](options.md#create-materialized-view) lists as such. The API accepts the
default `enableRefresh: true` and `allowNonIncrementalDefinition: false`; changing the definition
or refresh settings through `tables.patch` or `tables.update` is unsupported. Metadata-only
updates preserve the definition and materialized-view type.

See the [README compatibility exceptions](../README.md#compatibility) for refresh state and storage statistics.

## Types

### STRUCT values

An empty `STRUCT<>` can be passed only to JSON functions, `CONTAINS_SUBSTR` and ARRAY constructors; aggregates and
other functions reject it. BigQuery's restrictions on empty STRUCTs in query results also apply.

### INTERVAL values

INTERVAL arithmetic is limited to `+`, `-`, unary `-`, multiplication by INT64 and division by
INT64, and subtraction between DATE, DATETIME, TIMESTAMP or TIME values. Other limits are:

- `PARTITION BY` over ARRAYs containing INTERVAL values is unsupported.
- Aggregate and analytic functions are limited to `SUM`, `AVG`, `ARRAY_AGG`, `ARRAY_CONCAT_AGG`,
  `ANY_VALUE`, `COUNT`, `MIN`, `MAX`, `MIN_BY`, `MAX_BY` and navigation functions, subject to
  BigQuery's own restrictions on INTERVAL inputs.
- JSON conversions are limited to `TO_JSON`, `TO_JSON_STRING`, `JSON_ARRAY`, `JSON_OBJECT` and
  `JSON_SET`.
- Loads accept only CSV, with the full `Y-M D H:M:S[.F]` form. Extracts and Storage API reads
  are unsupported.

### Collation

[Collation](../third_party/bigquery_docs/reference/standard-sql/collation-concepts.md) support
is listed in [sql.md](sql.md#collation) and [functions.md](functions.md). `und:ci` uses
GoogleSQL's collator, ICU's root locale at secondary strength, whose Unicode data can order
some strings differently from BigQuery's.

Default collations on views and materialized views with collated columns remain unsupported.
Aggregates that compare collated ARRAY elements or STRUCT fields are limited to
`COUNT(DISTINCT ...)`; value-returning DISTINCT aggregates over those types remain unsupported.

### RANGE values

Functions over RANGE values are limited to range functions, casts, comparisons, JSON functions,
`FORMAT`, `COUNT`, `MIN`, `MAX`, `ANY_VALUE`, `ARRAY_AGG` and navigation functions. Extracts are
unsupported. JSON and Parquet loads fail, including a Parquet group of `start` and `end` that
BigQuery loads as NULL.

### GEOGRAPHY values

- `ST_DISTANCE` with `use_spheroid = TRUE` is unsupported for nonempty inputs.
- `ST_ASGEOJSON` splits lines and polygons at the antimeridian and closes polygons enclosing
  a pole along the map boundary. Tessellated coordinates can differ from BigQuery in their
  last digits; clipped polygon order, ring starts and boundary vertices can also differ, and
  lines through a pole can have different intermediate vertices. Some polygon cuts touching a
  pole and degenerate antimeridian cuts are unsupported.
- `S2_COVERINGCELLIDS` does not support `max_cells` above 2,147,483,647.
- `SAFE.ST_BUFFER` and `SAFE.ST_BUFFERWITHTOLERANCE` return NULL for `use_spheroid = TRUE`,
  where BigQuery fails.
- Planar polygons with an edge along a pole, including GeoJSON polygons such as
  `ST_ASGEOJSON`'s full polygon, are unsupported.

`ST_BOUNDINGBOX` latitude bounds can differ from BigQuery in the last floating-point bit.
`ST_SNAPTOGRID` rounds a coordinate halfway between grid lines, such as longitude 1.25 on a 0.1
degree grid, by its value after a round trip through S2's unit vectors, whose last bit depends on
the platform's math library. BigQuery rounds `ST_GEOGPOINT(1.25, -2.25)` to `POINT(1.2 -2.2)`, as
the emulator does on x86-64 Linux, while it returns `POINT(1.3 -2.3)` on arm64.

### RANGE_SESSIONIZE

The range column, partitioning columns and sessionize option must be non-NULL literals.
Value tables and INTERVAL or annotated partitioning columns are unsupported.

### EXTERNAL_QUERY

Connections to PostgreSQL, MySQL and Spanner are configured with `--connection`; see the
[README](../README.md#external-queries) for setup. The connection ID,
query and options must be literals, and views that read `EXTERNAL_QUERY` are unsupported.

PostgreSQL queries are prepared during analysis and read through a read-only attachment.
Their limits are:

- Types outside BigQuery's PostgreSQL mapping are unsupported, except `name`, which reads as
  STRING. `bit` and `bit varying`, which BigQuery reads as BYTES, are also unsupported.
- Duplicate column names are unsupported.
- Only the `default_type_for_decimal_columns` option is supported.

MySQL queries use a read-only attachment. Duplicate column names, zero dates, JSON results and
types outside BigQuery's [MySQL mapping](../third_party/bigquery_docs/reference/standard-sql/federated_query_functions.md)
are unsupported. Only the `default_type_for_decimal_columns` option is supported.

Spanner queries are limited to GoogleSQL returning BOOL, BYTES, DATE, FLOAT64, INT64, JSON,
NUMERIC, STRING and TIMESTAMP, using BigQuery's
[Spanner mapping](../third_party/bigquery_docs/reference/standard-sql/federated_query_functions.md).
ARRAY, STRUCT, annotated PostgreSQL types and all nonempty options, including
`query_execution_priority`, are unsupported. Queries run as single-use, strong, read-only
transactions; query partitioning and Data Boost are not implemented.

### ObjectRef functions

`OBJ.FETCH_METADATA` reads objects from the Cloud Storage endpoint that loads read; see the
[README](../README.md#google-cloud-storage). An authorizer must name a Cloud resource connection that
`--connection PROJECT.LOCATION.CONNECTION=cloud_resource` declares; any other is recorded as a
connection that cannot be used, as BigQuery records one that does not exist. Every declared
connection can read every object, and its location is not checked against the bucket's.

`OBJ.MAKE_REF`'s JSON form, `OBJ.GET_ACCESS_URL`, `OBJ.GET_READ_URL` and `OBJ.LIST` are
unsupported.

## User-defined functions

### SQL UDFs

Persistent SQL UDF bodies require dataset-qualified table and routine references and cannot call
temporary UDFs.

### Table functions

Only persistent SQL table functions with fixed scalar argument types are supported. Temporary,
non-SQL, templated, table-argument and value-table functions, and security clauses are unsupported.

### JavaScript UDFs

Temporary JavaScript UDF arguments are limited to BOOL, FLOAT64, STRING, BYTES, and arrays and
STRUCTs of those types. STRUCT fields must be named by JavaScript identifiers. Return types are
limited to BOOL, FLOAT64, STRING and INT64. The default call timeout is 5 minutes, and the
allocation limit is 256 MiB per query per thread; see
[JavaScript UDF limits](../README.md#javascript-udf-limits) for configuration and the
[compatibility exceptions](../README.md#compatibility) for runtime differences.

### Python UDFs

Persistent scalar Python UDFs support BOOL, INT64, FLOAT64, STRING and BYTES arguments and
results, with explicit result types. The runtime must be `python-3.11`; `runtime_version` and
`entry_point` are required. Description metadata is supported through DDL and the Routine API.
Other argument and result types, packages, external connections, libraries, container settings,
vectorized entry points and `SAFE.` calls are unsupported. FLOAT64 results also accept Python
integers; other non-NULL results require the declared Python type.

Workers have per-call timeouts and per-process memory limits, and are discarded when their
query execution state is released. There is no shared Python state between query jobs. On Linux,
the memory limit applies to address space above the worker's startup mappings, including Python
initialization; on macOS the parent checks total resident memory every 10 ms, so short
allocations can exceed the limit between checks. The defaults and configuration are described in
[Python UDFs](../README.md#python-udfs).

### Unsupported forms

Persistent JavaScript UDFs, aggregate UDFs, conditional routine updates, `SAFE.` UDF calls,
SQL UDF options other than description, JavaScript UDF options, the `library` option, and views that call
temporary SQL UDFs are unsupported.
Subquery bodies do not support volatile arguments. Queries with TEMP function declarations do
not support dry runs, destination tables or positional parameters.

## Scripts and transactions

Multi-statement queries cannot be dry runs. With a request default dataset, reading
`@@dataset_id` or `@@dataset_project_id` before assigning it is unsupported until its initial
value is verified against BigQuery. Assignments to other system variables are unsupported.
An `EXCEPTION` clause never handles an error that the emulator reports as unsupported.

Transaction modes, SQL UDF declarations inside transactions, writes to multiple projects (or
both permanent and temporary tables), and committing after a handled error are unsupported.
Roll back a failed transaction: it cannot preserve statements before the error for a later commit.

## Stored procedures

`ANY TYPE` arguments, non-SQL procedures, and options other than `strict_mode` and
`description` are unsupported.

Inside procedures, including their dynamic SQL, system variable reads are limited to
`@@dataset_id`, `@@dataset_project_id`, `@@project_id`, `@@time_zone` and `@@error.message`;
assignments are limited to `@@dataset_id` and `@@dataset_project_id`.
`EXECUTE IMMEDIATE` cannot execute a `CALL`. CALL queries do not support dry runs, destination
tables or positional query parameters.

## Approximate aggregates

`APPROX_TOP_SUM` computes exact sums, collecting the input rows before grouping them, so its memory
use grows with the input size rather than with the requested number of results.

`KLL_QUANTILES` sketches keep up to `64 * precision` items per compaction level, more than
BigQuery's sketches retain, so they grow larger and their approximate quantiles can differ from
BigQuery's for the same input. BigQuery reads and merges them; their bytes differ from those it
writes ([KLL sketches](architecture.md#kll-sketches)). At the default precision a level holds up to
64,000 values, so a sketch and each group's aggregation state can reach hundreds of KB per level;
queries with many groups use correspondingly more memory.

## Differential privacy

`SELECT WITH DIFFERENTIAL_PRIVACY` adds noise and removes small groups with Google's differential
privacy library, as GoogleSQL's reference implementation does. It computes `AVG`, `COUNT` and
`SUM`; `PERCENTILE_CONT` is unsupported.
Privacy parameters and contribution bounds must be literals, as BigQuery requires.

## Result schemas and values

Tables written by another DuckDB client may contain types BigQuery does not have:

- A DECIMAL that fits NUMERIC reads as NUMERIC; TIMESTAMP_NS reads as DATETIME truncated to
  microseconds; fixed-size arrays read as REPEATED.
- ENUM, UUID, BIT, MAP and UNION read as STRING in DuckDB's text form.
- UBIGINT, HUGEINT, UHUGEINT and DECIMAL types that do not fit NUMERIC read as BIGNUMERIC,
  including inside arrays and structs. Tables containing these types are read-only through the
  emulator: DML, streaming writes and jobs that retain the destination's schema are unsupported.
  Copy the table with `CREATE TABLE COPY` or a copy job to obtain writable emulator storage;
  `WRITE_TRUNCATE` can also replace it.
- Unmarked BIGNUM columns and nested fields are unsupported. The emulator reserves type aliases
  beginning with `bq_bignumeric_` for its scaled BIGNUMERIC storage.
- Numeric column metadata that disagrees with the actual DuckDB type is rejected.

Conversely, the emulator's BIGNUMERIC columns store integers in units of 10^-38 under
`bq_bignumeric_` type aliases. Other DuckDB clients, such as the DuckDB CLI, show those integers,
and their ordinary numeric operations do not implement BIGNUMERIC semantics; use the emulator to
read or update those values.

Duplicate result column names are compared without case. Without a destination table, a second
occurrence gains `_1`, as in BigQuery. Names repeated more than twice, or collisions with renamed
columns, are rejected because BigQuery's naming is unverified. In `SELECT AS STRUCT` and
`SELECT AS VALUE` results, the second occurrence uses its position, such as `_field_2`; further
repetitions and collisions with positional names are rejected.

REST timestamps use epoch seconds, or epoch microseconds with `formatOptions.useInt64Timestamp`,
including `RANGE<TIMESTAMP>` bounds. Seconds use Java's FLOAT64 notation, such as
`1.57793424525E9`; range bounds have six fractional digits. See the
[README](../README.md#compatibility) for the ignored `timestampOutputFormat` option.

The `SAFE.` prefix is unsupported on volatile functions.

Transcendental functions, such as SIN and EXP, and GEOGRAPHY measurements, such as ST_AREA, use
the platform's math library, so their FLOAT64 results can differ from BigQuery's in the last bits,
such as on macOS.

## Graphs

Graph DDL rejects:

- Keys of type FLOAT64, BIGNUMERIC, INTERVAL, RANGE, GEOGRAPHY, JSON, ARRAY or STRUCT, and keys
  other than columns.
- Computed properties, measures and property options.
- Views as element tables and graph options.
- `OR REPLACE` and `IF NOT EXISTS` in `CREATE PROPERTY GRAPH`, and `IF EXISTS` in
  `DROP PROPERTY GRAPH`.
- Edges whose endpoint nodes do not exist when the graph is created.

`GRAPH_TABLE` and GQL queries reject:

- Unbounded repetition, repetition bounds that are not literals, and `ANY CHEAPEST`.
- The GQL `CALL` statement and SQL/PGQ's `COLUMNS` clause.
- Graph elements or paths in query results.
- Graph values in comparisons such as `a = b`, grouping sets, `PARTITION BY`, and aggregate or
  analytic functions other than `COUNT`; paths in `GROUP BY`, `DISTINCT` and `COUNT(DISTINCT)`.
- Horizontal aggregation over group variables, such as `ARRAY_AGG(e.id)`.
- `ELEMENT_ID`, `SOURCE_NODE_ID` and `DESTINATION_NODE_ID`.
- NULL operands of `IS [NOT] SOURCE [OF]` and `IS [NOT] DESTINATION [OF]`, until their BigQuery
  behavior is verified.
- `SAFE.` calls of `SAME` and `ALL_DIFFERENT`, and calls that mix nodes and edges.
