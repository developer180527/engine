// Quick diagnostic: parse an FBX with Assimp, print bone/mesh/anim info.
// Build:  clang++ -std=c++17 -I third_party/assimp/include
//         -L build/third_party/assimp/lib -lassimp tools/fbx_diag.cpp -o fbx_diag
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <cstdio>
#include <string>
#include <set>
#include <vector>
#include <functional>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("Usage: fbx_diag <file.fbx>\n"); return 1; }

    Assimp::Importer imp;
    // Same setting as the engine — don't create $AssimpFbx$ helper nodes
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

    int totalBones = 0;
    // Collect unique bone names
    std::set<std::string> boneNames;
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        std::printf("\nMesh[%u] \"%s\": verts=%u faces=%u bones=%u primitive=%u\n",
            m, mesh->mName.C_Str(), mesh->mNumVertices, mesh->mNumFaces,
            mesh->mNumBones, mesh->mPrimitiveTypes);
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            boneNames.insert(mesh->mBones[b]->mName.C_Str());
        }
        if (mesh->mNumBones > 0) totalBones = mesh->mNumBones;
    }

    std::printf("\nUnique bone names: %zu\n", boneNames.size());
    if (boneNames.size() > 0) {
        std::printf("Bones:\n");
        int i = 0;
        for (auto& n : boneNames)
            std::printf("  [%3d] %s\n", i++, n.c_str());
    }

    // Count skeleton nodes (same logic as extractSkeleton)
    struct NodeInfo { std::string name; int depth; };
    std::vector<NodeInfo> skelNodes;
    std::function<void(const aiNode*, int)> walkNodes;
    walkNodes = [&](const aiNode* node, int depth) {
        std::string name = node->mName.C_Str();
        bool isBone = boneNames.count(name) > 0;
        // Check descendants
        bool hasDescBone = false;
        if (!isBone) {
            std::vector<const aiNode*> stack;
            for (unsigned c = 0; c < node->mNumChildren; ++c)
                stack.push_back(node->mChildren[c]);
            while (!stack.empty()) {
                auto* n = stack.back(); stack.pop_back();
                if (boneNames.count(n->mName.C_Str())) { hasDescBone = true; break; }
                for (unsigned c = 0; c < n->mNumChildren; ++c)
                    stack.push_back(n->mChildren[c]);
            }
        }
        if (isBone || hasDescBone)
            skelNodes.push_back({name, depth});
        for (unsigned c = 0; c < node->mNumChildren; ++c)
            walkNodes(node->mChildren[c], depth + 1);
    };
    walkNodes(scene->mRootNode, 0);

    std::printf("\nSkeleton nodes (bones + ancestors): %zu\n", skelNodes.size());
    if (skelNodes.size() > 128) {
        std::printf("*** WARNING: %zu > 128 (kMaxBones) — BUFFER OVERFLOW! ***\n",
            skelNodes.size());
    }
    for (auto& sn : skelNodes)
        std::printf("  %*s%s\n", sn.depth * 2, "", sn.name.c_str());

    // Animations
    for (unsigned a = 0; a < scene->mNumAnimations; ++a) {
        const aiAnimation* anim = scene->mAnimations[a];
        std::printf("\nAnimation[%u] \"%s\": channels=%u duration=%.1f tps=%.1f\n",
            a, anim->mName.C_Str(), anim->mNumChannels,
            anim->mDuration, anim->mTicksPerSecond);
    }

    std::printf("\n=== DIAGNOSIS: ");
    if (skelNodes.size() > 128)
        std::printf("BONE COUNT %zu > kMaxBones(128) — will cause stack buffer overflow!\n",
            skelNodes.size());
    else if (skelNodes.size() == 0)
        std::printf("No bones detected — will load as static mesh.\n");
    else
        std::printf("OK — %zu bones, within kMaxBones limit.\n", skelNodes.size());

    return 0;
}
