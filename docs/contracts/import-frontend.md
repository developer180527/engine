---
status: target
contract: import-frontend
kind: interface
state: planned
owner: src/assets
header:
implementations: []
---

# import-frontend — a source file in, the engine's ImportedScene out

One front end per parsing library (Assimp, cgltf; later USD, the FBX SDK, our
own). Each turns a source file into an `ImportedScene` in the engine's
conventions, plus a list of what it could not represent. One back end
(`cooker`) turns that into cooked assets. Design and rationale:
[`docs/plans/imported-scene.md`](../plans/imported-scene.md). Built by
WO-010 to WO-013.

```cpp
ImportResult importScene(const std::filesystem::path& source, const ImportOptions&);
// ImportResult: an ImportedScene (with its `dropped` list), or an ImportError.
```

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
