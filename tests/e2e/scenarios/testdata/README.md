# API compatibility scenarios

Put verified `*.api.yaml` (or `*.api.yml`) cases here. Each case requires `expect`,
whose keys are step names and whose values are the objects selected by `observe`.
See [the scenario format](../../../../tools/bqanswers/README.md).

`just e2e` runs cases in parallel. Use `${run}` and `${case}` in dataset names and
provide cleanup. A test that lists a whole project needs an isolated project;
keep such tests in Go until the scenario has a suitable isolated fixture.

Keep SQL returning one cell in `goclient/testdata/scalars`. Keep SDK type conversion,
iterators, managedwriter, binary decoding, concurrency and process lifecycle tests
in Go. Expected values must have an independent source, as required by AGENTS.md.
Do not copy the emulator's output into `expect` or automatically promote every
collected answer into an assertion. Deliberate emulator limitations can be tested
here, but must not be collected as expected BigQuery behavior.

The initial migrated cases are:

- `ddl_statistics.api.yaml`: statement types and DDL target tables, with independent
  fixtures for each statement and a shared execution template.
- `label_order.api.yaml`: label ordering after REST patches, preserving the recorded
  BigQuery expectations and the sequence of updates.

Query jobs are polled to completion; job errors are included in observations so an
HTTP-success response cannot hide a failed query. New cases should likewise observe
job completion and errors rather than assume that submitting a job finishes it.
