#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <QMatrix4x4>
#include <QString>
#include <rhi/qshader.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

class QRhi;
class QRhiBuffer;
class QRhiTexture;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;

namespace vk {

// World axes are Godot's: +Y up, right-handed, a camera looks down -Z. Source/VMF is Z-up; only the import
// boundary sees Source axes (sourceToYUp):
//   world  = (source.x, source.z, -source.y)
//   source = (world.x, -world.z, world.y)
// det = +1: a rotation, no mirror.
inline glm::vec3 sourceToYUp(const glm::vec3 &v) {
  return glm::vec3(v.x, v.z, -v.y);
}

// std140 mirror of the SceneUBO block in shaders/scene.vert. Matrices first, then vec4s: no padding gaps.
inline constexpr int kMaxSceneLights = 32;

struct SceneUniforms {
  glm::mat4 viewProj { 1.0f };
  glm::mat4 view { 1.0f };
  glm::mat4 lightView { 1.0f };
  glm::mat4 lightClip { 1.0f };
  glm::vec4 cameraPos { 0.0f };
  glm::vec4 sunDir { 0.0f, -1.0f, 0.0f, 0.0f };
  glm::vec4 sunColor { 1.0f };
  glm::vec4 ambient { 0.4f, 0.45f, 0.55f, 1.0f };
  glm::vec4 params0 { 0.0f }; // x renderMode, y shadowEnabled, z shadowTexel, w shadowDepthScale
  glm::vec4 params1 { 0.0f }; // x fogDensity, y shadowBias, z exposure, w 1/far
  glm::vec4 fogColor { 0.13f, 0.16f, 0.24f, 1.0f };
  // x pcfRadius (texels), y shadowStrength, z normalOffsetBias (world units), w shadow-edge softness ramp
  glm::vec4 shadowParams { 2.0f, 1.0f, 0.42f, 0.0f };
  // Shadow texel world size and light volume depth, so the shader biases in world units, not a varying far plane.
  glm::vec4 shadowFit { 0.25f, 500.0f, 1.0f, 0.0f };
  // Local lights (vk_entities.cpp), four vec4 each: (pos, range) (rgb, intensity) (spotDir, cosOuter|-2)
  // (cosInner, falloff, isSpot, affectsSpecular). lightCount: x = live count, y = world units per metre.
  glm::vec4 lightCount { 0.0f };
  glm::vec4 lights[kMaxSceneLights * 4] {};
  // Sun shadow cascades (1..4): light view / clip, atlas tile (xy offset, z scale), view-depth far split.
  glm::mat4 cascadeView[4] {};
  glm::mat4 cascadeClip[4] {};
  glm::vec4 cascadeUv[4] {};
  glm::vec4 cascadeSplits { 1e9f };   // view depth where cascade i ends
  glm::vec4 cascadeTexel { 0.25f };   // world units per texel
  glm::vec4 cascadeFar { 500.0f };    // light far plane (depth encode)
  glm::vec4 cascadeInfo { 1.0f, 0.1f, 0.0f, 0.0f }; // x count, y blend band, z debug tint
  // Entity light shadows (vk_light_shadows.cpp): x = first atlas tile (-1 = none), y = tan(half fov) (1 for omni).
  glm::vec4 lightShadow[kMaxSceneLights] {};
  glm::vec4 lightAtlasInfo { 0.0f }; // x atlas px, y NDC y sign, z tiles per row, w any shadows
};
static_assert(sizeof(SceneUniforms) == 400 + 16 + kMaxSceneLights * 64 + 512 + 64 + 64 + kMaxSceneLights * 16 + 16);

inline constexpr int kLightAtlasTilesPerRow = 8;

// One cascade as the shadow pass sees it (dynamic offset per cascade).
struct CascadeBlock {
  glm::mat4 lightView { 1.0f };
  glm::mat4 lightClip { 1.0f };
  glm::vec4 params { 0.0f }; // x = 1/far (depth encode)
};

// One entry per shadow-atlas tile of an entity light (a cube face, or a spot's cone), selected by dynamic offset
// at binding 15. Stride is rhi->ubufAligned(sizeof(LightShadowFace)), never sizeof.
struct LightShadowFace {
  glm::mat4 clip { 1.0f }; // corr * proj(90) * view
  glm::mat4 view { 1.0f };
  glm::vec4 info { 0.0f }; // x = 1/far (the depth encoding scale), y = near
};
static_assert(sizeof(LightShadowFace) == 144);

// std140 mirror of the SsaoUBO block in shaders/ssao.frag (view space: only the projection is needed).
struct SsaoUniforms {
  glm::mat4 clipFromView { 1.0f }; // corr * proj
  glm::vec4 params { 1.2f, 0.05f, 1.0f, 0.012f }; // radius, sample bias, intensity, blur depth sigma
  glm::vec4 info { 0.0f }; // 1/aoWidth, 1/aoHeight, unused, unused
};
static_assert(sizeof(SsaoUniforms) == 96);

// std140 mirror of the EntityUBO block in shaders/entity.vert: placement and colour of a marker.
struct EntityUniforms {
  glm::mat4 model { 1.0f };
  glm::vec4 color { 1.0f, 1.0f, 1.0f, 1.0f };
  glm::vec4 info { 0.0f };
};
static_assert(sizeof(EntityUniforms) == 96);

// Editor entity markers: one list (Engine3D::entityMarkers()) drawn by the 3D view and the 2D views alike.
enum class EntityKind : int {
  Generic = 0, // eng::Entity (vk_entities.cpp)
};

// Kind-default icons start the billboard atlas; class icons follow (vk_billboards.cpp).
inline constexpr int kEntityKindIconCount = 7;

struct EntityMarker {
  EntityKind kind { EntityKind::Generic };
  // Billboard icon slot (vk_billboards.cpp) and its tint; -1 = no icon.
  int icon { -1 };
  glm::vec4 iconTint { 1.0f };
  quint32 id { 0 };
  glm::vec3 position { 0.0f };
  // Half-extents of the marker box in world units (3D box and 2D wireframe).
  glm::vec3 halfSize { 1.0f };
  glm::vec4 color { 1.0f, 1.0f, 1.0f, 1.0f };
  bool selected { false };
  // Range in world units, 0 = none; drawn while selected.
  float range { 0.0f };
  // Spot lights: facing and cone half angle (degrees); with an angle the range is drawn as the cone (spotConeRing).
  glm::vec3 direction { 0.0f, 0.0f, 1.0f };
  float coneAngle { 0.0f };
  // Draw an arrow along `direction` (path anchors: where the corridor leaves).
  bool facingArrow { false };
  // Facing can be turned (Engine3D::entityRotatable): the 2D views draw a rotate knob while selected.
  bool rotatable { false };
  // One-line readout drawn while selected.
  QString label;
};

// An orthonormal basis whose third column (local +Z) is `dir`.
inline glm::mat3 basisAlong(const glm::vec3 &dir) {
  const glm::vec3 z = glm::normalize(dir);
  const glm::vec3 ref = std::abs(z.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
  const glm::vec3 x = glm::normalize(glm::cross(ref, z));
  return glm::mat3(x, glm::cross(z, x), z);
}

// A spot cone's rim: a circle around the facing at range·cos(angle), radius range·sin(angle); `segments` points.
inline void spotConeRing(const EntityMarker &m, const int segments, std::vector<glm::vec3> &out) {
  out.clear();
  const float a = glm::radians(m.coneAngle);
  const glm::mat3 b = basisAlong(m.direction);
  const glm::vec3 centre = m.position + b[2] * (m.range * std::cos(a));
  const float radius = m.range * std::sin(a);
  for (int k = 0; k < segments; ++k) {
    const float t = 2.0f * 3.14159265358979f * float(k) / float(segments);
    out.push_back(centre + (b[0] * std::cos(t) + b[1] * std::sin(t)) * radius);
  }
}

// An entity I/O link as drawn (Engine3D::ioLinks()); connections between the same two entities share one.
struct IOLink {
  enum Kind : int {
    Output = 0,    // an output connection to a resolved target
    Reference = 1, // a Target property (path_node next, trigger_look target)
    Broken = 2,    // an output whose target matches nothing (b == a)
  };
  int kind { Output };
  glm::vec3 a { 0.0f }; // from
  glm::vec3 b { 0.0f }; // to
  quint32 from { 0 };
  quint32 to { 0 };
  // 1 = leaves a selected entity, 2 = arrives at one, 0 = neither.
  int selectedSide { 0 };
  bool delayed { false }; // every connection on the link has a delay
  int count { 1 };
  QString label; // "OnTrigger → Open · 1s"
};

// A simulated I/O event travelling along a link (vk_io_sim.cpp); t is 0..1.
struct IOPulse {
  glm::vec3 a { 0.0f };
  glm::vec3 b { 0.0f };
  float t { 0.0f };
};

// One entry per object, addressed by a dynamic uniform offset.
struct ObjectBlock {
  glm::mat4 model { 1.0f };
  glm::mat4 normalMatrix { 1.0f };
  // x = the object's first slot in the per-face storage buffer (as uint bits).
  glm::vec4 info { 0.0f };
};
static_assert(sizeof(ObjectBlock) == 144);

// One entry per face: everything that can differ between faces of one object. Material props are per face so
// variants and map overrides need no SRB or pipeline of their own.
struct DrawBlock {
  // Godot material parameters: metallic, roughness, metallic_specular, normal_scale.
  glm::vec4 faceInfo { 0.0f, 1.0f, 0.5f, 1.0f };
  glm::uvec4 pickInfo { 0, 0, 0, 0 };             // pickingId, materialIndex, flags, pad
  // Texture basis, pre-multiplied: xyz = d(tile)/d(worldPos) along the axis, w = tile offset.
  // The vertex shader derives UV from world position alone.
  glm::vec4 uvWorldU { 0.0f };
  glm::vec4 uvWorldV { 0.0f };
  // ao_light_affect, emission_energy, unused, unused
  glm::vec4 matInfo { 0.0f, 1.0f, 0.0f, 0.0f };
  // Emission colour (linear rgb); w unused. Added to the emission map.
  glm::vec4 emission { 0.0f };
};
static_assert(sizeof(DrawBlock) == 96);

enum DrawFlag : uint32_t {
  DrawFlagHasNormalMap = 1 << 0,
  DrawFlagHasMRMap = 1 << 1,      // informational: the ORM texture is a real map (shader reads it either way)
  DrawFlagHasEmissionMap = 1 << 2,
  DrawFlagSelected = 1 << 3,      // whole object selected: darken
  DrawFlagSelectedFace = 1 << 4,  // face picked in material mode: red tint, wins over the darken
  DrawFlagVertexUV = 1 << 5,      // model face: UV from the vertex, not the world basis
  DrawFlagTransparent = 1 << 6,   // alpha scissor at 0.5 (Godot transparency)
  DrawFlagUnshaded = 1 << 7,      // shading_mode = unshaded: albedo only
  DrawFlagEmission = 1 << 8,      // emission_enabled
};

// std140 mirror of the GridUBO block in shaders/grid.vert.
struct GridUniforms {
  glm::mat4 gridToClip { 1.0f };
  glm::mat4 clipToGrid { 1.0f };
  glm::mat4 worldToClip { 1.0f };
  glm::vec4 viewInfo { 0.0f };   // pxPerWorldUnit, devicePixelRatio, showGrid, unused
  glm::vec4 gridStyle { 1.0f };  // stepWorld, majorEvery, minorAlpha, majorAlpha
  glm::vec4 colorMinor { 0.22f, 0.26f, 0.34f, 1.0f };
  glm::vec4 colorMajor { 0.34f, 0.40f, 0.52f, 1.0f };
  glm::vec4 colorAxis { 0.55f, 0.35f, 0.40f, 1.0f };
  glm::vec4 lineScale { 0.0f };   // NDC per device pixel (2 / pixelSize)
  glm::vec4 wireStyle { 0.0f };   // ramp length in extra lines, density texture size
  glm::vec4 wireCool { 0.42f, 0.62f, 0.92f, 1.0f };
  glm::vec4 wireWarm { 0.26f, 0.86f, 0.55f, 1.0f };
  glm::vec4 wireSelect { 1.00f, 0.72f, 0.30f, 1.0f };
};
static_assert(sizeof(GridUniforms) == 352);

// std140 mirror of the OverlayUBO block in shaders/wire.vert.
struct OverlayUniforms {
  glm::vec4 wireColor { 1.0f, 1.0f, 1.0f, 1.0f };
  glm::vec4 wireParams { 2.0f, 0.0015f, 0.0f, 0.0f }; // thickness px, push factor
};
static_assert(sizeof(OverlayUniforms) == 32);

// Axis basis of one 2D view; matches GridView::Perspective: 0 Top, 1 Front, 2 Side.
struct ViewBasis {
  glm::dvec3 right { 1.0, 0.0, 0.0 };
  glm::dvec3 up { 0.0, 1.0, 0.0 };
  glm::dvec3 depth { 0.0, 0.0, -1.0 };
};

// Each basis is right-handed in the sense the editor relies on: depth == -(right x up).
inline ViewBasis viewBasisFor(int perspective) {
  switch (perspective) {
    case 1: // Front: looking down -Z, the way a Godot camera does
      return { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, -1.0 } };
    case 2: // Side: looking along -X
      return { { 0.0, 0.0, -1.0 }, { 0.0, 1.0, 0.0 }, { -1.0, 0.0, 0.0 } };
    default: // Top: looking straight down, -Z toward the top of the screen
      return { { 1.0, 0.0, 0.0 }, { 0.0, 0.0, -1.0 }, { 0.0, -1.0, 0.0 } };
  }
}

// The eight resize handles of the selection box; handleRect() places one, handleSides() says which sides it drags.
enum SelectionHandle : int {
  SelectionHandleTopLeft = 0,
  SelectionHandleTopRight = 1,
  SelectionHandleBottomLeft = 2,
  SelectionHandleBottomRight = 3,
  SelectionHandleTop = 4,
  SelectionHandleBottom = 5,
  SelectionHandleLeft = 6,
  SelectionHandleRight = 7,
  SelectionHandleCount = 8,
};

// `box` is (a0, b0, a1, b1) in view-plane coords. Corner handles sit diagonally outside; side ones straddle the edge.
inline glm::dvec4 handleRect(const glm::dvec4 &box, const int handle, const double side) {
  const double a0 = box.x;
  const double b0 = box.y;
  const double a1 = box.z;
  const double b1 = box.w;
  const double midA = (a0 + a1) * 0.5;
  const double midB = (b0 + b1) * 0.5;
  const double half = side * 0.5;

  switch (handle) {
    case SelectionHandleTopLeft: return {a0 - side, b1, a0, b1 + side};
    case SelectionHandleTopRight: return {a1, b1, a1 + side, b1 + side};
    case SelectionHandleBottomLeft: return {a0 - side, b0 - side, a0, b0};
    case SelectionHandleBottomRight: return {a1, b0 - side, a1 + side, b0};
    case SelectionHandleTop: return {midA - half, b1, midA + half, b1 + side};
    case SelectionHandleBottom: return {midA - half, b0 - side, midA + half, b0};
    case SelectionHandleLeft: return {a0 - side, midB - half, a0, midB + half};
    default: return {a1, midB - half, a1 + side, midB + half};
  }
}

inline void handleSides(const int handle, bool &dragA0, bool &dragA1, bool &dragB0, bool &dragB1) {
  dragA0 = handle == SelectionHandleTopLeft || handle == SelectionHandleBottomLeft
      || handle == SelectionHandleLeft;
  dragA1 = handle == SelectionHandleTopRight || handle == SelectionHandleBottomRight
      || handle == SelectionHandleRight;
  dragB0 = handle == SelectionHandleBottomLeft || handle == SelectionHandleBottomRight
      || handle == SelectionHandleBottom;
  dragB1 = handle == SelectionHandleTopLeft || handle == SelectionHandleTopRight
      || handle == SelectionHandleTop;
}

// Grid-snapped selection box: a rect in the creating view's plane plus its depth range.
struct MarqueeBox {
  bool valid = false;
  int plane = 0;
  double a0 = 0.0, a1 = 0.0; // along basis.right
  double b0 = 0.0, b1 = 0.0; // along basis.up
  double d0 = 0.0, d1 = 0.0; // along basis.depth
};

inline void marqueeToWorld(const MarqueeBox &m, glm::dvec3 &mn, glm::dvec3 &mx) {
  const ViewBasis bs = viewBasisFor(m.plane);
  const glm::dvec3 c0 = m.a0 * bs.right + m.b0 * bs.up + m.d0 * bs.depth;
  const glm::dvec3 c1 = m.a1 * bs.right + m.b1 * bs.up + m.d1 * bs.depth;
  mn = glm::min(c0, c1);
  mx = glm::max(c0, c1);
}

// Extent of a world AABB along a signed axis unit vector (components 0 or +/-1).
inline void projectOntoAxis(const glm::dvec3 &ax, const glm::dvec3 &lo, const glm::dvec3 &hi,
                           double &o0, double &o1) {
  double a = 0.0;
  double b = 0.0;
  for (int i = 0; i < 3; ++i) {
    const double p0 = ax[i] * lo[i];
    const double p1 = ax[i] * hi[i];
    a += std::min(p0, p1);
    b += std::max(p0, p1);
  }
  o0 = a;
  o1 = b;
}

// The marquee's world box re-projected onto another view's right/up axes.
inline MarqueeBox marqueeAsSeenFrom(const MarqueeBox &m, int perspective) {
  glm::dvec3 mn {};
  glm::dvec3 mx {};
  marqueeToWorld(m, mn, mx);

  const ViewBasis bs = viewBasisFor(perspective);
  MarqueeBox out = m;
  out.plane = perspective;
  projectOntoAxis(bs.right, mn, mx, out.a0, out.a1);
  projectOntoAxis(bs.up, mn, mx, out.b0, out.b1);
  projectOntoAxis(bs.depth, mn, mx, out.d0, out.d1);
  return out;
}

enum class RenderMode : int {
  Fullbright = 0, // no shadows; face-tinted so large flat areas stay readable
  Component = 1,  // full lighting: sun, shadows, normal maps, fog
};

QShader loadBakedShader(const QString &resourcePath);

// QMatrix4x4::constData() and glm are both column-major, so data copies across; but QMatrix4x4's float[]
// constructor reads row-major, hence the transpose going the other way.
inline glm::mat4 qtToGlm(const QMatrix4x4 &q) { return glm::make_mat4(q.constData()); }
inline QMatrix4x4 glmToQt(const glm::mat4 &m) { return QMatrix4x4(glm::value_ptr(m)).transposed(); }

} // namespace vk
