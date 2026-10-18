# Query and API behavior

This page describes behavior that the generated [SQL](sql.md), [function](functions.md) and
[REST](api.md) tables cannot show, since they check only that a form runs. Deliberate differences
from BigQuery are listed in the [README](../README.md#compatibility), and Storage API behavior in
its [guide](storage-api.md).

## Jobs

Jobs finish before `jobs.insert` or `jobs.query` returns, so `jobs.cancel` returns the finished job,
and `jobs.list` returns no jobs for a `stateFilter` of `pending` or `running`. Only GoogleSQL is
supported. Statements are analyzed against the existing tables and parameter types; unsupported SQL
constructs fail as `invalidQuery`, naming the construct.

Multi-statement queries create no child jobs: `jobs.list` leaves out the ones BigQuery lists and
rejects the `parentJobId` of a multi-statement query.

### Locations

A job runs in the location its request names, `location` for `jobs.query` and
`jobReference.location` for `jobs.insert`. Otherwise it runs where the datasets and
`INFORMATION_SCHEMA` regions its query names are, as BigQuery infers it before running the query,
and in the `US` when they are in several locations or it names none; `INFORMATION_SCHEMA.SCHEMATA`
without a region qualifier is the `US` region's. A query that names none, such as one built by
`EXECUTE IMMEDIATE`, runs where the first dataset or connection it uses is. Every dataset a job
reads or writes must be in that location: a job that uses one elsewhere fails with `notFound`, and
the location that `CREATE SCHEMA` names must be the job's too. Copies to a dataset in another
location, which BigQuery supports, are unsupported. `jobs.get`, `jobs.getQueryResults`,
`jobs.cancel` and `jobs.delete` find a job outside the `US` and `EU` multi-regions only when the
request names its location. A view created with `tables.insert` reads only tables in its dataset's
location.

### Loads, extracts and copies

Loads and extracts also accept local paths. Loads read CSV, newline-delimited JSON and Parquet. A
Parquet DECIMAL wider than 38 digits loads only into BIGNUMERIC, which schema detection picks only
when `decimalTargetTypes` includes it. Parquet LIST columns load only with
`parquetOptions.enableListInference`, and not when the list's repeated group is named `array` or
`<list>_tuple` and holds one field. Query and load jobs cannot partition or cluster their
destination table, and loads reject `destinationTableProperties`.

Extracts write the whole table to one file and reject INTERVAL, RANGE and GEOGRAPHY columns. They write a
BIGNUMERIC to Parquet as a `DECIMAL(76, 38)` of `BYTE_ARRAY` rather than `FIXED_LEN_BYTE_ARRAY`.

## Datasets

A dataset is in the location that `datasets.insert` or `CREATE SCHEMA` names, the `US` unless
`datasets.insert` names one, and the [job's location](#locations) unless `CREATE SCHEMA` does. The
locations are those BigQuery lists for datasets, compared without case; BigQuery Omni locations are
unsupported. A dataset cannot move: `ALTER SCHEMA SET OPTIONS` rejects a location as BigQuery does,
and `datasets.patch` and `datasets.update` reject another location as unsupported. A table is in its dataset's location, and the
`INFORMATION_SCHEMA` region qualifier `` `region-REGION` ``, whose `region-` is in lower case,
covers the datasets of the project in that location.

Datasets keep a description, friendly name, labels, default table expiration and [default rounding
mode](#rounding-modes), which `CREATE SCHEMA`, `ALTER SCHEMA SET OPTIONS` and the `datasets` methods set; other options and fields, such
as default partition expiration and access controls, are unsupported. A table or view created in a
dataset with a default table expiration, by DDL, `tables.insert` or a job that creates its
destination, expires that long after its creation time unless it sets an expiration of its own.
Changing the default leaves existing tables as they are, and a job that replaces an existing table
with `WRITE_TRUNCATE` does not apply it. Temporary tables of scripts do not take it either.
`default_table_expiration_days` is rounded to milliseconds. `INFORMATION_SCHEMA.SCHEMATA` returns NULL last modified times
and NULL for options other than the location. Datasets created before the emulator recorded
creation times have none: `datasets.get` omits `creationTime`, and `SCHEMATA` returns NULL.

## Tables

### Options and expiration

Tables keep a description, friendly name, labels, expiration and [default rounding
mode](#rounding-modes), and columns a description and a rounding mode, which DDL `OPTIONS` and the
`tables` methods set; other options, such as partition expiration, are unsupported. Expiration comes from
REST `expirationTime`, DDL `expiration_timestamp`, or the `destinationExpirationTime` of a copy job
that creates its destination; a copy job with one to an existing table is rejected. Tables and
views created before the emulator recorded creation times have none: `tables.get` omits
`creationTime`, and `INFORMATION_SCHEMA.TABLES` returns NULL.

A DDL option's value is a literal or a constant expression of query parameters and the supported
scalar functions, which the backend evaluates when the statement runs; the functions BigQuery
excludes, such as `REPEAT` and `RAND`, fail, and other expressions, such as a parenthesized script
variable, are unsupported. As in BigQuery, a STRING literal converts to the TIMESTAMP of
`expiration_timestamp`, and a bare identifier that names a script variable is an error. Expired tables and views are deleted lazily when the catalog is read,
including before query analysis, and are treated as missing.

### Partitioning and clustering

Tables keep partitioning on a column or an integer range, and clustering. Ingestion-time
partitioning (`_PARTITIONTIME`, `_PARTITIONDATE`) is unsupported, and partitions do not expire.

### Rounding modes

NUMERIC and BIGNUMERIC columns and STRUCT fields keep the rounding mode that DDL `rounding_mode` or
a schema's `roundingMode` sets, which `tables.get` and the `rounding_mode` of
`INFORMATION_SCHEMA.COLUMNS` and `COLUMN_FIELD_PATHS` return; `ROUNDING_MODE_UNSPECIFIED` leaves it
unset. A rounding mode on another type is unsupported.

Tables and datasets keep a default rounding mode, from DDL `default_rounding_mode` or a resource's
`defaultRoundingMode`. A table created in a dataset, by DDL, `tables.insert` or a query or load job
that creates its destination, takes the dataset's default unless it sets its own; temporary tables
do not. The NUMERIC and BIGNUMERIC fields that creation, `ALTER TABLE ADD COLUMN` or a schema update
adds without a rounding mode take the table's default, which `tables.get` and `INFORMATION_SCHEMA`
then report as theirs; changing a default later keeps the modes of existing fields and tables.
Views cannot have a default.

Copies keep the rounding modes of the source's fields. `CREATE TABLE LIKE`, `COPY` and `CLONE` keep
the source's default unless their `OPTIONS` set another; in a dataset with a default, `LIKE` takes
it when the source has none, while `COPY` and `CLONE` keep the source's unset default. A table a
copy job creates takes no default, neither the source's nor the dataset's. A job that replaces a
table with `WRITE_TRUNCATE` keeps its default, which the new fields do not take, and appending
keeps it too.

A `tables.patch` or `tables.update` that changes the default and adds NUMERIC or BIGNUMERIC fields
is rejected, since which default the fields take is not known.

DML, `CREATE TABLE AS SELECT`, query and load jobs that append to an existing table, and
`tabledata.insertAll` round the values they write to a parameterized column's scale with its rounding mode,
`ROUND_HALF_AWAY_FROM_ZERO` when unset, and fail on a value out of its range. They round
parameterized `ROUND_HALF_EVEN` fields within STRUCTs and ARRAYs too, keeping NULL STRUCTs and
converting NULL arrays containing those fields to empty arrays. NULL elements in those arrays are
rejected. Other writes round halfway values away from zero, so they reject `ROUND_HALF_EVEN`
columns: loads that create a table or replace one with `WRITE_TRUNCATE`, since BigQuery rounds
2.25 to 2.3 in a field that takes `ROUND_HALF_EVEN` from the dataset's default, copy jobs that
append to a table of another schema, which BigQuery rejects, and the Storage Write API. Defaults on parameterized `ROUND_HALF_EVEN` columns are also unsupported.

### Column defaults

Column defaults from DDL `DEFAULT` or a schema's `defaultValueExpression` apply alike to tables
created by DDL, `tables.insert`, and load and copy jobs, and `INFORMATION_SCHEMA.COLUMNS` reports
them as `column_default`. Defaults accept literals, ARRAY and STRUCT constructors and the supported
forms of BigQuery's documented default functions; NULL arrays and NULL array elements are rejected.
Parameterized columns accept literal defaults only, checked against their constraints.
`tables.update` and `tables.patch` cannot add or change a default, and the Storage Write API cannot
write to a table with defaults.

### ALTER TABLE

`ALTER TABLE` supports `ADD COLUMN`, `DROP COLUMN`, `RENAME TO`, `SET OPTIONS` and `ALTER COLUMN`
with `SET OPTIONS`, `SET DEFAULT` or `DROP DEFAULT`, with `IF EXISTS` and `IF NOT EXISTS`. One
statement may list several `ADD COLUMN`, `DROP COLUMN` or `ALTER COLUMN SET OPTIONS` actions. `ALTER
COLUMN SET OPTIONS` sets the column's description and rounding mode only. `ADD COLUMN` with a default or with
top-level `NOT NULL`, and other actions, are unsupported.

`SET DEFAULT` and `DROP DEFAULT` change the default for future writes and the column's
`defaultValueExpression`. `SET DEFAULT NULL` removes a nullable column's default, and a NULL default
for a required column is rejected. `DROP DEFAULT` fails if the column has no default.

### Copies and clones

`CREATE TABLE LIKE` copies a table's schema, partitioning, clustering, description, friendly name
and labels; with `AS SELECT`, it fills the new table with the query's rows. `CREATE TABLE COPY` also
copies the rows, and `CREATE TABLE CLONE` also records the table it was cloned from and when, which
`tables.get` and `INFORMATION_SCHEMA.TABLES` return. All three keep the source's column defaults,
and reject a view as BigQuery does. A temporary table can be made only from a temporary table, as
BigQuery requires.

### Schema changes

`tables.patch` and `tables.update` change the schema, view definition, description, friendly name,
labels, clustering and expiration, and ignore other fields. Neither partitioning nor a view's schema
can change. `tables.update` keeps a schema or view definition that the request leaves out rather
than clearing it.

### Views

Logical views keep their GoogleSQL definition, schema, description, friendly name, labels and
expiration; other view options are unsupported. Temporary, recursive and value-table views, and
views that read temporary tables, are unsupported.

## Types

### STRUCT values

Query values may have anonymous or duplicate STRUCT field names; stored table columns require
distinct nonempty names. A STRUCT without fields, `STRUCT<>`, can only be passed to JSON functions,
such as `TO_JSON` and `JSON_ARRAY_APPEND`, and to ARRAY constructors; aggregates and other functions
reject it. As in BigQuery, a query result cannot contain one at any depth, and `SELECT AS STRUCT`
and `SELECT AS VALUE` results cannot have anonymous fields. Casts between STRUCT types match fields by position,
and JSON functions preserve the original field names, including in nested arrays and structs.
Equality, `IN`, `CASE` and `NULLIF` compare STRUCTs field by field by position, with BigQuery's
NULL semantics. `IS DISTINCT FROM` with anonymous or duplicate fields is unsupported, as are casts
of these values to JSON and correlated aggregate subqueries returning them; use JSON functions for
JSON conversion. As in BigQuery, ARRAYs, and STRUCTs that contain them, can be grouped but
cannot be compared, ordered or passed to aggregate functions with DISTINCT. BigQuery also rejects
STRUCT partition keys and STRUCT columns in `UNION DISTINCT`, `INTERSECT DISTINCT` and
`EXCEPT DISTINCT`; the emulator rejects them too. `INTERSECT ALL` and `EXCEPT ALL` are also rejected,
as BigQuery only supports `DISTINCT` for those operations.

### INTERVAL values

INTERVAL values keep separate month, day and microsecond components. Literals, constructors, query
parameters, `JUSTIFY` functions, `EXTRACT`, casts to and from STRING, `TO_JSON` and `TO_JSON_STRING`,
which encode them as ISO 8601 durations, `SUM` and `AVG`, and `+` with INTERVAL operands are
supported. `+` adds an INTERVAL's months, days and time in turn, as GoogleSQL does, and fails where
a step leaves the range, also when the result is within it; TIMESTAMP values take no months or
years. Tables store INTERVAL columns, also in ARRAYs and STRUCTs.
`tabledata.insertAll` rows give them as text in the canonical form, in full or in part, as query
parameters do, and CSV loads read only the full `Y-M D H:M:S[.F]` form BigQuery documents.
Comparisons, `IN`, `BETWEEN`, `ORDER BY`, `GROUP BY`, also with grouping sets, `DISTINCT`, `PARTITION BY`,
`UNION`, `INTERSECT` and `EXCEPT` with `DISTINCT`, `COUNT`, `MIN` and `MAX`, and `SUM` and `AVG` with
`DISTINCT` compare lengths with 30 days to a month and 24 hours to a day, as GoogleSQL does, so `INTERVAL 1 MONTH` equals
`INTERVAL 30 DAY`. STRUCT comparisons compare their INTERVAL fields the same way, and `GROUP BY` and
`DISTINCT` group STRUCT and ARRAY values by the lengths of their INTERVAL fields and elements. Among equal
values, as in BigQuery, a group, a distinct row, `MIN` and `MAX` return the first in input order, grouping
sets the first in the whole input, and `SUM` and `AVG` with `DISTINCT` count the first. `GREATEST`, `LEAST`
and `NULLIF` fail as in BigQuery. `PARTITION BY` and distinct set operations over ARRAY values with
INTERVAL elements, other arithmetic, other aggregates, other JSON conversions, loads of
INTERVAL columns from other formats, extracts of them, and reading them through the Storage API are
unsupported.

### RANGE values

`RANGE<DATE>`, `RANGE<DATETIME>` and `RANGE<TIMESTAMP>` values are supported in queries, stored
columns, query parameters and `tabledata.insertAll` rows, with comparisons, ordering, grouping,
`COUNT`, `MIN`, `MAX`, `ANY_VALUE`, `ARRAY_AGG`, navigation functions and casts to and from STRING,
including `SAFE_CAST`. `RANGE_BUCKET`, `APPROX_COUNT_DISTINCT`, `APPROX_TOP_COUNT` and
`APPROX_TOP_SUM` fail over RANGE values, as in BigQuery. `TO_JSON`, `TO_JSON_STRING` and the JSON functions that take values of any
type encode a RANGE as an object of its `start` and `end`, with `null` for an unbounded end.
`FORMAT` writes a RANGE with `%t` and `%T`, including one inside an ARRAY or STRUCT. CSV
loads read a RANGE column as BigQuery's `[start, end)` text, with `NULL` or `UNBOUNDED` for an
unbounded end, and [Storage Read sessions](storage-api.md#reading-rows) return RANGE columns. Other
functions over RANGE values, loads of RANGE columns from other formats, and extracts of RANGE
columns are unsupported.

### GEOGRAPHY values

Queries hold a GEOGRAPHY as the S2 points of its vertices, as BigQuery does, and return or store
it as the WKT BigQuery does, in 15 significant digits. Points, linestrings and empty geographies are
supported: from `ST_GEOGPOINT`, `ST_GEOGPOINTFROMGEOHASH`, `ST_MAKELINE` and `ST_GEOGFROMTEXT` of a
`POINT`, a `LINESTRING` or a geometry type followed by `EMPTY`. All empty geometries return as
`GEOMETRYCOLLECTION EMPTY`. Polygons, multipart geometries and collections are unsupported.
Functions over them use the [S2 Geometry](https://s2geometry.io/) library; distances use S2's
Earth radius of 6,371,010 meters, which BigQuery's distances match.
`ST_DISTANCE` with `use_spheroid` `TRUE` is unsupported for nonempty inputs. Stored GEOGRAPHY columns read as the STRING they hold, and
GEOGRAPHY query parameters are unsupported.

`ST_X` and `ST_Y` give a vertex's coordinates as S2 converts them back from its point.
`ST_GEOGFROMTEXT` and `ST_MAKELINE` build a linestring as BigQuery does
with S2Builder: vertices within a micrometer snap together, a line that folds back merges with
itself, crossing edges gain the vertex where they cross, and a line that snaps to one point is a
`POINT`. An edge longer than 135 degrees gains its midpoint, as BigQuery's do. A line that
BigQuery splits into a `MULTILINESTRING`, such as one that overlaps itself, is unsupported.
`ST_EQUALS` takes geographies as equal where each point of either is within a
micrometer of the other. `ST_ASGEOJSON` of a linestring is unsupported. `ST_MAXDISTANCE` measures
over edges, as BigQuery does, though its documentation describes vertices.

### RANGE_SESSIONIZE

`RANGE_SESSIONIZE` retains every input row and appends `session_range` for DATE, DATETIME and
TIMESTAMP ranges. The default `MEETS` mode merges touching or overlapping ranges; `OVERLAPS`
merges only overlapping ranges. Partitioning columns separate sessions, and an empty list
sessionizes the whole input. Unbounded ends are supported. A NULL range retains its input row
and receives a NULL `session_range`. Mode names are case sensitive.

The range column, partitioning columns and sessionize option must be non-NULL literals.
Value tables and INTERVAL or annotated partitioning columns are unsupported. An existing
`session_range` column is replaced with the computed session range.

### EXTERNAL_QUERY

`EXTERNAL_QUERY` reads from PostgreSQL databases that `--connection` names, standing in for Cloud
SQL for PostgreSQL connections; the emulator has no Connection API, and connections to MySQL or
Spanner are not supported. `--connection PROJECT.LOCATION.CONNECTION=CONNINFO` takes a libpq
connection string or URI; a query names the connection by any form BigQuery accepts, the location
in any case. The emulator prepares the query on the database when it analyzes the statement, so
the connection ID, query and options must be literals, and it attaches the database to DuckDB,
read-only, the first time a query reaches it. DuckDB's
[postgres_scanner](https://github.com/duckdb/duckdb-postgres) extension reads the results.

Columns take the BigQuery types of BigQuery's PostgreSQL mapping, and the types it lists as
unsupported fail with BigQuery's error. `name`, which `information_schema` uses, is read as
STRING. Other types, including `bit` and `bit varying`, which BigQuery reads as BYTES, are
unsupported, as are columns of the same name. Dates and times outside BigQuery's ranges, such as
PostgreSQL's `infinity`, fail when read. Of the options, only `default_type_for_decimal_columns`
is supported. Views that read `EXTERNAL_QUERY` are unsupported, since they would outlive the
attachment.

## User-defined functions

### SQL UDFs

`CREATE TEMP FUNCTION` defines a query-local SQL UDF. Persistent SQL UDFs use `CREATE [OR REPLACE]
FUNCTION [IF NOT EXISTS] dataset.name` or `routines.insert`/`routines.update`, and remain available
until `DROP FUNCTION` or `routines.delete`. Each query that calls a SQL UDF inlines its body.

Persistent UDF definitions and descriptions are returned by `routines.get` and `routines.list`.
Bodies resolve on each call against the UDF's project: table and UDF names need datasets, and
temporary UDFs cannot be called. `ANY TYPE` bodies resolve for each call's argument types; temporary
UDF bodies resolve against previously declared UDFs.

### JavaScript UDFs

Temporary JavaScript UDFs run in embedded QuickJS. They take BOOL, FLOAT64 and STRING arguments,
named by JavaScript identifiers, and return those or INT64. State is query-local, with a 5-minute
call timeout and a 256 MiB allocation limit per query per thread; see
[JavaScript UDF limits](../README.md#javascript-udf-limits) to change them.

### Unsupported forms

Persistent JavaScript UDFs, aggregate UDFs, conditional routine updates, `OR REPLACE` and `IF NOT
EXISTS` for temporary UDFs, `SAFE.` UDF calls, UDF options other than a persistent UDF's
description, the `library` option, and views that call SQL UDFs are unsupported. Subquery bodies do
not support volatile arguments. Queries with TEMP function declarations do not support dry runs,
destination tables or positional parameters.

## Scripts and transactions

Multi-statement queries cannot be dry runs. Temporary tables belong to no dataset and are dropped
when the script ends. `EXECUTE IMMEDIATE` runs one SQL statement, but no scripting statements or
transaction control. Scripts can set and read `@@dataset_id` and `@@dataset_project_id` to change
name resolution for subsequent statements, including dynamic SQL. Setting `@@dataset_project_id`
to `NULL` uses the query-executing project; setting `@@dataset_id` to `NULL` clears the default
dataset. Both initially return `NULL` without a request default dataset. These settings belong
to the current script. With a request default dataset, reading either variable before assigning
it is unsupported until its initial value is verified against BigQuery.
As in BigQuery, function and view definitions cannot reference system variables.
Assignments to other system variables are unsupported. An `EXCEPTION` clause never handles an
error that the emulator reports as unsupported.

Transactions support `BEGIN TRANSACTION`, `COMMIT TRANSACTION` and `ROLLBACK TRANSACTION` within a
multi-statement query. Unfinished transactions roll back when the script ends. Transaction modes,
SQL UDF declarations inside transactions, writes to multiple projects (or both permanent and
temporary tables), and committing after a handled error are unsupported; roll back a failed
transaction.

## Stored procedures

SQL procedures support `CREATE PROCEDURE`, `CREATE OR REPLACE PROCEDURE`, `CREATE PROCEDURE IF NOT
EXISTS`, `DROP PROCEDURE`, and `CALL`, with fixed-type `IN` arguments. Definitions must explicitly
set `OPTIONS(strict_mode = FALSE)` (or `strictMode: false` in the Routine API): only syntax is
checked at creation, and statements resolve when called. Strict mode's default validation,
`OUT` and `INOUT` arguments, non-SQL procedures, and options other than `strict_mode` and
`description` are unsupported. Procedure names in DDL must include their dataset.

The Routine API stores and returns the same definitions as DDL, including descriptions and
argument modes. Procedure bodies resolve dataset-qualified references against the procedure's
project. Permanent tables must include their datasets: the caller's default dataset does not
apply inside a procedure. The caller's `@@dataset_id` value can still be read from the body.
Temporary tables and temporary UDFs are shared with the calling script. Local variables and
`IN` argument assignments stay within the procedure, and caller variables are not visible in
the body. Query parameters, function declarations, system-variable assignments, reads of system
variables other than `@@dataset_id`, and dynamic SQL inside procedures are unsupported.
`EXECUTE IMMEDIATE` cannot execute a `CALL`.
CALL queries do not support dry runs, destination tables or positional query parameters.

## Approximate aggregates

`APPROX_TOP_SUM` computes exact sums, collecting the input rows before grouping them, so its memory
use grows with the input size rather than with the requested number of results.

## Result schemas and values

Query result schemas come from the GoogleSQL analyzer, so anonymous columns are named `f0_`, `f1_`,
… and `SUM` over integers reports `INTEGER`. Other statements derive the schema from DuckDB types:
`TIMESTAMPTZ` → `TIMESTAMP`, `TIMESTAMP` → `DATETIME`, lists → `REPEATED`, structs → `RECORD`,
except the structs that keep `RANGE` values → `RANGE`. A table another DuckDB client wrote may hold
types BigQuery does not have: a `DECIMAL` that fits `NUMERIC` reads as `NUMERIC`, `TIMESTAMP_NS` as
a `DATETIME` truncated to microseconds, fixed-size arrays as `REPEATED`, and types such as `ENUM`,
`UUID`, `BIT`, `MAP` and `UNION` as a `STRING` in DuckDB's text form. A table with a column, or a
field at any depth, of `UBIGINT`, `HUGEINT`, `UHUGEINT` or a wider `DECIMAL` cannot be read or
queried: `BIGNUMERIC` would hold their values, but the emulator does not convert them to how it
stores one.

Without a destination table, a second column with the same name as another is renamed with `_1`,
as `SELECT 1 AS a, 2 AS a` returns `a` and `a_1`; names that differ only in case are duplicates, as
`SELECT 1 AS a, 2 AS A` returns `a` and `A_1`. BigQuery does not document how it renames a name
repeated more than twice or a column that would collide with a renamed one, so the emulator rejects
those results. In a `SELECT AS STRUCT` or `SELECT AS VALUE`
result, a field whose name repeats an earlier one's, ignoring case, is named by its position, such
as `_field_2`; a name repeated more than twice, or a positional name another field already has, is
rejected.

Timestamps are encoded as epoch seconds, or epoch microseconds with
`formatOptions.useInt64Timestamp`, and so are the bounds of a `RANGE<TIMESTAMP>`. As in BigQuery,
seconds are a `FLOAT64` in Java's notation, such as `1.57793424525E9`, while the bounds of a range
have six fractional digits.

The `SAFE.` prefix is unsupported on volatile functions.
