// ── CgltfFrontend — see frontend_cgltf.h ──────────────────────────────────────
#include "assets/import/frontend_cgltf.h"
#include "assets/importers/gltf_losses.h"   // the skin/animation judgement, shared with GltfImporter

#include <cgltf.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace imp {
namespace {

const cgltf_accessor* attribute(const cgltf_primitive& p, cgltf_attribute_type t, int index = 0) {
    for (cgltf_size i = 0; i < p.attributes_count; ++i)
        if (p.attributes[i].type == t && p.attributes[i].index == index) return p.attributes[i].data;
    return nullptr;
}

// Smooth, area-weighted normals, for a primitive that has none. The glTF spec
// asks for flat normals here; smooth ones are what Assimp's GenSmoothNormals
// gives an FBX without normals, so the two formats agree (plan §7.1).
std::vector<Float3> smoothNormals(const std::vector<Float3>& p, const std::vector<uint32_t>& idx) {
    std::vector<Float3> n(p.size());
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const Float3 a = p[idx[i]], b = p[idx[i + 1]], c = p[idx[i + 2]];
        const Float3 e1{b.x - a.x, b.y - a.y, b.z - a.z}, e2{c.x - a.x, c.y - a.y, c.z - a.z};
        const Float3 f{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
        for (int k = 0; k < 3; ++k) { Float3& v = n[idx[i + k]]; v.x += f.x; v.y += f.y; v.z += f.z; }
    }
    for (Float3& v : n) {
        const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        v = l > 1e-12f ? Float3{v.x / l, v.y / l, v.z / l} : Float3{0, 1, 0};
    }
    return n;
}

struct Reader {
    const cgltf_data& data;
    std::filesystem::path dir;
    ImportedScene& out;

    std::map<const cgltf_mesh*, std::vector<uint32_t>> meshIds;   // glTF mesh -> one imp::Mesh per primitive
    int32_t  defaultMaterial = -1;
    uint32_t nonTriangles = 0, extraUvSets = 0, vertexColours = 0, noPositions = 0;
    std::set<std::string> droppedTextures;

    void drop(Dropped::Kind k, Dropped::Effect e, uint32_t count, std::string what) {
        out.dropped.push_back({k, e, count, std::move(what)});
    }

    // ── Images: the plan's ONE rule (§4.1). External files are named relative to
    // the glTF, embedded ones (a bufferView or a data: URI) carry their bytes,
    // and a reference that does not resolve is DROPPED with its path.
    TextureRef texture(const cgltf_texture* t) {
        TextureRef ref;
        if (!t) return ref;
        const cgltf_image* img = t->image;
        if (!img) { droppedTextures.insert("a texture with no image this reader can decode"); return ref; }
        const std::string label = img->name && *img->name ? img->name
                                : "image " + std::to_string(img - data.images);
        if (img->buffer_view && img->buffer_view->buffer->data) {
            const auto* b = static_cast<const uint8_t*>(img->buffer_view->buffer->data) + img->buffer_view->offset;
            ref.embedded.assign(b, b + img->buffer_view->size);
            ref.embeddedName = label;
        } else if (img->uri && std::strncmp(img->uri, "data:", 5) == 0) {
            // data:<mime>;base64,<payload> — decoded here; the old cook path
            // skipped these, so the material silently lost its texture.
            const char* comma = std::strchr(img->uri, ',');
            void* bytes = nullptr;
            const std::string payload = comma ? comma + 1 : "";
            const cgltf_size size = payload.size() / 4 * 3
                                  - (payload.size() >= 1 && payload.back() == '=')
                                  - (payload.size() >= 2 && payload[payload.size() - 2] == '=');
            cgltf_options o{};
            if (comma && cgltf_load_buffer_base64(&o, size, payload.c_str(), &bytes) == cgltf_result_success) {
                ref.embedded.assign(static_cast<uint8_t*>(bytes), static_cast<uint8_t*>(bytes) + size);
                ref.embeddedName = label;
                std::free(bytes);                   // default options: cgltf allocated with malloc
            } else {
                droppedTextures.insert("'" + label + "' (an undecodable data: URI)");
            }
        } else if (img->uri) {
            std::string uri = img->uri;
            uri.resize(cgltf_decode_uri(uri.data()));         // %20 -> ' ', in place
            if (std::filesystem::exists(dir / uri)) ref.path = uri;
            else droppedTextures.insert("'" + uri + "' (not found beside the glTF)");
        }
        return ref;
    }

    void materials() {
        for (cgltf_size i = 0; i < data.materials_count; ++i) {
            const cgltf_material& gm = data.materials[i];
            Material m;
            m.name = gm.name && *gm.name ? gm.name : "material " + std::to_string(i);
            if (gm.has_pbr_metallic_roughness) {
                const auto& pbr = gm.pbr_metallic_roughness;
                m.baseColorFactor = {pbr.base_color_factor[0], pbr.base_color_factor[1],
                                     pbr.base_color_factor[2], pbr.base_color_factor[3]};
                m.roughness = pbr.roughness_factor;
                m.metallic  = pbr.metallic_factor;
                m.baseColor = texture(pbr.base_color_texture.texture);
            }
            m.normal = texture(gm.normal_texture.texture);
            out.materials.push_back(std::move(m));
        }
    }

    // A primitive with no material gets a real default, appended once. (The old
    // cook path gave it material 0, which is whatever the file listed first.)
    uint32_t materialOf(const cgltf_primitive& p) {
        if (p.material) return (uint32_t)(p.material - data.materials);
        if (defaultMaterial < 0) {
            defaultMaterial = (int32_t)out.materials.size();
            Material m; m.name = "default";
            out.materials.push_back(m);
        }
        return (uint32_t)defaultMaterial;
    }

    // One imp::Mesh per triangle primitive, in file order, so the back end emits
    // them in the order the old cook path did.
    const std::vector<uint32_t>& meshesOf(const cgltf_mesh* gm) {
        if (auto it = meshIds.find(gm); it != meshIds.end()) return it->second;
        std::vector<uint32_t>& ids = meshIds[gm];
        const std::string meshName = gm->name && *gm->name ? gm->name : "mesh " + std::to_string(gm - data.meshes);
        uint32_t morphs = 0;
        for (cgltf_size pi = 0; pi < gm->primitives_count; ++pi) {
            const cgltf_primitive& p = gm->primitives[pi];
            if (p.type != cgltf_primitive_type_triangles) { ++nonTriangles; continue; }
            const cgltf_accessor* pos = attribute(p, cgltf_attribute_type_position);
            if (!pos) { ++noPositions; continue; }
            morphs += (uint32_t)p.targets_count;
            if (attribute(p, cgltf_attribute_type_color)) ++vertexColours;
            if (attribute(p, cgltf_attribute_type_texcoord, 1)) ++extraUvSets;

            Mesh m;
            m.name = gm->primitives_count > 1 ? meshName + " #" + std::to_string(pi) : meshName;
            const size_t n = pos->count;
            m.positions.resize(n);
            for (size_t v = 0; v < n; ++v) cgltf_accessor_read_float(pos, v, &m.positions[v].x, 3);

            if (p.indices) {
                m.indices.resize(p.indices->count);
                for (size_t i = 0; i < m.indices.size(); ++i)
                    m.indices[i] = (uint32_t)cgltf_accessor_read_index(p.indices, i);
            } else {                                        // non-indexed: the old path skipped these
                m.indices.resize(n);
                for (size_t i = 0; i < n; ++i) m.indices[i] = (uint32_t)i;
            }
            if (m.indices.size() % 3) m.indices.resize(m.indices.size() / 3 * 3);

            if (const cgltf_accessor* nrm = attribute(p, cgltf_attribute_type_normal)) {
                m.normals.resize(n);
                for (size_t v = 0; v < n; ++v) cgltf_accessor_read_float(nrm, v, &m.normals[v].x, 3);
            } else {
                m.normals = smoothNormals(m.positions, m.indices);
            }
            if (const cgltf_accessor* uv = attribute(p, cgltf_attribute_type_texcoord)) {
                m.uv0.resize(n);                            // glTF's UV origin is already top-left
                for (size_t v = 0; v < n; ++v) cgltf_accessor_read_float(uv, v, &m.uv0[v].x, 2);
            }
            if (const cgltf_accessor* tan = attribute(p, cgltf_attribute_type_tangent)) {
                m.tangents.resize(n);
                for (size_t v = 0; v < n; ++v) {
                    float t[4] = {1, 0, 0, 1};
                    cgltf_accessor_read_float(tan, v, t, 4);
                    m.tangents[v] = {t[0], t[1], t[2], t[3] < 0 ? -1.0f : 1.0f};
                }
            }
            if (m.indices.empty()) continue;
            m.submeshes = {{0, (uint32_t)m.indices.size(), materialOf(p)}};
            ids.push_back((uint32_t)out.meshes.size());
            out.meshes.push_back(std::move(m));
        }
        if (morphs) drop(Dropped::Kind::MorphTargets, Dropped::Effect::Less, morphs,
                         std::to_string(morphs) + " morph target(s) on '" + meshName + "'");
        return ids;
    }

    // Depth-first, pre-order: the order the old cook path emitted nodes in.
    void node(const cgltf_node* gn, int32_t parent) {
        const int32_t self = (int32_t)out.nodes.size();
        Node nd;
        nd.name   = gn->name && *gn->name ? gn->name : "node " + std::to_string(gn - data.nodes);
        nd.parent = parent;
        cgltf_node_transform_local(gn, nd.local.m);         // column-major, as Float4x4
        out.nodes.push_back(std::move(nd));
        if (gn->mesh) {
            const std::vector<uint32_t> ids = meshesOf(gn->mesh);   // copy: meshesOf may grow the map
            out.nodes[(size_t)self].meshes = ids;
        }
        for (cgltf_size c = 0; c < gn->children_count; ++c) node(gn->children[c], self);
    }
};

// Extensions this reader handles by reading the file as cgltf presents it.
bool understood(const char* ext) {
    return std::strcmp(ext, "KHR_mesh_quantization") == 0      // cgltf_accessor_read_float de-quantises
        || std::strcmp(ext, "KHR_lights_punctual") == 0;       // reported as Light, below
}

}  // namespace

ImportResult CgltfFrontend::importScene(const std::filesystem::path& source, const ImportOptions&) const {
    const std::string src = source.string();
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, src.c_str(), &data) != cgltf_result_success)
        return ImportError{ImportError::Kind::Unreadable, "not a readable glTF: " + src};
    struct Guard { cgltf_data* d; ~Guard() { cgltf_free(d); } } guard{data};
    if (cgltf_load_buffers(&options, data, src.c_str()) != cgltf_result_success)
        return ImportError{ImportError::Kind::Unreadable, "glTF buffers could not be loaded: " + src};
    // Structural validation (accessor ranges, index bounds) before reading a
    // byte: this is hostile input. The old cook path skipped it.
    if (cgltf_validate(data) != cgltf_result_success)
        return ImportError{ImportError::Kind::Unreadable, "invalid glTF (failed validation): " + src};

    ImportedScene scene;
    scene.source = src;
    Reader r{*data, source.parent_path(), scene};

    // ── What the file has that this front end does not carry ────────────────
    const GltfLosses losses = gltfLosses(*data);
    if (losses.skins > 0)
        r.drop(Dropped::Kind::Skin, Dropped::Effect::Wrong, (uint32_t)losses.skins, losses.describe());
    else if (losses.animations > 0 && losses.meshes > 0)
        r.drop(Dropped::Kind::Animation, Dropped::Effect::Less, (uint32_t)losses.animations, losses.describe());
    for (cgltf_size i = 0; i < data->extensions_required_count; ++i)
        if (!understood(data->extensions_required[i]))
            r.drop(Dropped::Kind::Extension, Dropped::Effect::Wrong, 1,
                   std::string("requires ") + data->extensions_required[i]);
    for (cgltf_size i = 0; i < data->extensions_used_count; ++i) {
        const char* e = data->extensions_used[i];
        bool required = false;
        for (cgltf_size k = 0; k < data->extensions_required_count; ++k)
            required |= std::strcmp(e, data->extensions_required[k]) == 0;
        if (!required && !understood(e))
            r.drop(Dropped::Kind::Extension, Dropped::Effect::Less, 1, std::string("ignores ") + e);
    }
    if (data->cameras_count)
        r.drop(Dropped::Kind::Camera, Dropped::Effect::Less, (uint32_t)data->cameras_count,
               std::to_string(data->cameras_count) + " camera(s)");
    if (data->lights_count)
        r.drop(Dropped::Kind::Light, Dropped::Effect::Less, (uint32_t)data->lights_count,
               std::to_string(data->lights_count) + " light(s)");

    // ── Materials, then the default scene's node tree under one root ────────
    r.materials();
    const cgltf_scene* gs = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
    Node root; root.name = "root";
    scene.nodes.push_back(root);
    if (gs)
        for (cgltf_size i = 0; i < gs->nodes_count; ++i) r.node(gs->nodes[i], 0);

    if (r.nonTriangles) r.drop(Dropped::Kind::NonTriangles, Dropped::Effect::Less, r.nonTriangles,
                               std::to_string(r.nonTriangles) + " point/line primitive(s)");
    if (r.noPositions) r.drop(Dropped::Kind::NonTriangles, Dropped::Effect::Less, r.noPositions,
                              std::to_string(r.noPositions) + " primitive(s) with no POSITION");
    if (r.vertexColours) r.drop(Dropped::Kind::VertexColours, Dropped::Effect::Less, r.vertexColours,
                                "COLOR_0 on " + std::to_string(r.vertexColours) + " primitive(s)");
    if (r.extraUvSets) r.drop(Dropped::Kind::ExtraUvSets, Dropped::Effect::Less, r.extraUvSets,
                              "TEXCOORD_1+ on " + std::to_string(r.extraUvSets) + " primitive(s)");
    for (const std::string& t : r.droppedTextures)
        r.drop(Dropped::Kind::Texture, Dropped::Effect::Less, 1, "texture " + t);

    // ── Nothing to import is its own answer (the contract's Empty) ──────────
    if (scene.meshes.empty()) {
        std::string why = "nothing to import: " + src;
        if (losses.animations > 0 || losses.skins > 0) why += " (" + losses.describe() + ")";
        return ImportError{ImportError::Kind::Empty, why};
    }
    return scene;
}

}  // namespace imp
