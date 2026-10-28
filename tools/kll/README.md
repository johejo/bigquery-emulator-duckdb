# KLL compatibility evidence

KLL is not implemented. This directory keeps the evidence needed for a native
BigQuery sketch reader and for testing a future writer. It is not a specification
of BigQuery's internal compaction algorithm.

## Independent observations

These files contain maintainer-recorded BigQuery responses with their original SQL:

- [sketch_format.answers.txt](sketch_format.answers.txt): typed wire specimens,
  extrema, exact/compacted/weighted sketches and malformed-input errors.
- [quantiles.answers.txt](quantiles.answers.txt): SQL behavior, NULLs, type and
  argument validation. Some queries failed analysis before the intended operation.
- [followup.answers.txt](followup.answers.txt): corrected constant-phi queries,
  EXTRACT results for fixed sketches, exact INT64 boundaries, and version/unknown-field
  acceptance.
- [sampler.answers.txt](sampler.answers.txt): extraction boundaries that distinguish
  retained mass from input count, and isolated sampler field mutations.

Do not regenerate these archives or use their INIT/MERGE bytes as golden writer
outputs: KLL is non-deterministic. Fixed bytes **with independently recorded
EXTRACT answers** can become client-visible reader tests. Decoding a sketch locally
is not an independent expected query answer.

## Observed encoding

The specimens use a protobuf envelope, not Apache DataSketches' binary preamble:

| Outer field | Wire type | Observation |
| --- | --- | --- |
| 1 | varint | Algorithm identifier 113 |
| 2 | varint | Input count, including weights |
| 3 | varint | Version; absent in original specimens, explicit 1/2 accepted in the singleton follow-up |
| 4 | varint | Value type: 4 for INT64, 10 for FLOAT64 |
| 113 | length-delimited | KLL payload |

| Payload field | Wire type | Observation |
| --- | --- | --- |
| 1 | varint | Precision-dependent capacity-like parameter; not the requested precision |
| 2 | varint | Requested precision |
| 3, 4 | length-delimited | Minimum and maximum value bytes |
| 5 | repeated length-delimited | Level messages, including empty levels |
| 6 | length-delimited | Sampler state: value in field 1, weight in field 2, update state in field 3 |

INT64 extrema and sampled values use two's-complement varints, not ZigZag. Level
field 2 contains an initial value followed by unsigned deltas in sorted order.
Accumulate modulo 2^64 and interpret as signed INT64; this preserves the recorded
`[INT64_MIN, 0, INT64_MAX]` level without floating-point conversion.

FLOAT64 extrema, sampled values and level field 1 contain little-endian IEEE754
doubles. NaN survives serialization; the recorded positive/negative-zero input
queries both produced positive-zero bytes. Preserve GoogleSQL's ordering, including
NaN, rather than relying on ordinary floating-point comparisons.

These observations are not a complete accepted-input grammar. Explicit versions
1/2 and a trailing unknown varint field `1000=1` were accepted for the recorded
singleton; arbitrary versions, unknown wire types and other mutations are not
thereby established as valid. Malformed/truncated bytes and an HLL sketch returned
`Invalid input sketch bytes`. A production reader needs safe parsing and explicit
handling of unsupported encoding forms.

## Fixed-sketch extraction

The fixed-byte EXTRACT answers support this reader model:

1. Each level-i item has weight `2^i`.
2. Add the sampled value with its stored weight when that weight is nonzero.
   The sampler's field 3 does not change the tested extraction when mutated from
   3 to 4; this does not make it irrelevant to future updates.
3. Sort the values and select the first whose cumulative weight reaches
   `phi * total_retained_weight`. For array quantile i/k, the rank is
   `ceil(i * total_retained_weight / k)`.
4. At phi 0/1 return the stored exact minimum/maximum.

Normalize by **retained weight**, not input count. The n=16/p=1 specimen has level
mass 8 and sampled weight 7; its phi boundary is 8/15, not 8/16. The n=4/p=1
specimen has total retained mass 5 and crosses at 2/5 and 4/5. Do not require
retained mass to equal input count: the p=1000/n=16384 specimen has level mass
16383 and no sampler. Nor should a reader impose a hard per-level bound from
payload field 1: that specimen retains 3339 items in one level with field 1=2954.

The model matches all 13 fixed-byte extraction responses in the follow-up and all
five sampler responses. It is evidence for those forms, not proof of every
floating-point rank boundary or accepted encoding. On exact `[10,20,30,40]`, phi
`[0,.249,.25,.251,.5,.75,1]` returned `[10,10,10,20,20,30,40]` for both types:
nearest-rank selection, not interpolation. Exact INT64 extraction preserves all bits.

## SQL behavior to retain in client tests

The archives also establish:

- Empty/NULL-only INIT returns NULL. A NULL weight skips the row; a NULL value with
  weight zero was skipped. Invalid precision zero still failed on empty input.
- Empty MERGE_INT64/FLOAT64 returns `[]`; empty MERGE_POINT returns NULL.
- INT64/FLOAT64 sketch mismatches fail instead of converting.
- EXTRACT_POINT's phi must be constant. Precision must be a literal or query
  parameter and cannot be NULL; EXTRACT quantile arguments cannot be NULL either.
- SAFE was rejected for INIT and EXTRACT_INT64. SAFE.EXTRACT_POINT with negative
  literal phi failed argument validation, not a demonstrated runtime SAFE path.
- NaN is retained and sorts before other values in the recorded extraction;
  NaN-only extraction returns NaN. Stored extrema remain exact after merging the
  tested fixed compacted sketches.

Use the saved [function reference](../../third_party/bigquery_docs/reference/standard-sql/kll_functions.md)
for documented precision, weight and argument limits and merge precision rules.
The recorded exact-state mixed-precision merge did not always downgrade, so do not
replace those rules with an unconditional minimum-precision rule.

## Offline diagnostic

```sh
go run ./tools/kll tools/kll/*.answers.txt
```

This emits one JSON object per recorded hex cell, retaining filename, line and cell
path, and reports typed levels, level mass and sampler state. It supports the
observed wire types 0/2 only, represents NaN/infinities as strings, and keeps INT64
values/mass in integer arithmetic. It is not a production codec, a validator for
BigQuery's full input language, or a source of expected percentiles.

## Writer boundary

A future INIT/MERGE implementation needs native BYTES interoperability, documented
precision/error guarantees, typed values, NULL/error behavior and merge semantics.
Check both directions: read BigQuery-produced bytes, and have BigQuery EXTRACT and
MERGE emulator-produced bytes for both types and compacted states.

Matching BigQuery's random sequence, individual generated bytes, internal capacity
formula or compaction thresholds is **not** an implementation prerequisite. Use a
justified KLL algorithm and precision mapping, not a fitted internal formula.
Unsupported forms should remain rejected until their semantics are implemented;
a private-format substitute is not native KLL compatibility.

Detailed internal-algorithm experiments were removed from the working tree; their
observations remain in Git history. They are not additional implementation gates.
