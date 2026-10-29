# TODO

## Check collation specifications when translating COLLATE

BigQuery rejects a specification other than `und:ci` and the empty one when it analyzes the query.
The emulator checks it only when the backend functions run: `bq_collate` checks COLLATE's argument,
and each collated operation, such as `bq_collation_key` or `bq_collation_like`, checks it again,
since `bq_collate` is not called for a NULL value. Neither runs over zero rows or for a column
DuckDB prunes, so such a query succeeds where BigQuery fails.

Checking COLLATE's literal specification in the translator would fail every such query as BigQuery
does, and the backend functions would no longer take `googlesql_collation::Options::und_ci_only`.
`EmulatorFunctionCatalog()` assembles a catalog for each `functions_duckdb::Options`, the same
choice the backend functions take, so COLLATE's registration can read it there: BigQuery's
semantics take only `und:ci`, while `--semantics googlesql` and the compliance tests take other
specifications such as `und:cs`.
