#version 440

// Glyph-sprite quads like the 3D labels, in the 2D view's plane space (constant size on screen).

layout(location = 0) in vec3 a_position; // xy = plane coords
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;

layout(std140, binding = 0) uniform GridUBO {
    mat4 u_gridToClip;
    mat4 u_clipToGrid;
    mat4 u_worldToClip;
    vec4 u_viewInfo;
    vec4 u_gridStyle;
    vec4 u_colorMinor;
    vec4 u_colorMajor;
    vec4 u_colorAxis;
    vec4 u_lineScale;
    vec4 u_wireStyle;
    vec4 u_wireCool;
    vec4 u_wireWarm;
    vec4 u_wireSelect;
};

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = u_gridToClip * vec4(a_position.xy, 0.0, 1.0);
}
