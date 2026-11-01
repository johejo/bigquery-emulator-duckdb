# Go client test data

- `scalars/*.txt`: a query returning one cell, checked through `jobs.query` against its REST
  value by `TestScalars`, using `scalars/setup.sql`. These cover SQL results without Go client
  type conversion.
- `scalars/graph/*.txt`: graph queries checked the same way by `TestGraphScalars`, using only
  `scalars/graph/setup.sql` and no default dataset. The two suites initialize independently and
  run in parallel.
- `errors/*.txt`: independent SQL queries expected to fail, checked through the Go client's
  `Query.Run` and `Job.Wait`. No default dataset is set. Use API scenarios in `tests/e2e/scenarios/testdata/` for request options, shared state and
  metadata assertions. Keep SDK behavior and typed decoding in Go tests.
- `unverified/*.txt`: questions for maintainers to resolve against BigQuery; see
  [its README](unverified/README.md).

## Case format

Write the SQL literally, over multiple lines if needed, followed by one expectation:

```text
SELECT 'hello'
=> "hello"

SELECT 1 / 0
=> error contains: division by zero
```

`=>` values are the JSON of the cell's `v` from `jobs.query`. `=> error:` expects any query
error; add a message after the colon to require an exact match. `=> error contains:` requires
an error containing the given text, for example when the Go client adds job context. `!>` marks
a known bug in scalar cases and fails once the expected answer is returned. Blank lines and `#` comments between cases are ignored.

## Choosing and running cases

Source expected values and error assertions as described in
[AGENTS.md](../../../../AGENTS.md#expected-values).
Deliberately unsupported emulator forms belong in `errors`, with an unsupported-error assertion.
Failures report the filename and the first SQL line, usable with `go test -run` to select a case.
Use `-run '^TestScalars$'` for non-graph scalars or `-run '^TestGraphScalars$'` for graph scalars;
only the selected suite's setup runs.
