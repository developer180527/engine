/* UNSTABLE (0.x): see eng_base.h.
 *
 * eng_window.h: monitors, windows and window events. Main thread only.
 *
 * THIS VERSION (WO-070): one window, created before the first engStep. Its
 * mode, monitor, size, position, limits, aspect and flags can be changed at
 * any time, and its events are read with engNextEvent. Several windows,
 * presentation modes, frame limits, cursor and text input, native handles,
 * embedding and the clipboard are in the draft
 * (docs/plans/engine-api-draft/eng/eng_window.h) and not declared here.
 *
 * A game may have any number of windows where the platform allows it, and asks
 * first (engGetWindowCapabilities). A platform that cannot do something says so
 * with ENG_ERROR_UNSUPPORTED. It never pretends.
 *
 * Sizes and positions are logical units unless the name says pixels. */
#ifndef ENG_WINDOW_H
#define ENG_WINDOW_H
#include "eng_base.h"

ENG_EXTERN_C_BEGIN

/* ── What this platform can do ── */
typedef struct EngWindowCapabilities {
    uint32_t struct_size;   /* set by the caller */
    uint32_t max_windows;   /* 0 headless, 1 web and phones, large on desktop */
    EngBool can_exclusive_fullscreen;
    EngBool can_borderless_fullscreen;
    EngBool can_resize;
    EngBool can_position;
    EngBool can_list_monitors;
    EngBool can_capture_cursor;
} EngWindowCapabilities;
ENG_API void engGetWindowCapabilities(EngEngine* engine,
                                      EngWindowCapabilities* out);

/* ── Monitors ── */
typedef struct EngDisplayMode {
    uint32_t width_pixels, height_pixels;
    uint32_t refresh_millihertz; /* 59940 is 59.94 Hz */
    uint32_t reserved;           /* must be zero */
} EngDisplayMode;

typedef struct EngMonitorInfo {
    uint32_t struct_size;  /* set by the caller */
    EngBool is_primary;
    int32_t x, y;          /* position on the virtual desktop */
    uint32_t width, height;
    float content_scale;
    float hdr_headroom;    /* 1.0 for SDR; 0 when unknown (this version does
                              not read it per monitor) */
    EngDisplayMode current_mode;
    char name[128];        /* UTF-8, NUL-terminated */
} EngMonitorInfo;

/* List functions: pass NULL to get the count, or an array and its capacity.
 * ENG_ERROR_BUFFER_TOO_SMALL sets *count to what is needed. */
ENG_API EngResult engGetMonitors(EngEngine* engine, EngMonitor* out,
                                 uint32_t* count);
ENG_API EngResult engGetMonitorInfo(EngEngine* engine, EngMonitor monitor,
                                    EngMonitorInfo* out);
ENG_API EngResult engGetMonitorModes(EngEngine* engine, EngMonitor monitor,
                                     EngDisplayMode* out, uint32_t* count);

/* ── Windows ── */
typedef uint32_t EngWindowMode;
enum {
    ENG_WINDOW_MODE_WINDOWED = 0,
    ENG_WINDOW_MODE_BORDERLESS_FULLSCREEN = 1,
    ENG_WINDOW_MODE_EXCLUSIVE_FULLSCREEN = 2
};

/* Zero is what most games want: decorated, resizable, visible, and closed when
 * the user closes it. */
typedef uint32_t EngWindowFlags;
enum {
    ENG_WINDOW_FIXED_SIZE = 1u << 0,         /* the user cannot resize it */
    ENG_WINDOW_NO_DECORATION = 1u << 1,      /* no title bar or border */
    ENG_WINDOW_ALWAYS_ON_TOP = 1u << 2,
    ENG_WINDOW_HIDDEN = 1u << 3,             /* show it when your first frame is ready */
    ENG_WINDOW_NO_USER_MODE_CHANGE = 1u << 4,/* the OS's own fullscreen button and
                                                shortcuts do nothing: for a game
                                                that is fullscreen only */
    ENG_WINDOW_HANDLE_CLOSE = 1u << 5,       /* closing only sends an event; you decide */
    ENG_WINDOW_HDR_IF_AVAILABLE = 1u << 6
};

typedef uint32_t EngPresentMode;
enum {
    ENG_PRESENT_VSYNC = 0,   /* the default: no tearing, display sets the rhythm */
    ENG_PRESENT_MAILBOX = 1, /* no tearing, the newest frame wins */
    ENG_PRESENT_IMMEDIATE = 2/* tearing allowed, lowest latency */
};

typedef struct EngWindowDesc {
    uint32_t struct_size;
    EngWindowFlags flags;
    const char* title;           /* UTF-8; NULL: the app name */
    EngWindowMode mode;
    EngMonitor monitor;          /* 0: the primary monitor */
    uint32_t width, height;      /* 0: a default size */
    int32_t x, y;                /* used when use_position is set */
    uint32_t use_position;       /* ENG_TRUE to honour x and y; 0: the OS places it */
    uint32_t min_width, min_height;
    uint32_t max_width, max_height;       /* 0: no limit */
    uint32_t aspect_numerator, aspect_denominator; /* 0: free */
    EngDisplayMode exclusive_mode; /* all zero: the monitor's current mode */
    EngPresentMode present_mode;   /* this version: VSYNC only; others refused */
    uint32_t max_frames_per_second;/* 0: no limit beyond the present mode.
                                      Non-zero is refused until WO-061 */
    uint32_t reserved;             /* must be zero */
} EngWindowDesc;

ENG_API EngResult engCreateWindow(EngEngine* engine, const EngWindowDesc* desc,
                                  EngWindow* out);
ENG_API void engDestroyWindow(EngEngine* engine, EngWindow window);

/* The window whose display sets the frame rhythm. The others present without
 * blocking, each up to its own limit. Default: the first window created.
 * This version has one window, which is always the primary one. */
ENG_API EngResult engSetPrimaryWindow(EngEngine* engine, EngWindow window);

typedef struct EngWindowInfo {
    uint32_t struct_size;   /* set by the caller */
    EngWindowMode mode;
    EngMonitor monitor;
    int32_t x, y;
    uint32_t width, height;
    uint32_t width_pixels, height_pixels;
    float content_scale;
    float hdr_headroom;
    EngBool is_focused;
    EngBool is_minimised;
    EngBool is_visible;
} EngWindowInfo;
ENG_API EngResult engGetWindowInfo(EngEngine* engine, EngWindow window,
                                   EngWindowInfo* out);

/* All of these can run at any time. Most take effect at the next pump, and an
 * event reports the actual outcome, because the OS may adjust the request. */
ENG_API EngResult engSetWindowMode(EngEngine* engine, EngWindow window,
                                   EngWindowMode mode, EngMonitor monitor,
                                   const EngDisplayMode* exclusive_mode_or_null);
ENG_API EngResult engSetWindowTitle(EngEngine* engine, EngWindow window, const char* title);
ENG_API EngResult engSetWindowSize(EngEngine* engine, EngWindow window, uint32_t width, uint32_t height);
ENG_API EngResult engSetWindowPosition(EngEngine* engine, EngWindow window, int32_t x, int32_t y);
ENG_API EngResult engSetWindowSizeLimits(EngEngine* engine, EngWindow window,
                                         uint32_t min_width, uint32_t min_height,
                                         uint32_t max_width, uint32_t max_height);
ENG_API EngResult engSetWindowAspect(EngEngine* engine, EngWindow window,
                                     uint32_t numerator, uint32_t denominator);
ENG_API EngResult engSetWindowFlags(EngEngine* engine, EngWindow window, EngWindowFlags flags);
ENG_API EngResult engShowWindow(EngEngine* engine, EngWindow window, EngBool visible);
ENG_API EngResult engFocusWindow(EngEngine* engine, EngWindow window);

/* ── Events ── */
typedef uint32_t EngEventType;
enum {
    ENG_EVENT_NONE = 0,
    ENG_EVENT_WINDOW_CLOSE_REQUESTED,
    ENG_EVENT_WINDOW_CLOSED,
    ENG_EVENT_WINDOW_RESIZED,        /* width, height, then pixels in a, b */
    ENG_EVENT_WINDOW_MOVED,          /* x, y */
    ENG_EVENT_WINDOW_FOCUS_CHANGED,  /* a: 1 gained, 0 lost */
    ENG_EVENT_WINDOW_MINIMISED_CHANGED, /* a: 1 minimised, 0 restored */
    ENG_EVENT_WINDOW_MODE_CHANGED,   /* a: the EngWindowMode now in effect */
    ENG_EVENT_WINDOW_SCALE_CHANGED,  /* value: the new content scale */
    ENG_EVENT_WINDOW_MONITOR_CHANGED,/* monitor */
    ENG_EVENT_MONITOR_ADDED,         /* monitor */
    ENG_EVENT_MONITOR_REMOVED,       /* monitor */
    ENG_EVENT_KEY_DOWN,              /* a: key, b: modifiers, c: 1 if a repeat */
    ENG_EVENT_KEY_UP,                /* a: key, b: modifiers */
    ENG_EVENT_TEXT,                  /* text */
    ENG_EVENT_POINTER_MOVED,         /* value_x, value_y, in window units */
    ENG_EVENT_POINTER_DOWN,          /* a: button; value_x, value_y */
    ENG_EVENT_POINTER_UP,            /* a: button; value_x, value_y */
    ENG_EVENT_SCROLL,                /* value_x, value_y */
    ENG_EVENT_APP_SUSPENDED,
    ENG_EVENT_APP_RESUMED,
    ENG_EVENT_DEVICE_LOST,
    ENG_EVENT_OVERFLOWED             /* a: how many older events were dropped */
};

/* Flat on purpose: unions are hard to read from Python, JS and Swift. Which
 * fields mean something depends on type. */
typedef struct EngEvent {
    uint32_t struct_size;  /* set by the caller */
    EngEventType type;
    EngWindow window;
    EngMonitor monitor;
    double time_seconds;
    int32_t x, y;
    uint32_t width, height;
    int32_t a, b, c;
    float value;
    float value_x, value_y;
    char text[32];         /* UTF-8, NUL-terminated */
} EngEvent;

/* The next event, or ENG_FALSE when none are left. Polling is the main path
 * because it works the same in every language. An unread queue is bounded, and
 * reports ENG_EVENT_OVERFLOWED rather than growing. */
ENG_API EngBool engNextEvent(EngEngine* engine, EngEvent* out);
/* This version produces the WINDOW_* events (and OVERFLOWED). Monitors being
 * connected or removed are not reported yet; keyboard, pointer, text and app
 * events still reach games through the input actions (input.json). */

ENG_EXTERN_C_END
#endif /* ENG_WINDOW_H */
