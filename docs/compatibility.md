# Query and API behavior

These limits supplement the generated [SQL](sql.md), [function](functions.md) and
[REST](api.md) support tables. Deliberate differences from BigQuery are listed in the
[README](../README.md#compatibility); Storage behavior is covered in its [guide](storage-api.md).

## Jobs and query analysis

Jobs complete synchronously. Only GoogleSQL is supported. Statements are analyzed against the
existing tables and parameter types; unsupported SQL constructs fail as `invalidQuery`, naming
the construct.

## Tables and views

Table and view expiration is stored through REST `expirationTime` or DDL
`expiration_timestamp` (TIMESTAMP literals or NULL). Expired tables and views are deleted lazily
when the catalog is read, including before query analysis, and are treated as missing. Dataset
default expiration and partition expiration remain unsupported.

Column defaults from DDL `DEFAULT` or a schema's `defaultValueExpression` apply alike to tables
created by DDL, `tables.insert`, and load and copy jobs, and `INFORMATION_SCHEMA.COLUMNS` reports
them as `column_default`. `tables.update` and `tables.patch` cannot add or change a default, the
Storage Write API cannot write to a table with defaults, and `CREATE TABLE COPY` and `CLONE`
cannot copy one.

`ALTER TABLE ... ALTER COLUMN [IF EXISTS] SET DEFAULT` changes the default for future
writes and the column's `defaultValueExpression`. Existing rows are unchanged.
`DROP DEFAULT` removes it and fails if the column has no declared default. Both support
`ALTER TABLE IF EXISTS`; a missing table or column is left alone. Repeated columns whose
default is removed again use an empty array when omitted. Defaults accept literals,
ARRAY/STRUCT constructors and supported forms of BigQuery's documented default functions;
NULL arrays and NULL array elements are rejected. `SET DEFAULT NULL` removes a nullable
column's default. Parameterized columns support literal defaults only, checked against
their constraints; NULL defaults for required columns are rejected.
`ADD COLUMN ... DEFAULT` remains unsupported.

Logical views support `CREATE [OR REPLACE] VIEW`, `CREATE VIEW IF NOT EXISTS`, `DROP VIEW`
and REST table creation, lookup, listing and deletion. Query views with SQL; `tabledata.list`
cannot read them. Temporary, recursive and value-table views, and views that read temporary
tables, are unsupported.

## User-defined functions

### SQL UDFs

`CREATE TEMP FUNCTION` defines a query-local SQL UDF. Persistent SQL UDFs use
`CREATE [OR REPLACE] FUNCTION [IF NOT EXISTS] dataset.name` or `routines.insert`/`routines.update`,
and remain available until `DROP FUNCTION` or `routines.delete`.

Persistent UDF definitions and descriptions are returned by `routines.get` and `routines.list`.
Bodies resolve on each call against the UDF's project: table and UDF names need datasets, and
temporary UDFs cannot be called. `ANY TYPE` bodies resolve for each call's argument types;
temporary UDF bodies resolve against previously declared UDFs.

### JavaScript UDFs

Temporary JavaScript UDFs run in embedded QuickJS. They take BOOL, FLOAT64 and STRING arguments,
named by JavaScript identifiers, and return those or INT64. State is query-local, with a
5-minute call timeout and a 256 MiB allocation limit per query per thread.

### Unsupported forms

Persistent JavaScript UDFs, aggregate UDFs, conditional routine updates, `OR REPLACE`
and `IF NOT EXISTS` for temporary UDFs, `SAFE.` UDF calls, UDF options other than a persistent
UDF's description, and views that call SQL UDFs are unsupported. Subquery bodies do not support
volatile arguments. Queries with TEMP function declarations do not support dry runs,
destination tables or positional parameters.

## Scripts and transactions

Multi-statement queries return the last executed statement's result, create no child jobs and
cannot be dry runs. Temporary tables belong to no dataset and are dropped when the script ends.
`EXECUTE IMMEDIATE` runs one SQL statement, but no scripting statements or transaction control. `CALL` and assignments to system variables are
unsupported, and an error the emulator reports as unsupported is never handled by an
`EXCEPTION` clause.

Transactions support `BEGIN TRANSACTION`, `COMMIT TRANSACTION` and `ROLLBACK TRANSACTION`
within a multi-statement query. Unfinished transactions roll back when the script ends.
Transaction modes, SQL UDF declarations inside transactions, writes to multiple projects
(or both permanent and temporary tables), and committing after a handled error are unsupported;
roll back a failed transaction.

## Writes

`UPDATE ... FROM` rejects multiple source matches for a target row. `MERGE` does so when it
has a matched `UPDATE` clause and a matched action applies. Failed writes leave no changes.

## Result schemas and values

Query result schemas come from the GoogleSQL analyzer, so anonymous columns are named `f0_`,
`f1_`, … and `SUM` over integers reports `INTEGER`. Other statements derive the schema from
DuckDB types: `TIMESTAMPTZ` → `TIMESTAMP`, `TIMESTAMP` → `DATETIME`, lists → `REPEATED`,
structs → `RECORD`, except the structs that keep `RANGE` values → `RANGE`.

Timestamps are encoded as epoch seconds, or epoch microseconds with
`formatOptions.useInt64Timestamp`, and so are the bounds of a `RANGE<TIMESTAMP>`.

The `SAFE.` prefix returns NULL for the function's own errors while argument errors still
propagate. It is unsupported on volatile functions.

## STRUCT values

Query values may have anonymous or duplicate STRUCT field names; stored table columns require
distinct nonempty names. JSON functions preserve the original field names, including in nested
arrays and structs. Comparisons involving anonymous or duplicate fields are unsupported because
DuckDB's composite equality has different NULL semantics. Casts of these values to JSON and
correlated aggregate subqueries returning them are also unsupported; use JSON functions for
JSON conversion.

## Locations

Every dataset is in the `US` location, so the `INFORMATION_SCHEMA` region qualifier
`` `region-us` `` covers every dataset of the project and any other region none.
