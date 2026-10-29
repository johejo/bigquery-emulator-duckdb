# TODO

## Check collation specifications when translating COLLATE

BigQuery rejects a specification other than `und:ci` and the empty one when it analyzes the query.
The emulator checks it only when the backend functions run: `bq_collate` checks COLLATE's argument,
and each collated operation, such as `bq_collation_key` or `bq_collation_like`, checks it again,
since `bq_collate` is not called for a NULL value. Neither runs over zero rows or for a column
DuckDB prunes, so such a query succeeds where BigQuery fails.

Checking COLLATE's literal specification in the translator would fail every such query as BigQuery
does, and the backend functions would no longer take `googlesql_collation::Options::und_ci_only`.
The translator does not know whether it runs with BigQuery's or GoogleSQL's semantics
(`--semantics`, and the compliance tests, which take other specifications such as `und:cs`), so
that choice has to reach it first; `EmulatorFunctionCatalog()` is shared by both.
