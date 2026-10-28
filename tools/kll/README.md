# BigQuery KLL observations

These are maintainer-recorded BigQuery answers to three completed investigation rounds
(86 SQL observations), not emulator outputs and not executable golden serialization tests:

- [sketch_format.answers.txt](sketch_format.answers.txt): SQL, hex wire specimens and errors.
- [quantiles.answers.txt](quantiles.answers.txt): SQL and observed behavior, including analysis
  errors where a query did not reach the intended operation.
- [followup.answers.txt](followup.answers.txt): corrected constant-phi queries, fixed-sketch
  extraction/merge answers and explicit version/unknown-field acceptance.
- [sampler.answers.txt](sampler.answers.txt): retained-mass quantile boundaries and isolated
  sampler weight/exponent-like field mutations.

The `?>` syntax preserves the original bqanswers responses and their query context. These
files are archived outside `unverified/` because the collection rounds are complete. Do not
run `bigquery-answers` here to regenerate sketches: KLL is non-deterministic. In particular,
none of these hex observations is an expected INIT or MERGE_PARTIAL serialization.

Interpretation and remaining unknowns are in the
[KLL investigation](../../docs/architecture.md#kll-sketch-format-investigation).
The completed follow-up provides independent reader expectations for exact and compacted
states. A candidate decoder model, including a sampled value/weight from payload field 6,
fits all 13 fixed-byte EXTRACT observations from the follow-up and all five sampler
responses. The sampler boundary grids support normalization by retained mass rather than
input count. Setting sampler weight to zero excluded its value from selection; changing
only its exponent-like field 3 from 3 to 4 left the tested extraction unchanged.
This is evidence for a reader, not a complete specification of accepted encodings.
The compaction/update algorithm, precision-to-capacity formula and general merge rules
remain unresolved. There are no pending KLL queries in `unverified/`.
When implementing KLL, promote verified behavior to client tests; retain each fixed sketch's
source input, precision and weight context. No KLL support is registered by this investigation.
