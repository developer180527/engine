#pragma once

#include <cstdint>

namespace core {

// Generic handle template. Used to reference assets stored in registries
// without holding raw pointers.
//
// The Tag template parameter ensures handles to different asset types are
// not interchangeable at the type level. MeshHandle and TextureHandle are
// both built on this template, but they're different types because their
// tags are different — the compiler refuses to confuse them.
//
// Index 0 is reserved as "invalid". A default-constructed handle is invalid,
// so `if (handle.valid())` works as expected.
//
// Wrapped in `core::` namespace because Apple's MacTypes.h (transitively
// included via Cocoa) defines a legacy `Handle` typedef. Namespacing avoids
// the collision.
template <typename Tag>
struct Handle {
    uint32_t id = 0;

    bool valid() const { return id != 0; }

    bool operator==(const Handle& other) const { return id == other.id; }
    bool operator!=(const Handle& other) const { return id != other.id; }
};

// Type tags. Each is just an empty struct — its only role is to make
// Handle<MeshTag> a different type from Handle<TextureTag> at compile time.
struct MeshTag {};
struct TextureTag {};
struct MaterialTag {};
struct AudioClipTag {};
struct ScriptTag {};

} // namespace core

// Aliases pulled to global scope for convenience. They live as core::Handle
// internally but are usable as bare names at use sites.
using MeshHandle      = core::Handle<core::MeshTag>;
using TextureHandle   = core::Handle<core::TextureTag>;
using MaterialHandle  = core::Handle<core::MaterialTag>;
using AudioClipHandle = core::Handle<core::AudioClipTag>;
using ScriptHandle    = core::Handle<core::ScriptTag>;
