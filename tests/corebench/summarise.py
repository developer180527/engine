#!/usr/bin/env python3
"""Turn a folder of jobs_bench --json files into one Markdown table.

    python3 tests/corebench/summarise.py <dir> [--out summary.md]

Files are grouped by platform (os + arch) and backend. Each platform ran every
backend several times, interleaved, on one machine; the table shows the median
of those runs and, per platform, native's time as a fraction of enkiTS's.
Numbers from different platforms are NOT comparable with each other: CI runners
are shared virtual machines with different core counts. The ratio column is the
one that means something.
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
    ("allocs.per_run", "allocations per run()", True),
]


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    folder = Path(sys.argv[1])
    out = Path(sys.argv[sys.argv.index("--out") + 1]) if "--out" in sys.argv else None
    runs: dict[tuple[str, str], dict[str, list[float]]] = defaultdict(lambda: defaultdict(list))
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
        for k, v in d["results"].items():
            runs[(plat, d["backend"])][k].append(v["value"])
            units[k] = v["unit"]

    plats = sorted({p for p, _ in runs})
    lines = ["## Job system: native vs enkiTS, per platform", "",
             "Medians of interleaved runs on one runner per platform. Compare within a",
             "column, never across: runners differ. **ratio** = native / enkiTS (below 1 is",
             "native faster or cheaper).", ""]
    for plat in plats:
        m = meta[plat]
        lines += [f"### {plat}: {m['threads']} task threads, {m['compiler'].split(' (')[0]}", "",
                  "| | unit | enkiTS | native | ratio |", "|---|---|---|---|---|"]
        for key, label, _ in ROWS:
            e = runs.get((plat, "enkits"), {}).get(key)
            n = runs.get((plat, "native"), {}).get(key)
            if not n:
                continue
            nm = statistics.median(n)
            em = statistics.median(e) if e else None
            ratio = f"{nm / em:.2f}" if em else "–"
            fmt = lambda x: "–" if x is None else (f"{x:.3g}" if abs(x) < 1000 else f"{x:,.0f}")
            lines.append(f"| {label} | {units[key]} | {fmt(em)} | {fmt(nm)} | {ratio} |")
        lines.append("")
    text = "\n".join(lines)
    print(text)
    if out:
        out.write_text(text + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
