#!/usr/bin/env bash
# Apply every patch in third_party/patches/ to its target submodule.
#
# ONE implementation, used by CI (all jobs) and by developers. It previously
# lived inline in the workflow — twice — and the two copies drifted the moment
# the naming convention gained nested-submodule support, breaking a job that
# looked identical to a working one.
#
# Filename convention: <submodule-path>__<description>.patch, where '+' in the
# path segment means '/' so nested submodules can be addressed:
#   bgfx.cmake__foo.patch        -> third_party/bgfx.cmake
#   bgfx.cmake+bgfx__foo.patch   -> third_party/bgfx.cmake/bgfx
#
# Patches must be applied from INSIDE the submodule: their paths are relative to
# that repo's root and --3way needs its object store.
#
# Idempotent: an already-applied patch is skipped rather than failing, so this
# is safe to re-run on a dirty working tree.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

shopt -s nullglob
patches=(third_party/patches/*.patch)
if [ ${#patches[@]} -eq 0 ]; then
    echo "no patches to apply"
    exit 0
fi

for p in "${patches[@]}"; do
    name="${p##*/}"
    sub="${name%%__*}"
    sub="${sub//+//}"                       # '+' encodes a path separator
    target="third_party/$sub"

    if [ ! -d "$target" ]; then
        echo "ERROR: $name targets $target, which does not exist" >&2
        exit 1
    fi

    # Already applied? Reverse-check succeeds only if the change is present.
    if git -C "$target" apply --check --reverse "$root/$p" 2>/dev/null; then
        echo "already applied: $name -> $target"
        continue
    fi

    echo "applying $name -> $target"
    git -C "$target" apply --3way "$root/$p"
done
