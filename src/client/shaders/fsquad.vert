#version 440

// Fullscreen quad for the SSAO chain. Clip space already carries QRhi's correction matrix, so ndc * 0.5 + 0.5
// has the projected passes' framebuffer orientation and the AO buffer lines up with the scene.

layout(location = 0) in vec2 a_ndc;

layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = a_ndc * 0.5 + 0.5;
    gl_Position = vec4(a_ndc, 0.0, 1.0);
}
