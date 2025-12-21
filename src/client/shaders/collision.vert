#version 440

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_color;

layout(location = 0) out vec4 v_color;

layout(std140, binding = 0) uniform SceneUBO {
    mat4 u_viewProj;
};

void main() {
    v_color = a_color;
    gl_Position = u_viewProj * vec4(a_pos, 1.0);
}
