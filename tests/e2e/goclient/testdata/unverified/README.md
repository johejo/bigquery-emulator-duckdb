# Unverified BigQuery cases

Cases whose expected value no source independent of the emulator settles yet go in `*.txt`
files here, split by feature. They stay outside `scalars/` so that no test runs them.
Each query stands alone, without the scalars fixture, and is followed by an empty `?>` line.

## Resolving cases

A maintainer fills those in with BigQuery's answers through `just bigquery-answers PROJECT`,
which processes every `*.txt` file in this directory, or `just bigquery-answers PROJECT FILE...`
for only the given files. Each answered case then moves to
[`scalars/`](../scalars/), written with `=>`, or with `!>` to `scalars/known_bugs.txt` where
the emulator answers otherwise.
