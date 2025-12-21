#version 440

layout(location = 0) in vec4 v_color;
layout(location = 1) in float v_dash;

layout(location = 0) out vec4 fragColor;

void main() {
    // 7 px on, 5 px off.
    if (v_dash >= 0.0 && mod(v_dash, 12.0) > 7.0)
        discard;
    fragColor = v_color;
}
