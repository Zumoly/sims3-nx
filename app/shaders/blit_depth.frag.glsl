#version 460
// Writes a resolved depth surface back into a depth target. Direct3D does this by drawing depth bytes
// through a colour target that shares the EDRAM of the depth target.
layout(binding = 0) uniform sampler2D u_source;

layout(location = 0) in vec2 v_source;

void main() {
  gl_FragDepth = texelFetch(u_source, ivec2(v_source), 0).r;
}
