# Asset Cooking Pipeline review


The primary future work is **not optimization** but improving scalability, dependency tracking, and build performance.

---

# Recommended Improvements


## 1. Parallel Cooking (Highest Priority)

Current:

Mesh A

↓

Mesh B

↓

Texture A

↓

Scene

Future:

Worker 1 → Mesh A

Worker 2 → Mesh B

Worker 3 → Texture A

Worker 4 → Texture B

↓

Merge Results

Because cookers are already stateless, they are naturally suited for a job system.

Priority:
★★★★★

---

# 2. Recursive Directory Scanning

Current:

directory_iterator

Future:

recursive_directory_iterator

Allows:

Assets/

Characters/

Weapons/

Vehicles/

Environment/

UI/

Automatically supported.

Priority:
★★★★☆

---

# 3. Dependency Graph

Current:

Mesh changed -> Recook every scene

Future:

Tree.fbx -> Forest.scene -> Only Forest.scene

Instead of rebuilding everything.

Store:

Asset UUID -> Dependent Assets -> Dirty propagation

Priority:
★★★★★

---

# 4. Incremental Filesystem Watching

Current:

Scan all assets every refresh.

Future:

Filesystem Watcher

↓

Changed Files Only

↓

Registry Update

↓

Cook Changed Assets

Much faster on large projects.

Priority:
★★★★★

---

# 5. Cancellation Support

Current:

Thread stops after current work finishes.

Future:

Each cooker periodically checks:

ShouldCancel()

Allows:

- instant shutdown
- cancel imports
- restart imports

Priority:
★★★★☆

---

# 6. Better Progress Reporting

Instead of:

43 / 120 assets cooked

Provide phases:

Scanning...

Cooking Meshes...

Cooking Textures...

Cooking Scenes...

Generating Registry...

Finished.

Much better editor UX.

Priority:
★★★☆☆

---

# 7. Structured Error Types

Current:

bool success

string error

Future:

enum class CookError
{
    FileMissing,
    UnsupportedFormat,
    InvalidMesh,
    InvalidTexture,
    InvalidScene,
    AssimpFailure,
    CorruptAsset,
    IOError
};

Advantages:

- Better UI
- Better logging
- Easier debugging

Priority:
★★★★☆

---

# 8. Hash-Based Staleness Detection

Current (likely):

Timestamp comparison

Future:

Store:

XXH3

or

SHA256

Advantages:

- Detect real file changes
- Avoid timestamp edge cases
- Better reproducibility

Priority:
★★★★☆

---

# 9. Reduce Temporary String Allocations

Many temporary strings are created during cooking.

Since cooking is offline, this is **not urgent**.

Optimize only after profiling.

Priority:
★★☆☆☆

---

# Mesh Cooker Future Refactor

As animation, materials, skeletons and LODs arrive, MeshCooker will become large.

Recommended split:

MeshCooker -> GeometryBuilder -> MaterialBuilder -> TextureResolver -> SkeletonBuilder -> AnimationBuilder -> MeshSerializer

Each component has a single responsibility.

Priority:
★★★★★ (future)

---

# Introduce a Shared CookContext

Eventually all cookers should receive:

CookContext

Containing:

- Project Root
- Output Root
- Asset Registry
- Logger
- Progress Reporter
- Cancellation Token
- Dependency Recorder
- Configuration

Advantages:

- Cleaner interfaces
- Easier testing
- Consistent APIs

Priority:
★★★★★

---

# Long-Term Architecture

Current: Scan -> Cook -> Recook Scenes

Future: Filesystem Watcher -> Dependency Graph -> Dirty Assets -> Parallel Job System -> Cook Outputs -> Registry Update -> Notify Editor

---

# Runtime Philosophy

Continue following this principle:

**Spend CPU offline so runtime stays simple.**

Runtime should never:

- Parse FBX
- Decode PNG
- Resolve asset references
- Search directories
- Build meshes

Instead: Runtime -> Read cooked binary -> Upload GPU resources -> Play

Exactly the philosophy used in modern game engines.

---

# Performance Philosophy

Do NOT micro-optimize cookers.

Instead focus on: Parallelism, Dependency tracking, Incremental builds, Background execution and Better scheduling

These improvements will save minutes of build time.

Replacing one division with multiplication never will.

---

# Immediate Roadmap

## Phase 1

- Recursive directory scanning
- Cancellation token
- Better progress reporting
- Reserve common vectors
- Structured error types

---

## Phase 2

- Dependency graph
- Filesystem watcher
- Parallel cooking
- Shared CookContext

---

## Phase 3

- Animation cooker
- Material cooker
- Shader cooker
- Prefab cooker
- Navigation mesh cooker
- Audio cooker

---

The remaining work is focused on scaling the pipeline—parallelism, dependency management, and incremental builds—rather than redesigning the architecture.