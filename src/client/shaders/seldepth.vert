#version 440

// Selected-object depth prepass: linear camera distance / far into a float colour attachment, because QRhi
// always clears depth on beginPass (a depth texture would not reach the occluder count pass). 0 = no selection.

layout(location = 0) in vec3 a_position;

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
    vec4 u_params1;             // w = 1 / camera far
    vec4 u_fogColor;
    vec4 u_shadowParams;
    vec4 u_shadowFit;          // x world units per shadow texel, y light far, z light near, w unused
};

layout(std140, binding = 1) uniform ObjectBlock {
    mat4 u_model;
    mat4 u_normalMatrix;
};

layout(location = 0) out float v_depth;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    vec4 viewPos = u_view * world;
    v_depth = clamp(-viewPos.z * u_params1.w, 0.0, 1.0);
    gl_Position = u_viewProj * world;
}
