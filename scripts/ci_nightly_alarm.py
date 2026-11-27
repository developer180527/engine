#!/usr/bin/env python3
"""A red nightly opens an issue; a green one closes it (WO-038).

CI's nightly (ci.yml, schedule) used to go red and stay red for weeks with
nobody told: GitHub's own mail for a failed scheduled run depends on one
person's notification settings. This runs as the nightly's last job and keeps
ONE open issue, labelled `nightly-red`, for as long as the nightly is red:

  red,   no open issue  -> open one: which jobs failed, and the run
  red,   open issue     -> comment on it (one thread, not an issue a night)
  green, open issue     -> comment that it is green again, and close it
  green, no open issue  -> nothing

Opening an issue notifies the repository's watchers, its owner included.
It uses the workflow's own GITHUB_TOKEN (issues: write) through the `gh` CLI
the runners carry; no secret is involved.

  ci_nightly_alarm.py --state red|green --run-url URL --run-id ID [--test]
  ci_nightly_alarm.py --self-test     the decision table, with no network

--test (a workflow_dispatch with test_alarm) titles the issue as a test, so
the path can be seen working once without waiting for a real failure.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys

LABEL = "nightly-red"


def decide(state: str, open_issue: int | None) -> str:
    """What to do: 'open', 'comment', 'close' or 'nothing'."""
    if state == "red":
        return "comment" if open_issue else "open"
    return "close" if open_issue else "nothing"


def gh(*args: str) -> str:
    r = subprocess.run(["gh", *args], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"ci_nightly_alarm: gh {' '.join(args[:2])} failed: {r.stderr.strip()}")
    return r.stdout


def failed_jobs(run_id: str) -> list[str]:
    jobs = json.loads(gh("run", "view", run_id, "--json", "jobs"))["jobs"]
    return [f"- {j['name']}: {j['conclusion']} ({j.get('url', '')})"
            for j in jobs if j.get("conclusion") in ("failure", "cancelled", "timed_out")]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--state", choices=["red", "green"])
    ap.add_argument("--run-url")
    ap.add_argument("--run-id")
    ap.add_argument("--test", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()

    if a.self_test:
        cases = [("red", None, "open"), ("red", 7, "comment"),
                 ("green", 7, "close"), ("green", None, "nothing")]
        bad = [(s, i, w, decide(s, i)) for s, i, w in cases if decide(s, i) != w]
        for s, i, w, got in bad:
            print(f"FAIL  {s}, open issue {i}: expected {w}, got {got}")
        print("ok    the alarm's decision table" if not bad else "")
        return 1 if bad else 0

    if not (a.state and a.run_url and a.run_id):
        ap.error("--state, --run-url and --run-id are required")

    # The label first: an issue cannot be filtered by a label that does not exist.
    gh("label", "create", LABEL, "--color", "d73a4a", "--force",
       "--description", "The nightly CI run is red (scripts/ci_nightly_alarm.py)")
    found = json.loads(gh("issue", "list", "--label", LABEL, "--state", "open",
                          "--json", "number", "--limit", "1"))
    issue = found[0]["number"] if found else None
    action = decide(a.state, issue)
    test = " (TEST of the alarm: close this by hand)" if a.test else ""
    print(f"ci_nightly_alarm: nightly is {a.state}, open issue {issue}: {action}")

    if action == "open":
        body = (f"The nightly CI run is red{test}.\n\nRun: {a.run_url}\n\nFailed jobs:\n"
                + ("\n".join(failed_jobs(a.run_id)) or "- (none listed: see the run)")
                + "\n\nThis issue stays open, and each red nightly comments here, until a nightly "
                  "is green; then it closes itself (WO-038).")
        gh("issue", "create", "--title", f"Nightly CI is red{test}", "--label", LABEL, "--body", body)
    elif action == "comment":
        body = (f"Still red{test}: {a.run_url}\n\n"
                + ("\n".join(failed_jobs(a.run_id)) or "- (none listed: see the run)"))
        gh("issue", "comment", str(issue), "--body", body)
    elif action == "close":
        gh("issue", "comment", str(issue), "--body", f"Green again: {a.run_url}")
        gh("issue", "close", str(issue))
    return 0


if __name__ == "__main__":
    sys.exit(main())
