#pragma once
#include <vector>
#include <cmath>
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include "render/vertex.h"
#include "render/asset_registry.h"
#include "render/mesh.h"

// ── PrimitiveLibrary ───────────────────────────────────────────────────────
// Generates procedural GPU meshes (Cube, Plane, Sphere) at startup and
// registers them with the AssetRegistry. Handles persist for the session.
// sourcePath = "engine://primitive/<name>" identifies them for serialization.
class PrimitiveLibrary {
public:
    void init(AssetRegistry& assets) {
        m_cube   = buildCube(assets);
        m_plane  = buildPlane(assets);
        m_sphere = buildSphere(assets);
    }
    bool       ready()  const { return m_cube.valid(); }
    MeshHandle cube()   const { return m_cube;   }
    MeshHandle plane()  const { return m_plane;  }
    MeshHandle sphere() const { return m_sphere; }

    MeshHandle byName(const std::string& n) const {
        if (n == "cube")   return m_cube;
        if (n == "plane")  return m_plane;
        if (n == "sphere") return m_sphere;
        return {};
    }

private:
    MeshHandle m_cube, m_plane, m_sphere;

    static Vertex vert(bx::Vec3 p, bx::Vec3 n, bx::Vec3 t, float u, float v) {
        Vertex x{};
        x.position[0]=p.x; x.position[1]=p.y; x.position[2]=p.z;
        x.normal[0]=n.x;   x.normal[1]=n.y;   x.normal[2]=n.z;
        x.tangent[0]=t.x;  x.tangent[1]=t.y;  x.tangent[2]=t.z; x.tangent[3]=1.f;
        x.uv[0]=u; x.uv[1]=v;
        return x;
    }

    static MeshHandle upload(AssetRegistry& assets,
                              std::vector<Vertex>&   verts,
                              std::vector<uint32_t>& idx,
                              const char* name) {
        auto* vm = bgfx::copy(verts.data(), (uint32_t)(verts.size() * sizeof(Vertex)));
        auto* im = bgfx::copy(idx.data(),   (uint32_t)(idx.size()   * sizeof(uint32_t)));
        bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vm, Vertex::layout());
        bgfx::IndexBufferHandle  ibh = bgfx::createIndexBuffer(im, BGFX_BUFFER_INDEX32);
        Mesh mesh(vbh, ibh, (uint32_t)idx.size());
        mesh.sourcePath  = std::string("engine://primitive/") + name;
        mesh.doubleSided = false;
        return assets.addMesh(std::move(mesh));
    }

    // ── Cube ──────────────────────────────────────────────────────────────
    static MeshHandle buildCube(AssetRegistry& assets) {
        std::vector<Vertex>   v;
        std::vector<uint32_t> idx;
        // Each face: 4 verts, 2 tris — proper normals + UVs + tangents
        auto face = [&](bx::Vec3 n, bx::Vec3 t,
                        bx::Vec3 p0, bx::Vec3 p1, bx::Vec3 p2, bx::Vec3 p3) {
            uint32_t b = (uint32_t)v.size();
            v.push_back(vert(p0,n,t,0,0)); v.push_back(vert(p1,n,t,1,0));
            v.push_back(vert(p2,n,t,1,1)); v.push_back(vert(p3,n,t,0,1));
            idx.insert(idx.end(),{b,b+1,b+2, b,b+2,b+3});
        };
        face({1,0,0},{0,0,1},  {1,-1,-1},{1, 1,-1},{1, 1,1},{1,-1,1});  // +X
        face({-1,0,0},{0,0,-1},{-1,-1,1},{-1,1, 1},{-1,1,-1},{-1,-1,-1}); // -X
        face({0,1,0},{1,0,0},  {-1,1,-1},{1, 1,-1},{1, 1,1},{-1,1, 1}); // +Y
        face({0,-1,0},{1,0,0}, {-1,-1,1},{1,-1, 1},{1,-1,-1},{-1,-1,-1}); // -Y
        face({0,0,1},{1,0,0},  {-1,-1,1},{1,-1, 1},{1, 1,1},{-1,1, 1}); // +Z
        face({0,0,-1},{-1,0,0},{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}); // -Z
        return upload(assets, v, idx, "cube");
    }

    // ── Plane (XZ, normal +Y) ─────────────────────────────────────────────
    static MeshHandle buildPlane(AssetRegistry& assets) {
        std::vector<Vertex> verts = {
            vert({-0.5f,0,-0.5f},{0,1,0},{1,0,0}, 0,0),
            vert({ 0.5f,0,-0.5f},{0,1,0},{1,0,0}, 1,0),
            vert({ 0.5f,0, 0.5f},{0,1,0},{1,0,0}, 1,1),
            vert({-0.5f,0, 0.5f},{0,1,0},{1,0,0}, 0,1),
        };
        std::vector<uint32_t> idx = {0,1,2, 0,2,3};
        return upload(assets, verts, idx, "plane");
    }

    // ── Sphere (UV sphere, stacks × slices) ───────────────────────────────
    static MeshHandle buildSphere(AssetRegistry& assets,
                                   int stacks=16, int slices=16) {
        std::vector<Vertex>   verts;
        std::vector<uint32_t> idx;
        constexpr float PI = 3.14159265358979f;
        for (int i = 0; i <= stacks; ++i) {
            float phi = PI * i / stacks;
            for (int j = 0; j <= slices; ++j) {
                float theta = 2.f * PI * j / slices;
                float x = sinf(phi)*cosf(theta);
                float y = cosf(phi);
                float z = sinf(phi)*sinf(theta);
                float tx = -sinf(theta), tz = cosf(theta);
                verts.push_back(vert({x,y,z},{x,y,z},{tx,0,tz},
                    (float)j/slices, (float)i/stacks));
            }
        }
        for (int i = 0; i < stacks; ++i)
            for (int j = 0; j < slices; ++j) {
                uint32_t a = i*(slices+1)+j, b = a+slices+1;
                idx.insert(idx.end(),{a,a+1,b, a+1,b+1,b});
            }
        return upload(assets, verts, idx, "sphere");
    }
};
