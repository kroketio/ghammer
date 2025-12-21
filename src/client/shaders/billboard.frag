#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 14) uniform EntityUBO {
    mat4 u_model;
    vec4 u_color;
    vec4 u_info;
};

layout(binding = 16) uniform sampler2D u_icons;

void main() {
    vec4 t = texture(u_icons, v_uv);
    if (t.a < 0.04)
        discard;
    fragColor = vec4(t.rgb * u_color.rgb, t.a * u_color.a);
}
