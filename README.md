# bigquery-emulator-duckdb

A BigQuery Emulator using DuckDB as the backend.

## Server

For BigQuery compatibility, an HTTP server is implemented to handle BigQuery's REST API requests. The server receives SQL queries, passes them through the frontend, translator, and backend, and returns the results in a format compatible with BigQuery.

The server is built on [cpp-httplib](https://github.com/yhirose/cpp-httplib) with [nlohmann/json](https://github.com/nlohmann/json). It serves the BigQuery v2 discovery document (which `bq` fetches from any non-Google endpoint) and the following resources:

| Resource | Methods |
| --- | --- |
| `jobs` | `query`, `insert` (query jobs only), `get`, `getQueryResults` |
| `datasets` | `list`, `get`, `insert`, `delete` |
| `tables` | `list`, `get`, `insert`, `delete` |
| `tabledata` | `list` |

Jobs always complete synchronously. Projects map to DuckDB catalogs (attached in-memory databases), datasets to schemas and tables to tables, so `project.dataset.table` references work unchanged.

## Frontend

[GoogleSQL](https://github.com/google/googlesql) is used as the frontend parser and analyzer. It parses and analyzes the SQL query and produces a GoogleSQL AST or resolved AST. The frontend should stay focused on BigQuery SQL semantics and should not depend on DuckDB-specific execution details.

## Translator

The translator converts the GoogleSQL AST or resolved AST into a DuckDB-compatible query. This layer owns the compatibility gap between BigQuery SQL and DuckDB SQL, such as function names, type mappings, identifier handling, and BigQuery-specific syntax.

Keeping this layer separate prevents the frontend from growing into both a parser/analyzer and a DuckDB query generator, while also keeping the backend focused on execution.

The translator walks the parser AST produced by the frontend and unparses it as DuckDB SQL,
extending GoogleSQL's own unparser and overriding only the constructs whose spelling differs:

- identifiers (`` `project.dataset.table` `` → `"project"."dataset"."table"`),
- string literals (the parser hands over the unescaped value, which is then quoted the DuckDB way)
  and bytes literals (`b'abc'` → `from_hex('616263')`, so quotes, NUL and non-UTF-8 survive),
- float literals (`1.5` → `1.5::DOUBLE`, since DuckDB would infer `DECIMAL`),
- type names in expressions, in DDL column definitions and in typed literals (`INT64` → `BIGINT`,
  `TIMESTAMP` → `TIMESTAMPTZ`, `DATETIME` → `TIMESTAMP`, `ARRAY<T>` → `T[]`,
  `STRUCT<a INT64>` → `STRUCT(a BIGINT)`, ...),
- `STRUCT(...)` constructors (→ `struct_pack(a := ...)`) and `ARRAY<T>[...]` constructors,
- `SAFE_CAST` (→ `TRY_CAST`) and the `CURRENT_TIMESTAMP()` family, which DuckDB spells without
  parentheses.

Everything else is printed by the base unparser, so joins, CTEs, subqueries and window functions
need no rules of their own. Because the output is generated from the AST it is normalised SQL
rather than the original text, and comments are dropped: they are not part of the AST.

Resolving names and types with GoogleSQL's analyzer, which would turn this into a resolved AST
based translation, is the next step.

## Backend

[DuckDB](https://duckdb.org/) is used as the backend database engine. It executes the translated DuckDB query and returns the results. Powered by libduckdb's C++ API.

The backend also encodes results in BigQuery's wire format: a `TableSchema` derived from the DuckDB column types (`TIMESTAMPTZ` → `TIMESTAMP`, `TIMESTAMP` → `DATETIME`, lists → `REPEATED`, structs → `RECORD`) and rows as `{"f": [{"v": ...}]}` with BigQuery's value encoding (timestamps as epoch seconds, bytes as base64, ...).

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

## Development

The project uses C++20 and Bazel. Prefer the standard library for project code.

Nix optionally provides a development shell with tools such as Bazel, compilers, and the `bq` command-line tool.

## Testing

End-to-end tests using the `bq` command-line tool are the primary compatibility tests, because the emulator should behave correctly from the BigQuery client's point of view. These scenarios should be described with [runn](https://github.com/k1LoW/runn), using it to execute `bq` commands against the emulator and verify the observable BigQuery-compatible behavior.

```bash
just e2e            # builds the emulator, starts it on a free port and runs tests/e2e/*.yml
just e2e --verbose  # extra arguments are passed to `runn run`
```

Translator tests use [GoogleTest](https://github.com/google/googletest). The translator has a small and well-defined boundary, so C++ unit tests should cover query conversion cases such as functions, types, identifiers, literals, and BigQuery-specific syntax before the translated query reaches DuckDB. The backend and the HTTP server have unit tests too (`just test`).

#### Internal structure memo

- HTTP server: cpp-httplib (header-only, in the Bazel Central Registry) + nlohmann/json. Crow was considered but it is not in the BCR and depends on asio.
- `third_party/bigquery/discovery.json` is the discovery document bundled with `bq`, embedded into the binary by a genrule.
