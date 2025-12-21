#version 440

// Sun shadow depth. One cascade per draw: the cascade block is bound with a
// dynamic offset, so the same pipeline renders every tile of the atlas.

layout(location = 0) in vec3 a_position;

layout(std140, binding = 0) uniform CascadeBlock {
    mat4 u_lightView;
    mat4 u_lightClip;
    vec4 u_cascadeParams; // x = 1/far
};

layout(std140, binding = 1) uniform ObjectBlock {
    mat4 u_model;
    mat4 u_normalMatrix;
};

layout(location = 0) out float v_depth;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    vec4 lightViewPos = u_lightView * world;
    v_depth = clamp(-lightViewPos.z * u_cascadeParams.x, 0.0, 1.0);
    gl_Position = u_lightClip * world;
}
