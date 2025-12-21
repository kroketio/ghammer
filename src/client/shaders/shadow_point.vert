#version 440

// Entity light shadow pass (one atlas tile per draw). As shadow.vert: linear depth / the light's far in a
// colour attachment, so the receiver biases in world units and no window-vs-clip z convention applies.
// The six cube faces share one pipeline and SRB: this block holds one face's matrices, the draw's dynamic
// offset picks the face. 90 degree perspective per face; far = the light's range.

layout(location = 0) in vec3 a_position;

layout(std140, binding = 15) uniform LightShadowUBO {
    mat4 u_faceClip;    // corr * proj(90) * view, for the face this offset points at
    mat4 u_faceView;    // view only, for the linear depth
    vec4 u_faceInfo;    // x = 1/far (the depth encoding scale), y = near, zw unused
};

layout(std140, binding = 1) uniform ObjectBlock {
    mat4 u_model;
    mat4 u_normalMatrix;
};

// The light-to-vertex vector (divided by the range), not its length: a vector interpolates linearly, an
// interpolated distance overshoots across large faces (up to ~50%).
layout(location = 0) out vec3 v_lightVec;

void main() {
    vec4 world = u_model * vec4(a_position, 1.0);
    vec4 lightViewPos = u_faceView * world;
    // Radial distance, not -viewZ: the receiver compares its own distance from the light, and off the face axis
    // the two differ by 1/cos(theta). Godot stores length() too ("shadow_len = length(local_vert)").
    v_lightVec = lightViewPos.xyz * u_faceInfo.x;
    gl_Position = u_faceClip * world;
}
