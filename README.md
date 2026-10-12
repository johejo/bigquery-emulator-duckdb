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

`projects.list` includes registered projects without datasets. REST references accept configured
numbers as aliases, but responses use project IDs. Unregistered projects return 404.

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

CSV and newline-delimited JSON loads accept gzip data from GCS, local files, and uploads.
Compression is detected from the file contents, so a `.gz` extension is not required, and
compressed and uncompressed files can be loaded together.

## Compatibility

What the emulator supports is listed in generated pages:

- [docs/api.md](docs/api.md): REST API methods
- [docs/storage.md](docs/storage.md): Storage v1 gRPC methods
- [docs/sql.md](docs/sql.md): statements and clauses
- [docs/functions.md](docs/functions.md): built-in functions

These tables check execution, not agreement with BigQuery's results or metadata. See
[query and API behavior](docs/compatibility.md) for details on jobs, views, UDFs, scripts,
transactions and result encoding, and the [Storage API guide](docs/storage-api.md) for sessions
and write streams.

Unsupported behavior is rejected, with these deliberate exceptions:

- `SESSION_USER()` returns the configured `--session-user` identity, rather than an authenticated
  caller.
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

Clang-tidy runs through the repository's [Bazel aspect](tools/clang_tidy/README.md).

## License

MIT License. See [LICENSE](LICENSE) for details.
