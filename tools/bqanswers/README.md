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
resources, jobs and streams in the cases. Directory arguments process `*.txt` and `*.api.json`.

SQL files keep the existing query / `?>` format described in
[the unverified cases README](../../tests/e2e/goclient/testdata/unverified/README.md).
API files use JSON scenarios. Observations are collected from BigQuery; they are not assertions
against guessed results. Filled answers are skipped on subsequent runs. Remove a case's `answer`
only when deliberately collecting that case again.

## API scenarios

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
string accept scalar values. JSON escaping and integers above 2^53 are preserved.

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

The [workflow fixture](testdata/workflow.api.json) combines a REST load job, streaming writes
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
step. Deleting an already absent REST resource is successful cleanup. Answers are written
atomically after each completed case, including when cleanup fails; a later failure does not
discard earlier answers. A cleanup error is reported and must be resolved separately: rerunning
the tool skips the answered case. The answer records the project and run ID, and stderr also
prints the run ID for unanswered cases, so remaining resources can be identified.

Once a behavior is confirmed, use the observations in the appropriate E2E test and remove the
resolved cases from the unverified directory. Do not replace BigQuery observations with an
emulator run.
