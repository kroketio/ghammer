#version 440

// The glyph mask is in the red channel, so labels never depend on alpha surviving the round trip.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(binding = 4) uniform sampler2D u_textTex;

layout(location = 0) out vec4 fragColor;

void main() {
    float mask = texture(u_textTex, v_uv).r;
    if (mask <= 0.004)
        discard;
    fragColor = vec4(v_color.rgb, v_color.a * mask);
}
