---
status: as-built
verified: 2026-11-26
covers:
  - cmake/EngineProviders.cmake
  - cmake/platforms/
tests:
  - tests/provider_manifests/CMakeLists.txt
  - tests/providers_test.cpp
---
# The provider manifest and the platform tables

**Each provider is declared once, in `<kind>-<name>.provider.json` beside its
source, and built only where it can run** (WO-085, DR-0037). CMake reads the
manifest; so will `engine_build` and the editor (WO-086). This page lists the
fields and what happens when one is wrong.

## The manifest

```json
{
  "kind": "physics",
  "name": "jolt",
  "contract": "engine-plugin",
  "platforms": ["windows", "linux", "macos"],
  "flavours": ["client", "server"],
  "needs": [],
  "sdk": [],
  "requires": ["engine.jobs.v1"],
  "capabilities": ["rigid_bodies", "character", "determinism.same_binary"]
}
```

| Field | Required | Meaning | Wrong |
|---|---|---|---|
| `kind` | yes | `physics`, `scripting`, `audio` or `renderer` | configure error, names the kinds |
| `name` | yes | what `project.json` chooses it by; the file must be named `<kind>-<name>.provider.json` | configure error, names the expected file name |
| `contract` | yes | the contract it implements, `docs/contracts/<contract>.md` | configure error if that file does not exist |
| `platforms` | yes | **an allow list** of platforms from `cmake/platforms/`. There are no deny lists: a new platform is supported only once someone adds it here | an unknown platform is a configure error; an empty list is a configure error |
| `flavours` | no | `client`, `server`; default both | an unknown flavour is a configure error |
| `needs` | no | platform facts that must be true (below) | a fact no table states is a configure error |
| `sdk` | no | CMake variables that must name an existing path, such as `WWISE_SDK` | not an error: the provider is **not built**, and the reason says `configure with -DWWISE_SDK=<path>` |
| `requires` | no | engine services it uses. Services, never other providers | recorded; checked by the resolver (WO-086) |
| `capabilities` | no | what it can do, for the editor and gameplay queries | recorded |
| `differences` | no | a document listing how it behaves unlike the kind's reference provider | recorded |

## Declaring it in CMake

```cmake
engine_provider(MANIFEST plugins/physics-jolt.provider.json
    LINKS Jolt INCLUDES ${CMAKE_SOURCE_DIR}/third_party/JoltPhysics)
engine_providers_apply(engine_runtime FLAVOUR client WRITE_LIST ${CMAKE_BINARY_DIR}/providers.json)
```

- `SOURCES` are compiled into a runtime target only where the provider is built.
- `LINKS` and `INCLUDES` become an INTERFACE target, `engine_provider_<kind>_<name>`,
  linked into the runtime target only where the provider is built.
- **A provider whose `LINKS` name another provider's target is refused**:
  providers depend on engine services, never on each other (`provider-abi.md` rule 9).
- `engine_providers_apply` writes `engine_providers_built.h` for the target, with
  `ENGINE_PROVIDER_<KIND>_<NAME>` set to 1 or 0 and every provider's reason. The
  build's list (`src/plugins/stock_providers.cpp`) registers a provider only when
  its macro is 1, and refuses to compile when a macro is missing. A provider
  that is not built is registered as unavailable: choosing it is refused with
  the reason, and `engine_player --list-providers` shows it.

## Built, or not, and why

A provider is built into a target when the platform is in `platforms`, the
target's flavour is in `flavours`, every fact in `needs` is true, and every SDK
in `sdk` is found. Configure prints one line per provider and target:

```
-- provider renderer forward-bgfx: built into engine_runtime
-- provider renderer forward-bgfx: not built for engine_runtime_server: it is not built for server builds (flavours: client)
```

**Not built is normal. A project choosing one that is not built is the error.**
Today that error comes at startup (WO-076). After WO-086 it comes from `engine_build`.

## The platform tables

`cmake/platforms/<platform>.json`, one per platform: `macos`, `linux`, `windows`,
`android`, `ios`, `web`. The platform being built comes from `CMAKE_SYSTEM_NAME`,
and an unknown system is a configure error.

```json
{
  "platform": "web",
  "facts": { "display": true, "threads": false, "dlopen": false, "audio_output": true },
  "gpu_apis": ["webgpu", "webgl2"]
}
```

Every table states every fact. A table missing one is a configure error, so a
`needs` can never pass only because nobody wrote the fact down. `gpu_apis` is
read by librhi's backend order (WO-089), not by `needs`.
