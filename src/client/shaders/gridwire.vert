#version 440

layout(location = 0) in vec4 a_data; // xy = centerline pos, zw = unit perpendicular

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

void main() {
    vec2 p = a_data.xy + a_data.zw * u_wireStyle.w;
    gl_Position = u_gridToClip * vec4(p, 0.0, 1.0);
}
