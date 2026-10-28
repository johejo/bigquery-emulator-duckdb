# Higher and unequal sampler cutoffs

The two queries in [sampler_updates.answers.txt](sampler_updates.answers.txt) have
maintainer-recorded answers: **1536 hex cells**, with no analysis errors. They
extend the [transition round](transitions.md). These are accepted mutated-state
experiments, not golden INIT/MERGE bytes or a complete writer specification.
Decode all archived states offline with `go run ./tools/kll tools/kll/*.answers.txt`.

## Equal cutoff 4: the unit-8 structure also holds at unit 16

Both inputs have n=32, p=1, k=3, level4=[4], cutoff 4. Their pending values are
16 and 15, with weights a,b in 0..15; each pair has four trials. Only pending
value/weight were changed from the previously recorded source bytes.

All **1024** responses satisfy:

| Input pending sum | Cells | Pending weight/value | Retained levels; resulting cutoff |
| --- | --- | --- | --- |
| a+b<16 | 544 | Weight a+b; absent at zero, otherwise one input's positive-weight value | level4=[4,4]; h=4 |
| a+b=16 | 60 | Zero weight, no pending value | level5=[4] or [4,15] or [4,16]; h=5 |
| a+b>16 | 420 | Weight a+b-16; value **15 in every cell** | level5=[4] or [4,15] or [4,16]; h=5 |

Thus every weight is `(a+b) mod 16`, even when compaction raises the final cutoff
to 5 (unit 32). The residual is not reduced modulo the **new** unit. As at h=3,
positive residual weight is not scaled simply because the cutoff changes.

Promotion and residue can have different labels: for example a=2,b=15,trial=2
has level5=[4,16] but pending value 15/weight 1. A single representative chosen
for all 17 units and reused for both pieces cannot explain this response.

This extends the earlier compatible chunked-reservoir model:

```text
U = 2^h before this update
fill the current reservoir with as much incoming weight as fits
if the reservoir reaches U:
  emit its selected value with weight U
  start a new pending reservoir from the incoming value's unused weight
compact retained levels if needed; preserve the pending weight across this growth
```

This grid crosses at most one unit per pair. It does not settle updates spanning
multiple units, the probability of selecting the incoming value, or how a
larger incoming compactor-level item is handled. The labels describe these
observations, not a guaranteed SQL aggregate input order.

## Mixed cutoff 3/4: retained lower levels migrate even without overflow

The low-cutoff input has n=16, level3=[4], pending value 16/weight a (0..7), h=3.
The high-cutoff input has n=32, level4=[4], pending value 15/weight b (0..15), h=4.
Both retain p=1/k=3. The query uses `UNNEST([low.s, high.s])` with four trials per
pair; SQL does not promise a particular aggregate execution/merge order.

All **512** responses have pending weight `(a+b) mod 16`. In particular, a remains
in the same weight units when the effective cutoff increases; it is not doubled,
rounded to 16 or independently discarded. For the 84 a+b>16 cells, the pending
value is again **15 in every cell**. At a+b=16 it is absent.

Unlike the earlier sampler/exact merges that preserved incoming levels below the
cutoff, **none of these mixed-cutoff outputs retain level 3**. Migration also
happens at a=b=0, so it cannot be explained as pending-reservoir overflow:

| Input pending sum | Cells | Retained levels; cutoff |
| --- | --- | --- |
| a+b<16 | 211 | level4=[4]; h=4 |
| a+b<16 | 189 | level4=[4,4]; h=4 |
| a+b=16 | 17 | level4=[4,15] or [4,16]; h=4 |
| a+b=16 | 11 | level5=[4] or [4,15] or [4,16]; h=5 |
| a+b>16 | 48 | level4=[4,15] or [4,16]; h=4 |
| a+b>16 | 36 | level5=[4] or [4,15] or [4,16]; h=5 |

For a=b=0 specifically, trial 0 retains two copies at level 4, while trials 1..3
retain one. The input had only two retained items in total, less than k=3. Thus
a rule that **only** compacts when the retained-item global budget is reached
cannot explain all MERGE work: level migration can compact earlier.

A compatible sequence is:

1. Raise the low state's cutoff from 3 to 4 and compact its lone level-3 item,
   promoting either zero or one copy of 4 to level 4. Leave its pending weight a
   unchanged.
2. Combine pending weight with the other reservoir at unit 16. If the sum reaches
   16, emit one more level-4 item and retain the unused incoming weight separately.
3. With no emission, the resulting level-4 count is one or two. With emission it
   is two or three; only the three-item case needs ordinary compaction to level 5,
   explaining the differing final cutoffs.

This explains every observed structure but does **not** distinguish two important
algorithms: normalize all inputs to the higher cutoff, or migrate only the current
receiver when adding higher levels. The latter would also explain why the earlier
merges of an already-high state with an exact singleton preserved the singleton's
low level. The no-sampler compaction model deliberately does not implement this
migration; its count-only hypotheses remain scoped to its documented domain.

## Completed discrimination: reverse order and absent-sampler controls

All three queries are now answered in
[merge_order.answers.txt](merge_order.answers.txt) (584 hex cells); findings are in
[merge_order.md](merge_order.md). They asked for:

1. The same mixed-cutoff grid with `UNNEST([high.s, low.s])`, retaining a/b as the
   low/high labels. This tests whether lower-level preservation and residual labels
   differ under this change in query spelling. It is **not** a contractual way to
   force BigQuery's aggregate execution order; unchanged results would not prove
   order independence either.
2. Fixed low states with pending weight 7 or 0: one-input controls, low/high and
   high/low pair spellings. This separates pending-reservoir overflow from level
   migration and records any normalization on a one-input MERGE.
3. The same controls with field 6 entirely absent from the low state. The payload
   length is corrected; all other state fields are unchanged. Acceptance is unknown.
   It is isolated so a rejection cannot hide the other two queries' results.

The fixed high input is the earlier recorded h=4, level4=[4] state with pending
weight changed to zero; the low input is the earlier n=16/h=3 state. These queries
are investigation-only, with no guessed expected results. Repetition does not
establish independent random draws or a replacement probability law.

The reverse query preserves level 3 below the unit boundary and changes the
overflow residual label to 16. Both zero-weight and absent-sampler controls retain
this directional distinction. The absent sampler normalizes to cutoff 3 on a
one-input MERGE. The [weighted-stream follow-ups](weighted_stream.md) are also
complete, as is the [three-input weighted/pending query](weighted_pending.md).
No prepared KLL investigation query remains unanswered. Precision-to-k conversion, general sampler
replacement probabilities and general merge/migration rules remain unresolved.
Do not register production support based on these candidate models alone.
