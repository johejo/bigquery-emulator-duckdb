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

### Loads, extracts and copies

Loads and extracts also accept local paths. Loads read CSV, newline-delimited JSON and Parquet. A
Parquet DECIMAL wider than 38 digits loads only into BIGNUMERIC, which schema detection picks only
when `decimalTargetTypes` includes it. Query and load jobs cannot partition or cluster their
destination table, and loads reject `destinationTableProperties`.

Extracts write the whole table to one file and reject INTERVAL and GEOGRAPHY columns. They write a
BIGNUMERIC to Parquet as a `DECIMAL(76, 38)` of `BYTE_ARRAY` rather than `FIXED_LEN_BYTE_ARRAY`.

## Datasets

Every dataset is in the `US` location, so the `INFORMATION_SCHEMA` region qualifier `` `region-us`
`` covers every dataset of the project, and any other region none.

Datasets keep a description, friendly name, labels and default table expiration, which `CREATE
SCHEMA`, `ALTER SCHEMA SET OPTIONS` and the `datasets` methods set; other options and fields, such
as default partition expiration and access controls, are unsupported. A table or view created in a
dataset with a default table expiration, by DDL, `tables.insert` or a job that creates its
destination, expires that long after its creation time unless it sets an expiration of its own.
Changing the default leaves existing tables as they are, and a job that replaces an existing table
with `WRITE_TRUNCATE` does not apply it. Temporary tables of scripts do not take it either.
`default_table_expiration_days` is rounded to milliseconds. `INFORMATION_SCHEMA.SCHEMATA` returns NULL creation times and
NULL for options other than the location.

## Tables

### Options and expiration

Tables keep a description, friendly name, labels and expiration, and columns a description, which
DDL `OPTIONS` and the `tables` methods set; other options are unsupported. Expiration comes from
REST `expirationTime`, DDL `expiration_timestamp` (TIMESTAMP literals or NULL), or the
`destinationExpirationTime` of a copy job that creates its destination; a copy job with one to an
existing table is rejected. Expired tables and views are deleted lazily when the catalog is read,
including before query analysis, and are treated as missing.

### Partitioning and clustering

Tables keep partitioning on a column or an integer range, and clustering. Ingestion-time
partitioning (`_PARTITIONTIME`, `_PARTITIONDATE`) is unsupported, and partitions do not expire.

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
COLUMN SET OPTIONS` sets the column's description only. `ADD COLUMN` with a default or with
top-level `NOT NULL`, and other actions, are unsupported.

`SET DEFAULT` and `DROP DEFAULT` change the default for future writes and the column's
`defaultValueExpression`. `SET DEFAULT NULL` removes a nullable column's default, and a NULL default
for a required column is rejected. `DROP DEFAULT` fails if the column has no default.

### Copies and clones

`CREATE TABLE LIKE` copies a table's schema, partitioning, clustering, description, friendly name
and labels; with `AS SELECT`, it fills the new table with the query's rows. `CREATE TABLE COPY` also
copies the rows, and `CREATE TABLE CLONE` also records the table it was cloned from and when, which
`tables.get` and `INFORMATION_SCHEMA.TABLES` return. None of them copies a view or a table with
column defaults.

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

Query values may have anonymous or duplicate STRUCT field names, but at least one field; stored
table columns require distinct nonempty names. Casts between STRUCT types match fields by position,
and JSON functions preserve the original field names, including in nested arrays and structs.
Equality, `IN`, `CASE` and `NULLIF` compare STRUCTs field by field by position, with BigQuery's
NULL semantics. `IS DISTINCT FROM` with anonymous or duplicate fields is unsupported, as are casts
of these values to JSON and correlated aggregate subqueries returning them; use JSON functions for
JSON conversion. As in BigQuery, ARRAYs, and STRUCTs that contain them, can be grouped but
cannot be compared, ordered or passed to aggregate functions with DISTINCT.

### INTERVAL values

INTERVAL values keep separate month, day and microsecond components. Literals, constructors,
`JUSTIFY` functions, `EXTRACT` and casts to STRING are supported. Stored columns, query parameters,
casts from STRING, arithmetic, comparisons, ordering, grouping, aggregates and JSON conversion are
unsupported.

### RANGE values

`RANGE<DATE>`, `RANGE<DATETIME>` and `RANGE<TIMESTAMP>` values are supported in queries, stored
columns, query parameters and `tabledata.insertAll` rows, with comparisons, ordering, grouping,
`COUNT`, `MIN`, `MAX`, `ANY_VALUE`, `ARRAY_AGG`, navigation functions and casts to and from STRING.
`SAFE_CAST` from STRING, JSON conversion, `FORMAT`, other functions over RANGE values, and loads and
extracts of RANGE columns are unsupported.

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
call timeout and a 256 MiB allocation limit per query per thread.

### Unsupported forms

Persistent JavaScript UDFs, aggregate UDFs, conditional routine updates, `OR REPLACE` and `IF NOT
EXISTS` for temporary UDFs, `SAFE.` UDF calls, UDF options other than a persistent UDF's
description, the `library` option, and views that call SQL UDFs are unsupported. Subquery bodies do
not support volatile arguments. Queries with TEMP function declarations do not support dry runs,
destination tables or positional parameters.

## Scripts and transactions

Multi-statement queries cannot be dry runs. Temporary tables belong to no dataset and are dropped
when the script ends. `EXECUTE IMMEDIATE` runs one SQL statement, but no scripting statements or
transaction control. `CALL` and assignments to system variables are unsupported, and an `EXCEPTION`
clause never handles an error that the emulator reports as unsupported.

Transactions support `BEGIN TRANSACTION`, `COMMIT TRANSACTION` and `ROLLBACK TRANSACTION` within a
multi-statement query. Unfinished transactions roll back when the script ends. Transaction modes,
SQL UDF declarations inside transactions, writes to multiple projects (or both permanent and
temporary tables), and committing after a handled error are unsupported; roll back a failed
transaction.

## Approximate aggregates

`APPROX_TOP_SUM` computes exact sums, collecting the input rows before grouping them, so its memory
use grows with the input size rather than with the requested number of results.

## Result schemas and values

Query result schemas come from the GoogleSQL analyzer, so anonymous columns are named `f0_`, `f1_`,
… and `SUM` over integers reports `INTEGER`. Other statements derive the schema from DuckDB types:
`TIMESTAMPTZ` → `TIMESTAMP`, `TIMESTAMP` → `DATETIME`, lists → `REPEATED`, structs → `RECORD`,
except the structs that keep `RANGE` values → `RANGE`.

Without a destination table, a second column with the same name as another is renamed with `_1`,
as `SELECT 1 AS a, 2 AS a` returns `a` and `a_1`. BigQuery does not document how it renames a name
repeated more than twice, names that differ only in case, or a column that would collide with a
renamed one, so the emulator rejects those results.

Timestamps are encoded as epoch seconds, or epoch microseconds with
`formatOptions.useInt64Timestamp`, and so are the bounds of a `RANGE<TIMESTAMP>`.

The `SAFE.` prefix is unsupported on volatile functions.