#version 460
// The menu's pixel shaders, as modes. Hand-translated from the captured Xenos microcode
// (tools/ucode_dis.py) and proven in software first (tools/capsim.py).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour0;
layout(location = 2) in vec4 v_colour1;

layout(location = 0) out vec4 o_colour;

layout(binding = 0) uniform sampler2D u_texture;

layout(std140, binding = 0) uniform Draw {
  vec4 c2;
  vec4 c3;
  vec4 solid;
  ivec4 mode;
};

const int kVertexColour = 0;   // 438c2af84c78a133
const int kSolid = 1;          // 03119cfd11beb173
const int kVertexTransform = 2;  // 7a27a638de062231
const int kTexture = 3;        // a15c6c9c2d249375
const int kTextureTransform = 4;  // 1d03653c7a4cff4b
const int kGradient = 5;       // 43f1e01c2eb935be
const int kGlyph = 6;          // 4d80b8b337ff38f4

void main() {
  const int m = mode.x;
  if (m == kVertexColour) {
    o_colour = v_colour0;
  } else if (m == kSolid) {
    o_colour = solid;
  } else if (m == kVertexTransform) {
    o_colour = v_colour0 * c2 + c3;
  } else {
    const vec4 t = texture(u_texture, v_uv);
    if (m == kTexture) {
      o_colour = t;
    } else if (m == kTextureTransform) {
      o_colour = t * c2 + c3;
    } else if (m == kGradient) {
      const vec4 mixed = (t - v_colour0) * v_colour1.z + v_colour0;
      o_colour = vec4(mixed.rgb * c2.rgb + c3.rgb, (mixed.a * c2.a + c3.a) * v_colour1.a);
    } else {
      o_colour = vec4(v_colour0.rgb * c2.rgb + c3.rgb, (v_colour0.a * c2.a + c3.a) * t.a);
    }
  }
}
