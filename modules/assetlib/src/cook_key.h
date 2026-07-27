#pragma once
#include "assetlib/asset_registry.h"
#include "assetlib/cooker.h"
#include <filesystem>
#include <string>

// Internal: cook IDENTITY and STALENESS — the "is this asset's cooked output
// already correct?" policy, isolated from orchestration so it can be reasoned
// about (and tested) on its own. Not a public header.
namespace assetlib {

// Lowercased extension of a project-relative source path (cooker lookup key).
std::string lowerExtOf(const std::string& sourcePath);

// The record's BLAKE3 source hash: rec.sourceHash when it's a valid 64-hex
// BLAKE3 (scan() keeps it fresh and upgrades legacy FNV), else hashed from
// disk. "" when the source is unreadable.
std::string cookSourceHash(const AssetRecord& rec,
                           const std::filesystem::path& projectRoot);

// The DDC key for this record's CURRENT inputs: source hash ⊕ cooker id ⊕
// cooker version ⊕ settings fingerprint ⊕ per-asset import settings.
// "" when the source is unreadable (no identity → nothing to cache).
std::string computeCookKey(const AssetRecord& rec, ICooker& cooker,
                           const std::filesystem::path& projectRoot);

// Content-addressed staleness, given the record and its current key:
//   • key differs from the last attempt's → stale (inputs changed)
//   • same key + Failed → NOT stale (these exact inputs already failed;
//     retry only when something changes, or via forceRecook)
//   • same key + empty cookedPath → NOT stale (deliberately skipped)
//   • same key + materialized output missing → stale (a .cache wipe; the
//     DDC restores it without recooking)
// No mtime comparison, no global cook version.
bool cookIsStale(const AssetRecord& rec, const std::string& currentKey,
                 const std::filesystem::path& cacheRoot);

} // namespace assetlib
