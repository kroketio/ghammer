#version 440

layout(location = 0) in vec2 v_plane;

layout(std140, binding = 0) uniform GridUBO {
    mat4 u_gridToClip;
    mat4 u_clipToGrid;
    mat4 u_worldToClip;
    vec4 u_viewInfo;
    vec4 u_gridStyle;
    vec4 u_colorMinor;
    vec4 u_colorMajor;
    vec4 u_colorAxis;
    vec4 u_lineScale;     // NDC per device pixel (2/pixelSize), thick wire line width
};

layout(location = 0) out vec4 fragColor;

float lineMask(vec2 coord, float step, float widthPx) {
    if (step <= 0.0)
        return 0.0;
    vec2 c = coord / step;
    vec2 d = abs(fract(c - 0.5) - 0.5) / max(fwidth(c), vec2(1e-7));
    return max(1.0 - min(min(d.x, d.y) / widthPx, 1.0), 0.0);
}

void main() {
    if (u_viewInfo.z < 0.5) {
        fragColor = vec4(0.0);
        return;
    }

    float step = u_gridStyle.x;
    float majorEvery = max(u_gridStyle.y, 1.0);

    float minor = lineMask(v_plane, step, max(u_lineScale.z, 0.2)) * u_gridStyle.z;
    float major = lineMask(v_plane, step * majorEvery, 1.5) * u_gridStyle.w;

    vec2 axisDist = abs(v_plane) / max(fwidth(v_plane), vec2(1e-7));
    float axisX = 1.0 - clamp(axisDist.x / 1.6, 0.0, 1.0);
    float axisY = 1.0 - clamp(axisDist.y / 1.6, 0.0, 1.0);

    vec4 outColor = vec4(0.0);
    outColor = mix(outColor, u_colorMinor, minor);
    outColor = mix(outColor, u_colorMajor, major);
    outColor = mix(outColor, u_colorAxis, max(axisX, axisY));

    fragColor = outColor;
}
