#version 460
// One pass of the stencil restore: keeps the pixels whose saved stencil has bit mode.x set, and the stencil
// state writes that bit (Replace with write mask 1 << bit). A fragment shader cannot write stencil values.
layout(std140, binding = 0) uniform Blit {
  vec4 dest;
  vec4 source;
  ivec4 mode;
};

layout(binding = 0) uniform sampler2D u_source;

layout(location = 0) in vec2 v_source;

void main() {
  const uint stencil = uint(texelFetch(u_source, ivec2(v_source), 0).r * 255.0 + 0.5);
  if (((stencil >> uint(mode.x)) & 1u) == 0u) discard;
}
