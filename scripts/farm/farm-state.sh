#!/usr/bin/env bash
# Prints the machine's state as key=value lines. Every result directory gets a
# copy (machine.txt), so a changed governor or kernel shows as a different
# machine instead of a mystery step in a graph. Read-only; needs no root.
set -uo pipefail
r() { cat "$1" 2>/dev/null || echo "n/a"; }
echo "date=$(date -u +%FT%TZ)"
echo "host=$(hostname)"
echo "kernel=$(uname -r)"
echo "cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "cpus=$(nproc --all) online=$(nproc)"
echo "governor=$(r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
if [ -e /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
  echo "turbo=$([ "$(r /sys/devices/system/cpu/intel_pstate/no_turbo)" = 1 ] && echo off || echo on) (intel_pstate)"
else
  echo "turbo=$([ "$(r /sys/devices/system/cpu/cpufreq/boost)" = 0 ] && echo off || echo on) (cpufreq/boost)"
fi
echo "isolated=$(r /sys/devices/system/cpu/isolated)"
echo "nohz_full=$(r /sys/devices/system/cpu/nohz_full)"
echo "swap=$(awk 'NR>1{s+=$3} END{print (s?s" kB":"off")}' /proc/swaps)"
echo "perf_event_paranoid=$(r /proc/sys/kernel/perf_event_paranoid)"
echo "core_pattern=$(r /proc/sys/kernel/core_pattern)"
echo "compiler=$( (c++ --version 2>/dev/null || echo n/a) | head -1)"
echo "cmake=$( (cmake --version 2>/dev/null || echo n/a) | head -1)"
echo "load=$(cut -d' ' -f1-3 /proc/loadavg)"
