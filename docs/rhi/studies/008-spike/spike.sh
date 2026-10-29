#!/usr/bin/env bash
# Study 008 spike: compile every shader here to SPIR-V, Metal source, metallib and
# WGSL with slangc, check the SPIR-V bindless decorations, then RUN the triangle
# and the compute cull on Metal. Exit 0 only if all of it holds.
#   SLANGC=/path/to/slangc ./spike.sh [out-dir]
set -u
cd "$(dirname "$0")"
SLANGC=${SLANGC:?set SLANGC to slangc (v2026.19 was used)}
OUT=${1:-$(mktemp -d)}
mkdir -p "$OUT"
bad=0
for f in triangle lit skinned cull; do
  for t in spirv-asm metal metallib wgsl; do
    if "$SLANGC" "$f.slang" -target "$t" -o "$OUT/$f.$t" >"$OUT/$f.$t.log" 2>&1; then
      echo "ok    $f -> $t"
    else
      echo "FAIL  $f -> $t"; sed 's/^/        /' "$OUT/$f.$t.log" | head -6; bad=1
    fi
  done
done
# Bindless on Vulkan is only correct with NonUniform where the handle can vary.
n=$(grep -c 'OpDecorate.*NonUniform' "$OUT/lit.spirv-asm" || true)
if [ "$n" -gt 0 ] && grep -q 'OpCapability SampledImageArrayNonUniformIndexing' "$OUT/lit.spirv-asm"; then
  echo "ok    lit SPIR-V carries $n NonUniform decorations and the capability"
else
  echo "FAIL  lit SPIR-V has no NonUniform decoration: divergent material fetches would be wrong"; bad=1
fi
swift run_metal.swift "$OUT/triangle.metallib" vsMain fsMain || bad=1
swift run_cull.swift  "$OUT/cull.metallib" || bad=1
exit $bad
