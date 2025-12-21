#version 440

// Main scene pass, one draw per face run; the object block (transform) comes by dynamic offset.

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_texCoord;
layout(location = 3) in vec3 a_tangent;
layout(location = 4) in vec3 a_bitangent;

layout(std140, binding = 0) uniform SceneUBO {
    mat4 u_viewProj;
    mat4 u_view;
    mat4 u_lightView;
    mat4 u_lightClip;
    vec4 u_cameraPos;
    vec4 u_sunDir;              // direction the sunlight travels
    vec4 u_sunColor;            // rgb, w = intensity
    vec4 u_ambient;             // rgb, w = intensity
    vec4 u_params0;             // x renderMode, y shadowEnabled, z shadowTexel, w shadowDepthScale
    vec4 u_params1;             // x fogDensity, y shadowBias, z exposure, w unused
    vec4 u_fogColor;
    vec4 u_shadowParams;
    vec4 u_shadowFit;          // x world units per shadow texel, y light far, z light near, w unused
};

layout(location = 5) in float a_faceIndex;

layout(std140, binding = 1) uniform ObjectBlock {
    mat4 u_model;
    mat4 u_normalMatrix;
    vec4 u_objectInfo;          // x = first face slot (uint bits)
};

// Per-face data, indexed by the object's first face slot + the mesh-local face index, so a run of faces with
// the same material is one draw.
struct FaceData {
    vec4 faceInfo;              // metallic, roughness, metallic_specular, normal_scale
    uvec4 pickInfo;             // pickingId, materialIndex, flags, pad
    vec4 uvWorldU;              // xyz = dU/d(worldPos), w = U offset in tiles
    vec4 uvWorldV;              // xyz = dV/d(worldPos), w = V offset in tiles
    vec4 matInfo;               // ao_light_affect, emission_energy
    vec4 emission;              // linear emission colour
};
layout(std430, binding = 2) readonly buffer FaceBuffer {
    FaceData faces[];
};

layout(location = 0) out vec3 v_worldPos;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) out vec3 v_tangent;
layout(location = 4) out vec3 v_bitangent;
layout(location = 6) flat out uint v_face;

void main() {
    uint faceSlot = floatBitsToUint(u_objectInfo.x) + uint(a_faceIndex + 0.5);
    v_face = faceSlot;
    uvec4 u_pickInfo = faces[faceSlot].pickInfo;
    vec4 u_uvWorldU = faces[faceSlot].uvWorldU;
    vec4 u_uvWorldV = faces[faceSlot].uvWorldV;
    vec4 world = u_model * vec4(a_position, 1.0);

    mat3 nmat = mat3(u_normalMatrix);
    v_worldPos = world.xyz;
    v_normal = normalize(nmat * a_normal);
    v_tangent = normalize(nmat * a_tangent);
    v_bitangent = normalize(nmat * a_bitangent);

    // UV from world position alone (pre-multiplied basis: U = dot(world, uAxis) * tilesPerUnitU + offsetU,
    // same for V), so a material stays on the grid through any brush transform.
    v_uv = vec2(dot(world.xyz, u_uvWorldU.xyz) + u_uvWorldU.w,
                dot(world.xyz, u_uvWorldV.xyz) + u_uvWorldV.w);
    // Models (props) carry their own UVs.
    if ((u_pickInfo.z & 32u) != 0u)
        v_uv = a_texCoord;

    gl_Position = u_viewProj * world;
}
