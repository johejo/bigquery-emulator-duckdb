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
`shell --semantics=googlesql` drops the BigQuery differences the emulator selects through options:
queries can call exactly GoogleSQL's built-in functions, which behave as GoogleSQL's do, use query
forms BigQuery rejects, such as INTERSECT ALL, except those
[still rejected in both modes](#forms-only-googlesql-accepts), and geography bounds are not
expanded outward by one floating-point step. TIMESTAMP values retain nanoseconds across
0001–9999, including in arrays, structs and comparisons; shell output shows them as timestamp text.
Operations still using the microsecond backend reject inputs that would lose precision, and
`RANGE<TIMESTAMP>` bounds remain limited to microseconds. The default, `bigquery`, keeps BigQuery's
microsecond TIMESTAMP representation.

```sh
bazel-bin/bigquery-emulator-duckdb shell --project='{"projectId":"test"}' \
  --semantics=googlesql -e 'SELECT LENGTH(CHR(0)) AS n'
```

### Forms only GoogleSQL accepts

These forms GoogleSQL accepts and BigQuery rejects. Where `--semantics=googlesql` also rejects
one, the compliance statements that use it are listed in
[known_errors.textproto](../tests/compliance/known_errors.textproto) under the reason the Form
column paraphrases. BigQuery's documentation states most of these restrictions; the last three it
does not.

| Form | Rejected in | `googlesql` | BigQuery's documentation |
|---|---|---|---|
| INTERSECT ALL and EXCEPT ALL | [scan.cc](../src/translator/scan.cc) `SetOperationScan` | Accepts, except over INTERVAL or collated columns | [Syntax](../third_party/bigquery_docs/reference/standard-sql/query-syntax.md) has DISTINCT only |
| ARRAY arguments of aggregate functions with DISTINCT | [analyzer.cc](../src/analyzer.cc) `CheckDistinctArguments` | Rejects | [Groupable types](../third_party/bigquery_docs/reference/standard-sql/data-types.md): ARRAY only in GROUP BY and SELECT DISTINCT |
| STRUCT and ARRAY columns in DISTINCT set operations | [scan.cc](../src/translator/scan.cc) `SetOperationScan` | Rejects | Groupable types, as above |
| STRUCT and ARRAY in a window's PARTITION BY | [aggregate.cc](../src/translator/aggregate.cc) | Rejects | Groupable types, as above |
| Type parameters in CAST | [analyzer.cc](../src/analyzer.cc) `CheckCastTypeParameters` | Rejects | [Parameterized types](../third_party/bigquery_docs/reference/standard-sql/data-types.md) only for columns and script variables |
| INTERVAL * FLOAT64 | `$MULTIPLY` in [functions_template.cc](../src/translator/functions_template.cc) | Rejects | [Multiplication](../third_party/bigquery_docs/reference/standard-sql/interval_functions.md) by an integer |
| HAVING modifiers and null-respecting ARRAY_AGG in PIVOT | Not located | Rejects | [PIVOT](../third_party/bigquery_docs/reference/standard-sql/query-syntax.md) takes only aggregates that ignore NULL inputs |
| ARRAYs with NULL elements in results, written values and loaded fixtures | `ResultValue` and `WrittenValue` in [function.cc](../src/translator/function.cc), [file_read.cc](../src/file_read.cc), [ddl_table.cc](../src/translator/ddl_table.cc) | Accepts, except in Parquet files and column defaults | [NULLs and the array type](../third_party/bigquery_docs/reference/standard-sql/data-types.md) |
| IS DISTINCT FROM over STRUCTs | `kDistinctRejections` in [functions_template.cc](../src/translator/functions_template.cc) | Rejects | [Defined](../third_party/bigquery_docs/reference/standard-sql/operators.md) by GROUP BY, which groups STRUCTs |
| SAFE.JSON_KEYS | `TypeUse::kRejectsSafe` in [functions_backend.cc](../src/translator/functions_backend.cc) | Rejects | [JSON_KEYS](../third_party/bigquery_docs/reference/standard-sql/json_functions.md) does not exclude SAFE |
| STRUCT<> or an anonymous field in a query's result | [analyzer.cc](../src/analyzer.cc) `CheckEmptyStructs` and `CheckResultColumns` | Rejects | [STRUCT<>](../third_party/bigquery_docs/reference/standard-sql/data-types.md) is a listed type, without the restriction |

A form that `googlesql` accepts has its own flag in `functions_duckdb::Options`, such as
`intersect_except_distinct_only`, which BigQuery's options set; end-to-end tests check that
rejection.

`shell --duckdb` runs the input as DuckDB SQL on the emulator's database instead of as query
jobs. Each project is a catalog, and `bq_geography_text` shows the S2 vertices of a stored
GEOGRAPHY, which DuckDB keeps as a BLOB. `bq_timestamp_nanos_text(t)` shows a nanosecond
TIMESTAMP created in GoogleSQL mode. Use the same semantics mode when creating and querying
stored tables; their timestamp representations differ.

```sh
bazel-bin/bigquery-emulator-duckdb shell --data-dir DIR --duckdb \
  -e 'SELECT bq_geography_text(g) AS g FROM test.ds.t'
```
