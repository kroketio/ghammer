#version 440

layout(binding = 1) uniform lowp sampler2D t_density;

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

layout(location = 0) out vec4 fragColor;

void main() {
    vec2 uv = gl_FragCoord.xy / max(u_wireStyle.yz, vec2(1.0));
    float count = texture(t_density, uv).x;
    float t = clamp((count - 1.0) / max(u_wireStyle.x, 0.0001), 0.0, 1.0);
    fragColor = vec4(mix(u_wireCool.rgb, u_wireWarm.rgb, t), 1.0);
}
