# Development

See [AGENTS.md](../AGENTS.md) for building, testing and the conventions changes follow, and
[architecture.md](architecture.md) for how a request is processed.

## Tools

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
[the bqanswers formats](../tools/bqanswers/README.md). `just bigquery-answers-validate` checks API
scenarios locally without authentication or requests.

Clang-tidy runs through the repository's [Bazel aspect](../tools/clang_tidy/README.md).
`just heavy-includes` checks [expensive GoogleSQL includes](../tools/heavy_includes/README.md) separately.

## Debugging shells

For comparing results with GoogleSQL's reference implementation (`just reference`),
`shell --semantics=googlesql` drops every BigQuery difference the emulator selects: queries can
call exactly GoogleSQL's built-in functions, which behave as GoogleSQL's do, and geography bounds
are not expanded outward by one floating-point step. TIMESTAMP values retain nanoseconds across
0001–9999, including in arrays, structs and comparisons; shell output shows them as timestamp text.
Operations still using the microsecond backend reject inputs that would lose precision, and
`RANGE<TIMESTAMP>` bounds remain limited to microseconds. The default, `bigquery`, keeps BigQuery's
microsecond TIMESTAMP representation.

```sh
bazel-bin/bigquery-emulator-duckdb shell --project='{"projectId":"test"}' \
  --semantics=googlesql -e 'SELECT LENGTH(CHR(0)) AS n'
```

`shell --duckdb` runs the input as DuckDB SQL on the emulator's database instead of as query
jobs. Each project is a catalog, and `bq_geography_text` shows the S2 vertices of a stored
GEOGRAPHY, which DuckDB keeps as a BLOB. `bq_timestamp_nanos_text(t)` shows a nanosecond
TIMESTAMP created in GoogleSQL mode. Use the same semantics mode when creating and querying
stored tables; their timestamp representations differ.

```sh
bazel-bin/bigquery-emulator-duckdb shell --data-dir DIR --duckdb \
  -e 'SELECT bq_geography_text(g) AS g FROM test.ds.t'
```
