# System variables reference

BigQuery supports the following system variables for [multi-statement queries](https://docs.cloud.google.com/bigquery/docs/multi-statement-queries) or within [sessions](https://docs.cloud.google.com/bigquery/docs/sessions-intro) . You can use system variables to set or retrieve information during query execution, similar to user-defined [procedural language variables](https://docs.cloud.google.com/bigquery/docs/multi-statement-queries#variables) .

<table>
<colgroup>
<col style="width: 25%" />
<col style="width: 25%" />
<col style="width: 25%" />
<col style="width: 25%" />
</colgroup>
<thead>
<tr class="header">
<th>Name</th>
<th>Type</th>
<th>Read and write or read-only</th>
<th>Description</th>
</tr>
</thead>
<tbody>
<tr class="odd">
<td><code>@@current_job_id</code></td>
<td><code>STRING</code></td>
<td>Read-only</td>
<td>Job ID of the currently executing job. In the context of a multi-statement query, this returns the job responsible for the current statement, not the entire multi-statement query.</td>
</tr>
<tr class="even">
<td><code>@@dataset_id</code></td>
<td><code>STRING</code></td>
<td>Read and write</td>
<td>ID of the default dataset in the current project. This ID is used when a dataset is not specified for a project in the query. You can use the <code>SET</code> statement to assign <code>@@dataset_id</code> to another dataset ID in the current project. The system variables <code>@@dataset_project_id</code> and <code>@@dataset_id</code> can be set and used together.</td>
</tr>
<tr class="odd">
<td><code>@@dataset_project_id</code></td>
<td><code>STRING</code></td>
<td>Read and write</td>
<td>ID of the default project that's used when one is not specified for a dataset used in the query. If <code>@@dataset_project_id</code> is not set, or if it is set to <code>NULL</code> , the query-executing project ( <code>@@project_id</code> ) is used. You can use the <code>SET</code> statement to assign <code>@@dataset_project_id</code> to another project ID. The system variables <code>@@dataset_project_id</code> and <code>@@dataset_id</code> can be set and used together.</td>
</tr>
<tr class="even">
<td><code>@@last_job_id</code></td>
<td><code>STRING</code></td>
<td>Read-only</td>
<td>Job ID of the most recent job to execute in the current multi-statement query, not including the current one. If the multi-statement query contains <code>CALL</code> statements, this job may have originated in a different procedure.</td>
</tr>
<tr class="odd">
<td><code>@@location</code></td>
<td><code>STRING</code></td>
<td>Read and write</td>
<td>The location in which to run the query. <code>@@location</code> can only be set to a string literal with a <a href="https://docs.cloud.google.com/bigquery/docs/locations#supported_locations">valid location</a> . A <code>SET @@location</code> statement must be the first statement in a query. An error occurs if there is a mismatch between <code>@@location</code> and another <a href="https://docs.cloud.google.com/bigquery/docs/locations#specify_locations">location setting</a> for the query. You can improve the latency of queries that set <code>@@location</code> by using <a href="https://docs.cloud.google.com/bigquery/docs/running-queries#optional-job-creation">optional job creation mode</a> . You can use the <code>@@location</code> system variable inside of <a href="https://docs.cloud.google.com/bigquery/docs/user-defined-functions#sql-udf-structure">SQL UDFs</a> and <a href="https://docs.cloud.google.com/bigquery/docs/table-functions">table functions</a> .</td>
</tr>
<tr class="even">
<td><code>@@max_staleness_override</code></td>
<td><code>INTERVAL</code></td>
<td>Read and write</td>
<td>Overrides the maximum staleness interval for all applicable entities queried within the current multi-statement query or session. Applicable entities include <a href="https://docs.cloud.google.com/bigquery/docs/write-api-grpc#query-streamed-data-max-staleness">streamed tables</a> , <a href="https://docs.cloud.google.com/bigquery/docs/change-data-capture#query-max-staleness">Change Data Capture (CDC) tables</a> , and <a href="https://docs.cloud.google.com/bigquery/docs/materialized-views-create#max_staleness">materialized views</a> ) queried within the current multi-statement query or session.
<p>By default, every time you run a query, BigQuery returns the most up-to-date results or respects the entity's configured <code>max_staleness</code> option. You can use <code>@@max_staleness_override</code> to dynamically customize the staleness behavior without altering table or view configurations:</p>
<ul>
<li><strong>Reduce query latency and costs:</strong> to tolerate stale data, set <code>@@max_staleness_override</code> to a non-negative interval—for example, <code>INTERVAL 10 MINUTE</code> .</li>
<li><strong>Ensure completely up-to-date results:</strong> to force pending modifications or refreshes to be applied at query run time, set <code>@@max_staleness_override</code> to <code>INTERVAL 0 SECOND</code> .</li>
<li><strong>Clear the override:</strong> to revert back to the entity-level <code>max_staleness</code> settings, set <code>@@max_staleness_override</code> to <code>NULL</code> .</li>
</ul></td>
</tr>
<tr class="odd">
<td><code>@@project_id</code></td>
<td><code>STRING</code></td>
<td>Read-only</td>
<td>ID of the project used to execute the current query. In the context of a procedure, <code>@@project_id</code> refers to the project that is running the multi-statement query, not the project which owns the procedure.</td>
</tr>
<tr class="even">
<td><code>@@query_label</code></td>
<td><code>STRING</code></td>
<td>Read and write</td>
<td>Query label to associate with query jobs in the current multi-statement query or session. If set in a query, all subsequent query jobs in the script or session will have this label. If not set in a query, the value for this system variable is <code>NULL</code> . For an example of how to set this system variable, see <a href="https://docs.cloud.google.com/bigquery/docs/adding-labels#adding-label-to-session">Associate jobs in a session with a label</a> .</td>
</tr>
<tr class="odd">
<td><code>@@reservation</code></td>
<td><code>STRING</code></td>
<td>Read and write</td>
<td>Lets you specify or override the reservation to use for running the following statements. Must be in the following format: <code>projects/ </code><var translate="no"> project_id </var><code> /locations/ </code><var translate="no"> location </var><code> /reservations/ </code><var translate="no"> reservation_id</var> .
<p>Forces the query to use on-demand billing when set to <code>'none'</code> . This requires the project or organization to have <code>reservation_override_mode</code> set to <code>ALLOW_ANY_OVERRIDE</code> .</p>
<p>The location of the reservation must match the location where the query is running. If <code>@@reservation</code> is <code>NULL</code> , the reservation is automatically detected based on <a href="https://docs.cloud.google.com/bigquery/docs/reservations-assignments">assignment settings</a> matching the query properties.</p></td>
</tr>
<tr class="even">
<td><code>@@row_count</code></td>
<td><code>INT64</code></td>
<td>Read-only</td>
<td>If used in a multi-statement query and the previous statement is DML, specifies the number of rows inserted, modified, or deleted, as a result of that DML statement. If the previous statement is a `MERGE` statement, <code>@@row_count</code> represents the combined total number of rows inserted, modified, and deleted. This value is <code>NULL</code> if not in a multi-statement query.</td>
</tr>
<tr class="odd">
<td><code>@@script.bytes_billed</code></td>
<td><code>INT64</code></td>
<td>Read-only</td>
<td>Total bytes billed so far in the currently executing multi-statement query job. This value is <code>NULL</code> if not in the job.</td>
</tr>
<tr class="even">
<td><code>@@script.bytes_processed</code></td>
<td><code>INT64</code></td>
<td>Read-only</td>
<td>Total bytes processed so far in the currently executing multi-statement query job. This value is <code>NULL</code> if not in the job.</td>
</tr>
<tr class="odd">
<td><code>@@script.creation_time</code></td>
<td><code>TIMESTAMP</code></td>
<td>Read-only</td>
<td>Creation time of the currently executing multi-statement query job. This value is <code>NULL</code> if not in the job.</td>
</tr>
<tr class="even">
<td><code>@@script.job_id</code></td>
<td><code>STRING</code></td>
<td>Read-only</td>
<td>Job ID of the currently executing multi-statement query job. This value is <code>NULL</code> if not in the job.</td>
</tr>
<tr class="odd">
<td><code>@@script.num_child_jobs</code></td>
<td><code>INT64</code></td>
<td>Read-only</td>
<td>Number of currently completed child jobs. This value is <code>NULL</code> if not in the job.</td>
</tr>
<tr class="even">
<td><code>@@script.slot_ms</code></td>
<td><code>INT64</code></td>
<td>Read-only</td>
<td>Number of slot milliseconds used so far by the script. This value is <code>NULL</code> if not in the job.</td>
</tr>
<tr class="odd">
<td><code>@@session_id</code></td>
<td><code>STRING</code></td>
<td>Read-only</td>
<td>ID of the session that the current query is associated with. You can use the <code>@@session_id</code> system variable within <a href="https://docs.cloud.google.com/bigquery/docs/user-defined-functions#sql-udf-structure">SQL user-defined functions</a> , <a href="https://docs.cloud.google.com/bigquery/docs/table-functions">table functions</a> , and <a href="https://docs.cloud.google.com/bigquery/docs/views">logical views</a> . The use of this system variable in materialized views isn't supported.</td>
</tr>
<tr class="even">
<td><code>@@time_zone</code></td>
<td><code>STRING</code></td>
<td>Read and write</td>
<td>The default time zone to use in time zone-dependent SQL functions, when a time zone is not specified as an argument. <code>@@time_zone</code> can be modified by using a <code>SET</code> statement to any valid time zone name. At the start of each script, <code>@@time_zone</code> begins as “UTC”.</td>
</tr>
</tbody>
</table>

For backward compatibility, expressions used in an `OPTIONS` or `FOR SYSTEM TIME AS OF` clause default to the `America/Los_Angeles` time zone, while all other date/time expressions default to the `UTC` time zone. If `@@time_zone` has been set earlier in the multi-statement query, the chosen time zone will apply to all date/time expressions, including `OPTIONS` and `FOR SYSTEM TIME AS OF` clauses.

In addition to the system variables shown previously, you can use `EXCEPTION` system variables during execution of a multi-statement query. For more information about the `EXCEPTION` system variables, see the procedural language statement [BEGIN...EXCEPTION](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/procedural-language#beginexceptionend) .

## Examples

You don't create system variables, but you can override the default value for some of them:

```
SET @@dataset_project_id = 'MyProject';
```

The following example overrides the maximum staleness interval for subsequent queries in the session to 10 minutes:

```
SET @@max_staleness_override = INTERVAL 10 MINUTE;
```

To force queries to return the freshest, fully up-to-date results and apply pending modifications at query run time:

```
SET @@max_staleness_override = INTERVAL 0 SECOND;
```

To reset the staleness override back to the table-level settings, set `@@max_staleness_override` to `NULL` :

```
SET @@max_staleness_override = NULL;
```

The following query returns the default time zone:

```
SELECT @@time_zone AS default_time_zone;
```

```
+-------------------+
| default_time_zone |
+-------------------+
| UTC               |
+-------------------+
```

You can use system variables with DDL and DML queries. For example, here are a few ways to use the system variable `@@time_zone` when creating and updating a table:

```
BEGIN
  CREATE TEMP TABLE MyTempTable
  AS SELECT @@time_zone AS default_time_zone;
END;
```

```
CREATE OR REPLACE TABLE MyDataset.MyTable(default_time_zone STRING)
  OPTIONS (description = @@time_zone);
```

```
UPDATE MyDataset.MyTable
SET default_time_zone = @@time_zone
WHERE TRUE;
```

There are some places where system variables can't be used in DDL and DML queries. For example, you can't use a system variable as a project name, dataset, or table name. The following query produces an error when you include the `@@dataset_id` system variable in a table path:

```
BEGIN
  CREATE TEMP TABLE @@dataset_id.MyTempTable (id STRING);
END;
```

For more examples of how you can use system variables in multi-statement queries, see [Set a variable](https://docs.cloud.google.com/bigquery/docs/multi-statement-queries#set_system_variable) .

For examples of how you can use system variables in sessions, see [Example session](https://docs.cloud.google.com/bigquery/docs/sessions-write-queries#session_system_variables) .
