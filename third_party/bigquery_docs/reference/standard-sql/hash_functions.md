GoogleSQL for BigQuery supports the following hash functions.

## Function list

| Name                                                                                                                     | Summary                                                                                             |
|--------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------|
| [`FARM_FINGERPRINT`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#farm_fingerprint) | Computes the fingerprint of a `STRING` or `BYTES` value using the FarmHash Fingerprint64 algorithm. |
| [`MD5`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#md5)                           | Computes the hash of a `STRING` or `BYTES` value using the MD5 algorithm.                           |
| [`SHA1`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#sha1)                         | Computes the hash of a `STRING` or `BYTES` value using the SHA-1 algorithm.                         |
| [`SHA256`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#sha256)                     | Computes the hash of a `STRING` or `BYTES` value using the SHA-256 algorithm.                       |
| [`SHA512`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#sha512)                     | Computes the hash of a `STRING` or `BYTES` value using the SHA-512 algorithm.                       |

## `FARM_FINGERPRINT`

```
FARM_FINGERPRINT(value)
```

**Description**

Computes the fingerprint of the `STRING` or `BYTES` input using the `Fingerprint64` function from the [open-source FarmHash library](https://github.com/google/farmhash) . The output is a signed INT64 value, uniformly distributed in the `[-2^63, 2^63 - 1]` range. The output of this function for a particular input never changes across runs or query engine implementations.

Use `FARM_FINGERPRINT` when you need a fast, deterministic 64-bit signed integer ( `INT64` ) for non-cryptographic use cases, such as the following:

- Sharding, partitioning, or bucketing large datasets consistently across distributed pipelines.
- Generating stable integer identifiers or grouping keys from strings.
- Sampling rows deterministically ( `ABS(FARM_FINGERPRINT(id)) % 10 = 0` ).

`FARM_FINGERPRINT` isn't a cryptographic hash function, so don't use it for security verification or adversarial inputs. Because it outputs 64 bits, collision probability increases as the number of distinct elements exceeds four billion ( `~2^32` items).

For cryptographic verification or lower collision probability, use [`SHA256`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#sha256) .

**Return type**

INT64

**Examples**

```
WITH example AS (
  SELECT 1 AS x, "foo" AS y, true AS z UNION ALL
  SELECT 2 AS x, "apple" AS y, false AS z UNION ALL
  SELECT 3 AS x, "" AS y, true AS z
)
SELECT
  *,
  FARM_FINGERPRINT(CONCAT(CAST(x AS STRING), y, CAST(z AS STRING)))
    AS row_fingerprint
FROM example;
/*---+-------+-------+----------------------+
 | x | y     | z     | row_fingerprint      |
 +---+-------+-------+----------------------+
 | 1 | foo   | true  | -1541654101129638711 |
 | 2 | apple | false | 2794438866806483259  |
 | 3 |       | true  | -4880158226897771312 |
 +---+-------+-------+----------------------*/
```

## `MD5`

```
MD5(input)
```

**Description**

Computes the hash of the input using the [MD5 algorithm](https://en.wikipedia.org/wiki/MD5) . The input can be either `STRING` or `BYTES` . The string version treats the input as an array of bytes.

Use `MD5` only for backward compatibility with legacy databases, historic checksums, or external communication protocols and deduplication workflows that strictly mandate an MD5 digest.

> **Warning:** `MD5` uses a vulnerable 128-bit algorithm and isn't secure against cryptographic collision attacks. Don't use `MD5` for new implementations, digital signatures, passwords, or security verification.

For cryptographic security, use [`SHA256`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#sha256) or [`SHA512`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#sha512) .

**Return type**

BYTES

**Examples**

```
SELECT MD5("Hello World") as md5;

-- Note that the result of MD5 is of type BYTES, displayed as a base64-encoded string.
/*--------------------------+
 | md5                      |
 +--------------------------+
 | sQqNsWTgdUEFt6mb5y4/5Q== |
 +--------------------------*/
```

## `SHA1`

```
SHA1(input)
```

**Description**

Computes the hash of the input using the [SHA-1 algorithm](https://en.wikipedia.org/wiki/SHA-1) . The input can be either `STRING` or `BYTES` . The string version treats the input as an array of bytes.

Use `SHA1` only when backward compatibility with legacy datasets, external APIs, or historical cryptographic identifiers (such as git commit hashes or legacy verification records) is strictly required.

> **Warning:** `SHA-1` has known cryptographic vulnerabilities and isn't secure against collision attacks. Don't use `SHA1` for new cryptographic designs, digital signatures, or security verification.

For cryptographic verification, use

[`SHA256`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#sha256) or [`SHA512`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#sha512) .

**Return type**

BYTES

**Examples**

```
SELECT SHA1("Hello World") as sha1;

-- Note that the result of SHA1 is of type BYTES, displayed as a base64-encoded string.
/*------------------------------+
 | sha1                         |
 +------------------------------+
 | Ck1VqNd45QIvq3AZd8XYQLvEhtA= |
 +------------------------------*/
```

## `SHA256`

```
SHA256(input)
```

**Description**

Computes the hash of the input using the [SHA-256 algorithm](https://en.wikipedia.org/wiki/SHA-2) . The input can be either `STRING` or `BYTES` . The string version treats the input as an array of bytes.

Use `SHA256` when you require a cryptographically secure hash function, such as the following use cases:

- Verifying string or binary data integrity and generating tamper-evident checksums.
- Creating secure cryptographic digests or globally unique content identifiers (256-bit hash space provides effectively zero collision probability).
- Interoperating with authentication workflows, digital signatures, and security protocols requiring standard SHA-2 digests.

Computing `SHA256` is computationally more expensive than non-cryptographic fingerprinting (for example, [`FARM_FINGERPRINT`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#farm_fingerprint) ). Avoid using `SHA256` solely for simple data sharding, bucketing, or indexing where high throughput and compact integer output are required.

**Return type**

BYTES

**Examples**

```
SELECT SHA256("Hello World") as sha256;
```

## `SHA512`

```
SHA512(input)
```

**Description**

Computes the hash of the input using the [SHA-512 algorithm](https://en.wikipedia.org/wiki/SHA-2) . The input can be either `STRING` or `BYTES` . The string version treats the input as an array of bytes.

Use `SHA512` when you require maximum cryptographic security (512-bit output), such as the following use cases:

- High-security data verification, digital signatures, and cryptographic protocols where SHA-512 digests are mandated by compliance standards.
- Applications requiring ultra-low collision probability or resistance to future-proofing attacks with hash outputs larger than 256 bits.
- Processing platforms on 64-bit hardware architectures where SHA-512 operates with high efficiency.

`SHA512` produces a 64-byte digest, double the size of [`SHA256`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#sha256) (32 bytes). When storing digests in tables or transmitting over the network where storage and bandwidth efficiency are essential, use [`SHA256`](https://docs.cloud.google.com/bigquery/docs/reference/standard-sql/hash_functions#sha256) unless 512-bit security is specifically required.

**Return type**

BYTES

**Examples**

```
SELECT SHA512("Hello World") as sha512;
```
