# bigquery-emulator-duckdb

A BigQuery Emulator using DuckDB as the backend.

## Compatibility and feature support

Supported means implemented within the scope described below; partial means there are known
differences or only a subset is implemented. SQL execution ultimately uses DuckDB semantics,
so accepting GoogleSQL syntax does not guarantee full BigQuery compatibility.

| Feature | Status | Scope and limitations |
| --- | --- | --- |
| `bq` CLI and Go BigQuery client | Supported | Covered by [end-to-end tests](tests/e2e); use the emulator URL as the endpoint override. |
| REST discovery and endpoint paths | Supported | Serves the v2 discovery document; resource routes accept both `/bigquery/v2` and root paths. |
| Query jobs | Partial | `jobs.query`, `jobs.insert`, `jobs.get`, and `jobs.getQueryResults`; jobs complete synchronously. |
| Load, extract, and copy jobs | Unsupported | `jobs.insert` accepts query jobs only. |
| Datasets and tables | Partial | `list`, `get`, `insert`, and `delete`; update and patch methods are not implemented. |
| Table data | Partial | `tabledata.list` and `tabledata.insertAll` are implemented. Streaming inserts accept scalar, repeated, and record fields, report row errors, and support `skipInvalidRows` and `ignoreUnknownValues`. Insert ID deduplication and template suffixes are unsupported. |
| Result pagination | Supported | Query results and table data accept `maxResults`, `startIndex`, and `pageToken`. |
| Query parameters | Supported | Named (`@name`) and positional (`?`) parameters, including ARRAY and STRUCT values. |
| Dry runs | Supported | Validates queries and returns their result schema without executing them. |
| Result schema | Supported | Queries take column names and types from the GoogleSQL analyzer, so anonymous columns are named `f0_`, `f1_`, … and `SUM` over integers reports `INTEGER`. Other statements derive `TableSchema` from DuckDB types: `TIMESTAMPTZ` → `TIMESTAMP`, `TIMESTAMP` → `DATETIME`, lists → `REPEATED`, structs → `RECORD`. |
| Result rows | Supported | BigQuery `{"f": [{"v": ...}]}` encoding, including nested values and base64 bytes. |
| Timestamp encoding | Supported | Epoch seconds by default; epoch microseconds with `formatOptions.useInt64Timestamp`. |
| Projects, datasets, and tables | Partial | Map to DuckDB in-memory catalogs, schemas, and tables; `project.dataset.table` references work, but data is not persisted across server restarts. |

## SQL compatibility

| Feature | Status | Scope and limitations |
| --- | --- | --- |
| GoogleSQL parsing | Partial | Released language features are enabled, including `QUALIFY`. Every statement is analyzed against the tables in DuckDB and the declared parameter types, so unknown names and type errors are rejected as BigQuery would. Queries, `INSERT` / `UPDATE` / `DELETE` / `MERGE` and the DDL below are translated from the resolved AST; statements, scans and expressions outside that subset are rejected as `invalidQuery`. |
| `INSERT` | Partial | `INSERT ... VALUES` (including `DEFAULT`) and `INSERT ... SELECT` with or without a column list are translated from the resolved AST. `INSERT OR IGNORE/REPLACE/UPDATE`, `ASSERT_ROWS_MODIFIED` and `THEN RETURN` are unsupported. |
| `UPDATE`, `DELETE`, `MERGE` | Partial | Translated from the resolved AST, including `UPDATE ... FROM`, `SET col = DEFAULT`, correlated subqueries, and `MERGE` clauses `WHEN MATCHED` / `NOT MATCHED [BY TARGET]` / `NOT MATCHED BY SOURCE` with `UPDATE`, `DELETE`, `INSERT (cols) VALUES` and `INSERT ROW`. `SET s.field = ...` rebuilds the struct with its other fields, and fails on a NULL struct as BigQuery does. Updates of array elements, nested DML, `ASSERT_ROWS_MODIFIED` and `THEN RETURN` are unsupported. Unlike BigQuery, DuckDB does not reject an `UPDATE ... FROM` or `MERGE` in which one target row matches several source rows. |
| `CREATE TABLE`, `CREATE SCHEMA`, `DROP TABLE` / `SCHEMA` | Partial | Translated from the resolved AST, with `table`, `dataset.table` and `dataset` completed from the default project and dataset. `CREATE [OR REPLACE] TABLE [IF NOT EXISTS]` keeps column types, `NOT NULL` and `DEFAULT`, and `NUMERIC(P, S)` becomes `DECIMAL(P, S)`; `STRING(L)` / `BYTES(L)` lose their length, and `BIGNUMERIC` is `DECIMAL(38, 19)`. `CREATE TABLE AS SELECT` casts the query's columns to the declared types. Partitioning, clustering, options and unenforced keys are accepted and dropped. Temporary tables, `LIKE`, `CLONE` / `COPY`, generated columns, collation, `NOT NULL` in `CREATE TABLE AS SELECT`, `CREATE OR REPLACE SCHEMA` and other object types are unsupported. |
| Table reads, `WHERE`, `ORDER BY`, `LIMIT` / `OFFSET` | Supported | Translated from resolved scans, including derived tables built from these scans, hidden sort columns, and BigQuery NULL ordering. |
| `SELECT AS STRUCT` / `AS VALUE` | Supported | A query returning STRUCT values returns their fields as columns, with anonymous fields named `_field_1`, `_field_2`, …; any other value table returns one `f0_` column. |
| Joins, CTEs, scalar/correlated subqueries, aggregates, window functions, and `QUALIFY` | Partial | Translated from resolved scans, including `DISTINCT` window aggregates, recursive CTEs, `GROUPING SETS` / `ROLLUP` / `CUBE`, `GROUPING()`, and `ARRAY_AGG` / `STRING_AGG` `LIMIT` and aggregate `HAVING MAX` / `MIN` modifiers outside grouping sets. Lateral joins are translated too; recursive CTEs `WITH DEPTH` are unsupported. |
| Set operations, `UNNEST`, `STRUCT` and array subscripts | Partial | Translated from resolved scans for `UNION` / `INTERSECT` / `EXCEPT`, including `CORRESPONDING [BY]` with `FULL` / `LEFT` / `INNER` propagation, `UNNEST` with `WITH OFFSET`, including multi-array `UNNEST` in each `mode`, named `STRUCT` fields and `OFFSET` / `ORDINAL` / `SAFE_` subscripts. |
| Identifiers | Supported | Backtick paths such as `` `project.dataset.table` `` become `"project"."dataset"."table"`. |
| String and bytes literals | Supported | Strings are re-quoted from their parsed values; `b'abc'` becomes `from_hex('616263')`, preserving quotes, NUL, and non-UTF-8 bytes. |
| Float literals | Supported | `1.5` becomes `1.5::DOUBLE` to avoid DuckDB inferring `DECIMAL`. |
| Type names and typed literals | Partial | Maps types in expressions, DDL, and typed literals: `INT64` → `BIGINT`, `TIMESTAMP` → `TIMESTAMPTZ`, `DATETIME` → `TIMESTAMP`, `ARRAY<T>` → `T[]`, `STRUCT<a INT64>` → `STRUCT(a BIGINT)`. |
| ARRAY and STRUCT constructors | Supported | Converts `ARRAY<T>[...]` and `STRUCT(...)` constructors to DuckDB syntax, including `struct_pack(a := ...)`. |
| Star modifiers | Supported | `SELECT * EXCEPT` becomes `EXCLUDE`; `REPLACE` keeps its spelling. |
| Parameter substitution | Supported | Uses typed literals such as `CAST('42' AS BIGINT)`, applying the same type and constructor conversions to ARRAY and STRUCT parameters. |
| Built-in functions | Partial | Each BigQuery function's support is listed in [docs/functions.md](docs/functions.md), which a probe of the emulator generates. Calls are renamed or rewritten using rules in [src/functions.cc](src/functions.cc) and the translator; templates cannot take `OVER`, so such calls keep their BigQuery spelling. |
| `SAFE.` function prefix | Partial | In resolved statements, the call runs inside DuckDB's `TRY` with its arguments evaluated outside it, so the function's own errors return NULL while argument errors still propagate. Volatile functions and calls translated to an explicit `error()` are unsupported. |

## Server

The HTTP server handles BigQuery REST requests, passes queries through the frontend,
translator, and backend, and returns BigQuery-compatible responses. It is built on
[cpp-httplib](https://github.com/yhirose/cpp-httplib) with
[nlohmann/json](https://github.com/nlohmann/json).

## Frontend

[GoogleSQL](https://github.com/google/googlesql) parses each query.
The frontend owns GoogleSQL syntax handling and stays independent of DuckDB execution details.

The GoogleSQL analyzer can also resolve a parsed statement into a resolved AST, with every name
bound and every expression typed. [src/catalog.cc](src/catalog.cc) looks tables up lazily
through a `TableSource`, completes `table` and `dataset.table` paths from the default project
and dataset, and maps BigQuery field types to GoogleSQL types; functions and types come from
GoogleSQL's built-ins, plus BigQuery functions GoogleSQL lacks such as `CONTAINS_SUBSTR`.
[src/analyzer.cc](src/analyzer.cc) runs the analyzer with the same language options as the
parser. The emulator analyzes every statement before translating it, with the
tables looked up in DuckDB, and a statement that fails analysis is reported as `invalidQuery`.
For a query, the resolved output columns supply the result schema's names, and its types where
they share a wire encoding with the column DuckDB returns.

## Translator

[src/translator.cc](src/translator.cc) translates queries composed of
SingleRow, Table, Project, Filter, OrderBy, LimitOffset, Join, Aggregate, Analytic, With,
WithRef, Recursive, RecursiveRef, SetOperation and Array scans, INSERT, UPDATE, DELETE and MERGE statements over such
queries, and CREATE TABLE [AS SELECT], CREATE SCHEMA and DROP TABLE / SCHEMA. DDL has no catalog
lookup, so the translator completes its table and dataset paths from the default project and dataset itself. INSERT names its target columns after the table's columns bound to the insert column list.
UPDATE, DELETE and MERGE alias the target table `_t`, so its columns resolve by ID like any other
scan's; an UPDATE's FROM clause and a MERGE's source become the derived table `q`. Each scan becomes a derived table whose columns are named
after resolved column IDs, with user-facing names applied only at the output boundary, so
duplicate or shadowed aliases never collide. A query returning a value table of STRUCTs selects each field with `struct_extract_at`. Sort keys survive projections even when they are
absent from the result, and ORDER BY explicitly implements BigQuery's default NULL ordering.
CTEs become DuckDB CTEs with positional column names; a recursive entry becomes a DuckDB
recursive CTE, whose UNION [ALL] iterates the same way. Grouping sets compute their keys in a
derived table below the aggregation and group by those column names, so ROLLUP, CUBE, nested
GROUPING SETS and GROUP BY a, ROLLUP(b) keep their DuckDB spelling. A set operation matching
columns by name is already aligned by position in the resolved AST, with missing columns padded
with NULLs, so it becomes a positional DuckDB set operation. A lateral join becomes DuckDB's `JOIN LATERAL`, and its right
side refers to the left's columns the way a correlated subquery does. UNNEST becomes a lateral `unnest` with
ordinality; several arrays are instead indexed in step over a `range` sized by the zip mode, and correlated subquery references resolve through the enclosing scan's column names.

Expressions include scalar literals and parameters, casts, arithmetic, comparisons, boolean
operators, CASE/IF/COALESCE, selected scalar functions, STRUCT construction and field access,
array literals and subscripts, and scalar, ARRAY, EXISTS and IN subqueries. Aggregate and
analytic calls cover the common numeric, string, array and navigation functions, with DISTINCT,
ORDER BY and IGNORE NULLS where DuckDB can express them, and window frames. An aggregate `LIMIT`
slices the aggregated list, and `HAVING MAX` / `MIN` filters the aggregate's rows against a
window maximum or minimum over the group. The existing function
renames and templates are reused, adapting resolved enum date parts, interval arguments and
default arguments. BYTE_LENGTH and LENGTH distinguish STRING from BYTES. String, hash and
trigonometric functions, `IEEE_DIVIDE` and `SAFE_ADD` / `SAFE_SUBTRACT` / `SAFE_MULTIPLY` /
`SAFE_NEGATE` (the arithmetic operators under `SAFE.`) are translated as well. `<<` and `>>` shift a 64-bit BIT string, so they drop the bits shifted out and fill with zeros as BigQuery does instead of failing on overflow or extending the sign; `IS [NOT] DISTINCT FROM`, `IN UNNEST`, `ERROR`, `ARRAY_CONCAT` (NULL when any array is NULL), `ARRAY_REVERSE`, `ROUND` with digits and `LPAD` / `RPAD` with the default pad are translated too, while their BYTES overloads and `SPLIT` on BYTES are unsupported. `EXTRACT` (including `DAYOFWEEK`, Sunday-based `WEEK`, `ISOWEEK`, sub-second parts, `AT TIME ZONE` and `EXTRACT(DATE/TIME/DATETIME FROM ...)`), the `DATE` / `DATETIME` / `TIME` / `TIMESTAMP` constructors, `LAST_DAY`, `UNIX_DATE` and `GENERATE_DATE_ARRAY` with a DAY or WEEK step follow BigQuery; `WEEK` and `ISOWEEK` truncation and differences start weeks on Sunday and Monday, and `TIMESTAMP_DIFF`, `TIME_DIFF` and differences below a day count whole units rather than boundaries. `REGEXP_EXTRACT` / `REGEXP_SUBSTR` and `REGEXP_EXTRACT_ALL` with a literal STRING pattern return the capturing group, NULL when nothing matches, and fail on more than one group. `JSON_QUERY` / `JSON_VALUE` and their `_ARRAY` variants, the legacy `JSON_EXTRACT` family, `JSON_TYPE`, `json.field` and `json[...]` access, and the `INT64` / `FLOAT64` / `BOOL` / `STRING` conversions from JSON follow BigQuery for literal JSONPaths built from keys and indexes: malformed JSON strings and JSON nulls give NULL where BigQuery returns NULL, and a conversion fails when the value has another JSON type. Wildcards, `REGEXP_INSTR`, the `LAX_` conversions and the other JSON functions are unsupported. Function and aggregate
results are cast to their resolved types. Existing documented function compatibility limitations
still apply.

The catalog, TypeFactory and analyzer output remain alive through translation. A statement containing
an unsupported node, function, type or modifier fails as `invalidQuery`, naming the construct.
Keeping conversion in this layer leaves the frontend focused on parsing and the backend on
execution. `tests/translator_test.cc` executes supported queries directly against
DuckDB, covering results, types, aliases, ordering and parameterized preparation.

### Functions

[src/functions.cc](src/functions.cc) defines two rule tables keyed by BigQuery function name.
A **rename** replaces only the function name. A **template** rewrites the call using `$n` for
the n-th argument and `#n` for that argument as a lowercase string literal, as required for
DuckDB date parts. For example, `DATE_DIFF(a, b, DAY)` becomes `date_diff('day', b, a)`.

[docs/functions.md](docs/functions.md) lists which BigQuery functions the emulator runs. It is
generated by `//:function_probe`, and `//:functions_md_test` fails when it is stale; run
`just docs` to regenerate it.

## Backend

[DuckDB](https://duckdb.org/) executes translated queries through libduckdb's C++ API.
The backend derives result schemas and encodes rows in the BigQuery wire format described above.

## Usage

### Installation

Get bigquery-emulator-duckdb binary:

```bash
bazelisk build //:bigquery-emulator-duckdb
# -> bazel-bin/bigquery-emulator-duckdb
```

### Run the server

```bash
bazel-bin/bigquery-emulator-duckdb --host 0.0.0.0 --port 9050
```

### Connect from BigQuery client

An example using the `bq` command-line tool to connect to the emulator. `bq` normally obtains credentials from `gcloud`; a dummy `--oauth_access_token` together with `--nouse_google_auth` skips that.

```bash
alias bqe='bq --api http://127.0.0.1:9050 --project_id test --oauth_access_token=dummy --nouse_google_auth'

bqe mk --dataset ds
bqe mk --table ds.users id:INTEGER,name:STRING,created:TIMESTAMP
bqe query --nouse_legacy_sql "INSERT INTO ds.users VALUES (1, 'alice', CURRENT_TIMESTAMP())"
bqe query --nouse_legacy_sql --format=json 'SELECT * FROM `test.ds.users`'
bqe head ds.users
```

`tests/e2e/bq.sh` wraps the same invocation for the end-to-end tests.

### Connect from the Go client library

The endpoint override replaces the whole API base path, so the emulator's URL is enough:

```go
client, err := bigquery.NewClient(ctx, "test",
	option.WithEndpoint("http://127.0.0.1:9050"), option.WithoutAuthentication())

query := client.Query("SELECT @name AS name")
query.Parameters = []bigquery.QueryParameter{{Name: "name", Value: "alice"}}
rows, err := query.Read(ctx)
```

## Development

The project uses C++20 and Bazel. Prefer the standard library for project code.

Nix optionally provides a development shell with tools such as Bazel, compilers, and the `bq` command-line tool.

## Testing

End-to-end tests using the `bq` command-line tool are the primary compatibility tests, because the emulator should behave correctly from the BigQuery client's point of view. These scenarios should be described with [runn](https://github.com/k1LoW/runn), using it to execute `bq` commands against the emulator and verify the observable BigQuery-compatible behavior.

```bash
just e2e            # builds the emulator, starts it on a free port and runs tests/e2e/*.yml
just e2e --verbose  # extra arguments are passed to `runn run`
```

`tests/e2e/goclient` covers the Go client library (`cloud.google.com/go/bigquery`), which drives the API differently from `bq`: it runs parameterised queries, polls jobs, streams rows with `Inserter.Put`, and asks for timestamps as epoch microseconds. It is a Go module of its own, so the first run downloads its dependencies; `runn` starts it like any other scenario.

Translator tests use [GoogleTest](https://github.com/google/googletest). The translator has a small and well-defined boundary, so C++ unit tests should cover query conversion cases such as functions, types, identifiers, literals, and BigQuery-specific syntax before the translated query reaches DuckDB. The backend and the HTTP server have unit tests too (`just test`).

Those tests check what a query is translated *into*. `tests/emulator_test.cc` checks that the translation then runs: it drives `Emulator::RunQuery`, so each case goes through the parser, the translator and DuckDB, and asserts the value BigQuery would return. Every function in `src/functions.cc` is exercised there, which is what tells a correct translation apart from a plausible looking one — `LOG()` means a different logarithm in each dialect and a division by zero a different thing, so a case like that fails on the value rather than on an error. Being linked against the same libduckdb as the emulator, it needs no DuckDB installation of its own, and the whole file runs in a few seconds.

#### Internal structure memo

- HTTP server: cpp-httplib (header-only, in the Bazel Central Registry) + nlohmann/json. Crow was considered but it is not in the BCR and depends on asio.
- `third_party/bigquery/discovery.json` is the discovery document bundled with `bq`, embedded into the binary by a genrule.
