# INIT/MERGE update investigation

This is an investigation, not an implementation specification. All observations below
come from the immutable BigQuery answers in this directory. Arithmetic on their decoded
state is local analysis, **not** a new independently verified query answer. No formula
or transition described as a hypothesis is sufficient to register INIT or MERGE.

All prepared rounds are now answered. The latest findings and consolidated
checkpoint are in [weighted_pending.md](weighted_pending.md); precision-to-k
conversion and random replacement laws remain unresolved.

## Reproduce the analysis offline

```sh
go run ./tools/kll tools/kll/*.answers.txt > /tmp/kll-states.jsonl
```

The command emits one JSON object per hex sketch in a recorded `?>` response (currently
5046 cells, including repeated source/merged bytes). Each object retains filename, line
and JSON cell path; consult the SQL at that location for input, precision and weight.
It reports field 1 as `capacity_field_1`, decoded level values, integer level mass and
sampler fields separately. It does not query BigQuery, calculate expected percentiles,
validate BigQuery's full input language, or turn writer observations into golden tests.
Only the observed INT64/FLOAT64 encodings and protobuf wire types 0/2 are implemented;
unknown fields are not interpreted. A future accepted encoding can require extending
this diagnostic parser. FLOAT64 NaN/infinities are strings in its JSON output. No
floating-point conversion is used for INT64 values or mass calculations.

## What the existing bytes now constrain

### Capacity is not a hard per-level limit

The precision-1000, n=4096 specimen has counts `[1142, 1477]` and field 1 = 2954.
Its upper level contains the odd values 1..2953, and its lower level is 2955..4096.
This is consistent with compacting a sorted block of 2954 into 1477 values and
continuing to append. It does **not** establish insertion order, the general trigger,
which parity is chosen on another run, or whether odd blocks preserve a leftover.

More importantly, the precision-1000, n=16384 specimen has counts
`[129, 1449, 3339]`. Thus:

- A retained level can exceed field 1 (3339 > 2954). Do not enforce `size[level] <= k`,
  even at the top level, as a validity or post-update invariant.
- The total number of retained items can exceed field 1 (4917 > 2954).
- Its level mass is 16383, not 16384, with no sampler. Do not invent a unit-weight
  remainder just to restore count equality.

A lazy/global-budget compaction policy is a candidate explanation, but the bytes do
not identify its budget, growth policy or next-level selection. A static per-level
capacity schedule alone does not explain when compaction occurs.

### A weighted update is not just the set bits of its weight

The precision-1000 single-input INT64 specimens with weights 1, 2, 127 and 128 have
one value at each set-bit level and no sampler. This description fails for the
weight-2147483647 specimens, in **both** types:

| State component | Decoded observation |
| --- | --- |
| Outer count | 2147483647 = 2^31 - 1 |
| Levels 0..12 | Empty |
| Level 13 | Two copies of the input value |
| Levels 14..30 | One copy at each level |
| Level mass | 2147483648 = 2^31 |
| Sampler | Field 3 = 13; no sampled value or weight |

The low 13 set bits have effectively been replaced by one additional level-13 item.
This is consistent with flushing/rounding a low-weight remainder through a sampler,
but one captured random outcome does not distinguish stochastic rounding from an
unconditional flush or other weighted fast path. In particular, do not implement
`weight => w` by set bits alone, or assume every unresolved remainder is serialized.

### Sampler field 3: initial cutoff hypothesis

For every original `sketch_format.answers.txt` state that has field 6, field `6.3`
is the index of the **lowest nonempty level**. The completed update round below
**disproves that equality as a general rule**, even for original weighted INITs. This includes the maximum-weight
specimens with no sampled value. It is not always the top level:

| Population / precision | Nonempty levels (index:count) | Field 6.3 | Sampler weight |
| --- | --- | --- | --- |
| 1..4 / 1 | 1:2 | 1 | 1 |
| 1..8 / 1 | 2:2 | 2 | 3 |
| 1..16 / 1 | 3:1 | 3 | 7 |
| 1..32 / 1 | 4:1 | 4 | 9 |
| 1..64 / 1 | 5:2 | 5 | 11 |
| 1..128 / 1 | 6:1 | 6 | 43 |
| 1..256 / 1 | 7:1 | 7 | 101 |
| 1..1024 / 1 | 9:2 | 9 | 29 |
| Weighted singleton / 1000, w=2^31-1 | 13:2, 14..30:1 each | 13 | Absent |
| FLOAT64 -256..256, step 1/8 / 1 | 11:2 | 11 | 854 |
| Mixed-precision merged INT64, n=8192 / 1 | 12:1 | 12 | 1539 |
| Mixed-precision merged FLOAT64, n=8192 / 1 | 12:2 | 12 | 1025 |

Each nonzero observed sampler weight is strictly less than `2^(6.3)`. A candidate
model is a sampler below a cutoff `h`, emitting items of weight `2^h` to level `h`.
Neither this bound nor the cutoff equality is established for all accepted bytes:
the archived mutation from field 3 = 3 to 4 still extracted successfully, without
moving level 3's value. Reader acceptance and writer invariants must stay separate.

The sampler weight is **not** `n mod 2^h`: n=16 has h=3 and sampler weight 7, whereas
`16 mod 8 = 0`. Nor is it necessarily `n - level_mass`. A proposed update rule must
explain how changing the cutoff reweights or flushes pending state, and why retained
mass can differ from input count. A fixed-count weighted reservoir alone does not
explain those differences. Ordinary set-bit updates alone do not explain them either.

For a geometric capacity hypothesis, the weighted example's top level is 30 and its
cutoff is 13, a depth of 17. Numerically, `2954 * (2/3)^17 = 2.998189556...`.
The precision-1 states have field 1 = 3 and cutoff equal to their top level (depth 0).
These are compatible with a 2/3 schedule and a near-three-item cutoff, but do not
identify the rounding rule, minimum capacity or even prove that schedule. In
particular, floor/ceil/nearest rounding near this boundary can move the cutoff.
Do not import Apache DataSketches' capacity formula on this evidence.

### Precision conversion remains unidentified

The six singleton `(precision, field 1)` pairs are
`(1,3), (2,5), (100,280), (1000,2954), (2000,6008), (100000,330058)`.
They exclude a constant multiplier, but do not distinguish logarithmic corrections,
implicit error-bound solving or a piecewise/table-driven conversion. Fitting a curve
to six points is not a compatibility argument. Measure small consecutive precisions,
boundaries and both types before proposing an exact integer formula.

## Findings from the completed update round

### Capacity conversion: type-independent on all 39 sampled precisions

Query 1 returned the same field 1 for both types at every tested precision:

```text
p: 1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16
k: 3  5  8 10 13 16 18 21 24 26 29 32 35 37 40 43

p: 31 32 33  63  64  65  99 100 101 255 256 257
k: 84 87 90 174 177 180 277 280 282 729 732 735

p:  999 1000 1001 1999 2000 2001  65535  65536  65537  99999 100000
k: 2951 2954 2957 6005 6008 6011 214115 214118 214121 330055 330058
```

No exact conversion formula is established. A fitted power law also remains only a
fit, not a derivation of Google's error guarantee or an implementation rule.

### First compaction and odd-length behavior are directly visible

Queries 2..5 show first compaction **at** n=k, not n=k+1, for k=3,5,280,2954.
At precision 2, the four n=5 INT64 outputs are `[2,4]` or `[1,3,5]` at level 1,
with empty level 0 and no sampler. There is no exact odd leftover: keeping every
other sorted value can produce floor(n/2) **or** ceil(n/2) retained items. This
explains a local +/-1 change in level mass without a missing serialized field.
It is inconsistent with the common implementation that always leaves one odd item
behind and compacts only the even remainder. At even k=280 and 2954, both sorted
parities are observed with exactly k/2 values.

Later compaction is not repeated at the same lower-level size. At p=100, n=561,
level counts are `[281,140]`; at p=1000, n=5909, they are `[2955,1477]`.
These directly disprove even a hard **bottom-level** k bound after height growth.
A candidate global budget for H levels is `sum(rounding(k*(2/3)^d), d=0..H-1)`;
when total item count reaches the budget, compact an eligible level. For p=1000,
ceil versus nearest rounding predicts the second trigger at n=6401 versus 6400,
respectively, assuming equality triggers and no other budget term. The completed
[transition round](transitions.md) samples those neighbors: it finds the second
trigger at n=6401, supporting ceil over nearest rounding for this budget.

### Weighted INIT: cutoff tracks height, not the first populated level

Queries 6..8 agree in all 648 weighted response cells with the following
**candidate singleton model**, for the three tested precisions only:

1. Let `L=floor(log2(w))`. Use depth D=0 for p=1, D=2 for p=2, D=17 for p=1000.
2. Set cutoff `h=max(0,L-D)` (field 6.3 when a sampler message is present).
3. Preserve each set-bit contribution above h. At h, stochastically round the
   remainder to a multiple of `2^h`; retain zero, one or two copies as appropriate.
4. No pending sampled value or weight remains in these captured singleton outputs.

For example, p=2, w=9 has L=3, h=1, and retained mass **8 or 10**. At w=15 it is
**14 or 16**. This disproves deterministic ceil/floor rounding of weighted
remainders; it does not measure an unbiased-rounding probability. At p=1000,
w=2^30+1, field 6.3 is 13 but the **only nonempty level is 30**. Thus field 3
is not the lowest populated level or simply an annotation for EXTRACT. A cutoff
can exist even when no item happens to occupy it. The triples (k,D)=(3,0),(5,2),
(2954,17) fit `D=ceil(log_(3/2)(k/3))`; more precisions are needed to separate that
formula from alternative schedules/rounding. The completed
[transition round](transitions.md) extends the matching set to 13 distinct precisions.

### MERGE is not reinsertion through the destination sampler

Queries 9..11 merge the fixed p=1, n=16 state (level 3 value 4, pending value 16,
weight 7, cutoff 3) with a weighted singleton 100. In all eight w=1 results,
100 remains at **level 0**, with the existing level-3 value and pending sampler
unchanged. Query 14 similarly keeps a level-1 item below cutoff 3. A writer cannot
assume all levels below the sampler cutoff are empty, or merge by routing every
incoming level value through that sampler.

Increasing only field 6.3 from 3 to 4 changes merge behavior even though earlier
EXTRACT observations were unchanged. All eight w=16 merges of that mutation keep
`level3=[4], level4=[100]`; the original state instead produces one or two values
at level 4 with cutoff 4. The mutated one-input merge appends an empty level 4
rather than restoring the cutoff to 3. This is evidence that the field participates
in future updates and level layout, not proof that arbitrary mutations are valid.

Setting sampler weight to zero removes its sampled-value field on one-input merge
while retaining cutoff 3. All eight original one-input merges preserved the original
bytes; neither result implies a universal serialization identity guarantee.

Query 12 merges the fixed original state with itself. Every output has cutoff 4,
pending value 16 with weight **6**, and level4 `[4]` or `[4,16]`. This is consistent
with combining pending weights 7+7, emitting one weight-8 item, retaining remainder
6, then compacting the three level-3 items with the odd parity rule. It does not
establish the general sampler-combination or replacement probability. Query 13's
unequal-cutoff merge has zero pending weight in every trial, but cannot alone
identify how smaller levels and a smaller sampler are resampled.

Query 15 downgrades to precision 1 for both the exact-low and compacted-low mixed
pairs. It provides no evidence for a compacted-only downgrade exception, nor does
it settle merge-order dependence.

## Completed transition round: four queries

The answers and detailed findings are now in
[transitions.answers.txt](transitions.answers.txt) and [transitions.md](transitions.md).
The round asked for:

1. Maximum-weight / power-of-two singletons at 11 further precisions, to test the
   proposed cutoff-depth calculation without a huge unweighted population.
2. p=100, n=604..609: the predicted second global-budget compaction boundary.
3. p=1000, n=6398..6403: distinguish candidate ceil/nearest rounding at that boundary.
4. Equal-cutoff fixed-state merges for every pending-weight pair 0..7, with sampled
   values 16 and 15, four trials each. These deliberately mutated states isolate
   pending-weight addition and labelled replacement from source INIT randomness.
   Rejection is informative; their acceptance is not an original-INIT invariant.

All four queries were answered, yielding 326 hex cells. The two-level boundary
supports ceil rounding; all 256 equal-cutoff sampler merges leave weight
`(a+b) mod 8`, and overflow can give the promoted and pending values different labels.
The fixed merge grid is still not a statistical test of replacement probabilities.
The two higher/unequal-cutoff sampler follow-ups are also completed; see
[sampler_updates.md](sampler_updates.md). The three
[reverse-order/absent-sampler controls](merge_order.md) are also complete;
the [weighted-stream follow-ups](weighted_stream.md) and
[three-input weighted/pending query](weighted_pending.md) are complete too.
No prepared KLL investigation query remains unanswered.

## Completed update round: 15 queries

[`updates.answers.txt`](updates.answers.txt) contains the maintainer-recorded answers
to all 15 queries (1788 hex response cells). All queries reached their intended
operation. The original collection design is retained below to explain the populations.
All subsequent follow-ups are also archived; see the
[latest checkpoint](weighted_pending.md). These queries return bytes for
investigation, not stable expected INIT/MERGE serializations. Preserve answers
and source context rather than re-running archived rounds. For future collections,
validate the unanswered format offline with `just bigquery-answers-validate`;
maintainers, not agents, collect BigQuery answers. The format validator does not
validate SQL syntax or analyzer acceptance; any analysis error is not evidence
about updates.

| Queries | Question / interpretation after collection |
| --- | --- |
| 1 | 39 literal precisions, both types: does field 1 depend on type? Use the dense small grid and power-of-two neighbors to reject candidate rounding/conversion rules; a match still requires a derivation or more evidence. |
| 2..5 | Four trials per n around small and known field-1 boundaries: first compaction, sampler creation, level growth, and whether constant FLOAT64 has the same count transitions as distinct INT64. These populations do not isolate type from duplicate handling; a difference requires a follow-up with equal populations. |
| 6..8 | Single weighted inputs at precision 1, 2 and 1000, near powers of two: which low levels disappear, whether remainder promotion varies, whether sampler field 3 depends on highest weight bit, and whether the 2^13 boundary explains the maximum-weight specimen. |
| 9..11 | Merge fixed n=16 p=1 bytes, with original sampler, only field 3 mutated, or only sampler weight set to zero, plus exact singleton weight 1/7/8/9/15/16/17. Weight 0 labels the one-input identity control; no invalid zero weight is passed to INIT. Does state normalize on load, and at what added mass does promotion occur? |
| 12..14 | Fixed sampler/sampler pairs with equal or unequal cutoffs, and sampler/exact pair: how is pending weight combined, when does the cutoff rise, and do duplicate input sketches retain extrema/count? |
| 15 | Exact low-precision versus compacted low-precision mixed with a fixed exact high-precision singleton: does precision downgrade depend on retained/compacted state? This does not measure merge-order dependence. |

All grouped trials are independent aggregate groups, not guarantees of independent
random seeds or sequential update order. Repeated results cannot establish a
probability or prove determinism. Fixed source bytes remove source-INIT randomness
from the merge comparisons; MERGE itself can still be random and executed through
an aggregation tree. Do not infer an insertion-order or merge-order law from UNION
ALL order. If a deterministic transition needs order, prepare an explicit fixed-byte
one-step merge chain after this round, rather than relying on input scan order.

For each collection, decode **all** outputs and record `(count, precision, field 1,
level counts, level mass, sampler value/weight/field3)` next to their parameters.
Distinguish rejection, normalization on one-input merge, and actual multi-input
updates. To investigate reservoir replacement probabilities later, use labelled
values and a separately designed statistical round; these four/eight/sixteen-trial
groups do not establish the sampler's random replacement law. Writer interoperability
still needs BigQuery to consume emulator-produced bytes in both directions after
an implementation exists.
