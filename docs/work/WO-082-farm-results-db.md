---
status: plan
id: WO-082
title: The farm's results database, A/B statistics and nightly report
program: process
priority: P2
size: M
state: todo
depends: [WO-081]
touches:
  - tests/corebench/summarise.py
source: docs/plans/farm-and-perf-ci.md §5.1; automated-testing-soak-fuzz-plan.md Parts 5 and 7
---
## Why
"Is this commit slower than last week's" is the question the server exists to answer, and one file per run cannot answer it.

## Done when
- [ ] the SQLite schema from Part 5 (machine, build, run, sample, baseline, finding), created by a script
- [ ] an ingester for `jobs_bench --json` (schema 1), and the same shape for every later benchmark
- [ ] A/B runs interleaved, at least 7 each; a change is flagged only beyond the noise floor and with Mann-Whitney p < 0.01
- [ ] a static HTML report each night: what changed, trends per metric, links to flame graphs; served on the home network
- [ ] the database copies to the Mac with one `rsync`

## Contract
Nothing.
