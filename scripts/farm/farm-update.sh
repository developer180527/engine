#!/usr/bin/env bash
# farm-update.sh — fetch main, build what is new, keep the last good build.
#
# NO CREDENTIALS. The repository is public: this fetches anonymously over HTTPS
# and never pushes. Nothing on this box can write to GitHub, and nothing on
# GitHub can reach this box (docs/plans/farm-and-perf-ci.md §3). Do not add a
# token, a deploy key or a self-hosted runner here.
#
# What it does, under the farm lock (so it never overlaps a benchmark or a fuzz
# run; the soak does not take the lock and keeps running):
#   1. git fetch origin main; stop if that commit is already built for every tree
#   2. check it out (detached), submodules, third-party patches
#   3. build each tree in $FARM_HOME/build/<tree> (incremental)
#   4. on success, copy the binaries the farm runs to bin/<tree>/<sha>/ and point
#      bin/<tree>/current at it; on failure, current stays on the last good build
#   5. keep the newest 5 builds per tree, and the one a soak is pinned to
# The soak notices a new bin/release/current within a minute and restarts on it.
#
#   farm-update.sh            normal run (the systemd timer)
#   farm-update.sh --force    rebuild even if the commit is already built
. "$(dirname "$(readlink -f "$0")")/farm-lib.sh"

FORCE=0; [ "${1:-}" = "--force" ] && FORCE=1
mkdir -p "$BUILDS" "$BIN" "$RESULTS/builds"
exec 9>"$LOCK"
flock -w 7200 9 || { log "lock busy for 2 h, giving up"; exit 1; }

# The binaries each tree must produce, copied out of the build directory.
RUN_BINARIES="soak_engine jobs_fuzz_test jobs_test stress_jobs stress_churn stress_physics stress_swarm"

if [ ! -d "$REPO/.git" ]; then
  log "first run: cloning $FARM_REPO_URL"
  git clone --quiet --branch "$FARM_BRANCH" "$FARM_REPO_URL" "$REPO"
fi
cd "$REPO" || exit 2
case "$(git remote get-url origin)" in
  https://*) ;;
  *) log "origin is not an https URL; refusing (the farm fetches anonymously)"; exit 2 ;;
esac
git fetch --quiet origin "$FARM_BRANCH"
SHA=$(git rev-parse FETCH_HEAD)
SHORT=${SHA:0:10}

# Disk: three trees of the whole engine with debug info filled a 69 GB root
# (2026-11-14: the TSan build died in the assembler and ar with the disk full).
# Builds now make only RUN_BINARIES, and refuse to start below FARM_MIN_FREE_GB.
free_gb=$(df -Pk "$FARM_HOME" | awk 'NR==2{print int($4/1048576)}')
if [ "$free_gb" -lt "${FARM_MIN_FREE_GB:-20}" ]; then
  log "only ${free_gb} GB free on $FARM_HOME (need ${FARM_MIN_FREE_GB:-20}); not building. Free space or move FARM_HOME."
  exit 1
fi

todo=""
for t in $FARM_TREES; do
  if [ "$FORCE" = 1 ] || [ ! -e "$BIN/$t/$SHORT/.ok" ]; then todo="$todo $t"; fi
done
if [ -z "$todo" ]; then log "$SHORT already built for: $FARM_TREES"; exit 0; fi

log "building $SHORT ($(git log -1 --format=%s "$SHA" | cut -c1-70)) for:$todo"
git -c advice.detachedHead=false checkout --quiet --force "$SHA"
git submodule sync --quiet --recursive
git submodule update --init --recursive --depth 1 --jobs 4 --quiet
git clean -ffdq -e /build   # leftovers from a previous commit (submodules included)
./scripts/apply_third_party_patches.sh >/dev/null

flags_for() {
  case "$1" in
    release) echo "-DCMAKE_BUILD_TYPE=Release" ;;
    asan)    echo "-DCMAKE_BUILD_TYPE=RelWithDebInfo -DENGINE_SANITIZE=address,undefined" ;;
    tsan)    echo "-DCMAKE_BUILD_TYPE=RelWithDebInfo -DENGINE_SANITIZE=thread" ;;
    *)       return 1 ;;
  esac
}

status=0
for t in $todo; do
  f=$(flags_for "$t") || { log "unknown tree '$t'"; status=1; continue; }
  b="$BUILDS/$t"; logf="$RESULTS/builds/$SHORT-$t.log"
  log "[$t] configure + build -> $logf"
  # Editor, samples and Rust off: the farm runs headless tests only.
  # shellcheck disable=SC2086
  if cmake -S "$REPO" -B "$b" -G Ninja $f \
        -DENGINE_BUILD_EDITOR=OFF -DENGINE_BUILD_SAMPLES=OFF -DENGINE_WITH_RUST=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >"$logf" 2>&1 \
     && cmake --build "$b" --parallel "$FARM_JOBS" --target $RUN_BINARIES >>"$logf" 2>&1; then
    out="$BIN/$t/$SHORT"; mkdir -p "$out"
    for x in $RUN_BINARIES; do [ -x "$b/$x" ] && cp "$b/$x" "$out/"; done
    "$REPO/scripts/farm/farm-state.sh" >"$out/machine.txt"
    echo "$SHA" >"$out/commit"
    touch "$out/.ok"
    ln -sfn "$SHORT" "$BIN/$t/current.new" && mv -T "$BIN/$t/current.new" "$BIN/$t/current"
    log "[$t] ok"
    # Keep the newest 5 builds of this tree. Real directories only: a glob like
    # */ also matches the `current` symlink, and rm -rf current/ would follow it.
    # Never the build a soak is running on, or kept after a finding
    # (bin/<tree>/.soak-pin, farm-soak.sh): a run lasts days, past 5 builds.
    pinned=$(cat "$BIN/$t/.soak-pin" 2>/dev/null || true)
    find "$BIN/$t" -mindepth 1 -maxdepth 1 -type d -printf '%T@ %p\n' \
      | sort -rn | tail -n +6 | cut -d' ' -f2- \
      | { if [ -n "$pinned" ]; then grep -vxF "$BIN/$t/$pinned" || true; else cat; fi; } \
      | xargs -r rm -rf --
  else
    log "[$t] FAILED (bin/$t/current stays on the last good build); tail of the log:"
    tail -n 30 "$logf" | sed 's/^/    /'
    status=1
  fi
done
exit $status
