#!/usr/bin/env bash
# farm-tune.sh — make the machine quiet enough to measure (farm-and-perf-ci.md §4).
#   sudo farm-tune.sh           apply the runtime settings (lost on reboot unless
#                               the farm-tune unit is enabled, which setup does)
#   farm-tune.sh --print        show what would change, change nothing
# The kernel arguments (isolcpus, nohz_full, rcu_nocbs) cannot be set at runtime;
# this prints the GRUB line to add. Choose the benchmark cores once and record them.
set -euo pipefail
dry=0; [ "${1:-}" = "--print" ] && dry=1
run() { if [ $dry = 1 ]; then echo "would: $*"; else echo "+ $*"; eval "$@"; fi; }
[ $dry = 1 ] || [ "$(id -u)" = 0 ] || { echo "run as root, or with --print" >&2; exit 2; }

# 1. Governor: performance on every core (cpupower if present, else sysfs).
if command -v cpupower >/dev/null; then run "cpupower frequency-set -g performance >/dev/null"
else
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -e "$g" ] && run "echo performance > $g"
  done
fi
# 2. Turbo: OFF, so a hot afternoon does not look like a regression. Recorded either way.
[ -e /sys/devices/system/cpu/intel_pstate/no_turbo ] && run "echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo"
[ -e /sys/devices/system/cpu/cpufreq/boost ] && run "echo 0 > /sys/devices/system/cpu/cpufreq/boost"
# 3. perf without root.
run "sysctl -q kernel.perf_event_paranoid=1"
# 4. Cores kept: systemd-coredump catches them (installed by farm-setup.sh).
run "sysctl -q kernel.core_pattern='|/lib/systemd/systemd-coredump %P %u %g %s %t 9223372036854775808 %h'"
# 5. Swap: off while benchmarks run. Left alone here (the soak and builds may want
#    it); the benchmark lane (WO-083) turns it off around its own runs.

n=$(nproc --all); half=$((n / 2))
cat <<MSG

Kernel arguments (once, then reboot). Half the cores for benchmarks ($half-$((n-1))),
the rest for the OS, builds, fuzzing and the soak:
  edit /etc/default/grub:
    GRUB_CMDLINE_LINUX_DEFAULT="... isolcpus=$half-$((n-1)) nohz_full=$half-$((n-1)) rcu_nocbs=$half-$((n-1))"
  then: update-grub && reboot
  and set FARM_SOAK_CPUS=0-$((half-1)) in /etc/engine-farm.conf.
Current state: scripts/farm/farm-state.sh
MSG
