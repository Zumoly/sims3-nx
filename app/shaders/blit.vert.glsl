#version 460
// Full-rectangle copy used for resolves and the final present. No vertex buffer: gl_VertexID picks a corner.
layout(std140, binding = 0) uniform Blit {
  vec4 dest;    // x0, y0, x1, y1 in NDC
  vec4 source;  // x0, y0, x1, y1 in source texels
  ivec4 mode;
};

layout(location = 0) out vec2 v_source;

void main() {
  const vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
  gl_Position = vec4(mix(dest.xy, dest.zw, corner), 0.0, 1.0);
  v_source = mix(source.xy, source.zw, corner);
}
