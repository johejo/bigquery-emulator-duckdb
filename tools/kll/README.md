# BigQuery KLL observations

These are maintainer-recorded BigQuery answers from completed investigation rounds
(113 SQL observations), not emulator outputs and not executable golden serialization tests:

- [sketch_format.answers.txt](sketch_format.answers.txt): SQL, hex wire specimens and errors.
- [quantiles.answers.txt](quantiles.answers.txt): SQL and observed behavior, including analysis
  errors where a query did not reach the intended operation.
- [followup.answers.txt](followup.answers.txt): corrected constant-phi queries, fixed-sketch
  extraction/merge answers and explicit version/unknown-field acceptance.
- [sampler.answers.txt](sampler.answers.txt): retained-mass quantile boundaries and isolated
  sampler weight/exponent-like field mutations.
- [updates.answers.txt](updates.answers.txt): capacity grid, first/odd compaction,
  weighted singletons and fixed-state sampler merges (15 queries, 1788 hex cells).
- [transitions.answers.txt](transitions.answers.txt): second compaction boundaries,
  cutoff depths and a complete equal-cutoff pending-weight grid (4 queries, 326 hex cells).
- [sampler_updates.answers.txt](sampler_updates.answers.txt): higher/unequal-cutoff
  pending-weight grids and lower-level migration (2 queries, 1536 hex cells).
- [merge_order.answers.txt](merge_order.answers.txt): reverse-pair migration and
  empty/absent-sampler controls (3 queries, 584 hex cells).
- [weighted_stream.answers.txt](weighted_stream.answers.txt): weighted INIT before
  and after height growth, upper-bit placement and pending remainders (2 queries, 368 hex cells).
- [weighted_pending.answers.txt](weighted_pending.answers.txt): labelled weighted
  insertion with an existing pending reservoir and six permutations (1 query, 384 hex cells).

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
remain unresolved. The [update investigation](updates.md) records additional constraints
from these bytes and results of the completed 15-query update round. The subsequent
[transition findings](transitions.md) support individually ceil-rounded geometric
capacities and a global compaction budget, a height-dependent sampler cutoff, and
separate promotion/residue values on pending-weight overflow. The
[sampler follow-up](sampler_updates.md) extends this to unit 16 and shows that
lower-level migration can compact below the global budget. The completed
[reverse-order controls](merge_order.md) distinguish receiver migration from
normalizing all inputs, and show absent-sampler normalization. The
[weighted-stream findings](weighted_stream.md) distinguish direct upper-bit
placement from pending remainders. The completed
[nonempty-reservoir follow-up](weighted_pending.md) supports inserting upper bits
without consuming existing pending state; only the low remainder combines with it.
All prepared investigation queries are now answered and archived. The latest
checkpoint lists the remaining model/compatibility gaps. In particular, odd compaction
can retain either parity without a leftover, weighted singleton remainders can round
up or down, and MERGE can retain items below the sampler cutoff.

Decode archived response cells offline, preserving source line and cell context:

```sh
go run ./tools/kll tools/kll/*.answers.txt
```

A count-only hypothesis explorer is also available with
`go run ./tools/kll -model-k 5 -model-steps 30`; its exact assumptions and limits
are documented in [transitions.md](transitions.md). Neither diagnostic tool is a
production codec or a source of expected query results.
When implementing KLL, promote verified behavior to client tests; retain each fixed sketch's
source input, precision and weight context. No KLL support is registered by this investigation.
