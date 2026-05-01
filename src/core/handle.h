#pragma once

#include <cstdint>

// Generic handle template. Used to reference assets stored in registries
// without holding raw pointers.
//
// The Tag template parameter ensures that handles to different asset types
// are not interchangeable at the type level. A MeshHandle and a TextureHandle
// are both built on this same template, but the compiler will refuse to
// confuse them — they are different types because their tags are different.
//
// Index 0 is reserved as "invalid". A default-constructed handle is invalid,
// which lets us check `if (handle.valid())` everywhere.
template <typename Tag>
struct Handle {
    uint32_t id = 0;

    bool valid() const { return id != 0; }

    bool operator==(const Handle& other) const { return id == other.id; }
    bool operator!=(const Handle& other) const { return id != other.id; }
};

// Type tags used to distinguish different handle kinds. Each tag is just an
// empty struct — its only purpose is to make Handle<MeshTag> a different
// type from Handle<TextureTag> at compile time.
struct MeshTag {};
struct TextureTag {};
struct MaterialTag {};
struct AudioClipTag {};
struct ScriptTag {};

using MeshHandle      = Handle<MeshTag>;
using TextureHandle   = Handle<TextureTag>;
using MaterialHandle  = Handle<MaterialTag>;
using AudioClipHandle = Handle<AudioClipTag>;
using ScriptHandle    = Handle<ScriptTag>;
