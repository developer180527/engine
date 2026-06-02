// miniaudio implementation unit. This is the only place MA_IMPLEMENTATION is
// defined; everywhere else just includes miniaudio.h for the declarations.
#define MA_IMPLEMENTATION
#define MA_NO_ENCODING        // playback only — we never write audio files
#include "miniaudio.h"
