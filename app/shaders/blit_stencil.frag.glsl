#version 460
// One pass of the stencil capture at a depth resolve: drawn with the depth target's stencil tested against
// bit mode.x, it adds that bit to an R8 copy (blend One + One). The stencil is never sampled.
layout(std140, binding = 0) uniform Blit {
  vec4 dest;
  vec4 source;
  ivec4 mode;
};

layout(location = 0) out vec4 o_colour;

void main() {
  o_colour = vec4(float(1u << uint(mode.x)) / 255.0);
}
