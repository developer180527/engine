---
status: decided
id: DR-0033
title: Every tool is a CLI over the library
date: 2026-11-06
source:
  - ARCHITECTURE.md
---

## Decided
Every operation the editor offers is a library call first and a command
second: one `engine` command with subcommands, `--json` output, meaningful exit
codes, no prompts, and a headless run that saves screenshots
(`docs/plans/engine-architecture.md` §7a). The editor calls the same library.

## Rejected
Operations that exist only behind editor UI: neither scripts, CI nor agents
can do them.

## Why
People, CI and agents can then do everything a developer does in the editor,
and every editor feature is testable without a window.

## What would reverse it
None expected.
