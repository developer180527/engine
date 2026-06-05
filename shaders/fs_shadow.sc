#include <bgfx_shader.sh>
void main() {
    gl_FragColor = vec4_splat(1.0);
}

// TODO (Jun 4, 09:00 PM):
// Implement alpha-cutout shadow casting.
// Support transparent shadow casters.
// Add variance/EVSM shadow techniques if needed.
// Keep this pass depth-only whenever possible.