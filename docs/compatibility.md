# Query and API limits

This page records emulator-specific limits and behavior that the generated [SQL](sql.md),
[function](functions.md) and [REST](api.md) support tables cannot show. For supported features,
use BigQuery's documentation for their normal semantics. The tables check execution, not
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

- Parquet DECIMAL values wider than 38 digits require BIGNUMERIC; schema detection selects it
  only when `decimalTargetTypes` includes it.
- Parquet files with differing schemas cannot load if one contains a nullable LIST or a
  single-field group that DuckDB reads as its field, including a LIST without
  `parquetOptions.enableListInference`.
- Query and load jobs cannot partition or cluster their destination table. Loads reject
  `destinationTableProperties`.
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

### Options and expiration

Table options are limited to description, friendly name, labels, expiration and
default rounding mode; column options to description and rounding mode.
Partition expiration is unsupported.

DDL option values accept literals and constant expressions using query parameters and supported
scalar functions. Other expressions, such as a parenthesized script variable, are unsupported.

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
UPDATE, DELETE, MERGE and TRUNCATE targeting a table with this requirement are unsupported. The DDL
option cannot be NULL, or be enabled on an unpartitioned table or view.

### Column defaults

Parameterized columns accept literal defaults only. `tables.update` and `tables.patch` cannot
add or change defaults, and the Storage Write API cannot write to a table with defaults.

### ALTER TABLE

Supported actions are `ADD COLUMN`, `DROP COLUMN`, `RENAME TO`, `SET OPTIONS`, and `ALTER COLUMN`
with `SET OPTIONS`, `SET DEFAULT` or `DROP DEFAULT`. Other actions are unsupported. Column options
have the limits described under [Options and expiration](#options-and-expiration).

### Copies and clones

For `CREATE TABLE LIKE`, `COPY` and `CLONE`, see the [SQL support table](sql.md) for rejected
forms.

### Table snapshots

The emulator keeps no table history, so `CREATE SNAPSHOT TABLE` copies the table's current rows.
`ALTER TABLE` actions other than `SET OPTIONS`, `ADD COLUMN` and `DROP COLUMN` are unsupported on a
snapshot rather than rejected with BigQuery's message, and a table restored from a snapshot omits
the empty `friendly_name` and `labels` that BigQuery lists in `TABLE_OPTIONS`.

### Schema changes

`tables.patch` and `tables.update` change schema, view definition, description, friendly name,
labels, clustering, default rounding mode and expiration, and ignore other fields. Partitioning and
a view's schema cannot change. `tables.update` keeps a schema or view definition omitted from the request.

### Views

Temporary, recursive and value-table views, and views that read temporary tables, are unsupported.
View options are limited to description, friendly name, labels and expiration.

Queries resolve views using the current definitions of persistent SQL UDFs and table functions.
A changed UDF return type is reflected in query results, while the stored view schema stays
unchanged.

## Types

### STRUCT values

An empty `STRUCT<>` can be passed only to JSON functions and ARRAY constructors; aggregates and
other functions reject it. BigQuery's restrictions on empty STRUCTs in query results also apply.

### INTERVAL values

INTERVAL arithmetic is limited to `+`, `-`, unary `-`, multiplication by INT64 and division by
INT64, and subtraction between DATE, DATETIME, TIMESTAMP or TIME values. Other limits are:

- `PARTITION BY` and distinct set operations over ARRAYs with INTERVAL elements are unsupported.
- Aggregate and analytic functions are limited to `SUM`, `AVG`, `ARRAY_AGG`, `ARRAY_CONCAT_AGG`,
  `ANY_VALUE`, `COUNT`, `MIN`, `MAX`, `MIN_BY`, `MAX_BY` and navigation functions, subject to
  BigQuery's own restrictions on INTERVAL inputs.
- JSON conversions are limited to `TO_JSON`, `TO_JSON_STRING`, `JSON_ARRAY`, `JSON_OBJECT` and
  `JSON_SET`.
- Loads accept only CSV, with the full `Y-M D H:M:S[.F]` form. Extracts and Storage API reads
  are unsupported.

### RANGE values

Functions over RANGE values are limited to range functions, casts, comparisons, JSON functions,
`FORMAT`, `COUNT`, `MIN`, `MAX`, `ANY_VALUE`, `ARRAY_AGG` and navigation functions. Extracts are
unsupported. JSON and Parquet loads fail, including a Parquet group of `start` and `end` that
BigQuery loads as NULL.

### GEOGRAPHY values

Loads and `tabledata.insertAll` take GEOGRAPHY values as WKT; GeoJSON and WKB are unsupported.
Query parameters, loads and `tabledata.insertAll` take a polygon only where its shell runs
counterclockwise and its holes clockwise, since BigQuery takes the region on the left of each
ring. Additional limits are:

- `ST_DISTANCE` with `use_spheroid = TRUE` is unsupported for nonempty inputs.
- `ST_ASGEOJSON` is unsupported for anything but a single point, the empty geography and the
  full polygon.
- Polygons that require edge snapping, rings that cross or touch at an edge, and shells with
  holes that collapse are unsupported.
- `ST_GEOGFROMTEXT`'s `oriented`, `planar` and `make_valid` arguments are unsupported.

`ST_MAXDISTANCE` measures over edges, as BigQuery does, although its documentation describes
vertices.

`ST_BOUNDINGBOX` latitude bounds can differ from BigQuery in the last floating-point bit.

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
types outside BigQuery's [MySQL mapping](https://cloud.google.com/bigquery/docs/reference/standard-sql/federated_query_functions#mysql_mapping)
are unsupported. Only the `default_type_for_decimal_columns` option is supported.

Spanner queries are limited to GoogleSQL returning BOOL, BYTES, DATE, FLOAT64, INT64, JSON,
NUMERIC, STRING and TIMESTAMP, using BigQuery's
[Spanner mapping](https://cloud.google.com/bigquery/docs/reference/standard-sql/federated_query_functions#spanner-mapping).
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

### Unsupported forms

Persistent JavaScript UDFs, aggregate UDFs, conditional routine updates, `SAFE.` UDF calls,
UDF options other than a persistent UDF's description, the `library` option, and views that call
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
