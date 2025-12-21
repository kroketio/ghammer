#version 440

layout(location = 0) in vec3 a_pos;

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

void main() {
    gl_Position = u_viewProj * vec4(a_pos, 1.0);
}
