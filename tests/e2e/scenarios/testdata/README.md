# API compatibility scenarios

Put verified `*.api.yaml` (or `*.api.yml`) cases here. Each case requires `expect`,
whose keys are step names and whose values are the objects selected by `observe`.
See [the scenario format](../../../../tools/bqanswers/README.md), and
[AGENTS.md](../../../../AGENTS.md#tests-and-probes) for what belongs in scenarios rather than
Go tests or scalar cases, and where expected values come from.

`just e2e` runs cases in parallel. Use `${run}` and `${case}` in dataset names and
provide cleanup. A test that lists a whole project needs an isolated project;
keep such tests in Go until the scenario has a suitable isolated fixture.

Do not automatically promote every collected answer into an assertion. Deliberate emulator
limitations can be tested here, but must not be collected as expected BigQuery behavior.

Query jobs are polled to completion; job errors are included in observations so an
HTTP-success response cannot hide a failed query. New cases should likewise observe
job completion and errors rather than assume that submitting a job finishes it.
