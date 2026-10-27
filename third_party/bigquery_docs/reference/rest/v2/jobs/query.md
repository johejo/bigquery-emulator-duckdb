- [HTTP request](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#body.HTTP_TEMPLATE)
- [Path parameters](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#body.PATH_PARAMETERS)
- [Request body](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#body.request_body)
- [Response body](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#body.response_body)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#body.QueryResponse.SCHEMA_REPRESENTATION)
- [Authorization scopes](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#body.aspect)
- [QueryRequest](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#QueryRequest)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#QueryRequest.SCHEMA_REPRESENTATION)
- [JobCreationMode](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#JobCreationMode)
- [Try it!](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#try-it)

Runs a BigQuery SQL query synchronously and returns query results if the query completes within a specified timeout.

### IAM Permissions

Requires the `bigquery.jobs.create` permission on the project resource.

Data-level permissions are highly dependent on the SQL statement being executed. While standard queries require data access (such as `bigquery.tables.getData` ), complex operations like DDL or DCL may require permissions to manage reservations, IAM policies, or project settings.

### HTTP request

`POST https://bigquery.googleapis.com/bigquery/v2/projects/{projectId}/queries`

The URL uses [gRPC Transcoding](https://google.aip.dev/127) syntax.

### Path parameters

| Parameters  |                                                     |
|-------------|-----------------------------------------------------|
| `projectId` | `string` Required. Project ID of the query request. |

### Request body

The request body contains an instance of [`QueryRequest`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#QueryRequest) .

### Response body

If successful, the response body contains data with the following structure:

**JSON representation**

```
{
  "kind": string,
  "schema": {
    object (TableSchema)
  },
  "jobReference": {
    object (JobReference)
  },
  "jobCreationReason": {
    object (JobCreationReason)
  },
  "queryId": string,
  "location": string,
  "totalRows": string,
  "pageToken": string,
  "rows": [
    {
      object
    }
  ],
  "totalBytesProcessed": string,
  "jobComplete": boolean,
  "errors": [
    {
      object (ErrorProto)
    }
  ],
  "cacheHit": boolean,
  "numDmlAffectedRows": string,
  "sessionInfo": {
    object (SessionInfo)
  },
  "dmlStats": {
    object (DmlStats)
  },
  "totalBytesBilled": string,
  "totalSlotMs": string,
  "creationTime": string,
  "startTime": string,
  "endTime": string
}
```

| Fields                |                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
|-----------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `kind`                | `string` The resource type.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `schema`              | `object ( `[`TableSchema`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/tables#TableSchema)` )` The schema of the results. Present only when the query completes successfully.                                                                                                                                                                                                                                                                                                                                                                                                               |
| `jobReference`        | `object ( `[`JobReference`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/JobReference)` )` Reference to the Job that was created to run the query. This field will be present even if the original request timed out, in which case jobs.getQueryResults can be used to read the results once the query has completed. Since this API only returns the first page of results, subsequent pages can be fetched via the same mechanism (jobs.getQueryResults). If jobCreationMode was set to `JOB_CREATION_OPTIONAL` and the query completes without creating a job, this field will be empty. |
| `jobCreationReason`   | `object ( `[`JobCreationReason`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/JobCreationReason)` )` Optional. The reason why a Job was created. Only relevant when a jobReference is present in the response. If jobReference is not present it will always be unset.                                                                                                                                                                                                                                                                                                                       |
| `queryId`             | `string` Auto-generated ID for the query.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| `location`            | `string` Output only. The geographic location of the query. For more information about BigQuery locations, see: <https://cloud.google.com/bigquery/docs/locations>                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `totalRows`           | `string ( `[`UInt64Value`](https://developers.google.com/discovery/v1/type-format)` format)` The total number of rows in the complete query result set, which can be more than the number of rows in this single page of results.                                                                                                                                                                                                                                                                                                                                                                             |
| `pageToken`           | `string` A token used for paging results. A non-empty token indicates that additional results are available. To see additional results, query the [`jobs.getQueryResults`](https://cloud.google.com/bigquery/docs/reference/rest/v2/jobs/getQueryResults) method. For more information, see [Paging through table data](https://cloud.google.com/bigquery/docs/paging-results) .                                                                                                                                                                                                                              |
| `rows[]`              | `object ( `[`Struct`](https://protobuf.dev/reference/protobuf/google.protobuf/#struct)` format)` An object with as many results as can be contained within the maximum permitted reply size. To get any additional rows, you can call jobs.getQueryResults and specify the jobReference returned above.                                                                                                                                                                                                                                                                                                       |
| `totalBytesProcessed` | `string ( `[`Int64Value`](https://developers.google.com/discovery/v1/type-format)` format)` The total number of bytes processed for this query. If this query was a dry run, this is the number of bytes that would be processed if the query were run.                                                                                                                                                                                                                                                                                                                                                       |
| `jobComplete`         | `boolean` Whether the query has completed or not. If rows or totalRows are present, this will always be true. If this is false, totalRows will not be available.                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| `errors[]`            | `object ( `[`ErrorProto`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/ErrorProto)` )` Output only. The first errors or warnings encountered during the running of the job. The final message includes the number of errors that caused the process to stop. Errors here do not necessarily mean that the job has completed or was unsuccessful. For more information about error messages, see [Error messages](https://cloud.google.com/bigquery/docs/error-messages) .                                                                                                                    |
| `cacheHit`            | `boolean` Whether the query result was fetched from the query cache.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| `numDmlAffectedRows`  | `string ( `[`Int64Value`](https://developers.google.com/discovery/v1/type-format)` format)` Output only. The number of rows affected by a DML statement. Present only for DML statements INSERT, UPDATE or DELETE.                                                                                                                                                                                                                                                                                                                                                                                            |
| `sessionInfo`         | `object ( `[`SessionInfo`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/SessionInfo)` )` Output only. Information of the session if this job is part of one.                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| `dmlStats`            | `object ( `[`DmlStats`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/DmlStats)` )` Output only. Detailed statistics for DML statements INSERT, UPDATE, DELETE, MERGE or TRUNCATE.                                                                                                                                                                                                                                                                                                                                                                                                            |
| `totalBytesBilled`    | `string ( `[`int64`](https://developers.google.com/discovery/v1/type-format)` format)` Output only. If the project is configured to use on-demand pricing, then this field contains the total bytes billed for the job. If the project is configured to use flat-rate pricing, then you are not billed for bytes and this field is informational only.                                                                                                                                                                                                                                                        |
| `totalSlotMs`         | `string ( `[`int64`](https://developers.google.com/discovery/v1/type-format)` format)` Output only. Number of slot ms the user is actually billed for.                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| `creationTime`        | `string ( `[`int64`](https://developers.google.com/discovery/v1/type-format)` format)` Output only. Creation time of this query, in milliseconds since the epoch. This field will be present on all queries.                                                                                                                                                                                                                                                                                                                                                                                                  |
| `startTime`           | `string ( `[`int64`](https://developers.google.com/discovery/v1/type-format)` format)` Output only. Start time of this query, in milliseconds since the epoch. This field will be present when the query job transitions from the PENDING state to either RUNNING or DONE.                                                                                                                                                                                                                                                                                                                                    |
| `endTime`             | `string ( `[`int64`](https://developers.google.com/discovery/v1/type-format)` format)` Output only. End time of this query, in milliseconds since the epoch. This field will be present whenever a query job is in the DONE state.                                                                                                                                                                                                                                                                                                                                                                            |

### Authorization scopes

Requires one of the following OAuth scopes:

- `https://www.googleapis.com/auth/bigquery`
- `https://www.googleapis.com/auth/cloud-platform`
- `https://www.googleapis.com/auth/bigquery.readonly`
- `https://www.googleapis.com/auth/cloud-platform.read-only`

For more information, see the [Authentication Overview](https://docs.cloud.google.com/docs/authentication#authorization-gcp) .

## QueryRequest

Describes the format of the jobs.query request.

**JSON representation**

```
{
  "kind": string,
  "query": string,
  "maxResults": integer,
  "defaultDataset": {
    object (DatasetReference)
  },
  "timeoutMs": integer,
  "destinationEncryptionConfiguration": {
    object (EncryptionConfiguration)
  },
  "dryRun": boolean,
  "preserveNulls": boolean,
  "useQueryCache": boolean,
  "useLegacySql": boolean,
  "parameterMode": string,
  "queryParameters": [
    {
      object (QueryParameter)
    }
  ],
  "location": string,
  "formatOptions": {
    object (DataFormatOptions)
  },
  "connectionProperties": [
    {
      object (ConnectionProperty)
    }
  ],
  "labels": {
    string: string,
    ...
  },
  "maximumBytesBilled": string,
  "requestId": string,
  "createSession": boolean,
  "jobCreationMode": enum (JobCreationMode),
  "jobTimeoutMs": string,
  "reservation": string
}
```

<table>
<colgroup>
<col style="width: 50%" />
<col style="width: 50%" />
</colgroup>
<thead>
<tr class="header">
<th>Fields</th>
<th></th>
</tr>
</thead>
<tbody>
<tr class="odd">
<td><code>kind</code></td>
<td><p><code>string</code></p>
<p>The resource type of the request.</p></td>
</tr>
<tr class="even">
<td><code>query</code></td>
<td><p><code>string</code></p>
<p>Required. A query string to execute, using Google Standard SQL or legacy SQL syntax. Example: "SELECT COUNT(f1) FROM myProjectId.myDatasetId.myTableId".</p></td>
</tr>
<tr class="odd">
<td><code>maxResults</code></td>
<td><p><code>integer</code></p>
<p>Optional. The maximum number of rows of data to return per page of results. Setting this flag to a small value such as 1000 and then paging through results might improve reliability when the query result set is large. In addition to this limit, responses are also limited to 10 MB. By default, there is no maximum row count, and only the byte limit applies.</p></td>
</tr>
<tr class="even">
<td><code>defaultDataset</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/datasets#DatasetReference"><code>DatasetReference</code></a><code> )</code></p>
<p>Optional. Specifies the default datasetId and projectId to assume for any unqualified table names in the query. If not set, all table names in the query string must be qualified in the format 'datasetId.tableId'.</p></td>
</tr>
<tr class="odd">
<td><code>timeoutMs</code></td>
<td><p><code>integer</code></p>
<p>Optional. Optional: Specifies the maximum amount of time, in milliseconds, that the client is willing to wait for the query to complete. By default, this limit is 10 seconds (10,000 milliseconds). If the query is complete, the jobComplete field in the response is true. If the query has not yet completed, jobComplete is false.</p>
<p>You can request a longer timeout period in the timeoutMs field. However, the call is not guaranteed to wait for the specified timeout; it typically returns after around 200 seconds (200,000 milliseconds), even if the query is not complete.</p>
<p>If jobComplete is false, you can continue to wait for the query to complete by calling the getQueryResults method until the jobComplete field in the getQueryResults response is true.</p></td>
</tr>
<tr class="even">
<td><code>destinationEncryptionConfiguration</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/EncryptionConfiguration"><code>EncryptionConfiguration</code></a><code> )</code></p>
<p>Optional. Custom encryption configuration (e.g., Cloud KMS keys)</p></td>
</tr>
<tr class="odd">
<td><code>dryRun</code></td>
<td><p><code>boolean</code></p>
<p>Optional. If set to true, BigQuery doesn't run the job. Instead, if the query is valid, BigQuery returns statistics about the job such as how many bytes would be processed. If the query is invalid, an error returns. The default value is false.</p></td>
</tr>
<tr class="even">
<td><code>preserveNulls </code><strong><code>(deprecated)</code></strong></td>
<td><p><code>boolean</code></p>
<blockquote>
<p>This item is deprecated!</p>
</blockquote>
<p>This property is deprecated.</p></td>
</tr>
<tr class="odd">
<td><code>useQueryCache</code></td>
<td><p><code>boolean</code></p>
<p>Optional. Whether to look for the result in the query cache. The query cache is a best-effort cache that will be flushed whenever tables in the query are modified. The default value is true.</p></td>
</tr>
<tr class="even">
<td><code>useLegacySql</code></td>
<td><p><code>boolean</code></p>
<p>Specifies whether to use BigQuery's legacy SQL dialect for this query. The default value is true. If set to false, the query uses BigQuery's <a href="https://docs.cloud.google.com/bigquery/docs/introduction-sql">GoogleSQL</a> . When useLegacySql is set to false, the value of flattenResults is ignored; query will be run as if flattenResults is false.</p></td>
</tr>
<tr class="odd">
<td><code>parameterMode</code></td>
<td><p><code>string</code></p>
<p>GoogleSQL only. Set to POSITIONAL to use positional (?) query parameters or to NAMED to use named (@myparam) query parameters in this query.</p></td>
</tr>
<tr class="even">
<td><code>queryParameters[]</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/QueryParameter"><code>QueryParameter</code></a><code> )</code></p>
<p>jobs.query parameters for GoogleSQL queries.</p></td>
</tr>
<tr class="odd">
<td><code>location</code></td>
<td><p><code>string</code></p>
<p>The geographic location where the job should run. For more information, see how to <a href="https://cloud.google.com/bigquery/docs/locations#specify_locations">specify locations</a> .</p></td>
</tr>
<tr class="even">
<td><code>formatOptions</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/DataFormatOptions"><code>DataFormatOptions</code></a><code> )</code></p>
<p>Optional. Output format adjustments.</p></td>
</tr>
<tr class="odd">
<td><code>connectionProperties[]</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/ConnectionProperty"><code>ConnectionProperty</code></a><code> )</code></p>
<p>Optional. Connection properties which can modify the query behavior.</p></td>
</tr>
<tr class="even">
<td><code>labels</code></td>
<td><p><code>map (key: string, value: string)</code></p>
<p>Optional. The labels associated with this query. Labels can be used to organize and group query jobs. Label keys and values can be no longer than 63 characters, can only contain lowercase letters, numeric characters, underscores and dashes. International characters are allowed. Label keys must start with a letter and each label in the list must have a different key.</p></td>
</tr>
<tr class="odd">
<td><code>maximumBytesBilled</code></td>
<td><p><code>string ( </code><a href="https://developers.google.com/discovery/v1/type-format"><code>Int64Value</code></a><code> format)</code></p>
<p>Optional. Limits the bytes billed for this query. Queries with bytes billed above this limit will fail (without incurring a charge). If unspecified, the project default is used.</p></td>
</tr>
<tr class="even">
<td><code>requestId</code></td>
<td><p><code>string</code></p>
<p>Optional. A unique user provided identifier to ensure idempotent behavior for queries. Note that this is different from the jobId. It has the following properties:</p>
<ol>
<li><p>It is case-sensitive, limited to up to 36 ASCII characters. A UUID is recommended.</p></li>
<li><p>Read only queries can ignore this token since they are nullipotent by definition.</p></li>
<li><p>For the purposes of idempotency ensured by the requestId, a request is considered duplicate of another only if they have the same requestId and are actually duplicates. When determining whether a request is a duplicate of another request, all parameters in the request that may affect the result are considered. For example, query, connectionProperties, queryParameters, useLegacySql are parameters that affect the result and are considered when determining whether a request is a duplicate, but properties like timeoutMs don't affect the result and are thus not considered. Dry run query requests are never considered duplicate of another request.</p></li>
<li><p>When a duplicate mutating query request is detected, it returns: a. the results of the mutation if it completes successfully within the timeout. b. the running operation if it is still in progress at the end of the timeout.</p></li>
<li><p>Its lifetime is limited to 15 minutes. In other words, if two requests are sent with the same requestId, but more than 15 minutes apart, idempotency is not guaranteed.</p></li>
</ol></td>
</tr>
<tr class="odd">
<td><code>createSession</code></td>
<td><p><code>boolean</code></p>
<p>Optional. If true, creates a new session using a randomly generated sessionId. If false, runs query with an existing sessionId passed in ConnectionProperty, otherwise runs query in non-session mode.</p>
<p>The session location will be set to QueryRequest.location if it is present, otherwise it's set to the default location based on existing routing logic.</p></td>
</tr>
<tr class="even">
<td><code>jobCreationMode</code></td>
<td><p><code>enum ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/query#JobCreationMode"><code>JobCreationMode</code></a><code> )</code></p>
<p>Optional. If not set, jobs are always required.</p>
<p>If set, the query request will follow the behavior described JobCreationMode.</p></td>
</tr>
<tr class="odd">
<td><code>jobTimeoutMs</code></td>
<td><p><code>string ( </code><a href="https://developers.google.com/discovery/v1/type-format"><code>int64</code></a><code> format)</code></p>
<p>Optional. Job timeout in milliseconds. If this time limit is exceeded, BigQuery will attempt to stop a longer job, but may not always succeed in canceling it before the job completes. For example, a job that takes more than 60 seconds to complete has a better chance of being stopped than a job that takes 10 seconds to complete. This timeout applies to the query even if a job does not need to be created.</p></td>
</tr>
<tr class="even">
<td><code>reservation</code></td>
<td><p><code>string</code></p>
<p>Optional. The reservation that jobs.query request would use. User can specify a reservation to execute the job.query. The expected format is <code>projects/{project}/locations/{location}/reservations/{reservation}</code> . Forces the query to use on-demand billing when set to <code>none</code> . This requires the project or organization to have <code>reservation_override_mode</code> set to <code>ALLOW_ANY_OVERRIDE</code> .</p></td>
</tr>
</tbody>
</table>

## JobCreationMode

Job Creation Mode provides different options on job creation.

| Enums                           |                                                                                                                                                                                                                                                                                                                                   |
|---------------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `JOB_CREATION_MODE_UNSPECIFIED` | If unspecified JOB_CREATION_REQUIRED is the default.                                                                                                                                                                                                                                                                              |
| `JOB_CREATION_REQUIRED`         | Default. Job creation is always required.                                                                                                                                                                                                                                                                                         |
| `JOB_CREATION_OPTIONAL`         | Job creation is optional. Returning immediate results is prioritized. BigQuery will automatically determine if a Job needs to be created. The conditions under which BigQuery can decide to not create a Job are subject to change. If Job creation is required, JOB_CREATION_REQUIRED mode should be used, which is the default. |
