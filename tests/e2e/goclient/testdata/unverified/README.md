# Unverified BigQuery cases

Cases whose expected value no source independent of the emulator settles yet go here, split by
feature. They stay outside `scalars/` so that no test runs them. SQL cases go in `*.txt`;
REST and BigQuery Storage gRPC scenarios go in `*.api.yaml` (legacy `*.api.json` is also supported),
using the [bqanswers scenario format](../../../../../tools/bqanswers/README.md).

Each SQL query stands alone, without the scalars fixture, and is followed by an empty `?>` line.
Lines starting with `param> ` before that line each give a named query parameter of the request,
as the JSON of an entry of `jobs.query`'s `queryParameters`, such as
`param> {"name": "p", "parameterType": {"type": "INT64"}, "parameterValue": {"value": "1"}}`.

## Resolving cases

A maintainer fills those in with BigQuery's answers through `just bigquery-answers PROJECT`,
which processes unanswered queries in `*.txt` and unanswered cases in API scenario files. A file or
directory can be selected with `just bigquery-answers PROJECT PATH`.
`just bigquery-answers-validate` checks SQL case formats and API scenarios without authentication
or requests. It rejects missing `?>` markers, markers without queries, malformed parameter JSON
objects and invalid recorded answers; it does not check SQL syntax. `just check` runs it too.

Each answered SQL case then moves to
[`scalars/`](../scalars/), written with `=>`, or with `!>` to `scalars/known_bugs.txt` where
the emulator answers otherwise. A case with parameters moves to a Go test instead, since
`scalars/` cases send none.

YAML scenarios retain selected BigQuery responses in a separate `*.api.answers.json` file;
legacy JSON scenarios use each case's `answer`. Promote verified YAML with explicit `expect`
values to `tests/e2e/scenarios/testdata/`, or use the observations in an appropriate SDK E2E test. Remove
the resolved cases from this directory. Do not copy generated IDs, timestamps or etags into
expected values. A case with a cleanup failure still retains its answer; use its recorded run ID
and project to clean up any remaining resources before removing it.
