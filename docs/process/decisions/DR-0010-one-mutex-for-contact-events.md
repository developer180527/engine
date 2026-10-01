---
status: decided
id: DR-0010
title: Jolt contact events are collected under one mutex
date: 2026-10-01
source:
  - src/plugins/jolt_plugin.h
---

## Decided
`ContactListenerImpl` appends contact starts and ends to one vector under one
`std::mutex`; the flush sorts them for determinism.

## Rejected
Per-thread contact buffers merged at the flush (WO-049's original plan).

## Why
The lock is only taken when a contact starts or ends, not while it persists.
It was blamed for ~580 samples of mutex wait in a profile taken while BUG-0071
kept piles from settling (3 100 starts and ends per tick at scale 2). Settled,
it is 27 per tick, and a sampling profile shows 0 samples waiting on it. A
merge step would buy nothing measurable.

## What would reverse it
A profile showing this lock again: a game whose every tick is a burst of new
contacts (debris, destruction). Then per-thread buffers, with the same sort.
