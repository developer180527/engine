#!/usr/bin/env bash
# Periodic architectural audit for the home server.
#
# WHAT THIS IS FOR
#   The audit (scripts/engine_audit.py) is a pure text analysis — no build, no
#   GPU, a couple of seconds. That makes it the cheapest thing the farm can run,
#   and the only lane that is useful on EVERY commit rather than nightly.
#
# WHAT IT DELIBERATELY DOES NOT DO
#   It never pushes, never commits, never rewrites the working tree of a repo
#   you are using. It clones/fetches into its own checkout and writes a report
#   plus SQLite rows. This is the rule the farm plan already sets for itself:
#   docs/plans/automated-testing-soak-fuzz-plan.md §7.4 — "the server does not
#   push to git", because a repo with no branch protection plus an autonomous
#   pusher is a bad trade.
#
# INSTALL (Debian home server), as a systemd timer:
#   cp scripts/audit_cron.sh /usr/local/bin/engine-audit
#   # /etc/systemd/system/engine-audit.service
#   #   [Service]
#   #   Type=oneshot
#   #   ExecStart=/usr/local/bin/engine-audit
#   #   Environment=ENGINE_AUDIT_DIR=/var/lib/engine-audit
#   # /etc/systemd/system/engine-audit.timer
#   #   [Timer]
#   #   OnCalendar=hourly
#   #   Persistent=true
#   systemctl enable --now engine-audit.timer
#
# Or plain cron:  17 * * * * /usr/local/bin/engine-audit >/dev/null 2>&1
set -uo pipefail

DIR="${ENGINE_AUDIT_DIR:-$HOME/.engine-audit}"
REMOTE="${ENGINE_AUDIT_REMOTE:-}"        # e.g. git@github.com:you/engine.git
BRANCH="${ENGINE_AUDIT_BRANCH:-main}"
REPO="$DIR/repo"
DB="$DIR/farm.db"
OUT="$DIR/reports"
mkdir -p "$DIR" "$OUT"

# ── Get the latest commit, into OUR OWN checkout ────────────────────────────
if [ ! -d "$REPO/.git" ]; then
    [ -n "$REMOTE" ] || { echo "set ENGINE_AUDIT_REMOTE on first run"; exit 2; }
    git clone --filter=blob:none "$REMOTE" "$REPO" || exit 2
fi
git -C "$REPO" fetch --quiet origin "$BRANCH" || exit 2
# A shallow clone cannot answer "when did this code last change", which is how
# engine_doctor decides staleness — it detects that and skips, so the audit
# would silently lose its doc leg. --filter=blob:none above keeps full history.
git -C "$REPO" reset --quiet --hard "origin/$BRANCH" || exit 2

SHA="$(git -C "$REPO" rev-parse --short HEAD)"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
REPORT="$OUT/audit-$STAMP-$SHA.txt"
JSON="$OUT/audit-$STAMP-$SHA.json"

# ── Run it ──────────────────────────────────────────────────────────────────
# --check makes the exit code mean "something NEW appeared", which is what an
# alert should fire on. Pre-existing debt is in the report but never pages you.
python3 "$REPO/scripts/engine_audit.py" --check --with-external \
        --json "$JSON" --sqlite "$DB" >"$REPORT" 2>&1
STATUS=$?

ln -sfn "$REPORT" "$DIR/latest.txt"
ln -sfn "$JSON"   "$DIR/latest.json"

if [ $STATUS -ne 0 ]; then
    # Alert however you already alert. Keep the body short: the report is the
    # detail, and a notifier that pastes 200 lines gets muted within a week.
    SUBJECT="engine audit: new findings at $SHA"
    if command -v mail >/dev/null 2>&1 && [ -n "${ENGINE_AUDIT_MAIL:-}" ]; then
        head -60 "$REPORT" | mail -s "$SUBJECT" "$ENGINE_AUDIT_MAIL"
    fi
    if [ -n "${ENGINE_AUDIT_WEBHOOK:-}" ] && command -v curl >/dev/null 2>&1; then
        curl -fsS -X POST -H 'Content-Type: application/json' \
             -d "{\"text\":\"$SUBJECT — $DIR/latest.txt\"}" \
             "$ENGINE_AUDIT_WEBHOOK" >/dev/null || true
    fi
fi

# Reports are small; keep a quarter's worth and let the DB be the long memory.
find "$OUT" -name 'audit-*' -mtime +90 -delete 2>/dev/null

exit $STATUS
