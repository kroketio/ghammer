#version 440

// G-buffer write. Cleared to zero: a zero view position means "no geometry", the SSAO pass's sky test.

layout(location = 0) in vec3 v_viewPos;
layout(location = 1) in vec3 v_viewNormal;

layout(location = 0) out vec4 fragPos;
layout(location = 1) out vec4 fragNormal;

void main() {
    fragPos = vec4(v_viewPos, 1.0);
    // Brush normals point inward; turn them outward, as the scene shader lights them.
    vec3 n = normalize(v_viewNormal);
    if (dot(n, normalize(-v_viewPos)) < 0.0)
        n = -n;
    fragNormal = vec4(n, 0.0);
}
