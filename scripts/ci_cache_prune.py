#!/usr/bin/env python3
"""Keep one compiler cache per CI leg: delete the older copies (WO-038).

GitHub gives the repository 10 GB of Actions cache. Every run saves each leg's
compiler cache under a new key (ccache-action appends a timestamp), about 8 GB
a run, and GitHub evicts the least recently used entry when it is full. So the
older copies of the legs that run every push crowded out the one leg that runs
only on the full matrix, Windows arm64, and it compiled everything cold, twice
(its dev tree and the shipping tree): 23.5 minutes of build against Windows
x64's 6, on 2026-11-27.

This runs as the last job of a run on main, after every leg has saved its new
cache, and keeps only the NEWEST entry for each key (the key without its
timestamp) on each ref. That leaves one copy per leg, about 7 GB in all, so
every leg's cache survives until its next run.

  ci_cache_prune.py            list with gh, delete the older copies
  ci_cache_prune.py --dry-run  say what would be deleted
  ci_cache_prune.py --self-test  the grouping, with no network
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys

# ccache-action's key: "<variant>-<key>-<ISO timestamp>"; the timestamp is what
# makes each save a new entry.
STAMP = re.compile(r"-\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z$")


def stale(caches: list[dict]) -> list[dict]:
    """Every entry but the newest of its (ref, key without timestamp) group.
    Entries with no timestamp in their key are not ours: left alone."""
    groups: dict[tuple[str, str], list[dict]] = {}
    for c in caches:
        if not STAMP.search(c["key"]):
            continue
        groups.setdefault((c.get("ref", ""), STAMP.sub("", c["key"])), []).append(c)
    out = []
    for entries in groups.values():
        entries.sort(key=lambda c: c["createdAt"], reverse=True)
        out += entries[1:]
    return out


def self_test() -> int:
    t = lambda h: f"2026-11-27T0{h}:00:00.000Z"
    caches = [
        {"id": 1, "key": f"sccache-windows-x64-msvc-{t(2)}", "createdAt": t(2), "ref": "refs/heads/main"},
        {"id": 2, "key": f"sccache-windows-x64-msvc-{t(4)}", "createdAt": t(4), "ref": "refs/heads/main"},
        {"id": 3, "key": f"sccache-windows-arm64-msvc-{t(1)}", "createdAt": t(1), "ref": "refs/heads/main"},
        {"id": 4, "key": f"ccache-linux-x64-gcc-{t(3)}", "createdAt": t(3), "ref": "refs/pull/9/merge"},
        {"id": 5, "key": f"ccache-linux-x64-gcc-{t(4)}", "createdAt": t(4), "ref": "refs/heads/main"},
        {"id": 6, "key": "someone-elses-cache", "createdAt": t(1), "ref": "refs/heads/main"},
    ]
    got = sorted(c["id"] for c in stale(caches))
    ok = got == [1]
    print("ok    only the older copy of a leg's cache on the same ref is deleted" if ok
          else f"FAIL  expected [1] to be deleted, got {got}")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()

    r = subprocess.run(["gh", "cache", "list", "--limit", "200",
                        "--json", "id,key,createdAt,ref,sizeInBytes"], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"ci_cache_prune: gh cache list failed: {r.stderr.strip()}", file=sys.stderr)
        return 1
    old = stale(json.loads(r.stdout))
    freed = sum(c.get("sizeInBytes", 0) for c in old) / 2**30
    for c in old:
        print(f"{'would delete' if a.dry_run else 'delete'} {c['key']} ({c.get('ref', '')})")
        if not a.dry_run:
            d = subprocess.run(["gh", "cache", "delete", str(c["id"])], capture_output=True, text=True)
            if d.returncode != 0:   # gone already (another run pruned it): not a failure
                print(f"  not deleted: {d.stderr.strip()}")
    print(f"ci_cache_prune: {len(old)} older copies, {freed:.1f} GB "
          f"{'would be ' if a.dry_run else ''}freed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
