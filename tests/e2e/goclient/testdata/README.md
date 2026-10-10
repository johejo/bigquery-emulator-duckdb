# Go client test data

- `scalars/*.txt`: a query returning one cell, checked through `jobs.query` against its REST
  value. These cover SQL results without Go client type conversion.
- `errors/*.txt`: independent SQL queries expected to fail, checked through the Go client's
  `Query.Run` and `Job.Wait`. No default dataset is set. Keep cases that need request options,
  shared state, typed decoding or metadata assertions in Go tests.
- `unverified/*.txt`: questions for maintainers to resolve against BigQuery; see its README.

Write the SQL literally, over multiple lines if needed, followed by one expectation:

```text
SELECT 'hello'
=> "hello"

SELECT 1 / 0
=> error contains: division by zero
```

`=>` values are the JSON of the cell's `v` from `jobs.query`. `=> error:` expects any query
error; add a message after the colon to require an exact match. `=> error contains:` requires
an error containing the given text, for example when the Go client adds job context. Existing
exact-message assertions remain exact. `!>` marks a known bug in scalar cases and fails once
the expected answer is returned. Blank lines and `#` comments between cases are ignored.

Keep expected values and error assertions sourced as described in the repository's AGENTS.md.
Deliberately unsupported emulator forms belong in `errors`, with an unsupported-error assertion.
Failures report the filename and the first SQL line, usable with `go test -run` to select a case.
