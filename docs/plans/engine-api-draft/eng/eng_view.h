/* ----DRAFT------ Nothing in this file is promised, built or linked yet.
 *
 * eng_view.h: what appears where. A view is one camera on one world, drawn to a
 * rectangle of a target. A window can hold several views, and several windows
 * can show one world. Main thread only. */
#ifndef ENG_VIEW_H
#define ENG_VIEW_H
#include "eng_base.h"

ENG_EXTERN_C_BEGIN

/* A rectangle in 0..1 of the target. All zero: the whole target. */
typedef struct EngViewRect { float x, y, width, height; } EngViewRect;

typedef struct EngViewDesc {
    uint32_t struct_size;
    uint32_t order;        /* lower draws first, within one target */
    EngEntity camera;      /* an entity with core.Camera and core.Transform */
    EngWorld* world;
    const char* name;      /* shown by the profiler; NULL is fine */
    EngWindow window;      /* exactly one of window and offscreen */
    EngOffscreen offscreen;
    EngViewRect rect;
    float render_scale;    /* 0: 1.0. Below 1 renders fewer pixels and upscales */
    uint32_t max_frames_per_second; /* 0: every frame. Slow panels can draw less */
} EngViewDesc;

ENG_API EngResult engCreateView(EngEngine* engine, const EngViewDesc* desc, EngView* out);
ENG_API void engDestroyView(EngEngine* engine, EngView view);
ENG_API EngResult engSetViewCamera(EngEngine* engine, EngView view, EngEntity camera);
ENG_API EngResult engSetViewRect(EngEngine* engine, EngView view, EngViewRect rect);
ENG_API EngResult engSetViewEnabled(EngEngine* engine, EngView view, EngBool enabled);

/* ── Offscreen: screenshots, thumbnails, headless tests and agents ── */
typedef struct EngOffscreenDesc {
    uint32_t struct_size;
    uint32_t width_pixels, height_pixels;
} EngOffscreenDesc;
ENG_API EngResult engCreateOffscreen(EngEngine* engine, const EngOffscreenDesc* desc,
                                     EngOffscreen* out);
ENG_API void engDestroyOffscreen(EngEngine* engine, EngOffscreen offscreen);
/* RGBA8, top row first. ENG_NOT_READY until the GPU has finished; never blocks.
 * size: in, the buffer's capacity; out, the bytes needed. */
ENG_API EngResult engReadOffscreen(EngEngine* engine, EngOffscreen offscreen,
                                   void* out, size_t* size);

ENG_EXTERN_C_END
#endif /* ENG_VIEW_H */
