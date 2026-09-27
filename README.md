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
| Query jobs | Partial | Jobs complete synchronously, so cancellation cannot interrupt them. Legacy SQL is unsupported, and results are not stored in anonymous tables. |
| Load jobs | Partial | `bq load` accepts local files and `gs://` objects; direct `jobs.insert` also accepts local paths and `file://` paths. CSV, newline-delimited JSON, and Parquet are supported. Jobs complete synchronously. GCS uses `STORAGE_EMULATOR_HOST` for fake-gcs-server or the public Storage API; set `GOOGLE_OAUTH_ACCESS_TOKEN` for private objects. Wildcard URIs and compressed inputs are unsupported. |
| Copy jobs | Partial | `bq cp` and `jobs.insert` copy one or multiple tables, with create and write dispositions. Jobs complete synchronously. Table snapshots, clones, and cross-region behavior are unsupported. |
| Extract jobs | Unsupported | `jobs.insert` does not implement extract jobs. |
| Datasets and tables | Partial | `list`, `get`, `insert`, and `delete`; update and patch methods are not implemented. |
| Table data | Partial | `tabledata.list` and `tabledata.insertAll` are implemented. Streaming inserts accept scalar, repeated, and record fields, report row errors, and support `skipInvalidRows` and `ignoreUnknownValues`. Insert ID deduplication and template suffixes are unsupported. |
| Result pagination | Supported | Query results and table data accept `maxResults`, `startIndex`, and `pageToken`. |
| Query parameters | Supported | Named (`@name`) and positional (`?`) parameters, including ARRAY and STRUCT values. |
| Dry runs | Supported | Validates queries and returns their result schema without executing them. |
| Result schema | Supported | Queries take column names and types from the GoogleSQL analyzer, so anonymous columns are named `f0_`, `f1_`, … and `SUM` over integers reports `INTEGER`. A query returning STRUCT values returns their fields as columns, with anonymous fields named `_field_1`, `_field_2`, …. Other statements derive `TableSchema` from DuckDB types: `TIMESTAMPTZ` → `TIMESTAMP`, `TIMESTAMP` → `DATETIME`, lists → `REPEATED`, structs → `RECORD`. |
| Result rows | Supported | BigQuery `{"f": [{"v": ...}]}` encoding, including nested values and base64 bytes. |
| Timestamp encoding | Supported | Epoch seconds by default; epoch microseconds with `formatOptions.useInt64Timestamp`. |
| Projects, datasets, and tables | Partial | Map to DuckDB catalogs, schemas, and tables; `project.dataset.table` references work. Catalogs are in memory unless `--data-dir` is given, in which case each project persists to its own DuckDB file. |

## SQL compatibility

Rows list only what is unsupported or differs from BigQuery; everything else in a statement kind
is expected to run.

| Feature | Status | Scope and limitations |
| --- | --- | --- |
| Analysis | Partial | Released GoogleSQL language features are enabled. Every statement is analyzed against the tables in DuckDB and the declared parameter types, so unknown names and type errors fail as in BigQuery. Statements, scans and expressions the translator does not handle fail as `invalidQuery`, naming the construct. |
| Statements and clauses | Partial | Listed per feature in [docs/sql.md](docs/sql.md). |
| Built-in functions | Partial | Listed per function in [docs/functions.md](docs/functions.md). |
| `SAFE.` function prefix | Partial | The function's own errors return NULL while argument errors still propagate. Volatile functions and calls translated to an explicit `error()` are unsupported. |

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

[src/translator.cc](src/translator.cc) translates the resolved AST of queries, INSERT, UPDATE,
DELETE and MERGE, and CREATE TABLE [AS SELECT], CREATE SCHEMA and DROP TABLE / SCHEMA into DuckDB
SQL. A statement containing an unsupported node, function, type or modifier fails as
`invalidQuery`, naming the construct.

Each scan becomes a derived table whose columns are named after resolved column IDs, and
user-facing names are applied only at the output boundary, so duplicate or shadowed aliases never
collide. Where DuckDB differs from BigQuery, the translator spells out BigQuery's semantics: ORDER
BY states the default NULL ordering, sort keys survive projections, set operations matching
columns by name become positional ones, and function and aggregate results are cast to their
resolved types. Operators follow BigQuery too, such as `<<` and `>>` on 64-bit values,
`IS [NOT] DISTINCT FROM`, `IN UNNEST` and the `SAFE.` prefix on arithmetic.
Literals, types, constructors and star modifiers are respelled for DuckDB, such as `1.5` as
`1.5::DOUBLE`, `b'abc'` as `from_hex('616263')` and `* EXCEPT` as `* EXCLUDE`, and query
parameters are substituted as typed literals.

`tests/translator_test.cc` runs supported queries directly against DuckDB, covering results,
types, aliases, ordering and parameterized preparation.

### Functions

[src/functions.cc](src/functions.cc) defines two rule tables keyed by BigQuery function name.
A **rename** replaces only the function name. A **template** rewrites the call using `$n` for
the n-th argument and `#n` for that argument as a lowercase string literal, as required for
DuckDB date parts. For example, `DATE_DIFF(a, b, DAY)` becomes `date_diff('day', b, a)`.

[docs/functions.md](docs/functions.md) lists which BigQuery functions the emulator runs, with
notes on where their behavior needs care. It is generated by `//:function_probe` from
`tools/function_probe_hints.txt`. [docs/sql.md](docs/sql.md) does the same for statements and
clauses, generated by `//:sql_probe` from `tools/sql_features.txt`. `//:functions_md_test` and
`//:sql_md_test` fail when they are stale; run `just docs` to regenerate them.

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

By default all data lives in memory and is lost when the server exits. With `--data-dir DIR`,
each project is stored in `DIR/<project>.duckdb` (characters outside `[A-Za-z0-9_-]` are
percent-encoded, so `example.com:proj` becomes `example%2Ecom%3Aproj.duckdb`) and its datasets
and tables come back after a restart. Jobs are still kept in memory only.

```bash
bazel-bin/bigquery-emulator-duckdb --data-dir ./data
```

On SIGINT or SIGTERM the server shuts down cleanly and checkpoints every project file. A
project file can be open in only one process at a time, and a file written by a newer DuckDB
cannot be opened by an older one.

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

It builds on macOS and on Linux x86_64 and aarch64, linking DuckDB's prebuilt `libduckdb` for the target platform.

Nix optionally provides a development shell with tools such as Bazel, compilers, and the `bq` command-line tool.

## Testing

End-to-end tests using the `bq` command-line tool are the primary compatibility tests, because the emulator should behave correctly from the BigQuery client's point of view. These scenarios should be described with [runn](https://github.com/k1LoW/runn), using it to execute `bq` commands against the emulator and verify the observable BigQuery-compatible behavior.

```bash
just e2e            # builds the emulator, starts it on a free port and runs tests/e2e/*.yml
just e2e --verbose  # extra arguments are passed to `runn run`
```

`tests/e2e/goclient` covers the Go client library (`cloud.google.com/go/bigquery`), which drives the API differently from `bq`: it runs parameterised queries, polls jobs, streams rows with `Inserter.Put`, and asks for timestamps as epoch microseconds. It is a Go module of its own, so the first run downloads its dependencies; `runn` starts it like any other scenario.

The load scenario starts fake-gcs-server, uploads a fixture object, and checks both GCS and local file load jobs through runn.

The end-to-end scenarios own client-visible results, including representative function semantics, query parameters, and table operations. Add new behavior checks there first. Keep C++ tests for focused internal boundaries: translation choices and edge cases in `tests/translator_test.cc`, result encoding in `tests/backend_test.cc`, and raw HTTP response details in `tests/server_test.cc` (`just test`). `tests/emulator_test.cc` covers direct `Emulator::RunQuery` behavior that the client scenarios do not exercise. Avoid copying the same SQL and expected result between these layers.

#### Internal structure memo

- HTTP server: cpp-httplib (header-only, in the Bazel Central Registry) + nlohmann/json. Crow was considered but it is not in the BCR and depends on asio.
- `third_party/bigquery/discovery.json` is the discovery document bundled with `bq`, embedded into the binary by a genrule.

## License

MIT License. See [LICENSE](LICENSE) for details.
