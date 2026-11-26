// ── fuzz_scene_reader_parity_test — the two scene readers agree (WO-092) ─────
//
// The editor reads a .scene as JSON (EntitySerde::createEntity). The shipped
// player reads the cooked scene's CBOR in place (createEntityCbor), with no JSON
// tree. They share every field reader through one value interface
// (component_schema.h), but the entity walk, the component walk and the CBOR
// adapter are separate code, and a scene must mean the same thing in both.
//
// So each case builds one random entity: every registered component, each
// well-formed, wrong-typed, out of range or junk; unknown keys; a kit's block; a
// revision marker; a mesh reference whose fields are strings or not. It loads it
// through both readers into two worlds and requires the saved results (Disk
// mode) to be identical, and the set of components on the two entities to be the
// same. A fake host resolves some cooked mesh paths and gives some a skin, so the
// mesh and skin paths run too.
#include <cstdio>
#include "fuzz/fuzz.h"

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "components/meta_registry.h"
#include "scene/entity_serializer.h"

#include <algorithm>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace EntitySerde;

static constexpr uint32_t kGeneratorVersion = 1;

namespace {

struct Health { float current = 100.0f; float max = 100.0f; };

json junk(fuzz::Rng& rng, int depth = 0) {
    switch (depth > 2 ? rng.below(6) : rng.below(9)) {
        case 0: return nullptr;
        case 1: return rng.chance(50);
        case 2: return (int64_t)rng.interestingU32() * (rng.chance(30) ? -1 : 1);
        case 3: return (uint64_t)rng.next();
        case 4: return (double)(int32_t)rng.next() / 64.0;
        case 5: { std::string s; for (uint32_t k = rng.range(0, 12); k > 0; --k) s.push_back((char)rng.range(32, 126)); return s; }
        case 6: { json a = json::array(); for (uint32_t k = rng.range(0, 5); k > 0; --k) a.push_back(junk(rng, depth + 1)); return a; }
        case 7: { json a = json::array(); for (uint32_t k = rng.range(0, 5); k > 0; --k) a.push_back((double)rng.below(100) / 4.0); return a; }
        default: { json o = json::object(); for (uint32_t k = rng.range(0, 4); k > 0; --k) o["k" + std::to_string(k)] = junk(rng, depth + 1); return o; }
    }
}

// A component object from a registered schema's own fields, each value either
// plausible for its type or junk, sometimes with an extra key or a marker.
json componentFor(const ComponentSchema& s, fuzz::Rng& rng) {
    if (rng.chance(8)) return junk(rng);
    json o = json::object();
    for (const FieldInfo& f : s.fields) {
        if (rng.chance(20)) continue;
        if (rng.chance(25)) { o[f.name] = junk(rng); continue; }
        switch (f.type) {
            case FieldType::Bool:   o[f.name] = rng.chance(50); break;
            case FieldType::F32: case FieldType::F64:
                o[f.name] = rng.chance(10) ? json((uint64_t)rng.next()) : json((double)(int32_t)rng.next() / 1000.0); break;
            case FieldType::String: o[f.name] = "s" + std::to_string(rng.below(100)); break;
            case FieldType::Vec2: case FieldType::Vec3: case FieldType::Vec4: case FieldType::Quat: case FieldType::Mat4: {
                json a = json::array();
                for (uint32_t k = rng.range(0, 18); k > 0; --k) a.push_back(rng.chance(85) ? json((double)rng.below(1000) / 8.0) : junk(rng));
                o[f.name] = a; break;
            }
            default:   // the integers, enums, entity and asset fields
                o[f.name] = rng.chance(70) ? json((int64_t)rng.interestingU32() - (rng.chance(40) ? 300 : 0)) : junk(rng);
        }
    }
    if (rng.chance(5)) o["$rev"] = rng.chance(50) ? json(rng.below(3)) : junk(rng);
    if (rng.chance(10)) o["extra"] = junk(rng);
    return o;
}

json meshRef(fuzz::Rng& rng) {
    if (rng.chance(8)) return junk(rng);
    json o = json::object();
    const char* keys[] = { "cookedPath", "path", "asset", "material", "sourceType", "sourcePath" };
    for (const char* k : keys) {
        if (rng.chance(50)) continue;
        if (rng.chance(12)) { o[k] = junk(rng); continue; }
        if (std::string(k) == "cookedPath") o[k] = "meshs/m" + std::to_string(rng.below(4)) + ".cooked";
        else if (std::string(k) == "sourceType") o[k] = rng.chance(50) ? "primitive" : "x";
        else o[k] = std::string(k) + std::to_string(rng.below(3));
    }
    return o;
}

json randomEntity(fuzz::Rng& rng, const SchemaRegistry& reg) {
    json e = json::object();
    if (rng.chance(85)) e["id"] = rng.chance(80) ? json((uint64_t)rng.range(1, 1000000)) : junk(rng);
    if (rng.chance(15)) e["parentId"] = junk(rng);
    for (const ComponentSchema& s : reg.all()) {
        if (!rng.chance(35)) continue;
        if (s.key == "meshRenderer") e[s.key] = meshRef(rng);
        else if (s.key == "name") e[s.key] = rng.chance(85) ? json("n" + std::to_string(rng.below(50))) : junk(rng);
        else if (!s.fields.empty()) e[s.key] = componentFor(s, rng);
        else e[s.key] = rng.chance(50) ? json::object() : junk(rng);   // lodMesh, skinnedMesh, animator
    }
    if (rng.chance(15)) e["sprite"] = junk(rng);
    if (rng.chance(10)) e["zz.Unknown"] = junk(rng);
    if (rng.chance(15)) e["reflected"] = rng.chance(70)
        ? json{{"combat::Health", {{"current", (double)rng.below(200)}, {"max", junk(rng)}}}} : junk(rng);
    return e;
}

flecs::world makeWorld() {
    flecs::world w;
    MetaRegistry::registerAll(w);
    w.component<Health>("combat::Health").member("current", &Health::current).member("max", &Health::max);
    return w;
}

// The fake host: meshs/m0 and m1 resolve, m1 with a skin; the rest do not.
SceneAssets fakeHost() {
    SceneAssets a;
    a.loadCookedMesh = [](const char* path, std::vector<MeshHandle>* lods, StreamedMesh* skin) {
        const std::string p = path;
        MeshHandle h;
        if (p == "meshs/m0.cooked" || p == "meshs/m1.cooked") h.id = p == "meshs/m0.cooked" ? 3 : 4;
        if (lods && p == "meshs/m0.cooked") { MeshHandle l; l.id = 30; lods->push_back(l); }
        if (skin && p == "meshs/m1.cooked") {
            skin->skeleton.id = 5;
            AnimClipHandle c; c.id = 7; skin->clips = { c };
        }
        return h;
    };
    a.loadMaterial = [](const char* name) { MaterialHandle m; m.id = std::string(name) == "material1" ? 9 : 0; return m; };
    a.bindClip = [](const std::string& path, SkeletonHandle) { AnimClipHandle c; c.id = path.size() % 2 ? 11 : 0; return c; };
    return a;
}

json saved(flecs::entity e) {
    SerdeContext ctx; ctx.mode = SerdeMode::Disk;
    ctx.meshLookup = [](MeshHandle) -> const Mesh* { return nullptr; };
    return saveEntity(e, ctx);
}

std::vector<std::string> componentsOf(flecs::entity e) {
    std::vector<std::string> out;
    e.each([&](flecs::id id) { out.push_back(id.str().c_str()); });
    // Sorted: each world numbers components in the order it first meets them.
    std::sort(out.begin(), out.end());
    return out;
}

void oneCase(uint64_t masterSeed, fuzz::Report& rep) {
    fuzz::ReproKey key;
    key.masterSeed       = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target           = "scene_reader_parity";
    fuzz::Rng rng(fuzz::deriveSeed(masterSeed, "parity_entity"));

    // Through text, as a .scene is: a tree built in memory can hold a
    // non-negative integer as signed, which no parsed file does, and the two
    // readers are compared on what the editor actually reads.
    const json je = json::parse(randomEntity(rng, schemas()).dump());
    const std::vector<uint8_t> bytes = json::to_cbor(je);

    const SceneAssets host = fakeHost();
    flecs::world wa = makeWorld(), wb = makeWorld();
    SerdeContext ca; ca.mode = SerdeMode::Disk; ca.sceneAssets = &host;
    SerdeContext cb; cb.mode = SerdeMode::Disk; cb.sceneAssets = &host;
    std::vector<SerdeContext::EntityRef> ra, rb;
    ca.entityRefs = &ra; cb.entityRefs = &rb;

    flecs::entity a = createEntity(wa, je, ca, IdPolicy::Preserve);
    flecs::entity b;
    const bool ok = createEntityCbor(wb, bytes.data(), bytes.size(), 0, cb, IdPolicy::Preserve, b);
    if (!ok) {
        // Only bytes the cook does not write may take the tree path, and a
        // to_cbor of a JSON tree is exactly what the cook writes.
        rep.fail(key, "the CBOR reader refused what to_cbor wrote: " + je.dump());
        return;
    }
    json sa = saved(a), sb = saved(b);
    // An entity with no usable id is given a fresh, random one by each reader:
    // that is the same behaviour, not a disagreement.
    uint64_t want = 0;
    if (auto it = je.find("id"); it != je.end()) want = idFrom(JsonValue{&*it});
    if (want == 0) { sa.erase("id"); sb.erase("id"); }
    if (sa != sb) {
        rep.fail(key, "the readers disagree.\n  entity: " + je.dump() + "\n  json:   " + sa.dump()
                 + "\n  cbor:   " + sb.dump());
        return;
    }
    const auto ka = componentsOf(a), kb = componentsOf(b);
    if (ka != kb) {
        std::string da, db;
        for (const auto& c : ka) da += c + " ";
        for (const auto& c : kb) db += c + " ";
        rep.fail(key, "the readers left different components on the entity: " + je.dump()
                 + "\n  json: " + da + "\n  cbor: " + db);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return fuzz::run("scene_reader_parity", argc, argv,
                     [](uint64_t seed, fuzz::Report& rep) { oneCase(seed, rep); });
}
