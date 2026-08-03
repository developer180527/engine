#!/usr/bin/env python3
"""Generate a render stress project: N objects, no assets, no cook.

WHY THIS EXISTS. Every scalability question about the renderer — instancing,
the flat-render-packet redesign, the shadow pass's missing cull — is currently
undecidable, because the only scene in the repo submits 12 draws. Four separate
findings this month were rated High from reading the code and then measured as
nil, worse, or irrelevant. The way to stop guessing is a scene where the numbers
are large enough to mean something.

Deliberately uses `engine://primitive/cube` for every object, so:
  * no asset files, no cook, no DDC — the scene loads instantly at any N;
  * it is fully deterministic, so two runs are comparable;
  * every object shares one mesh and one material, which is the BEST case for
    batching. `draws` vs `batchRuns` from the submit seam then reports the
    instancing CEILING: with one batch run for N draws, instancing would remove
    N-1 submits. Real content sits below that ceiling; this measures the top.

The camera looks level down -Z from the grid's near edge, so part of the grid is
always outside the frustum and the cull ratio is non-trivial — a stress scene where
everything is visible cannot show culling working, and one where nothing is cannot
show submission.

Usage:
    scripts/gen_stress_scene.py <out-dir> --objects 4000 [--shadows]
    ./build/engine_host <out-dir> --frames 300

--shadows makes the directional light a shadow caster, which is the switch that
exposes the shadow pass walking EVERY item instead of a culled set: with it on,
`shadowDraws` should track scene size while `draws` stays bounded by the frustum.
"""
import argparse, json, math, os

def cube(eid, name, pos, scale):
    return {
        "id": eid,
        "name": name,
        "meshRenderer": {"path": "engine://primitive/cube",
                         "sourceType": "primitive"},
        "transform": {"position": list(pos),
                      "rotation": [0.0, 0.0, 0.0, 1.0],
                      "scale": list(scale)},
    }

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--objects", type=int, default=4000)
    ap.add_argument("--shadows", action="store_true",
                    help="directional light casts shadows (exposes the shadow "
                         "pass's missing frustum cull)")
    a = ap.parse_args()

    ents = [
        # Primary camera. Pulled back and up so roughly half the grid falls
        # outside the frustum — culling has to do real work to be observable.
        {"id": 1, "name": "Camera",
         "camera": {"clearColor": [0.1, 0.1, 0.12, 1.0], "farPlane": 1000.0,
                    "fov": 60.0, "isPrimary": True, "nearPlane": 0.1,
                    "orthoSize": 10.0, "projection": 0},
         # IDENTITY rotation, deliberately: looking straight down -Z, level, from
         # the grid's near edge. An earlier version used a pitched-down quaternion
         # and the camera ended up facing away from the scene — the cull then
         # reported 2000 of 2001 items rejected, which reads exactly like an
         # over-culling bug in the renderer and is not one. A stress scene whose
         # camera orientation you have to reason about is a stress scene that will
         # mislead you.
         "transform": {"position": [0.0, 8.0, 100.0],
                       "rotation": [0.0, 0.0, 0.0, 1.0],
                       "scale": [1.0, 1.0, 1.0]}},
        {"id": 2, "name": "Sun",
         "light": {"castShadows": bool(a.shadows), "color": [1.0, 1.0, 1.0],
                   "intensity": 3.0, "range": 15.0, "spotInner": 25.0,
                   "spotOuter": 35.0, "temperatureK": 6500.0, "type": 0,
                   "useTemperature": False},
         "transform": {"position": [0.0, 60.0, 0.0],
                       "rotation": [-0.4226, 0.0, 0.0, 0.9063],
                       "scale": [1.0, 1.0, 1.0]}},
        cube(100, "Ground", (0.0, -0.5, 0.0), (400.0, 1.0, 400.0)),
    ]

    # Square grid, 4 units apart, centred on the origin, so larger N extends PAST
    # the frustum and the cull has real work at every size. Height varies so the
    # scene is not a flat plane of coincident depths (the sort key quantises depth
    # to 24 bits, and identical depths would make the ordering degenerate).
    side = max(1, int(math.ceil(math.sqrt(a.objects))))
    spacing, eid, made = 4.0, 1000, 0
    for gz in range(side):
        for gx in range(side):
            if made >= a.objects:
                break
            x = (gx - side / 2.0) * spacing
            z = (gz - side / 2.0) * spacing
            h = 1.0 + ((gx * 7 + gz * 13) % 9) * 0.5
            ents.append(cube(eid, f"Obj_{made}", (x, h * 0.5, z),
                             (1.5, h, 1.5)))
            eid += 1
            made += 1

    scenes = os.path.join(a.out, "assets", "scenes")
    os.makedirs(scenes, exist_ok=True)
    with open(os.path.join(scenes, "stress.scene"), "w") as f:
        json.dump({"entities": ents, "version": 1}, f, indent=1)
    with open(os.path.join(a.out, "project.json"), "w") as f:
        json.dump({"assetRoot": "assets", "engine": "0.1.0",
                   "lastScene": "assets/scenes/stress.scene",
                   "name": "render_stress", "template": "basic3d",
                   "version": 2}, f, indent=1)

    print(f"wrote {a.out}: {made} objects ({side}x{side} grid), "
          f"shadows={'on' if a.shadows else 'off'}")

if __name__ == "__main__":
    main()
