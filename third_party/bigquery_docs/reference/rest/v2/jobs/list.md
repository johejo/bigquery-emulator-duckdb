- [HTTP request](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/list#body.HTTP_TEMPLATE)
- [Path parameters](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/list#body.PATH_PARAMETERS)
- [Query parameters](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/list#body.QUERY_PARAMETERS)
- [Request body](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/list#body.request_body)
- [Response body](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/list#body.response_body)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/list#body.JobList.SCHEMA_REPRESENTATION)
- [Authorization scopes](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/list#body.aspect)
- [Try it!](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/list#try-it)

Lists all jobs that you started in the specified project. Job information is available for a six month period after creation. The job list is sorted in reverse chronological order, by job creation time. Requires the Can View project role, or the Is Owner project role if you set the allUsers property.

### IAM Permissions

Requires no specific IAM permission(s) to use this method. Users are able to list the jobs they created.

Additional access is granted based on the following permissions:

- Users with the `bigquery.jobs.listAll` permission can list all jobs with all metadata.
- Users with the `bigquery.jobs.list` permission can list all jobs, but with redacted information for jobs they did not create.

### HTTP request

`GET https://bigquery.googleapis.com/bigquery/v2/projects/{projectId}/jobs`

The URL uses [gRPC Transcoding](https://google.aip.dev/127) syntax.

### Path parameters

| Parameters  |                                          |
|-------------|------------------------------------------|
| `projectId` | `string` Project ID of the jobs to list. |

### Query parameters

<table>
<colgroup>
<col style="width: 50%" />
<col style="width: 50%" />
</colgroup>
<thead>
<tr class="header">
<th>Parameters</th>
<th></th>
</tr>
</thead>
<tbody>
<tr class="odd">
<td><code>allUsers</code></td>
<td><p><code>boolean</code></p>
<p>Whether to display jobs owned by all users in the project. Default False.</p></td>
</tr>
<tr class="even">
<td><code>maxResults</code></td>
<td><p><code>integer</code></p>
<p>The maximum number of results to return in a single response page. Leverage the page tokens to iterate through the entire collection.</p></td>
</tr>
<tr class="odd">
<td><code>minCreationTime</code></td>
<td><p><code>string</code></p>
<p>Min value for job creation time, in milliseconds since the POSIX epoch. If set, only jobs created after or at this timestamp are returned.</p></td>
</tr>
<tr class="even">
<td><code>maxCreationTime</code></td>
<td><p><code>string ( </code><a href="https://developers.google.com/discovery/v1/type-format"><code>UInt64Value</code></a><code> format)</code></p>
<p>Max value for job creation time, in milliseconds since the POSIX epoch. If set, only jobs created before or at this timestamp are returned.</p></td>
</tr>
<tr class="odd">
<td><code>pageToken</code></td>
<td><p><code>string</code></p>
<p>Page token, returned by a previous call, to request the next page of results.</p></td>
</tr>
<tr class="even">
<td><code>projection</code></td>
<td><p><code>enum</code></p>
<p>Restrict information returned to a set of selected fields</p>
<p>Valid values of this enum field are:</p>
<ul>
<li><code>MINIMAL</code></li>
<li><code>FULL</code></li>
</ul></td>
</tr>
<tr class="odd">
<td><code>stateFilter[]</code></td>
<td><p><code>enum</code></p>
<p>Filter for job state</p>
<p>Valid values of this enum field are:</p>
<ul>
<li><code>DONE</code></li>
<li><code>PENDING</code></li>
<li><code>RUNNING</code></li>
</ul></td>
</tr>
<tr class="even">
<td><code>parentJobId</code></td>
<td><p><code>string</code></p>
<p>If set, show only child jobs of the specified parent. Otherwise, show all top-level jobs.</p></td>
</tr>
</tbody>
</table>

### Request body

The request body must be empty.

### Response body

JobList is the response format for a jobs.list call.

If successful, the response body contains data with the following structure:

**JSON representation**

```
{
  "etag": string,
  "kind": string,
  "nextPageToken": string,
  "jobs": [
    {
      "id": string,
      "kind": string,
      "jobReference": {
        object (JobReference)
      },
      "state": string,
      "errorResult": {
        object (ErrorProto)
      },
      "statistics": {
        object (JobStatistics)
      },
      "configuration": {
        object (JobConfiguration)
      },
      "status": {
        object (JobStatus)
      },
      "user_email": string,
      "principal_subject": string
    }
  ],
  "unreachable": [
    string
  ]
}
```

| Fields                     |                                                                                                                                                                                                               |
|----------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `etag`                     | `string` A hash of this page of results.                                                                                                                                                                      |
| `kind`                     | `string` The resource type of the response.                                                                                                                                                                   |
| `nextPageToken`            | `string` A token to request the next page of results.                                                                                                                                                         |
| `jobs[]`                   | `object` tabledata.list of jobs that were requested.                                                                                                                                                          |
| `jobs[].id`                | `string` Unique opaque ID of the job.                                                                                                                                                                         |
| `jobs[].kind`              | `string` The resource type.                                                                                                                                                                                   |
| `jobs[].jobReference`      | `object ( `[`JobReference`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/JobReference)` )` Unique opaque ID of the job.                                                                      |
| `jobs[].state`             | `string` Running state of the job. When the state is DONE, errorResult can be checked to determine whether the job succeeded or failed.                                                                       |
| `jobs[].errorResult`       | `object ( `[`ErrorProto`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/ErrorProto)` )` A result object that will be present only if the job has failed.                                      |
| `jobs[].statistics`        | `object ( `[`JobStatistics`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/Job#JobStatistics)` )` Output only. Information about the job, including starting time and ending time of the job. |
| `jobs[].configuration`     | `object ( `[`JobConfiguration`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/Job#JobConfiguration)` )` Required. Describes the job configuration.                                            |
| `jobs[].status`            | `object ( `[`JobStatus`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/Job#JobStatus)` )` \[Full-projection-only\] Describes the status of this job.                                          |
| `jobs[].user_email`        | `string` \[Full-projection-only\] Email address of the user who ran the job.                                                                                                                                  |
| `jobs[].principal_subject` | `string` \[Full-projection-only\] String representation of identity of requesting party. Populated for both first- and third-party identities. Only present for APIs that support third-party identities.     |
| `unreachable[]`            | `string` A list of skipped locations that were unreachable. For more information about BigQuery locations, see: <https://cloud.google.com/bigquery/docs/locations> . Example: "europe-west5"                  |

### Authorization scopes

Requires one of the following OAuth scopes:

- `https://www.googleapis.com/auth/bigquery`
- `https://www.googleapis.com/auth/cloud-platform`
- `https://www.googleapis.com/auth/bigquery.readonly`
- `https://www.googleapis.com/auth/cloud-platform.read-only`

For more information, see the [Authentication Overview](https://docs.cloud.google.com/docs/authentication#authorization-gcp) .
