# Merge direction and absent-sampler controls

All three queries in [merge_order.answers.txt](merge_order.answers.txt) were
answered by a maintainer: **584 hex cells**, no analysis errors. They follow the
[higher/unequal-cutoff sampler round](sampler_updates.md). Recorded bytes remain
immutable investigation evidence, not stable writer expectations.

## Reversing the pair changes lower-level preservation and residual labels

The same low/high states and pending weights were used as before:

- Low: n=16, p=1, level3=[4], h=3, pending value 16/weight a in 0..7.
- High: n=32, p=1, level4=[4], h=4, pending value 15/weight b in 0..15.

The prior query spelled the input array `[low, high]`; the new query spells it
`[high, low]`. Both have four trials per pair. The a/b labels still identify low
and high states, not array positions.

All 512 new responses have pending weight `(a+b) mod 16`. The significant
structural differences are:

| Pending sum | New cells | New retained levels/cutoff |
| --- | --- | --- |
| a+b<16 | 400 | **level3=[4], level4=[4]**, h=4 in every cell |
| a+b=16 | 18 | level4=[4,15] or [4,16], h=4 |
| a+b=16 | 10 | level5=[4] or [4,15] or [4,16], h=5 |
| a+b>16 | 43 | level4=[4,15] or [4,16], h=4 |
| a+b>16 | 41 | level5=[4] or [4,15] or [4,16], h=5 |

In all **84** a+b>16 cells, the pending value is now **16**, rather than the
previous round's 15. The modulo rule is unchanged; the leftover label tracks the
other input. At exact equality the pending value is absent.

For all 400 below-unit sums, level 3 now survives; in the prior spelling it never
survived. Thus unconditional normalization of **every** input's levels to the
maximum cutoff cannot describe both collections. A lower-than-cutoff retained
level is a valid observed MERGE output, not malformed state to repair eagerly.

These are differences between two independently collected query spellings. SQL
UNNEST does not contractually force an aggregate evaluation order, and the finite
samples do not prove a deterministic left-fold law. Nevertheless, the collections
provide concrete constraints on a candidate directional merge primitive.

## Overflow compacts the previously preserved lower level

When a+b reaches 16, no reverse-grid output retains level 3 either. A compatible
sequence explains both the below-unit and overflow cases:

1. Start with the high state, then merge the low state's levels without increasing
   the receiver's height. The lower level is appended without immediate normalization.
2. Fill the pending reservoir at unit 16. A positive leftover belongs to the newly
   incoming low state's value 16.
3. If no item is emitted, there are only two retained items, below the global
   budget of 3, so both levels survive.
4. If one item is emitted at level 4, total retained count becomes three. Compact
   the below-cutoff level 3, promoting zero or one of its singleton 4.
   Level 4 then has two or three items; only the latter compacts again to level 5.

For the prior `[low, high]` collection, growing the receiver to the incoming higher
level instead migrated its old level 3 immediately, even without overflow. This
suggests **height/cutoff growth migration** and **budget-triggered compaction** are
separate steps. It does not yet prove their exact internal sequencing or the
eligibility test used for arbitrary below-cutoff level populations.

Together with the earlier sampler/exact merges, these observations reject a
single policy of reinserting every incoming value through the target sampler or
normalizing every low level on every MERGE. Preserving source level indices is
necessary in at least some merge paths.

## Empty versus absent sampler: same identity normalization here

Query 2 uses the original low state with pending weight 7 or weight changed to 0;
query 3 removes its entire sampler field 6 and corrects the payload length. Each
variant has eight trials of one-input MERGE, `[low,high]`, and `[high,low]`. The
high state has zero pending weight to isolate migration from overflow.

All 72 controls were accepted:

| Low variant | One-input MERGE | low/high pair | high/low pair |
| --- | --- | --- | --- |
| Pending 7 | level3=[4], pending 16/7, h=3 | level4=[4] or [4,4], pending 16/7, h=4 | level3=[4], level4=[4], pending 16/7, h=4 |
| Pending 0 | level3=[4], no pending value/weight, h=3 | level4=[4] or [4,4], no pending value/weight, h=4 | level3=[4], level4=[4], no pending value/weight, h=4 |
| Sampler absent | Same normalized bytes as Pending 0 in all identity trials | level4=[4] or [4,4], no pending value/weight, h=4 | level3=[4], level4=[4], no pending value/weight, h=4 |

The absent field is therefore **not** preserved by this one-input MERGE: it
synthesizes field 6 containing cutoff 3, with no pending value or weight. A reader
and writer cannot equate “no sampled value” with “no sampler/cutoff.” Absence also
does not make this high-level low-precision state behave like a low-level exact
singleton. This conclusion is scoped to the tested state; arbitrary accepted
encodings may normalize differently.

Changing the pending weight does not control the migration distinction: the
zero-weight and absent-sampler controls reproduce it. Counts of the random one-
versus two-copy results are observations, not replacement/compaction probabilities.

## Consolidated candidate model and remaining boundary

The ordinary compaction, cutoff and merge observations are now compatible with:

- Individually ceil-rounded geometric capacities, summed for a global budget;
  ordinary compaction chooses an eligible low level and retains either parity,
  including odd blocks without an exact leftover.
- A height-dependent cutoff `h=max(0,H-1-D)`, with candidate
  `D=ceil(log_(3/2)(k/3))`. Do not overwrite a loaded field 6.3 blindly: earlier
  deliberate mutations show it affects future updates.
- A pending reservoir below unit `2^h`, filling to the unit, emitting a selected
  value and starting a separately labelled leftover from unused incoming weight.
  Ordinary cutoff growth can preserve that leftover weight unchanged.
- A directional merge step that can migrate the receiver's old levels on height
  growth, yet preserve incoming lower levels when no growth/compaction is needed.

These are candidate explanations, not a production implementation specification.
Precision-to-k conversion, random replacement laws, full migration/eligibility
rules and general weighted INIT remain unresolved. In particular, the observed
**weighted singleton** outputs have no pending weight, while ordinary unit-input
INITs and MERGEs can retain one; simply applying a uniform streaming reservoir
update to every weighted input has not been established.

Two completed queries in
[weighted_stream.answers.txt](weighted_stream.answers.txt) isolate that last
distinction: a weight-2^20 input plus one small weighted
input, with both array spellings, eight trials each, at p=1 and p=1000. The large
input alone is an exact power-of-two sketch; candidate units are respectively
2^20 and 8. They test whether a small added weight is retained, rounded immediately,
or split across units, and whether query spelling changes that observation. No
answer is guessed and no aggregate execution order is promised.

All 368 cells are now analyzed in [weighted_stream.md](weighted_stream.md):
weighted INIT can retain a low remainder, and the observed upper-bit placement
is not equivalent to streaming the whole weight through a reservoir. One
three-input labelled follow-up is also complete; see
[weighted_pending.md](weighted_pending.md) for findings and the investigation
checkpoint. No production KLL function is registered.
