vec3 a_position  : POSITION;
vec3 a_normal    : NORMAL;
vec4 a_tangent   : TANGENT;
vec2 a_texcoord0 : TEXCOORD0;
vec4 a_indices   : BLENDINDICES;
vec4 a_weight    : BLENDWEIGHT;
vec4 a_color0    : COLOR0;

vec4 i_data0     : TEXCOORD7;
vec4 i_data1     : TEXCOORD6;
vec4 i_data2     : TEXCOORD5;
vec4 i_data3     : TEXCOORD4;

vec4 v_color0       : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);
vec3 v_worldPos     : TEXCOORD0 = vec3(0.0, 0.0, 0.0);
vec3 v_worldNormal  : TEXCOORD1 = vec3(0.0, 1.0, 0.0);
vec4 v_worldTangent : TEXCOORD2 = vec4(1.0, 0.0, 0.0, 1.0);
vec2 v_texcoord0    : TEXCOORD3 = vec2(0.0, 0.0);

// NOTE: keep declarations flush. shaderc's varying.def parser swallowed the entry
// that followed a comment block placed mid-list — i_data0 vanished from the
// generated main() signature and the shader failed with "HLSL parsing failed" on
// a line that looked correct. Comments belong at the end of the file, like this.
//
// i_data0..i_data3 are INSTANCE data: four columns of a model matrix filled by
// bgfx::allocInstanceDataBuffer. TEXCOORD7..4 is bgfx's own convention (see
// bgfx/examples/06-bump/varying.def.sc) and does not collide with the v_*
// varyings on TEXCOORD0..3.
