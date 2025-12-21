#version 440

// Depth/normal prepass for SSAO, in VIEW space: small coordinates near the camera keep the half float
// position buffer accurate enough.

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;

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
    vec4 u_shadowFit;
};

layout(std140, binding = 1) uniform ObjectBlock {
    mat4 u_model;
    mat4 u_normalMatrix;
};

layout(location = 0) out vec3 v_viewPos;
layout(location = 1) out vec3 v_viewNormal;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    v_viewPos = (u_view * world).xyz;
    v_viewNormal = normalize(mat3(u_view) * (mat3(u_normalMatrix) * a_normal));
    gl_Position = u_viewProj * world;
}
