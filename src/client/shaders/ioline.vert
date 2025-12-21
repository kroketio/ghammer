#version 440

// Entity I/O lines (vk_io_lines.cpp): camera-facing strips built on the CPU, coloured per vertex.
// a_dash >= 0: distance along the line in pixels (dashed); < 0: solid.
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in float a_dash;

layout(location = 0) out vec4 v_color;
layout(location = 1) out float v_dash;

layout(std140, binding = 0) uniform SceneUBO {
    mat4 u_viewProj;
};

void main() {
    v_color = a_color;
    v_dash = a_dash;
    gl_Position = u_viewProj * vec4(a_pos, 1.0);
}
