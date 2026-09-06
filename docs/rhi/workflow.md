---
status: decided
covers:
  - docs/rhi/
---
# How RHI study works

> **Why this document exists.** The next stretch of RHI work is *reading* — API
> specs, Decima and RAGE papers, NVRHI and NRI source, Metal 4 and Vulkan 1.3
> documentation — and reading has no failure condition. It never terminates on
> its own, it produces no artefact anyone can check, and its conclusions arrive
> as opinions that are indistinguishable from the measured claims already in
> these documents. This is the process that stops that.

## 0. The rule

> **A study is a QUESTION with an answer that could have come out the other way.
> A design document holds only conclusions. The working stays in
> [`studies/`](studies/).**

Everything below follows from that. The failure it prevents is specific and this
repo has already seen its cousin: a design doc that grows by accretion until
nobody can tell which paragraphs were measured, which were argued, and which
were absorbed from a conference talk in 2019.

## 1. The evidence ladder

Every claim in this directory sits on one of these rungs, and **the rung is
stated, not implied**. A claim whose rung is not obvious from its wording must
say it.

| rung | means | example in this directory |
|---|---|---|
| **1. Measured here** | a number from this tree, on named hardware, on a named date | `Render.extract` 18.8 ms of a 24.8 ms CPU frame / 9.11 ms GPU at 50 000 props |
| **2. Counted here** | a fact about the tree a script could re-derive | "~72 distinct `bgfx::` symbols"; "5 files couple loading to GPU upload" |
| **3. Vendor specification** | a normative document from the API owner | Metal 4 resources are untracked by default; `descriptor_indexing` is core in Vulkan 1.2 |
| **4. Reproduced elsewhere** | a number someone else published with method | a shipped engine's stated draw-call budgets |
| **5. Prior-art argument** | how a shipped engine solved it, from a paper or talk | Unreal's single apply point; Unity's `SparseUploader` |
| **6. Inference** | our reasoning from the above | "Metal 4 is structurally Vulkan's shape, so two backends suffice" |

**Rungs 5 and 6 may not, on their own, close a decision in
[`open-decisions.md`](open-decisions.md).** They can *propose* one. This is the
same standard `engineering-standards.md` §3 applies to numbers, extended to
architecture: a claim nobody can check is a claim that will quietly stop being
true.

The one exception is a decision that is explicitly a **bet** — axiom 6's
Vulkan-on-Windows call is rung 6 and says so, and names its fallback. A bet is
allowed. A bet that does not know it is one is not.

## 2. The lifecycle of a study

```
  a question              open-decisions.md, or an entry in studies/README.md
      ↓
  studies/NNN-slug.md     created with the TEMPLATE, status: open
      ↓                   ← states its falsifier BEFORE the work starts
  the work                reading, a spike, a measurement
      ↓
  status: concluded       finding + verdict + rung, in the same file
      ↓
  propagation             the CONCLUSION lands in the right design doc;
                          open-decisions.md loses an entry or gains a ~~strike~~
      ↓
  the study is history    never edited again except to mark it superseded
```

Two properties are load-bearing:

**The falsifier is written first.** Before the reading starts, the study says
what result would make the answer *no*. A study that cannot name one is not a
study — it is advocacy, and it should be labelled a position instead. This is the
written form of what already happens informally here: NEON was declined on a
measurement, and the QoS hypothesis was half-refuted by its own test.

**A concluded study is never rewritten.** If a later study overturns it, the old
one gets a `superseded-by:` line and keeps its wrong answer visible. The record
of having been wrong is the most useful thing in the directory — it is how you
learn which rungs you over-trust.

## 3. Where a conclusion goes

A study's verdict is propagated to **exactly one** design document, and the study
file records which. This is what stops the design docs turning back into one
monolith.

| the study concluded something about… | it lands in |
|---|---|
| whether the project is worth doing at all | [`decision-record.md`](decision-record.md) |
| what bgfx costs, or what it can already do | [`evidence-bgfx.md`](evidence-bgfx.md) |
| what an API must or must not expose | [`design-axioms.md`](design-axioms.md) |
| the shape of a type or a call | [`design-api.md`](design-api.md) |
| ordering, or what a phase must prove | [`phases.md`](phases.md) |
| what we can and cannot trust a number to mean | [`method-measurement.md`](method-measurement.md) |

If a verdict seems to belong in two places, it belongs in the more general one
and the other gets a link. Duplicated prose is how two documents start
disagreeing — this directory already carries one such scar, recorded in
[`open-decisions.md`](open-decisions.md) decision 3, where two documents stated
incompatible minimum specs for months and nobody had written the conflict down.

## 4. Reading has a shape too

Most of this month is prior art, which is rung 5 — the weakest rung that still
counts. To keep it useful:

- **Read against a question, not a topic.** "How does Decima handle the
  extract/submit sync?" terminates. "Read about Decima" does not.
- **Record what you actually read**, not what the paper is famous for. Title,
  venue, year, and the specific section. A citation nobody can follow is rung 6
  wearing rung 5's clothes.
- **Prefer source to talks.** NVRHI and NRI are readable, shipping, reusable
  RHIs with the exact scope we want. Their *choices* are rung 5; their *code* is
  closer to rung 3, because it is normative for at least one shipping product.
- **Write the disagreement down.** Where two engines solved the same problem
  differently, that difference is the finding. Where they agree, the agreement
  is stronger than either.

## 5. When the study phase ends

Not on a date. It ends when **[`open-decisions.md`](open-decisions.md) contains
nothing that reading could answer** — every remaining item is either decided, or
blocked on hardware the farm does not have yet.

That condition is deliberately reachable. There are seven open decisions today,
two of them already struck through. Three are answerable by reading alone; the
rest need G0a or G0b.

**And the study phase does not block G0a.** The bgfx GPU-driven spike
([`phases.md`](phases.md) G0a) needs no decision from this directory — it uses
compute, indirect draws and storage buffers that bgfx already exposes and this
engine has never called. It is the forcing function: reading Decima's culling
chapter is far more useful after you have hit the wall yourself and know which
paragraph you are looking for. Run it *during* the research, not after.

## 6. What this process is not

It is not a requirement that every idea become a study. Small facts go straight
into the design docs with their rung attached. A study is for a question that is
**expensive to get wrong** — a backend count, a binding model, a phase order.
There should be few of them, and each should be worth the file.
