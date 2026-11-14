#!/usr/bin/env python3
"""Turn a folder of jobs_bench --json files into one Markdown table.

    python3 tests/corebench/summarise.py <dir> [--out summary.md]

Files are grouped by platform (os + arch). Each platform ran the benchmark
several times on one machine; the table shows the median of those runs and
their spread (lowest to highest). Numbers from different platforms are NOT
comparable with each other: CI runners are shared virtual machines with
different core counts. A wide spread means the runner was noisy, not that the
scheduler is. (Until 2026-11-14 there was an enkiTS column and a ratio; enkiTS
was removed, WO-079.)
"""
from __future__ import annotations

import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

ROWS = [  # key, label, lower is better
    ("run.dispatch", "run() from one thread", True),
    ("run.dispatch.mt", "run() from 4 threads", True),
    ("pfor.small", "parallelFor, 64 small items", True),
    ("pfor.scale", "parallelFor, 4M floats", True),
    ("frame.mix", "frame, back to back", True),
    ("frame.paced.median", "frame every 4 ms, median", True),
    ("frame.paced.worst", "frame every 4 ms, worst", True),
    ("frame.paced.cpu", "CPU over the paced run", True),
    ("idle.cpu", "CPU while idle", True),
    ("frame.prewake.on.first_worker", "first job to a worker, with frameBegin", True),
    ("frame.paced.alone", "paced frames with no worker (should be 0)", True),
    ("allocs.per_run", "allocations per run(), 20 000 held", True),
    ("allocs.steady", "allocations per run(), steady state", True),
]


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    folder = Path(sys.argv[1])
    out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else None
    runs: dict[str, dict[str, list[float]]] = defaultdict(lambda: defaultdict(list))
    meta: dict[str, dict] = {}
    units: dict[str, str] = {}
    for f in sorted(folder.rglob("*.json")):
        try:
            d = json.loads(f.read_text())
        except (OSError, json.JSONDecodeError) as e:
            print(f"skipping {f}: {e}", file=sys.stderr)
            continue
        if d.get("bench") != "jobs_bench":
            continue
        plat = f"{d['os']}-{d['arch']}"
        meta[plat] = d
        if d.get("backend", "native") != "native":
            continue   # an old enkiTS file in the folder
        for k, v in d["results"].items():
            runs[plat][k].append(v["value"])
            units[k] = v["unit"]

    fmt = lambda x: f"{x:.3g}" if abs(x) < 1000 else f"{x:,.0f}"
    lines = ["## Job system, per platform", "",
             "Medians of several runs on one runner per platform, with the lowest and",
             "highest. Compare within a platform, never across: runners differ.", ""]
    for plat in sorted(runs):
        m = meta[plat]
        lines += [f"### {plat}: {m['threads']} task threads, {m['compiler'].split(' (')[0]}", "",
                  "| | unit | median | spread | runs |", "|---|---|---|---|---|"]
        for key, label, _ in ROWS:
            n = runs[plat].get(key)
            if not n:
                continue
            lines.append(f"| {label} | {units[key]} | {fmt(statistics.median(n))} | "
                         f"{fmt(min(n))} to {fmt(max(n))} | {len(n)} |")
        lines.append("")
    text = "\n".join(lines)
    print(text)
    if out:
        out.write_text(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
