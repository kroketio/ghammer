#version 440

// Entity icon billboard: camera-facing quad of constant screen size, expanded in clip space (offset * w);
// depth stays the entity's own.

layout(location = 0) in vec2 a_corner; // -1..1

layout(std140, binding = 0) uniform SceneUBO {
    mat4 u_viewProj;
};

layout(std140, binding = 14) uniform EntityUBO {
    mat4 u_model;  // translation = entity position
    vec4 u_color;  // tint (rgb) and opacity
    vec4 u_info;   // xy = half size in NDC (y signed for the NDC convention), z = atlas slot, w = slot count
};

layout(location = 0) out vec2 v_uv;

void main() {
    vec4 clip = u_viewProj * vec4(u_model[3].xyz, 1.0);
    clip.xy += a_corner * u_info.xy * clip.w;
    gl_Position = clip;
    v_uv = vec2((a_corner.x * 0.5 + 0.5 + u_info.z) / max(u_info.w, 1.0), a_corner.y * 0.5 + 0.5);
}
