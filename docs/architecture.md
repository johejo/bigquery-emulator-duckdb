# Architecture

A request passes through the server, frontend, translator and backend in turn.

## Server

The HTTP server handles BigQuery REST requests and returns BigQuery-compatible responses. It is
built on [cpp-httplib](https://github.com/yhirose/cpp-httplib) with
[nlohmann/json](https://github.com/nlohmann/json). It serves the v2 discovery document in
`third_party/bigquery/discovery.json`, the one bundled with `bq`, and routes requests by it:
[src/server/routes.cc](../src/server/routes.cc) reads each served method's paths, which accept both
`/bigquery/v2` and root paths, and its query parameters. A request with a query parameter the
method does not take, or a value its type does not allow, fails as in BigQuery, and one with a
parameter the method's handler does not handle is rejected as unsupported.

## Frontend

[GoogleSQL](https://github.com/google/googlesql) parses each query and resolves it into a
resolved AST, with every name bound and every expression typed. The frontend stays independent
of DuckDB execution details.

[src/catalog.cc](../src/catalog.cc) looks tables up lazily through a `TableSource`, completes
`table` and `dataset.table` paths from the default project and dataset.
[src/type_mapping.cc](../src/type_mapping.cc) maps column types between BigQuery's
TableFieldSchema, GoogleSQL and DuckDB; every DuckDB column type, of a table, a query parameter
or a translated expression, is derived from the GoogleSQL type. Functions and types come from
GoogleSQL's built-ins, plus BigQuery functions GoogleSQL lacks. [src/information_schema.cc](../src/information_schema.cc) serves
`INFORMATION_SCHEMA` views as tables whose rows are computed when the query is analyzed; the
translator reads such a table from the DuckDB query that carries its rows.

[src/analyzer.cc](../src/analyzer.cc) runs the analyzer with the same language options as the
parser; a statement that fails analysis is reported as `invalidQuery`. For a query, the resolved
output columns supply the result schema.

A request with more than one statement, or with a procedural statement such as `DECLARE` or
`IF`, is a multi-statement query. GoogleSQL's `ScriptExecutor` runs its control flow and keeps
its variables, and [src/emulator_script.cc](../src/emulator_script.cc) evaluates each statement
and expression the executor reaches: it analyzes them against a catalog that holds the variables
as constants ahead of the tables, translates them like any other statement, and decodes the
value of an expression from the row DuckDB returns.

## Translator

[src/translator.cc](../src/translator.cc) translates the resolved AST into DuckDB SQL. Each scan
becomes a derived table whose columns are named after resolved column IDs, and user-facing names
are applied only at the output boundary, so duplicate or shadowed aliases never collide. Where
DuckDB differs from BigQuery, the translator spells out BigQuery's semantics, such as default
NULL ordering and result types.

The translator's sources in [src/translator/](../src/translator) share
[internal.h](../src/translator/internal.h): a `Context` for the whole statement, and the `Scope`
of what a scan can see besides its input. Expressions, scalar functions, scans, aggregation, DML,
DDL, and types and literals each have a file of their own.

[src/translator/functions.cc](../src/translator/functions.cc) is the registry of the functions the
emulator supports, each under one implementation: a DuckDB function of the same or another name,
DuckDB SQL templates, GoogleSQL's own implementation, code in
[src/translator/function.cc](../src/translator/function.cc), or a DuckDB aggregate or window
function. A **template** rewrites the call using `$n` for the n-th argument and `#n` for that
argument as a lowercase string literal, so `DATE_DIFF(a, b, DAY)` becomes
`date_diff('day', b, a)`; the rules are chosen by argument count, types, date parts, rounding
modes and STRING literal values. AGENTS.md says which implementation to choose.

## Backend

[DuckDB](https://duckdb.org/) executes translated statements through libduckdb's C++ API; each
project is a DuckDB catalog and each dataset a schema. The backend derives result schemas and
encodes rows in BigQuery's `{"f": [{"v": ...}]}` format. Functions DuckDB lacks are registered
from GoogleSQL's own implementations in [src/backend_functions/](../src/backend_functions/), one
source per area.
GCS load jobs download objects with
[google-cloud-cpp](https://github.com/googleapis/google-cloud-cpp) to temporary files, which
DuckDB then reads.

Logical views are native DuckDB views with translated SQL and explicit GoogleSQL output types.
Their DuckDB comments hold the original GoogleSQL query and result schema as JSON. Creation and
metadata writes share a transaction, so replacement failures preserve the previous definition.
This metadata survives restarts and serves the REST API without executing the view.

BigQuery metadata that DuckDB types cannot carry lives in DuckDB comments as JSON: each column's
TableFieldSchema in its column comment, and a table's description, friendly name, labels,
partitioning and clustering in its table comment, or in its view comment next to the query. DuckDB
cannot comment on a schema, so a dataset's description, friendly name and labels live as JSON in
the `emulator_datasets` table of the project's `main` schema, which is not a dataset; the same
transaction that creates or drops the schema writes them.
