// Quick diagnostic: parse an FBX with Assimp, print bone/mesh/anim info,
// and verify bind-pose skin matrices are near-identity.
// Build:  clang++ -std=c++17 -I third_party/assimp/include
//         -I build/third_party/assimp/include
//         -L build/third_party/assimp/lib -lassimpd tools/fbx_diag.cpp -o fbx_diag
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <set>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>

// ── Minimal mat4 helpers (no bx dependency) ─────────────────────────────────
static void mat4Identity(float m[16]) {
    std::memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4Mul(float out[16], const float a[16], const float b[16]) {
    float tmp[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            tmp[r*4+c] = 0.0f;
            for (int k = 0; k < 4; ++k)
                tmp[r*4+c] += a[r*4+k] * b[k*4+c];
        }
    std::memcpy(out, tmp, 64);
}

static float mat4DistFromIdentity(const float m[16]) {
    float id[16]; mat4Identity(id);
    float sum = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float d = m[i] - id[i];
        sum += d * d;
    }
    return std::sqrt(sum);
}

static void printMat4(const char* label, const float m[16]) {
    std::printf("  %s:\n", label);
    for (int r = 0; r < 4; ++r)
        std::printf("    [%8.4f %8.4f %8.4f %8.4f]\n",
            m[r*4], m[r*4+1], m[r*4+2], m[r*4+3]);
}

// ── Convert Assimp mat4 to float[16] — SAME as engine's aiMat4ToFloat16 ────
// Transposes: Assimp columns → output rows (col-vec → row-vec convention)
static void aiMat4ToFloat16(const aiMatrix4x4& m, float out[16]) {
    out[ 0] = m.a1; out[ 1] = m.b1; out[ 2] = m.c1; out[ 3] = m.d1;
    out[ 4] = m.a2; out[ 5] = m.b2; out[ 6] = m.c2; out[ 7] = m.d2;
    out[ 8] = m.a3; out[ 9] = m.b3; out[10] = m.c3; out[11] = m.d3;
    out[12] = m.a4; out[13] = m.b4; out[14] = m.c4; out[15] = m.d4;
}

// ── Also keep a DIRECT (non-transposed) conversion for comparison ───────────
static void aiMat4ToFloat16Direct(const aiMatrix4x4& m, float out[16]) {
    out[ 0] = m.a1; out[ 1] = m.a2; out[ 2] = m.a3; out[ 3] = m.a4;
    out[ 4] = m.b1; out[ 5] = m.b2; out[ 6] = m.b3; out[ 7] = m.b4;
    out[ 8] = m.c1; out[ 9] = m.c2; out[10] = m.c3; out[11] = m.c4;
    out[12] = m.d1; out[13] = m.d2; out[14] = m.d3; out[15] = m.d4;
}

// ── Decompose aiMatrix4x4 → SQT, recompose as row-vector mat4 ──────────────
// Mimics: decomposeAiMatrix → BoneTransform::toMatrix (uses row-vector SRT)
static void aiDecomposeRecompose(const aiMatrix4x4& m, float out[16]) {
    aiVector3D pos, scl;
    aiQuaternion rot;
    m.Decompose(scl, rot, pos);

    // Rotation matrix from quaternion (row-major, row-vector convention)
    float x = rot.x, y = rot.y, z = rot.z, w = rot.w;
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, xz = x*z, yz = y*z;
    float wx = w*x, wy = w*y, wz = w*z;

    // bx::mtxFromQuaternion produces a row-major matrix
    float R[16];
    R[ 0] = 1-2*(yy+zz); R[ 1] =   2*(xy+wz); R[ 2] =   2*(xz-wy); R[ 3] = 0;
    R[ 4] =   2*(xy-wz); R[ 5] = 1-2*(xx+zz); R[ 6] =   2*(yz+wx); R[ 7] = 0;
    R[ 8] =   2*(xz+wy); R[ 9] =   2*(yz-wx); R[10] = 1-2*(xx+yy); R[11] = 0;
    R[12] = 0;           R[13] = 0;           R[14] = 0;           R[15] = 1;

    float S[16]; mat4Identity(S);
    S[0] = scl.x; S[5] = scl.y; S[10] = scl.z;

    float T[16]; mat4Identity(T);
    T[12] = pos.x; T[13] = pos.y; T[14] = pos.z;

    // BoneTransform::toMatrix: S * R * T (row-vector: scale, rotate, translate)
    float SR[16];
    mat4Mul(SR, S, R);
    mat4Mul(out, SR, T);
}

// ── Skeleton node info ──────────────────────────────────────────────────────
struct SkelNode {
    std::string name;
    int         parentIndex;
    aiMatrix4x4 localTransform;
};

static bool hasDescendantBone(const aiNode* node,
                              const std::unordered_set<std::string>& boneNames) {
    std::vector<const aiNode*> stack;
    for (unsigned c = 0; c < node->mNumChildren; ++c)
        stack.push_back(node->mChildren[c]);
    while (!stack.empty()) {
        const aiNode* n = stack.back(); stack.pop_back();
        if (boneNames.count(n->mName.C_Str())) return true;
        for (unsigned c = 0; c < n->mNumChildren; ++c)
            stack.push_back(n->mChildren[c]);
    }
    return false;
}

static void collectSkelNodes(const aiNode* node, int parentIdx,
                             const std::unordered_set<std::string>& boneNames,
                             std::vector<SkelNode>& out) {
    std::string name = node->mName.C_Str();
    bool isBone = boneNames.count(name) > 0;
    if (!isBone && !hasDescendantBone(node, boneNames)) return;
    int myIdx = (int)out.size();
    out.push_back({ name, parentIdx, node->mTransformation });
    for (unsigned c = 0; c < node->mNumChildren; ++c)
        collectSkelNodes(node->mChildren[c], myIdx, boneNames, out);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("Usage: fbx_diag <file.fbx>\n"); return 1; }

    Assimp::Importer imp;
    imp.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    const aiScene* scene = imp.ReadFile(argv[1],
        aiProcess_Triangulate | aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace | aiProcess_FlipUVs |
        aiProcess_JoinIdenticalVertices | aiProcess_SortByPType);

    if (!scene || !scene->mRootNode) {
        std::printf("FAIL: %s\n", imp.GetErrorString());
        return 1;
    }

    std::printf("=== %s ===\n", argv[1]);
    std::printf("Meshes: %u   Materials: %u   Animations: %u   Textures: %u\n",
        scene->mNumMeshes, scene->mNumMaterials,
        scene->mNumAnimations, scene->mNumTextures);

    // Collect unique bone names + offset matrices
    std::unordered_set<std::string> boneNameSet;
    std::unordered_map<std::string, aiMatrix4x4> ibmMap;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        std::printf("\nMesh[%u] \"%s\": verts=%u faces=%u bones=%u prim=%u\n",
            m, mesh->mName.C_Str(), mesh->mNumVertices, mesh->mNumFaces,
            mesh->mNumBones, mesh->mPrimitiveTypes);
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            boneNameSet.insert(bone->mName.C_Str());
            ibmMap[bone->mName.C_Str()] = bone->mOffsetMatrix;
        }
    }

    std::printf("\nUnique bones (from aiBone): %zu\n", boneNameSet.size());

    // ── Build skeleton (same as engine) ─────────────────────────────────────
    std::vector<SkelNode> nodes;
    collectSkelNodes(scene->mRootNode, -1, boneNameSet, nodes);
    std::printf("Skeleton nodes (bones + structural ancestors): %zu\n", nodes.size());

    // Print hierarchy
    for (int i = 0; i < (int)nodes.size(); ++i) {
        bool isBone = boneNameSet.count(nodes[i].name) > 0;
        std::printf("  [%3d] parent=%3d %s%s\n", i, nodes[i].parentIndex,
            nodes[i].name.c_str(), isBone ? " (BONE)" : " (structural)");
    }

    // ── Build bone name → skeleton index map ────────────────────────────────
    std::unordered_map<std::string, int> boneMap;
    for (int i = 0; i < (int)nodes.size(); ++i)
        boneMap[nodes[i].name] = i;

    // ── Compute bind-pose world matrices (row-vector convention) ────────────
    // Using the engine's approach: decompose → recompose → chain multiply
    const int n = (int)nodes.size();
    std::vector<float> worldMtx(n * 16);
    for (int i = 0; i < n; ++i) {
        float local[16];
        aiDecomposeRecompose(nodes[i].localTransform, local);
        float* world = &worldMtx[i * 16];
        if (nodes[i].parentIndex >= 0) {
            const float* parentWorld = &worldMtx[nodes[i].parentIndex * 16];
            mat4Mul(world, local, parentWorld);
        } else {
            std::memcpy(world, local, 64);
        }
    }

    // ── Compute skin matrices: IBM * World (row-vector convention) ──────────
    std::printf("\n=== BIND-POSE SKIN MATRIX VERIFICATION ===\n");
    std::printf("Expected: near-identity (dist < 0.01) for all bones with IBM\n\n");

    int okCount = 0, badCount = 0, missingIbm = 0;
    for (int i = 0; i < n; ++i) {
        auto it = ibmMap.find(nodes[i].name);
        if (it == ibmMap.end()) {
            // Structural node (not a bone) — no IBM, skip
            ++missingIbm;
            continue;
        }

        // Convert IBM using the SAME transpose as the engine
        float ibm[16];
        aiMat4ToFloat16(it->second, ibm);

        float* world = &worldMtx[i * 16];
        float skin[16];
        mat4Mul(skin, ibm, world);  // IBM * World — same as engine

        float dist = mat4DistFromIdentity(skin);

        if (dist > 0.01f) {
            ++badCount;
            std::printf("  BONE [%3d] %-35s dist=%.4f  *** BAD ***\n",
                i, nodes[i].name.c_str(), dist);
            if (badCount <= 3) {
                printMat4("skin (IBM * World)", skin);
                printMat4("world", world);
                printMat4("IBM (transposed)", ibm);
                // Also show what the direct (non-transposed) IBM gives
                float ibmDirect[16];
                aiMat4ToFloat16Direct(it->second, ibmDirect);
                float skinDirect[16];
                mat4Mul(skinDirect, ibmDirect, world);
                float distDirect = mat4DistFromIdentity(skinDirect);
                std::printf("  --> Direct (non-transposed) IBM * World dist=%.4f\n", distDirect);
                printMat4("skin (direct IBM * World)", skinDirect);
            }
        } else {
            ++okCount;
        }
    }

    std::printf("\n=== SUMMARY ===\n");
    std::printf("Bones OK (near-identity): %d\n", okCount);
    std::printf("Bones BAD (far from identity): %d\n", badCount);
    std::printf("Structural nodes (no IBM): %d\n", missingIbm);
    std::printf("Total skeleton nodes: %d\n", n);

    if (badCount > 0) {
        std::printf("\n*** DIAGNOSIS: Bind-pose skin matrices are WRONG.\n");
        std::printf("    The IBM transpose in aiMat4ToFloat16 may be incorrect,\n");
        std::printf("    or the world matrix chain doesn't match what Assimp expects.\n");
    } else if (okCount > 0) {
        std::printf("\n*** DIAGNOSIS: Bind-pose skin matrices are CORRECT (near-identity).\n");
        std::printf("    If rendering is still wrong, the issue is in animation playback,\n");
        std::printf("    shader, or vertex data (bone indices/weights).\n");
    }

    // ── Also verify: Assimp native IBM * Assimp native world ────────────────
    // Compute world matrices using Assimp's own math (column-vector, no transpose)
    std::printf("\n=== CROSS-CHECK: Assimp-native IBM * World ===\n");
    std::unordered_map<std::string, aiMatrix4x4> nativeWorldMap;
    std::function<void(const aiNode*, aiMatrix4x4)> computeNativeWorld;
    computeNativeWorld = [&](const aiNode* node, aiMatrix4x4 parentWorld) {
        aiMatrix4x4 world = parentWorld * node->mTransformation;
        nativeWorldMap[node->mName.C_Str()] = world;
        for (unsigned c = 0; c < node->mNumChildren; ++c)
            computeNativeWorld(node->mChildren[c], world);
    };
    aiMatrix4x4 identity;
    computeNativeWorld(scene->mRootNode, identity);

    int nativeOk = 0, nativeBad = 0;
    for (auto& [name, offset] : ibmMap) {
        auto wit = nativeWorldMap.find(name);
        if (wit == nativeWorldMap.end()) {
            std::printf("  %-35s NOT FOUND in node tree!\n", name.c_str());
            continue;
        }
        aiMatrix4x4 skin = offset * wit->second;
        // Check if near identity
        float dist = 0.0f;
        float vals[16] = {
            skin.a1, skin.a2, skin.a3, skin.a4,
            skin.b1, skin.b2, skin.b3, skin.b4,
            skin.c1, skin.c2, skin.c3, skin.c4,
            skin.d1, skin.d2, skin.d3, skin.d4
        };
        float id[16]; mat4Identity(id);
        for (int i = 0; i < 16; ++i) {
            float d = vals[i] - id[i];
            dist += d * d;
        }
        dist = std::sqrt(dist);
        if (dist > 0.01f) {
            ++nativeBad;
            if (nativeBad <= 3)
                std::printf("  %-35s dist=%.4f BAD\n", name.c_str(), dist);
        } else {
            ++nativeOk;
        }
    }
    std::printf("Native: OK=%d  BAD=%d\n", nativeOk, nativeBad);

    // ── Animations ──────────────────────────────────────────────────────────
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* anim = scene->mAnimations[a];
        double tps = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 24.0;
        std::printf("\nAnimation[%u] \"%s\": channels=%u duration=%.1f tps=%.1f\n",
            a, anim->mName.C_Str(), anim->mNumChannels,
            anim->mDuration, tps);
        // Check channel names match skeleton
        int matched = 0, unmatched = 0;
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            const std::string chName = anim->mChannels[c]->mNodeName.C_Str();
            if (boneMap.count(chName)) ++matched;
            else {
                ++unmatched;
                if (unmatched <= 5)
                    std::printf("  Channel \"%s\" — NOT in skeleton!\n", chName.c_str());
            }
        }
        std::printf("  Channels matched: %d  unmatched: %d\n", matched, unmatched);

        // ── Verify animated skin matrices at t=0 ────────────────────────────
        // Apply the first keyframe's SQT to the bind-pose locals (mimicking
        // the engine's sampleClip), recompute world + skin matrices.
        std::printf("\n=== ANIMATED SKIN MATRIX VERIFICATION (t=0, first keyframe) ===\n");

        // Start from bind-pose local transforms (decomposed → SQT)
        struct SQT { float px,py,pz; float qx,qy,qz,qw; float sx,sy,sz; };
        std::vector<SQT> locals(n);
        for (int i = 0; i < n; ++i) {
            aiVector3D pos, scl; aiQuaternion rot;
            nodes[i].localTransform.Decompose(scl, rot, pos);
            locals[i] = { pos.x, pos.y, pos.z,
                          rot.x, rot.y, rot.z, rot.w,
                          scl.x, scl.y, scl.z };
        }

        // Overwrite with animation channel first-key values (time=0)
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim* na = anim->mChannels[c];
            auto bit = boneMap.find(na->mNodeName.C_Str());
            if (bit == boneMap.end()) continue;
            int idx = bit->second;
            if (na->mNumPositionKeys > 0) {
                const auto& v = na->mPositionKeys[0].mValue;
                locals[idx].px = v.x; locals[idx].py = v.y; locals[idx].pz = v.z;
            }
            if (na->mNumRotationKeys > 0) {
                const auto& q = na->mRotationKeys[0].mValue;
                locals[idx].qx = q.x; locals[idx].qy = q.y;
                locals[idx].qz = q.z; locals[idx].qw = q.w;
            }
            if (na->mNumScalingKeys > 0) {
                const auto& s = na->mScalingKeys[0].mValue;
                locals[idx].sx = s.x; locals[idx].sy = s.y; locals[idx].sz = s.z;
            }
        }

        // Recompose local → mat4 (same as BoneTransform::toMatrix)
        auto sqtToMat4 = [](const SQT& t, float out[16]) {
            float x=t.qx, y=t.qy, z=t.qz, w=t.qw;
            float xx=x*x, yy=y*y, zz=z*z;
            float xy=x*y, xz=x*z, yz=y*z;
            float wx=w*x, wy=w*y, wz=w*z;
            float R[16];
            R[ 0]=1-2*(yy+zz); R[ 1]=2*(xy+wz);   R[ 2]=2*(xz-wy);   R[ 3]=0;
            R[ 4]=2*(xy-wz);   R[ 5]=1-2*(xx+zz); R[ 6]=2*(yz+wx);   R[ 7]=0;
            R[ 8]=2*(xz+wy);   R[ 9]=2*(yz-wx);   R[10]=1-2*(xx+yy); R[11]=0;
            R[12]=0;           R[13]=0;           R[14]=0;           R[15]=1;
            float S[16]; mat4Identity(S);
            S[0]=t.sx; S[5]=t.sy; S[10]=t.sz;
            float T[16]; mat4Identity(T);
            T[12]=t.px; T[13]=t.py; T[14]=t.pz;
            float SR[16]; mat4Mul(SR, S, R);
            mat4Mul(out, SR, T);
        };

        // Compute world matrices from animated locals
        std::vector<float> animWorld(n * 16);
        for (int i = 0; i < n; ++i) {
            float local[16];
            sqtToMat4(locals[i], local);
            float* world = &animWorld[i * 16];
            if (nodes[i].parentIndex >= 0) {
                const float* pw = &animWorld[nodes[i].parentIndex * 16];
                mat4Mul(world, local, pw);
            } else {
                std::memcpy(world, local, 64);
            }
        }

        // Compute animated skin matrices and check for huge values
        int animOk = 0, animBad = 0;
        float maxVal = 0.0f;
        for (int i = 0; i < n; ++i) {
            auto it = ibmMap.find(nodes[i].name);
            if (it == ibmMap.end()) continue;
            float ibm[16];
            aiMat4ToFloat16(it->second, ibm);
            float skin[16];
            mat4Mul(skin, ibm, &animWorld[i * 16]);
            // Check for unreasonable values
            float boneMax = 0.0f;
            bool hasNan = false;
            for (int k = 0; k < 16; ++k) {
                if (std::isnan(skin[k]) || std::isinf(skin[k])) hasNan = true;
                float av = std::fabs(skin[k]);
                if (av > boneMax) boneMax = av;
                if (av > maxVal) maxVal = av;
            }
            if (hasNan || boneMax > 100.0f) {
                ++animBad;
                if (animBad <= 5) {
                    std::printf("  BONE [%3d] %-35s maxVal=%.1f %s\n",
                        i, nodes[i].name.c_str(), boneMax,
                        hasNan ? "HAS NaN!" : "HUGE!");
                    printMat4("animated skin", skin);
                }
            } else {
                ++animOk;
            }
        }
        std::printf("Animated skin matrices: OK=%d  BAD=%d  maxAbsVal=%.1f\n",
            animOk, animBad, maxVal);

        // Print a few sample animation key values for the first bone with channels
        std::printf("\n=== SAMPLE KEY VALUES (first 3 channels) ===\n");
        int shown = 0;
        for (unsigned c = 0; c < anim->mNumChannels && shown < 3; ++c) {
            const aiNodeAnim* na = anim->mChannels[c];
            std::printf("  Channel \"%s\":\n", na->mNodeName.C_Str());
            if (na->mNumPositionKeys > 0) {
                const auto& v = na->mPositionKeys[0].mValue;
                std::printf("    Pos key[0]: (%.4f, %.4f, %.4f)\n", v.x, v.y, v.z);
            }
            if (na->mNumRotationKeys > 0) {
                const auto& q = na->mRotationKeys[0].mValue;
                std::printf("    Rot key[0]: (%.4f, %.4f, %.4f, %.4f) [xyzw]\n",
                    q.x, q.y, q.z, q.w);
            }
            if (na->mNumScalingKeys > 0) {
                const auto& s = na->mScalingKeys[0].mValue;
                std::printf("    Scl key[0]: (%.4f, %.4f, %.4f)\n", s.x, s.y, s.z);
            }
            ++shown;
        }

        // Also check: bind-pose SQT vs first-keyframe SQT for the same bone
        std::printf("\n=== BIND vs ANIM SQT COMPARISON (first 3 bones with channels) ===\n");
        shown = 0;
        for (unsigned c = 0; c < anim->mNumChannels && shown < 3; ++c) {
            const aiNodeAnim* na = anim->mChannels[c];
            auto bit = boneMap.find(na->mNodeName.C_Str());
            if (bit == boneMap.end()) continue;
            int idx = bit->second;
            // Bind pose (from node tree decomposition)
            aiVector3D bPos, bScl; aiQuaternion bRot;
            nodes[idx].localTransform.Decompose(bScl, bRot, bPos);
            std::printf("  Bone \"%s\" (idx=%d):\n", na->mNodeName.C_Str(), idx);
            std::printf("    Bind pos: (%.4f, %.4f, %.4f)\n", bPos.x, bPos.y, bPos.z);
            if (na->mNumPositionKeys > 0) {
                const auto& v = na->mPositionKeys[0].mValue;
                std::printf("    Anim pos: (%.4f, %.4f, %.4f)  delta=(%.4f, %.4f, %.4f)\n",
                    v.x, v.y, v.z, v.x-bPos.x, v.y-bPos.y, v.z-bPos.z);
            }
            std::printf("    Bind rot: (%.4f, %.4f, %.4f, %.4f)\n", bRot.x, bRot.y, bRot.z, bRot.w);
            if (na->mNumRotationKeys > 0) {
                const auto& q = na->mRotationKeys[0].mValue;
                std::printf("    Anim rot: (%.4f, %.4f, %.4f, %.4f)\n", q.x, q.y, q.z, q.w);
            }
            ++shown;
        }
    }

    std::printf("\n=== FINAL: ");
    if (nodes.size() > 128)
        std::printf("BONE COUNT %zu > kMaxBones(128)!\n", nodes.size());
    else if (nodes.size() == 0)
        std::printf("No bones detected.\n");
    else
        std::printf("OK — %zu bones, within kMaxBones limit.\n", nodes.size());

    return 0;
}
