- [HTTP request](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/get#body.HTTP_TEMPLATE)
- [Path parameters](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/get#body.PATH_PARAMETERS)
- [Query parameters](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/get#body.QUERY_PARAMETERS)
- [Request body](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/get#body.request_body)
- [Response body](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/get#body.response_body)
- [Authorization scopes](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/get#body.aspect)
- [Try it!](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/jobs/get#try-it)

Returns information about a specific job. Job information is available for a six month period after creation. Requires that you're the person who ran the job, or have the Is Owner project role.

### IAM Permissions

Requires the `bigquery.jobs.get` permission on the job resource. If the user matches the creator of the job, the `bigquery.jobs.create` permission on the project is required instead.

### HTTP request

`GET https://bigquery.googleapis.com/bigquery/v2/projects/{projectId}/jobs/{jobId}`

The URL uses [gRPC Transcoding](https://google.aip.dev/127) syntax.

### Path parameters

| Parameters  |                                                     |
|-------------|-----------------------------------------------------|
| `projectId` | `string` Required. Project ID of the requested job. |
| `jobId`     | `string` Required. Job ID of the requested job.     |

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
<td><code>location</code></td>
<td><p><code>string</code></p>
<p>The geographic location of the job. You must specify the location to run the job for the following scenarios:</p>
<ul>
<li>If the location to run a job is not in the <code>us</code> or the <code>eu</code> multi-regional location</li>
<li>If the job's location is in a single region (for example, <code>us-central1</code> )</li>
</ul>
<p>For more information, see how to <a href="https://cloud.google.com/bigquery/docs/locations#specify_locations">specify locations</a> .</p></td>
</tr>
</tbody>
</table>

### Request body

The request body must be empty.

### Response body

If successful, the response body contains an instance of [`Job`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/Job) .

### Authorization scopes

Requires one of the following OAuth scopes:

- `https://www.googleapis.com/auth/bigquery`
- `https://www.googleapis.com/auth/cloud-platform`
- `https://www.googleapis.com/auth/bigquery.readonly`
- `https://www.googleapis.com/auth/cloud-platform.read-only`

For more information, see the [Authentication Overview](https://docs.cloud.google.com/docs/authentication#authorization-gcp) .
