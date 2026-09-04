---
status: plan
covers:
  - docs/rhi/
---
<!-- Copy to NNN-slug.md. Fill sections 1–3 BEFORE doing any work; 4–6 after. -->
# Study NNN — <the question, as a question>

| | |
|---|---|
| **Status** | `open` \| `in-progress` \| `concluded` \| `abandoned` \| `superseded` |
| **Opened** | YYYY-MM-DD |
| **Concluded** | YYYY-MM-DD |
| **Verdict lands in** | `../<one design doc>.md` |
| **Highest rung reached** | 1–6, see [`../workflow.md`](../workflow.md) §1 |
| **Superseded by** | study NNN, if ever |

## 1. The question

One sentence, phrased so it has an answer. Then a paragraph on **why it is
expensive to get wrong** — a study is only worth a file when the cost of the
wrong answer is a rewrite rather than an edit.

## 2. The falsifier — written before the work

> **What result would make the answer NO?**

State it concretely enough that you would recognise it when you saw it. A study
that cannot name its falsifier is a position, not a study; label it as one and
put it in the design doc directly.

Also state what you currently *expect*, so the record shows whether the work
changed your mind. Being wrong here is the most valuable line in the file.

## 3. Method

What will actually be done, and which rung it can reach.

- reading: which documents, which sections
- spike: which scene, which counters, on which machine
- measurement: what is varied, what is held fixed, how many runs

## 4. What was found

Findings, each with its rung. Separate **what a source says** from **what we
conclude from it** — that boundary is where rung 5 quietly becomes rung 6.

Contradictions between sources are findings, not noise. Record them.

## 5. Verdict

The answer, in one sentence. Then:

- **Did the falsifier fire?** Yes / no / partially, and what that means.
- **What changed?** The exact edit made to the design document, so the
  propagation is traceable in both directions.
- **What is still unknown?** Anything the method could not reach.

## 6. Sources

Title, author, venue, year, and the specific section actually read. A citation
nobody can follow is inference wearing prior art's clothes.
