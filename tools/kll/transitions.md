# Capacity, compaction and sampler transitions

This records the completed four-query follow-up in
[transitions.answers.txt](transitions.answers.txt): 326 hex response cells, with no
analysis errors. It follows the [15-query update round](updates.md). These are
maintainer-recorded BigQuery observations, not emulator-generated answers. Do not
regenerate their non-deterministic INIT/MERGE bytes as golden serialization tests.

## 1. Two-level global budget: ceil wins over nearest rounding

The extra unit-input boundary queries returned these level counts:

| Precision | n | Retained counts by level |
| --- | --- | --- |
| 100 (k=280) | 604, 605, 606 | `[324,140]`, `[325,140]`, `[326,140]` |
| 100 | 607 | `[0,303]` or `[0,304]` |
| 100 | 608, 609 | `[1,303/304]`, `[2,303/304]` |
| 1000 (k=2954) | 6398, 6399, 6400 | `[3444,1477]`, `[3445,1477]`, `[3446,1477]` |
| 1000 | 6401 | `[0,3200]` or `[0,3201]` |
| 1000 | 6402, 6403 | `[1,3200/3201]`, `[2,3200/3201]` |

Immediately before the second compaction, the retained item count is
`n - k/2`. Thus the observed thresholds correspond to:

```text
p=100:  607 - 140  = 467  = 280  + ceil(280 * 2/3)
p=1000: 6401 - 1477 = 4924 = 2954 + ceil(2954 * 2/3)
```

The p=1000 result rejects the previously proposed **nearest-rounded** two-level
budget (4923, triggering at n=6400). With two levels, ceil of the whole sum and
sum of per-level ceils are identical; this boundary alone cannot distinguish them.

### Count-only exploration also separates the two ceil schedules

The offline command now includes a deliberately hypothetical compactor:

```sh
go run ./tools/kll -model-k 5 -model-steps 30
go run ./tools/kll -model-k 280 -model-steps 1121
go run ./tools/kll -model-k 2954 -model-steps 16384
```

For each unit-weight insertion it enumerates possible **count vectors**, using:

```text
H = number of retained level messages
capacity[i] = ceil(k * (2/3)^(H-1-i))
budget = sum(capacity[i])

append one item to level 0
while total retained items >= budget:
  choose the lowest i with count[i] >= capacity[i]
  clear that level, promote floor(count[i]/2) OR ceil(count[i]/2)
  grow the level list if necessary; recalculate capacities and budget
```

It uses exact integer arithmetic, not floating-point powers, and enumerates both
possible count parities without assigning probabilities. It does not model values,
sort order, error guarantees, parity correlation, weighted updates or sampler
migration. It explicitly fails if any explored branch needs a capacity below 3;
for example, k=5 reaches that unsupported branch at n=31. This is not an emulator
backend, and its output is not an independent expected BigQuery answer.

Every observed count vector for the sampler-free unit-input forms at p=2/n<=30,
p=100, and p=1000 in the original/update/transition rounds lies in these candidate
sets, including the old p=1000/n=16384 vector `[129,1449,3339]`. Membership is only
a necessary condition: the candidate sets may include outcomes BigQuery never
produces, and this does not identify the probability or unique compaction policy.

Replacing the budget with `ceil(sum(k*(2/3)^d))` **fails** against the already
recorded p=2 unit-input data. At three levels it gives 11 instead of 12. For
example, the recorded n=19 vector `[8,0,3]` has 11 items and survives; that
alternative budget would already have compacted it. The sum-of-ceils candidate
permits it. This is stronger evidence for individual rounding than the two-level
boundary, but still does not prove the full schedule or lowest-eligible rule.

## 2. Weighted singleton cutoff formula fits 13 distinct precisions

Transition query 1 extends the prior p=1/2/1000 observations. For w=2^30 and
w=2^31-1, both outputs at each precision have the following cutoff h and depth
D=30-h:

| Precision | k | D | h |
| --- | --- | --- | --- |
| 1 (earlier round) | 3 | 0 | 30 |
| 2 (earlier round) | 5 | 2 | 28 |
| 3 | 8 | 3 | 27 |
| 4 | 10 | 3 | 27 |
| 5 | 13 | 4 | 26 |
| 8 | 21 | 5 | 25 |
| 10 | 26 | 6 | 24 |
| 16 | 43 | 7 | 23 |
| 100 | 280 | 12 | 18 |
| 256 | 732 | 14 | 16 |
| 1000 | 2954 | 17 | 13 |
| 2000 | 6008 | 19 | 11 |
| 100000 | 330058 | 29 | 1 |

All match the candidate `D=ceil(log_(3/2)(k/3))`, with singleton cutoff
`h=max(0,floor(log2(w))-D)`. Equivalently, D is the first depth at which the
unrounded geometric capacity is <=3. This connects the sampler boundary to the
2/3 schedule independently of ordinary compaction counts. It does not establish
behavior at exact geometric/integer boundaries that were not sampled, or the
precision-to-k conversion itself.

All power-of-two inputs retain only their level-30 item and zero pending weight,
even when h is much smaller. For w=2^31-1 at p=100000, the output has one item
at each level 1..30, mass **2^31-2**: this round captured rounding down of the
lowest unit remainder, unlike the higher-cutoff examples that rounded up. The
candidate singleton rounding model permits both; it must not be strengthened to
always round this maximum weight upwards.

## 3. Equal-cutoff sampler merging: residual weight and value separate

Query 4 independently mutates two fixed n=16/p=1 states. Both retain level3=[4]
and cutoff h=3. The left pending value is 16 with weight a, the right pending value
is 15 with weight b; a,b range over 0..7, with four trials per pair. Changing the
pending weights deliberately leaves outer count unchanged; these are accepted
mutation experiments, not claims about original INIT states.

All **256** outputs match the following structural observations:

- Pending weight is `(a+b) mod 8`, including exact boundary a+b=8.
- If a+b<8, cutoff stays 3 and level3=[4,4]. A zero-weight pending value is absent.
  If only one input has nonzero pending weight, its value survives. With both
  positive, either labelled value can survive across the grid, consistent with a
  weighted reservoir; this small sample does not establish a replacement probability.
- If a+b>=8, cutoff becomes 4 and the only nonempty level is level 4: either `[4]`
  or `[4,15]` or `[4,16]`. A weight-8 promotion followed by odd compaction of
  `[4,4,promoted_value]` explains these outcomes.
- If a+b>8, the pending value is **15 in every one of the 84 outputs**, with weight
  a+b-8. It is not necessarily the promoted value. If a+b=8 it is absent.
- The residual is modulo the **old** unit 8, not the new unit 16 after compaction
  raises h. Cutoff migration need not scale the pending weight.

A compatible candidate processes one incoming weighted value in chunks: fill a
reservoir up to the current unit, emit its selected value, then retain the unused
part of the incoming value as a new pending reservoir. That explains why the
residual is labelled 15 after overflow, instead of sharing a single randomly
selected value with the promoted item. In contrast, choosing one representative
for all a+b mass and using it for both promotion and residue does not explain the
observed label separation. The collection does not guarantee aggregate merge order;
label 15's behavior is an observation of these queries, not a promise that SQL
UNNEST always selects the right input as the receiver's next update.

The pending reservoir is therefore distinct from compactor parity sampling. The
remaining unknowns include replacement probabilities, incoming weights spanning
multiple units, unequal-cutoff migration, merge-tree/order effects, and the exact
precision-to-k conversion. The no-sampler count model does not settle them.

## Completed sampler collection: two queries

[sampler_updates.answers.txt](sampler_updates.answers.txt) now contains all 1536
response cells; detailed findings are in [sampler_updates.md](sampler_updates.md).
The round asked for:

1. All pending-weight pairs 0..15 at h=4, checking whether the same overflow/residue
   structure generalizes from unit 8 to unit 16.
2. Mixed h=3/h=4 states, weights 0..7 and 0..15, checking migration and interaction
   with the retained lower-level item.

Both use the recorded query-4 a=b=7/trial=0 output as their h=4 source:
outer n=32, p=1, level4=[4], pending value 15/weight 6, cutoff 4. Only pending value
and weight are mutated; the h=3 source is the earlier fixed n=16 sketch. Full
bytes and source inputs remain in their archive. There are four trials per pair,
not enough to establish probability laws. No hypothetical serialization is being
promoted as a writer expectation.

All 1024 equal-cutoff responses follow the modulo-16 residual structure; the 512
mixed-cutoff responses also keep pending weights in the same units, but migrate the
lower level even with zero pending weight. That migration is separate from ordinary
global-budget compaction. The subsequent
[reverse-order/absent-sampler controls](merge_order.md) are also complete;
the [weighted-stream follow-ups](weighted_stream.md) and
[three-input weighted/pending query](weighted_pending.md) are complete too.
No prepared KLL investigation query remains unanswered.
No KLL function is registered by this investigation.
