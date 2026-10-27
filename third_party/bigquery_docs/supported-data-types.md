# Supported protocol buffer and Arrow data types

The BigQuery Storage Write API (gRPC) maps each BigQuery column data type to specific protocol buffer and Apache Arrow input types and encoding formats. The reference tables in this document list the supported type mappings, value ranges, and conversion parameters. For foundational API concepts, see [Overview of the BigQuery Storage Write API (gRPC)](https://docs.cloud.google.com/bigquery/docs/write-api#overview) .

## Supported protocol buffer data types

The following table shows the supported data types in protocol buffers and the corresponding input format in BigQuery:

<table>
<colgroup>
<col style="width: 50%" />
<col style="width: 50%" />
</colgroup>
<thead>
<tr class="header">
<th>BigQuery data type</th>
<th>Supported protocol buffer types</th>
</tr>
</thead>
<tbody>
<tr class="odd">
<td><code>BOOL</code></td>
<td><code>bool</code> , <code>int32</code> , <code>int64</code> , <code>uint32</code> , <code>uint64</code> , <code>google.protobuf.BoolValue</code></td>
</tr>
<tr class="even">
<td><code>BYTES</code></td>
<td><code>bytes</code> , <code>string</code> , <code>google.protobuf.BytesValue</code></td>
</tr>
<tr class="odd">
<td><code>DATE</code></td>
<td><code>int32</code> (preferred), <code>int64</code> , <code>string</code>
<p>The value is the number of days since the Unix epoch (1970-01-01). The valid range is <code>-719162</code> (0001-01-01) to <code>2932896</code> (9999-12-31).</p></td>
</tr>
<tr class="even">
<td><code>DATETIME</code> , <code>TIME</code></td>
<td><code>string</code>
<p>The value must be a <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/lexical#datetime_literals"><code>DATETIME</code></a> or <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/lexical#time_literals"><code>TIME</code></a> literal.</p></td>
</tr>
<tr class="odd">
<td><code>int64</code>
<p>Use the <a href="https://github.com/googleapis/java-bigquerystorage/blob/main/google-cloud-bigquerystorage/src/main/java/com/google/cloud/bigquery/storage/v1/CivilTimeEncoder.java"><code>CivilTimeEncoder</code> class</a> to perform the conversion.</p></td>
<td></td>
</tr>
<tr class="even">
<td><code>FLOAT</code></td>
<td><code>double</code> , <code>float</code> , <code>google.protobuf.DoubleValue</code> , <code>google.protobuf.FloatValue</code></td>
</tr>
<tr class="odd">
<td><code>GEOGRAPHY</code></td>
<td><code>string</code>
<p>The value is a geometry in either WKT or GeoJson format.</p></td>
</tr>
<tr class="even">
<td><code>INTEGER</code></td>
<td><code>int32</code> , <code>int64</code> , <code>uint32</code> , <code>enum</code> , <code>google.protobuf.Int32Value</code> , <code>google.protobuf.Int64Value</code> , <code>google.protobuf.UInt32Value</code></td>
</tr>
<tr class="odd">
<td><code>JSON</code></td>
<td><code>string</code></td>
</tr>
<tr class="even">
<td><code>NUMERIC</code> , <code>BIGNUMERIC</code></td>
<td><code>int32</code> , <code>int64</code> , <code>uint32</code> , <code>uint64</code> , <code>double</code> , <code>float</code> , <code>string</code></td>
</tr>
<tr class="odd">
<td><code>bytes</code> , <code>google.protobuf.BytesValue</code>
<p>Use the <a href="https://github.com/googleapis/java-bigquerystorage/blob/main/google-cloud-bigquerystorage/src/main/java/com/google/cloud/bigquery/storage/v1/BigDecimalByteStringEncoder.java"><code>BigDecimalByteStringEncoder</code> class</a> to perform the conversion.</p></td>
<td></td>
</tr>
<tr class="even">
<td><code>STRING</code></td>
<td><code>string</code> , <code>enum</code> , <code>google.protobuf.StringValue</code></td>
</tr>
<tr class="odd">
<td><code>TIME</code></td>
<td><code>string</code>
<p>The value must be a <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/lexical#time_literals"><code>TIME</code> literal</a> .</p></td>
</tr>
<tr class="even">
<td><code>TIMESTAMP</code></td>
<td><code>int64</code> (preferred), <code>int32</code> , <code>uint32</code> , <code>google.protobuf.Timestamp</code>
<p>The value is given in microseconds since the Unix epoch (1970-01-01).</p></td>
</tr>
<tr class="odd">
<td><code>INTERVAL</code></td>
<td><code>string</code> , <code>google.protobuf.Duration</code>
<p>The string value must be an <a href="https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/lexical#interval_literals"><code>INTERVAL</code> literal</a> .</p></td>
</tr>
<tr class="even">
<td><code>RANGE&lt;T&gt;</code></td>
<td><code>message</code>
<p>A nested message type in the proto with two fields, <code>start</code> and <code>end</code> , where both fields must be of the same supported protocol buffer type that corresponds to a BigQuery data type <code>T</code> . <code>T</code> must be one of <code>DATE</code> , <code>DATETIME</code> , or <code>TIMESTAMP</code> . If a field ( <code>start</code> or <code>end</code> ) is not set in the proto message, it represents an unbounded boundary. In the following example, <code>f_range_date</code> represents a <code>RANGE</code> column in a table. Since the <code>end</code> field is not set in the proto message, the end boundary of this range is unbounded.</p>
<pre data-fenced=""><code>{
  f_range_date: {
    start: 1
  }
}</code></pre></td>
</tr>
<tr class="odd">
<td><code>REPEATED FIELD</code></td>
<td><code>array</code>
<p>An array type in the proto corresponds to a repeated field in BigQuery.</p></td>
</tr>
<tr class="even">
<td><code>RECORD</code></td>
<td><code>message</code>
<p>A nested message type in the proto corresponds to a record field in BigQuery.</p></td>
</tr>
</tbody>
</table>

## Supported Apache Arrow data types

The following table shows the supported data types in Apache Arrow and the corresponding input format in BigQuery.

| BigQuery data type | Supported Apache Arrow types                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | Supported type parameters                                                                                                                                                                                      |
|--------------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `BOOL`             | `Boolean`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |                                                                                                                                                                                                                |
| `BYTES`            | `Binary`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |                                                                                                                                                                                                                |
| `DATE`             | `Date`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          | unit = Day                                                                                                                                                                                                     |
| `String` , `int32` |                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |                                                                                                                                                                                                                |
| `DATETIME`         | `Timestamp`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | unit = MICROSECONDS timezone is empty                                                                                                                                                                          |
| `FLOAT`            | `FloatingPoint`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | Precision in {SINGLE, DOUBLE}                                                                                                                                                                                  |
| `GEOGRAPHY`        | `Utf8` The value is a geometry in either WKT or GeoJson format.                                                                                                                                                                                                                                                                                                                                                                                                                                                 |                                                                                                                                                                                                                |
| `INTEGER`          | `int`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           | bitWidth in {8, 16, 32, 64} is_signed = false                                                                                                                                                                  |
| `JSON`             | `Utf8`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |                                                                                                                                                                                                                |
| `NUMERIC`          | `Decimal128`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | You can provide a NUMERIC that has any precision or scale that's smaller than the [BigQuery supported range](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-types#decimal_types) .    |
| `BIGNUMERIC`       | `Decimal256`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | You can provide a BIGNUMERIC that has any precision or scale that's smaller than the [BigQuery supported range](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/data-types#decimal_types) . |
| `STRING`           | `Utf8`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |                                                                                                                                                                                                                |
| `TIMESTAMP`        | `Timestamp`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | unit= MICROSECONDS timezone = UTC                                                                                                                                                                              |
| `INTERVAL`         | `Interval`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      | unit in {YEAR_MONTH, DAY_TIME, MONTH_DAY_NANO}                                                                                                                                                                 |
| `Utf8`             |                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |                                                                                                                                                                                                                |
| `RANGE<T>`         | `Struct` The Arrow Struct must have two subfields named `start` and `end` . For the `RANGE<DATE>` column, the fields must be Arrow type `Date` with `unit=Day` . For the `RANGE<DATETIME>` column, the fields must be the Arrow type `Timestamp` with `unit=MICROSECONDS` , without the timezone. For the `RANGE<TIMESTAMP>` , the fields must be the Arrow type `Timestamp` with `unit=MICROSECONDS` , `timezone=UTC` . A `NULL` value in any of the `start` and `end` fields will be treated as `UNBOUNDED` . |                                                                                                                                                                                                                |
| `REPEATED FIELD`   | `List`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          | A `NULL` value must be represented by an empty list.                                                                                                                                                           |
| `RECORD`           | `Struct`                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |                                                                                                                                                                                                                |
