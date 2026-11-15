#version 460
layout(std140, binding = 0) uniform Blit {
  vec4 dest;
  vec4 source;
  ivec4 mode;
};

layout(binding = 0) uniform sampler2D u_source;

layout(location = 0) in vec2 v_source;
layout(location = 0) out vec4 o_colour;

void main() {
  const vec4 texel = texelFetch(u_source, ivec2(v_source), 0);
  // Mode 1: depth into a single channel surface. Mode 2: the resolve swaps red and blue.
  o_colour = mode.x == 1 ? vec4(texel.r) : mode.x == 2 ? texel.bgra : texel;
}
