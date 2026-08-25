#pragma once
/* ── ENGINE_MODULE_EXPORT — making a symbol findable across the seam ─────────
 *
 * A kit, a game module and a provider are all loaded with dlopen/LoadLibrary and
 * then looked up by NAME. That lookup only works if the symbol is exported, and
 * the three toolchains spell "export this" differently:
 *
 *   GCC / Clang   __attribute__((visibility("default")))
 *   MSVC          __declspec(dllexport)
 *
 * Three SDK headers wrote the GCC form directly — contract.h,
 * engine_api_client.h and game_module.h — so on MSVC every module entry point
 * was a syntax error: "C3861: 'visibility': identifier not found", "C2374:
 * '__attribute__': redefinition", and a cascade of C2059/C2143 after it. The
 * entire module ABI, whose whole purpose is to be loaded from a shared library,
 * could not be compiled by the toolchain most likely to be building one.
 *
 * Its own header rather than a copy in each: these three are independent SDK
 * entry points (contract.h includes nothing but <stdint.h>), so each must stay
 * usable on its own — and three copies of a platform decision is three places to
 * fix it the next time a compiler is added.
 *
 * Deliberately no dllIMPORT case. The host never imports these; it dlopens the
 * module and resolves by name, which is what makes the seam a C ABI rather than
 * a link-time dependency.
 */

#if defined(_WIN32)
#  define ENGINE_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define ENGINE_MODULE_EXPORT __attribute__((visibility("default")))
#else
/* Nothing is better than something wrong: on an unknown toolchain the symbol
 * keeps whatever default visibility that compiler gives it, which is usually
 * exported. A guessed attribute that the compiler silently ignores would look
 * correct and fail at load time with "module exports no ...". */
#  define ENGINE_MODULE_EXPORT
#endif
