# External table limits

This page records emulator-specific limits for external tables. See the
[SQL](sql.md), [DDL option](options.md) and [REST](api.md) tables for supported forms,
and the [README](../README.md#google-cloud-storage) for endpoint configuration.
Other query and API limits are in [compatibility.md](compatibility.md).

## Sources and schema

GCS tables support Parquet, CSV and newline-delimited JSON through `CREATE EXTERNAL TABLE`
or `tables.insert`. Sources must be `gs://` URIs; multiple URIs and one wildcard per URI are
supported. Without a schema, creation reads the sources and takes the schema BigQuery detects
from the alphabetically last file. Hive partition keys are also fixed at creation.

### Parquet

The base conversions follow [BigQuery's Parquet conversions](../third_party/bigquery_docs/loading-data-cloud-storage-parquet.md),
with these supported forms:

| Parquet input | Detected schema / read behavior |
|---|---|
| Types mapped to INT64, BOOL, FLOAT64, STRING, BYTES, DATE, TIME or TIMESTAMP | Corresponding BigQuery type |
| Nanosecond TIMESTAMP | INT64 value |
| DECIMAL | NUMERIC, subject to the limits below |
| Group / repeated field | RECORD / REPEATED |
| LIST or MAP group, by default | RECORD of its repeated field |
| LIST with `enable_list_inference` / `parquetOptions.enableListInference` | Unwrapped REPEATED field; NULL elements preserved, unlike loads |

An explicit schema gives nested fields their detected type. Top-level columns additionally
support these conversions; combinations not covered either fail as BigQuery's do or are
unsupported.

| Input | Explicit target | Read behavior |
|---|---|---|
| Integer or DECIMAL | NUMERIC or BIGNUMERIC | Numeric value |
| DECIMAL | STRING | String value |
| DECIMAL | INT64 | Unscaled value, as in BigQuery |
| BYTE_ARRAY | STRING or BYTES | Corresponding value |
| Millisecond or microsecond TIMESTAMP | DATETIME | DATETIME value |
| DATE | TIMESTAMP | TIMESTAMP value |
| Nanosecond TIMESTAMP | TIMESTAMP | Value interpreted as microseconds, as in BigQuery |
| BYTE_ARRAY | TIMESTAMP | Timestamp 0, as in BigQuery |
| BOOLEAN or signed integer | TIMESTAMP | Value interpreted as microseconds, as in BigQuery |
| DOUBLE | TIMESTAMP | Bits interpreted as microseconds, as in BigQuery |
| STRING of WKT or BYTE_ARRAY of WKB | GEOGRAPHY | Parsed geography |

A DECIMAL exceeding NUMERIC's range fails on an out-of-range value, as BigQuery does for a
byte-encoded DECIMAL. These DECIMAL forms remain unsupported:

- INT32- or INT64-encoded values exceeding NUMERIC's range;
- values with both too many integer and fractional digits, or more than 38 integer digits;
- nested DECIMAL conversions.

GeoParquet, column-name conversions and options selecting other types are unsupported.
Every file must contain all REQUIRED and REPEATED columns and fields of the schema.

### CSV

CSV takes a flat schema of STRING, INT64, BOOL, FLOAT64, BYTES, NUMERIC, DATE, TIME and TIMESTAMP,
given or [detected as for loads](compatibility.md#loads-extracts-and-copies). A given schema
can also contain DATETIME, BIGNUMERIC, JSON, GEOGRAPHY, INTERVAL and RANGE columns.

Supported CSV options are:

- `skip_leading_rows` / `csvOptions.skipLeadingRows`;
- `field_delimiter` / `csvOptions.fieldDelimiter`: one ASCII character other than quote,
  line break or NUL;
- the [CSV options accepted by loads](compatibility.md#loads-extracts-and-copies).

Schema detection does not take a quote, quoted line breaks or NULL markers. Other CSV options
are unsupported. Encoding, compression and format options have the additional limits below.

### Newline-delimited JSON

Schemas can contain STRING, INT64, BOOL, FLOAT64, BYTES, NUMERIC, BIGNUMERIC, DATE, DATETIME,
TIME, TIMESTAMP, JSON and GEOGRAPHY fields, RECORDs and repeated fields of these types.
Other scalar types are unsupported.

Given a schema, scalar values require native JSON representations, with these forms:

| Type | Accepted representation / limit |
|---|---|
| INT64 | Also decimal strings |
| BOOL | Also text spellings |
| BYTES | Base64 strings |
| NUMERIC | Decimal numbers or strings, at most 29 integer and 9 fractional digits, without exponent notation |
| DATE, TIME | Canonical `YYYY-MM-DD` and `HH:MM:SS[.SSSSSS]`, unless a format applies |
| TIMESTAMP | [CSV timestamp forms](../third_party/bigquery_docs/loading-data-cloud-storage-csv.md), except Unix epoch time |

Without a schema, [detection](../third_party/bigquery_docs/schema-detect.md) uses the
alphabetically last file. Reads then also accept strings in forms detection converts, and
numbers and BOOL values for STRING fields.

With both `autodetect` and a schema, `tables.insert` keeps the given schema. Reads omit missing
top-level fields, but reject missing nested fields and values requiring detected-schema
conversions as unsupported. Other JSON-specific options besides `jsonOptions.encoding` are
unsupported.

## Encoding, formats and bad records

CSV and JSON support these shared options:

| DDL | REST |
|---|---|
| `compression` | `compression` |
| `encoding` (CSV) | `csvOptions.encoding` |
| — | `jsonOptions.encoding` |
| `max_bad_records` | `maxBadRecords` |
| `time_zone` | `timeZone` |
| `date_format` | `dateFormat` |
| `datetime_format` | `datetimeFormat` |
| `time_format` | `timeFormat` |
| `timestamp_format` | `timestampFormat` |

CSV detection takes all these options; JSON detection takes only the date format. A parsed
value takes the format's type, and a default-form DATE becomes STRING given a date format,
except for the unsupported JSON case below.

These forms are unsupported:

- A value parsed by several formats.
- A default-form TIME or TIMESTAMP given a format of its type; a default-form TIMESTAMP given
  a DATETIME format.
- A detected DATETIME CSV column without a header.
- A default-form JSON DATE given a date format.
- A gzip source with compression `NONE` (BigQuery reads it without decompressing), or a gzip
  source in UTF-16 or UTF-32.
- Invalid UTF-16 or UTF-32 sequences.
- JSON rows that are numbers, BOOLs or null.

A conversion failure in a valid JSON record still fails the query rather than counting as a
bad record, except for BIGNUMERIC, DATETIME, TIMESTAMP, GEOGRAPHY and values parsed by a format.
Malformed-line messages use the most common BigQuery message unless covered by
[recorded answers](../tests/e2e/scenarios/testdata/external_table_types_open.api.yaml).

## Hive partitioning

[Hive partitioning](../third_party/bigquery_docs/hive-partitioned-queries.md), through
`WITH PARTITION COLUMNS` or `hivePartitioningOptions`, takes key names and types from source
paths at creation. Queries read every source file without partition pruning and fail if a
file's path lacks those keys.

## Bigtable

Bigtable supports BYTES and STRING cell values, `readRowkeyAsString`,
`ignoreUnspecifiedColumnFamilies` and family-level `onlyReadLatest`. These are unsupported:

- Promoting individual qualifiers through `columns`.
- Numeric/boolean conversion, JSON/PROTO values and `outputColumnFamiliesAsJson`.
- Explicit schemas and GCS-specific read options.
- Reading a row missing a selected family.

Schemas are fixed at creation; queries read current source rows. Endpoint setup is in the
[README](../README.md#bigtable-external-tables).

## Updates and other interfaces

`tables.patch` and `tables.update` change description, friendly name, labels, tags and
expiration. They accept the schema and `externalDataConfiguration` returned by `tables.get`
but reject changes to them. `ALTER TABLE SET OPTIONS` fails as
[BigQuery's does](../tests/e2e/scenarios/testdata/external_table_alter.api.yaml); other
`ALTER TABLE` actions are unsupported.

BigLake connections, metadata caching and time travel are unsupported. Dry runs validate
references against the stored schema without source access or external-size estimates.
`tabledata.list`, Storage Read, extracts and copy jobs do not read external tables; use a
query instead. Writes are rejected.
