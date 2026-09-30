// ── CgltfFrontend — see frontend_cgltf.h ──────────────────────────────────────
#include "assets/import/frontend_cgltf.h"
#include "assets/import/imported_scene_check.h"   // worldOf, inverse, nearIdentity

#include <cgltf.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
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

    // glTF mesh (+ the skin it is drawn with) -> one imp::Mesh per primitive
    std::map<std::pair<const cgltf_mesh*, const cgltf_skin*>, std::vector<uint32_t>> meshIds;
    std::vector<const cgltf_node*> gltfOf;               // imp node index -> glTF node
    std::map<const cgltf_node*, uint16_t> boneOf;        // glTF node -> imp bone index
    uint32_t unweighted = 0, extraInfluences = 0;
    int32_t  defaultMaterial = -1;
    uint32_t nonTriangles = 0, extraUvSets = 0, vertexColours = 0, noPositions = 0;
    uint32_t emptySkins = 0;   // mesh nodes whose skin lists no joints
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
    const std::vector<uint32_t>& meshesOf(const cgltf_mesh* gm, const cgltf_skin* skin) {
        if (auto it = meshIds.find({gm, skin}); it != meshIds.end()) return it->second;
        std::vector<uint32_t>& ids = meshIds[{gm, skin}];
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
            // Skin weights: JOINTS_0 indexes the SKIN's joint list; remap each to
            // the skeleton's bone. Weights are normalised; a vertex influenced by
            // nothing follows the root bone rather than collapsing to the origin.
            const cgltf_accessor* ja = attribute(p, cgltf_attribute_type_joints);
            const cgltf_accessor* wa = attribute(p, cgltf_attribute_type_weights);
            if (skin && ja && wa) {
                if (attribute(p, cgltf_attribute_type_joints, 1)) ++extraInfluences;
                m.joints.resize(n); m.weights.resize(n);
                for (size_t v = 0; v < n; ++v) {
                    cgltf_uint j[4] = {0, 0, 0, 0};
                    float w[4] = {0, 0, 0, 0};
                    cgltf_accessor_read_uint(ja, v, j, 4);
                    cgltf_accessor_read_float(wa, v, w, 4);
                    float sum = 0;
                    for (int k = 0; k < 4; ++k) {
                        if (w[k] < 0 || j[k] >= skin->joints_count) w[k] = 0;
                        sum += w[k];
                    }
                    if (sum < 1e-6f) { w[0] = 1; sum = 1; j[0] = 0; ++unweighted; }
                    std::array<uint16_t, 4> bones{};
                    for (int k = 0; k < 4; ++k) {
                        bones[k] = w[k] > 0 ? boneOf.at(skin->joints[j[k]]) : 0;
                        w[k] /= sum;
                    }
                    m.joints[v] = bones;
                    m.weights[v] = {w[0], w[1], w[2], w[3]};
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
    // Only the tree here; meshes are attached once the skeleton exists, because
    // skin weights are remapped to its bones.
    // An explicit stack, not recursion: the depth is the FILE's, and a 44 KB
    // chain of 2,000 nodes overflowed a 512 KB thread stack (SIGBUS, which no
    // exception boundary catches). Children are pushed in reverse, so the
    // order is the recursive pre-order exactly.
    void node(const cgltf_node* top, int32_t topParent) {
        std::vector<std::pair<const cgltf_node*, int32_t>> stack{{top, topParent}};
        while (!stack.empty()) {
            const auto [gn, parent] = stack.back();
            stack.pop_back();
            const int32_t self = (int32_t)out.nodes.size();
            Node nd;
            nd.name   = gn->name && *gn->name ? gn->name : "node " + std::to_string(gn - data.nodes);
            nd.parent = parent;
            cgltf_node_transform_local(gn, nd.local.m);     // column-major, as Float4x4
            out.nodes.push_back(std::move(nd));
            gltfOf.push_back(gn);
            for (cgltf_size c = gn->children_count; c-- > 0;) stack.push_back({gn->children[c], self});
        }
    }

    // ── The skeleton (WO-014) ────────────────────────────────────────────────
    // Bones are every skin's joints plus their ancestors, in node order: the
    // ancestors carry any conversion a file puts above its armature, as the
    // Assimp front end does. Rest pose is the node transforms; a joint's
    // inverseBind is its skin's own matrix (the pose the skin was bound in),
    // an ancestor's is the inverse of its world. `animatedOnly` builds the
    // skeleton of an animation-only file from the nodes its clips animate.
    void skeleton(bool animatedOnly) {
        std::set<const cgltf_node*> keep;
        // Stops at the first ancestor already kept: its own ancestors are too.
        // Walking every joint to the root was O(joints x depth).
        auto withAncestors = [&](const cgltf_node* n) { for (; n && keep.insert(n).second; n = n->parent) {} };
        for (cgltf_size si = 0; si < data.skins_count; ++si)
            for (cgltf_size j = 0; j < data.skins[si].joints_count; ++j) withAncestors(data.skins[si].joints[j]);
        if (animatedOnly)
            for (cgltf_size a = 0; a < data.animations_count; ++a)
                for (cgltf_size c = 0; c < data.animations[a].channels_count; ++c)
                    withAncestors(data.animations[a].channels[c].target_node);
        if (keep.empty()) return;
        if (keep.size() > kMaxSceneBones) {       // joints (and boneOf) would wrap; skinned meshes read static
            drop(Dropped::Kind::Skin, Dropped::Effect::Wrong, 1,
                 std::to_string(keep.size()) + " bones: an imported skeleton holds at most " + std::to_string(kMaxSceneBones));
            return;
        }

        Skeleton sk;
        std::set<std::string> names;
        const std::vector<Float4x4> world = worldsOf(out.nodes, &Node::local);   // once, not per bone
        for (size_t ni = 1; ni < out.nodes.size(); ++ni) {        // node 0 is the synthetic root
            const cgltf_node* gn = gltfOf[ni];
            if (!keep.count(gn)) continue;
            Bone b;
            b.name = out.nodes[ni].name;
            if (!names.insert(b.name).second) b.name += " #" + std::to_string(ni);   // clips bind by name
            b.parent = -1;
            for (const cgltf_node* p = gn->parent; p; p = p->parent)
                if (auto it = boneOf.find(p); it != boneOf.end()) { b.parent = it->second; break; }
            b.bindLocal = out.nodes[ni].local;
            b.inverseBind = inverse(world[ni]);
            boneOf[gn] = (uint16_t)sk.bones.size();
            sk.bones.push_back(std::move(b));
        }
        for (cgltf_size si = 0; si < data.skins_count; ++si) {    // the skin's own bind matrices
            const cgltf_skin& skin = data.skins[si];
            if (!skin.inverse_bind_matrices) continue;             // absent means identity (spec)
            // One MAT4 per joint. cgltf_validate checks neither, and
            // cgltf_accessor_read_float does not bounds-check the index, so a
            // short accessor was a heap overflow (found by
            // fuzz_import_frontend_test under ASan). Without its bind matrices
            // the skin is wrong, not less: refused, the bones left at identity.
            const cgltf_accessor& ibm = *skin.inverse_bind_matrices;
            if (ibm.type != cgltf_type_mat4 || ibm.count < skin.joints_count) {
                drop(Dropped::Kind::Skin, Dropped::Effect::Wrong, 1,
                     "skin " + std::to_string(si) + ": inverseBindMatrices holds " + std::to_string(ibm.count) +
                     (ibm.type == cgltf_type_mat4 ? " matrices" : " non-MAT4 elements") + " for " +
                     std::to_string(skin.joints_count) + " joints");
                continue;
            }
            for (cgltf_size j = 0; j < skin.joints_count; ++j)
                cgltf_accessor_read_float(skin.inverse_bind_matrices, j,
                                          sk.bones[boneOf.at(skin.joints[j])].inverseBind.m, 16);
        }
        for (cgltf_size si = 0; si < data.skins_count; ++si)       // spec default when IBMs are absent
            if (!data.skins[si].inverse_bind_matrices)
                for (cgltf_size j = 0; j < data.skins[si].joints_count; ++j)
                    sk.bones[boneOf.at(data.skins[si].joints[j])].inverseBind = Float4x4{};
        out.skeleton = std::move(sk);
    }

    // ── Meshes, attached to the nodes that place them ────────────────────────
    // A skinned mesh's own node transform is IGNORED (glTF spec): its vertices
    // live in the space the skin was bound in, which is a joint's bind world
    // times that joint's inverse bind. It hangs from a node with exactly that
    // transform (the root itself when it is identity), so the scene means what
    // the file means; the back end cooks skinned meshes in their own space.
    void meshes() {
        const size_t fileNodes = out.nodes.size();       // bind-space nodes are appended below
        for (size_t ni = 1; ni < fileNodes; ++ni) {
            const cgltf_node* gn = gltfOf[ni];
            if (!gn->mesh) continue;
            // A skin with "joints": [] is invalid glTF, but cgltf and
            // cgltf_validate both accept it. Its weights can name no joint, and
            // reading them bound every vertex to joints[0] of an empty array (a
            // crash). The mesh is read static and the loss is Wrong: a skinned
            // mesh without its skeleton is not a lesser asset but an incorrect one.
            if (gn->skin && gn->skin->joints_count == 0) ++emptySkins;
            const cgltf_skin* skin = gn->skin && gn->skin->joints_count && out.skeleton ? gn->skin : nullptr;
            const std::vector<uint32_t> ids = meshesOf(gn->mesh, skin);   // copy: meshesOf may grow the map
            if (!skin) { out.nodes[ni].meshes = ids; continue; }
            const uint16_t j0 = boneOf.at(skin->joints[0]);
            const Float4x4 bindSpace = mul(worldOf(out.skeleton->bones, j0, &Bone::bindLocal),
                                           out.skeleton->bones[j0].inverseBind);
            if (nearIdentity(bindSpace, 1e-5f)) {
                for (uint32_t id : ids) out.nodes[0].meshes.push_back(id);
            } else {
                Node place; place.name = out.nodes[ni].name + " (bind space)"; place.parent = 0;
                place.local = bindSpace; place.meshes = ids;
                out.nodes.push_back(std::move(place));
                gltfOf.push_back(nullptr);
            }
        }
    }

    // ── Clips ────────────────────────────────────────────────────────────────
    // Translation, rotation and scale channels on bones. Morph-weight channels,
    // channels on nodes that are not bones, and interpolation the engine does
    // not play (CUBICSPLINE: its values are kept, its tangents are not; STEP:
    // keyed values, played linearly) are each reported, never silently lost.
    void clips(const std::string& stem) {
        uint32_t weightChannels = 0, strayChannels = 0, splineChannels = 0, stepChannels = 0;
        for (cgltf_size a = 0; a < data.animations_count; ++a) {
            const cgltf_animation& ga = data.animations[a];
            Clip c;
            c.name = clipDisplayName(ga.name ? ga.name : "", stem, (unsigned)a, (unsigned)data.animations_count);
            std::map<std::string, Track> tracks;
            for (cgltf_size ci = 0; ci < ga.channels_count; ++ci) {
                const cgltf_animation_channel& ch = ga.channels[ci];
                if (ch.target_path == cgltf_animation_path_type_weights) { ++weightChannels; continue; }
                auto bone = boneOf.find(ch.target_node);
                if (bone == boneOf.end()) { ++strayChannels; continue; }
                const cgltf_animation_sampler& sm = *ch.sampler;
                const bool spline = sm.interpolation == cgltf_interpolation_type_cubic_spline;
                if (spline) ++splineChannels;
                if (sm.interpolation == cgltf_interpolation_type_step) ++stepChannels;
                Track& tr = tracks[out.skeleton->bones[bone->second].name];
                tr.bone = out.skeleton->bones[bone->second].name;
                for (cgltf_size k = 0; k < sm.input->count; ++k) {
                    float t = 0; cgltf_accessor_read_float(sm.input, k, &t, 1);
                    c.duration = std::max(c.duration, t);
                    const cgltf_size at = spline ? k * 3 + 1 : k;  // spline outputs: in-tangent, value, out-tangent
                    float v[4] = {0, 0, 0, 1};
                    if (ch.target_path == cgltf_animation_path_type_rotation) {
                        cgltf_accessor_read_float(sm.output, at, v, 4);
                        Quat q{v[0], v[1], v[2], v[3]};                           // source convention, not conjugated
                        normalizeRotation(q);                                     // a bad key stays bad: checkScene names it
                        tr.rotation.push_back({t, q});
                    } else {
                        cgltf_accessor_read_float(sm.output, at, v, 3);
                        (ch.target_path == cgltf_animation_path_type_translation ? tr.translation : tr.scale)
                            .push_back({t, {v[0], v[1], v[2]}});
                    }
                }
            }
            for (auto& [name, tr] : tracks) c.tracks.push_back(std::move(tr));
            c.duration = std::max(c.duration, 1e-4f);
            if (!c.tracks.empty()) out.clips.push_back(std::move(c));
        }
        if (weightChannels) drop(Dropped::Kind::MorphTargets, Dropped::Effect::Less, weightChannels,
                                 std::to_string(weightChannels) + " morph-weight animation channel(s)");
        if (strayChannels) drop(Dropped::Kind::Animation, Dropped::Effect::Less, strayChannels,
                                std::to_string(strayChannels) + " animation channel(s) on nodes that are not bones");
        if (splineChannels) drop(Dropped::Kind::Animation, Dropped::Effect::Less, splineChannels,
                                 std::to_string(splineChannels) + " cubic-spline channel(s): keyed values kept, tangents dropped");
        if (stepChannels) drop(Dropped::Kind::Animation, Dropped::Effect::Less, stepChannels,
                               std::to_string(stepChannels) + " step-interpolated channel(s), played linearly");
    }
};

// Extensions this reader handles by reading the file as cgltf presents it.
bool understood(const char* ext) {
    return std::strcmp(ext, "KHR_mesh_quantization") == 0      // cgltf_accessor_read_float de-quantises
        || std::strcmp(ext, "KHR_lights_punctual") == 0;       // reported as Light, below
}

}  // namespace

ImportResult CgltfFrontend::importScene(const std::filesystem::path& source, const ImportOptions&) const {
    return guardedImport(source, "cgltf", [&]() -> ImportResult {
        const std::string src = source.string();
        cgltf_options options{};
        cgltf_data* data = nullptr;
        if (cgltf_parse_file(&options, src.c_str(), &data) != cgltf_result_success)
            return ImportError{ImportError::Kind::Unreadable, "not a readable glTF: " + src};
        struct Guard { cgltf_data* d; ~Guard() { cgltf_free(d); } } guard{data};
        if (cgltf_load_buffers(&options, data, src.c_str()) != cgltf_result_success)
            return ImportError{ImportError::Kind::Unreadable, "glTF buffers could not be loaded: " + src};
        // Alignment, which the spec requires and cgltf_validate does not check: every
        // accessor starts, and every element strides, on a multiple of its component
        // size. cgltf reads through typed pointers, so a misaligned one is undefined
        // behaviour (UBSan, found by fuzz_import_frontend_test), however well ARM64 and
        // x86 happen to tolerate it. Checked BEFORE cgltf_validate, which itself reads
        // index buffers through typed pointers to check their bounds.
        for (cgltf_size i = 0; i < data->accessors_count; ++i) {
            const cgltf_accessor& a = data->accessors[i];
            const cgltf_size c = cgltf_component_size(a.component_type);
            auto misaligned = [](const cgltf_buffer_view* v, cgltf_size offset, cgltf_size size) {
                return v && size > 1 && (v->offset + offset) % size != 0;
            };
            // The dense data (absent: all zeros), then a sparse accessor's own
            // index and value views, which cgltf reads the same way.
            bool bad = misaligned(a.buffer_view, a.offset, c) || (a.buffer_view && c > 1 && a.stride % c != 0);
            if (a.is_sparse)
                bad |= misaligned(a.sparse.indices_buffer_view, a.sparse.indices_byte_offset,
                                  cgltf_component_size(a.sparse.indices_component_type)) ||
                       misaligned(a.sparse.values_buffer_view, a.sparse.values_byte_offset, c);
            if (bad)
                return ImportError{ImportError::Kind::Unreadable,
                                   "invalid glTF: accessor " + std::to_string(i) + " is not aligned to its " +
                                   std::to_string(c) + "-byte components: " + src};
        }
        // Structural validation (accessor ranges, index bounds) before reading a
        // byte: this is hostile input. The old cook path skipped it.
        if (cgltf_validate(data) != cgltf_result_success)
            return ImportError{ImportError::Kind::Unreadable, "invalid glTF (failed validation): " + src};

        ImportedScene scene;
        scene.source = src;
        Reader r{*data, source.parent_path(), scene};

        // ── What the file has that this front end does not carry ────────────────
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
        r.gltfOf.push_back(nullptr);                         // node 0 is ours, not the file's
        if (gs)
            for (cgltf_size i = 0; i < gs->nodes_count; ++i) r.node(gs->nodes[i], 0);

        // Skins make a skeleton; so do the clips of an animation-only file. Node
        // animation on a static model has no skeleton to play on, and is reported.
        const bool animationOnly = data->meshes_count == 0 && data->animations_count > 0;
        r.skeleton(animationOnly);
        r.meshes();
        if (scene.skeleton) r.clips(source.stem().string());
        else if (data->animations_count)
            r.drop(Dropped::Kind::Animation, Dropped::Effect::Less, (uint32_t)data->animations_count,
                   std::to_string(data->animations_count) + " node animation(s) on a model with no skin");
        if (r.unweighted) r.drop(Dropped::Kind::Skin, Dropped::Effect::Less, r.unweighted,
                                 std::to_string(r.unweighted) + " skinned vertex(es) had no bone influence; bound to the root bone");
        if (r.extraInfluences) r.drop(Dropped::Kind::Skin, Dropped::Effect::Less, r.extraInfluences,
                                      "JOINTS_1 on " + std::to_string(r.extraInfluences) + " primitive(s): only the first 4 influences are kept");

        if (r.nonTriangles) r.drop(Dropped::Kind::NonTriangles, Dropped::Effect::Less, r.nonTriangles,
                                   std::to_string(r.nonTriangles) + " point/line primitive(s)");
        if (r.emptySkins) r.drop(Dropped::Kind::Skin, Dropped::Effect::Wrong, r.emptySkins,
                                 std::to_string(r.emptySkins) + " mesh node(s) on a skin with no joints");
        if (r.noPositions) r.drop(Dropped::Kind::NonTriangles, Dropped::Effect::Less, r.noPositions,
                                  std::to_string(r.noPositions) + " primitive(s) with no POSITION");
        if (r.vertexColours) r.drop(Dropped::Kind::VertexColours, Dropped::Effect::Less, r.vertexColours,
                                    "COLOR_0 on " + std::to_string(r.vertexColours) + " primitive(s)");
        if (r.extraUvSets) r.drop(Dropped::Kind::ExtraUvSets, Dropped::Effect::Less, r.extraUvSets,
                                  "TEXCOORD_1+ on " + std::to_string(r.extraUvSets) + " primitive(s)");
        for (const std::string& t : r.droppedTextures)
            r.drop(Dropped::Kind::Texture, Dropped::Effect::Less, 1, "texture " + t);

        // ── Nothing to import is its own answer (the contract's Empty) ──────────
        // A file of clips alone is not empty: it is the clip cooker's input (WO-016:
        // assets/cookers/clip/),
        // and the mesh back end skips it.
        if (scene.meshes.empty() && scene.clips.empty())
            return ImportError{ImportError::Kind::Empty, "nothing to import: " + src};
        return scene;
    });
}

}  // namespace imp
