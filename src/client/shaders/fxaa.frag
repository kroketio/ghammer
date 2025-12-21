#version 440

// FXAA (after Lottes' FXAA 3.11, console/quality hybrid) on the finished, display-encoded frame.

layout(location = 0) in vec2 v_uv;
layout(binding = 0) uniform sampler2D u_src;
layout(location = 0) out vec4 fragColor;

const float EDGE_THRESHOLD = 0.125;
const float EDGE_THRESHOLD_MIN = 0.0312;
const float SUBPIX = 0.75;
const int SEARCH_STEPS = 10;
const float SEARCH_STEP_SIZE[10] = float[10](1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
float lumaAt(vec2 uv) { return luma(textureLod(u_src, uv, 0.0).rgb); }

void main() {
    vec2 px = 1.0 / vec2(textureSize(u_src, 0));
    vec4 centre = textureLod(u_src, v_uv, 0.0);
    float lM = luma(centre.rgb);
    float lN = lumaAt(v_uv + vec2(0.0, -px.y));
    float lS = lumaAt(v_uv + vec2(0.0, px.y));
    float lE = lumaAt(v_uv + vec2(px.x, 0.0));
    float lW = lumaAt(v_uv + vec2(-px.x, 0.0));

    float lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float lMin = min(lM, min(min(lN, lS), min(lE, lW)));
    float range = lMax - lMin;
    if (range < max(EDGE_THRESHOLD_MIN, lMax * EDGE_THRESHOLD)) {
        fragColor = centre;
        return;
    }

    float lNW = lumaAt(v_uv + vec2(-px.x, -px.y));
    float lNE = lumaAt(v_uv + vec2(px.x, -px.y));
    float lSW = lumaAt(v_uv + vec2(-px.x, px.y));
    float lSE = lumaAt(v_uv + vec2(px.x, px.y));

    // Sub-pixel aliasing amount from the 3x3 low-pass.
    float avg = (2.0 * (lN + lS + lE + lW) + lNW + lNE + lSW + lSE) / 12.0;
    float subpix = clamp(abs(avg - lM) / range, 0.0, 1.0);
    subpix = smoothstep(0.0, 1.0, subpix);
    subpix = subpix * subpix * SUBPIX;

    float edgeH = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    float edgeV = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    bool horizontal = edgeH >= edgeV;

    float l1 = horizontal ? lN : lW;   // negative side
    float l2 = horizontal ? lS : lE;   // positive side
    float g1 = abs(l1 - lM);
    float g2 = abs(l2 - lM);
    float stepLen = horizontal ? px.y : px.x;
    float lLocal;
    float gScaled;
    if (g1 >= g2) {
        stepLen = -stepLen;
        lLocal = 0.5 * (l1 + lM);
        gScaled = 0.25 * g1;
    } else {
        lLocal = 0.5 * (l2 + lM);
        gScaled = 0.25 * g2;
    }

    vec2 uvEdge = v_uv;
    if (horizontal)
        uvEdge.y += stepLen * 0.5;
    else
        uvEdge.x += stepLen * 0.5;
    vec2 dir = horizontal ? vec2(px.x, 0.0) : vec2(0.0, px.y);

    vec2 uv1 = uvEdge - dir;
    vec2 uv2 = uvEdge + dir;
    float e1 = lumaAt(uv1) - lLocal;
    float e2 = lumaAt(uv2) - lLocal;
    bool done1 = abs(e1) >= gScaled;
    bool done2 = abs(e2) >= gScaled;
    for (int i = 1; i < SEARCH_STEPS && !(done1 && done2); ++i) {
        if (!done1) {
            uv1 -= dir * SEARCH_STEP_SIZE[i];
            e1 = lumaAt(uv1) - lLocal;
            done1 = abs(e1) >= gScaled;
        }
        if (!done2) {
            uv2 += dir * SEARCH_STEP_SIZE[i];
            e2 = lumaAt(uv2) - lLocal;
            done2 = abs(e2) >= gScaled;
        }
    }

    float d1 = horizontal ? (v_uv.x - uv1.x) : (v_uv.y - uv1.y);
    float d2 = horizontal ? (uv2.x - v_uv.x) : (uv2.y - v_uv.y);
    bool nearer1 = d1 < d2;
    float dMin = min(d1, d2);
    float edgeLen = d1 + d2;

    // Only blend when the luma at the nearer end moves the opposite way to the
    // centre (otherwise this pixel is on the far side of the edge).
    bool centreSmaller = (lM - lLocal) < 0.0;
    bool correct = ((nearer1 ? e1 : e2) < 0.0) != centreSmaller;
    float edgeOffset = correct ? (0.5 - dMin / edgeLen) : 0.0;
    float offset = max(edgeOffset, subpix);

    vec2 uv = v_uv;
    if (horizontal)
        uv.y += offset * stepLen;
    else
        uv.x += offset * stepLen;
    fragColor = vec4(textureLod(u_src, uv, 0.0).rgb, centre.a);
}
