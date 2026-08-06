#!/usr/bin/env python3
"""Layered, seeded fuzz scene over REAL COOKED ASSETS.

WHY THIS EXISTS, AND WHY IT IS NOT gen_stress_scene.py. That generator spawns N
copies of `engine://primitive/cube`: one mesh, one material, no submeshes, static.
Every renderer number measured against it is therefore a BEST CASE — one batch run,
one instanced draw, nothing to re-extract. Real content breaks all four of those
assumptions at once, and the pipeline has branches that only real content reaches:

  * a mesh with SUBMESHES is excluded from instancing by construction
    (ForwardPipeline: `it.mesh->submeshes.empty()`), so it costs one draw per
    material group, per instance;
  * a data-driven material supplies its own program and is likewise excluded;
  * MOVING entities defeat any future incremental-extraction scheme and force the
    PrevTransform snapshot to do real work every step;
  * a PARENTED entity with non-uniform parent scale is the one case where the
    bounding sphere used to under-estimate (world/issues.md A1.1).

The Medieval Village MegaKit is 176 models with 1-4 material groups each (80 have 1,
71 have 2, 23 have 3, 2 have 4 — ~1.7 average), already cooked. That is the shape of
real content, so it is what the renderer should be measured against.

SIX LAYERS, all driven from ONE seed so a finding is reproducible by seed alone:

  L0  ASSETS    which cooked meshes, drawn from the source project's registry
  L1  PLACEMENT position, rotation, and NON-UNIFORM scale
  L2  MOTION    a fraction get Spinner, so transforms change every frame
  L3  HIERARCHY a fraction are parented, so the ancestor walk and nested-scale
                bounding spheres are exercised
  L4  CAMERA    placed so a tunable fraction of the scene starts outside the frustum
  L5  LOD       a fraction get an LOD chain (--lods), so the selection path, the
                mesh swap and the sort-key repair are exercised at scene scale

NON-DESTRUCTIVE. The generated project borrows the source project's cooked cache:
the big directories are symlinked, and registry.db is COPIED so the engine's scan
can never write to the real one. Nothing is cooked and nothing is modified.

Usage:
    scripts/gen_fuzz_scene.py --src fps_shooter --out /tmp/fz --objects 50000 --seed 1
    ./build/engine_host /tmp/fz --frames 300
"""
import argparse, json, math, os, random, shutil, sqlite3, sys

ASSET_TYPE_MESH = 1
ASSET_STATE_COOKED = 1


def load_meshes(src, fmt, subdir_filter):
    """L0: every cooked mesh in the source project, one row per model."""
    db_path = os.path.join(src, ".cache", "registry.db")
    if not os.path.exists(db_path):
        sys.exit(f"no asset registry at {db_path} — is the project cooked?")
    db = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    q = ("SELECT uuid, source_path, cooked_path FROM assets "
         "WHERE type=? AND state=?")
    rows = list(db.execute(q, (ASSET_TYPE_MESH, ASSET_STATE_COOKED)))
    db.close()
    if subdir_filter:
        rows = [r for r in rows if subdir_filter.lower() in r[1].lower()]
    # One format only. The kit ships OBJ, FBX and glTF of the SAME 176 models, and
    # loading all three would triple VRAM while testing nothing extra.
    if fmt:
        rows = [r for r in rows if f"/{fmt}/" in r[1] or r[1].lower().endswith(fmt.lower())]
    if not rows:
        sys.exit("no cooked meshes matched — check --filter/--format against the registry")
    rows.sort(key=lambda r: r[1])          # deterministic order before seeding
    return rows


def link_cache(src, out):
    """Borrow the cooked cache without being able to damage it."""
    src_cache = os.path.join(src, ".cache")
    out_cache = os.path.join(out, ".cache")
    os.makedirs(out_cache, exist_ok=True)
    for name in os.listdir(src_cache):
        s, d = os.path.join(src_cache, name), os.path.join(out_cache, name)
        if os.path.lexists(d):
            continue
        if name.startswith("registry.db"):
            shutil.copy2(s, d)             # COPIED: the scan opens it read-write
        elif os.path.isdir(s):
            os.symlink(os.path.abspath(s), d)   # symlinked: 381 MB stays put
    return out_cache


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="fps_shooter", help="cooked source project")
    ap.add_argument("--out", required=True, help="scratch project to generate")
    ap.add_argument("--objects", type=int, default=50000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--format", default="OBJ",
                    help="which source format to draw from (OBJ/FBX/glTF); the kit "
                         "ships the same models in all three")
    ap.add_argument("--filter", default="MegaKit",
                    help="substring of the source path, to pick one kit")
    ap.add_argument("--movers", type=float, default=0.25,
                    help="L2: fraction with a Spinner (transforms change per frame)")
    ap.add_argument("--children", type=float, default=0.15,
                    help="L3: fraction parented to an earlier entity")
    ap.add_argument("--lods", type=float, default=0.0,
                    help="L5: fraction of entities given an LOD chain. The levels "
                         "are OTHER KIT MESHES, not decimated versions of the same "
                         "one — LOD generation is a cooker feature that does not "
                         "exist yet, and the renderer cannot tell the difference. "
                         "What this exercises is the whole selection path: the "
                         "optional query term, the mesh swap, and above all the "
                         "SORT KEY REPAIR, because a stale key makes the submit "
                         "loop draw an instanced run with the wrong mesh. The frame "
                         "looks like nonsense on purpose; the counters do not.")
    ap.add_argument("--nonuniform", type=float, default=0.30,
                    help="L1: fraction with non-uniform scale (the nested-scale "
                         "bounding-sphere case when combined with --children)")
    ap.add_argument("--shadows", action="store_true", default=True)
    ap.add_argument("--no-shadows", dest="shadows", action="store_false")
    ap.add_argument("--spread", type=float, default=None,
                    help="half-extent of the placement volume; default scales with "
                         "--objects so density stays comparable across sizes")
    a = ap.parse_args()

    rng = random.Random(a.seed)            # ONE seed drives every layer
    meshes = load_meshes(a.src, a.format, a.filter)

    spread = a.spread if a.spread is not None else max(60.0, 2.2 * math.sqrt(a.objects))

    ents = [
        # L4: the camera. Deliberately inside the volume looking out, so the cull
        # keeps a large fraction and rejects a large fraction — a scene that is
        # entirely visible cannot show culling, and one that is entirely culled
        # cannot show submission.
        {"id": 1, "name": "Camera",
         "camera": {"clearColor": [0.1, 0.1, 0.12, 1.0], "farPlane": 1000.0,
                    "fov": 60.0, "isPrimary": True, "nearPlane": 0.1,
                    "orthoSize": 10.0, "projection": 0},
         "transform": {"position": [0.0, 12.0, spread * 0.45],
                       "rotation": [0.0, 0.0, 0.0, 1.0],
                       "scale": [1.0, 1.0, 1.0]}},
        {"id": 2, "name": "Sun",
         "light": {"castShadows": bool(a.shadows), "color": [1.0, 1.0, 1.0],
                   "intensity": 3.0, "range": 60.0, "spotInner": 25.0,
                   "spotOuter": 35.0, "temperatureK": 6500.0, "type": 0,
                   "useTemperature": False},
         "transform": {"position": [0.0, 80.0, 0.0],
                       "rotation": [-0.4226, 0.0, 0.0, 0.9063],
                       "scale": [1.0, 1.0, 1.0]}},
    ]

    spawned = []                            # ids eligible to be parents (L3)
    next_id = 1000
    counts = {"movers": 0, "children": 0, "nonuniform": 0, "lods": 0}

    for i in range(a.objects):
        uuid, src_path, cooked = meshes[rng.randrange(len(meshes))]   # L0

        # L1: placement. Uniform in a box, so density is even and the frustum
        # slices it rather than catching a shell.
        pos = [rng.uniform(-spread, spread),
               rng.uniform(0.0, 24.0),
               rng.uniform(-spread, spread)]
        # A real quaternion, not identity: rotation is what makes a bounding sphere
        # non-trivial, and combined with non-uniform scale it is the A1.1 case.
        ax, ay, az = rng.gauss(0, 1), rng.gauss(0, 1), rng.gauss(0, 1)
        n = math.sqrt(ax*ax + ay*ay + az*az) or 1.0
        ang = rng.uniform(0.0, math.pi)
        s = math.sin(ang * 0.5)
        rot = [ax/n*s, ay/n*s, az/n*s, math.cos(ang * 0.5)]

        if rng.random() < a.nonuniform:
            scale = [rng.uniform(0.5, 3.0), rng.uniform(0.5, 3.0), rng.uniform(0.5, 3.0)]
            counts["nonuniform"] += 1
        else:
            u = rng.uniform(0.6, 1.8)
            scale = [u, u, u]

        e = {
            "id": next_id,
            "name": f"fz_{i}",
            "meshRenderer": {"asset": uuid, "cookedPath": cooked, "path": src_path},
            "transform": {"position": pos, "rotation": rot, "scale": scale},
        }

        # L5: LOD chain. Thresholds are deliberately GENEROUS (0.5 / 0.2 / 0.05
        # of the viewport height) rather than the component defaults, so that at
        # this scene's spread most entities land on a coarser level and the census
        # is not three zeros and a big L0 — a chain that never triggers proves
        # only that it compiles.
        if a.lods > 0.0 and rng.random() < a.lods:
            levels = [meshes[rng.randrange(len(meshes))]
                      for _ in range(rng.randint(1, 3))]
            e["lodMesh"] = {
                "levels": [{"asset": u, "cookedPath": c, "path": p}
                           for (u, p, c) in levels],
                "coarsenBelow": [0.5, 0.2, 0.05][:len(levels)],
            }
            counts["lods"] += 1

        # L2: motion. A Spinner rewrites this entity's rotation every frame, so
        # extraction and the PrevTransform snapshot cannot be skipped for it.
        if rng.random() < a.movers:
            e["spinner"] = {"speedYaw": rng.uniform(-2.0, 2.0),
                            "speedPitch": rng.uniform(-1.0, 1.0)}
            counts["movers"] += 1

        # L3: hierarchy. A child of an earlier entity, so extraction's parented
        # query and the ancestor walk are exercised — and when the parent has
        # non-uniform scale this is exactly the composed transform whose bounding
        # sphere used to be under-estimated.
        if spawned and rng.random() < a.children:
            # "parentId", NOT "parent" — SceneSerializer::restoreParents reads
            # parentId, and an unknown key is silently ignored, so the first
            # version of this generator emitted 7 550 "children" at 50 000 objects
            # that all loaded as roots. The hierarchy layer was inert and nothing
            # said so. Verified against src/scene/scene_serializer.h.
            e["parentId"] = rng.choice(spawned[-64:])   # recent: short chains
            # A child's position is parent-relative, so keep it small.
            e["transform"]["position"] = [rng.uniform(-3, 3), rng.uniform(0, 3),
                                          rng.uniform(-3, 3)]
            counts["children"] += 1

        ents.append(e)
        spawned.append(next_id)
        next_id += 1

    scenes_dir = os.path.join(a.out, "assets", "scenes")
    os.makedirs(scenes_dir, exist_ok=True)
    scene_rel = "assets/scenes/fuzz.scene"
    with open(os.path.join(a.out, scene_rel), "w") as f:
        json.dump({"entities": ents, "version": 1}, f)

    link_cache(a.src, a.out)

    with open(os.path.join(a.out, "project.json"), "w") as f:
        json.dump({"assetRoot": "assets", "engine": "0.1.0",
                   "lastScene": scene_rel, "name": "fuzz_scene",
                   "template": "basic3d", "version": 2}, f, indent=1)

    print(f"{a.out}: {a.objects} objects from {len(meshes)} cooked meshes "
          f"({a.format}, filter='{a.filter}')")
    print(f"  seed {a.seed}  spread +-{spread:.0f}  shadows={'on' if a.shadows else 'off'}")
    print(f"  movers {counts['movers']}  children {counts['children']}  "
          f"non-uniform scale {counts['nonuniform']}  lod chains {counts['lods']}")


if __name__ == "__main__":
    main()
