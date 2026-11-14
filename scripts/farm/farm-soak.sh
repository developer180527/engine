#!/usr/bin/env bash
# farm-soak.sh — run soak_engine for days and keep the evidence when it stops.
#
# One soak process at a time, from bin/$FARM_SOAK_TREE/current. Every minute it
# records the process's resident memory, peak, threads and open descriptors to
# metrics.csv beside the soak's own log. It stops, and KEEPS the evidence, on:
#   - soak_engine failing an invariant ("FAIL" in its log: memory slope, the
#     canary job, entity baseline, NaN creep, parallelFor coverage)
#   - a crash or sanitizer report (the core goes to systemd-coredump)
#   - a hang: no output for FARM_SOAK_HANG_MINUTES (it is sent SIGABRT, so the
#     hang leaves a core with every thread's stack)
#   - a leak the process itself cannot see: resident memory growing faster than
#     FARM_SOAK_MAX_RSS_MB_PER_HOUR after the first hour, or descriptors growing
#     by more than FARM_SOAK_MAX_FD_GROWTH
# Exit codes, which the systemd unit relies on:
#   0  finished its minutes, or a newer build arrived (the unit starts the next run)
#   1  a finding: the unit stays stopped until a person looks (farm-soak-status.sh)
#   2  could not start (no build yet, bad config)
. "$(dirname "$(readlink -f "$0")")/farm-lib.sh"

TREE=${FARM_SOAK_TREE:-release}
CUR="$BIN/$TREE/current"
[ -x "$CUR/soak_engine" ] || { log "no $CUR/soak_engine yet (run farm-update.sh)"; exit 2; }
BUILD=$(readlink "$CUR")
RUN="$RESULTS/soak/$(date -u +%Y%m%d-%H%M%S)-$TREE-$BUILD"
mkdir -p "$RUN"
cp "$CUR/machine.txt" "$CUR/commit" "$RUN/" 2>/dev/null || true
"$REPO/scripts/farm/farm-state.sh" >"$RUN/machine-at-start.txt"
ln -sfn "$RUN" "$RESULTS/soak/latest"

finish() {   # verdict, reason
  printf 'verdict=%s\nreason=%s\nbuild=%s\ntree=%s\nended=%s\n' \
    "$1" "$2" "$BUILD" "$TREE" "$(date -u +%FT%TZ)" >"$RUN/verdict.txt"
  log "soak $1: $2 ($RUN)"
}

export SOAK_MINUTES=${FARM_SOAK_MINUTES:-2880}
# Sanitizer builds: keep the first report as the failure, and write it beside the log.
export ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=1:log_path=$RUN/asan"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:log_path=$RUN/ubsan"
export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:log_path=$RUN/tsan"

pin=(); [ -n "${FARM_SOAK_CPUS:-}" ] && pin=(taskset -c "$FARM_SOAK_CPUS")
cd "$RUN"
log "soak start: $TREE build $BUILD, $SOAK_MINUTES minutes -> $RUN"
"${pin[@]}" "$CUR/soak_engine" >"$RUN/soak.log" 2>&1 &
PID=$!
trap 'kill -TERM $PID 2>/dev/null; finish stopped "the service was stopped"; exit 0' TERM INT

echo "minute,rss_kb,hwm_kb,threads,fds,log_bytes" >"$RUN/metrics.csv"
hang_s=$(( ${FARM_SOAK_HANG_MINUTES:-10} * 60 ))
last_size=0; last_change=$(date +%s); minute=0
rss0=""; fd0=""
while kill -0 "$PID" 2>/dev/null; do
  sleep 60; minute=$((minute + 1))
  kill -0 "$PID" 2>/dev/null || break
  st=/proc/$PID/status
  rss=$(awk '/^VmRSS/{print $2}' "$st" 2>/dev/null || echo 0)
  hwm=$(awk '/^VmHWM/{print $2}' "$st" 2>/dev/null || echo 0)
  thr=$(awk '/^Threads/{print $2}' "$st" 2>/dev/null || echo 0)
  fds=$(ls /proc/$PID/fd 2>/dev/null | wc -l)
  size=$(stat -c %s "$RUN/soak.log")
  echo "$minute,$rss,$hwm,$thr,$fds,$size" >>"$RUN/metrics.csv"

  if grep -q "FAIL" "$RUN/soak.log"; then
    kill -ABRT "$PID"; wait "$PID" 2>/dev/null || true
    finish fail "soak_engine reported: $(grep -m1 FAIL "$RUN/soak.log" | sed 's/^ *//')"; exit 1
  fi
  now=$(date +%s)
  if [ "$size" != "$last_size" ]; then last_size=$size; last_change=$now; fi
  if [ $((now - last_change)) -ge "$hang_s" ]; then
    kill -ABRT "$PID"; wait "$PID" 2>/dev/null || true
    finish fail "hang: no output for ${FARM_SOAK_HANG_MINUTES:-10} minutes (SIGABRT sent; see coredumpctl)"; exit 1
  fi
  # Leak gates, measured from the end of the first hour.
  if [ "$minute" -eq 60 ]; then rss0=$rss; fd0=$fds; fi
  if [ -n "$rss0" ] && [ "$minute" -ge 180 ]; then
    hours=$(( (minute - 60) / 60 ))
    growth_mb=$(( (rss - rss0) / 1024 ))
    if [ "$hours" -gt 0 ] && [ $((growth_mb / hours)) -gt "${FARM_SOAK_MAX_RSS_MB_PER_HOUR:-8}" ]; then
      kill -ABRT "$PID"; wait "$PID" 2>/dev/null || true
      finish fail "resident memory grew ${growth_mb} MB in ${hours} h after warm-up"; exit 1
    fi
    if [ $((fds - fd0)) -gt "${FARM_SOAK_MAX_FD_GROWTH:-32}" ]; then
      kill -ABRT "$PID"; wait "$PID" 2>/dev/null || true
      finish fail "open descriptors grew from $fd0 to $fds"; exit 1
    fi
  fi
  # A newer build: end this run cleanly; the unit starts the next one on it.
  if [ "$(readlink "$CUR")" != "$BUILD" ]; then
    kill -TERM "$PID"; wait "$PID" 2>/dev/null || true
    finish superseded "build $(readlink "$CUR") arrived after $minute minutes, no finding"; exit 0
  fi
done

rc=0; wait "$PID" || rc=$?
if [ "$rc" -eq 0 ] && grep -q "SURVIVED" "$RUN/soak.log"; then
  finish pass "ran $SOAK_MINUTES minutes: $(grep -m1 'cycles,' "$RUN/soak.log" | sed 's/^soak_engine: //')"; exit 0
fi
ls "$RUN"/asan* "$RUN"/ubsan* "$RUN"/tsan* >/dev/null 2>&1 && extra=" (sanitizer report in the run folder)" || extra=""
finish fail "soak_engine exited $rc$extra; see soak.log and coredumpctl"; exit 1
