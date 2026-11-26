#!/usr/bin/env bash
# What the soak is doing: the service, the current run, and the last verdicts.
# Read-only. `farm-soak-status.sh -n 20` lists more past runs.
. "$(dirname "$(readlink -f "$0")")/farm-lib.sh"
n=5; [ "${1:-}" = "-n" ] && n=${2:-5}
echo "service: $(systemctl is-active engine-farm-soak.service 2>/dev/null || echo unknown)"
for t in $FARM_TREES; do echo "build $t: $(readlink "$BIN/$t/current" 2>/dev/null || echo none)"; done
L="$RESULTS/soak/latest"
if [ -d "$L" ]; then
  echo "latest run: $(readlink "$L")"
  if [ -f "$L/verdict.txt" ]; then sed 's/^/  /' "$L/verdict.txt"
  else
    echo "  running: $(( $(wc -l <"$L/metrics.csv") - 1 )) minutes"
    tail -n 1 "$L/metrics.csv" | awk -F, '{printf "  rss %.0f MB, peak %.0f MB, %s threads, %s fds\n",$2/1024,$3/1024,$4,$5}'
    grep "cycle" "$L/soak.log" | tail -n 1 | sed 's/^ */  /'
  fi
fi
echo "past runs:"
for d in $(ls -1dt "$RESULTS"/soak/2*/ 2>/dev/null | head -n "$n"); do
  # `|| true`: farm-lib.sh sets -e and pipefail, and a run in progress has no
  # verdict.txt, so a bare grep here ended the script at the first (newest) run.
  v=$(grep -m1 '^verdict=' "$d/verdict.txt" 2>/dev/null | cut -d= -f2 || true)
  r=$(grep -m1 '^reason=' "$d/verdict.txt" 2>/dev/null | cut -d= -f2- || true)
  if [ -z "$v" ]; then r="$(( $(wc -l <"$d/metrics.csv" 2>/dev/null || echo 1) - 1 )) minutes so far"; fi
  printf '  %-40s %-10s %s\n' "$(basename "$d")" "${v:-running}" "$r"
done
echo "after a 'fail': read soak.log in the run folder, 'coredumpctl list soak_engine' for a core,"
echo "then 'sudo systemctl start engine-farm-soak' to start a fresh run."
