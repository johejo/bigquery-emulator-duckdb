# TABLE_OPTIONS view

The `INFORMATION_SCHEMA.TABLE_OPTIONS` view contains one row for each option, for each table or view in a dataset. The `TABLES` and `TABLE_OPTIONS` views also contain high-level information about views. For detailed information, query the [`INFORMATION_SCHEMA.VIEWS`](https://docs.cloud.google.com/bigquery/docs/information-schema-views) view.

## Required permissions

To query the `INFORMATION_SCHEMA.TABLE_OPTIONS` view, you need the following Identity and Access Management (IAM) permissions:

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

When you query the `INFORMATION_SCHEMA.TABLE_OPTIONS` view, the query results contain one row for each option, for each table or view in a dataset. For detailed information about views, query the [`INFORMATION_SCHEMA.VIEWS` view](https://docs.cloud.google.com/bigquery/docs/information-schema-views) instead.

The `INFORMATION_SCHEMA.TABLE_OPTIONS` view has the following schema:

| Column name     | Data type | Value                                                                                                                                          |
|-----------------|-----------|------------------------------------------------------------------------------------------------------------------------------------------------|
| `table_catalog` | `STRING`  | The project ID of the project that contains the dataset                                                                                        |
| `table_schema`  | `STRING`  | The name of the dataset that contains the table or view also referred to as the `datasetId`                                                    |
| `table_name`    | `STRING`  | The name of the table or view also referred to as the `tableId`                                                                                |
| `option_name`   | `STRING`  | One of the name values in the [options table](https://docs.cloud.google.com/bigquery/docs/information-schema-table-options#options_table)      |
| `option_type`   | `STRING`  | One of the data type values in the [options table](https://docs.cloud.google.com/bigquery/docs/information-schema-table-options#options_table) |
| `option_value`  | `STRING`  | One of the value options in the [options table](https://docs.cloud.google.com/bigquery/docs/information-schema-table-options#options_table)    |

##### Options table

| `OPTION_NAME`               | `OPTION_TYPE`                   | `OPTION_VALUE`                                                                                                                                                                        |
|-----------------------------|---------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `description`               | `STRING`                        | A description of the table                                                                                                                                                            |
| `enable_refresh`            | `BOOL`                          | Whether automatic refresh is enabled for a materialized view                                                                                                                          |
| `expiration_timestamp`      | `TIMESTAMP`                     | The time when this table expires                                                                                                                                                      |
| `friendly_name`             | `STRING`                        | The table's descriptive name                                                                                                                                                          |
| `kms_key_name`              | `STRING`                        | The name of the Cloud KMS key used to encrypt the table                                                                                                                               |
| `labels`                    | `ARRAY<STRUCT<STRING, STRING>>` | An array of `STRUCT` 's that represent the labels on the table                                                                                                                        |
| `max_staleness`             | `INTERVAL`                      | The configured table's maximum staleness for [BigQuery change data capture (CDC) upserts](https://docs.cloud.google.com/bigquery/docs/change-data-capture#manage_table_staleness)     |
| `partition_expiration_days` | `FLOAT64`                       | The default lifetime, in days, of all partitions in a partitioned table                                                                                                               |
| `refresh_interval_minutes`  | `FLOAT64`                       | How frequently a materialized view is refreshed                                                                                                                                       |
| `require_partition_filter`  | `BOOL`                          | Whether queries over the table require a partition filter                                                                                                                             |
| `tags`                      | `ARRAY<STRUCT<STRING, STRING>>` | Tags attached to a table in a namespaced \<key, value\> syntax. For more information, see [Tags and conditional access](https://docs.cloud.google.com/iam/docs/tags-access-control) . |

For external tables, the following options are possible:

<table>
<colgroup>
<col style="width: 50%" />
<col style="width: 50%" />
</colgroup>
<tbody>
<tr class="header">
<th>Options</th>
<th></th>
</tr>

<tr class="odd">
<td><code>allow_jagged_rows</code></td>
<td><p><code>BOOL</code></p>
<p>If <code>true</code> , allow rows that are missing trailing optional columns.</p>
<p>Applies to CSV data.</p></td>
</tr>
<tr class="even">
<td><code>allow_quoted_newlines</code></td>
<td><p><code>BOOL</code></p>
<p>If <code>true</code> , allow quoted data sections that contain newline characters in the file.</p>
<p>Applies to CSV data.</p></td>
</tr>
<tr class="odd">
<td><code>bigtable_options</code></td>
<td><p><code>STRING</code></p>
<p>Only required when creating a Bigtable external table.</p>
<p>Specifies the schema of the Bigtable external table in JSON format.</p>
<p>For a list of Bigtable table definition options, see <a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/tables#bigtableoptions"><code>BigtableOptions</code></a> in the REST API reference.</p></td>
</tr>
<tr class="even">
<td><code>column_name_character_map</code></td>
<td><p><code>STRING</code></p>
<p>Defines the scope of supported column name characters and the handling behavior of unsupported characters. The default setting is <code>STRICT</code> , which means unsupported characters cause BigQuery to throw errors. <code>V1</code> and <code>V2</code> replace any unsupported characters with underscores.</p>
<p>Supported values include:</p>
<ul>
<li><code>STRICT</code> . Enables <a href="https://docs.cloud.google.com/bigquery/docs/schemas#flexible-column-names">flexible column names</a> . This is the default value. Load jobs with unsupported characters in column names fail with an error message. To configure the replacement of unsupported characters with underscores so that the load job succeeds, specify the <a href="https://docs.cloud.google.com/bigquery/docs/default-configuration"><code>default_column_name_character_map</code></a> configuration setting.</li>
<li><code>V1</code> . Column names can only contain <a href="https://docs.cloud.google.com/bigquery/docs/schemas#column_names">standard column name characters</a> . Unsupported characters (except <a href="https://docs.cloud.google.com/bigquery/docs/loading-data-cloud-storage-parquet#limitations_2">periods in Parquet file column names</a> ) are replaced with underscores. This is the default behavior for tables created before the introduction of <code>column_name_character_map</code> .</li>
<li><code>V2</code> . Besides <a href="https://docs.cloud.google.com/bigquery/docs/schemas#column_names">standard column name characters</a> , it also supports <a href="https://docs.cloud.google.com/bigquery/docs/schemas#flexible-column-names">flexible column names</a> . Unsupported characters (except <a href="https://docs.cloud.google.com/bigquery/docs/loading-data-cloud-storage-parquet#limitations_2">periods in Parquet file column names</a> ) are replaced with underscores.</li>
</ul>
<p>Applies to CSV and Parquet data.</p></td>
</tr>
<tr class="odd">
<td><code>compression</code></td>
<td><p><code>STRING</code></p>
<p>The compression type of the data source. Supported values include: <code>GZIP</code> . If not specified, the data source is uncompressed.</p>
<p>Applies to CSV and JSON data.</p></td>
</tr>
<tr class="even">
<td><code>decimal_target_types</code></td>
<td><p><code>ARRAY&lt;STRING&gt;</code></p>
<p>Determines how to convert a <code>Decimal</code> type. Equivalent to <a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/tables#ExternalDataConfiguration.FIELDS.decimal_target_types">ExternalDataConfiguration.decimal_target_types</a></p>
<p>Example: <code>["NUMERIC", "BIGNUMERIC"]</code> .</p></td>
</tr>
<tr class="odd">
<td><code>description</code></td>
<td><p><code>STRING</code></p>
<p>A description of this table.</p></td>
</tr>
<tr class="even">
<td><code>enable_list_inference</code></td>
<td><p><code>BOOL</code></p>
<p>If <code>true</code> , use schema inference specifically for Parquet LIST logical type.</p>
<p>Applies to Parquet data.</p></td>
</tr>
<tr class="odd">
<td><code>enable_logical_types</code></td>
<td><p><code>BOOL</code></p>
<p>If <code>true</code> , convert Avro logical types into their corresponding SQL types. For more information, see <a href="https://docs.cloud.google.com/bigquery/docs/loading-data-cloud-storage-avro#logical_types">Logical types</a> .</p>
<p>Applies to Avro data.</p></td>
</tr>
<tr class="even">
<td><code>encoding</code></td>
<td><p><code>STRING</code></p>
<p>The character encoding of the data. Supported values include: <code>UTF8</code> (or <code>UTF-8</code> ), <code>ISO_8859_1</code> (or <code>ISO-8859-1</code> ), <code>UTF-16BE</code> , <code>UTF-16LE</code> , <code>UTF-32BE</code> , or <code>UTF-32LE</code> . The default value is <code>UTF-8</code> .</p>
<p>Applies to CSV data.</p></td>
</tr>
<tr class="odd">
<td><code>enum_as_string</code></td>
<td><p><code>BOOL</code></p>
<p>If <code>true</code> , infer Parquet ENUM logical type as STRING instead of BYTES by default.</p>
<p>Applies to Parquet data.</p></td>
</tr>
<tr class="even">
<td><code>expiration_timestamp</code></td>
<td><p><code>TIMESTAMP</code></p>
<p>The time when this table expires. If not specified, the table does not expire.</p>
<p>Example: <code>"2025-01-01 00:00:00 UTC"</code> .</p></td>
</tr>
<tr class="odd">
<td><code>field_delimiter</code></td>
<td><p><code>STRING</code></p>
<p>The separator for fields in a CSV file.</p>
<p>Applies to CSV data.</p></td>
</tr>
<tr class="even">
<td><code>format</code></td>
<td><p><code>STRING</code></p>
<p>The format of the external data. Supported values for <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#create_external_table_statement"><code>CREATE EXTERNAL TABLE</code></a> include: <code>AVRO</code> , <code>CLOUD_BIGTABLE</code> , <code>CSV</code> , <code>DATASTORE_BACKUP</code> , <code>DELTA_LAKE</code> ( <a href="https://cloud.google.com/products/#product-launch-stages">preview</a> ), <code>GOOGLE_SHEETS</code> , <code>NEWLINE_DELIMITED_JSON</code> (or <code>JSON</code> ), <code>ORC</code> , <code>PARQUET</code> .</p>
<p>Supported values for <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/load-statements"><code>LOAD DATA</code></a> include: <code>AVRO</code> , <code>CSV</code> , <code>DELTA_LAKE</code> ( <a href="https://cloud.google.com/products/#product-launch-stages">preview</a> ) <code>NEWLINE_DELIMITED_JSON</code> (or <code>JSON</code> ), <code>ORC</code> , <code>PARQUET</code> .</p>
<p>The value <code>JSON</code> is equivalent to <code>NEWLINE_DELIMITED_JSON</code> .</p></td>
</tr>
<tr class="odd">
<td><code>hive_partition_uri_prefix</code></td>
<td><p><code>STRING</code></p>
<p>A common prefix for all source URIs before the partition key encoding begins. Applies only to hive-partitioned external tables.</p>
<p>Applies to Avro, CSV, JSON, Parquet, and ORC data.</p>
<p>Example: <code>"gs://bucket/path"</code> .</p></td>
</tr>
<tr class="even">
<td><code>file_set_spec_type</code></td>
<td><p><code>STRING</code></p>
<p>Specifies how to interpret source URIs for load jobs and external tables.</p>
<p>Supported values include:</p>
<ul>
<li><code>FILE_SYSTEM_MATCH</code> . Expands source URIs by listing files from the object store. This is the default behavior if FileSetSpecType is not set.</li>
<li><code>NEW_LINE_DELIMITED_MANIFEST</code> . Indicates that the provided URIs are newline-delimited manifest files, with one URI per line. Wildcard URIs are not supported in the manifest files, and all referenced data files must be in the same bucket as the manifest file.</li>
</ul>
<p>For example, if you have a source URI of <code>"gs://bucket/path/file"</code> and the <code>file_set_spec_type</code> is <code>FILE_SYSTEM_MATCH</code> , then the file is used directly as a data file. If the <code>file_set_spec_type</code> is <code>NEW_LINE_DELIMITED_MANIFEST</code> , then each line in the file is interpreted as a URI that points to a data file.</p></td>
</tr>
<tr class="odd">
<td><code>ignore_unknown_values</code></td>
<td><p><code>BOOL</code></p>
<p>If <code>true</code> , ignore extra values that are not represented in the table schema, without returning an error.</p>
<p>Applies to CSV and JSON data.</p></td>
</tr>
<tr class="even">
<td><code>json_extension</code></td>
<td><p><code>STRING</code></p>
<p>For JSON data, indicates a particular JSON interchange format. If not specified, BigQuery reads the data as generic JSON records.</p>
<p>Supported values include:<br />
<code>GEOJSON</code> . Newline-delimited GeoJSON data. For more information, see <a href="https://docs.cloud.google.com/bigquery/docs/geospatial-data#external-geojson">Creating an external table from a newline-delimited GeoJSON file</a> .</p></td>
</tr>
<tr class="odd">
<td><code>max_bad_records</code></td>
<td><p><code>INT64</code></p>
<p>The maximum number of bad records to ignore when reading the data.</p>
<p>Applies to: CSV, JSON, and Google Sheets data.</p></td>
</tr>
<tr class="even">
<td><code>max_staleness</code></td>
<td><p><code>INTERVAL</code></p>
<p>Applicable for <a href="https://docs.cloud.google.com/bigquery/docs/biglake-intro#metadata_caching_for_performance">BigLake tables</a> and <a href="https://docs.cloud.google.com/bigquery/docs/object-table-introduction#metadata_caching_for_performance">object tables</a> .</p>
<p>Specifies whether cached metadata is used by operations against the table, and how fresh the cached metadata must be in order for the operation to use it.</p>
<p>To disable metadata caching, specify 0. This is the default.</p>
<p>To enable metadata caching, specify an <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/lexical#interval_literals">interval literal</a> value between 30 minutes and 7 days. For example, specify <code>INTERVAL 4 HOUR</code> for a 4 hour staleness interval. With this value, operations against the table use cached metadata if it has been refreshed within the past 4 hours. If the cached metadata is older than that, the operation falls back to retrieving metadata from Cloud Storage instead.</p></td>
</tr>
<tr class="odd">
<td><code>null_marker</code></td>
<td><p><code>STRING</code></p>
<p>The string that represents <code>NULL</code> values in a CSV file.</p>
<p>Applies to CSV data.</p></td>
</tr>
<tr class="even">
<td><code>null_markers</code></td>
<td><p><code>ARRAY&lt;STRING&gt;</code></p>
<p>The list of strings that represent <code>NULL</code> values in a CSV file.</p>
<p>This option cannot be used with <code>null_marker</code> option.</p>
<p>Applies to CSV data.</p></td>
</tr>
<tr class="odd">
<td><code>object_metadata</code></td>
<td><p><code>STRING</code></p>
<p>Only required when creating an <a href="https://docs.cloud.google.com/bigquery/docs/object-table-introduction">object table</a> .</p>
<p>Set the value of this option to <code>SIMPLE</code> when creating an object table.</p></td>
</tr>
<tr class="even">
<td><code>preserve_ascii_control_characters</code></td>
<td><p><code>BOOL</code></p>
<p>If <code>true</code> , then the embedded ASCII control characters which are the first 32 characters in the ASCII table, ranging from '\x00' to '\x1F', are preserved.</p>
<p>Applies to CSV data.</p></td>
</tr>
<tr class="odd">
<td><code>projection_fields</code></td>
<td><p><code>STRING</code></p>
<p>A list of entity properties to load.</p>
<p>Applies to Datastore data.</p></td>
</tr>
<tr class="even">
<td><code>quote</code></td>
<td><p><code>STRING</code></p>
<p>The string used to quote data sections in a CSV file. If your data contains quoted newline characters, also set the <code>allow_quoted_newlines</code> property to <code>true</code> .</p>
<p>Applies to CSV data.</p></td>
</tr>
<tr class="odd">
<td><code>reference_file_schema_uri</code></td>
<td><p><code>STRING</code></p>
<p>User provided reference file with the table schema.</p>
<p>Applies to Parquet/ORC/AVRO data.</p>
<p>Example: <code>"gs://bucket/path/reference_schema_file.parquet"</code> .</p></td>
</tr>
<tr class="even">
<td><code>require_hive_partition_filter</code></td>
<td><p><code>BOOL</code></p>
<p>If <code>true</code> , all queries over this table require a partition filter that can be used to eliminate partitions when reading data. Applies only to hive-partitioned external tables.</p>
<p>Applies to Avro, CSV, JSON, Parquet, and ORC data.</p></td>
</tr>
<tr class="odd">
<td><code>sheet_range</code></td>
<td><p><code>STRING</code></p>
<p>Range of a Google Sheets spreadsheet to query from.</p>
<p>Applies to Google Sheets data.</p>
<p>Example: <code>"sheet1!A1:B20"</code> ,</p></td>
</tr>
<tr class="even">
<td><code>skip_leading_rows</code></td>
<td><p><code>INT64</code></p>
<p>The number of rows at the top of a file to skip when reading the data.</p>
<p>Applies to CSV and Google Sheets data.</p></td>
</tr>
<tr class="odd">
<td><code>source_column_match</code></td>
<td><p><code>STRING</code></p>
<p>This controls the strategy used to match loaded columns to the schema.</p>
<p>If this value is unspecified, then the default is based on how the schema is provided. If autodetect is enabled, then the default behavior is to match columns by name. Otherwise, the default is to match columns by position. This is done to keep the behavior backward-compatible.</p>
<p>Supported values include:</p>
<ul>
<li><code>POSITION</code> : matches by position. This option assumes that the columns are ordered the same way as the schema.</li>
<li><code>NAME</code> : matches by name. This option reads the header row as column names and reorders columns to match the field names in the schema. Column names are read from the last skipped row based on the <code>skip_leading_rows</code> property.</li>
</ul></td>
</tr>
<tr class="even">
<td><code>tags</code></td>
<td><code>&lt;ARRAY&lt;STRUCT&lt;STRING, STRING&gt;&gt;&gt;</code>
<p>An array of IAM tags for the table, expressed as key-value pairs. The key should be the <a href="https://docs.cloud.google.com/iam/docs/tags-access-control#definitions">namespaced key name</a> , and the value should be the <a href="https://docs.cloud.google.com/iam/docs/tags-access-control#definitions">short name</a> .</p></td>
</tr>
<tr class="odd">
<td><code>time_zone</code></td>
<td><p><code>STRING</code></p>
<p>Default time zone that will apply when parsing timestamp values that have no specific time zone.</p>
<p>Check <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-types#time_zone_name">valid time zone names</a> .</p>
<p>If this value is not present, the timestamp values without specific time zone is parsed using default time zone UTC.</p>
<p>Applies to CSV and JSON data.</p></td>
</tr>
<tr class="even">
<td><code>date_format</code></td>
<td><p><code>STRING</code></p>
<p><a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/format-elements#format_string_as_datetime">Format elements</a> that define how the DATE values are formatted in the input files (for example, <code>MM/DD/YYYY</code> ).</p>
<p>If this value is present, this format is the only compatible DATE format. <a href="https://docs.cloud.google.com/bigquery/docs/schema-detect#date_and_time_values">Schema autodetection</a> will also decide DATE column type based on this format instead of the existing format.</p>
<p>If this value is not present, the DATE field is parsed with the <a href="https://docs.cloud.google.com/bigquery/docs/loading-data-cloud-storage-csv#data_types">default formats</a> .</p>
<p>Applies to CSV and JSON data.</p></td>
</tr>
<tr class="odd">
<td><code>datetime_format</code></td>
<td><p><code>STRING</code></p>
<p><a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/format-elements#format_string_as_datetime">Format elements</a> that define how the DATETIME values are formatted in the input files (for example, <code>MM/DD/YYYY HH24:MI:SS.FF3</code> ).</p>
<p>If this value is present, this format is the only compatible DATETIME format. <a href="https://docs.cloud.google.com/bigquery/docs/schema-detect#date_and_time_values">Schema autodetection</a> will also decide DATETIME column type based on this format instead of the existing format.</p>
<p>If this value is not present, the DATETIME field is parsed with the <a href="https://docs.cloud.google.com/bigquery/docs/loading-data-cloud-storage-csv#data_types">default formats</a> .</p>
<p>Applies to CSV and JSON data.</p></td>
</tr>
<tr class="even">
<td><code>time_format</code></td>
<td><p><code>STRING</code></p>
<p><a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/format-elements#format_string_as_datetime">Format elements</a> that define how the TIME values are formatted in the input files (for example, <code>HH24:MI:SS.FF3</code> ).</p>
<p>If this value is present, this format is the only compatible TIME format. <a href="https://docs.cloud.google.com/bigquery/docs/schema-detect#date_and_time_values">Schema autodetection</a> will also decide TIME column type based on this format instead of the existing format.</p>
<p>If this value is not present, the TIME field is parsed with the <a href="https://docs.cloud.google.com/bigquery/docs/loading-data-cloud-storage-csv#data_types">default formats</a> .</p>
<p>Applies to CSV and JSON data.</p></td>
</tr>
<tr class="odd">
<td><code>timestamp_format</code></td>
<td><p><code>STRING</code></p>
<p><a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/format-elements#format_string_as_datetime">Format elements</a> that define how the TIMESTAMP values are formatted in the input files (for example, <code>MM/DD/YYYY HH24:MI:SS.FF3</code> ).</p>
<p>If this value is present, this format is the only compatible TIMESTAMP format. <a href="https://docs.cloud.google.com/bigquery/docs/schema-detect#date_and_time_values">Schema autodetection</a> will also decide TIMESTAMP column type based on this format instead of the existing format.</p>
<p>If this value is not present, the TIMESTAMP field is parsed with the <a href="https://docs.cloud.google.com/bigquery/docs/loading-data-cloud-storage-csv#data_types">default formats</a> .</p>
<p>Applies to CSV and JSON data.</p></td>
</tr>
<tr class="even">
<td><code>uris</code></td>
<td><p>For external tables, including object tables, that aren't Bigtable tables:</p>
<p><code>ARRAY&lt;STRING&gt;</code></p>
<p>An array of fully qualified URIs for the external data locations. Each URI can contain one asterisk ( <code>*</code> ) <a href="https://docs.cloud.google.com/bigquery/docs/loading-data-cloud-storage#load-wildcards">wildcard character</a> , which must come after the bucket name. When you specify <code>uris</code> values that target multiple files, all of those files must share a compatible schema.</p>
<p>The following examples show valid <code>uris</code> values:</p>
<ul>
<li><code>['gs://bucket/path1/myfile.csv']</code></li>
<li><code>['gs://bucket/path1/*.csv']</code></li>
<li><code>['gs://bucket/path1/*', 'gs://bucket/path2/file00*']</code></li>
</ul>
<br />

<p>For Bigtable tables:</p>
<p><code>STRING</code></p>
<p>The URI identifying the Bigtable table to use as a data source. You can only specify one Bigtable URI.</p>
<p>Example: <code>https://googleapis.com/bigtable/projects/ </code><var translate="no"> project_id </var><code> /instances/ </code><var translate="no"> instance_id </var><code> [/appProfiles/ </code><var translate="no"> app_profile </var><code> ]/tables/ </code><var translate="no"> table_name</var></p>
<p>For more information on constructing a Bigtable URI, see <a href="https://docs.cloud.google.com/bigquery/docs/create-bigtable-external-table#bigtable-uri">Retrieve the Bigtable URI</a> .</p></td>
</tr>
</tbody>
</table>

For stability, we recommend that you explicitly list columns in your information schema queries instead of using a wildcard ( `SELECT *` ). Explicitly listing columns prevents queries from breaking if the underlying schema changes.

## Scope and syntax

Queries against this view must include a dataset or a region qualifier. For queries with a dataset qualifier, you must have permissions for the dataset. For queries with a region qualifier, you must have permissions for the project. For more information see [Syntax](https://docs.cloud.google.com/bigquery/docs/information-schema-intro#syntax) . The following table explains the region and resource scopes for this view:

| View name                                                                               | Resource scope | Region scope     |
|-----------------------------------------------------------------------------------------|----------------|------------------|
| `[ `` PROJECT_ID ```  .]`region-  ``` REGION ```  `.INFORMATION_SCHEMA.TABLE_OPTIONS `` | Project level  | `REGION`         |
| `[ `` PROJECT_ID `` .] `` DATASET_ID `` .INFORMATION_SCHEMA.TABLE_OPTIONS`              | Dataset level  | Dataset location |

Replace the following:

- Optional: `PROJECT_ID` : the ID of your Google Cloud project. If not specified, the default project is used.

- `REGION` : any [dataset region name](https://docs.cloud.google.com/bigquery/docs/locations) . For example, `` `region-us` `` .

- `DATASET_ID` : the ID of your dataset. For more information, see [Dataset qualifier](https://docs.cloud.google.com/bigquery/docs/information-schema-intro#dataset_qualifier) .

  > **Note:** You must use [a region qualifier](https://docs.cloud.google.com/bigquery/docs/information-schema-intro#region_qualifier) to query `INFORMATION_SCHEMA` views. The location of the query execution must match the region of the `INFORMATION_SCHEMA` view.

## Example

##### Example 1:

The following example retrieves the default table expiration times for all tables in `mydataset` in your default project ( `myproject` ) by querying the `INFORMATION_SCHEMA.TABLE_OPTIONS` view.

To run the query against a project other than your default project, add the project ID to the dataset in the following format: `` `  ``` project_id ```  `.  ``` dataset `` .INFORMATION_SCHEMA. `` view` ; for example, `` `myproject`.mydataset.INFORMATION_SCHEMA.TABLE_OPTIONS `` .

> **Note:** `INFORMATION_SCHEMA` view names are case-sensitive.

```
SELECT
    *
  FROM
    mydataset.INFORMATION_SCHEMA.TABLE_OPTIONS
  WHERE
    option_name = 'expiration_timestamp';
```

The result is similar to the following:

```
  +----------------+---------------+------------+----------------------+-------------+--------------------------------------+
  | table_catalog  | table_schema  | table_name |     option_name      | option_type |             option_value             |
  +----------------+---------------+------------+----------------------+-------------+--------------------------------------+
  | myproject      | mydataset     | mytable1   | expiration_timestamp | TIMESTAMP   | TIMESTAMP "2020-01-16T21:12:28.000Z" |
  | myproject      | mydataset     | mytable2   | expiration_timestamp | TIMESTAMP   | TIMESTAMP "2021-01-01T21:12:28.000Z" |
  +----------------+---------------+------------+----------------------+-------------+--------------------------------------+
  
```

> **Note:** Tables without an expiration time are excluded from the query results.

##### Example 2:

The following example retrieves metadata about all tables in `mydataset` that contain test data. The query uses the values in the `description` option to find tables that contain "test" anywhere in the description. `mydataset` is in your default project — `myproject` .

To run the query against a project other than your default project, add the project ID to the dataset in the following format: `` `  ``` project_id ```  `.  ``` dataset `` .INFORMATION_SCHEMA. `` view` ; for example, `` `myproject`.mydataset.INFORMATION_SCHEMA.TABLE_OPTIONS `` .

```
SELECT
    *
  FROM
    mydataset.INFORMATION_SCHEMA.TABLE_OPTIONS
  WHERE
    option_name = 'description'
    AND option_value LIKE '%test%';
```

The result is similar to the following:

```
  +----------------+---------------+------------+-------------+-------------+--------------+
  | table_catalog  | table_schema  | table_name | option_name | option_type | option_value |
  +----------------+---------------+------------+-------------+-------------+--------------+
  | myproject      | mydataset     | mytable1   | description | STRING      | "test data"  |
  | myproject      | mydataset     | mytable2   | description | STRING      | "test data"  |
  +----------------+---------------+------------+-------------+-------------+--------------+
  
```
