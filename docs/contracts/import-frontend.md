---
status: as-built
contract: import-frontend
kind: interface
state: provisional
owner: src/assets
header: src/assets/import/import_frontend.h
implementations:
  - real: src/assets/import/frontend_cgltf.h#CgltfFrontend
  - real: src/assets/import/frontend_assimp.h#AssimpFrontend
  - fake: src/assets/import/fake_frontend.h#FakeFrontend
tests:
  - tests/import_frontend_contract_test.cpp
  - tests/frontend_cgltf_test.cpp
  - tests/frontend_assimp_test.cpp
covers:
  - src/assets/import/
verified: 2026-09-30
---

# import-frontend — a source file in, the engine's ImportedScene out

One front end per parsing library (Assimp, cgltf; later USD, the FBX SDK, our
own). Each turns a source file into an `ImportedScene` in the engine's
conventions, plus a list of what it could not represent. One back end
(`cooker`) turns that into cooked assets. Design and rationale:
[`docs/plans/imported-scene.md`](../plans/imported-scene.md). Built by
WO-010 to WO-013.

```cpp
// src/assets/import/import_frontend.h
class IImportFrontend {
    virtual std::vector<std::string> extensions() const = 0;          // {"gltf", "glb"}
    virtual ImportResult importScene(const std::filesystem::path&, const ImportOptions&) const = 0;
};
// ImportResult: an ImportedScene (with its `dropped` list), or an ImportError.
// ImportFrontendRegistry routes by extension; with no match, it returns Unsupported.
```

**The contract test** is one suite every front end must pass, in
`tests/import_contract.h`. It has seven reference cases in engine conventions,
each compared by meaning (world-space triangles with winding preserved,
skeleton by bone name, clips, the dropped list) plus `imp::checkScene`'s
structural invariants. A front end's test supplies a source file per case; a
case its format cannot express is reported as skipped, never counted as passed.
`import_frontend_contract_test` shows the suite passing the fake and failing
front ends broken the ways real parsers break.

## Nothing
- **An extension no front end handles** is a stub answer: `ImportError::Unsupported`
  naming the extension, logged once per extension. Never an empty scene, because
  an empty scene cannot be told apart from an empty file.
- **A file with nothing importable** (no triangles and no clips) is
  `ImportError::Empty`, which is a different answer from Unsupported.
- **Part of a file the front end cannot represent** is not an error. The scene
  is returned, and every loss is an entry in `dropped` with an effect of
  `Wrong` (the back end refuses) or `Less` (it cooks and logs). Leaving
  something out *without* an entry is a contract violation, and the contract
  suite tests for it.
- **The fake** (WO-010) builds a scene in memory from a description. Until the
  real front ends are ported, it is the implementation the back end is
  developed and tested against.

## Ownership
The result owns everything by value. The library's own memory (`aiScene`,
`cgltf_data`) is freed before `importScene` returns, and no pointer into it
survives. That is what makes "library types never leave a front end" checkable
(audit rule, WO-013). Embedded texture bytes are copied into `TextureRef::embedded`.

## Threading
Reentrant: every call builds its own importer (`Assimp::Importer`,
`cgltf_data`) and touches no global state, so cook workers call it concurrently.
The Assimp front end's resident-import memory permit (`AssimpGatePass`) is
acquired inside the call. Whether it stays there is imported-scene.md open
question 5.

## Timing
**Synchronous, deliberately.** It runs inside cook workers, which are already
off the main thread, and the runtime never imports: after WO-018 a missing
cooked asset is a cook *job* that the runtime waits on, not an import it
performs. An asynchronous import API would invite the runtime to call it.

## Errors
Returned, never thrown across the boundary. `ImportError` carries a kind
(`Unsupported`, `Unreadable` for an I/O or parse failure, `Empty`) and a
message with the path. A front end does not log failures itself: the caller
has the context, such as the asset, the cook and the project, and logs once.
Losses are data (`dropped`), not log lines.

"Never thrown" is enforced, not hoped for: every front end's `importScene` runs
inside `imp::guardedImport`, which turns any exception into `Unreadable` naming
the file and the reason. A front end reads hostile input, and one bug in it
must refuse that file, not terminate the cook worker and every cook queued
behind it. The input is fuzzed (`fuzz_import_frontend_test`, under ASan and
UBSan too); what cgltf's own validator does not check, the front end does:
inverse-bind matrices one MAT4 per joint, accessors aligned to their
components, skins with at least one joint.

Depth is input too. An exception boundary cannot catch a stack overflow, so
nothing a front end runs may recurse once per level of the file's node tree:
both front ends and the Assimp skeleton extraction walk it with explicit
stacks, and a 10,000-deep chain imports on a 512 KB stack (a macOS secondary
thread's size), `frontend_{cgltf,assimp}_test` §4. Assimp's own readers and
destructors DO recurse per level, and that is not ours to rewrite: the Assimp
front end runs every import on a thread with a 256 MB reserved stack
(`core/thread_stack.h`), and a file deeper even than that kills only the
isolated `engine_cook_worker` it runs in (WO-039).
