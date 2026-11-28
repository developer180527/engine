---
status: plan
id: WO-095
title: The C++ wrapper over the C world API — RAII handles, Result, typed components — so C++ kits and providers stop reaching into flecs
program: providers
priority: P1
size: M
state: active
depends: [WO-073]
touches:
  - include/eng/
  - tests/eng_cpp_test.cpp
  - tests/cpp_kit_host_test.cpp
  - tests/fixtures/cpp_module_regen.cpp
  - tests/eng_query_bench.cpp
  - scripts/engine_audit.py
source: review of WO-072..094 against the plan, 2026-11-28 (engine-api.md §8: "C++: a header-only wrapper: RAII handles, Result, typed components"; DR-0028: one C API with a C++ wrapper, for everyone)
---
## Why
The plan has one API for everyone, C, with C++ as a thin header-only wrapper over it (DR-0028, engine-api.md §8). WO-073 built the C world and WO-094 loads C kits, but no order builds the wrapper. Without it, every C++ user of the world has two bad choices: the raw C calls with hand-built descriptors and sizes, or flecs through the old kit ABI. WO-094's open item is exactly that: the FPS and AI kits are C++ and reach the combat kit's components through flecs, and its log weighs giving C++ kits an `EngWorld*` against porting them. The plan's answer is this wrapper, and WO-074 (physics on the world API, next on the board) needs it too, since Jolt's plugin is C++.

## Done when
- [x] `include/eng/eng.hpp`, header-only over `eng_world.h` and nothing else: `eng::World`, `eng::Entity`, `eng::Query<Ts...>` and `eng::System` as RAII handles; `eng::Result<T>` carrying `EngResult` and the error text; typed components (`world.component<T>("ns.Name")` builds the field list from a description the type gives, checked against `sizeof`/`alignof`)
- [x] a typed query iterates batches as spans of `T` per term, with WRITE and WRITE_MARKED as distinct types, so marking cannot be forgotten
- [x] the combat kit's C++ users can be written against it: a test kit in C++ reads and writes `combat.Health` through the wrapper, loaded like any C module, with no flecs or engine C++ include
- [x] it adds no exported symbol and no cost: the wrapper's query loop measured against the C one on `eng_query_bench`
- [x] an `engine_audit` rule: `include/eng/eng.hpp` includes only `include/eng/`

## Contract
Nothing: the C API is unchanged; the wrapper is headers over it.

## Not in scope
Porting the FPS and AI kits (WO-094 part 2 decides that, on this); the Python, Swift, JS and Rust layers (engine-api.md §8).

## Log
- 2026-11-28 — **Built: everything in "Done when".** Left `active` until CI
  is green on every leg (the GCC and MSVC legs are the first to compile it).
  - `include/eng/eng.hpp` is header-only over `<eng/engine.h>` and the
    standard library (`<cstddef>` … `<vector>`). It throws nothing and needs
    no RTTI: the test and the kit build with `-fno-exceptions -fno-rtti`.
    - **Results:** `eng::Result<T>` and `Result<void>` carry the `EngResult`
      and `engGetLastErrorMessage()`. `eng::check(r)` wraps a C call.
    - **Components:** `ENG_COMPONENT(T, "ns.Name", eng::field("x", &T::x), …)`
      describes a type once. The name is part of the description, not an
      argument to `component<T>()` as the done-when line put it, so one type
      cannot be registered under two names by accident.
      - The field type comes from the member's C++ type:
        - ints, floats and doubles;
        - `EngVec3`, `EngQuat`, and `float[2/3/4/16]` as VEC2, VEC3, VEC4 and MAT4;
        - `char[N]` as a fixed string.
      - The type has to be stated where C++ cannot tell: `ENG_FIELD_ENTITY`,
        `BLOB` and `BOOL32`, and arrays of other types.
      - The offsets and the registered defaults come from a default-constructed
        `T`, and `static_assert`s require it to be standard-layout and trivially
        copyable.
      - Registration passes `sizeof` and `alignof`, and the engine compares
        layout, fields and defaults. A C++ kit that describes another kit's
        component is therefore *checked* against it: the regen fixture
        describes `combat.Health` itself.
      - `findComponent<T>(name)` is the unchecked escape hatch, documented as such.
    - **Handles:**
      - `eng::World` (a view) and `eng::Entity` (a value);
      - the owning `eng::Engine` and `eng::OwnedWorld`, for programs;
      - `Query`, `System` and `ChangeStream` as RAII handles, move-only.
    - **Queries:** `Query<Read<T>, Write<T>, WriteMarked<T>, With<T>,
      Without<T>>`. `column<I>()` returns:
      - `Span<const T>` for Read;
      - `Span<T>` for Write, where the C API marks every row;
      - `MarkedColumn<T>` for WriteMarked. Its `operator[]` is const, and the
        only mutable access is `write(i)`, which records the row. Contiguous
        rows are coalesced into one `engMarkChanged` when the column goes.
        An unmarked write cannot be written.
    - **Systems:** `System::over(world, query, options, fn)` and
      `System::every(...)`. The heap functor is owned by the handle, and the
      trampolines are `noexcept`.
    - **Modules:** `eng::module<Kit>("name", "ver")` adapts a class with
      `start(World)` and `stop(World)`.
  - **Tests:**
    - `eng_cpp_test` (unit) covers registration (same id, refusal with its
      message), entities, typed columns (`static_assert`ed per access), and
      WriteMarked reporting exactly the 4 written rows of 8.
    - `cpp_kit_host_test` (unit) loads `cpp_module_regen` from project.json
      beside the C combat kit:
      - the engine accepts the C++ kit's description of `combat.Health`;
      - it heals what combat's damage took, 2 a step up to the max;
      - the change stream sees only the healed entity;
      - after Stop and Play its RAII handles have ended its system (one heal
        a step, not two);
      - after an unload it stops healing.
  - **Exports:** `nm -gU` on the kit shows `_engModuleEntry` only. Its only
    libraries are libc++ and libSystem.
  - **Cost:** `eng_query_bench`, build-rel, 50 000 entities, 8 tables,
    three runs:

    | pass | wrapper | C API |
    |---|---|---|
    | plain (Read + Write) | 0.018 ms | 0.018–0.024 ms |
    | WRITE_MARKED, watched, every row written | 1.542–1.571 ms | 1.528–1.569 ms |

    The plain pass is the same as the C API (both are the same as flecs).
    In the marked pass, `MarkedColumn::write` adds 0.001–0.025 ms against
    one `engMarkChanged` per batch by hand, inside the run-to-run spread.
  - **Audit:** HDR-02 (not baselinable) says every header in `include/eng/`
    includes only `include/eng/` (normalised, so a `../` path does not slip
    through) and an allowlist of standard headers. It was checked by
    planting `<flecs.h>`, `<map>`, a `../../src` path to a real file, and
    `<eng/../../src/…>`: each was caught, and the header was restored.
