#!/usr/bin/env bash
# farm-setup.sh — one-time setup of the home server (Debian), run as root:
#   curl -fsSLO https://raw.githubusercontent.com/<owner>/engine/main/scripts/farm/farm-setup.sh
#   (read it), then: sudo bash farm-setup.sh
# Or from a checkout: sudo scripts/farm/farm-setup.sh
#
# Installs build packages, creates the `farm` user and /srv/engine-farm, clones
# the repository ANONYMOUSLY (no credentials, ever: farm-update.sh refuses
# anything but an https origin), installs the systemd units, and runs the first
# build. Idempotent: running it again updates the units and changes nothing else.
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 2; }
CONF=/etc/engine-farm.conf
HERE=$(dirname "$(readlink -f "$0")")

if [ ! -f "$CONF" ]; then
  cp "$HERE/farm.conf.example" "$CONF"
  echo "wrote $CONF — check FARM_REPO_URL and FARM_JOBS, then run this again"; exit 0
fi
. "$CONF"

apt-get update -q
apt-get install -y -q --no-install-recommends \
  git ca-certificates cmake ninja-build build-essential clang pkg-config python3 \
  xorg-dev libx11-dev libgl1-mesa-dev libwayland-dev libxkbcommon-dev wayland-protocols \
  libasound2-dev libpulse-dev libsqlite3-dev \
  systemd-coredump linux-cpupower util-linux

id "$FARM_USER" >/dev/null 2>&1 || useradd --system --create-home --home-dir "$FARM_HOME" --shell /usr/sbin/nologin "$FARM_USER"
install -d -o "$FARM_USER" -g "$FARM_USER" "$FARM_HOME" "$FARM_HOME/results"
if [ ! -d "$FARM_HOME/repo/.git" ]; then
  # A full checkout: the systemd units run scripts/farm/ from it.
  sudo -u "$FARM_USER" git clone --quiet --branch "$FARM_BRANCH" "$FARM_REPO_URL" "$FARM_HOME/repo"
fi

# The units run the scripts from the farm's own checkout, so they update with main.
for u in "$HERE"/systemd/*.service "$HERE"/systemd/*.timer; do
  sed -e "s|@FARM_HOME@|$FARM_HOME|g" -e "s|@FARM_USER@|$FARM_USER|g" "$u" >"/etc/systemd/system/$(basename "$u")"
done
systemctl daemon-reload
systemctl enable --now engine-farm-tune.service
systemctl enable --now engine-farm-update.timer
systemctl enable engine-farm-soak.service

echo "first build (about 20-40 min; log in $FARM_HOME/results/builds/):"
systemctl start engine-farm-update.service || true
systemctl start engine-farm-soak.service
"$HERE/farm-soak-status.sh" || true
echo
echo "next: scripts/farm/farm-tune.sh --print for the kernel arguments (isolated cores)."
