# bigquery-emulator-duckdb

A BigQuery emulator using DuckDB as the backend.

Queries are parsed and analyzed as GoogleSQL, then translated to and executed by DuckDB, so
accepting a query does not guarantee BigQuery-identical results.

## Quick start

Enter the development shell, which provides Bazel, `just` and `bq`, then build the binary:

```bash
nix develop
bazelisk build //:bigquery-emulator-duckdb
# -> bazel-bin/bigquery-emulator-duckdb
```

Run the server:

```bash
bazel-bin/bigquery-emulator-duckdb --host 0.0.0.0 --port 9050 \
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
`--session-user` and the [JavaScript UDF limits](#javascript-udf-limits) as the server does, and uses the only registered project unless
`--project-id` names one. `--dataset-id` sets the default dataset.

```bash
bazel-bin/bigquery-emulator-duckdb shell --project='{"projectId":"test"}' < script.sql
bazel-bin/bigquery-emulator-duckdb shell --project='{"projectId":"test"}' -e 'SELECT 1 AS x'
# {"x":"1"}
```

Standard input that is not a terminal, or the `-e` argument, runs as one query job, so a
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
just run --project='{"projectId":"test","numericId":"123456789012","friendlyName":"Local test"}' \
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
just run --project='{"projectId":"test"}' --session-user='alice@example.com'
```

## JavaScript UDF limits

BigQuery documents neither limit exactly, so by default one JavaScript UDF call may run for 5
minutes, and the calls of one query may allocate 256 MiB on each thread that runs them.
`--javascript-timeout-ms MS` and `--javascript-memory-mib MIB` change them; both take positive
integers. A call past either limit fails its query.

```sh
just run --project='{"projectId":"test"}' --javascript-timeout-ms=1000
```

## External queries

With `--connection PROJECT.LOCATION.CONNECTION=CONNINFO`, `EXTERNAL_QUERY` reaches the PostgreSQL
or Spanner database named by the connection. PostgreSQL takes a libpq connection string or URI;
Spanner takes `spanner://projects/PROJECT/instances/INSTANCE/databases/DATABASE`. Set
`SPANNER_EMULATOR_HOST=localhost:9010` to reach a locally running Spanner emulator; otherwise
Spanner uses Application Default Credentials. For example:

```sh
SPANNER_EMULATOR_HOST=localhost:9010 bazel-bin/bigquery-emulator-duckdb \
  --project='{"projectId":"test"}' \
  --connection='test.us.spanner=spanner://projects/test/instances/test/databases/test'
```

The Spanner instance and database must already exist. Start a
[Spanner emulator](https://cloud.google.com/spanner/docs/emulator) in a separate terminal with
`${SPANNER_EMULATOR_GCLOUD:-gcloud} emulators spanner start`. On Linux x86_64, the Nix shell sets
`SPANNER_EMULATOR_GCLOUD` to an SDK with the native emulator component.

Connections are not persisted in the data directory. See
[EXTERNAL_QUERY](docs/compatibility.md#external_query) for limits. For PostgreSQL:

```sh
just run --project='{"projectId":"test"}' \
  --connection='test.us.orders=postgresql://postgres@localhost:5432/orders'
```

## Persistence

By default all data lives in memory and is lost when the server exits. With `--data-dir DIR`,
each project is stored in `DIR/<project>.duckdb` (characters outside `[A-Za-z0-9_-]` are
percent-encoded, so `example.com:proj` becomes `example%2Ecom%3Aproj.duckdb`) and its registration,
datasets, tables and logical views survive restarts. Jobs and Storage session/stream state remain
in memory only.

On SIGINT or SIGTERM the server shuts down cleanly and checkpoints every project file. A
project file can be open in only one process at a time, and a file written by a newer DuckDB
cannot be opened by an older one.

## Google Cloud Storage

Load jobs read `gs://` objects from [fake-gcs-server](https://github.com/fsouza/fake-gcs-server)
when `STORAGE_EMULATOR_HOST` is set, and from the public Storage API otherwise, using Application
Default Credentials when they are available and anonymous access otherwise.

GCS source URIs support one `*` in the object name, following the
[BigQuery wildcard examples](https://cloud.google.com/bigquery/docs/batch-loading-data#load-wildcards):
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
- [docs/functions.md](docs/functions.md): built-in functions

These tables check execution, not agreement with BigQuery's results or metadata.
[Query and API limits](docs/compatibility.md) covers restrictions and differences, and the
[Storage API guide](docs/storage-api.md) covers sessions and write streams.

Unsupported behavior is rejected, with these deliberate exceptions:

- `SESSION_USER()` returns the configured `--session-user` identity, rather than an authenticated
  caller.
- `EXTERNAL_QUERY` reaches the PostgreSQL or Spanner databases that `--connection` configures,
  and ObjectRef functions authorize with the Cloud resource connections it declares, rather than
  connections that the Connection API creates.
- Authentication and IAM are not checked. Every registered project is visible and accessible to
  every client, and is treated as having the BigQuery API enabled.
- A query that leaves `useLegacySql` unset runs as GoogleSQL, although BigQuery runs it as legacy
  SQL.
- `formatOptions.timestampOutputFormat` of `jobs.getQueryResults` and `tabledata.list` is ignored;
  timestamps use the documented
  [result encoding](docs/compatibility.md#result-schemas-and-values).
- The `fields` query parameter is ignored, so responses carry every field rather than the
  partial response asked for.
- `updateMode` of `datasets.patch` and `datasets.update` is ignored; they change the metadata
  that the emulator keeps, since it keeps no access controls.
- JavaScript UDFs run in QuickJS rather than V8, which BigQuery uses. The language is the same,
  but built-ins differ: QuickJS has no `Intl`, so `toLocaleString` and similar methods ignore
  locales, and nonstandard `Date.parse` formats differ. Errors omit BigQuery's line and column.

## Development

See [AGENTS.md](AGENTS.md) for building and testing, and
[docs/architecture.md](docs/architecture.md) for how a request is processed.

`just e2e` starts gcloud's native Spanner emulator on Linux x86_64. On other platforms, Spanner
cases are skipped unless `SPANNER_EMULATOR_HOST` and `E2E_SPANNER_DATABASE` name an externally
started emulator and an empty GoogleSQL database.

Clang-tidy runs through the repository's [Bazel aspect](tools/clang_tidy/README.md).

## License

MIT License. See [LICENSE](LICENSE) for details.
