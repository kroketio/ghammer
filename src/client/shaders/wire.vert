#version 440

// Selection wireframe overlay. QRhi has no polygon offset, so it is pushed toward the eye by a fraction of
// its own distance.

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_barycentric;
layout(location = 2) in vec3 a_normal;

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

layout(std140, binding = 1) uniform ObjectBlock {
    mat4 u_model;
    mat4 u_normalMatrix;
};

layout(std140, binding = 3) uniform OverlayUBO {
    vec4 u_wireColor;
    vec4 u_wireParams;      // x thickness in px, y push factor, zw unused
};

layout(location = 0) out vec3 v_bary;
// Camera distance in world units after the push; compared with scene depth in the fragment stage.
layout(location = 1) out float v_viewDist;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    vec3 toCam = u_cameraPos.xyz - world.xyz;
    float dist = max(length(toCam), 1e-3);
    world.xyz += (toCam / dist) * dist * u_wireParams.y;

    v_viewDist = -(u_view * world).z;
    v_bary = a_barycentric;
    gl_Position = u_viewProj * world;
}
