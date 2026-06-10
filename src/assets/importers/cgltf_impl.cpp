// cgltf is a "header + implementation" library: the header has both
// declarations and (gated by CGLTF_IMPLEMENTATION) definitions. To get
// the definitions into our binary exactly once, define the macro in
// this single .cpp file and include the header. Other files that need
// cgltf APIs just #include <cgltf.h> normally — they'll see only the
// declarations.
//
// This is the same pattern stb libraries use, and it's the standard
// way to consume single-header libraries in C++ projects.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
