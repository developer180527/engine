{
  "$comment": [
    "The engine's default forward PBR shader, expressed as a cooked asset with",
    "a DECLARED INTERFACE. Everything under \"parameters\" and \"samplers\" is",
    "what a material may set; everything else the shader uses (u_lights,",
    "u_shadowMtx, u_camPos, s_shadowMap) is engine-driven and deliberately not",
    "exposed — the cook logs those as informational, not as errors.",
    "",
    "\"features\" is empty on purpose. Skinning is currently a SEPARATE vertex",
    "source (vs_skinned.sc) rather than a define, so folding it in here would",
    "mean rewriting shader code in the same change that introduces the format.",
    "That merge is the natural first use of the feature matrix.",
    "",
    "Offsets are float components within the named uniform. They mirror how",
    "fs_triangle.sc already packs u_params: x=hasBaseColor y=roughness",
    "z=metallic w=hasNormalMap — x and w are engine-set flags, so only y and z",
    "are material-settable."
  ],

  "name": "standard",
  "vertex": "vs_triangle.sc",
  "fragment": "fs_triangle.sc",
  "varying": "varying.def.sc",

  "features": [],

  "parameters": [
    { "name": "baseColorFactor", "type": "color", "uniform": "u_colorFactor",
      "offset": 0, "default": [1.0, 1.0, 1.0, 1.0] },
    { "name": "roughness", "type": "float", "uniform": "u_params",
      "offset": 1, "default": 0.7 },
    { "name": "metallic", "type": "float", "uniform": "u_params",
      "offset": 2, "default": 0.0 }
  ],

  "samplers": [
    { "name": "baseColor", "uniform": "s_baseColor", "stage": 0,
      "fallback": "white" },
    { "name": "normalMap", "uniform": "s_normalMap", "stage": 1,
      "fallback": "flatNormal" }
  ]
}
