# bigquery-emulator-duckdb

A BigQuery emulator using DuckDB as the backend.

Queries are parsed and analyzed as GoogleSQL, then translated to and executed by DuckDB, so
accepting a query does not guarantee BigQuery-identical results.

## Quick start

Enter the development shell, which provides the build and development tools, including `just`
and `bq`, then build the binary:

```bash
nix develop
bazelisk build //:bigquery-emulator-duckdb
# -> bazel-bin/bigquery-emulator-duckdb
```

Run the server:

```bash
bazel-bin/bigquery-emulator-duckdb serve --host 0.0.0.0 --http-port 9050 \
  --project='{"projectId":"test"}'
```

### Connect with `bq`

A dummy `--oauth_access_token` with `--nouse_google_auth` skips `bq`'s credential lookup.
Queries run as GoogleSQL by default.

```bash
alias bqe='bq --api http://127.0.0.1:9050 --project_id test --oauth_access_token=dummy --nouse_google_auth'

bqe mk --dataset ds
bqe mk --table ds.users id:INTEGER,name:STRING,created:TIMESTAMP
bqe query "INSERT INTO ds.users VALUES (1, 'alice', CURRENT_TIMESTAMP())"
bqe query --format=json 'SELECT * FROM `test.ds.users`'
bqe head ds.users
```

### Connect with the Go client library

The endpoint override replaces the whole API base path, so the emulator's URL is enough:

```go
client, err := bigquery.NewClient(ctx, "test",
	option.WithEndpoint("http://127.0.0.1:9050"), option.WithoutAuthentication())

query := client.Query("SELECT @name AS name")
query.Parameters = []bigquery.QueryParameter{{Name: "name", Value: "alice"}}
rows, err := query.Read(ctx)
```

### BigQuery Storage API (gRPC)

The same process exposes Storage Read and Write v1 on `127.0.0.1:9060`, sharing tables
with the REST API. Set `--grpc-port` to change the port (`0` chooses an available port).
Both listeners use `--host`. Enable Storage Read with insecure transport and no credentials:

```go
err = client.EnableStorageReadClient(ctx,
	option.WithEndpoint("127.0.0.1:9060"), option.WithoutAuthentication(),
	option.WithGRPCDialOption(grpc.WithTransportCredentials(insecure.NewCredentials())))
```

See the [Storage API guide](docs/storage-api.md) for writing rows, limits and stream lifetime.

### Run queries without a server

`shell` runs queries in the process instead of listening. It accepts `--project`, `--data-dir`,
`--session-user` and the [JavaScript UDF limits](#javascript-udf-limits) as `serve` does, and uses the only registered project unless
`--default-project` names one. `--default-dataset` sets the default dataset. Each command's
flags follow its name, and `serve --help` and `shell --help` list them.

```bash
bazel-bin/bigquery-emulator-duckdb shell --project='{"projectId":"test"}' < script.sql
bazel-bin/bigquery-emulator-duckdb shell --project='{"projectId":"test"}' -e 'SELECT 1 AS x'
# {"x":"1"}
```

For comparing results with GoogleSQL's reference implementation (`just reference`),
`shell --semantics=googlesql` drops every BigQuery difference the emulator selects: queries can
call exactly GoogleSQL's built-in functions, which behave as GoogleSQL's do, and geography bounds
are not expanded outward by one floating-point step. The default, `bigquery`, matches BigQuery.

```sh
bazel-bin/bigquery-emulator-duckdb shell --project='{"projectId":"test"}' \
  --semantics=googlesql -e 'SELECT LENGTH(CHR(0)) AS n'
```

For debugging the emulator itself, `shell --duckdb` runs the input as DuckDB SQL on the
emulator's database instead of as query jobs. Each project is a catalog, and
`bq_geography_text` shows the S2 vertices of a stored GEOGRAPHY, which DuckDB keeps as a BLOB.

```sh
bazel-bin/bigquery-emulator-duckdb shell --data-dir DIR --duckdb \
  -e 'SELECT bq_geography_text(g) AS g FROM test.ds.t'
```

Standard input that is not a terminal, or the `-e` (`--execute`) argument, runs as one query job, so a
multi-statement input is a script whose result is its last statement's. The shell exits with 1
when the job fails.

On a terminal, each entry runs as its own job once it ends with `;` after a complete script, so
an entry can hold a `BEGIN ... END` block. As in BigQuery without sessions, jobs share no
variables or temporary tables. Lines can be edited and recalled from the session's history with
the arrow keys. Ctrl+C discards the entry being typed and Ctrl+D exits. Lines
starting with `.` or `\` outside an entry are commands: `.help`, `.quit`,
`.use PROJECT[.DATASET]` and `.format FORMAT`.

Rows are written to standard output, keeping the values the REST API returns, such as INT64 as a
string. `--format` chooses `pretty`, `json`, `jsonl` or `csv`, and defaults to `pretty` on a
terminal and `jsonl` otherwise. Errors and DML row counts go to standard error. A project file
can be open in only one process, so the shell and a server cannot share a `--data-dir`.

## Projects

Register projects before using them. `--project` is repeatable and accepts either a JSON object
or `@FILE` to read that object from a local file. Relative paths are resolved from the working
directory; bare file paths, `file://` URIs and `@-` are not accepted.

```bash
just run serve --project='{"projectId":"test","numericId":"123456789012","friendlyName":"Local test"}' \
  --project=@./another-project.json
```

Each object requires a nonempty `projectId` without `/`; domain-scoped IDs are accepted.
Optional fields are `friendlyName` and `numericId`, both strings. Numbers must be positive
uint64 decimals without leading zeros. Invalid or unknown fields and conflicting registrations
are startup errors.

`projects.list` includes registered projects without datasets. REST references and SQL
qualifiers accept configured numbers as aliases, but responses and INFORMATION_SCHEMA use project
IDs. Unregistered projects return 404.
`projects.getServiceAccount` returns `bq-NUMBER@bigquery-encryption.iam.gserviceaccount.com`, the
form BigQuery uses, from the configured `numericId`, and rejects a project without one.

With `--data-dir`, registrations are stored in `projects.json` and restored at startup without
`--project`. Explicit registrations replace the same project's saved metadata, clearing optional
fields omitted from the supplied object. Other saved projects remain registered. Existing DuckDB
files from before explicit registration must be registered once with `--project` to use them.

## Execution identity

With `--session-user ID`, `SESSION_USER()` returns that identity as a `STRING` for every
client and project. The value must be nonempty and is returned unchanged. Without this option,
the function is unsupported. Stored views use the executing process's identity; it is not persisted.

```sh
just run serve --project='{"projectId":"test"}' --session-user='alice@example.com'
```

## JavaScript UDF limits

BigQuery documents neither limit exactly, so by default one JavaScript UDF call may run for 5
minutes, and the calls of one query may allocate 256 MiB on each thread that runs them.
`--javascript-timeout-ms MS` and `--javascript-memory-mib MIB` change them; both take positive
integers. A call past either limit fails its query.

```sh
just run serve --project='{"projectId":"test"}' --javascript-timeout-ms=1000
```

## Python UDFs

Persistent scalar Python UDFs run in a separate process using the emulator's internal worker
mode, linked to CPython 3.11's libpython. Bazel supplies the interpreter and standard library;
no host Python installation is needed. Keep the binary's `.runfiles` directory alongside it
when distributing the build. The Python runtime is a runtime dependency, so copying only the
main binary is insufficient.

```sql
CREATE FUNCTION ds.multiply(x FLOAT64, y FLOAT64)
RETURNS FLOAT64 LANGUAGE python
OPTIONS(runtime_version='python-3.11', entry_point='multiply') AS r'''
def multiply(x, y):
    return x * y
''';
SELECT ds.multiply(2, 10);
```

The default timeout is 5 minutes per call, including initial worker startup, and the worker's
memory limit is 256 MiB. On Linux this limits additional address space above the worker's
startup mappings, including Python initialization; on macOS the parent checks total resident
memory every 10 ms, so short allocations can exceed the limit between checks.
`--python-timeout-ms MS` and `--python-memory-mib MIB` change
these limits; both take positive integers. `--python-worker PATH` selects an external worker
instead of the internal worker mode.
The same options work in `shell` mode. See [Python UDF limits](docs/compatibility.md#python-udfs).

Workers run with the emulator user's filesystem and network permissions. Process separation
contains interpreter crashes and lets the emulator stop timed-out calls; it is not a sandbox
for untrusted Python code.

## External queries

With `--connection PROJECT.LOCATION.CONNECTION=CONNINFO`, `EXTERNAL_QUERY` reaches the PostgreSQL,
MySQL or Spanner database named by the connection. PostgreSQL takes a libpq connection string or URI;
MySQL takes `mysql:` followed by the DuckDB MySQL extension's `key=value` connection string;
Spanner takes `spanner://projects/PROJECT/instances/INSTANCE/databases/DATABASE`. Set
`SPANNER_EMULATOR_HOST=localhost:9010` to reach a locally running Spanner emulator; otherwise
Spanner uses Application Default Credentials. For example:

```sh
SPANNER_EMULATOR_HOST=localhost:9010 bazel-bin/bigquery-emulator-duckdb serve \
  --project='{"projectId":"test"}' \
  --connection='test.us.spanner=spanner://projects/test/instances/test/databases/test'
```

The Spanner instance and database must already exist. Start a
[Spanner emulator](https://cloud.google.com/spanner/docs/emulator) in a separate terminal with
`${SPANNER_EMULATOR_GATEWAY:-gateway_main}`. On Linux x86_64 and arm64, the Nix shell sets
`SPANNER_EMULATOR_GATEWAY` to the standalone emulator gateway.

Connections are not persisted in the data directory. See
[EXTERNAL_QUERY](docs/compatibility.md#external_query) for limits. For PostgreSQL:

```sh
just run serve --project='{"projectId":"test"}' \
  --connection='test.us.orders=postgresql://postgres@localhost:5432/orders'
```

For MySQL:

```sh
just run serve --project='{"projectId":"test"}' \
  --connection='test.us.orders=mysql:host=localhost port=3306 user=root database=orders'
```

## Persistence

By default all data lives in memory and is lost when the server exits. With `--data-dir DIR`,
each project is stored in `DIR/<project>.duckdb` (characters outside `[A-Za-z0-9_-]` are
percent-encoded, so `example.com:proj` becomes `example%2Ecom%3Aproj.duckdb`) and its registration,
datasets, tables, logical views and materialized views survive restarts. Jobs and Storage session/stream state remain
in memory only.

On SIGINT or SIGTERM the server shuts down cleanly and checkpoints every project file. A
project file can be open in only one process at a time, and a file written by a newer DuckDB
cannot be opened by an older one.

BIGNUMERIC columns store integers in units of 10^-38 under `bq_bignumeric_` type aliases. The
DuckDB CLI displays those integers; its ordinary numeric operations do not implement BIGNUMERIC
semantics. Use the emulator to read or update those values. For tables created by other DuckDB
clients, see [external type mappings and write limits](docs/compatibility.md#result-schemas-and-values).

## Logging

`serve` logs each REST request, each job's outcome and internal errors to standard error as text
lines that name the request and job they belong to. `--log-file PATH` appends them to a file
instead, and `--log-level` chooses `debug`, `info` (the default), `warning` or `error`; `debug`
adds each query's text and parameters and the DuckDB SQL it runs. The startup announcement stays on standard
error. `shell` writes only warnings and errors to standard error unless `--log-file` is given.

```sh
bazel-bin/bigquery-emulator-duckdb serve --project='{"projectId":"test"}' \
  --log-file emulator.log --log-level debug
```

## Google Cloud Storage

Load jobs read `gs://` objects from [fake-gcs-server](https://github.com/fsouza/fake-gcs-server)
when `STORAGE_EMULATOR_HOST` is set, and from the public Storage API otherwise, using Application
Default Credentials when they are available and anonymous access otherwise.

GCS source URIs support one `*` in the object name, following the
[BigQuery wildcard examples](third_party/bigquery_docs/batch-loading-data.md):
`gs://bucket/events/*.jsonl` and `gs://bucket/events/part-*` include subfolders, while
`gs://bucket/events/part-*.jsonl` matches only files in `events`. Bucket-name wildcards and
multiple asterisks are rejected. Wildcard expansion requires permission to list objects;
exact URIs are read without listing. The emulator rejects patterns with no matches.

`OBJ.FETCH_METADATA` reads object metadata from the same endpoint. Its authorizers are Cloud
resource connections that `--connection PROJECT.LOCATION.CONNECTION=cloud_resource` declares,
such as `--connection='test.us.storage=cloud_resource'`; see
[ObjectRef functions](docs/compatibility.md#objectref-functions) for limits.

CSV and newline-delimited JSON loads accept gzip data from GCS, local files, and uploads.
Compression is detected from the file contents, so a `.gz` extension is not required, and
compressed and uncompressed files can be loaded together.

## Compatibility

What the emulator supports is listed in generated pages:

- [docs/api.md](docs/api.md): REST API methods
- [docs/storage.md](docs/storage.md): Storage v1 gRPC methods
- [docs/sql.md](docs/sql.md): statements and clauses
- [docs/options.md](docs/options.md): DDL options
- [docs/functions.md](docs/functions.md): built-in functions
- [docs/function-types.md](docs/function-types.md): input-type combinations and return types, including enumeration gaps

These tables check execution, not agreement with BigQuery's results or metadata.
[Query and API limits](docs/compatibility.md) covers restrictions and differences, and the
[Storage API guide](docs/storage-api.md) covers sessions and write streams.

Unsupported behavior is rejected, with these deliberate exceptions:

- Materialized views compute current results on every read. Background refresh state is not
  simulated, and their stored row and byte counts are always zero. See
  [Materialized views](docs/compatibility.md#materialized-views) for the supported forms.
- `TABLESAMPLE SYSTEM` samples DuckDB vectors of 2048 rows rather than BigQuery storage blocks.
- `SESSION_USER()` returns the configured `--session-user` identity, rather than an authenticated
  caller.
- `EXTERNAL_QUERY` reaches the PostgreSQL, MySQL or Spanner databases that `--connection` configures,
  and ObjectRef functions authorize with the Cloud resource connections it declares, rather than
  connections that the Connection API creates.
- Authentication and IAM are not checked. Every registered project is visible and accessible to
  every client, and is treated as having the BigQuery API enabled. Resource tags are stored as
  metadata; Cloud Resource Manager tag definitions and tag-based access controls are not checked,
  and tags inherited from parent resources are not modeled. See [tag limits](docs/compatibility.md#resource-tags).
- A query that leaves `useLegacySql` unset runs as GoogleSQL, although BigQuery runs it as legacy
  SQL.
- `formatOptions.timestampOutputFormat` of `jobs.getQueryResults` and `tabledata.list` is ignored;
  timestamps use the documented
  [result encoding](docs/compatibility.md#result-schemas-and-values).
- The `fields` query parameter is ignored, so responses carry every field rather than the
  partial response asked for.
- `updateMode` of `datasets.patch` and `datasets.update` is ignored; they change the metadata
  that the emulator keeps, since it keeps no access controls.
- Python UDFs run locally in CPython workers with the standard library, rather than BigQuery's
  managed containers and pre-installed packages. Definitions are stored synchronously, with Python
  compilation and entry-point validation on the first call. Worker stdout and stderr are discarded.
- JavaScript UDFs run in QuickJS rather than V8, which BigQuery uses. The language is the same,
  but built-ins differ: QuickJS has no `Intl`, so `toLocaleString` and similar methods ignore
  locales, and nonstandard `Date.parse` formats differ. Errors omit BigQuery's line and column.

## Development

See [AGENTS.md](AGENTS.md) for building and testing, and
[docs/architecture.md](docs/architecture.md) for how a request is processed.

`nix develop` provides the build tools, `just`, formatters, linters and test tools.
CI uses apps such as `nix run .#clang-tidy`, `nix run .#test` and `nix run .#e2e`;
`nix run .#check` runs all checks. Check apps call the same `just` recipes used locally,
and share a fixed build-tool PATH for Bazel actions with the development shell.
Test dependencies are available through `TEST_TOOLS_PATH`, which the test and probe recipes add to PATH only for their execution;
`SPANNER_EMULATOR_GATEWAY` selects the standalone Spanner gateway on Linux.

`just e2e` starts the standalone Spanner emulator on Linux x86_64 and arm64. On other platforms,
Spanner cases are skipped unless `SPANNER_EMULATOR_HOST` and `E2E_SPANNER_DATABASE` name an externally
started emulator and an empty GoogleSQL database.

Maintainers collect unresolved BigQuery behavior with `just bigquery-answers PROJECT`, or select
one file with `just bigquery-answers PROJECT PATH`. SQL, REST and BigQuery Storage gRPC cases use
[the bqanswers formats](tools/bqanswers/README.md). `just bigquery-answers-validate` checks API
scenarios locally without authentication or requests.

Clang-tidy runs through the repository's [Bazel aspect](tools/clang_tidy/README.md).
`just heavy-includes` checks [expensive GoogleSQL includes](tools/heavy_includes/README.md) separately.

## License

MIT License. See [LICENSE](LICENSE) for details.
