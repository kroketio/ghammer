#version 440

// Entity marker fragment stage: colour from the EntityUBO, so every entity type shares one pipeline pair.

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 14) uniform EntityUBO {
    mat4 u_model;
    vec4 u_color;
    vec4 u_info;
};

void main() {
    fragColor = u_color;
}
