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

No existing compatibility cases have been migrated as part of introducing the runner.
