# scripts/farm — the home server

The headless Debian box that builds `main` by itself and runs the engine for
days (docs/plans/farm-and-perf-ci.md; WO-081, WO-084). It **pulls** the public
repository anonymously and **never pushes**: no GitHub credentials live on it,
and nothing on GitHub can reach it.

## Install (once)

```
git clone https://github.com/developer180527/engine.git /tmp/engine
sudo /tmp/engine/scripts/farm/farm-setup.sh      # writes /etc/engine-farm.conf, stops
sudoedit /etc/engine-farm.conf                   # FARM_JOBS, soak length, cores
sudo /tmp/engine/scripts/farm/farm-setup.sh      # packages, user, units, first build, soak
```

The second run builds three trees (about 20 to 40 minutes the first time) and
starts the soak. After that `/tmp/engine` can go; the box uses its own checkout
in `/srv/engine-farm/repo`.

Then make it quiet: `scripts/farm/farm-tune.sh --print` shows the kernel
arguments for isolated benchmark cores (a GRUB edit and a reboot).

## Every day

| | |
|---|---|
| `farm-soak-status.sh` | the soak: running or not, minutes, memory, last verdicts |
| `journalctl -u engine-farm-soak -u engine-farm-update` | what the services said |
| `coredumpctl list soak_engine` | cores from crashes and hangs |
| `farm-state.sh` | governor, turbo, isolated cores, compiler: the machine as measured |

A soak run ends three ways, written to `verdict.txt` in its folder under
`/srv/engine-farm/results/soak/`:
- **pass** — it ran its minutes. The next run starts.
- **superseded** — a newer build arrived. The next run starts on it.
- **fail** — a finding: an invariant `soak_engine` checks, a crash or sanitizer
  report, a hang (no output for 10 minutes; it is sent SIGABRT so the core has
  every thread), or resident memory or open descriptors growing. The service
  **stays stopped** so the evidence is not overwritten. Read `soak.log` and
  `metrics.csv`, then `sudo systemctl start engine-farm-soak` for a fresh run.

## What is where

```
/etc/engine-farm.conf                 settings (farm.conf.example)
/srv/engine-farm/repo                 the checkout (detached at the last built commit)
/srv/engine-farm/build/<tree>         incremental build directories: release, asan, tsan
/srv/engine-farm/bin/<tree>/<sha>/    the binaries the farm runs, the last 5 builds
/srv/engine-farm/bin/<tree>/current   the newest GOOD build (a failed build never moves it)
/srv/engine-farm/results/builds/      build logs
/srv/engine-farm/results/soak/<run>/  soak.log, metrics.csv, verdict.txt, machine*.txt
/srv/engine-farm/farm.lock            builds and fuzzing take it; benchmarks will; the soak never
```

## Not here yet

Benchmarks against a baseline and the noise floor (WO-081, WO-083), the results
database and report (WO-082), fuzzing through the night and the C-API soak
program with a churning scene (WO-084). Results reach the Mac by `rsync` of
`results/` until the database exists.
