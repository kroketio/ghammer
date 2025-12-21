#version 440

// Point / spot light shadow write: distance to the light / range, per fragment from the interpolated light
// vector (shadow_point.vert: the distance itself must not be interpolated).

layout(location = 0) in vec3 v_lightVec;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = vec4(clamp(length(v_lightVec), 0.0, 1.0), 0.0, 0.0, 1.0);
}
