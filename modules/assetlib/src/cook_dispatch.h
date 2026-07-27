#pragma once
#include "assetlib/cooker.h"
#include <filesystem>

// Internal: HOW a cook is executed — isolated child process or in-process.
// Orthogonal to WHAT gets cooked and WHERE the output is cached, which is
// the pipeline's job. Not a public header.
namespace assetlib {

// Cook in an isolated `engine_cook_worker` child process. A corrupt asset
// that SIGSEGVs Assimp kills the child, not us; the child also self-imposes
// a hard setrlimit memory cap. Crash, timeout, and a missing/garbled result
// all come back as an ordinary per-asset failure.
// POSIX only — returns a failure result on Windows.
CookResult cookInWorkerProcess(const std::filesystem::path& workerExe,
                               ICooker& cooker, const CookContext& ctx);

// Cook on this thread behind an exception net. Exceptions from third-party
// parsers become per-asset failures instead of std::terminate; SIGNALS still
// kill the host — which is precisely what the worker process is for.
CookResult cookInProcess(ICooker& cooker, const CookContext& ctx);

// THE dispatch seam: worker process when `workerExe` is set (and supported),
// in-process otherwise. Every cook in the pipeline goes through here.
CookResult dispatchCook(const std::filesystem::path& workerExe,
                        ICooker& cooker, const CookContext& ctx);

} // namespace assetlib
