---
status: unreviewed
---
# Fuzz corpus — the regression gate

One `.seeds` file per origin, one decimal seed per line (`#` comments allowed).
Seeds, not blobs: a seed is ~20 bytes in git and regenerates the entire
structured input, so the corpus stays reviewable and diffable.

**This directory is the mechanism that makes "the engine must pass all previous
tests" true.** The `fuzz-regress` ctest lane replays every seed here on every
change, forever. A bug found once can never come back silently.

## Adding a case

When the `fuzz-explore` lane (or a local run) fails, it prints the exact
command:

    echo <seed> >> tests/fuzz/corpus/<target>/found.seeds

Then confirm it reproduces, fix the bug, and confirm it passes:

    ./build/fuzz_<target>_test --seed <seed>

Commit the seed **with** the fix, and say in the commit message what it caught.
A seed with no explanation is a mystery to whoever sees it fail in two years.

## Generator versions

A seed's meaning depends on the generator that produced it
(`kGeneratorVersion` in each target). If a generator changes such that old
seeds no longer reach the same code, do **not** silently drop them: bump the
version, keep the old file, and note in a comment which version it was found
under. Losing a regression case is strictly worse than replaying a case that
now exercises something slightly different.
