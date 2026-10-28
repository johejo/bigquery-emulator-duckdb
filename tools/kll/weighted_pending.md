# Weighted insertion into a nonempty reservoir

The query in [weighted_pending.answers.txt](weighted_pending.answers.txt) has
maintainer-recorded answers: **384 hex cells**, no analysis errors. It completes
the targeted follow-up to [weighted INIT before/after growth](weighted_stream.md).
The bytes remain immutable evidence, not golden writer expectations.

## Inputs and scope

Each aggregate uses precision 1000 and three labelled weighted inputs:

| Label | Value | Weight |
| --- | --- | --- |
| A | 42 | 2^20 |
| B | 100 | 1 |
| C | 200 | w in 8,9,15,16,17,23,24,25 |

All six array spellings have eight trials per w (64 cells per spelling). A alone
establishes level20=[42] and observed cutoff h=3, unit U=8. No collection here
approaches the candidate global retained-item budget.

All 384 outputs keep h=3 and level20=[42]. A/B/C describe the SQL input spelling,
not a contractual aggregate execution order. The observations support a candidate
sequential primitive but do not establish a universal left-fold execution law.

## Upper bits bypass an existing pending reservoir

ABC is the targeted spelling: A establishes height, B supplies pending 100/1,
then C adds the larger weight. All eight w=16 responses contain:

```text
level4 = [200]
level20 = [42]
pending value = 100, weight = 1, cutoff = 3
```

The pending value was not consumed or replaced, and there are not two level-3
copies of 200. This directly supports inserting C's upper bit at level 4 while
leaving a nonempty pending reservoir untouched. It rejects processing the whole
weight 16 by repeatedly filling and emitting from that reservoir, without an
additional unobserved transformation.

All 128 ABC/ACB cells fit the same split:

```text
upper = w - (w mod 8)
insert one 200 at each set-bit level of upper
combine only r = w mod 8 with pending weight 1
```

| w | Direct upper-bit levels | Pending result | Extra sampler emission |
| --- | --- | --- | --- |
| 8 | 3 | 100/1 | None |
| 9 | 3 | 100/2 or 200/2 | None |
| 15 | 3 | Empty | One 100 or 200 at level 3 |
| 16 | 4 | 100/1 | None |
| 17 | 4 | 100/2 or 200/2 | None |
| 23 | 4 | Empty | One 200 at level 3 in these samples |
| 24 | 3,4 | 100/1 | None |
| 25 | 3,4 | 100/2 or 200/2 | None |

For w=23, every captured sampler emission selected 200; that finite observation
must not be interpreted as excluding 100 or establishing a deterministic
replacement rule. For w=15, both emitted labels occur across the ABC/ACB cells.

The pending-weight pattern also agrees when B and C are swapped after A (ACB):
r=0 leaves the sole pending B untouched; r=1 gives pending weight 2 with either
label; r=7 fills the unit and emits one additional level-3 item. Equal structures
do not imply identical probabilities or order independence for arbitrary inputs.

## Permutations agree with growth migration, not a final flush

Let r=w mod 8. Every output has the following pending weight and value constraints:

| Spelling | Cells | Pending weight | Value when positive |
| --- | --- | --- | --- |
| ABC | 64 | (1+r) mod 8 | 100 for r=0; 100 or 200 for r=1 |
| ACB | 64 | (1+r) mod 8 | Same observed alternatives |
| BAC | 64 | r | 200 |
| BCA | 64 | 0 | Absent |
| CAB | 64 | 1 | 100 |
| CBA | 64 | 0 | Absent |

No captured output disagrees with these structural constraints. The non-A-first
forms can also retain one extra migrated low-level item at level 3, labelled 100
or 200 as applicable. These items are not converted into pending weight:

- BAC: B is present before the height increase, so it can be lost/promoted during
  migration. C's low remainder subsequently becomes the pending reservoir.
- CAB: C's lower bits can be rounded during A's height increase. B subsequently
  starts the pending reservoir with weight 1.
- BCA/CBA: both small inputs precede the height increase; their old lower-level
  contributions migrate without leaving a new pending reservoir.

This is consistent with using the **current, pre-insertion** cutoff to split a
weighted input, then handling any cutoff growth as compactor migration. It does
not require a special end-of-aggregate flush of weighted INIT remainders. The
ordinary unit-input and weighted-singleton observations can share the same
candidate state model.

## Investigation checkpoint

All prepared KLL investigation queries are now answered and archived. No new
unverified query is added at this checkpoint. The main structural candidates are:

1. A global retained-item budget from individually ceil-rounded geometric level
   capacities, with odd compaction choosing either sorted parity without keeping
   an exact odd leftover.
2. A height-dependent sampler cutoff and distinct height-growth migration, which
   may compact lower levels even before the ordinary global budget is reached.
3. Weighted insertion by direct upper-bit placement and a low-remainder reservoir;
   pending combination can emit a value and retain a differently labelled residue.
4. A directional merge primitive that can preserve incoming levels below the
   receiver's cutoff, instead of normalizing every input eagerly.

The evidence does **not** establish exact precision-to-k conversion, replacement
probabilities/unbiasedness, parity correlations, all migration/eligibility cases,
or arbitrary accepted-byte normalization. These remain implementation questions,
not reasons to promote a fitted formula or speculative probability as established
behavior. A match to possible outcomes is weaker than matching BigQuery's output
law. The existing count-only hypothesis explorer is still sampler-free; it has
not been extended into a production codec or KLL backend.

A production implementation will also need independent client-visible tests,
NULL/error/type boundary coverage and writer interoperability checked by BigQuery
consuming its bytes. The archived INIT/MERGE bytes must not become byte-for-byte
serialization assertions. No KLL function is registered by this investigation.
