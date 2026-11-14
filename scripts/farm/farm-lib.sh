# shellcheck shell=bash disable=SC2034
# Sourced by the scripts in scripts/farm/. Not run on its own.
set -euo pipefail
CONF=${ENGINE_FARM_CONF:-/etc/engine-farm.conf}
[ -r "$CONF" ] || { echo "farm: no config at $CONF (copy scripts/farm/farm.conf.example)" >&2; exit 2; }
# shellcheck disable=SC1090
. "$CONF"
: "${FARM_HOME:?}" "${FARM_REPO_URL:?}" "${FARM_BRANCH:=main}"
REPO="$FARM_HOME/repo"
BUILDS="$FARM_HOME/build"      # one persistent build directory per tree (incremental)
BIN="$FARM_HOME/bin"           # bin/<tree>/<sha>/ : kept binaries; bin/<tree>/current -> the newest good one
RESULTS="$FARM_HOME/results"
LOCK="$FARM_HOME/farm.lock"    # held by builds, fuzzing and (later) benchmarks; never by the soak
log() { printf '%s farm: %s\n' "$(date -u +%FT%TZ)" "$*"; }
