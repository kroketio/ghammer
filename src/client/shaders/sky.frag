#version 440

// Sky (lit mode): with a skybox (u_lightCount.w = 1) the HDR panorama, sampled as Godot's PanoramaSkyMaterial
// and tone mapped like the scene; else a gradient from the ambient and fog colours with a sun disc.

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

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
    vec4 u_params1;
    vec4 u_fogColor;
    vec4 u_shadowParams;
    vec4 u_shadowFit;
    vec4 u_lightCount;   // w: 1 = a skybox panorama is bound
};

// Colour grading (Godot's adjustment_color_correction with a 3D LUT) after tone mapping, on the clamped
// sRGB colour. u_cameraPos.w = LUT size, 0 = none.
layout(binding = 15) uniform sampler3D u_colorLut;

// The skybox: equirectangular, linear HDR (RGBA16F), U wraps.
layout(binding = 13) uniform sampler2D u_panorama;

vec3 applyColorLut(vec3 c) {
    float n = u_cameraPos.w;
    if (n < 2.0)
        return c;
    // As Godot's tonemap.glsl: the colour is the coordinate, no texel-centre remap.
    return textureLod(u_colorLut, clamp(c, 0.0, 1.0), 0.0).rgb;
}


vec3 linearToSrgb(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

// Godot 4's tone mappers (servers/rendering/renderer_rd/shaders/effects/tonemap.glsl) for SDR (output_max_value 1),
// Environment defaults (white 1, AgX contrast 1.25): 0 Linear, 1 Reinhard, 2 Filmic, 3 ACES, 4 AgX.
vec3 tonemapFilmic(vec3 color) {
    const float bias = 2.0;
    const float A = 0.22 * bias * bias, B = 0.30 * bias, C = 0.10, D = 0.20, E = 0.01, F = 0.30;
    const float W = 1.0;
    float white = ((W * (A * W + C * B) + D * E) / (W * (A * W + B) + D * F)) - E / F;
    vec3 t = ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
    return t / white;
}

vec3 tonemapAces(vec3 color) {
    const float bias = 1.8;
    const float A = 0.0245786, B = 0.000090537, C = 0.983729, D = 0.432951, E = 0.238081;
    const mat3 rgbToRrt = mat3(vec3(0.59719 * bias, 0.35458 * bias, 0.04823 * bias),
                               vec3(0.07600 * bias, 0.90834 * bias, 0.01566 * bias),
                               vec3(0.02840 * bias, 0.13383 * bias, 0.83777 * bias));
    const mat3 odtToRgb = mat3(vec3(1.60475, -0.53108, -0.07367),
                               vec3(-0.10208, 1.10813, -0.00605),
                               vec3(-0.00327, -0.07276, 1.07602));
    const float W = 1.0 * bias;
    float white = (W * (W + A) - B) / (W * (C * W + D) + E);
    color *= rgbToRrt;
    vec3 t = (color * (color + A) - B) / (color * (C * color + D) + E);
    t *= odtToRgb;
    return t / white;
}

vec3 tonemapAgx(vec3 color) {
    const mat3 inset = mat3(0.544814746488245, 0.140416948464053, 0.0888104196149096,
                            0.373787398372697, 0.754137554567394, 0.178871756420858,
                            0.0813978551390581, 0.105445496968552, 0.732317823964232);
    const mat3 outset = mat3(1.96488741169489, -0.299313364904742, -0.164352742528393,
                             -0.855988495690215, 1.32639796461980, -0.238183969428088,
                             -0.108898916004672, -0.0270845997150571, 1.40253671195648);
    // allenwp curve, parameters as Godot computes them for white 2 (Forward+).
    const float crossover = 0.18, maxValue = 1.0, contrast = 1.25, highClip = 2.0;
    const float shoulderMax = maxValue - crossover;
    float toeA = ((1.0 / crossover) - 1.0) * pow(crossover, contrast);
    float slopeDen = pow(crossover, contrast) + toeA;
    float slope = (contrast * pow(crossover, contrast - 1.0) * toeA) / (slopeDen * slopeDen);
    float w = (highClip - crossover) * (highClip - crossover) / shoulderMax * slope;

    color = inset * color;
    vec3 s = color - crossover;
    vec3 ss = slope * s;
    s = ss * (1.0 + s / w) / (1.0 + ss / shoulderMax) + crossover;
    vec3 t = pow(max(color, vec3(0.0)), vec3(contrast));
    t = t / (t + toeA);
    color = mix(s, t, lessThan(color, vec3(crossover)));
    color = min(vec3(maxValue), color);
    return outset * color;
}

vec3 toneMap(vec3 c, int op) {
    if (op == 0)
        return c;  // Linear
    c = max(c, vec3(0.0));
    if (op == 1)
        return c * (1.0 + c) / (1.0 + c);  // Reinhard, white^2 / max = 1
    if (op == 2)
        return tonemapFilmic(c);
    if (op == 3)
        return tonemapAces(c);
    return tonemapAgx(c);
}

const float PI = 3.14159265358979;

void main() {
    vec2 ndc = v_uv * 2.0 - 1.0;
    mat4 inv = inverse(u_viewProj);
    vec4 far = inv * vec4(ndc, 1.0, 1.0);
    vec4 near = inv * vec4(ndc, 0.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - near.xyz / near.w);

    if (u_lightCount.w > 0.5) {
        // Godot's sky.glsl: u = atan(x, -z) / 2pi (wrapped), v = acos(y) / pi.
        // The export keeps the editor's axes, so the sky lines up in Godot.
        float u = atan(dir.x, -dir.z);
        if (u < 0.0)
            u += 2.0 * PI;
        vec2 st = vec2(u / (2.0 * PI), acos(clamp(dir.y, -1.0, 1.0)) / PI);
        vec3 c = textureLod(u_panorama, st, 0.0).rgb * u_params1.z;
        fragColor = vec4(applyColorLut(linearToSrgb(toneMap(c, int(u_fogColor.w + 0.5)))), 1.0);
        return;
    }

    vec3 L = normalize(-u_sunDir.xyz);
    float sunUp = clamp(L.y * 2.0 + 0.2, 0.0, 1.0);

    vec3 zenith = mix(vec3(0.05, 0.07, 0.12), u_ambient.rgb * 1.2, sunUp);
    vec3 horizon = mix(u_fogColor.rgb, u_sunColor.rgb * 0.6 + u_ambient.rgb * 0.4, 0.35 * sunUp);
    vec3 ground = u_ambient.rgb * 0.35;

    float h = dir.y;
    vec3 sky = h > 0.0 ? mix(horizon, zenith, pow(clamp(h, 0.0, 1.0), 0.45))
                       : mix(horizon, ground, clamp(-h * 4.0, 0.0, 1.0));

    // Sun: a soft disc plus a wide forward-scatter glow.
    float cosA = dot(dir, L);
    float disc = smoothstep(0.9994, 0.9998, cosA);
    float glow = pow(max(cosA, 0.0), 48.0) * 0.35 + pow(max(cosA, 0.0), 6.0) * 0.08;
    vec3 sun = u_sunColor.rgb * u_sunColor.w * (disc * 8.0 + glow) * step(-0.05, L.y);

    vec3 color = (sky * max(u_ambient.w, 0.15) * 1.6 + sun) * u_params1.z;
    fragColor = vec4(applyColorLut(linearToSrgb(toneMap(color, int(u_fogColor.w + 0.5)))), 1.0);
}
