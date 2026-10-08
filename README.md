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
bazel-bin/bigquery-emulator-duckdb --host 0.0.0.0 --port 9050 \
  --project='{"projectId":"test"}'
```

### Connect with `bq`

`bq` normally obtains credentials from `gcloud`; a dummy `--oauth_access_token` together with
`--nouse_google_auth` skips that. The emulator runs a query that does not set
`--use_legacy_sql` as GoogleSQL, so `--nouse_legacy_sql` is unnecessary, although BigQuery itself
may run such a query as legacy SQL.

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

## Projects

Register projects before using them. `--project` is repeatable and accepts either a JSON object
or `@FILE` to read that object from a local file. Relative paths are resolved from the working
directory; bare file paths, `file://` URIs and `@-` are not accepted.

```bash
just run --project='{"projectId":"test","numericId":"123456789012","friendlyName":"Local test"}' \
  --project=@./another-project.json
```

Each object has a required, nonempty `projectId` without `/`, and optional `numericId` and
`friendlyName` strings. Domain-scoped project IDs are accepted. `numericId` must be a positive
uint64 in decimal, without leading zeros. `friendlyName` may be empty. Unknown fields, invalid
values, duplicate project IDs, duplicate numbers and numbers that identify a different project
are startup errors.

`projects.list` returns registered projects, including those with no datasets. Unconfigured
numbers and friendly names are omitted. REST project references accept the configured number
as an alias of the project ID; response references use the project ID. Unregistered projects
return 404 rather than being created by an API request. `projects.getServiceAccount` remains
unsupported.

With `--data-dir`, registrations are stored in `projects.json` and restored at startup without
`--project`. Explicit registrations replace the same project's saved metadata, clearing optional
fields omitted from the supplied object. Other saved projects remain registered. Existing DuckDB
files from before explicit registration must be registered once with `--project` to use them.

## Execution identity

With `--session-user ID`, `SESSION_USER()` returns that identity as a `STRING` for every
client and project. Supply an email address or a principal identifier; the value must be nonempty
and is returned unchanged. Without this option, the function is unsupported. The identity is not
persisted: stored views use the identity configured for the process executing them.

```sh
just run --project='{"projectId":"test"}' --session-user='alice@example.com'
```

## Persistence

By default all data lives in memory and is lost when the server exits. With `--data-dir DIR`,
each project is stored in `DIR/<project>.duckdb` (characters outside `[A-Za-z0-9_-]` are
percent-encoded, so `example.com:proj` becomes `example%2Ecom%3Aproj.duckdb`) and its registration,
datasets and tables come back after a restart. Logical views, including their GoogleSQL definitions and
schemas, are persisted too. Jobs are kept in memory only.

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
- [docs/sql.md](docs/sql.md): statements and clauses
- [docs/functions.md](docs/functions.md): built-in functions

Behavior that applies across them:

- Jobs complete synchronously, before the request that starts them returns.
- Only GoogleSQL is supported, not legacy SQL.
- Every statement is analyzed against the existing tables and parameter types first, so unknown
  names and type errors fail as in BigQuery. Constructs the translator does not handle fail as
  `invalidQuery`, naming the construct.
- Logical views support `CREATE [OR REPLACE] VIEW`, `CREATE VIEW IF NOT EXISTS`, `DROP VIEW`
  and REST table creation, lookup, listing and deletion. Query views with SQL; `tabledata.list`
  cannot read them. Temporary, recursive and value-table views, and views that read temporary
  tables, are unsupported.
- Typed SQL UDFs created with `CREATE TEMP FUNCTION` are available within their query and
  are expanded by GoogleSQL before translation. `ANY TYPE`, JavaScript, persistent and aggregate
  UDFs, `OR REPLACE`, `IF NOT EXISTS`, `SAFE.` UDF calls, UDF options, and views that call temporary
  UDFs are unsupported. Subquery bodies do not support volatile arguments. Queries with TEMP function declarations do not support dry runs,
  destination tables or positional parameters.
- Multi-statement queries return the result of the last statement that ran. They create no
  child jobs and cannot be dry runs. Their temporary tables are dropped when they end, and no
  dataset holds them. `EXECUTE IMMEDIATE` runs one SQL statement, but no scripting statement such
  as `BEGIN ... END` and no transaction control. `CALL` and assignments to system variables are
  unsupported, and an error the emulator reports as unsupported is never handled by an
  `EXCEPTION` clause.
- Transactions support `BEGIN TRANSACTION`, `COMMIT TRANSACTION` and `ROLLBACK TRANSACTION`
  within a multi-statement query. Unfinished transactions roll back when the script ends.
  Transaction modes, SQL UDF declarations inside transactions, writes to multiple projects (or both permanent and temporary tables),
  and committing after a handled error are unsupported; roll back a failed transaction.
- `UPDATE ... FROM` rejects multiple source matches for a target row. `MERGE` does so when it
  has a matched `UPDATE` clause and a matched action applies. Failed writes leave no changes.
- Query result schemas come from the GoogleSQL analyzer, so anonymous columns are named `f0_`,
  `f1_`, … and `SUM` over integers reports `INTEGER`. Other statements derive the schema from
  DuckDB types: `TIMESTAMPTZ` → `TIMESTAMP`, `TIMESTAMP` → `DATETIME`, lists → `REPEATED`,
  structs → `RECORD`.
- Timestamps are encoded as epoch seconds, or epoch microseconds with
  `formatOptions.useInt64Timestamp`.
- The `SAFE.` prefix returns NULL for the function's own errors while argument errors still
  propagate. It is unsupported on volatile functions.
- Every dataset is in the `US` location, so the `INFORMATION_SCHEMA` region qualifier
  `` `region-us` `` covers every dataset of the project and any other region none.

The emulator rejects what it cannot emulate rather than accepting it and behaving differently.
These differences are deliberate exceptions, kept for convenience:

- `SESSION_USER()` returns the configured `--session-user` identity, rather than an authenticated
  caller.
- Authentication and IAM are not checked. Every registered project is visible and accessible to
  every client, and is treated as having the BigQuery API enabled.
- A query that leaves `useLegacySql` unset runs as GoogleSQL, although BigQuery runs it as legacy
  SQL. Legacy SQL itself is unsupported.
- `formatOptions.timestampOutputFormat` of `jobs.getQueryResults` and `tabledata.list` is ignored;
  timestamps are encoded as above.
- The `fields` query parameter is ignored, so responses carry every field rather than the
  partial response asked for. Clients such as the Go client send it on every `jobs.get`.
- `updateMode` of `datasets.patch` and `datasets.update` is ignored; they change the metadata
  that the emulator keeps, since it keeps no access controls.

## Development

See [AGENTS.md](AGENTS.md) for building and testing, and
[docs/architecture.md](docs/architecture.md) for how a request is processed.

## License

MIT License. See [LICENSE](LICENSE) for details.
