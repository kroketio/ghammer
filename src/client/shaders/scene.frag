#version 440

// Material and lighting. Fullbright is deliberately not flat (per-face tints + a grazing lift keep walls readable);
// Component mode is the lit one, in linear space and tone mapped on the way out.

layout(location = 0) in vec3 v_worldPos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec3 v_tangent;
layout(location = 4) in vec3 v_bitangent;
layout(location = 6) flat in uint v_face;

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
    vec4 u_shadowFit;          // x world units per shadow texel, y light far, z light near, w shadow render distance (world units, 0 = unlimited)
    vec4 u_lightCount;
    vec4 u_lights[32 * 4];
    mat4 u_cascadeView[4];
    mat4 u_cascadeClip[4];
    vec4 u_cascadeUv[4];
    vec4 u_cascadeSplits;
    vec4 u_cascadeTexel;
    vec4 u_cascadeFar;
    vec4 u_cascadeInfo;   // x count, y blend band (fraction), z debug tint
    vec4 u_lightShadow[32];  // x first atlas tile (-1 none), y tan(half fov), z depth bias (world), w normal bias
    vec4 u_lightAtlasInfo;   // x atlas px, y NDC y sign, z tiles per row, w any
};

layout(binding = 14) uniform sampler2D u_lightShadowAtlas;

// Colour grading (Godot's adjustment_color_correction with a 3D LUT) after tone mapping, on the clamped
// sRGB colour. u_cameraPos.w = LUT size, 0 = none.
layout(binding = 15) uniform sampler3D u_colorLut;

vec3 applyColorLut(vec3 c) {
    float n = u_cameraPos.w;
    if (n < 2.0)
        return c;
    // As Godot's tonemap.glsl: the colour is the coordinate, no texel-centre remap.
    return textureLod(u_colorLut, clamp(c, 0.0, 1.0), 0.0).rgb;
}


struct FaceData {
    vec4 faceInfo;   // Godot params: metallic, roughness, metallic_specular, normal_scale
    uvec4 pickInfo;
    vec4 uvWorldU;
    vec4 uvWorldV;
    vec4 matInfo;    // ao_light_affect, emission_energy
    vec4 emission;   // linear emission colour
};
layout(std430, binding = 2) readonly buffer FaceBuffer {
    FaceData faces[];
};
// The face's flags and material parameters, read once in main() (draws cover runs of faces).
uvec4 u_pickInfo;
vec4 u_matParams;
vec4 u_matInfo;

layout(binding = 4) uniform sampler2D u_albedoMap;
layout(binding = 5) uniform sampler2D u_normalMap;
// ORM: R = ambient occlusion, G = roughness, B = metallic; a missing map is white (Godot's default).
layout(binding = 6) uniform sampler2D u_mrMap;
layout(binding = 7) uniform sampler2D u_emissionMap;
layout(binding = 8) uniform sampler2D u_shadowMap;

layout(location = 0) out vec4 fragColor;

const uint FLAG_HAS_NORMAL = 1u;
const uint FLAG_HAS_MR = 2u;
const uint FLAG_HAS_EMISSION_MAP = 4u;
const uint FLAG_SELECTED = 8u;
const uint FLAG_SELECTED_FACE = 16u;
const uint FLAG_TRANSPARENT = 64u;
const uint FLAG_UNSHADED = 128u;
const uint FLAG_EMISSION = 256u;

// +X, -X, +Y, -Y, +Z, -Z. Floors (+Y) brightest, ceilings (-Y) darkest.
const float FACE_SHADES[6] = float[6](0.94, 1.00, 1.06, 0.96, 0.84, 0.90);

float faceAxisShade(vec3 n) {
    vec3 a = abs(n);
    int axis;
    if (a.x > a.y && a.x > a.z) axis = n.x > 0.0 ? 0 : 1;
    else if (a.y > a.z) axis = n.y > 0.0 ? 2 : 3;
    else axis = n.z > 0.0 ? 4 : 5;
    return FACE_SHADES[axis];
}

vec3 shadeNormal() {
    if ((u_pickInfo.z & FLAG_HAS_NORMAL) == 0u)
        return normalize(v_normal);

    vec3 t = normalize(v_tangent);
    vec3 b = normalize(v_bitangent);
    vec3 n = texture(u_normalMap, v_uv).xyz * 2.0 - 1.0;
    n.xy *= u_matParams.w * u_sunDir.w;  // normal_scale (Godot NORMAL_MAP_DEPTH) x Render/NormalStrength
    return normalize(t * n.x + b * n.y + normalize(v_normal) * max(n.z, 0.3));
}

// Poisson disk rotated by a per-pixel hash: a fixed kernel bands on large flat surfaces, noise does not.
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Everything below works in WORLD units, not normalised 0..1 depth: the shadow volume changes size with the
// camera every frame, so a normalised bias would mean a different distance each time.
// 2x2 percentage-closer test with bilinear weights: four depth comparisons from one gather.
float pcfBilinear(vec2 uv, float receiver, float bias, float farPlane, float texSize) {
    vec2 st = uv * texSize - 0.5;
    vec2 f = fract(st);
    vec2 centre = (floor(st) + 1.0) / texSize; // between the 4 texels
    vec4 d = textureGather(u_shadowMap, centre, 0) * farPlane;
    // gather order: (0,1) (1,1) (1,0) (0,0)
    vec4 lit = step(vec4(receiver - bias), d);
    float top = mix(lit.w, lit.z, f.x);
    float bottom = mix(lit.x, lit.y, f.x);
    return mix(top, bottom, f.y);
}

const vec2 POISSON16[16] = vec2[16](
    vec2(-0.94201624, -0.39906216), vec2( 0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760), vec2(-0.91588581,  0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845), vec2( 0.97484398,  0.75648379), vec2( 0.44323325, -0.97511554),
    vec2( 0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2( 0.79197514,  0.19090188),
    vec2(-0.24188840,  0.99706507), vec2(-0.81409955,  0.91437590), vec2( 0.19984126,  0.78641367),
    vec2( 0.14383161, -0.14100790));

// Entity light shadow from the atlas. Face selection and projection must match faceAxes()/spotUp() in
// vk_light_shadows.cpp. Bias as Godot's lights: info.z = shadow_bias (world units), info.w = shadow_normal_bias,
// the lookup moved out along the normal by that many texels × (1 - |n·l|) (keeps slanted faces free of acne).
bool lightAtlasTexel(vec3 d, int tile0, bool spot, vec3 spotDir, float tanHalf, out int tile, out float depthF,
                     out vec2 local) {
    vec3 F, U;
    tile = tile0;
    if (spot) {
        F = normalize(spotDir);
        U = abs(F.y) > 0.99 ? vec3(0, 0, 1) : vec3(0, 1, 0);
    } else {
        vec3 a = abs(d);
        int face;
        if (a.x >= a.y && a.x >= a.z) face = d.x > 0.0 ? 0 : 1;
        else if (a.y >= a.z) face = d.y > 0.0 ? 2 : 3;
        else face = d.z > 0.0 ? 4 : 5;
        const vec3 Fs[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));
        const vec3 Us[6] = vec3[6](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, -1), vec3(0, 0, 1), vec3(0, 1, 0), vec3(0, 1, 0));
        F = Fs[face];
        U = Us[face];
        tile += face;
    }
    vec3 R = normalize(cross(F, U));
    vec3 Up = cross(R, F);
    depthF = dot(d, F);
    if (depthF <= 1e-4)
        return false;
    vec2 ndc = vec2(dot(d, R), dot(d, Up)) / (depthF * tanHalf);
    local = ndc;
    return true;
}

float lightAtlasShadow(int li, vec3 lightPos, float range, bool spot, vec3 spotDir, vec3 n) {
    vec4 info = u_lightShadow[li];
    if (info.x < -0.5 || u_lightAtlasInfo.w < 0.5)
        return 1.0;
    float perRow = u_lightAtlasInfo.z;
    float atlas = u_lightAtlasInfo.x;
    float tileUv = 1.0 / perRow;
    int tile0 = int(info.x + 0.5);

    // Texel size where the fragment is, for the normal offset.
    vec3 d0 = v_worldPos - lightPos;
    int tile;
    float depthF;
    vec2 ndc;
    if (!lightAtlasTexel(d0, tile0, spot, spotDir, info.y, tile, depthF, ndc))
        return 1.0;
    float texelWorld = 2.0 * depthF * info.y / (atlas * tileUv);
    vec3 L = -normalize(d0);
    float ndl = dot(n, L);
    vec3 d = d0 + n * (texelWorld * 2.5 * info.w * (1.0 - abs(ndl)));
    float dist = length(d);
    if (!lightAtlasTexel(d, tile0, spot, spotDir, info.y, tile, depthF, ndc))
        return 1.0;
    if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0)
        return spot ? 0.0 : 1.0; // outside a spot cone: no light anyway
    ndc.y *= u_lightAtlasInfo.y;
    vec2 local = ndc * 0.5 + 0.5;

    float col = mod(float(tile), perRow);
    float row = floor(float(tile) / perRow);
    vec2 origin = vec2(col * tileUv, 1.0 - (row + 1.0) * tileUv);
    vec2 lo = origin + vec2(1.5 / atlas), hi = origin + vec2(tileUv) - vec2(1.5 / atlas);
    vec2 uv = clamp(origin + local * tileUv, lo, hi);

    // A texel's worth for the depth quantisation, plus the light's own bias.
    float receiver = dist - (texelWorld * (0.5 + 0.5 * (1.0 - max(ndl, 0.0))) + info.z);

    vec2 st = uv * atlas - 0.5;
    vec2 f = fract(st);
    vec4 g = textureGather(u_lightShadowAtlas, (floor(st) + 1.0) / atlas, 0) * range;
    vec4 lit = step(vec4(receiver), g);
    return mix(mix(lit.w, lit.z, f.x), mix(lit.x, lit.y, f.x), f.y);
}

// One cascade's shadow factor, or -1 when the point falls outside its tile.
float cascadeShadow(int ci, vec3 n, vec3 lightDir, float ndl, float slope) {
    float texelWorld = max(u_cascadeTexel[ci], 1e-5);
    float farPlane = max(u_cascadeFar[ci], 1.0);
    float atlasSize = 1.0 / max(u_params0.z, 1e-6);
    vec4 tileUv = u_cascadeUv[ci];
    float tileSize = atlasSize * tileUv.z;

    vec3 biased = v_worldPos + n * texelWorld * (1.0 + u_shadowParams.z) * (0.5 + slope);
    vec4 lightViewPos = u_cascadeView[ci] * vec4(biased, 1.0);
    vec4 lc = u_cascadeClip[ci] * vec4(biased, 1.0);
    if (lc.w <= 0.0)
        return -1.0;
    vec2 local = lc.xy / lc.w * 0.5 + 0.5;
    if (local.x < 0.0 || local.x > 1.0 || local.y < 0.0 || local.y > 1.0)
        return -1.0;
    float receiver = -lightViewPos.z;
    float bias = texelWorld * (0.6 + 1.5 * slope / max(ndl, 0.2)) + u_params1.y * 200.0 * texelWorld;

    int mode = int(u_shadowParams.w) / 100;
    int taps = clamp(int(u_shadowParams.w) - mode * 100, 1, 32);
    float radius = max(u_shadowParams.x, 0.0);

    // Keep every tap inside this cascade's tile of the atlas.
    vec2 lo = tileUv.xy + vec2(1.5 / atlasSize);
    vec2 hi = tileUv.xy + vec2(tileUv.z) - vec2(1.5 / atlasSize);
    vec2 uv = tileUv.xy + local * tileUv.z;

    float angle = hash12(gl_FragCoord.xy) * 6.2831853;
    float ca = cos(angle), sa = sin(angle);
    mat2 rot = mat2(ca, sa, -sa, ca);

    if (mode == 0)
        return pcfBilinear(clamp(uv, lo, hi), receiver, bias, farPlane, atlasSize);

    if (mode == 2) {
        float searchTexels = max(radius, 1.0) * 6.0;
        float blockers = 0.0, blockerSum = 0.0;
        for (int i = 0; i < 16; ++i) {
            vec2 off = rot * POISSON16[i] * searchTexels / atlasSize;
            float d = texture(u_shadowMap, clamp(uv + off, lo, hi)).r * farPlane;
            if (d < receiver - bias) {
                blockerSum += d;
                blockers += 1.0;
            }
        }
        if (blockers < 0.5)
            return 1.0;
        float penumbra = (receiver - blockerSum / blockers) / texelWorld * 0.06 * max(radius, 0.25);
        radius = clamp(penumbra, 0.5, 24.0);
    }

    float lit = 0.0;
    int n16 = min(taps, 16);
    for (int i = 0; i < n16; ++i)
        lit += pcfBilinear(clamp(uv + rot * POISSON16[i] * radius / atlasSize, lo, hi), receiver, bias, farPlane, atlasSize);
    if (taps > 16) {
        mat2 rot2 = mat2(ca, -sa, sa, ca);
        for (int i = 0; i < 16; ++i)
            lit += pcfBilinear(clamp(uv + rot2 * POISSON16[i] * radius * 0.6 / atlasSize, lo, hi), receiver, bias,
                               farPlane, atlasSize);
    }
    lit /= float(taps > 16 ? 32 : n16);
    // Single cascade covering the whole map: fade at the volume's edge.
    if (u_cascadeInfo.x < 1.5) {
        vec2 edge = min(local, vec2(1.0) - local);
        lit = mix(1.0, lit, clamp(min(edge.x, edge.y) * 32.0, 0.0, 1.0));
    }
    return lit;
}

vec3 g_cascadeTint = vec3(1.0);

float shadowTerm(vec3 n, vec3 lightDir) {
    if (u_params0.y < 0.5)
        return 1.0;

    // Past the shadow distance there is no shadow; the tail fades to lit.
    float fade = 1.0;
    if (u_shadowFit.w > 0.0) {
        float camDist = length(u_cameraPos.xyz - v_worldPos);
        if (camDist > u_shadowFit.w)
            return 1.0;
        fade = clamp((u_shadowFit.w - camDist) / (u_shadowFit.w * 0.125), 0.0, 1.0);
    }

    float ndl = max(dot(n, lightDir), 0.0);
    float slope = sqrt(1.0 - ndl * ndl);
    int count = int(u_cascadeInfo.x + 0.5);
    // Distance from the camera, not view depth: cascades are spheres, so turning the view never changes them.
    float viewDepth = length(v_worldPos - u_cameraPos.xyz);

    int ci = 0;
    if (count > 1) {
        ci = count;
        for (int i = 0; i < 4; ++i)
            if (i < count && viewDepth <= u_cascadeSplits[i]) {
                ci = i;
                break;
            }
        if (ci >= count)
            return 1.0;
    }
    float lit = cascadeShadow(ci, n, lightDir, ndl, slope);
    // Outside this cascade's tile: use the next (coarser) one; calling it lit flashes bands at cascade edges.
    while (lit < 0.0 && ci + 1 < count) {
        ++ci;
        lit = cascadeShadow(ci, n, lightDir, ndl, slope);
    }
    if (lit < 0.0)
        return 1.0;

    // Blend into the next cascade over the last part of this one, hiding the seam.
    if (count > 1 && ci + 1 < count) {
        float end = u_cascadeSplits[ci];
        float start = ci == 0 ? 0.0 : u_cascadeSplits[ci - 1];
        float band = (end - start) * u_cascadeInfo.y;
        float t = clamp((viewDepth - (end - band)) / max(band, 1e-3), 0.0, 1.0);
        if (t > 0.0) {
            float next = cascadeShadow(ci + 1, n, lightDir, ndl, slope);
            if (next >= 0.0)
                lit = mix(lit, next, t);
        }
    }
    if (u_cascadeInfo.z > 0.5) {
        const vec3 tints[4] = vec3[4](vec3(1.0, 0.6, 0.6), vec3(0.6, 1.0, 0.6), vec3(0.6, 0.7, 1.0), vec3(1.0, 1.0, 0.5));
        g_cascadeTint = tints[ci];
    }
    return mix(1.0, lit, clamp(u_shadowParams.y, 0.0, 1.0) * fade);
}

// Cook-Torrance: GGX distribution, Smith visibility (Schlick-GGX) and Schlick fresnel.
const float PI = 3.14159265359;
// Direct specular (sun, omni and spot lights) scaled up so light_specular 1 here looks like Godot's.
const float SPECULAR_MATCH = 3.0;

float distributionGGX(vec3 n, vec3 h, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float nh = max(dot(n, h), 0.0);
    float d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

float visibilitySchlick(float cosTheta, float k) {
    return cosTheta / max(cosTheta * (1.0 - k) + k, 1e-7);
}

float visibilitySmith(vec3 n, vec3 v, vec3 l, float rough) {
    float r = rough + 1.0;
    float k = (r * r) / 8.0;
    return visibilitySchlick(max(dot(n, v), 0.0), k)
         * visibilitySchlick(max(dot(n, l), 0.0), k);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Godot's BRDF terms (scene_forward_lights_inc.glsl) for the entity lights, as OmniLight3D / SpotLight3D:
// Heitz D in Godot's form, Hammon visibility, Fresnel f90 clamped against metallic rather than 1.
float schlickPow5(float u) {
    float m = 1.0 - u;
    float m2 = m * m;
    return m2 * m2 * m;
}

float godotD(vec3 n, vec3 h, float alpha) {
    float noh = max(dot(n, h), 0.0);
    float a = noh * alpha;
    float k = alpha / (1.0 - noh * noh + a * a);
    return clamp(k * k / PI, 0.0, 1.0);
}

float godotV(float ndl, float ndv, float alpha) {
    return clamp(0.5 / mix(2.0 * ndl * ndv, ndl + ndv, alpha), 0.0, 1.0);
}

vec3 godotF(vec3 f0, float cosTheta, float metallic) {
    float f90 = clamp(dot(f0, vec3(50.0 * 0.33)), metallic, 1.0);
    return f0 + (f90 - f0) * schlickPow5(cosTheta);
}

// Godot's default diffuse, Burley: lifts grazing angles by FdV * FdL, unlike Lambert.
float godotDiffuseBurley(float ndl, float ndv, float ldh, float roughness) {
    float fd90Minus1 = 2.0 * ldh * ldh * roughness - 0.5;
    float fdV = 1.0 + fd90Minus1 * schlickPow5(ndv);
    float fdL = 1.0 + fd90Minus1 * schlickPow5(ndl);
    return fdV * fdL * ndl / PI;
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

void main() {
    u_pickInfo = faces[v_face].pickInfo;
    u_matParams = faces[v_face].faceInfo;
    u_matInfo = faces[v_face].matInfo;
    vec4 albedoTex = texture(u_albedoMap, v_uv);
    // Godot transparency, as alpha scissor (threshold 0.5).
    if ((u_pickInfo.z & FLAG_TRANSPARENT) != 0u && albedoTex.a < 0.5)
        discard;
    vec3 albedo = albedoTex.rgb;
    vec3 N = shadeNormal();
    vec3 V = normalize(u_cameraPos.xyz - v_worldPos);

    // Source brush planes point into the brush, so visible faces arrive with inward normals: turn the normal
    // toward the eye (stable for a closed brush seen from outside).
    if (dot(N, V) < 0.0)
        N = -N;

    // Godot StandardMaterial3D: METALLIC = texture * metallic, ROUGHNESS = texture * roughness, AO from its channel.
    vec3 orm = texture(u_mrMap, v_uv).rgb;
    float ao = orm.r;
    float roughness = clamp(orm.g * u_matParams.y, 0.0, 1.0);
    float metallic = clamp(orm.b * u_matParams.x, 0.0, 1.0);
    // Dielectric reflectance from metallic_specular, like Godot's F0():
    // 0.16 * specular^2 (0.04 at the default 0.5).
    float dielectric = 0.16 * u_matParams.z * u_matParams.z;
    // ao_light_affect: how much AO also darkens direct light (ambient always).
    float aoDirect = mix(1.0, ao, u_matInfo.x);

    vec3 emissionColor = vec3(0.0);
    if ((u_pickInfo.z & FLAG_EMISSION) != 0u) {
        vec3 emissionTex = (u_pickInfo.z & FLAG_HAS_EMISSION_MAP) != 0u ? texture(u_emissionMap, v_uv).rgb : vec3(0.0);
        emissionColor = (faces[v_face].emission.rgb + emissionTex) * u_matInfo.y;
    }

    vec3 color;
    vec3 F0L = mix(vec3(dielectric), albedo, metallic);
    float metallicL = metallic;
    bool unshaded = (u_pickInfo.z & FLAG_UNSHADED) != 0u;

    if (unshaded) {
        // shading_mode = unshaded: the albedo as is (still tone mapped, like Godot).
        color = albedo;
    } else if (u_params0.x < 0.5) {
        // Fullbright: no shadows, no specular. Just enough shaping to read.
        float grazing = 1.0 - clamp(dot(N, V), 0.0, 1.0);
        float shade = faceAxisShade(N) * mix(1.0, 1.10, grazing * 0.4);
        color = albedo * shade * mix(0.88, 1.0, ao) * 1.15;
    } else {
        vec3 L = normalize(-u_sunDir.xyz);
        float shadow = shadowTerm(N, L);
        albedo *= g_cascadeTint;
        float NdotL = max(dot(N, L), 0.0);

        vec3 diffuseColor = albedo * (1.0 - metallic);
        vec3 F0 = mix(vec3(dielectric), albedo, metallic);
        vec3 H = normalize(L + V);

        // Specular off (u_lightCount.z = 0): diffuse only, keeping what the specular lobe would have taken.
        float specScale = u_lightCount.z;
        float specOn = specScale > 0.0 ? 1.0 : 0.0;
        vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
        vec3 specular = distributionGGX(N, H, roughness) * visibilitySmith(N, V, L, roughness) * F * specScale
                        * SPECULAR_MATCH;
        // diffuseColor already carries (1 - metallic); don't apply it again here.
        vec3 kD = vec3(1.0) - F * specOn;

        vec3 radiance = u_sunColor.rgb * u_sunColor.w;
        vec3 direct = (kD * diffuseColor / PI + specular) * radiance * NdotL * shadow * aoDirect;

        // Hemisphere ambient (no irradiance probe): sky above, ground bounce below.
        // Same 1/PI as the direct diffuse (both Lambertian); normalising only the direct term washes shadows out.
        float hemi = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
        vec3 irradiance = mix(u_ambient.rgb * vec3(0.78, 0.74, 0.70), u_ambient.rgb, hemi)
                      * u_ambient.w;
        vec3 ambientDiffuse = irradiance * diffuseColor * ao / PI;

        // Ambient specular without an environment map: the sky colour, fresnel-weighted, fading with roughness.
        // Not scaled by (1 - metallic): fully metallic surfaces would turn black away from the sun.
        float NdotV = max(dot(N, V), 0.0);
        vec3 Fr = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
        float gloss = mix(0.35, 1.0, 1.0 - roughness);
        vec3 ambientSpec = irradiance * Fr * gloss * ao / PI * 2.0 * specScale;

        color = ambientDiffuse + ambientSpec + direct;
    }

    // Entity lights as Godot's forward shader (light_process_omni / light_process_spot): get_omni_attenuation
    // over metres, the spot rim, specular scaled by specular_amount.
    // u_lights: (pos, range) (linear rgb, energy*PI) (dir, cos(spot_angle) | -2)
    // (spot_angle_attenuation, attenuation, isSpot, light_specular*2).
    if (u_params0.x >= 0.5 && !unshaded) {
        int nLights = int(u_lightCount.x);
        for (int li = 0; li < nLights; ++li) {
            vec4 lp = u_lights[li * 4 + 0];
            vec4 lc = u_lights[li * 4 + 1];
            vec4 ld = u_lights[li * 4 + 2];
            vec4 lx = u_lights[li * 4 + 3];
            vec3 toL = lp.xyz - v_worldPos;
            float d = length(toL);
            float range = max(lp.w, 0.01);
            if (d >= range)
                continue;
            vec3 Ld = toL / max(d, 1e-4);
            float ndl = max(dot(N, Ld), 0.0);
            if (ndl <= 0.0)
                continue;
            float nd = d / range;
            nd = nd * nd * nd * nd;
            float atten = max(1.0 - nd, 0.0);
            // Distance in metres (u_lightCount.y = world units per metre).
            atten *= atten * pow(max(d / u_lightCount.y, 1e-4), -lx.y);
            if (lx.z > 0.5) {
                float cone = ld.w;
                float scos = max(dot(-Ld, normalize(ld.xyz)), cone);
                float rim = max(1e-4, (1.0 - scos) / max(1.0 - cone, 1e-4));
                atten *= 1.0 - pow(rim, lx.x);
            }
            if (atten <= 0.0)
                continue;
            atten *= lightAtlasShadow(li, lp.xyz, range, lx.z > 0.5, ld.xyz, N);
            if (atten <= 0.0)
                continue;
            vec3 h = normalize(Ld + V);
            float ndv = max(dot(N, V), 1e-4);
            float alpha = roughness * roughness;
            vec3 Fl = godotF(F0L, max(dot(h, V), 0.0), metallicL);
            vec3 spec = godotD(N, h, alpha) * godotV(ndl, ndv, alpha) * Fl * lx.w * u_lightCount.z * SPECULAR_MATCH;
            float diff = godotDiffuseBurley(ndl, ndv, clamp(dot(Ld, h), 0.0, 1.0), roughness);
            color += (diff * albedo * (1.0 - metallicL) + spec) * lc.rgb * (lc.w * atten) * aoDirect;
        }
    }

    color += emissionColor;

    float dist = length(u_cameraPos.xyz - v_worldPos);
    float fog = 1.0 - exp(-pow(max(dist * u_params1.x, 0.0), 1.6));
    color = mix(color, u_fogColor.rgb, clamp(fog, 0.0, 1.0));

    // u_fogColor.w selects the tone mapper; output uses the real sRGB transfer function, not a 2.2 power.
    color = toneMap(color * u_params1.z, int(u_fogColor.w + 0.5));
    color = linearToSrgb(color);
    color = applyColorLut(color);

    // Selection highlight, after tone map and gamma so the tone curve does not absorb it.
    // Material mode faces get a red tint, selected objects a plain darkening.
    if ((u_pickInfo.z & FLAG_SELECTED_FACE) != 0u)
        color = mix(color, vec3(0.86, 0.13, 0.10), 0.38);
    else if ((u_pickInfo.z & FLAG_SELECTED) != 0u)
        color *= 0.90;

    fragColor = vec4(color, 1.0);
}
