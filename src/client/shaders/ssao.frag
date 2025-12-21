#version 440

// Screen-space hemisphere occlusion. The kernel is rotated per pixel so its pattern becomes noise the blur
// averages away.

layout(location = 0) in vec2 v_uv;

layout(std140, binding = 11) uniform SsaoUBO {
    mat4 u_clipFromView;      // corr * proj
    vec4 u_params;           // x radius (world units), y sample bias, z intensity, w unused
    vec4 u_info;             // x 1/aoWidth, y 1/aoHeight, zw unused
};

layout(binding = 4) uniform sampler2D u_posTex;
layout(binding = 5) uniform sampler2D u_nrmTex;

layout(location = 0) out vec4 fragColor;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    vec3 surfPos = texture(u_posTex, v_uv).rgb;
    vec3 surfNrm = texture(u_nrmTex, v_uv).rgb;

    // Zero normal means the g-buffer was never written here: sky, no occlusion.
    if (dot(surfNrm, surfNrm) < 0.5) {
        fragColor = vec4(1.0);
        return;
    }

    vec3 N = normalize(surfNrm);
    vec3 up = abs(N.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(N, up));
    vec3 B = cross(N, T);

    float ang = hash12(gl_FragCoord.xy) * 6.2831853;
    float ca = cos(ang);
    float sa = sin(ang);
    mat3 rot = mat3(vec3(ca, -sa, 0.0), vec3(sa, ca, 0.0), vec3(0.0, 0.0, 1.0));
    mat3 tbn = mat3(T, B, N) * rot;

    float radius = max(u_params.x, 1e-4);
    float bias = max(u_params.y, 1e-4);
    float myDepth = -surfPos.z;
    if (myDepth <= 0.0) {
        fragColor = vec4(1.0);
        return;
    }

    float occluded = 0.0;
    // Sample count from the quality pref (8 / 16 / 32). Kernel: golden-angle spiral over the hemisphere, cosine
    // weighted, packed towards the surface.
    int sampleCount = clamp(int(u_info.z + 0.5), 4, 64);
    for (int i = 0; i < sampleCount; ++i) {
        float t = (float(i) + 0.5) / float(sampleCount);
        float phi = float(i) * 2.39996323;
        float rr = sqrt(t);
        vec3 k = vec3(rr * cos(phi), rr * sin(phi), sqrt(max(0.0, 1.0 - t)));
        float scale = mix(0.1, 1.0, t * t);
        vec3 samplePos = surfPos + tbn * (k * scale) * radius;
        vec4 clip = u_clipFromView * vec4(samplePos, 1.0);
        if (clip.w <= 0.0)
            continue;

        vec2 suv = clip.xy / clip.w * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0)
            continue;

        float occluderDepth = -texture(u_posTex, suv).z;
        if (occluderDepth <= 0.0)
            continue;

        // Occluded when the surface at the sample's pixel is in front of the *sample point*: comparing against
        // the centre pixel makes sloped floors occlude themselves.
        float sampleDepth = -samplePos.z;
        if (occluderDepth < sampleDepth - bias) {
            // Range check: geometry far in front of this pixel does not count.
            occluded += smoothstep(0.0, 1.0, radius / abs(myDepth - occluderDepth));
        }
    }

    float ao = 1.0 - clamp((occluded / float(sampleCount)) * max(u_params.z, 0.0), 0.0, 1.0);
    fragColor = vec4(ao, ao, ao, 1.0);
}
