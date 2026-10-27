# ROUTINES view

The `INFORMATION_SCHEMA.ROUTINES` view contains one row for each routine in a dataset.

## Required permissions

To query the `INFORMATION_SCHEMA.ROUTINES` view, you need the following Identity and Access Management (IAM) permissions:

- `bigquery.routines.get`
- `bigquery.routines.list`

Each of the following predefined IAM roles includes the permissions that you need in order to get routine metadata:

- `roles/bigquery.admin`
- `roles/bigquery.metadataViewer`
- `roles/bigquery.dataViewer`

For more information about BigQuery permissions, see [Access control with IAM](https://docs.cloud.google.com/bigquery/docs/access-control) .

## Schema

When you query the `INFORMATION_SCHEMA.ROUTINES` view, the query results contain one row for each routine in a dataset.

The `INFORMATION_SCHEMA.ROUTINES` view has the following schema:

<table>
<colgroup>
<col style="width: 33%" />
<col style="width: 33%" />
<col style="width: 33%" />
</colgroup>
<tbody>
<tr class="header">
<th>Column name</th>
<th>Data type</th>
<th>Value</th>
</tr>

<tr class="odd">
<td><code>specific_catalog</code></td>
<td><code>STRING</code></td>
<td>The name of the project that contains the dataset where the routine is defined</td>
</tr>
<tr class="even">
<td><code>specific_schema</code></td>
<td><code>STRING</code></td>
<td>The name of the dataset that contains the routine</td>
</tr>
<tr class="odd">
<td><code>specific_name</code></td>
<td><code>STRING</code></td>
<td>The name of the routine</td>
</tr>
<tr class="even">
<td><code>routine_catalog</code></td>
<td><code>STRING</code></td>
<td>The name of the project that contains the dataset where the routine is defined</td>
</tr>
<tr class="odd">
<td><code>routine_schema</code></td>
<td><code>STRING</code></td>
<td>The name of the dataset that contains the routine</td>
</tr>
<tr class="even">
<td><code>routine_name</code></td>
<td><code>STRING</code></td>
<td>The name of the routine</td>
</tr>
<tr class="odd">
<td><code>routine_type</code></td>
<td><code>STRING</code></td>
<td>The routine type:<br />

<ul>
<li><code>FUNCTION</code> : A BigQuery persistent user-defined function</li>
<li><code>AGGREGATE FUNCTION</code> : A BigQuery persistent user-defined aggregate function</li>
<li><code>PROCEDURE</code> : A BigQuery stored procedure</li>
<li><code>TABLE FUNCTION</code> : A BigQuery table function</li>
</ul></td>
</tr>
<tr class="even">
<td><code>data_type</code></td>
<td><code>STRING</code></td>
<td>The data type that the routine returns. <code>NULL</code> if the routine is a stored procedure</td>
</tr>
<tr class="odd">
<td><code>routine_body</code></td>
<td><code>STRING</code></td>
<td>How the body of the routine is defined, either <code>SQL</code> or <code>EXTERNAL</code> if the routine is a JavaScript user-defined function</td>
</tr>
<tr class="even">
<td><code>routine_definition</code></td>
<td><code>STRING</code></td>
<td>The definition of the routine</td>
</tr>
<tr class="odd">
<td><code>external_language</code></td>
<td><code>STRING</code></td>
<td><code>JAVASCRIPT</code> if the routine is a JavaScript user-defined function or <code>NULL</code> if the routine was defined with SQL</td>
</tr>
<tr class="even">
<td><code>is_deterministic</code></td>
<td><code>STRING</code></td>
<td><code>YES</code> if the routine is known to be deterministic, <code>NO</code> if it is not, or <code>NULL</code> if unknown</td>
</tr>
<tr class="odd">
<td><code>security_type</code></td>
<td><code>STRING</code></td>
<td>Security type of the routine, always <code>NULL</code></td>
</tr>
<tr class="even">
<td><code>created</code></td>
<td><code>TIMESTAMP</code></td>
<td>The routine's creation time</td>
</tr>
<tr class="odd">
<td><code>last_altered</code></td>
<td><code>TIMESTAMP</code></td>
<td>The routine's last modification time</td>
</tr>
<tr class="even">
<td><code>ddl</code></td>
<td><code>STRING</code></td>
<td>The <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language">DDL statement</a> that can be used to create the routine, such as <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#create_function_statement"><code>CREATE FUNCTION</code></a> or <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#create_procedure"><code>CREATE PROCEDURE</code></a></td>
</tr>
<tr class="odd">
<td><code>connection</code></td>
<td><code>STRING</code></td>
<td>The connection name, if the routine has one. Otherwise <code>NULL</code></td>
</tr>
</tbody>
</table>

For stability, we recommend that you explicitly list columns in your information schema queries instead of using a wildcard ( `SELECT *` ). Explicitly listing columns prevents queries from breaking if the underlying schema changes.

## Scope and syntax

Queries against this view must include a dataset or a region qualifier. For more information see [Syntax](https://docs.cloud.google.com/bigquery/docs/information-schema-intro#syntax) . The following table explains the region and resource scopes for this view:

| View name                                                                          | Resource scope | Region scope     |
|------------------------------------------------------------------------------------|----------------|------------------|
| `[ `` PROJECT_ID ```  .]`region-  ``` REGION ```  `.INFORMATION_SCHEMA.ROUTINES `` | Project level  | `REGION`         |
| `[ `` PROJECT_ID `` .] `` DATASET_ID `` .INFORMATION_SCHEMA.ROUTINES`              | Dataset level  | Dataset location |

Replace the following:

- Optional: `PROJECT_ID` : the ID of your Google Cloud project. If not specified, the default project is used.

- `REGION` : any [dataset region name](https://docs.cloud.google.com/bigquery/docs/locations) . For example, `` `region-us` `` .

- `DATASET_ID` : the ID of your dataset. For more information, see [Dataset qualifier](https://docs.cloud.google.com/bigquery/docs/information-schema-intro#dataset_qualifier) .

  > **Note:** You must use [a region qualifier](https://docs.cloud.google.com/bigquery/docs/information-schema-intro#region_qualifier) to query `INFORMATION_SCHEMA` views. The location of the query execution must match the region of the `INFORMATION_SCHEMA` view.

**Example**

```
-- Returns metadata for routines in a single dataset.
SELECT * FROM myDataset.INFORMATION_SCHEMA.ROUTINES;

-- Returns metadata for routines in a region.
SELECT * FROM region-us.INFORMATION_SCHEMA.ROUTINES;
```

## Example

#### Example

To run the query against a project other than your default project, add the project ID to the dataset in the following format:

```
`PROJECT_ID`.INFORMATION_SCHEMA.ROUTINES
```

. For example, `` `myproject`.INFORMATION_SCHEMA.ROUTINES `` .

The following example retrieves all columns from the `INFORMATION_SCHEMA.ROUTINES` view. The metadata returned is for all routines in `mydataset` in your default project — `myproject` . The dataset `mydataset` contains a routine named `myroutine1` .

```
SELECT
  *
FROM
  mydataset.INFORMATION_SCHEMA.ROUTINES;
```

> **Note:** `INFORMATION_SCHEMA` view names are case-sensitive.

The result is similar to the following:

```
+------------------+-----------------+---------------+-----------------+----------------+--------------+--------------+-----------+--------------+--------------------+-------------------+------------------+---------------+-----------------------------+-----------------------------+-----------------------------------------------------------+
| specific_catalog | specific_schema | specific_name | routine_catalog | routine_schema | routine_name | routine_type | data_type | routine_body | routine_definition | external_language | is_deterministic | security_type |           created           |         last_altered        |                            ddl                             |
+------------------+-----------------+---------------+-----------------+----------------+--------------+--------------+-----------+--------------+--------------------+-------------------+------------------+---------------+-----------------------------+-----------------------------+-----------------------------------------------------------+
| myproject        | mydataset       | myroutine1    | myproject       | mydataset      | myroutine1   | FUNCTION     | NULL      | SQL          | x + 3              | NULL              | NULL             | NULL          | 2019-10-03 17:29:00.235 UTC | 2019-10-03 17:29:00.235 UTC | CREATE FUNCTION myproject.mydataset.myroutine1(x FLOAT64) |
|                  |                 |               |                 |                |              |              |           |              |                    |                   |                  |               |                             |                             | AS (                                                      |
|                  |                 |               |                 |                |              |              |           |              |                    |                   |                  |               |                             |                             | x + 3                                                     |
|                  |                 |               |                 |                |              |              |           |              |                    |                   |                  |               |                             |                             | );                                                        |
+------------------+-----------------+---------------+-----------------+----------------+--------------+--------------+-----------+--------------+--------------------+-------------------+------------------+---------------+-----------------------------+-----------------------------+-----------------------------------------------------------+
```
