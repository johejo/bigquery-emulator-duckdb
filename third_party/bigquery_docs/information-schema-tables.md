# TABLES view

The `INFORMATION_SCHEMA.TABLES` view contains one row for each table or view in a dataset. The `TABLES` and `TABLE_OPTIONS` views also contain high-level information about views. For detailed information, query the [`INFORMATION_SCHEMA.VIEWS`](https://docs.cloud.google.com/bigquery/docs/information-schema-views) view.

## Required permissions

To query the `INFORMATION_SCHEMA.TABLES` view, you need the following Identity and Access Management (IAM) permissions:

- `bigquery.tables.get`
- `bigquery.tables.list`
- `bigquery.routines.get`
- `bigquery.routines.list`

Each of the following predefined IAM roles includes the preceding permissions:

- `roles/bigquery.admin`
- `roles/bigquery.dataViewer`
- `roles/bigquery.metadataViewer`

For more information about BigQuery permissions, see [Access control with IAM](https://docs.cloud.google.com/bigquery/docs/access-control) .

## Schema

When you query the `INFORMATION_SCHEMA.TABLES` view, the query results contain one row for each table or view in a dataset. For detailed information about views, query the [`INFORMATION_SCHEMA.VIEWS` view](https://docs.cloud.google.com/bigquery/docs/information-schema-views) instead.

The `INFORMATION_SCHEMA.TABLES` view has the following schema:

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
<td><code>table_catalog</code></td>
<td><code>STRING</code></td>
<td>The project ID of the project that contains the dataset.</td>
</tr>
<tr class="even">
<td><code>table_schema</code></td>
<td><code>STRING</code></td>
<td>The name of the dataset that contains the table or view. Also referred to as the <code>datasetId</code> .</td>
</tr>
<tr class="odd">
<td><code>table_name</code></td>
<td><code>STRING</code></td>
<td>The name of the table or view. Also referred to as the <code>tableId</code> .</td>
</tr>
<tr class="even">
<td><code>table_type</code></td>
<td><code>STRING</code></td>
<td>The table type; one of the following:<br />

<ul>
<li><code>BASE TABLE</code> : A standard <a href="https://docs.cloud.google.com/bigquery/docs/tables-intro">table</a></li>
<li><code>CLONE</code> : A <a href="https://docs.cloud.google.com/bigquery/docs/table-clones-intro">table clone</a></li>
<li><code>SNAPSHOT</code> : A <a href="https://docs.cloud.google.com/bigquery/docs/table-snapshots-intro">table snapshot</a></li>
<li><code>VIEW</code> : A <a href="https://docs.cloud.google.com/bigquery/docs/views-intro">view</a></li>
<li><code>MATERIALIZED VIEW</code> : A <a href="https://docs.cloud.google.com/bigquery/docs/materialized-views-intro">materialized view</a> or <a href="https://docs.cloud.google.com/bigquery/docs/load-data-using-cross-cloud-transfer#materialized_view_replicas">materialized view replica</a></li>
<li><code>EXTERNAL</code> : A table that references an <a href="https://docs.cloud.google.com/bigquery/external-data-sources">external data source</a></li>
</ul></td>
</tr>
<tr class="odd">
<td><code>managed_table_type</code></td>
<td><code>STRING</code></td>
<td>This column is in Preview. The managed table type; one of the following:<br />

<ul>
<li><code>NATIVE</code> : A standard <a href="https://docs.cloud.google.com/bigquery/docs/tables-intro">table</a></li>
<li><code>BIGLAKE</code> : A <a href="https://docs.cloud.google.com/bigquery/docs/iceberg-tables">Apache Iceberg managed table</a></li>
</ul></td>
</tr>
<tr class="even">
<td><code>is_insertable_into</code></td>
<td><code>STRING</code></td>
<td><code>YES</code> or <code>NO</code> depending on whether the table supports <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/dml-syntax#insert_statement">DML INSERT</a> statements</td>
</tr>
<tr class="odd">
<td><code>is_fine_grained_mutations_enabled</code></td>
<td><code>STRING</code></td>
<td><code>YES</code> or <code>NO</code> depending on whether <a href="https://docs.cloud.google.com/bigquery/docs/data-manipulation-language#enable_fine-grained_dml">fine-grained DML mutations</a> are enabled on the table</td>
</tr>
<tr class="even">
<td><code>is_typed</code></td>
<td><code>STRING</code></td>
<td>The value is always <code>NO</code></td>
</tr>
<tr class="odd">
<td><code>is_change_history_enabled</code></td>
<td><code>STRING</code></td>
<td><code>YES</code> or <code>NO</code> depending on whether <a href="https://docs.cloud.google.com/bigquery/docs/change-history">change history</a> is enabled</td>
</tr>
<tr class="even">
<td><code>creation_time</code></td>
<td><code>TIMESTAMP</code></td>
<td>The table's creation time</td>
</tr>
<tr class="odd">
<td><code>base_table_catalog</code></td>
<td><code>STRING</code></td>
<td>For <a href="https://docs.cloud.google.com/bigquery/docs/table-clones-intro">table clones</a> and <a href="https://docs.cloud.google.com/bigquery/docs/table-snapshots-intro">table snapshots</a> , the base table's project. Applicable only to tables with <code>table_type</code> set to <code>CLONE</code> or <code>SNAPSHOT</code> .</td>
</tr>
<tr class="even">
<td><code>base_table_schema</code></td>
<td><code>STRING</code></td>
<td>For <a href="https://docs.cloud.google.com/bigquery/docs/table-clones-intro">table clones</a> and <a href="https://docs.cloud.google.com/bigquery/docs/table-snapshots-intro">table snapshots</a> , the base table's dataset. Applicable only to tables with <code>table_type</code> set to <code>CLONE</code> or <code>SNAPSHOT</code> .</td>
</tr>
<tr class="odd">
<td><code>base_table_name</code></td>
<td><code>STRING</code></td>
<td>For <a href="https://docs.cloud.google.com/bigquery/docs/table-clones-intro">table clones</a> and <a href="https://docs.cloud.google.com/bigquery/docs/table-snapshots-intro">table snapshots</a> , the base table's name. Applicable only to tables with <code>table_type</code> set to <code>CLONE</code> or <code>SNAPSHOT</code> .</td>
</tr>
<tr class="even">
<td><code>snapshot_time_ms</code></td>
<td><code>TIMESTAMP</code></td>
<td>For <a href="https://docs.cloud.google.com/bigquery/docs/table-clones-intro">table clones</a> and <a href="https://docs.cloud.google.com/bigquery/docs/table-snapshots-intro">table snapshots</a> , the time when the <a href="https://docs.cloud.google.com/bigquery/docs/table-clones-create">clone</a> or <a href="https://docs.cloud.google.com/bigquery/docs/table-snapshots-create">snapshot</a> operation was run on the base table to create this table. If <a href="https://docs.cloud.google.com/bigquery/docs/time-travel">time travel</a> was used, then this field contains the time travel timestamp. Otherwise, the <code>snapshot_time_ms</code> field is the same as the <code>creation_time</code> field. Applicable only to tables with <code>table_type</code> set to <code>CLONE</code> or <code>SNAPSHOT</code> .</td>
</tr>
<tr class="odd">
<td><code>replica_source_catalog</code></td>
<td><code>STRING</code></td>
<td>For <a href="https://docs.cloud.google.com/bigquery/docs/load-data-using-cross-cloud-transfer#materialized_view_replicas">materialized view replicas</a> , the base materialized view's project.</td>
</tr>
<tr class="even">
<td><code>replica_source_schema</code></td>
<td><code>STRING</code></td>
<td>For <a href="https://docs.cloud.google.com/bigquery/docs/load-data-using-cross-cloud-transfer#materialized_view_replicas">materialized view replicas</a> , the base materialized view's dataset.</td>
</tr>
<tr class="odd">
<td><code>replica_source_name</code></td>
<td><code>STRING</code></td>
<td>For <a href="https://docs.cloud.google.com/bigquery/docs/load-data-using-cross-cloud-transfer#materialized_view_replicas">materialized view replicas</a> , the base materialized view's name.</td>
</tr>
<tr class="even">
<td><code>replication_status</code></td>
<td><code>STRING</code></td>
<td>For <a href="https://docs.cloud.google.com/bigquery/docs/load-data-using-cross-cloud-transfer#materialized_view_replicas">materialized view replicas</a> , the status of the replication from the base materialized view to the materialized view replica; one of the following:<br />

<ul>
<li><code>REPLICATION_STATUS_UNSPECIFIED</code></li>
<li><code>ACTIVE</code> : Replication is active with no errors</li>
<li><code>SOURCE_DELETED</code> : The source materialized view has been deleted</li>
<li><code>PERMISSION_DENIED</code> : The source materialized view hasn't been <a href="https://docs.cloud.google.com/bigquery/docs/authorized-views">authorized</a> on the dataset that contains the source Amazon S3 BigLake tables used in the query that created the materialized view.</li>
<li><code>UNSUPPORTED_CONFIGURATION</code> : There is an issue with the replica's <a href="https://docs.cloud.google.com/bigquery/docs/load-data-using-cross-cloud-transfer#create">prerequisites</a> other than source materialized view authorization.</li>
</ul></td>
</tr>
<tr class="odd">
<td><code>replication_error</code></td>
<td><code>STRING</code></td>
<td>If <code>replication_status</code> indicates a replication issue for a <a href="https://docs.cloud.google.com/bigquery/docs/load-data-using-cross-cloud-transfer#materialized_view_replicas">materialized view replica</a> , <code>replication_error</code> provides further details about the issue.</td>
</tr>
<tr class="even">
<td><code>ddl</code></td>
<td><code>STRING</code></td>
<td>The <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language">DDL statement</a> that can be used to recreate the table, such as <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#create_table_statement"><code>CREATE TABLE</code></a> or <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#create_view_statement"><code>CREATE VIEW</code></a></td>
</tr>
<tr class="odd">
<td><code>default_collation_name</code></td>
<td><code>STRING</code></td>
<td>The name of the default <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/collation-concepts">collation specification</a> if it exists; otherwise, <code>NULL</code> .</td>
</tr>
<tr class="even">
<td><code>sync_status</code></td>
<td><code>JSON</code></td>
<td>The status of the sync between the primary and secondary replicas for <a href="https://docs.cloud.google.com/bigquery/docs/data-replication">cross-region replication</a> and <a href="https://docs.cloud.google.com/bigquery/docs/managed-disaster-recovery">disaster recovery</a> datasets. Returns <code>NULL</code> if the replica is a primary replica or the dataset doesn't use replication.</td>
</tr>
<tr class="odd">
<td><code>upsert_stream_apply_watermark</code></td>
<td><code>TIMESTAMP</code></td>
<td>For tables that use change data capture (CDC), the time when row modifications were last applied. For more information, see <a href="https://docs.cloud.google.com/bigquery/docs/change-data-capture#monitor_table_upsert_operation_progress">Monitor table upsert operation progress</a> .</td>
</tr>
</tbody>
</table>

For stability, we recommend that you explicitly list columns in your information schema queries instead of using a wildcard ( `SELECT *` ). Explicitly listing columns prevents queries from breaking if the underlying schema changes.

## Scope and syntax

Queries against this view must include a dataset or a region qualifier. For queries with a dataset qualifier, you must have permissions for the dataset. For queries with a region qualifier, you must have permissions for the project. For more information see [Syntax](https://docs.cloud.google.com/bigquery/docs/information-schema-intro#syntax) . The following table explains the region and resource scopes for this view:

| View name                                                                        | Resource scope | Region scope     |
|----------------------------------------------------------------------------------|----------------|------------------|
| `[ `` PROJECT_ID ```  .]`region-  ``` REGION ```  `.INFORMATION_SCHEMA.TABLES `` | Project level  | `REGION`         |
| `[ `` PROJECT_ID `` .] `` DATASET_ID `` .INFORMATION_SCHEMA.TABLES`              | Dataset level  | Dataset location |

Replace the following:

- Optional: `PROJECT_ID` : the ID of your Google Cloud project. If not specified, the default project is used.

- `REGION` : any [dataset region name](https://docs.cloud.google.com/bigquery/docs/locations) . For example, `` `region-us` `` .

- `DATASET_ID` : the ID of your dataset. For more information, see [Dataset qualifier](https://docs.cloud.google.com/bigquery/docs/information-schema-intro#dataset_qualifier) .

  > **Note:** You must use [a region qualifier](https://docs.cloud.google.com/bigquery/docs/information-schema-intro#region_qualifier) to query `INFORMATION_SCHEMA` views. The location of the query execution must match the region of the `INFORMATION_SCHEMA` view.

**Example**

```
-- Returns metadata for tables in a single dataset.
SELECT * FROM myDataset.INFORMATION_SCHEMA.TABLES;
```

## Examples

##### Example 1:

The following example retrieves table metadata for all of the tables in the dataset named `mydataset` . The metadata that's returned is for all types of tables in `mydataset` in your default project.

`mydataset` contains the following tables:

- `mytable1` : a standard BigQuery table
- `myview1` : a BigQuery view

To run the query against a project other than your default project, add the project ID to the dataset in the following format: `` `  ``` project_id ```  `.  ``` dataset `` .INFORMATION_SCHEMA. `` view` ; for example, `` `myproject`.mydataset.INFORMATION_SCHEMA.TABLES `` .

> **Note:** `INFORMATION_SCHEMA` view names are case-sensitive.

```
SELECT
  table_catalog, table_schema, table_name, table_type,
  is_insertable_into, creation_time, ddl
FROM
  mydataset.INFORMATION_SCHEMA.TABLES;
```

The result is similar to the following. For readability, some columns are excluded from the result.

```
+----------------+---------------+----------------+------------+--------------------+---------------------+---------------------------------------------+
| table_catalog  | table_schema  |   table_name   | table_type | is_insertable_into |    creation_time    |                     ddl                     |
+----------------+---------------+----------------+------------+--------------------+---------------------+---------------------------------------------+
| myproject      | mydataset     | mytable1       | BASE TABLE | YES                | 2018-10-29 20:34:44 | CREATE TABLE `myproject.mydataset.mytable1` |
|                |               |                |            |                    |                     | (                                           |
|                |               |                |            |                    |                     |   id INT64                                  |
|                |               |                |            |                    |                     | );                                          |
| myproject      | mydataset     | myview1        | VIEW       | NO                 | 2018-12-29 00:19:20 | CREATE VIEW `myproject.mydataset.myview1`   |
|                |               |                |            |                    |                     | AS SELECT 100 as id;                        |
+----------------+---------------+----------------+------------+--------------------+---------------------+---------------------------------------------+
```

##### Example 2:

The following example retrieves table metadata for all tables of type `CLONE` or `SNAPSHOT` from the `INFORMATION_SCHEMA.TABLES` view. The metadata returned is for tables in `mydataset` in your default project.

To run the query against a project other than your default project, add the project ID to the dataset in the following format: `` `  ``` project_id ```  `.  ``` dataset `` .INFORMATION_SCHEMA. `` view` ; for example, `` `myproject`.mydataset.INFORMATION_SCHEMA.TABLES `` .

```
SELECT
    table_name, table_type, base_table_catalog,
    base_table_schema, base_table_name, snapshot_time_ms
  FROM
    mydataset.INFORMATION_SCHEMA.TABLES
  WHERE
    table_type = 'CLONE'
  OR
    table_type = 'SNAPSHOT';
```

The result is similar to the following. For readability, some columns are excluded from the result.

```
  +--------------+------------+--------------------+-------------------+-----------------+---------------------+
  | table_name   | table_type | base_table_catalog | base_table_schema | base_table_name | snapshot_time_ms    |
  +--------------+------------+--------------------+-------------------+-----------------+---------------------+
  | items_clone  | CLONE      | myproject          | mydataset         | items           | 2018-10-31 22:40:05 |
  | orders_bk    | SNAPSHOT   | myproject          | mydataset         | orders          | 2018-11-01 08:22:39 |
  +--------------+------------+--------------------+-------------------+-----------------+---------------------+
```

##### Example 3:

The following example retrieves `table_name` and `ddl` columns from the `INFORMATION_SCHEMA.TABLES` view for the `population_by_zip_2010` table in the [`census_bureau_usa`](https://console.cloud.google.com/bigquery?p=bigquery-public-data&d=census_bureau_usa&page=dataset) dataset. This dataset is part of the BigQuery [public dataset program](https://docs.cloud.google.com/bigquery/public-data) .

Because the table you're querying is in another project, you add the project ID to the dataset in the following format: `` `  ``` project_id ```  `.  ``` dataset `` .INFORMATION_SCHEMA. `` view` . In this example, the value is `` `bigquery-public-data`.census_bureau_usa.INFORMATION_SCHEMA.TABLES `` .

```
SELECT
  table_name, ddl
FROM
  `bigquery-public-data`.census_bureau_usa.INFORMATION_SCHEMA.TABLES
WHERE
  table_name = 'population_by_zip_2010';
```

The result is similar to the following:

```
+------------------------+----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
|       table_name       |                                                                                                            ddl                                                                                                             |
+------------------------+----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
| population_by_zip_2010 | CREATE TABLE `bigquery-public-data.census_bureau_usa.population_by_zip_2010`                                                                                                                                               |
|                        | (                                                                                                                                                                                                                          |
|                        |   geo_id STRING OPTIONS(description="Geo code"),                                                                                                                                                                           |
|                        |   zipcode STRING NOT NULL OPTIONS(description="Five digit ZIP Code Tabulation Area Census Code"),                                                                                                                          |
|                        |   population INT64 OPTIONS(description="The total count of the population for this segment."),                                                                                                                             |
|                        |   minimum_age INT64 OPTIONS(description="The minimum age in the age range. If null, this indicates the row as a total for male, female, or overall population."),                                                          |
|                        |   maximum_age INT64 OPTIONS(description="The maximum age in the age range. If null, this indicates the row as having no maximum (such as 85 and over) or the row is a total of the male, female, or overall population."), |
|                        |   gender STRING OPTIONS(description="male or female. If empty, the row is a total population summary.")                                                                                                                    |
|                        | )                                                                                                                                                                                                                          |
|                        | OPTIONS(                                                                                                                                                                                                                   |
|                        |   labels=[("freebqcovid", "")]                                                                                                                                                                                             |
|                        | );                                                                                                                                                                                                                         |
+------------------------+----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+
  
```
