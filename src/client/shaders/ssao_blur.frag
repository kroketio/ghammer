#version 440

// Bilateral blur of the raw AO: the view-depth weight keeps it from crossing silhouettes; sigma grows with
// distance so it keeps its screen size.

layout(location = 0) in vec2 v_uv;

layout(std140, binding = 11) uniform SsaoUBO {
    mat4 u_clipFromView;
    vec4 u_params;           // w = depth sigma per unit depth
    vec4 u_info;             // x 1/aoWidth, y 1/aoHeight
};

layout(binding = 4) uniform sampler2D u_posTex;
layout(binding = 12) uniform sampler2D u_aoTex;

layout(location = 0) out vec4 fragColor;

void main() {
    float myDepth = -texture(u_posTex, v_uv).z;
    if (myDepth <= 0.0) {
        fragColor = vec4(1.0);
        return;
    }

    float sigma = max(0.25, abs(myDepth) * max(u_params.w, 1e-4));

    float sum = 0.0;
    float weight = 0.0;
    int r = clamp(int(u_info.w + 0.5), 1, 4); // blur radius follows quality
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            vec2 suv = v_uv + vec2(float(x), float(y)) * u_info.xy;
            float otherDepth = -texture(u_posTex, suv).z;
            float w = 1.0;
            if (myDepth > 0.0 && otherDepth > 0.0) {
                float d = abs(otherDepth - myDepth) / sigma;
                w = exp(-0.5 * d * d);
            }
            sum += texture(u_aoTex, suv).r * w;
            weight += w;
        }
    }

    fragColor = vec4(weight > 1e-5 ? sum / weight : 1.0);
}
