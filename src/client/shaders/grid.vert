#version 440

// Infinite 2D grid: one fullscreen quad, computed per pixel.

layout(location = 0) in vec2 a_ndc;

layout(std140, binding = 0) uniform GridUBO {
    mat4 u_gridToClip;
    mat4 u_clipToGrid;
    mat4 u_worldToClip;
    vec4 u_viewInfo;      // x pxPerWorldUnit, y devicePixelRatio, z showGrid, w showFill
    vec4 u_gridStyle;     // x stepWorld, y majorEvery, z minorAlpha, w majorAlpha
    vec4 u_colorMinor;
    vec4 u_colorMajor;
    vec4 u_colorAxis;
    vec4 u_lineScale;     // NDC per device pixel (2/pixelSize), thick wire line width
};

layout(location = 0) out vec2 v_plane;

void main() {
    // The mapping is separable: x depends only on the plane's first axis and y
    // only on the second, so any depth reconstructs the plane position correctly.
    v_plane = (u_clipToGrid * vec4(a_ndc, 0.0, 1.0)).xy;
    gl_Position = vec4(a_ndc, 0.0, 1.0);
}
