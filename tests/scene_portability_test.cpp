// ── scene_portability_test — a cooked scene is the same file on every platform ─
//
// A scene is cooked on one machine and loaded on another: the editor on a Mac,
// the player on Windows, a server on Linux arm64. So the cooked bytes cannot
// depend on the compiler, the standard library, the CPU or the OS. This holds
// that mechanically, on every CI leg (ci.yml runs every platform when a change
// touches the scene format), with two committed GOLDEN files:
//
//   tests/fixtures/scenes/every_component.packed.cooked   the tools' form (WO-092)
//   tests/fixtures/scenes/every_component.cbor.cooked     a cook with no packer
//
// Both were cooked from every_component.disk.json (every saved component, none
// at its defaults) by `scene_portability_test --write-fixtures`, once, on one
// machine. They are never regenerated to make this test pass: a change that
// needs them regenerated is a change to the cooked format (bump the layout or
// the version, and re-cook), not an edit here.
//
// On each platform:
//   1. WRITE: cooking the fixture here gives the golden bytes exactly. A struct
//      padded differently, a bool or enum of another width, another byte order,
//      a container iterated in another order: each shows as the first byte that
//      differs, named by the entity and record it falls in.
//   2. READ: the golden files load here, through every reader the player uses
//      (packed, CBOR in place, CBOR through the tree), and every entity saves
//      back as the fixture has it.
// BUG-0082 (a check MSVC could not compile) is the class of defect this exists
// to catch before a player does.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <flecs.h>
#include <nlohmann/json.hpp>
#include <assetlib/scene_asset.h>

#include "assets/cooked_scene.h"
#include "assets/packed_scene.h"
#include "components/meta_registry.h"
#include "scene/entity_serializer.h"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace EntitySerde;

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

namespace {

const fs::path kFixtures = fs::path(ENGINE_SOURCE_DIR) / "tests" / "fixtures" / "scenes";
const fs::path kSource   = kFixtures / "every_component.disk.json";
const fs::path kPacked   = kFixtures / "every_component.packed.cooked";
const fs::path kCbor     = kFixtures / "every_component.cbor.cooked";

std::vector<uint8_t> readBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The kit component the fixture carries (as scene_schema_test registers it).
struct Health { float current = 100.0f; float max = 100.0f; };

flecs::world makeWorld() {
    flecs::world w;
    MetaRegistry::registerAll(w);
    w.component<Health>("combat::Health")
        .member("current", &Health::current)
        .member("max", &Health::max);
    return w;
}

json sourceScene() {
    std::ifstream f(kSource);
    return json::parse(f);
}

// Cooked here, through the code under test, written as the tools write it.
std::vector<uint8_t> cookHere(bool packedForm) {
    assetlib::SceneAsset file;
    std::string why;
    if (packedForm) packScene(sourceScene(), file, why);
    else            cooked_scene::encode(sourceScene(), file);
    const fs::path tmp = fs::temp_directory_path() /
        (packedForm ? "scene_portability.packed.cooked" : "scene_portability.cbor.cooked");
    assetlib::saveScene(file, tmp);
    std::vector<uint8_t> bytes = readBytes(tmp);
    fs::remove(tmp);
    return bytes;
}

// Where offset `at` falls in a packed cooked file, for a message a person can
// act on: the header, the schema table, or entity N's record of schema S.
std::string whereInPacked(const std::vector<uint8_t>& file, std::size_t at) {
    constexpr std::size_t kHeader = sizeof(assetlib::SceneHeader);
    if (at < kHeader) return "the file header";
    const uint8_t* p = file.data() + kHeader;
    const std::size_t n = file.size() - kHeader;
    const std::size_t off = at - kHeader;
    packed::View view;
    std::string why;
    if (!view.open(p, n, why)) return "the payload (which does not open: " + why + ")";
    packed::Entity e;
    for (uint32_t i = 0; view.next(e); ++i) {
        for (const packed::Record& r : e.records) {
            const std::size_t b = (std::size_t)(r.data - p);
            if (off >= b && off < b + r.size)
                return "entity " + std::to_string(i) + ", the " + view.schemas()[r.schema].name +
                       " record, byte " + std::to_string(off - b) + " of " + std::to_string(r.size);
        }
        if (e.rest) {
            const std::size_t b = (std::size_t)(e.rest - p);
            if (off >= b && off < b + e.restSize)
                return "entity " + std::to_string(i) + ", its CBOR rest";
        }
    }
    return "the payload's tables (schemas, counts, ids)";
}

// The first byte two files differ at, or npos.
std::size_t firstDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) if (a[i] != b[i]) return i;
    return a.size() == b.size() ? std::string::npos : n;
}

void checkWrite(const char* what, const fs::path& golden, bool packedForm) {
    const std::vector<uint8_t> want = readBytes(golden), got = cookHere(packedForm);
    CHECK(!want.empty(), "%s: the golden file is present (%s)", what, golden.filename().string().c_str());
    if (want.empty()) return;
    const std::size_t at = firstDifference(want, got);
    if (at == std::string::npos) {
        CHECK(true, "%s: cooked here, byte for byte the golden file (%zu bytes)", what, want.size());
        return;
    }
    const std::string where = packedForm ? whereInPacked(want, at) : std::string("the CBOR payload");
    CHECK(false, "%s: cooked here differs from the golden file at byte %zu (%s); sizes %zu here, "
                 "%zu golden. This platform writes a different cooked scene.",
          what, at, where.c_str(), got.size(), want.size());
}

// Every entity loaded from `file` by the named reader, saved back as disk JSON.
json loadAndSave(const assetlib::SceneAsset& file, const char* reader) {
    flecs::world w = makeWorld();
    SerdeContext ctx; ctx.mode = SerdeMode::Disk;
    std::vector<SerdeContext::EntityRef> refs; ctx.entityRefs = &refs;
    std::vector<flecs::entity> made;
    std::vector<uint64_t> parents;
    const uint8_t* data = file.payload.data();
    const std::size_t size = file.payload.size();
    const std::string r = reader;
    if (r == "packed") {
        packed::View view;
        std::string why;
        if (!view.open(data, size, why)) return json();
        const PackedSchemas match = matchPackedSchemas(view, schemas());
        packed::Entity pe;
        while (view.next(pe)) {
            flecs::entity e;
            if (!createEntityPacked(w, view, pe, match, ctx, IdPolicy::Preserve, e)) return json();
            made.push_back(e);
            parents.push_back(pe.parentId);
        }
        if (!view.complete()) return json();
    } else if (r == "cbor in place") {
        cbor::Reader rd(data, size);
        cbor::Item top, key, arr;
        if (!rd.head(top) || top.kind != cbor::Kind::Map) return json();
        for (uint64_t n = 0; n < top.count; ++n) {
            if (!rd.head(key) || key.kind != cbor::Kind::Text) return json();
            if (key.text != "entities") { rd.skip(); continue; }
            if (!rd.head(arr) || arr.kind != cbor::Kind::Array) return json();
            for (uint64_t k = 0; k < arr.count; ++k) {
                const std::size_t at = rd.pos();
                CookedIds ids;
                flecs::entity e;
                if (!createEntityCbor(w, data, size, at, ctx, IdPolicy::Preserve, e, &ids)) return json();
                made.push_back(e);
                parents.push_back(ids.parentId);
                rd.skip();
            }
        }
    } else {
        json scene;
        std::string why;
        if (!cooked_scene::decode(file, scene, why)) return json();
        for (const json& je : scene["entities"]) {
            made.push_back(createEntity(w, je, ctx, IdPolicy::Preserve));
            parents.push_back(je.value("parentId", 0ull));
        }
    }
    resolveEntityRefs(w, ctx);
    // Parents by the ids as written, as SceneService links them.
    for (std::size_t i = 0; i < made.size(); ++i) {
        if (!parents[i]) continue;
        for (flecs::entity p : made)
            if (const EntityId* id = p.try_get<EntityId>(); id && id->value == parents[i])
                made[i].add(flecs::ChildOf, p);
    }
    json out = json::array();
    for (flecs::entity e : made) out.push_back(saveEntity(e, ctx));
    return out;
}

void checkRead(const char* what, const fs::path& golden, std::initializer_list<const char*> readers) {
    assetlib::SceneAsset file;
    std::string why;
    const bool loaded = assetlib::loadScene(file, golden, &why);
    CHECK(loaded, "%s: the golden file loads here (%s)", what, loaded ? "header and size agree" : why.c_str());
    if (!loaded) return;
    const json want = sourceScene()["entities"];
    for (const char* reader : readers) {
        const json got = loadAndSave(file, reader);
        if (got == want) {
            CHECK(true, "%s, read %s: every entity saves back as authored (%zu entities)", what, reader, want.size());
            continue;
        }
        std::string detail = "no entities read";
        if (got.is_array())
            for (std::size_t i = 0; i < want.size(); ++i)
                if (i >= got.size() || got[i] != want[i]) {
                    detail = "entity " + std::to_string(i) + "\n    want " + want[i].dump() + "\n    got  " +
                             (i < got.size() ? got[i].dump() : std::string("(missing)"));
                    break;
                }
        CHECK(false, "%s, read %s: differs from the source, first at %s", what, reader, detail.c_str());
    }
}

void writeFixture(const fs::path& p, bool packedForm) {
    if (fs::exists(p)) { std::printf("kept     %s\n", p.string().c_str()); return; }
    const std::vector<uint8_t> bytes = cookHere(packedForm);
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    std::printf("wrote    %s (%zu bytes)\n", p.string().c_str(), bytes.size());
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::strcmp(argv[1], "--write-fixtures") == 0) {
        // Never over an existing golden file: it records what an EARLIER build
        // wrote, and regenerating it with the code under test proves nothing.
        writeFixture(kPacked, true);
        writeFixture(kCbor, false);
        return 0;
    }
    std::printf("scene_portability_test: one cooked scene, every platform\n");

    std::printf("\n-- cooked here, byte for byte the golden files --\n");
    checkWrite("packed", kPacked, true);
    checkWrite("cbor", kCbor, false);

    std::printf("\n-- the golden files, read here --\n");
    checkRead("packed", kPacked, {"packed"});
    checkRead("cbor", kCbor, {"cbor in place", "cbor through the tree"});

    std::printf("\n-- what the layout assumes, on this compiler --\n");
    CHECK(sizeof(assetlib::SceneHeader) == 32 && sizeof(float) == 4 && sizeof(double) == 8,
          "the header is 32 bytes, floats 4, doubles 8");
    {
        const uint32_t one = 1;
        uint8_t first = 0;
        std::memcpy(&first, &one, 1);
        CHECK(first == 1, "little-endian at run time, as packed_scene.h asserts at compile time");
    }

    if (g_failures) {
        std::printf("\nscene_portability_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nscene_portability_test: all checks passed\n");
    return 0;
}
