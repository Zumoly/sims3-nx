#version 460
// Vertices are decoded and transformed on the CPU (RenderDraw), so this only passes them on.
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_colour0;
layout(location = 3) in vec4 a_colour1;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_colour0;
layout(location = 2) out vec4 v_colour1;

void main() {
  gl_Position = vec4(a_position, 0.0, 1.0);
  v_uv = a_uv;
  v_colour0 = a_colour0;
  v_colour1 = a_colour1;
}
