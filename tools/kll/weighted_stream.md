# Weighted INIT before and after cutoff growth

The two queries in [weighted_stream.answers.txt](weighted_stream.answers.txt) now
have maintainer-recorded answers: **368 hex cells**, no analysis errors. They
follow the [merge direction investigation](merge_order.md). These bytes are
immutable observations, not golden serialization expectations.

Each aggregate has a large input `(value=42, weight=2^20)` and a small input
`(value=100, weight=w)`. Both array spellings, `big_first` and `small_first`, are
sampled eight times per w. Those labels describe the SQL input arrays, not a
contractual guarantee of aggregate execution order.

## Precision 1: pending state depends on when the cutoff grows

All 160 cells have cutoff h=20 and only one retained item, level20=[42]. For
w in `1,2,3,4,7,8,9,15,16,17`:

| Spelling | Cells | Pending state |
| --- | --- | --- |
| big_first | 80 | Value 100, weight **w** |
| small_first | 80 | No pending value or weight |

Thus explicitly weighted INIT **can** retain pending weight. It is not generally
an immediate-rounding operation or an end-of-aggregate flush of every remainder.

The observations fit different handling of an incoming remainder below an
**existing** cutoff versus migration of previously retained lower levels when the
cutoff grows. In the latter case the small mass can be discarded by compaction
parity instead of becoming pending reservoir weight. These small-first samples
cannot establish that the mass is always discarded: a very rare retained/promoted
small value is not ruled out by eight trials.

## Precision 1000: direct upper bits and a pending low remainder

All 208 cells have cutoff h=3 (unit U=8), keep level20=[42], and agree with:

- **big_first**: pending value 100 with weight `w mod 8`, absent if zero; one
  additional 100 at each set-bit level of `w - (w mod 8)`.
- **small_first**: zero pending weight; retained lower levels are compatible with
  compacting old lower bits up to cutoff 3, not putting their mass into a new
  pending reservoir.

Observed small-value contributions (all are value 100) are:

| w | big_first retained levels | Pending weight | small_first retained levels, observed only |
| --- | --- | --- | --- |
| 1,2,3,4,7 | None | w | None or one at level 3 |
| 8 | One at level 3 | 0 | One at level 3 |
| 9 | One at level 3 | 1 | One at level 3 |
| 15 | One at level 3 | 7 | One or two at level 3 |
| 16 | One at level 4 | 0 | One at level 4 |
| 17 | One at level 4 | 1 | One at level 4 |
| 23 | One at level 4 | 7 | One at level 3 and one at level 4 |
| 24 | One at level 3 and one at level 4 | 0 | Same |
| 25 | One at level 3 and one at level 4 | 1 | One or two at level 3, one at level 4 |

Entries in the final column are finite observed outcomes, not exhaustive allowed
sets or deterministic laws. In particular, the eight w=9/17 samples do not rule
out stochastic promotion of their low unit remainder on another run.

The w=16 **big_first** result is particularly discriminating: there is one item
at level 4, **not two at level 3**. With the lone level-20 item, both alternatives
are far below the candidate global compaction budget of 8865 for 18 active levels
at k=2954. Purely filling/emitting an 8-unit reservoir twice would leave two
level-3 items unless an additional unexplained compaction step were added.
Direct insertion of the upper set bit explains the observed single level-4 item.
Consequently, the earlier chunked-reservoir model for merging two pending weights
must **not** be generalized to streaming the entire weight of every INIT input.

## Candidate weighted-update path

A compatible, still unproven sequence is:

```text
use the receiver's current cutoff h, before this input changes its height
split input weight into:
  upper set bits at levels >= h: insert directly at those levels
  low remainder below 2^h: feed into the pending weighted reservoir
if the input grows the level list:
  increase the cutoff as needed
  migrate/compact old lower levels, rather than putting them into the reservoir
apply ordinary global-budget compaction where required
```

With an empty receiver h=0, all bits are initially level contributions and no
low remainder exists. Later growth can therefore compact lower bits and leave
zero pending weight, explaining the earlier weighted singleton observations.
With an already-high receiver, the same small input can instead leave a pending
remainder. This reconciles the observations without inventing a universal final
sampler flush or a special singleton-only rounding law.

The two-input queries do **not** test how a later large weight interacts with an
already **nonempty** reservoir. For example, after establishing U=8 and pending
value 100/weight 1, a weight-16 input of value 200 could insert its upper bit while
leaving that reservoir untouched, or could consume/replace the existing pending
state through another weighted path. The current observations alone do not settle
that distinction.

## Completed targeted follow-up

[weighted_pending.answers.txt](weighted_pending.answers.txt) contains the answer
to one standalone query: large height-establishing input A, labelled
weight-1 input B, and labelled weighted input C, at p=1000. It tests weights
8,9,15,16,17,23,24,25 with all six array spellings and eight trials each. Labels
separate promoted values from pending residues. The w=16 case distinguishes
upper-bit insertion with an existing pending value from streaming the whole
weight through it. Other permutations check migration interactions without
claiming to force aggregate execution order.

All 384 cells are analyzed in [weighted_pending.md](weighted_pending.md).
The w=16 cases preserve the existing pending 100/1 while directly inserting 200
at level 4; only the low remainder combines with pending weight. All prepared
investigation queries are now answered. General precision-to-k conversion,
replacement probabilities and merge/update rules remain unproven; no production
KLL support is registered by this investigation.
