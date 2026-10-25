# Collecting BigQuery answers

`bqanswers` collects behavior whose expected result is not settled by BigQuery's documentation or
independently verified test data. Only maintainers run it on BigQuery: requests can create billable
jobs and resources. Agents write and validate cases, but do not execute them on BigQuery.

```sh
just bigquery-answers PROJECT                      # All pending SQL and API cases.
just bigquery-answers PROJECT PATH                 # One file or directory.
just bigquery-answers-validate                      # Offline validation of API cases.
just bigquery-answers-validate PATH
```

Authentication uses `gcloud auth print-access-token`. REST requests go to `bigquery.googleapis.com`;
gRPC requests go to `bigquerystorage.googleapis.com`. The account needs the permissions for the
resources, jobs and streams in the cases. Directory arguments process `*.txt`, `*.api.yaml`,
`*.api.yml` and legacy `*.api.json`. Observation sidecars are excluded.

SQL files keep the existing query / `?>` format described in
[the unverified cases README](../../tests/e2e/goclient/testdata/unverified/README.md).
API scenarios use YAML with JSON-compatible values; JSON objects can be embedded directly.
Legacy JSON scenarios remain supported. Observations are collected from BigQuery; they are not
assertions against guessed results. Filled answers are skipped on subsequent runs. For YAML,
observations live in a separate JSON sidecar; remove its case entry only when deliberately
collecting that case again. Legacy JSON files retain inline `answer` fields.

## YAML scenarios and E2E verification

The same scenario executes through `internal/bqscenario` for answer collection and E2E
verification. Write new scenarios as `*.api.yaml` or `*.api.yml`. YAML must contain one document,
unique string mapping keys and JSON-compatible values. Anchors, aliases, tags and merge keys are
not supported; use `templates` and `vars` for reuse. Numbers use JSON syntax and retain their
literal precision without passing through float64. Quote numeric-looking strings such as protobuf
int64 values. Plain `on`, `off` and date-like text remain strings.

```yaml
version: 1
vars:
  dataset: bqa_${run}_${case}
cases:
  - name: description
    steps:
      - name: create
        rest:
          method: POST
          path: /bigquery/v2/projects/${project}/datasets
          body:
            datasetReference: {datasetId: "${dataset}"}
            description: example
      - name: read
        rest:
          method: GET
          path: /bigquery/v2/projects/${project}/datasets/${dataset}
        observe:
          description: /body/description
    cleanup:
      - name: delete
        rest:
          method: DELETE
          path: /bigquery/v2/projects/${project}/datasets/${dataset}?deleteContents=true
    expect: {"read": {"description": "example"}}
```

`body` and `expect` are structured values, whether written with YAML indentation or JSON syntax.
Use `query: |` for multiline SQL and `upload: |` for inline media. Do not put structured JSON in a
block scalar: that creates a string rather than an object.

Collecting `example.api.yaml` writes `example.api.answers.json`, with `version: 1` and a `cases`
object mapping case names to `{project, run, steps}` observations. The YAML file, comments and
formatting are never rewritten. Existing answers are skipped; unknown case names or malformed
sidecars fail collection. Changing a case does not invalidate its old answer automatically;
remove its sidecar entry when deliberately recollecting it. Both formats persist each completed
case atomically, including when cleanup fails.

Collection ignores `expect`. E2E verification ignores collected answers and requires `expect`
for each case. It compares the entire selected observation object: object key order is irrelevant,
array order and missing versus null matter, and numeric literals are compared exactly without
float rounding. Expected strings may reference scenario variables and response bindings.
Keep timestamps, generated IDs and etags out of observations unless they are under test.
The runner reports the file, case, actual observations and expectations on failure, and runs
cleanup independently of cancellation. Cases run in parallel with separate transports.

To check an API rejection and continue to inspect its effects, set `expectFailure: true` on the
step and select its error fields with `observe`. An unexpected success fails the case. This form
requires `observe` and cannot use `until`. A rejected observed step without `expectFailure`
retains the original collection behavior described below: record its entire error and stop.
Errors returned inside an HTTP-success body or gRPC-success message must be observed explicitly.
Collection treats authentication, quota and server failures as operational failures; the emulator
transport exposes these responses for E2E assertions.

After independent verification, move the YAML into `tests/e2e/scenarios/testdata/`, add `expect`
from documentation or BigQuery observations and run `just e2e`. Retain the sidecar if it supplies
the expectation's provenance; E2E never writes or loads it. Review collected answers before
promotion: a resource or account restriction may have prevented the intended operation.
Keep SDK behavior, binary decoding, concurrency and process lifecycle tests in Go, and one-cell
SQL checks in `scalars/*.txt`.

## API scenario fields

A file contains `version: 1`, optional `description` and `vars`, and a `cases` array. Each case has
a unique `name`, optional `description` and `vars`, `steps`, and optional `cleanup`. Case names
start with a letter and contain only letters, digits and underscores. Each step has a unique
`name` and exactly one of `rest` or `grpc`.

```json
{
  "version": 1,
  "vars": {"dataset": "bqa_${run}_${case}"},
  "cases": [{
    "name": "dataset_rounding",
    "steps": [{
      "name": "create",
      "rest": {
        "method": "POST",
        "path": "/bigquery/v2/projects/${project}/datasets",
        "body": {
          "datasetReference": {"datasetId": "${dataset}"},
          "location": "US",
          "defaultRoundingMode": "ROUND_HALF_EVEN"
        }
      }
    }, {
      "name": "read",
      "rest": {
        "method": "GET",
        "path": "/bigquery/v2/projects/${project}/datasets/${dataset}"
      },
      "observe": {"roundingMode": "/body/defaultRoundingMode"}
    }],
    "cleanup": [{
      "name": "delete",
      "rest": {
        "method": "DELETE",
        "path": "/bigquery/v2/projects/${project}/datasets/${dataset}?deleteContents=true"
      }
    }]
  }]
}
```

`${project}` is the CLI project, `${run}` is a fresh random identifier for the file's execution,
and `${case}` is the case name. These names are reserved. File and case `vars` supply other
values, with case variables taking precedence. Values can refer to other variables. A whole
JSON value `${schema}` preserves an object's, array's or number's type; substitutions within a
string accept scalar values. Object keys also expand variables and must remain strings;
colliding expanded keys are rejected. JSON escaping and integers above 2^53 are preserved.

`bind` saves response values as variables for later steps. `observe` selects response values to
record under the step name. Both map names to [JSON Pointers](https://www.rfc-editor.org/rfc/rfc6901).
An empty pointer selects the whole response. Missing observation paths are omitted; JSON null
is retained. A missing binding is a tool failure. Avoid selecting generated IDs, timestamps and
etags unless they are what the case investigates.

A REST response is `{"status": HTTP_STATUS, "body": JSON_BODY}`; an empty body is null.
The optional `headers` map supplies request headers. For a load job, use an upload path and put
CSV or JSON Lines media in `upload`; `body` is the job metadata:

```json
{
  "name": "load",
  "rest": {
    "method": "POST",
    "path": "/upload/bigquery/v2/projects/${project}/jobs?uploadType=multipart",
    "body": "${load_job}",
    "upload": "${csv_rows}"
  },
  "bind": {"job": "/body/jobReference/jobId"},
  "observe": {"httpStatus": "/status"}
}
```

This sends `multipart/related` with JSON metadata and inline media; it requires no GCS bucket.
Use `until` on a subsequent REST GET to poll for completion:

```json
{
  "name": "completed_load",
  "rest": {"method": "GET", "path": "/bigquery/v2/projects/${project}/jobs/${job}?location=US"},
  "until": {"/body/status/state": "DONE"},
  "observe": {"status": "/body/status"}
}
```

Every pointer in `until` must equal its given JSON value. Only GET can poll; mutations are never
retried. The default deadline is two minutes per step, including polling. It can be changed with
`go run ./tools/bqanswers -timeout 5m PROJECT PATH`. Include the dataset/job location in requests
where BigQuery requires it. `jobs.query` cases should request a sufficient `timeoutMs` and observe
`jobComplete`; a query that does not finish needs a separate jobs.getQueryResults polling step.

## gRPC

`grpc.method` names a BigQuery Storage v1 Read or Write RPC as `Service/Method`. Requests use
[protobuf JSON](https://protobuf.dev/programming-guides/json/): bytes are base64, int64 values can
be strings, and descriptors use their protobuf JSON field names. Services use their installed
protobuf descriptors, so server reflection is not required.

```json
{
  "name": "stream",
  "grpc": {
    "method": "google.cloud.bigquery.storage.v1.BigQueryWrite/CreateWriteStream",
    "headers": {"x-goog-request-params": "parent=projects/${project}/datasets/${dataset}/tables/t"},
    "requests": [{
      "parent": "projects/${project}/datasets/${dataset}/tables/t",
      "writeStream": {"type": "PENDING"}
    }]
  },
  "bind": {"stream": "/messages/0/name"},
  "observe": {"type": "/messages/0/type"}
}
```

Unary and server-streaming RPCs take one request. For `AppendRows`, `requests` contains the
ordered messages for a single bidirectional connection. Responses are received concurrently
with sends and are recorded in arrival order. The response envelope is
`{"code": "OK", "messages": [...]}`; a failed RPC also contains the full protobuf status in
`error` (unknown detail messages retain their `type_url` and base64 `value`). An application error inside an OK response, such as `AppendRowsResponse.error` or
`BatchCommitWriteStreamsResponse.streamErrors`, stays inside `messages`: observe those fields
or the messages themselves, as well as the success fields.

The [workflow fixture](../../internal/bqscenario/testdata/workflow.api.json) combines a REST load job, streaming writes
and a query of stored values.

## Reuse, failures and cleanup

Repeated cases can select a named plan from the file's `templates` map:

```json
{
  "version": 1,
  "templates": {"read": {
    "steps": [{
      "name": "read",
      "rest": {"method": "GET", "path": "/bigquery/v2/projects/${project}/datasets/${dataset}"},
      "observe": {"roundingMode": "/body/defaultRoundingMode"}
    }]
  }},
  "cases": [{"name": "example", "template": "read", "vars": {"dataset": "existing_dataset"}}]
}
```

Templates contain `steps` and optional `cleanup`. A case uses either a template or its own steps
and cleanup. Each case runs independently with fresh bindings. Use `${run}` and `${case}` in
resource names so cases and repeated executions do not interfere.

An unobserved step is setup: an API rejection leaves the case unanswered. A rejection of an
observed step records its full error response, ends that case, and runs cleanup. Transport,
authentication/permission, quota, server and deadline failures leave the case unanswered. Other
independent cases continue. Job errors returned inside a successful response must be observed,
for example with `/body/status` after a load completes.

Cleanup runs in its listed order, even on failure or Ctrl-C, with an independent deadline per
step. Deleting an already absent REST resource is successful cleanup. Observations are written
atomically after each completed case, including when cleanup fails; a later failure does not
discard earlier answers. A cleanup error is reported and must be resolved separately: rerunning
the tool skips the answered case. The answer records the project and run ID, and stderr also
prints the run ID for unanswered cases, so remaining resources can be identified.

Once a behavior is confirmed, promote the scenario with explicit expectations or use the
observations in the appropriate SDK E2E test, then remove the resolved unverified cases. Do not replace BigQuery observations with an
emulator run.
