#version 440

// AO composite: writes only the occlusion factor; the blend state (src = DstColor, dst = Zero) multiplies it in.

layout(location = 0) in vec2 v_uv;

layout(binding = 12) uniform sampler2D u_aoTex;

layout(location = 0) out vec4 fragColor;

void main() {
    float ao = texture(u_aoTex, v_uv).r;
    fragColor = vec4(ao, ao, ao, 1.0);
}
