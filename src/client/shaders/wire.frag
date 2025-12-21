#version 440

// Screen-space thick lines: barycentric weight / its derivative = distance in pixels.
// Drawn without depth test; it fades by how far the scene depth texture puts something in front, down to a floor.

layout(location = 0) in vec3 v_bary;
layout(location = 1) in float v_viewDist;

layout(std140, binding = 0) uniform SceneUBO {
    mat4 u_viewProj;
    mat4 u_view;
    mat4 u_lightView;
    mat4 u_lightClip;
    vec4 u_cameraPos;
    vec4 u_sunDir;
    vec4 u_sunColor;
    vec4 u_ambient;
    vec4 u_params0;
    vec4 u_params1;             // w = 1 / camera far
    vec4 u_fogColor;
    vec4 u_shadowParams;
    vec4 u_shadowFit;
};

layout(std140, binding = 3) uniform OverlayUBO {
    vec4 u_wireColor;
    vec4 u_wireParams;          // x thickness px, y push, z fade span in world units, w min alpha
};

layout(binding = 9) uniform highp sampler2D u_sceneDepth;

layout(location = 0) out vec4 fragColor;

void main() {
    vec3 rate = fwidth(v_bary);
    vec3 dist = v_bary / max(rate, vec3(1e-6));
    float nearest = min(min(dist.x, dist.y), dist.z);

    float thickness = max(u_wireParams.x, 1.0);
    float alpha = 1.0 - clamp(nearest / thickness, 0.0, 1.0);
    if (alpha <= 0.002)
        discard;

    float visible = 1.0;
    ivec2 texel = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(u_sceneDepth, 0);
    if (texel.x >= 0 && texel.y >= 0 && texel.x < size.x && texel.y < size.y) {
        float packed = texelFetch(u_sceneDepth, texel, 0).r;
        // 0 is the clear value: no geometry there, so nothing hides this wire.
        if (packed > 0.0) {
            float sceneDist = packed / max(u_params1.w, 1e-9);
            float gap = v_viewDist - sceneDist;
            float occ = clamp(gap / max(u_wireParams.z, 1e-3), 0.0, 1.0);
            visible = mix(1.0, u_wireParams.w, occ);
        }
    }

    fragColor = vec4(u_wireColor.rgb, u_wireColor.a * alpha * visible);
}
