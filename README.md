# bigquery-emulator-duckdb

A BigQuery Emulator using DuckDB as the backend.

## Server

For BigQuery compatibility, an HTTP server is implemented to handle BigQuery's REST API requests. The server receives SQL queries, passes them through the frontend, translator, and backend, and returns the results in a format compatible with BigQuery.

## Frontend

[GoogleSQL](https://github.com/google/googlesql) is used as the frontend parser and analyzer. It parses and analyzes the SQL query and produces a GoogleSQL AST or resolved AST. The frontend should stay focused on BigQuery SQL semantics and should not depend on DuckDB-specific execution details.

## Translator

The translator converts the GoogleSQL AST or resolved AST into a DuckDB-compatible query. This layer owns the compatibility gap between BigQuery SQL and DuckDB SQL, such as function names, type mappings, identifier handling, and BigQuery-specific syntax.

Keeping this layer separate prevents the frontend from growing into both a parser/analyzer and a DuckDB query generator, while also keeping the backend focused on execution.

## Backend

[DuckDB](https://duckdb.org/) is used as the backend database engine. It executes the translated DuckDB query and returns the results. Powered by libduckdb's C++ API.

## Usage

### Installation

Get bigquery-emulator-duckdb binary:

```bash
# TBD
```

### Run the server

```bash
# TBD
```

### Connect from BigQuery client

An example using the `bq` command-line tool to connect to the emulator:

```bash
# TBD
```

## Development

The project uses C++20 and Bazel. Prefer the standard library for project code.

Nix optionally provides a development shell with tools such as Bazel, compilers, and the `bq` command-line tool.

## Testing

End-to-end tests using the `bq` command-line tool are the primary compatibility tests, because the emulator should behave correctly from the BigQuery client's point of view. These scenarios should be described with [runn](https://github.com/k1LoW/runn), using it to execute `bq` commands against the emulator and verify the observable BigQuery-compatible behavior.

Translator tests use [GoogleTest](https://github.com/google/googletest). The translator has a small and well-defined boundary, so C++ unit tests should cover query conversion cases such as functions, types, identifiers, literals, and BigQuery-specific syntax before the translated query reaches DuckDB.

#### Internal structure memo

- Use Crow as the HTTP server framework.
