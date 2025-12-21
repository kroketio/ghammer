#version 440

// Tool brush overlay (collision, triggers): the colour is per vertex, already
// shaded on the CPU (vk_collision.cpp).
layout(location = 0) in vec4 v_color;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = v_color;
}
