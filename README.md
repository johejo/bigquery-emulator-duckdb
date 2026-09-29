# bigquery-emulator-duckdb

A BigQuery Emulator using DuckDB as the backend.

Queries are parsed and analyzed as GoogleSQL, then translated to and executed by DuckDB, so
accepting a query does not guarantee BigQuery-identical results.

## Quick start

Build the binary:

```bash
bazelisk build //:bigquery-emulator-duckdb
# -> bazel-bin/bigquery-emulator-duckdb
```

Run the server:

```bash
bazel-bin/bigquery-emulator-duckdb --host 0.0.0.0 --port 9050
```

### Connect with `bq`

`bq` normally obtains credentials from `gcloud`; a dummy `--oauth_access_token` together with
`--nouse_google_auth` skips that.

```bash
alias bqe='bq --api http://127.0.0.1:9050 --project_id test --oauth_access_token=dummy --nouse_google_auth'

bqe mk --dataset ds
bqe mk --table ds.users id:INTEGER,name:STRING,created:TIMESTAMP
bqe query --nouse_legacy_sql "INSERT INTO ds.users VALUES (1, 'alice', CURRENT_TIMESTAMP())"
bqe query --nouse_legacy_sql --format=json 'SELECT * FROM `test.ds.users`'
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

## Persistence

By default all data lives in memory and is lost when the server exits. With `--data-dir DIR`,
each project is stored in `DIR/<project>.duckdb` (characters outside `[A-Za-z0-9_-]` are
percent-encoded, so `example.com:proj` becomes `example%2Ecom%3Aproj.duckdb`) and its datasets
and tables come back after a restart. Jobs are kept in memory only.

On SIGINT or SIGTERM the server shuts down cleanly and checkpoints every project file. A
project file can be open in only one process at a time, and a file written by a newer DuckDB
cannot be opened by an older one.

## Google Cloud Storage

Load jobs read `gs://` objects from [fake-gcs-server](https://github.com/fsouza/fake-gcs-server)
when `STORAGE_EMULATOR_HOST` is set, and from the public Storage API otherwise, using Application
Default Credentials when they are available and anonymous access otherwise.

## Compatibility

What the emulator supports is listed in generated pages:

- [docs/api.md](docs/api.md): REST API methods
- [docs/sql.md](docs/sql.md): statements and clauses
- [docs/functions.md](docs/functions.md): built-in functions

Behavior that applies across them:

- Jobs complete synchronously, before the request that starts them returns.
- Only GoogleSQL is supported, not legacy SQL.
- Every statement is analyzed against the existing tables and parameter types first, so unknown
  names and type errors fail as in BigQuery. Constructs the translator does not handle fail as
  `invalidQuery`, naming the construct.
- Query result schemas come from the GoogleSQL analyzer, so anonymous columns are named `f0_`,
  `f1_`, … and `SUM` over integers reports `INTEGER`. Other statements derive the schema from
  DuckDB types: `TIMESTAMPTZ` → `TIMESTAMP`, `TIMESTAMP` → `DATETIME`, lists → `REPEATED`,
  structs → `RECORD`.
- Timestamps are encoded as epoch seconds, or epoch microseconds with
  `formatOptions.useInt64Timestamp`.
- The `SAFE.` prefix returns NULL for the function's own errors while argument errors still
  propagate. It is unsupported on volatile functions.

## Development

See [AGENTS.md](AGENTS.md) for building and testing, and
[docs/architecture.md](docs/architecture.md) for how a request is processed.

## License

MIT License. See [LICENSE](LICENSE) for details.
