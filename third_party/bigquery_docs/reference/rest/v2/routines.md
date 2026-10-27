- [Resource: Routine](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#Routine)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#Routine.SCHEMA_REPRESENTATION)
- [RoutineReference](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#RoutineReference)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#RoutineReference.SCHEMA_REPRESENTATION)
- [RoutineType](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#RoutineType)
- [Language](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#Language)
- [Argument](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#Argument)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#Argument.SCHEMA_REPRESENTATION)
- [ArgumentKind](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#ArgumentKind)
- [Mode](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#Mode)
- [StandardSqlTableType](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#StandardSqlTableType)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#StandardSqlTableType.SCHEMA_REPRESENTATION)
- [DeterminismLevel](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#DeterminismLevel)
- [RemoteFunctionOptions](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#RemoteFunctionOptions)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#RemoteFunctionOptions.SCHEMA_REPRESENTATION)
- [SparkOptions](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#SparkOptions)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#SparkOptions.SCHEMA_REPRESENTATION)
- [DataGovernanceType](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#DataGovernanceType)
- [PythonOptions](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#PythonOptions)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#PythonOptions.SCHEMA_REPRESENTATION)
- [ExternalRuntimeOptions](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#ExternalRuntimeOptions)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#ExternalRuntimeOptions.SCHEMA_REPRESENTATION)
- [RoutineBuildStatus](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#RoutineBuildStatus)
  - [JSON representation](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#RoutineBuildStatus.SCHEMA_REPRESENTATION)
- [BuildState](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#BuildState)
- [Methods](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#METHODS_SUMMARY)

## Resource: Routine

A user-defined function or a stored procedure.

**JSON representation**

```
{
  "etag": string,
  "routineReference": {
    object (RoutineReference)
  },
  "routineType": enum (RoutineType),
  "creationTime": string,
  "lastModifiedTime": string,
  "language": enum (Language),
  "arguments": [
    {
      object (Argument)
    }
  ],
  "returnType": {
    object (StandardSqlDataType)
  },
  "returnTableType": {
    object (StandardSqlTableType)
  },
  "importedLibraries": [
    string
  ],
  "definitionBody": string,
  "description": string,
  "determinismLevel": enum (DeterminismLevel),
  "strictMode": boolean,
  "remoteFunctionOptions": {
    object (RemoteFunctionOptions)
  },
  "sparkOptions": {
    object (SparkOptions)
  },
  "dataGovernanceType": enum (DataGovernanceType),
  "pythonOptions": {
    object (PythonOptions)
  },
  "externalRuntimeOptions": {
    object (ExternalRuntimeOptions)
  },
  "buildStatus": {
    object (RoutineBuildStatus)
  }
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
<td><code>etag</code></td>
<td><p><code>string</code></p>
<p>Output only. A hash of this resource.</p></td>
</tr>
<tr class="even">
<td><code>routineReference</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#RoutineReference"><code>RoutineReference</code></a><code> )</code></p>
<p>Required. Reference describing the ID of this routine.</p></td>
</tr>
<tr class="odd">
<td><code>routineType</code></td>
<td><p><code>enum ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#RoutineType"><code>RoutineType</code></a><code> )</code></p>
<p>Required. The type of routine.</p></td>
</tr>
<tr class="even">
<td><code>creationTime</code></td>
<td><p><code>string ( </code><a href="https://developers.google.com/discovery/v1/type-format"><code>int64</code></a><code> format)</code></p>
<p>Output only. The time when this routine was created, in milliseconds since the epoch.</p></td>
</tr>
<tr class="odd">
<td><code>lastModifiedTime</code></td>
<td><p><code>string ( </code><a href="https://developers.google.com/discovery/v1/type-format"><code>int64</code></a><code> format)</code></p>
<p>Output only. The time when this routine was last modified, in milliseconds since the epoch.</p></td>
</tr>
<tr class="even">
<td><code>language</code></td>
<td><p><code>enum ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#Language"><code>Language</code></a><code> )</code></p>
<p>Optional. Defaults to "SQL" if remoteFunctionOptions field is absent, not set otherwise.</p></td>
</tr>
<tr class="odd">
<td><code>arguments[]</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#Argument"><code>Argument</code></a><code> )</code></p>
<p>Optional.</p></td>
</tr>
<tr class="even">
<td><code>returnType</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/StandardSqlDataType"><code>StandardSqlDataType</code></a><code> )</code></p>
<p>Optional if language = "SQL"; required otherwise. Cannot be set if routineType = "TABLE_VALUED_FUNCTION".</p>
<p>If absent, the return type is inferred from definitionBody at query time in each query that references this routine. If present, then the evaluated result will be cast to the specified returned type at query time.</p>
<p>For example, for the functions created with the following statements:</p>
<ul>
<li><p><code>CREATE FUNCTION Add(x FLOAT64, y FLOAT64) RETURNS FLOAT64 AS (x + y);</code></p></li>
<li><p><code>CREATE FUNCTION Increment(x FLOAT64) AS (Add(x, 1));</code></p></li>
<li><p><code>CREATE FUNCTION Decrement(x FLOAT64) RETURNS FLOAT64 AS (Add(x, -1));</code></p></li>
</ul>
<p>The returnType is <code>{typeKind: "FLOAT64"}</code> for <code>Add</code> and <code>Decrement</code> , and is absent for <code>Increment</code> (inferred as FLOAT64 at query time).</p>
<p>Suppose the function <code>Add</code> is replaced by <code>CREATE OR REPLACE FUNCTION Add(x INT64, y INT64) AS (x + y);</code></p>
<p>Then the inferred return type of <code>Increment</code> is automatically changed to INT64 at query time, while the return type of <code>Decrement</code> remains FLOAT64.</p></td>
</tr>
<tr class="odd">
<td><code>returnTableType</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#StandardSqlTableType"><code>StandardSqlTableType</code></a><code> )</code></p>
<p>Optional. Can be set only if routineType = "TABLE_VALUED_FUNCTION".</p>
<p>If absent, the return table type is inferred from definitionBody at query time in each query that references this routine. If present, then the columns in the evaluated table result will be cast to match the column types specified in return table type, at query time.</p></td>
</tr>
<tr class="even">
<td><code>importedLibraries[]</code></td>
<td><p><code>string</code></p>
<p>Optional. If language = "JAVASCRIPT", this field stores the path of the imported JAVASCRIPT libraries.</p></td>
</tr>
<tr class="odd">
<td><code>definitionBody</code></td>
<td><p><code>string</code></p>
<p>Required. The body of the routine.</p>
<p>For functions, this is the expression in the AS clause.</p>
<p>If <code>language = "SQL"</code> , it is the substring inside (but excluding) the parentheses. For example, for the function created with the following statement:</p>
<p><code>CREATE FUNCTION JoinLines(x string, y string) as (concat(x, "\n", y))</code></p>
<p>The definitionBody is <code>concat(x, "\n", y)</code> (\n is not replaced with linebreak).</p>
<p>If <code>language="JAVASCRIPT"</code> , it is the evaluated string in the AS clause. For example, for the function created with the following statement:</p>
<p><code>CREATE FUNCTION f() RETURNS STRING LANGUAGE js AS 'return "\n";\n'</code></p>
<p>The definitionBody is</p>
<p><code>return "\n";\n</code></p>
<p>Note that both \n are replaced with linebreaks.</p>
<p>If <code>definitionBody</code> references another routine, then that routine must be fully qualified with its project ID.</p></td>
</tr>
<tr class="even">
<td><code>description</code></td>
<td><p><code>string</code></p>
<p>Optional. The description of the routine, if defined.</p></td>
</tr>
<tr class="odd">
<td><code>determinismLevel</code></td>
<td><p><code>enum ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#DeterminismLevel"><code>DeterminismLevel</code></a><code> )</code></p>
<p>Optional. The determinism level of the JavaScript UDF, if defined.</p></td>
</tr>
<tr class="even">
<td><code>strictMode</code></td>
<td><p><code>boolean</code></p>
<p>Optional. Use this option to catch many common errors. Error checking is not exhaustive, and successfully creating a procedure doesn't guarantee that the procedure will successfully execute at runtime. If <code>strictMode</code> is set to <code>TRUE</code> , the procedure body is further checked for errors such as non-existent tables or columns. The <code>CREATE PROCEDURE</code> statement fails if the body fails any of these checks.</p>
<p>If <code>strictMode</code> is set to <code>FALSE</code> , the procedure body is checked only for syntax. For procedures that invoke themselves recursively, specify <code>strictMode=FALSE</code> to avoid non-existent procedure errors during validation.</p>
<p>Default value is <code>TRUE</code> .</p></td>
</tr>
<tr class="odd">
<td><code>remoteFunctionOptions</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#RemoteFunctionOptions"><code>RemoteFunctionOptions</code></a><code> )</code></p>
<p>Optional. Remote function specific options.</p></td>
</tr>
<tr class="even">
<td><code>sparkOptions</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#SparkOptions"><code>SparkOptions</code></a><code> )</code></p>
<p>Optional. Spark specific options.</p></td>
</tr>
<tr class="odd">
<td><code>dataGovernanceType</code></td>
<td><p><code>enum ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#DataGovernanceType"><code>DataGovernanceType</code></a><code> )</code></p>
<p>Optional. If set to <code>DATA_MASKING</code> , the function is validated and made available as a masking function. For more information, see <a href="https://cloud.google.com/bigquery/docs/user-defined-functions#custom-mask">Create custom masking routines</a> .</p></td>
</tr>
<tr class="even">
<td><code>pythonOptions</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#PythonOptions"><code>PythonOptions</code></a><code> )</code></p>
<p>Optional. Options for the Python UDF. <a href="https://cloud.google.com/products/#product-launch-stages">Preview</a></p></td>
</tr>
<tr class="odd">
<td><code>externalRuntimeOptions</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#ExternalRuntimeOptions"><code>ExternalRuntimeOptions</code></a><code> )</code></p>
<p>Optional. Options for the runtime of the external system executing the routine. This field is only applicable for Python UDFs. <a href="https://cloud.google.com/products/#product-launch-stages">Preview</a></p></td>
</tr>
<tr class="even">
<td><code>buildStatus</code></td>
<td><p><code>object ( </code><a href="https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#RoutineBuildStatus"><code>RoutineBuildStatus</code></a><code> )</code></p>
<p>Output only. The build status of the routine. This field is only applicable to Python UDFs. <a href="https://cloud.google.com/products/#product-launch-stages">Preview</a></p></td>
</tr>
</tbody>
</table>

## RoutineReference

Id path of a routine.

**JSON representation**

```
{
  "projectId": string,
  "datasetId": string,
  "routineId": string
}
```

| Fields      |                                                                                                                                                                  |
|-------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `projectId` | `string` Required. The ID of the project containing this routine.                                                                                                |
| `datasetId` | `string` Required. The ID of the dataset containing this routine.                                                                                                |
| `routineId` | `string` Required. The ID of the routine. The ID must contain only letters (a-z, A-Z), numbers (0-9), or underscores (\_). The maximum length is 256 characters. |

## RoutineType

The fine-grained type of the routine.

| Enums                      |                                          |
|----------------------------|------------------------------------------|
| `ROUTINE_TYPE_UNSPECIFIED` | Default value.                           |
| `SCALAR_FUNCTION`          | Non-built-in persistent scalar function. |
| `PROCEDURE`                | Stored procedure.                        |
| `TABLE_VALUED_FUNCTION`    | Non-built-in persistent TVF.             |

## Language

The language of the routine.

| Enums                  |                      |
|------------------------|----------------------|
| `LANGUAGE_UNSPECIFIED` | Default value.       |
| `SQL`                  | SQL language.        |
| `JAVASCRIPT`           | JavaScript language. |
| `PYTHON`               | Python language.     |
| `JAVA`                 | Java language.       |
| `SCALA`                | Scala language.      |

## Argument

Input/output argument of a function or a stored procedure.

**JSON representation**

```
{
  "name": string,
  "argumentKind": enum (ArgumentKind),
  "mode": enum (Mode),
  "dataType": {
    object (StandardSqlDataType)
  },
  "tableType": {
    object (StandardSqlTableType)
  }
}
```

| Fields         |                                                                                                                                                                                                 |
|----------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `name`         | `string` Optional. The name of this argument. Can be absent for function return argument.                                                                                                       |
| `argumentKind` | `enum ( `[`ArgumentKind`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#ArgumentKind)` )` Optional. Defaults to FIXED_TYPE.                                            |
| `mode`         | `enum ( `[`Mode`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#Mode)` )` Optional. Specifies whether the argument is input or output. Can be set for procedures only. |
| `dataType`     | `object ( `[`StandardSqlDataType`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/StandardSqlDataType)` )` Set if argumentKind == FIXED_TYPE.                                    |
| `tableType`    | `object ( `[`StandardSqlTableType`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#StandardSqlTableType)` )` Optional. Set if argumentKind == FIXED_TABLE.              |

## ArgumentKind

Represents the kind of a given argument.

| Enums                       |                                                                                                           |
|-----------------------------|-----------------------------------------------------------------------------------------------------------|
| `ARGUMENT_KIND_UNSPECIFIED` | Default value.                                                                                            |
| `FIXED_TYPE`                | The argument is a variable with fully specified type, which can be a struct or an array, but not a table. |
| `ANY_TYPE`                  | The argument is any type, including struct or array, but not a table.                                     |
| `FIXED_TABLE`               | The argument is a table with fully specified column names and types.                                      |
| `ANY_TABLE`                 | The argument is any table type.                                                                           |

## Mode

The input/output mode of the argument.

| Enums              |                                              |
|--------------------|----------------------------------------------|
| `MODE_UNSPECIFIED` | Default value.                               |
| `IN`               | The argument is input-only.                  |
| `OUT`              | The argument is output-only.                 |
| `INOUT`            | The argument is both an input and an output. |

## StandardSqlTableType

A table type

**JSON representation**

```
{
  "columns": [
    {
      object (StandardSqlField)
    }
  ]
}
```

| Fields      |                                                                                                                                                    |
|-------------|----------------------------------------------------------------------------------------------------------------------------------------------------|
| `columns[]` | `object ( `[`StandardSqlField`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/StandardSqlField)` )` The columns in this table type |

## DeterminismLevel

JavaScript UDF determinism levels.

If all JavaScript UDFs are DETERMINISTIC, the query result is potentially cacheable (see below). If any JavaScript UDF is NOT_DETERMINISTIC, the query result is not cacheable.

Even if a JavaScript UDF is deterministic, many other factors can prevent usage of cached query results. Example factors include but not limited to: DDL/DML, non-deterministic SQL function calls, update of referenced tables/views/UDFs or imported JavaScript libraries.

SQL UDFs cannot have determinism specified. Their determinism is automatically determined.

| Enums                           |                                                                                                                                        |
|---------------------------------|----------------------------------------------------------------------------------------------------------------------------------------|
| `DETERMINISM_LEVEL_UNSPECIFIED` | The determinism of the UDF is unspecified.                                                                                             |
| `DETERMINISTIC`                 | The UDF is deterministic, meaning that 2 function calls with the same inputs always produce the same result, even across 2 query runs. |
| `NOT_DETERMINISTIC`             | The UDF is not deterministic.                                                                                                          |

## RemoteFunctionOptions

Options for a remote user-defined function.

**JSON representation**

```
{
  "endpoint": string,
  "connection": string,
  "userDefinedContext": {
    string: string,
    ...
  },
  "maxBatchingRows": string
}
```

| Fields               |                                                                                                                                                                                                                                                                                   |
|----------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `endpoint`           | `string` Endpoint of the user-provided remote service, e.g. `https://us-east1-my_gcf_project.cloudfunctions.net/remote_add`                                                                                                                                                       |
| `connection`         | `string` Fully qualified name of the user-provided connection object which holds the authentication information to send requests to the remote service. Format: `"projects/{projectId}/locations/{locationId}/connections/{connectionId}"`                                        |
| `userDefinedContext` | `map (key: string, value: string)` User-defined context as a set of key/value pairs, which will be sent as function invocation context together with batched arguments in the requests to the remote service. The total number of bytes of keys and values must be less than 8KB. |
| `maxBatchingRows`    | `string ( `[`int64`](https://developers.google.com/discovery/v1/type-format)` format)` Max number of rows in each batch sent to the remote service. If absent or if 0, BigQuery dynamically decides the number of rows in a batch.                                                |

## SparkOptions

Options for a user-defined Spark routine.

**JSON representation**

```
{
  "connection": string,
  "runtimeVersion": string,
  "containerImage": string,
  "properties": {
    string: string,
    ...
  },
  "mainFileUri": string,
  "pyFileUris": [
    string
  ],
  "jarUris": [
    string
  ],
  "fileUris": [
    string
  ],
  "archiveUris": [
    string
  ],
  "mainClass": string
}
```

| Fields           |                                                                                                                                                                                                                                                                                                                                                                                      |
|------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `connection`     | `string` Fully qualified name of the user-provided Spark connection object. Format: `"projects/{projectId}/locations/{locationId}/connections/{connectionId}"`                                                                                                                                                                                                                       |
| `runtimeVersion` | `string` Runtime version. If not specified, the default runtime version is used.                                                                                                                                                                                                                                                                                                     |
| `containerImage` | `string` Custom container image for the runtime environment.                                                                                                                                                                                                                                                                                                                         |
| `properties`     | `map (key: string, value: string)` Configuration properties as a set of key/value pairs, which will be passed on to the Spark application. For more information, see [Apache Spark](https://spark.apache.org/docs/latest/index.html) and the [procedure option list](https://cloud.google.com/bigquery/docs/reference/standard-sql/data-definition-language#procedure_option_list) . |
| `mainFileUri`    | `string` The main file/jar URI of the Spark application. Exactly one of the definitionBody field and the mainFileUri field must be set for Python. Exactly one of mainClass and mainFileUri field should be set for Java/Scala language type.                                                                                                                                        |
| `pyFileUris[]`   | `string` Python files to be placed on the PYTHONPATH for PySpark application. Supported file types: `.py` , `.egg` , and `.zip` . For more information about Apache Spark, see [Apache Spark](https://spark.apache.org/docs/latest/index.html) .                                                                                                                                     |
| `jarUris[]`      | `string` JARs to include on the driver and executor CLASSPATH. For more information about Apache Spark, see [Apache Spark](https://spark.apache.org/docs/latest/index.html) .                                                                                                                                                                                                        |
| `fileUris[]`     | `string` Files to be placed in the working directory of each executor. For more information about Apache Spark, see [Apache Spark](https://spark.apache.org/docs/latest/index.html) .                                                                                                                                                                                                |
| `archiveUris[]`  | `string` Archive files to be extracted into the working directory of each executor. For more information about Apache Spark, see [Apache Spark](https://spark.apache.org/docs/latest/index.html) .                                                                                                                                                                                   |
| `mainClass`      | `string` The fully qualified name of a class in jarUris, for example, com.example.wordcount. Exactly one of mainClass and main_jar_uri field should be set for Java/Scala language type.                                                                                                                                                                                             |

## DataGovernanceType

Data governance type values. Only supports `DATA_MASKING` .

| Enums                              |                                           |
|------------------------------------|-------------------------------------------|
| `DATA_GOVERNANCE_TYPE_UNSPECIFIED` | The data governance type is unspecified.  |
| `DATA_MASKING`                     | The data governance type is data masking. |

## PythonOptions

Options for a user-defined Python function.

**JSON representation**

```
{
  "entryPoint": string,
  "packages": [
    string
  ]
}
```

| Fields       |                                                                                                                                                                                                                                                                                                       |
|--------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `entryPoint` | `string` Required. The name of the function defined in Python code as the entry point when the Python UDF is invoked.                                                                                                                                                                                 |
| `packages[]` | `string` Optional. A list of Python package names along with versions to be installed. Example: \["pandas\>=2.1", "google-cloud-translate==3.11"\]. For more information, see [Use third-party packages](https://cloud.google.com/bigquery/docs/user-defined-functions-python#third-party-packages) . |

## ExternalRuntimeOptions

Options for the runtime of the external system.

**JSON representation**

```
{
  "containerMemory": string,
  "containerCpu": number,
  "runtimeConnection": string,
  "maxBatchingRows": string,
  "runtimeVersion": string,
  "containerRequestConcurrency": string
}
```

| Fields                        |                                                                                                                                                                                                                                                                                                                                                                                                                        |
|-------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `containerMemory`             | `string` Optional. Amount of memory provisioned for a Python UDF container instance. Format: {number}{unit} where unit is one of "M", "G", "Mi" and "Gi" (e.g. 1G, 512Mi). If not specified, the default value is 512Mi. For more information, see [Configure container limits for Python UDFs](https://cloud.google.com/bigquery/docs/user-defined-functions-python#configure-container-limits)                       |
| `containerCpu`                | `number` Optional. Amount of CPU provisioned for a Python UDF container instance. For more information, see [Configure container limits for Python UDFs](https://cloud.google.com/bigquery/docs/user-defined-functions-python#configure-container-limits)                                                                                                                                                              |
| `runtimeConnection`           | `string` Optional. Fully qualified name of the connection whose service account will be used to execute the code in the container. Format: `"projects/{projectId}/locations/{locationId}/connections/{connectionId}"`                                                                                                                                                                                                  |
| `maxBatchingRows`             | `string ( `[`int64`](https://developers.google.com/discovery/v1/type-format)` format)` Optional. Maximum number of rows in each batch sent to the external runtime. If absent or if 0, BigQuery dynamically decides the number of rows in a batch.                                                                                                                                                                     |
| `runtimeVersion`              | `string` Optional. Language runtime version. Example: `python-3.11` .                                                                                                                                                                                                                                                                                                                                                  |
| `containerRequestConcurrency` | `string ( `[`int64`](https://developers.google.com/discovery/v1/type-format)` format)` Optional. Maximum number of requests that a Python UDF instance can handle concurrently. If absent or if `0` , the default concurrency value is used. For more information, see [Configure container limits for Python UDFs](https://cloud.google.com/bigquery/docs/user-defined-functions-python#configure-container-limits) . |

## RoutineBuildStatus

The status of a routine build.

**JSON representation**

```
{
  "buildState": enum (BuildState),
  "errorResult": {
    object (ErrorProto)
  },
  "buildStateUpdateTime": string,
  "buildDuration": string,
  "imageSizeBytes": string
}
```

| Fields                 |                                                                                                                                                                                                                                                                                                           |
|------------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `buildState`           | `enum ( `[`BuildState`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines#BuildState)` )` Output only. The current build state of the routine.                                                                                                                                       |
| `errorResult`          | `object ( `[`ErrorProto`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/ErrorProto)` )` Output only. A result object that will be present only if the build has failed.                                                                                                                   |
| `buildStateUpdateTime` | `string ( `[`Timestamp`](https://protobuf.dev/reference/protobuf/google.protobuf/#timestamp)` format)` Output only. The time when the build state was updated last.                                                                                                                                       |
| `buildDuration`        | `string ( `[`Duration`](https://protobuf.dev/reference/protobuf/google.protobuf/#duration)` format)` Output only. The time taken for the image build. Populated only after the build succeeds or fails. A duration in seconds with up to nine fractional digits, ending with ' `s` '. Example: `"3.5s"` . |
| `imageSizeBytes`       | `string ( `[`int64`](https://developers.google.com/discovery/v1/type-format)` format)` Output only. The size of the image in bytes. Populated only after the build succeeds.                                                                                                                              |

## BuildState

The build state of a routine.

| Enums                     |                           |
|---------------------------|---------------------------|
| `BUILD_STATE_UNSPECIFIED` | Default value.            |
| `IN_PROGRESS`             | The build is in progress. |
| `SUCCEEDED`               | The build has succeeded.  |
| `FAILED`                  | The build has failed.     |

| Methods                                                                                                           |                                                                  |
|-------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------|
| [`delete`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines/delete)                         | Deletes the routine specified by routineId from the dataset.     |
| [`get`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines/get)                               | Gets the specified routine resource by routine ID.               |
| [`getIamPolicy`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines/getIamPolicy)             | Gets the access control policy for a resource.                   |
| [`insert`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines/insert)                         | Creates a new routine in the dataset.                            |
| [`list`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines/list)                             | Lists all routines in the specified dataset.                     |
| [`setIamPolicy`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines/setIamPolicy)             | Sets the access control policy on the specified resource.        |
| [`testIamPermissions`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines/testIamPermissions) | Returns permissions that a caller has on the specified resource. |
| [`update`](https://docs.cloud.google.com/bigquery/docs/reference/rest/v2/routines/update)                         | Updates information in an existing routine.                      |
