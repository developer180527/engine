#pragma once
// ── cook_requests — a missing cooked asset is a cook JOB (WO-018) ─────────────
//
// The runtime loads cooked content only. When something asks for an asset by
// its SOURCE path and no cooked version is Ready, there are exactly two honest
// answers, and which one depends on the build, not the caller:
//
//   real  (the editor)       ask the cooker for it, show a placeholder, and
//                            swap the asset in when the cook lands: Pending,
//                            then Ready or Failed.
//   null  (player, server,   there is no cooker here, so the asset is Failed
//          a dev runner)     at once, "not cooked: <path>", logged once, and
//                            the placeholder stays.
//
// The runtime used to have a third answer: parse the source itself, with a
// different parser from the cook's. So one asset could look different
// depending on whether it had been cooked yet: a pipeline bug that looks like
// a rendering bug. That path is gone; this interface is what replaced it.
//
// It lives in src/assets, below both the cook stack (CookService implements
// it) and the runtime (AsyncLoader calls it), so neither depends on the other.
// Contract: docs/contracts/cook-request.md.
#include <string>

class ICookRequests {
public:
    virtual ~ICookRequests() = default;

    // Whether this build can cook at all. False is the null provider: a
    // request is pointless, and the caller reports Failed without waiting.
    virtual bool canCook() const = 0;

    // Ask for `sourcePath` (absolute, or relative to the project root) to be
    // cooked soon, ahead of other work. Non-blocking and idempotent; any
    // thread. Completion is observed through the asset registry: the record
    // becomes Ready (or Failed), which is where the cook pipeline has always
    // published its results.
    virtual void requestCook(const std::string& sourcePath) = 0;
};

// The null provider: a build with no cooker (player, server, engine_host).
class NullCookRequests final : public ICookRequests {
public:
    bool canCook() const override { return false; }
    void requestCook(const std::string&) override {}
};
