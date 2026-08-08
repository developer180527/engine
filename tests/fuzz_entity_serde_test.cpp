// ── fuzz_entity_serde_test — the JSON scene deserializer ─────────────────────
// `EntitySerde::createEntity` is the OTHER scene deserializer. Its sibling —
// the cooked-binary path in assetlib — is covered by fuzz_scene_loader_test;
// this one covers the authored `.scene` JSON that the editor writes and both
// the editor and `engine_host` read back.
//
// It is the less obviously dangerous of the two and the more exposed in
// practice: a `.scene` is a text file that people hand-edit, merge, and resolve
// conflicts in. It does not have to be malicious to be malformed.
//
// The specific hazard is nlohmann's typed accessors. `j.value("fov", 60.0f)`
// does NOT fall back to the default when the key exists with the WRONG type —
// it throws `nlohmann::json::type_error`. And in `scene_serializer.h` the
// `try/catch` wraps only `json::parse`; the `createEntity` loop sits outside
// it, so one `"fov": "60"` in a hand-edited scene propagates an exception out
// of scene load entirely.
//
// Properties asserted per case:
//   1. createEntity NEVER throws, whatever the JSON says. This is the whole
//      point of the target.
//   2. It never crashes or hangs — deep nesting must not blow the stack, and a
//      huge declared count must not become a huge allocation.
//   3. The entity it produces is well-formed: alive, and any component it set
//      holds finite values. A NaN scale silently poisons every transform that
//      inherits from it, and the frame it corrupts is far from the load.
#include "fuzz/fuzz.h"

#include "scene/entity_serializer.h"

#include <cmath>
#include <string>
#include <vector>

static constexpr uint32_t kGeneratorVersion = 1;

namespace {

using json = nlohmann::json;

// A value of the RIGHT shape most of the time, and something hostile the rest.
// Wrong-typed values are the point: random noise mostly produces missing keys,
// which every accessor already handles.
json fuzzValue(fuzz::Rng& rng, int depth = 0) {
    if (depth > 3) return json(rng.next());
    switch (rng.below(12)) {
        case 0:  return json(nullptr);
        case 1:  return json(rng.chance(50));
        case 2:  return json((int64_t)rng.interestingU32());
        case 3:  return json((double)rng.interestingU32());
        // The float values that survive a parse and poison everything
        // downstream. A JSON literal cannot hold these, but a double built
        // in-process can, and the editor's writer goes through the same type.
        case 4:  return json(std::nan(""));
        case 5:  return json(std::numeric_limits<double>::infinity());
        case 6:  return json(-std::numeric_limits<double>::infinity());
        case 7: {                       // string, sometimes very long
            const uint32_t n = rng.range(0, rng.chance(10) ? 4000 : 24);
            std::string s;
            for (uint32_t i = 0; i < n; ++i) s.push_back((char)rng.range(1, 126));
            return json(s);
        }
        case 8: {                       // array of the wrong length
            json a = json::array();
            const uint32_t n = rng.range(0, 6);
            for (uint32_t i = 0; i < n; ++i) a.push_back(fuzzValue(rng, depth + 1));
            return a;
        }
        case 9: {                       // nested object where a scalar belongs
            json o = json::object();
            const uint32_t n = rng.range(0, 3);
            for (uint32_t i = 0; i < n; ++i)
                o["k" + std::to_string(i)] = fuzzValue(rng, depth + 1);
            return o;
        }
        case 10: {                      // deep nesting — stack safety
            json d = json(1);
            const uint32_t n = rng.range(1, 64);
            for (uint32_t i = 0; i < n; ++i) { json w = json::array(); w.push_back(d); d = w; }
            return d;
        }
        default: return json((float)((int32_t)rng.next() % 1000) * 0.5f);
    }
}

// A float triple that is USUALLY a valid one, so the case reaches component
// code instead of dying at the first accessor.
json vec(fuzz::Rng& rng, uint32_t n, float dflt) {
    if (rng.chance(20)) return fuzzValue(rng);
    json a = json::array();
    const uint32_t count = rng.chance(85) ? n : rng.range(0, 8);
    for (uint32_t i = 0; i < count; ++i) {
        if (rng.chance(10)) a.push_back(fuzzValue(rng));
        else                a.push_back(dflt + (float)((int32_t)rng.next() % 100) * 0.1f);
    }
    return a;
}

json buildEntity(fuzz::Rng& rng) {
    json je = json::object();
    if (rng.chance(90)) je["id"]       = rng.next();
    if (rng.chance(50)) je["parentId"] = rng.next();
    // Wrong-typed identity: `id` is read as uint64.
    if (rng.chance(10)) je["id"]       = "not-a-number";

    auto maybe = [&](const char* key, json sub) {
        if (rng.chance(60)) je[key] = std::move(sub);
    };
    auto str = [&](uint32_t maxLen) -> json {
        if (rng.chance(15)) return fuzzValue(rng);
        const uint32_t n = rng.range(0, maxLen);
        std::string s;
        for (uint32_t i = 0; i < n; ++i) s.push_back((char)rng.range(32, 126));
        return json(s);
    };

    {   json t = json::object();
        t["position"] = vec(rng, 3, 0.0f);
        t["rotation"] = vec(rng, 4, 0.0f);
        t["scale"]    = vec(rng, 3, 1.0f);
        maybe("transform", std::move(t)); }
    {   json n = json::object(); n["value"] = str(64); maybe("name", std::move(n)); }
    {   json m = json::object();
        m["cookedPath"] = str(rng.chance(10) ? 2000 : 60);
        m["path"]       = str(60);
        m["uuid"]       = str(40);
        m["sourceType"] = rng.chance(50) ? json("primitive") : str(16);
        m["material"]   = str(48);
        maybe("meshRenderer", std::move(m)); }
    {   json l = json::object();
        l["levels"] = [&]{ json a = json::array();
            const uint32_t n = rng.range(0, 6);
            for (uint32_t i = 0; i < n; ++i) {
                json lv = json::object();
                lv["cookedPath"] = str(48);
                a.push_back(rng.chance(85) ? lv : fuzzValue(rng));
            }
            return a; }();
        l["coarsenBelow"] = rng.chance(80) ? json((float)rng.range(0, 400)) : fuzzValue(rng);
        maybe("lodMesh", std::move(l)); }
    {   json c = json::object();
        c["fov"]        = rng.chance(75) ? json(60.0f) : fuzzValue(rng);
        c["nearPlane"]  = rng.chance(75) ? json(0.1f)  : fuzzValue(rng);
        c["farPlane"]   = rng.chance(75) ? json(1000.0f) : fuzzValue(rng);
        c["orthoSize"]  = fuzzValue(rng);
        c["projection"] = rng.chance(50) ? json(rng.below(4)) : fuzzValue(rng);
        c["clearColor"] = vec(rng, 4, 0.1f);
        c["isPrimary"]  = rng.chance(70) ? json(true) : fuzzValue(rng);
        maybe("camera", std::move(c)); }
    {   json r = json::object();
        r["bodyType"]    = rng.chance(60) ? json(rng.below(4)) : fuzzValue(rng);
        r["shape"]       = rng.chance(60) ? json(rng.below(4)) : fuzzValue(rng);
        r["mass"]        = fuzzValue(rng);
        r["restitution"] = fuzzValue(rng);
        r["friction"]    = fuzzValue(rng);
        r["halfExtent"]  = vec(rng, 3, 0.5f);
        r["radius"]      = fuzzValue(rng);
        r["halfHeight"]  = fuzzValue(rng);
        r["useGravity"]  = rng.chance(70) ? json(true) : fuzzValue(rng);
        maybe("rigidBody", std::move(r)); }
    {   json s = json::object(); s["path"] = str(80); maybe("script", std::move(s)); }
    {   json c = json::object();
        c["radius"]      = fuzzValue(rng);
        c["height"]      = fuzzValue(rng);
        c["maxSlopeDeg"] = fuzzValue(rng);
        c["stepHeight"]  = fuzzValue(rng);
        c["mass"]        = fuzzValue(rng);
        c["gravityScale"] = fuzzValue(rng);
        maybe("characterController", std::move(c)); }
    {   json l = json::object();
        l["type"]        = rng.chance(60) ? json(rng.below(5)) : fuzzValue(rng);
        l["color"]       = vec(rng, 3, 1.0f);
        l["intensity"]   = fuzzValue(rng);
        l["range"]       = fuzzValue(rng);
        l["spotInner"]   = fuzzValue(rng);
        l["spotOuter"]   = fuzzValue(rng);
        l["temperatureK"] = fuzzValue(rng);
        l["castShadows"] = rng.chance(70) ? json(false) : fuzzValue(rng);
        maybe("light", std::move(l)); }
    {   json a = json::object();
        a["clipIndex"] = rng.chance(70) ? json((int)rng.range(0, 8)) : fuzzValue(rng);
        a["clipPath"]  = str(60);
        a["speed"]     = fuzzValue(rng);
        a["fade"]      = fuzzValue(rng);
        a["playing"]   = rng.chance(70) ? json(true) : fuzzValue(rng);
        a["looping"]   = rng.chance(70) ? json(true) : fuzzValue(rng);
        maybe("animator", std::move(a)); }
    {   json s = json::object();
        s["skeleton"] = fuzzValue(rng);
        s["path"]     = str(60);
        maybe("skinnedMesh", std::move(s)); }

    // The generic reflected path, plus keys no component claims.
    if (rng.chance(30)) {
        json r = json::object();
        const uint32_t n = rng.range(0, 4);
        for (uint32_t i = 0; i < n; ++i) r["Comp" + std::to_string(i)] = fuzzValue(rng);
        je["reflected"] = std::move(r);
    }
    if (rng.chance(20)) je["totallyUnknownComponent"] = fuzzValue(rng);
    // A whole component that is not an object at all.
    if (rng.chance(15)) je[rng.chance(50) ? "transform" : "camera"] = fuzzValue(rng);
    // And occasionally: not an object at all.
    if (rng.chance(5)) return fuzzValue(rng);
    return je;
}

bool finite3(const float* v, int n) {
    for (int i = 0; i < n; ++i) if (!std::isfinite(v[i])) return false;
    return true;
}

void oneCase(uint64_t masterSeed, fuzz::Report& rep) {
    fuzz::ReproKey key;
    key.masterSeed       = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target           = "entity_serde";

    fuzz::Rng rng(fuzz::deriveSeed(masterSeed, "entity_json"));

    flecs::world w;
    EntitySerde::SerdeContext ctx;
    // Disk mode with every service null: the shape a headless tool or a player
    // with no registry actually has, and the shape that must not crash.
    ctx.mode = EntitySerde::SerdeMode::Disk;

    EntityIdIndex ids;
    ids.build(w);
    ctx.idIndex = &ids;

    const json je = buildEntity(rng);

    flecs::entity e;
    try {
        // ── Property 1: this must not throw ─────────────────────────────────
        e = EntitySerde::createEntity(w, je, ctx, EntitySerde::IdPolicy::Preserve);
    } catch (const std::exception& ex) {
        rep.fail(key, std::string("createEntity THREW: ") + ex.what()
                 + " — scene_serializer.h wraps only json::parse in try/catch, "
                   "so this propagates out of scene load and takes down the "
                   "editor or the player on one hand-edited field");
        return;
    } catch (...) {
        rep.fail(key, "createEntity threw a non-std exception");
        return;
    }

    if (!e.is_alive()) {
        rep.fail(key, "createEntity returned a dead entity");
        return;
    }

    // ── Property 3: whatever it set must be usable ──────────────────────────
    // A NaN here is not a crash; it is worse. It propagates through every child
    // transform and corrupts a frame far away from the load that caused it.
    if (const Transform* t = e.try_get<Transform>()) {
        if (!finite3(&t->position.x, 3) || !finite3(&t->scale.x, 3)
            || !std::isfinite(t->rotation.x) || !std::isfinite(t->rotation.y)
            || !std::isfinite(t->rotation.z) || !std::isfinite(t->rotation.w)) {
            rep.fail(key, "accepted a non-finite Transform — NaN/Inf propagates "
                          "into every descendant's world matrix");
            return;
        }
    }
    if (const Camera* c = e.try_get<Camera>()) {
        if (!std::isfinite(c->fov) || !std::isfinite(c->nearPlane)
            || !std::isfinite(c->farPlane)) {
            rep.fail(key, "accepted a non-finite Camera — a NaN in the "
                          "projection matrix blanks the whole view");
            return;
        }
    }
    if (const Light* l = e.try_get<Light>()) {
        if (!std::isfinite(l->intensity) || !std::isfinite(l->range)
            || !finite3(&l->color.x, 3)) {
            rep.fail(key, "accepted a non-finite Light");
            return;
        }
    }
    if (const RigidBody* rb = e.try_get<RigidBody>()) {
        if (!std::isfinite(rb->mass) || !std::isfinite(rb->friction)
            || !std::isfinite(rb->restitution)) {
            rep.fail(key, "accepted a non-finite RigidBody — Jolt asserts or "
                          "NaNs the whole simulation island");
            return;
        }
    }

    // ── Save must survive whatever load produced ────────────────────────────
    // The editor saves what it loaded. A component that loads but cannot be
    // written back is a scene that cannot be re-saved.
    try {
        (void)EntitySerde::saveEntity(e, ctx);
    } catch (const std::exception& ex) {
        rep.fail(key, std::string("saveEntity threw on an entity createEntity "
                                  "produced: ") + ex.what());
    }
}

} // namespace

int main(int argc, char** argv) {
    return fuzz::run("entity_serde", argc, argv, oneCase);
}
