#version 440

// Dimension label decals, built on the CPU in the face's u/v basis (flat on the surface).

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;

layout(std140, binding = 0) uniform SceneUBO {
    mat4 u_viewProj;
    mat4 u_view;
    mat4 u_lightView;
    mat4 u_lightClip;
    vec4 u_cameraPos;
    vec4 u_sunDir;
    vec4 u_sunColor;
    vec4 u_ambient;
    vec4 u_params0;
    vec4 u_params1;
    vec4 u_fogColor;
    vec4 u_shadowParams;
    vec4 u_shadowFit;          // x world units per shadow texel, y light far, z light near, w unused
};

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = u_viewProj * vec4(a_position, 1.0);
}
