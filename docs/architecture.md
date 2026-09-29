# Architecture

A request passes through the server, frontend, translator and backend in turn.

## Server

The HTTP server handles BigQuery REST requests and returns BigQuery-compatible responses. It is
built on [cpp-httplib](https://github.com/yhirose/cpp-httplib) with
[nlohmann/json](https://github.com/nlohmann/json). It serves the v2 discovery document in
`third_party/bigquery/discovery.json`, the one bundled with `bq`, and resource routes accept both
`/bigquery/v2` and root paths.

## Frontend

[GoogleSQL](https://github.com/google/googlesql) parses each query and resolves it into a
resolved AST, with every name bound and every expression typed. The frontend stays independent
of DuckDB execution details.

[src/catalog.cc](../src/catalog.cc) looks tables up lazily through a `TableSource`, completes
`table` and `dataset.table` paths from the default project and dataset, and maps BigQuery field
types to GoogleSQL types. Functions and types come from GoogleSQL's built-ins, plus BigQuery
functions GoogleSQL lacks. [src/analyzer.cc](../src/analyzer.cc) runs the analyzer with the same
language options as the parser; a statement that fails analysis is reported as `invalidQuery`.
For a query, the resolved output columns supply the result schema.

## Translator

[src/translator.cc](../src/translator.cc) translates the resolved AST into DuckDB SQL. Each scan
becomes a derived table whose columns are named after resolved column IDs, and user-facing names
are applied only at the output boundary, so duplicate or shadowed aliases never collide. Where
DuckDB differs from BigQuery, the translator spells out BigQuery's semantics, such as default
NULL ordering and result types.

[src/functions.cc](../src/functions.cc) maps BigQuery functions to DuckDB with two rule tables.
A **rename** replaces only the function name. A **template** rewrites the call using `$n` for
the n-th argument and `#n` for that argument as a lowercase string literal, so
`DATE_DIFF(a, b, DAY)` becomes `date_diff('day', b, a)`.

## Backend

[DuckDB](https://duckdb.org/) executes translated statements through libduckdb's C++ API; each
project is a DuckDB catalog and each dataset a schema. The backend derives result schemas and
encodes rows in BigQuery's `{"f": [{"v": ...}]}` format. GCS load jobs download objects with
[google-cloud-cpp](https://github.com/googleapis/google-cloud-cpp) to temporary files, which
DuckDB then reads.
