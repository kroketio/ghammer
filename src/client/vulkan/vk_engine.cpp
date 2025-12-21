#include "vk_engine.h"
#include "client/prefs/preferences.h"
#include "client/engine/smooth_units.h"
#include "client/engine/brush.h"
#include <QGuiApplication>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include <QLoggingCategory>
#include <QMatrix4x4>
#include <QTimer>
#include <QKeyEvent>
#include <QElapsedTimer>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <rhi/qrhi.h>

#include "client/lib/globals.h"
#include "shared/lib/config.h"
#include "engine/face_fill.h"
#include "vk_frustum.h"
#include "vk_gridview.h"
#include "vk_shared.h"
#include "vk_text.h"

namespace vk {

static QLoggingCategory engLog("vk.engine", QtWarningMsg);

// A failed target allocation is retried after this many frames, not every frame.
static constexpr int kAllocRetryFrames = 60;

// A stage slower than this is reported at once (VK_PERF_SLOW_MS lowers it).
static const double kSlowStageMs = perfSlowMs();

// Runtime kill-switches for profiling:
//   VK_PERF=1                 frame rate + CPU ms per stage + rebuild counters
//   VK_NO_ENTITIES=1          no marker box, no range sphere, no gizmo at all
//   VK_NO_GIZMO=1             keep box + sphere, drop the translate gizmo
//   VK_NO_RANGE_SPHERE=1      keep box + gizmo, drop the range sphere
//   VK_NO_SSAO=1              skip the g-buffer + AO chain
static bool noEntities() {
  static const bool v = envFlag("VK_NO_ENTITIES");
  return v;
}
static bool noGizmo() {
  static const bool v = envFlag("VK_NO_GIZMO");
  return v;
}
static bool noRangeSphere() {
  static const bool v = envFlag("VK_NO_RANGE_SPHERE");
  return v;
}
static bool noSsao() {
  static const bool v = envFlag("VK_NO_SSAO");
  return v;
}
// VK_COMPONENT=1 starts in Component (PBR) mode.
static bool startComponent() {
  static const bool v = envFlag("VK_COMPONENT");
  return v;
}

namespace {

// Offset bounds from the 1-based level range. Level 1 = 1 source unit, level 10 = 512.
constexpr int kGridOffsetMin = Engine3D::kGridLevelMin - Engine3D::kGridLevelBase;
constexpr int kGridOffsetMax = Engine3D::kGridLevelMax - Engine3D::kGridLevelBase;

// Entity UBO capacity (markers, helpers, gizmo parts, vertex handles). Never grown: growing would mean
// rebuilding the SRB and all three pipelines.
constexpr quint32 kEntityCapacity = 2048;

// Translate gizmo, in gizmo-local units (proportions of the on-screen gizmo).
// Below this viewport height the gizmo shrinks with it (Godot's manipulator_gizmo_size / 400 clamp).
constexpr float kGizmoViewportBasePx = 400.0f;
constexpr float kArrowShaftEnd = 1.30f;
constexpr float kArrowTipEnd = 1.65f;
constexpr float kArrowTipRadius = 0.048f;
constexpr float kArrowShaftRadius = 0.012f;
constexpr int kArrowSides = 16;
constexpr float kPlaneSize = 0.25f;
constexpr float kPlaneDst = 0.20f;
// Grabbers are fatter than what is drawn.
constexpr float kGrabTolerance = 1.5f;

constexpr glm::vec3 kAxisColor[3] = {
    {0.95f, 0.28f, 0.28f},
    {0.35f, 0.85f, 0.30f},
    {0.30f, 0.55f, 1.00f},
};

glm::vec3 unitAxis(const int i) {
  glm::vec3 v(0.0f);
  v[i] = 1.0f;
  return v;
}

// +/-1 per world axis: an axis pointing away from the camera flips its plane handle so the handles face the eye
// (as Godot). The same signs must be used for drawing and hit testing. `camToCenter`: camera toward the gizmo.
glm::vec3 gizmoPlaneSigns(const glm::vec3 &camToCenter) {
  glm::vec3 signs(1.0f);
  for (int j = 0; j < 3; ++j) {
    const glm::vec3 a = unitAxis(j);
    signs[j] = glm::dot(a, camToCenter) > 1e-5f ? -1.0f : 1.0f;
  }
  return signs;
}

glm::mat4 scaleDiag(const glm::vec3 &s) {
  glm::mat4 m(1.0f);
  m[0][0] = s.x;
  m[1][1] = s.y;
  m[2][2] = s.z;
  return m;
}

// Nearest positive hit only.
bool raySphere(const glm::vec3 &ro, const glm::vec3 &rd, const glm::vec3 &center, const float radius,
              float &t) {
  const glm::vec3 oc = ro - center;
  const float b = glm::dot(oc, rd);
  const float c = glm::dot(oc, oc) - radius * radius;
  const float disc = b * b - c;
  if (disc < 0.0f)
    return false;
  const float sq = std::sqrt(disc);
  t = -b - sq;
  if (t < 0.0f)
    t = -b + sq;
  return t >= 0.0f;
}

bool rayPlane(const glm::vec3 &ro, const glm::vec3 &rd, const glm::vec3 &n, const glm::vec3 &point,
             glm::vec3 &out) {
  const float denom = glm::dot(n, rd);
  if (std::fabs(denom) < 1e-6f)
    return false;
  const float t = glm::dot(point - ro, n) / denom;
  out = ro + rd * t;
  return true;
}

// Revolve a 2D (along, radius) profile around `axis` into triangles.
void revolveProfile(std::vector<glm::vec3> &v, const glm::vec3 &axis,
                   const std::vector<std::pair<float, float>> &profile, const int sides) {
  glm::vec3 u(0.0f, 1.0f, 0.0f);
  if (std::fabs(glm::dot(u, axis)) > 0.95f)
    u = glm::vec3(1.0f, 0.0f, 0.0f);
  const glm::vec3 w = glm::normalize(glm::cross(axis, u));
  u = glm::normalize(glm::cross(w, axis));

  const float step = 2.0f * 3.14159265358979f / float(sides);
  for (int s = 0; s < sides; ++s) {
    const float a0 = step * float(s);
    const float a1 = step * float(s + 1);
    const glm::vec3 r0 = u * std::cos(a0) + w * std::sin(a0);
    const glm::vec3 r1 = u * std::cos(a1) + w * std::sin(a1);
    for (size_t k = 0; k + 1 < profile.size(); ++k) {
      const auto &[d0, rad0] = profile[k];
      const auto &[d1, rad1] = profile[k + 1];
      const glm::vec3 p00 = axis * d0 + r0 * rad0;
      const glm::vec3 p01 = axis * d0 + r1 * rad0;
      const glm::vec3 p11 = axis * d1 + r1 * rad1;
      const glm::vec3 p10 = axis * d1 + r0 * rad1;
      v.push_back(p00);
      v.push_back(p01);
      v.push_back(p11);
      v.push_back(p00);
      v.push_back(p11);
      v.push_back(p10);
    }
  }
}

void buildBoxTriangles(std::vector<glm::vec3> &v, const float size) {
  const float h = size * 0.5f;
  const glm::vec3 c[8] = {
      {-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
      {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h},
  };
  static const int faces[6][4] = {
      {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 4, 7, 3}, {1, 2, 6, 5},
  };
  for (const auto &f: faces) {
    v.push_back(c[f[0]]);
    v.push_back(c[f[1]]);
    v.push_back(c[f[2]]);
    v.push_back(c[f[0]]);
    v.push_back(c[f[2]]);
    v.push_back(c[f[3]]);
  }
}

// Unit-radius UV sphere as line-segment pairs, poles on +/-Y.
void buildSphereLines(std::vector<glm::vec3> &v, const int stacks, const int slices) {
  auto pointAt = [](float phi, float theta) {
    const float r = std::sin(phi);
    return glm::vec3(r * std::cos(theta), std::cos(phi), -r * std::sin(theta));
  };
  const float dphi = 3.14159265358979f / float(stacks);
  const float dtheta = 2.0f * 3.14159265358979f / float(slices);

  for (int s = 1; s < stacks; ++s) {
    const float phi = dphi * float(s);
    for (int k = 0; k < slices; ++k) {
      v.push_back(pointAt(phi, dtheta * float(k)));
      v.push_back(pointAt(phi, dtheta * float(k + 1)));
    }
  }
  for (int k = 0; k < slices; ++k) {
    const float theta = dtheta * float(k);
    for (int s = 0; s < stacks; ++s) {
      v.push_back(pointAt(dphi * float(s), theta));
      v.push_back(pointAt(dphi * float(s + 1), theta));
    }
  }
}

// Plane handle in the (+j, -k) quadrant, off the two arrows it spans.
void buildPlaneHandle(std::vector<glm::vec3> &v, const glm::vec3 &ej, const glm::vec3 &ek) {
  const glm::vec3 off = (ej - ek) * kPlaneDst;
  const glm::vec3 p0 = off;
  const glm::vec3 p1 = off + ej * kPlaneSize;
  const glm::vec3 p2 = off + (ej - ek) * kPlaneSize;
  const glm::vec3 p3 = off - ek * kPlaneSize;
  v.push_back(p0);
  v.push_back(p1);
  v.push_back(p2);
  v.push_back(p0);
  v.push_back(p2);
  v.push_back(p3);
}

QRhiShaderResourceBindings *makeSrb(QRhi *rhi,
                                    QRhiBuffer *sceneUbo,
                                    QRhiBuffer *objectUbo,
                                    QRhiBuffer *drawUbo,
                                    QRhiBuffer *overlayUbo,
                                    eng::Material *material,
                                    QRhiTexture *extraTex = nullptr,
                                    QRhiTexture *shadowTex = nullptr,
                                    const bool litScene = false,
                                    QRhiTexture *lightAtlasTex = nullptr,
                                    QRhiTexture *lutTex = nullptr) {
  QVector<QRhiShaderResourceBinding> bindings;
  bindings << QRhiShaderResourceBinding::uniformBuffer(
      0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, sceneUbo);

  if (objectUbo)
    bindings << QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
        1, QRhiShaderResourceBinding::VertexStage, objectUbo, sizeof(ObjectBlock));

  // scene.frag reads u_pickInfo from this block too: the fragment stage needs the binding.
  if (drawUbo)
    bindings << QRhiShaderResourceBinding::bufferLoad(
        2, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, drawUbo);

  if (overlayUbo)
    bindings << QRhiShaderResourceBinding::uniformBuffer(
        3, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
        overlayUbo);

  auto &shared = Shared::instance();
  auto *sampler = shared.linearSampler();
  QRhiTexture *albedo = material ? material->albedo : shared.fallbackWhite();
  QRhiTexture *normal = material ? material->normal : shared.fallbackNormal();
  QRhiTexture *mr = material ? material->metallicRoughness : shared.fallbackWhite();
  QRhiTexture *emission = material && material->emission ? material->emission : shared.fallbackBlack();
  if (!albedo || !normal || !mr || !emission)
    return nullptr;

  bindings << QRhiShaderResourceBinding::sampledTexture(
      4, QRhiShaderResourceBinding::FragmentStage, albedo, sampler);
  bindings << QRhiShaderResourceBinding::sampledTexture(
      5, QRhiShaderResourceBinding::FragmentStage, normal, sampler);
  bindings << QRhiShaderResourceBinding::sampledTexture(
      6, QRhiShaderResourceBinding::FragmentStage, mr, sampler);
  // Emission (AO lives in the ORM texture's red channel).
  bindings << QRhiShaderResourceBinding::sampledTexture(
      7, QRhiShaderResourceBinding::FragmentStage, emission, sampler);
  // Shadow map: nearest (the PCF kernel reads texel-for-texel); white = unshadowed.
  bindings << QRhiShaderResourceBinding::sampledTexture(
      8, QRhiShaderResourceBinding::FragmentStage,
      shadowTex ? shadowTex : shared.fallbackWhite(), shared.nearestSampler());

  // Overlay texture (selected-mesh depth for the count pass, occluder count for the wire overlay): nearest.
  if (extraTex)
    bindings << QRhiShaderResourceBinding::sampledTexture(
        9, QRhiShaderResourceBinding::FragmentStage, extraTex, shared.nearestSampler());

  // Bindings 14 and 15 are declared by scene.frag only.
  if (litScene) {
    // Entity-light shadow atlas: nearest, the taps must not interpolate.
    bindings << QRhiShaderResourceBinding::sampledTexture(
        14, QRhiShaderResourceBinding::FragmentStage, lightAtlasTex ? lightAtlasTex : shared.fallbackWhite(),
        shared.nearestSampler());
    // Colour-grading 3D LUT.
    if (lutTex)
      bindings << QRhiShaderResourceBinding::sampledTexture(15, QRhiShaderResourceBinding::FragmentStage, lutTex,
                                                            shared.clampLinearSampler());
  }

  auto *srb = rhi->newShaderResourceBindings();
  srb->setBindings(bindings.constData(), bindings.constData() + bindings.size());
  if (!srb->create()) {
    qWarning(engLog, "srb create failed (%d bindings)", static_cast<int>(bindings.size()));
    delete srb;
    return nullptr;
  }
  return srb;
}

// SSAO bindings: each pass gets exactly the slots its shader declares.
QRhiShaderResourceBindings *makeSsaoSrb(QRhi *rhi,
                                        QRhiBuffer *ssaoUbo,
                                        QRhiTexture *posTex,
                                        QRhiTexture *normalTex,
                                        QRhiTexture *aoTex) {
  QVector<QRhiShaderResourceBinding> bindings;
  auto &shared = Shared::instance();

  if (ssaoUbo)
    bindings << QRhiShaderResourceBinding::uniformBuffer(
        11, QRhiShaderResourceBinding::FragmentStage, ssaoUbo);
  // Nearest: interpolating view positions across a depth edge invents geometry.
  if (posTex)
    bindings << QRhiShaderResourceBinding::sampledTexture(
        4, QRhiShaderResourceBinding::FragmentStage, posTex, shared.nearestSampler());
  if (normalTex)
    bindings << QRhiShaderResourceBinding::sampledTexture(
        5, QRhiShaderResourceBinding::FragmentStage, normalTex, shared.nearestSampler());
  if (aoTex)
    bindings << QRhiShaderResourceBinding::sampledTexture(
        12, QRhiShaderResourceBinding::FragmentStage, aoTex, shared.linearSampler());

  if (bindings.isEmpty())
    return nullptr;

  auto *srb = rhi->newShaderResourceBindings();
  srb->setBindings(bindings.constData(), bindings.constData() + bindings.size());
  if (!srb->create()) {
    qWarning(engLog, "ssao srb create failed (%d bindings)", static_cast<int>(bindings.size()));
    delete srb;
    return nullptr;
  }
  return srb;
}

// Scene UBO + one EntityUniforms array addressed by dynamic offset.
QRhiShaderResourceBindings *makeEntitySrb(QRhi *rhi, QRhiBuffer *sceneUbo, QRhiBuffer *entityUbo) {
  if (!sceneUbo || !entityUbo)
    return nullptr;
  auto *srb = rhi->newShaderResourceBindings();
  srb->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
          sceneUbo),
      QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
          14, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
          entityUbo, sizeof(EntityUniforms)),
  });
  if (!srb->create()) {
    qWarning(engLog, "entity srb create failed");
    delete srb;
    return nullptr;
  }
  return srb;
}

} // namespace

Engine3D::Engine3D(QObject *parent, const Role role) :
    QObject(parent),
    m_camera(std::make_unique<eng::Camera>(glm::vec3(0.0f, 15.0f, -8.0f))),
    m_text(std::make_unique<TextRenderer>()) {
  m_preview = role == Role::Preview;
  m_undoStack = makeUndoStack();
  m_text->setPixelSize(28);
  // Keeps hideMirrored copies hidden through sync rebuilds, duplicates, undo and loads.
  connect(this, &Engine3D::mirrorChanged, this, &Engine3D::refreshMirrorVisibility);
  // Modifier output: the poll keeps it following edits when no 3D frame is drawn (2D drags, hidden 3D view).
  connect(this, &Engine3D::selectionChanged, this, &Engine3D::refreshDrawSelection);
  m_modPoll = new QTimer(this);
  m_modPoll->setInterval(30);
  connect(m_modPoll, &QTimer::timeout, this, [this] { pumpModifiers(ModPump::Poll); });
  if (!m_preview)
    m_modPoll->start();
  connect(this, &Engine3D::sceneChanged, this, &Engine3D::refreshMirrorVisibility);
  m_showCollision = prefs::b("View/ShowCollision");
  m_showIOLines = prefs::b("View/ShowIOLines");
  m_ioLinesAll = prefs::b("View/IOLinesAll");
  m_ioLabels = prefs::b("View/IOLabels");
  m_showTriggers = prefs::b("View/ShowTriggers");
  connect(this, &Engine3D::selectionChanged, this, &Engine3D::syncCameraPreview);
  connect(this, &Engine3D::entitiesChanged, this, &Engine3D::syncCameraPreview);
  m_frameTimer = std::make_unique<QElapsedTimer>();
  m_frameTimer->start();

  connect(this, &Engine3D::marqueeChanged, this, &Engine3D::onMarqueeChangedForAutoSelect);
  for (auto sig: {&Engine3D::cutLineChanged, &Engine3D::cutModeChanged, &Engine3D::cutHoleChanged})
    connect(this, sig, this, &Engine3D::requestRedraw);

  bindPreferences();
  setupPhysics();
  watchLutCatalog();

  // Level-scope settings: undo / redo and loads change them behind the registry's back.
  if (!m_preview) {
    m_levelStore = makeLevelStore(*this);
    prefs::get().setStore(prefs::Scope::Level, m_levelStore.get());
    connect(this, &Engine3D::levelSettingsChanged, this,
            [] { prefs::get().refreshScope(prefs::Scope::Level); });
  }

  // "The picture changed" signals bump the view epoch the on-demand redraw polls.
  for (auto sig: {&Engine3D::selectionChanged, &Engine3D::sunChanged, &Engine3D::skyChanged, &Engine3D::physicsChanged,
                  &Engine3D::undoRedoChanged,
                  &Engine3D::renderDistanceChanged, &Engine3D::msaaSamplesChanged,
                  &Engine3D::redrawRequested})
    connect(this, sig, this, [this] { ++m_viewEpoch; });

  if (startComponent())
    m_renderMode = RenderMode::Component;
  // The last brushes selected size a new 2D box along the axis its view cannot see (lastSelectionBox).
  connect(this, &Engine3D::selectionChanged, this, [this] {
    glm::dvec3 mn, mx;
    if (selectionWorldAABB(mn, mx)) {
      m_lastSelectionMin = mn;
      m_lastSelectionMax = mx;
      m_hasLastSelection = true;
    }
  });
  // Zen mode always lets the selection through, so a selection change can change what it shows.
  connect(this, &Engine3D::selectionChanged, this, [this] {
    if (m_zenOn) {
      ++m_zenEpoch;
      emit zenChanged();
    }
  });
  prefs::get().watch(QStringLiteral("Viewport/ZenBox"), this, [this](const QVariant &v) {
    const QStringList parts = v.toString().split(QLatin1Char('x'));
    if (parts.size() == 3)
      setZenSizeSource(glm::vec3(parts[0].toFloat(), parts[1].toFloat(), parts[2].toFloat()));
  });
  captureLevelDefaults(); // for newDocumentState
}

void Engine3D::bindPreferences() {
  auto &p = prefs::get();
  p.watch(QStringLiteral("CameraRenderDistance"), this, [this](const QVariant &v) {
    m_renderDistanceSource = std::clamp(v.toFloat(), kRenderDistanceMinSource, kRenderDistanceMaxSource);
    updateClipPlanes();
    emit renderDistanceChanged();
  });
  p.watch(QStringLiteral("LightRenderDistance"), this, [this](const QVariant &v) {
    m_lightRenderDistanceSource = std::clamp(v.toFloat(), kRenderDistanceMinSource, kRenderDistanceMaxSource);
    emit renderDistanceChanged();
  });
  p.watch(QStringLiteral("ShadowRenderDistance"), this, [this](const QVariant &v) {
    m_shadowRenderDistanceSource = std::clamp(v.toFloat(), kRenderDistanceMinSource, kRenderDistanceMaxSource);
    emit renderDistanceChanged();
  });
  p.watch(QStringLiteral("Viewport/FieldOfView"), this, [this](const QVariant &v) {
    m_camera->setFOV(v.toFloat());
    requestRedraw();
  });
  p.watch(QStringLiteral("Viewport/MouseSensitivity"), this,
          [this](const QVariant &v) { m_camera->setMouseSensitivity(v.toFloat()); });
  p.watch(QStringLiteral("Viewport/InvertY"), this, [this](const QVariant &v) { m_camera->setInvertY(v.toBool()); });
  p.watch(QStringLiteral("Viewport/NearPlane"), this, [this](const QVariant &) { updateClipPlanes(); });
  p.watch(QStringLiteral("Viewport/FlyShiftMultiplier"), this,
          [this](const QVariant &v) { m_flyShiftMultiplier = v.toFloat(); });
  p.watch(QStringLiteral("Viewport/FlySmoothing"), this, [this](const QVariant &v) { m_flySmoothing = v.toFloat(); });
  p.watch(QStringLiteral("General/UndoLimit"), this, [this](const QVariant &v) {
    if (m_undoStack)
      m_undoStack->setMaxSteps(v.toInt()); // parked levels get it when they come back
  });
  p.watch(QStringLiteral("Render/Msaa"), this, [this](const QVariant &v) { setMsaaSamples(v.toInt()); });
  p.watch(QStringLiteral("Render/MaxTextureSize"), this,
          [](const QVariant &v) { TextureManager::instance().setMaxTextureSize(v.toInt()); });
  p.watch(QStringLiteral("Gizmo/Size"), this, [this](const QVariant &v) { m_gizmoScreenPx = v.toFloat(); });
  p.watch(QStringLiteral("Gizmo/ShowLabels"), this, [this](const QVariant &v) { m_showLabels = v.toBool(); });
  p.watch(QStringLiteral("Gizmo/LabelSize"), this, [this](const QVariant &v) {
    if (m_text)
      m_text->setPixelSize(v.toInt());
  });
  p.watch(QStringLiteral("Gizmo/WireThickness"), this,
          [this](const QVariant &v) { setWireframeThickness(v.toFloat()); });
  p.watch(QStringLiteral("Render/Sky"), this, [this](const QVariant &v) {
    if (m_backgroundOverride)
      return;
    m_skyEnabled = v.toBool();
    requestRedraw();
  });
  p.watch(QStringLiteral("Debug/NoLights"), this, [this](const QVariant &v) { m_noLocalLights = v.toBool(); });
  p.watch(QStringLiteral("Render/Tonemapper"), this, [this](const QVariant &v) {
    m_tonemapper = v.toInt();
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/ShadowFilter"), this, [this](const QVariant &v) {
    m_shadowFilter = v.toInt();
    invalidateDerivedCaches();
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/ShadowCascades"), this, [this](const QVariant &v) {
    m_cascadeCount = std::clamp(v.toInt(), 1, 4);
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/CascadeSplitLambda"), this, [this](const QVariant &v) {
    m_cascadeLambda = v.toDouble();
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/ShowCascades"), this, [this](const QVariant &v) {
    m_cascadeDebug = v.toBool();
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/ShadowTaps"), this, [this](const QVariant &v) {
    m_shadowTaps = v.toInt();
    requestRedraw();
  });
  // A quality preference, not a map edit: no undo step, no dirty flag.
  p.watch(QStringLiteral("Render/ShadowMapSize"), this, [this](const QVariant &v) {
    m_shadowMapSize = std::clamp(v.toInt(), 256, 8192);
    emit sunChanged();
  });
  p.watch(QStringLiteral("Render/SsaoQuality"), this, [this](const QVariant &v) {
    m_ssaoSamples = std::clamp(v.toInt(), 4, 64);
    requestRedraw();
  });
  p.watch(QStringLiteral("Viewport/EntityIconSize"), this, [this](const QVariant &v) {
    m_entityIconPx = float(v.toInt()) * float(qGuiApp ? qGuiApp->devicePixelRatio() : 1.0);
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/Fxaa"), this, [this](const QVariant &v) {
    m_fxaaEnabled = v.toBool();
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/SsaoHalfRes"), this, [this](const QVariant &v) {
    m_ssaoHalfRes = v.toBool();
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/Ssao"), this, [this](const QVariant &v) {
    m_ssaoAllowed = v.toBool();
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/Specular"), this, [this](const QVariant &v) {
    m_specular = v.toBool();
    ++m_viewEpoch;
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/SpecularStrength"), this, [this](const QVariant &v) {
    m_specularStrength = float(v.toDouble());
    ++m_viewEpoch;
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/NormalStrength"), this, [this](const QVariant &v) {
    m_normalStrength = float(v.toDouble());
    ++m_viewEpoch;
    requestRedraw();
  });
  p.watch(QStringLiteral("Render/NormalMaps"), this, [this](const QVariant &v) {
    m_normalMaps = v.toBool();
    ++m_viewEpoch;
    requestRedraw();
  });
  p.watch(QStringLiteral("Color/Background3D"), this, [this](const QVariant &v) {
    if (m_backgroundOverride)
      return;
    m_clearColor = QColor(v.toString());
    requestRedraw();
  });
}

void Engine3D::updateClipPlanes() {
  const float nearPlane = float(prefs::d("Viewport/NearPlane"));
  // Past the render distance so the depth range does not clip brushes before the distance cull drops them.
  m_camera->setClipPlanes(nearPlane, std::max(100.0f, renderDistanceWorld() * 1.25f));
}

// One level's history. Only the active level's records, so the connections can stay while it is parked.
std::unique_ptr<eng::UndoStack> Engine3D::makeUndoStack() {
  // No QObject parent: owned by its level alone.
  auto u = std::make_unique<eng::UndoStack>(nullptr);
  // Mirror links follow every edit, in the same undo step.
  u->setCommitHook([this](eng::UndoCommand &c) { syncMirrors(c); });
  u->setMaxSteps(prefs::i("General/UndoLimit"));
  connect(u.get(), &eng::UndoStack::canUndoChanged, this, &Engine3D::undoRedoChanged);
  connect(u.get(), &eng::UndoStack::canRedoChanged, this, &Engine3D::undoRedoChanged);
  connect(u.get(), &eng::UndoStack::undoTextChanged, this, &Engine3D::undoRedoChanged);
  connect(u.get(), &eng::UndoStack::redoTextChanged, this, &Engine3D::undoRedoChanged);
  // Every push and undo moves the cursor, which is all the dirty state depends on.
  connect(u.get(), &eng::UndoStack::undoTextChanged, this, &Engine3D::refreshDirty);
  return u;
}

Engine3D::~Engine3D() {
  if (m_levelStore && prefs::get().store(prefs::Scope::Level) == m_levelStore.get())
    prefs::get().setStore(prefs::Scope::Level, nullptr);
  Shared::removeRhiCleanup(this);
  if (Shared::forRhi(m_rhi)) {
    Shared::Scope scope(m_rhi);
    releaseResources();
  }
}

// Drops gestures in flight, tools and previews without committing. The level's own state (selection, auto-select
// runs, mirror mode) stays.
void Engine3D::endTransientState() {
  // Gestures in flight: undone, not recorded.
  endMirrorPreview();
  if (m_vertexEditOpen)
    finishVertexEdit(false, {});
  if (m_vertexScaleActive)
    cancelVertexScale();
  m_vertexPlaneDrag = {};
  m_vertexDragging = false;
  m_dragTarget = DragTarget::None;
  m_dragHandle = -1;
  m_dragDuplicate = {};
  m_dragObjects.clear();
  m_dragEntBase.clear();
  m_gizmoDrag = GizmoHandle::None;
  m_gizmoDuplicate = {};
  m_gizmoMeshIds.clear();
  m_gizmoEntBase.clear();
  m_gizmoReadout.clear();
  emit gizmoReadoutChanged(QString());
  m_rot2D = {};
  if (m_gizmoMode == GizmoMode::Rotate)
    setGizmoMode(GizmoMode::Translate);

  // Tools and previews.
  cancelShapeTool();
  if (m_cutMode != CutMode::Off)
    exitCutMode();
  cancelTargetPick();
  ioSimStop();
  clearFill();
  if (m_previewCamera)
    setCameraPreview(false);
  m_mirrorDrag = {};
  m_mirrorHoverValid = false;
  m_mirrorTurned = false;
  m_mirrorFresh.clear();
  m_pen = {};
  m_pathDrag = {};
  m_anchorDrag = {};
  // A UV editor drag: restore the faces, nothing recorded.
  if (m_faceEdit.active && m_scene) {
    for (size_t i = 0; i < m_faceEdit.refs.size() && i < m_faceEdit.base.size(); ++i)
      if (const auto o = m_scene->findById(m_faceEdit.refs[i].first); o && o->getMesh())
        o->getMesh()->setFaceTexBasis(m_faceEdit.refs[i].second, m_faceEdit.base[i]);
    emit uvTransformChanged();
  }
  m_faceEdit = {};
  m_alignEdgeCycle = 0;
  // One simulation at a time, and it belongs to this level: its bodies go back where they started (a paused one
  // has written its poses into the level).
  physicsReset();
  leaveBevelMode();
}

// Drops every tool, mode and gesture of the open map without committing. Runs while the old scene is still set,
// so previews can put the old objects back first.
void Engine3D::resetEditingState() {
  endTransientState();
  clearVertexSelection();
  clearMarquee();
  clearAutoSelect();
  {
    const auto typeOn = m_autoSel.typeOn; // a preference of the panel, not of the map
    m_autoSel = {};
    m_autoSel.typeOn = typeOn;
    m_autoSelOther = {};
    m_autoSelOther.typeOn = typeOn;
    emit autoSelectChanged();
  }
  m_mirrorOn = false;
  emit mirrorChanged();
  setMaskHidden(false);
}

void Engine3D::setScene(const QSharedPointer<eng::Scene> &scene) {
  resetEditingState();
  // The old history names ids of the old scene.
  clearUndoHistory();
  m_shapePreviewIds.clear();
  // Repoint the overrides before the old scene can go away.
  if (!m_preview)
    TextureManager::instance().setOverrides(scene ? &scene->materialOverrides : nullptr);
  if (m_sceneLight)
    disconnect(m_sceneLight, nullptr, this, nullptr);
  m_sceneLight = nullptr;
  m_scene = scene;
  resetModRuntime();
  m_layoutBlob.clear();
  m_propObjects.clear();
  m_propEpoch = ~0ull;
  m_selectedEntities.clear();
  m_selectedFaces.clear();
  m_selectedObjects.clear();
  clearObjectSlotState();
  m_faceSigById.clear();
  m_faceStructVersion = ~0ull;
  m_visible.clear();

  if (m_scene) {
    // The scene's own DirectionalLight is the sun.
    m_sceneLight = m_scene->directionalLights().isEmpty() ? nullptr : m_scene->directionalLights().first();
    connectSceneLight();
    adoptSunFromSceneLight();
    ensureWorldEnv();
  }

  m_shadowDirty = true;
  emit levelSettingsChanged();
  emit sceneChanged();
  emit selectionChanged();
}

void Engine3D::setBackgroundOverride(const QColor &color) {
  m_backgroundOverride = true;
  m_skyEnabled = false;
  m_clearColor = color;
  ++m_viewEpoch;
  requestRedraw();
}

void Engine3D::setPreviewEntities(const std::vector<eng::Entity> &entities) {
  if (!m_scene)
    return;
  auto &v = m_scene->entities;
  v.erase(std::remove_if(v.begin(), v.end(), [](const eng::Entity &e) { return e.classname != eng::kWorldEnvClass; }),
          v.end());
  for (eng::Entity e: entities) {
    e.id = m_scene->mintEntityId();
    v.push_back(e);
  }
  ++m_entityEpoch;
  emit entitiesChanged();
}

// Light edits move the rendered sun; m_pushingSunToLight breaks the loop the other way.
// Parked levels' lights are disconnected (switchDocument).
void Engine3D::connectSceneLight() {
  if (!m_sceneLight)
    return;
  disconnect(m_sceneLight, nullptr, this, nullptr);
  connect(m_sceneLight, &eng::DirectionalLight::directionChanged, this, [this](const QVector3D &dir) {
    if (m_pushingSunToLight)
      return;
    const glm::vec3 d(dir.x(), dir.y(), dir.z());
    if (glm::length(d) > 1e-5f)
      m_sunDirection = glm::normalize(d);
    m_shadowDirty = true;
    emit sunChanged();
  });
  connect(m_sceneLight, &eng::AbstractLight::colorChanged, this, [this](const QVector3D &c) {
    if (m_pushingSunToLight)
      return;
    m_sunColor = glm::vec3(c.x(), c.y(), c.z());
    emit sunChanged();
  });
  connect(m_sceneLight, &eng::AbstractLight::intensityChanged, this, [this](float value) {
    if (m_pushingSunToLight)
      return;
    m_sunIntensity = value;
    emit sunChanged();
  });
  connect(m_sceneLight, &eng::AbstractLight::enabledChanged, this, [this](bool on) {
    if (m_pushingSunToLight)
      return;
    m_shadowsEnabled = on;
    emit sunChanged();
  });
}

void Engine3D::setRenderMode(const int mode) {
  const RenderMode m = mode == static_cast<int>(RenderMode::Component) ? RenderMode::Component
                                                                    : RenderMode::Fullbright;
  if (m_renderMode == m)
    return;
  m_renderMode = m;
  emit renderModeChanged();
}

Engine3D::SelectionState Engine3D::selectionState() const {
  return {m_selectedObjects, m_selectedFaces, m_selectedEntities, m_selectedMods, m_selectedPaths};
}

void Engine3D::applySelectionState(const SelectionState &s) {
  if (!m_scene)
    return;
  // Ids gone since (deleted, rebuilt) stay out.
  m_selectedObjects.clear();
  for (const quint32 id: s.objects)
    if (const auto o = m_scene->findById(id); o && !o->isDerived())
      m_selectedObjects.insert(id);
  m_selectedFaces.clear();
  for (const quint32 pid: s.faces)
    if (m_scene->findById(eng::stableIdFromPicking(pid)))
      m_selectedFaces.insert(pid);
  m_selectedEntities.clear();
  for (const quint32 id: s.entities)
    if (m_scene->findEntity(id))
      m_selectedEntities.insert(id);
  m_selectedMods.clear();
  for (const quint32 id: s.mods)
    if (m_scene->findModHost(id))
      m_selectedMods.insert(id);
  m_selectedPaths.clear();
  for (const quint32 id: s.paths)
    if (m_scene->findPath(id))
      m_selectedPaths.insert(id);
  refreshDrawSelection();
  noteFillSelection();
  emit selectionChanged();
  requestRedraw();
}

void Engine3D::beginSelectionStep() {
  if (m_selStepDepth++ == 0)
    m_selStepBefore = selectionState();
}

void Engine3D::endSelectionStep() {
  if (m_selStepDepth <= 0 || --m_selStepDepth > 0)
    return;
  if (!m_undoStack || m_undoStack->isApplying() || m_undoStack->inMacro())
    return;
  const SelectionState before = m_selStepBefore, after = selectionState();
  if (before == after)
    return;
  m_undoStack->push(std::make_unique<eng::SelectionCommand>(
      QStringLiteral("Select"), [this, before] { applySelectionState(before); },
      [this, after] { applySelectionState(after); }));
}

void Engine3D::clearSelection() {
  if (m_selectedFaces.empty() && m_selectedObjects.empty() && m_selectedEntities.empty() && m_selectedVerts.empty()
      && m_selectedMods.empty() && m_selectedPaths.empty() && !m_materialPickHost)
    return;
  m_materialPickHost = 0;
  m_selectedMods.clear();
  m_selectedPaths.clear();
  m_drawSelection.clear();
  m_selectedVerts.clear();
  m_selectedEntities.clear();
  m_selectedFaces.clear();
  m_selectedObjects.clear();
  noteFillSelection();
  emit selectionChanged();
}

void Engine3D::setMaterialMode(const bool material) {
  if (material == m_materialMode)
    return;
  // Each mode keeps its own auto-select run.
  parkAutoSelect();
  if (material)
    leaveBevelMode();
  m_materialMode = material;
  m_materialPickHost = 0;
  setMaskHidden(false);

  if (material) {
    m_stashedObjects = m_selectedObjects;
    m_hasStashedSelection = !m_stashedObjects.empty();
    m_selectedObjects.clear();

    // Restore the stashed faces, minus brushes deleted meanwhile.
    std::set<quint32> restored;
    for (const quint32 pickingId: m_stashedFaces) {
      if (m_scene && m_scene->findById(eng::stableIdFromPicking(pickingId)))
        restored.insert(pickingId);
    }
    m_stashedFaces.clear();
    m_selectedFaces = restored;
  } else {
    m_stashedFaces = m_selectedFaces;
    m_selectedFaces.clear();
    if (m_hasStashedSelection) {
      m_selectedObjects.clear();
      for (const quint32 id: m_stashedObjects)
        if (m_scene && m_scene->indexOf(id) >= 0)
          m_selectedObjects.insert(id);
      m_stashedObjects.clear();
      m_hasStashedSelection = false;
    }
  }

  noteFillSelection();
  qDebug().noquote() << QStringLiteral("[mode] %1: %2 object(s) active, %3 face(s), %4 stashed")
                           .arg(material ? QStringLiteral("material") : QStringLiteral("object"))
                           .arg(m_selectedObjects.size())
                           .arg(m_selectedFaces.size())
                           .arg(m_stashedObjects.size());
  emit selectionChanged();
  unparkAutoSelect();
}

// Object ids plus the stable ids of the selected faces' picking ids.
std::set<quint32> Engine3D::selectedBrushIds() const {
  std::set<quint32> ids = m_selectedObjects;
  for (const quint32 pickingId: m_selectedFaces) {
    if (pickingId == 0)
      continue;
    ids.insert(eng::stableIdFromPicking(pickingId));
  }
  return ids;
}

void Engine3D::deleteSelected() {
  // End drags first: their captured bases are indexed by the slots this shifts.
  if (m_dragTarget != DragTarget::None)
    endDrag();
  if (m_gizmoDrag != GizmoHandle::None)
    endGizmoDrag();

  if (deleteSelectedPathPoints())
    return;
  if (!m_selectedPaths.empty()) {
    const std::vector<quint32> paths(m_selectedPaths.begin(), m_selectedPaths.end());
    deletePaths(paths);
    if (m_selectedObjects.empty() && m_selectedEntities.empty() && m_selectedMods.empty() && m_selectedFaces.empty())
      return;
  }
  std::set<quint32> brushIds = selectedBrushIds();
  // A selected modified object goes with its sources.
  for (const quint32 h: m_selectedMods)
    for (const quint32 id: modSources(h))
      brushIds.insert(id);
  m_selectedMods.clear();
  std::vector<quint32> entityIds(m_selectedEntities.begin(), m_selectedEntities.end());
  if (m_scene) {
    // A generated object goes as a whole, together with its collision entity.
    std::set<quint32> procs;
    for (const quint32 id: brushIds)
      if (const auto o = m_scene->findById(id); o && o->procObject())
        procs.insert(o->procObject());
    for (const quint32 pid: procs) {
      for (const quint32 m: procMembers(pid))
        brushIds.insert(m);
      const auto *p = m_scene->findProc(pid);
      const quint32 eid = p ? quint32(p->params.value(QStringLiteral("collisionEntity")).toDouble(0)) : 0;
      if (eid == 0 || !m_scene->findEntity(eid)
          || std::find(entityIds.begin(), entityIds.end(), eid) != entityIds.end())
        continue;
      const auto owned = brushesOfEntity(eid);
      if (std::all_of(owned.begin(), owned.end(), [&](quint32 b) { return brushIds.count(b) != 0; }))
        entityIds.push_back(eid);
    }
  }

  auto command = std::make_unique<DeleteCommand>(this);
  command->rememberSelection(std::vector<quint32>(m_selectedObjects.begin(), m_selectedObjects.end()),
                           std::vector<quint32>(m_selectedFaces.begin(), m_selectedFaces.end()));

  if (m_scene) {
    for (const quint32 id: brushIds) {
      const int slot = m_scene->indexOf(id);
      if (slot >= 0)
        command->addBrush(m_scene->findById(id), slot);
    }
  }

  if (command->empty() && entityIds.empty()) {
    qInfo() << "[delete] nothing selected";
    return;
  }

  // Brushes first, so deleting a brush entity does not clear the selection under the brush delete.
  const bool macro = m_undoStack && !command->empty() && !entityIds.empty();
  if (macro)
    m_undoStack->beginMacro(QStringLiteral("Delete"));
  const size_t brushes = command->brushCount();
  if (!command->empty()) {
    command->redo();
    m_undoStack->push(std::move(command));
  }
  deleteEntities(entityIds);
  if (macro)
    m_undoStack->endMacro();

  qInfo().noquote() << QStringLiteral("[delete] removed %1 brush(es), %2 left")
                          .arg(brushes).arg(m_scene ? m_scene->gameObjects.size() : 0);
}

bool Engine3D::undo() {
  cancelShapeTool();
  if (m_vertexScaleActive)
    cancelVertexScale();
  const bool ok = m_undoStack && m_undoStack->undo();
  // Inside the vertex tool: re-attach the corner selection.
  if (ok && m_editMode)
    remapSelectedVertices();
  return ok;
}

bool Engine3D::redo() {
  cancelShapeTool();
  if (m_vertexScaleActive)
    cancelVertexScale();
  const bool ok = m_undoStack && m_undoStack->redo();
  if (ok && m_editMode)
    remapSelectedVertices();
  return ok;
}

void Engine3D::clearUndoHistory() {
  if (m_undoStack)
    m_undoStack->clear(); // marks it clean
  refreshDirty();
}

void Engine3D::markClean() {
  if (m_undoStack)
    m_undoStack->markClean();
  refreshDirty();
}

void Engine3D::refreshDirty() {
  const bool now = dirty();
  if (now == m_dirtyReported)
    return;
  m_dirtyReported = now;
  qDebug().noquote() << QStringLiteral("[dirty] %1 (undo depth %2, clean at %3)")
                          .arg(now ? QStringLiteral("dirty") : QStringLiteral("clean"))
                          .arg(m_undoStack ? m_undoStack->undoDepth() : 0)
                          .arg(m_undoStack ? m_undoStack->cleanDepth() : 0);
  emit dirtyChanged();
}

bool Engine3D::canUndo() const {
  return m_undoStack && m_undoStack->canUndo();
}

bool Engine3D::canRedo() const {
  return m_undoStack && m_undoStack->canRedo();
}

QString Engine3D::undoText() const {
  return m_undoStack ? m_undoStack->undoText() : QString();
}

QString Engine3D::redoText() const {
  return m_undoStack ? m_undoStack->redoText() : QString();
}

void Engine3D::notifyStructureChanged() {
  invalidateDerivedCaches();
  syncBrushEntityOrigins();
  emit sceneChanged();
}

ObjectState Engine3D::captureObjectState(const quint32 stableId, const bool withFaces) const {
  ObjectState state;
  state.id = stableId;
  const auto obj = m_scene ? m_scene->findById(stableId) : nullptr;
  if (!obj)
    return state;

  state.position = obj->getPosition();
  state.rotation = obj->getRotation();
  state.scale = obj->getScale();

  if (withFaces) {
    const auto mesh = obj->getMesh();
    if (mesh) {
      const int faces = mesh->faceCount();
      state.faceBases.reserve(static_cast<size_t>(faces));
      state.faceMaterials.reserve(static_cast<size_t>(faces));
      state.faceGroups.reserve(static_cast<size_t>(faces));
      for (int f = 0; f < faces; ++f) {
        state.faceBases.push_back(mesh->faceTexBasis(f));
        state.faceMaterials.push_back(mesh->faceMaterialRef(f));
        state.faceGroups.push_back(mesh->faceGroups(f));
      }
    }
  }
  return state;
}

void Engine3D::applyObjectStates(const std::vector<ObjectState> &states) {
  if (!m_scene)
    return;

  size_t applied = 0;
  for (const ObjectState &state: states) {
    const auto obj = m_scene->findById(state.id);
    if (!obj) {
      qWarning().noquote()
          << QStringLiteral("[undo] object %1 is gone, cannot restore its state").arg(state.id);
      continue;
    }

    obj->setPosition(state.position);
    obj->setRotation(state.rotation);
    obj->setScale(state.scale);

    const auto mesh = obj->getMesh();
    if (mesh) {
      const size_t faces = static_cast<size_t>(mesh->faceCount());
      for (size_t f = 0; f < state.faceBases.size() && f < faces; ++f)
        mesh->setFaceTexBasis(static_cast<int>(f), state.faceBases[f]);
      for (size_t f = 0; f < state.faceMaterials.size() && f < faces; ++f)
        mesh->setFaceMaterial(static_cast<int>(f), state.faceMaterials[f]);
      for (size_t f = 0; f < state.faceGroups.size() && f < faces; ++f)
        mesh->setFaceGroups(static_cast<int>(f), state.faceGroups[f]);
    }
    ++applied;
  }

  if (applied > 0)
    emit sceneChanged();
}

void Engine3D::setSelected(const std::vector<quint32> &objects, const std::vector<quint32> &faces) {
  // Derived objects are never selected as brushes (an output's host goes through m_selectedMods).
  auto editable = [this](quint32 id) {
    const auto o = m_scene ? m_scene->findById(id) : nullptr;
    return o && !o->isDerived();
  };
  m_selectedObjects.clear();
  for (const quint32 id: objects) {
    if (editable(id))
      m_selectedObjects.insert(id);
  }

  m_selectedFaces.clear();
  for (const quint32 pickingId: faces) {
    if (pickingId == 0)
      continue;
    if (editable(eng::stableIdFromPicking(pickingId)))
      m_selectedFaces.insert(pickingId);
  }

  noteFillSelection();
  emit selectionChanged();
}

namespace {

bool faceBasisChanged(const eng::FaceTexBasis &was, const eng::FaceTexBasis &now) {
  return was.uAxis != now.uAxis || was.vAxis != now.vAxis
      || was.tilesPerUnitU != now.tilesPerUnitU || was.tilesPerUnitV != now.tilesPerUnitV
      || was.offsetU != now.offsetU || was.offsetV != now.offsetV;
}

} // namespace

void Engine3D::recordTransformEdit(const QString &label, const std::vector<quint32> &ids,
                                  const std::vector<glm::vec3> &basePos,
                                  const std::vector<glm::vec3> &baseScale,
                                  const std::vector<DragFaceUv> &baseUv) {
  if (!m_undoStack || m_undoStack->isApplying())
    return;

  std::vector<ObjectState> before;
  std::vector<ObjectState> after;

  for (size_t k = 0; k < ids.size(); ++k) {
    const auto obj = m_scene ? m_scene->findById(ids[k]) : nullptr;
    if (obj && obj->isDerived()) {
      qWarning("[modifiers] recordTransformEdit: skipped derived object %u", ids[k]);
      Q_ASSERT_X(false, "recordTransformEdit", "derived object in a recorded edit");
      continue;
    }
    if (!obj)
      continue;
    const auto mesh = obj->getMesh();

    ObjectState was;
    was.id = ids[k];
    was.position = k < basePos.size() ? basePos[k] : obj->getPosition();
    was.rotation = obj->getRotation();
    was.scale = k < baseScale.size() ? baseScale[k] : obj->getScale();
    ObjectState now = captureObjectState(ids[k], false);

    const bool moved = was.position != now.position || was.scale != now.scale
        || was.rotation != now.rotation;

    // Face bases only when the texture lock rewrote them, so undo of a plain move rewrites no DrawBlocks.
    if (mesh && k < baseUv.size()
        && baseUv[k].bases.size() == static_cast<size_t>(mesh->faceCount())) {
      bool facesChanged = false;
      for (int f = 0; f < mesh->faceCount(); ++f) {
        if (faceBasisChanged(baseUv[k].bases[f], mesh->faceTexBasis(f))) {
          facesChanged = true;
          break;
        }
      }
      if (facesChanged) {
        was.faceBases = baseUv[k].bases;
        now.faceBases.reserve(static_cast<size_t>(mesh->faceCount()));
        for (int f = 0; f < mesh->faceCount(); ++f)
          now.faceBases.push_back(mesh->faceTexBasis(f));
      }
    }

    if (!moved && was.faceBases.empty())
      continue;

    before.push_back(std::move(was));
    after.push_back(std::move(now));
  }

  if (!before.empty())
    m_undoStack->push(std::make_unique<ObjectStatesCommand>(this, label, std::move(before), std::move(after)));
}

void Engine3D::setSunDirection(const glm::vec3 &dir) {
  const glm::vec3 n = glm::normalize(dir);
  if (n == m_sunDirection)
    return;
  recordValue(QStringLiteral("sun.direction"), QStringLiteral("Sun direction"), m_sunDirection, n,
              &Engine3D::setSunDirection);
  applySunDirection(n);
}

void Engine3D::applySunDirection(const glm::vec3 &dir) {
  m_sunDirection = dir;
  m_shadowDirty = true;
  syncSunToSceneLight();
  emit sunChanged();
}

namespace {

// Direction the sunlight travels (Y-up: elevation 90 points straight down).
glm::vec3 sunDirFromAngles(float azimuthDeg, float elevationDeg) {
  const float az = glm::radians(azimuthDeg);
  const float el = glm::radians(elevationDeg);
  const glm::vec3 towardSun(std::cos(el) * std::cos(az), std::sin(el),
                           -std::cos(el) * std::sin(az));
  return glm::normalize(-towardSun);
}

} // namespace

float Engine3D::sunAzimuth() const {
  const glm::vec3 toward = -m_sunDirection;
  float deg = std::atan2(-toward.z, toward.x) * 180.0f / 3.14159265f;
  if (deg < 0.0f)
    deg += 360.0f;
  return deg;
}

float Engine3D::sunElevation() const {
  const glm::vec3 toward = -m_sunDirection;
  const float len = glm::length(glm::vec2(toward.x, toward.z));
  return std::atan2(toward.y, len) * 180.0f / 3.14159265f;
}

void Engine3D::setSunAzimuth(const float degrees) {
  const glm::vec3 next = sunDirFromAngles(degrees, sunElevation());
  if (glm::length(next - m_sunDirection) < 1e-6f)
    return;
  recordValue(QStringLiteral("sun.azimuth"), QStringLiteral("Sun azimuth"), sunAzimuth(), degrees,
              &Engine3D::setSunAzimuth);
  applySunDirection(next);
}

void Engine3D::setSunElevation(const float degrees) {
  // Short of the horizon and zenith: the light's lookAt up-vector and the shadow fit degenerate there.
  const float el = std::clamp(degrees, 1.0f, 89.5f);
  const glm::vec3 next = sunDirFromAngles(sunAzimuth(), el);
  if (glm::length(next - m_sunDirection) < 1e-6f)
    return;
  recordValue(QStringLiteral("sun.elevation"), QStringLiteral("Sun elevation"), sunElevation(), el,
              &Engine3D::setSunElevation);
  applySunDirection(next);
}

void Engine3D::setSunIntensity(const float intensity) {
  if (std::fabs(m_sunIntensity - intensity) < 1e-4f)
    return;
  recordValue(QStringLiteral("sun.intensity"), QStringLiteral("Sun intensity"), m_sunIntensity,
              intensity, &Engine3D::setSunIntensity);
  m_sunIntensity = intensity;
  syncSunToSceneLight();
  emit sunChanged();
}

QColor Engine3D::sunColor() const {
  const auto ch = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  return QColor(ch(m_sunColor.r), ch(m_sunColor.g), ch(m_sunColor.b));
}

void Engine3D::setSunColor(const QColor &color) {
  const glm::vec3 c(color.redF(), color.greenF(), color.blueF());
  if (glm::length(c - m_sunColor) < 1e-4f)
    return;
  recordValue(QStringLiteral("sun.color"), QStringLiteral("Sun color"), sunColor(), color,
              &Engine3D::setSunColor);
  m_sunColor = c;
  syncSunToSceneLight();
  emit sunChanged();
}

QColor Engine3D::ambientColor() const {
  const auto ch = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  return QColor(ch(m_ambientColor.r), ch(m_ambientColor.g), ch(m_ambientColor.b));
}

void Engine3D::setAmbientColor(const QColor &color) {
  const glm::vec3 c(color.redF(), color.greenF(), color.blueF());
  if (glm::length(c - m_ambientColor) < 1e-4f)
    return;
  recordValue(QStringLiteral("ambient.color"), QStringLiteral("Ambient color"), ambientColor(), color,
              &Engine3D::setAmbientColor);
  m_ambientColor = c;
  emit sunChanged();
}

void Engine3D::setAmbientIntensity(const float intensity) {
  if (std::fabs(m_ambientIntensity - intensity) < 1e-4f)
    return;
  recordValue(QStringLiteral("ambient.intensity"), QStringLiteral("Ambient intensity"),
              m_ambientIntensity, intensity, &Engine3D::setAmbientIntensity);
  m_ambientIntensity = intensity;
  emit sunChanged();
}

void Engine3D::setFogDensity(const float density) {
  const float d = std::max(0.0f, density);
  if (std::fabs(m_fogDensity - d) < 1e-6f)
    return;
  recordValue(QStringLiteral("fog.density"), QStringLiteral("Fog density"), m_fogDensity, d,
              &Engine3D::setFogDensity);
  m_fogDensity = d;
  emit sunChanged();
}

QColor Engine3D::fogColor() const {
  const auto ch = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  return QColor(ch(m_fogColor.r), ch(m_fogColor.g), ch(m_fogColor.b));
}

void Engine3D::setFogColor(const QColor &color) {
  const glm::vec3 c(color.redF(), color.greenF(), color.blueF());
  if (glm::length(c - m_fogColor) < 1e-4f)
    return;
  recordValue(QStringLiteral("fog.color"), QStringLiteral("Fog color"), fogColor(), color,
              &Engine3D::setFogColor);
  m_fogColor = c;
  emit sunChanged();
}

void Engine3D::setExposure(const float exposure) {
  const float e = std::max(0.05f, exposure);
  if (std::fabs(m_exposure - e) < 1e-4f)
    return;
  recordValue(QStringLiteral("exposure"), QStringLiteral("Exposure"), m_exposure, e,
              &Engine3D::setExposure);
  m_exposure = e;
  emit sunChanged();
}

void Engine3D::setShadowsEnabled(const bool on) {
  if (m_shadowsEnabled == on)
    return;
  recordValue(QStringLiteral("shadows.enabled"), QStringLiteral("Shadows"), m_shadowsEnabled, on,
              &Engine3D::setShadowsEnabled);
  m_shadowsEnabled = on;
  syncSunToSceneLight();
  emit sunChanged();
}

void Engine3D::setShadowBias(const float bias) {
  if (std::fabs(m_shadowBias - bias) < 1e-6f)
    return;
  recordValue(QStringLiteral("shadows.bias"), QStringLiteral("Shadow bias"), m_shadowBias,
              std::max(0.0f, bias), &Engine3D::setShadowBias);
  m_shadowBias = std::max(0.0f, bias);
  emit sunChanged();
}

void Engine3D::setShadowNormalBias(const float bias) {
  if (std::fabs(m_shadowNormalBias - bias) < 1e-5f)
    return;
  recordValue(QStringLiteral("shadows.normalBias"), QStringLiteral("Shadow normal bias"),
              m_shadowNormalBias, std::max(0.0f, bias), &Engine3D::setShadowNormalBias);
  m_shadowNormalBias = std::max(0.0f, bias);
  emit sunChanged();
}

void Engine3D::setShadowRadius(const float radius) {
  if (std::fabs(m_shadowRadius - radius) < 1e-4f)
    return;
  recordValue(QStringLiteral("shadows.radius"), QStringLiteral("Shadow radius"), m_shadowRadius,
              std::max(0.0f, radius), &Engine3D::setShadowRadius);
  m_shadowRadius = std::max(0.0f, radius);
  emit sunChanged();
}

void Engine3D::setShadowStrength(const float strength) {
  const float s = std::clamp(strength, 0.0f, 1.0f);
  if (std::fabs(m_shadowStrength - s) < 1e-4f)
    return;
  recordValue(QStringLiteral("shadows.strength"), QStringLiteral("Shadow strength"), m_shadowStrength,
              s, &Engine3D::setShadowStrength);
  m_shadowStrength = s;
  emit sunChanged();
}

void Engine3D::setShadowRange(const float range) {
  const float r = std::clamp(range, 20.0f, 8000.0f);
  if (std::fabs(m_shadowFar - r) < 1e-3f)
    return;
  recordValue(QStringLiteral("shadows.range"), QStringLiteral("Shadow range"), m_shadowFar, r,
              &Engine3D::setShadowRange);
  m_shadowFar = r;
  emit sunChanged();
}

void Engine3D::setShadowMapSize(const int size) {
  const int clamped = std::clamp(size, 256, 8192);
  if (clamped == m_shadowMapSize)
    return;
  recordValue(QStringLiteral("shadows.mapSize"), QStringLiteral("Shadow map size"), m_shadowMapSize,
              clamped, &Engine3D::setShadowMapSize);
  m_shadowMapSize = clamped;
  // ensureShadowTarget() rebuilds the texture next frame; its generation bump invalidates the old SRBs.
  emit sunChanged();
}

void Engine3D::setSsaoEnabled(const bool on) {
  if (m_ssaoEnabled == on)
    return;
  recordValue(QStringLiteral("ssao.enabled"), QStringLiteral("SSAO"), m_ssaoEnabled, on,
              &Engine3D::setSsaoEnabled);
  m_ssaoEnabled = on;
  emit sunChanged();
}

void Engine3D::setSsaoRadius(const float radius) {
  const float r = std::clamp(radius, 0.05f, 20.0f);
  if (std::fabs(m_ssaoRadius - r) < 1e-4f)
    return;
  recordValue(QStringLiteral("ssao.radius"), QStringLiteral("SSAO radius"), m_ssaoRadius, r,
              &Engine3D::setSsaoRadius);
  m_ssaoRadius = r;
  emit sunChanged();
}

void Engine3D::setSsaoIntensity(const float intensity) {
  const float v = std::clamp(intensity, 0.0f, 3.0f);
  if (std::fabs(m_ssaoIntensity - v) < 1e-4f)
    return;
  recordValue(QStringLiteral("ssao.intensity"), QStringLiteral("SSAO intensity"), m_ssaoIntensity, v,
              &Engine3D::setSsaoIntensity);
  m_ssaoIntensity = v;
  emit sunChanged();
}

void Engine3D::setMsaaSamples(const int samples) {
  int clamped = 1;
  if (samples >= 8)
    clamped = 8;
  else if (samples >= 4)
    clamped = 4;
  else if (samples >= 2)
    clamped = 2;
  if (m_msaaSamples == clamped)
    return;
  m_msaaSamples = clamped;
  emit msaaSamplesChanged();
}

void Engine3D::syncSunToSceneLight() {
  if (!m_sceneLight || m_pushingSunToLight)
    return;

  m_pushingSunToLight = true;
  m_sceneLight->setDirection(QVector3D(m_sunDirection.x, m_sunDirection.y, m_sunDirection.z));
  m_sceneLight->setColor(QVector3D(m_sunColor.r, m_sunColor.g, m_sunColor.b));
  m_sceneLight->setIntensity(m_sunIntensity);
  m_sceneLight->setEnabled(m_shadowsEnabled);
  m_pushingSunToLight = false;
}

void Engine3D::adoptSunFromSceneLight() {
  if (!m_scene || m_scene->directionalLights().isEmpty() || m_pushingSunToLight)
    return;

  eng::DirectionalLight *light = m_scene->directionalLights().first();
  if (!light)
    return;
  m_sceneLight = light;

  const QVector3D dir = light->direction();
  const glm::vec3 d(dir.x(), dir.y(), dir.z());
  if (glm::length(d) > 1e-5f)
    m_sunDirection = glm::normalize(d);

  const QVector3D c = light->color();
  m_sunColor = glm::vec3(c.x(), c.y(), c.z());
  m_sunIntensity = light->intensity();
  m_shadowsEnabled = light->enabled();
  m_shadowDirty = true;
  emit sunChanged();
}

void Engine3D::setWireframeThickness(const float px) {
  m_wireframeThicknessPx = std::max(1.0f, px);
}

void Engine3D::setRenderDistanceSource(const float sourceUnits) {
  const float next = std::clamp(sourceUnits, kRenderDistanceMinSource, kRenderDistanceMaxSource);
  if (qFuzzyCompare(next, m_renderDistanceSource))
    return;
  m_renderDistanceSource = next;
  prefs::get().setValue(QStringLiteral("CameraRenderDistance"), static_cast<double>(next));
  qDebug().noquote()
      << QStringLiteral("[settings] 3d camera distance %1 source units = %2 world units")
              .arg(m_renderDistanceSource, 0, 'f', 1)
              .arg(renderDistanceWorld(), 0, 'f', 1);
  emit renderDistanceChanged();
}

bool Engine3D::withinRenderDistance(const eng::GameObject &object) const {
  if (object.hidden() || toolBrushHidden(object))
    return false;
  const auto [lo, hi] = object.getWorldAABB();
  const glm::vec3 closest = glm::clamp(m_camera->getPosition(), lo, hi);
  return glm::distance(m_camera->getPosition(), closest) <= renderDistanceWorld();
}

void Engine3D::setLightRenderDistanceSource(const float sourceUnits) {
  const float next = std::clamp(sourceUnits, kRenderDistanceMinSource, kRenderDistanceMaxSource);
  if (qFuzzyCompare(next, m_lightRenderDistanceSource))
    return;
  m_lightRenderDistanceSource = next;
  prefs::get().setValue(QStringLiteral("LightRenderDistance"), static_cast<double>(next));
  qDebug().noquote()
      << QStringLiteral("[settings] light render distance %1 source units = %2 world units")
              .arg(m_lightRenderDistanceSource, 0, 'f', 1)
              .arg(lightRenderDistanceWorld(), 0, 'f', 1);
  emit renderDistanceChanged();
}

void Engine3D::setShadowRenderDistanceSource(const float sourceUnits) {
  const float next = std::clamp(sourceUnits, kRenderDistanceMinSource, kRenderDistanceMaxSource);
  if (qFuzzyCompare(next, m_shadowRenderDistanceSource))
    return;
  m_shadowRenderDistanceSource = next;
  prefs::get().setValue(QStringLiteral("ShadowRenderDistance"), static_cast<double>(next));
  qDebug().noquote()
      << QStringLiteral("[settings] shadow render distance %1 source units = %2 world units")
              .arg(m_shadowRenderDistanceSource, 0, 'f', 1)
              .arg(shadowRenderDistanceWorld(), 0, 'f', 1);
  emit renderDistanceChanged();
}

QVector<EntityMarker> Engine3D::entityMarkers() const {
  QVector<EntityMarker> markers;
  if (!noEntities())
    appendEntityMarkers(markers);
  return markers;
}

void Engine3D::setGridLevelOffset(const int offset) {
  const int clamped = std::clamp(offset, kGridOffsetMin, kGridOffsetMax);
  if (clamped == m_gridLevelOffset)
    return;
  m_gridLevelOffset = clamped;
  emit gridLevelChanged(m_gridLevelOffset);
}

void Engine3D::stepGridLevel(const int direction) {
  const int step = direction > 0 ? 1 : (direction < 0 ? -1 : 0);
  if (step == 0)
    return;
  setGridLevelOffset(m_gridLevelOffset + step);
  qDebug().noquote() << QStringLiteral("[grid] level -> %1 (offset %2)")
                           .arg(m_gridLevelOffset + kGridLevelBase)
                           .arg(m_gridLevelOffset);
}

void Engine3D::handleKeyPress(QKeyEvent *event) {
  requestRedraw();
  if (event->isAutoRepeat())
    return;
  const int key = event->key();
  if (key >= 0 && key < 128)
    m_keysPressed[key] = true;
}

void Engine3D::handleKeyRelease(QKeyEvent *event) {
  if (event->isAutoRepeat())
    return;
  const int key = event->key();
  if (key >= 0 && key < 128)
    m_keysPressed[key] = false;
}

void Engine3D::releaseAllKeys() {
  // A key released while another window had focus never reaches us.
  m_keysPressed.fill(false);
}

bool Engine3D::toggleFreeLook() {
  m_freeLook = !m_freeLook;
  return m_freeLook;
}

void Engine3D::updateCamera(const float deltaTime) {
  m_cameraMovedThisFrame = false;
  // Looking through a Camera3D entity: no flying.
  if (m_previewCamera) {
    m_flyVelocity = glm::vec3(0.0f);
    m_freelookDx = m_freelookDy = 0;
    applyCameraPreview();
    return;
  }
  if (prefs::b("Viewport/FlyRequiresRightMouse")
      && !QGuiApplication::mouseButtons().testFlag(Qt::RightButton)) {
    for (const Qt::Key k: {Qt::Key_W, Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_Q, Qt::Key_E})
      m_keysPressed[static_cast<int>(k)] = false;
  }
  const auto held = [this](const Qt::Key key) {
    return m_keysPressed[static_cast<int>(key)];
  };
  glm::vec3 wish(0.0f);
  if (held(Qt::Key_W)) wish += m_camera->getDirection();
  if (held(Qt::Key_S)) wish -= m_camera->getDirection();
  if (held(Qt::Key_D)) wish += m_camera->getRight();
  if (held(Qt::Key_A)) wish -= m_camera->getRight();
  if (held(Qt::Key_E)) wish += glm::vec3(0.0f, 1.0f, 0.0f);
  if (held(Qt::Key_Q)) wish -= glm::vec3(0.0f, 1.0f, 0.0f);

  float speed = m_cameraSpeed;
  if (QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ShiftModifier))
    speed *= m_flyShiftMultiplier;
  const glm::vec3 target = glm::length(wish) > 0.0f ? glm::normalize(wish) * speed : glm::vec3(0.0f);

  // Exponential smoothing (0 = instant), frame-rate independent: the factor is per 1/60 s.
  if (m_flySmoothing <= 0.0f) {
    m_flyVelocity = target;
  } else {
    const float k = 1.0f - std::pow(m_flySmoothing, deltaTime * 60.0f);
    m_flyVelocity += (target - m_flyVelocity) * k;
    if (glm::length(target) == 0.0f && glm::length(m_flyVelocity) < 0.01f)
      m_flyVelocity = glm::vec3(0.0f);
  }
  if (glm::length(m_flyVelocity) > 0.0f) {
    m_camera->setPose(m_camera->getPosition() + m_flyVelocity * deltaTime, m_camera->getYaw(),
                      m_camera->getPitch());
    m_cameraMovedThisFrame = true;
  }

  if (m_freelookDx != 0 || m_freelookDy != 0) {
    m_camera->rotate(-m_freelookDx, m_freelookDy);
    m_freelookDx = 0;
    m_freelookDy = 0;
    m_cameraMovedThisFrame = true;
  }
  if (m_camera->getYaw() != m_lastYaw || m_camera->getPitch() != m_lastPitch) {
    m_lastYaw = m_camera->getYaw();
    m_lastPitch = m_camera->getPitch();
    QMetaObject::invokeMethod(this, [this] { emit cameraOrientationChanged(); }, Qt::QueuedConnection);
  }
}

quint64 Engine3D::viewSignature() const {
  const auto mix = [](quint64 h, quint64 v) { return (h ^ v) * 1099511628211ull; };
  const auto bits = [](float f) {
    quint32 u;
    std::memcpy(&u, &f, sizeof(u));
    return quint64(u);
  };
  quint64 h = 1469598103934665603ull;
  const glm::vec3 p = m_camera->getPosition();
  h = mix(h, bits(p.x));
  h = mix(h, bits(p.y));
  h = mix(h, bits(p.z));
  h = mix(h, bits(m_camera->getYaw()));
  h = mix(h, bits(m_camera->getPitch()));
  h = mix(h, bits(m_camera->getFov()));
  h = mix(h, m_viewEpoch);
  h = mix(h, m_entityEpoch * 31 + m_selectedEntities.size());
  h = mix(h, quint64(m_selectedObjects.size()) << 20 | quint64(m_selectedFaces.size()));
  h = mix(h, m_scene ? m_scene->structureVersion() : 0);
  h = mix(h, quint64(reinterpret_cast<quintptr>(m_scene.data())));
  return h;
}

bool Engine3D::wantsContinuousFrames() const {
  bool keyHeld = false;
  for (const Qt::Key k: {Qt::Key_W, Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_Q, Qt::Key_E})
    keyHeld = keyHeld || m_keysPressed[static_cast<int>(k)];
  return keyHeld || glm::length(m_flyVelocity) > 0.0f || m_freelookDx != 0 || m_freelookDy != 0
         || TextureManager::instance().pendingCount(m_rhi) > 0 || m_cameraMovedThisFrame || (m_ioSim && !m_ioSimPaused);
}

void Engine3D::performPicking(const float x, const float y, const bool additive) {
  clearMarquee();
  // Material mode: Ctrl+click pastes the copied face attributes, Ctrl+Shift+click wraps them.
  if (g::editorMode == g::EditorMode::MATERIAL
      && QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier) && m_faceClip.valid) {
    pasteFaceAttributesAt(x, y, additive ? 2 : 1);
    return;
  }
  if (m_editMode && pickVertexHandle(x, y, additive))
    return;
  if (m_editMode) {
    // Edit mode keeps its brushes: a click on nothing only drops the vertices.
    if (!additive)
      clearVertexSelection();
    return;
  }
  pick(x, y, static_cast<float>(m_lastViewSize.width()), static_cast<float>(m_lastViewSize.height()),
       additive, g::editorMode == g::EditorMode::MATERIAL);
}

void Engine3D::releaseResources() {
  TextureManager::instance().releaseSrbsOf(this);
  invalidateDerivedCaches();
  releaseFxaa();
  releaseIconAtlas();
  releaseLutTexture();
  releaseSkyTexture();
  m_faceSigById.clear();
  m_faceStructVersion = ~0ull;
  if (m_text)
    m_text->invalidate();
  m_gpu.invalidate();

  delete m_sceneTemplateSrb;
  delete m_shadowSrb;
  delete m_wireSrb;
  delete m_selDepthSrb;
  m_sceneTemplateSrb = nullptr;
  m_shadowSrb = nullptr;
  m_wireSrb = nullptr;
  m_selDepthSrb = nullptr;
  m_builtBufferGen = ~0ull;

  m_scenePipeline.reset();
  m_shadowPipeline.reset();
  m_wirePipeline.reset();
  m_selDepthPipeline.reset();

  delete m_sceneUbo;
  delete m_objectUbo;
  delete m_drawUbo;
  delete m_overlayUbo;
  m_sceneUbo = m_objectUbo = m_drawUbo = m_overlayUbo = nullptr;

  delete m_marqueePipeline;
  delete m_marqueeSrb;
  delete m_marqueeVb;
  m_marqueePipeline = nullptr;
  m_marqueeSrb = nullptr;
  m_marqueeVb = nullptr;
  m_marqueeBatch = nullptr;
  m_marqueeVertexCount = 0;
  m_marquee = {};
  m_dragTarget = DragTarget::None;

  delete m_cutWirePipeline;
  delete m_cutFillPipeline;
  delete m_cutWireSrb;
  delete m_cutWireVb;
  m_cutWirePipeline = nullptr;
  m_cutFillPipeline = nullptr;
  m_cutFillVertexCount = 0;
  m_cutWireSrb = nullptr;
  m_cutWireVb = nullptr;
  m_cutWireBatch = nullptr;
  m_cutWireVertexCount = 0;
  releaseIOLinePipelines();
  delete m_ioLineVb;
  m_ioLineVb = nullptr;
  m_ioLineBatch = nullptr;
  m_ioLineVertexCount = 0;
  releaseCollisionPipelines();
  delete m_collisionVb;
  m_collisionVb = nullptr;
  m_collisionBatch = nullptr;
  delete m_mirrorVb;
  m_mirrorVb = nullptr;
  delete m_pathVb;
  m_pathVb = nullptr;
  m_pathBatch = nullptr;
  m_pathEdgeCount = 0;
  m_mirrorOverlayKey = ~0ull;
  m_mirrorBatch = nullptr;
  m_mirrorFillCount = m_mirrorEdgeCount = 0;
  m_collisionFillCount = m_collisionEdgeCount = 0;
  m_collisionBuiltKey = ~0ull;

  delete m_fillWireVb;
  delete m_fillWireIb;
  delete m_layerObjUbo;
  delete m_layerOverlayWallUbo;
  delete m_layerOverlaySpaceUbo;
  m_fillWireVb = nullptr;
  m_fillWireIb = nullptr;
  m_fillWireCapacity = 0;
  m_fillWireIbCapacity = 0;
  m_fillWireBatch = nullptr;
  m_fillWireWallIdxCount = 0;
  m_fillWireSpaceIdxCount = 0;

  m_entityTriDepthPipeline.reset();
  m_entityTriOverlayPipeline.reset();
  m_entityLinePipeline.reset();
  delete m_entitySrb;
  m_entitySrb = nullptr;
  releaseEntityResources(false);
  m_entityGeomBuilt = false;
  m_entityDraws.clear();
  m_entityScratch.clear();
  m_gizmoDrag = GizmoHandle::None;
  m_gizmoHover = GizmoHandle::None;

  delete m_shadowTarget;
  delete m_shadowRenderPass;
  delete m_shadowDepthBuffer;
  delete m_shadowTexture;
  m_shadowTarget = nullptr;
  m_shadowRenderPass = nullptr;
  m_shadowDepthBuffer = nullptr;
  m_shadowTexture = nullptr;
  m_builtShadowMapSize = 0;

  m_gbufferPipeline.reset();
  m_ssaoPipeline.reset();
  m_aoBlurPipeline.reset();
  m_aoApplyPipeline.reset();
  m_skyPipeline.reset();
  releaseSsaoTargets(false);
  delete m_ssaoUbo;
  m_ssaoUbo = nullptr;
  delete m_quadVb;
  delete m_quadIb;
  m_quadVb = m_quadIb = nullptr;

  delete m_selDepthTarget;
  delete m_selDepthRp;
  delete m_selDepthDs;
  delete m_selDepthTex;
  m_selDepthTarget = nullptr;
  m_selDepthRp = nullptr;
  m_selDepthDs = nullptr;
  m_selDepthTex = nullptr;
  m_overlaySize = QSize();
  m_overlayFailedSize = QSize();
  m_overlayRetry = 0;
  m_ssaoFailedSize = QSize();
  m_ssaoRetry = 0;

  m_builtRenderPass = nullptr;
  m_builtSampleCount = 0;
  m_builtOverlaySize = QSize();
  m_rhi = nullptr;
  clearObjectSlotState();
  m_faceSigById.clear();
  m_faceStructVersion = ~0ull;
}

void Engine3D::ensureBuffers() {
  QRhi *rhi = Shared::instance().rhi();
  if (!m_lightFaceUbo) {
    m_lightFaceStride = rhi->ubufAligned(sizeof(LightShadowFace));
    m_lightFaceUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                    m_lightFaceStride * kLightAtlasTilesPerRow * kLightAtlasTilesPerRow);
    if (!m_lightFaceUbo->create()) {
      delete m_lightFaceUbo;
      m_lightFaceUbo = nullptr;
    } else {
      ++m_bufferGen;
    }
  }
  if (!m_cascadeUbo) {
    m_cascadeStride = rhi->ubufAligned(sizeof(CascadeBlock));
    m_cascadeUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, m_cascadeStride * 4);
    if (!m_cascadeUbo->create()) {
      delete m_cascadeUbo;
      m_cascadeUbo = nullptr;
    } else {
      ++m_bufferGen;
    }
  }
  if (!m_sceneUbo) {
    m_sceneUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(SceneUniforms));
    m_sceneUbo->create();
  }
  if (!m_overlayUbo) {
    m_overlayUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(OverlayUniforms));
    m_overlayUbo->create();
  }
  if (!m_ssaoUbo) {
    m_ssaoUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(SsaoUniforms));
    m_ssaoUbo->create();
  }
  // Layer overlay: identity model block, one overlay block per layer kind (the wire colour is a UBO value).
  if (!m_layerObjUbo) {
    m_layerObjUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                   int(rhi->ubufAligned(sizeof(ObjectBlock))));
    if (!m_layerObjUbo->create()) {
      qWarning(engLog, "layer overlay object UBO failed");
      delete m_layerObjUbo;
      m_layerObjUbo = nullptr;
    }
  }
  if (!m_layerOverlayWallUbo) {
    m_layerOverlayWallUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                          sizeof(OverlayUniforms));
    m_layerOverlayWallUbo->create();
  }
  if (!m_layerOverlaySpaceUbo) {
    m_layerOverlaySpaceUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                           sizeof(OverlayUniforms));
    m_layerOverlaySpaceUbo->create();
  }
  syncObjectSlots();

  const quint32 objectCount = std::max<quint32>(m_objectSlotAlloc.highWater(), 1u);
  // Slots, not live faces: freed runs leave holes below the high-water mark.
  const quint32 faceCount = std::max<quint32>(m_gpu.faceSlots(), 1u);
  m_objectStride = rhi->ubufAligned(sizeof(ObjectBlock));
  // Per-face data is a storage buffer read by index: tightly packed.
  m_drawStride = int(sizeof(DrawBlock));

  if (objectCount > m_objectCapacity) {
    const quint32 capacity = std::max(objectCount, m_objectCapacity * 2);
    if (m_objectUbo)
      Shared::instance().retire(m_objectUbo);
    m_objectUbo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, capacity * m_objectStride);
    m_objectUbo->create();
    m_objectCapacity = capacity;
    // Slots stay, but every object must be written again into the new buffer.
    m_objectUploadedVersion.clear();
    m_uploadedTransformEpoch = ~0ull;
    ++m_bufferGen;
  }

  if (faceCount > m_drawCapacity) {
    const quint32 capacity = std::max(faceCount, m_drawCapacity * 2);
    if (m_drawUbo)
      Shared::instance().retire(m_drawUbo);
    m_drawUbo = rhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::StorageBuffer, capacity * m_drawStride);
    m_drawUbo->create();
    m_drawCapacity = capacity;
    // The new buffer is empty: every face block counts as unwritten.
    m_faceSigById.clear();
    m_faceStructVersion = ~0ull;
      ++m_bufferGen;
  }
}

// RAII perf probe for the drag input paths (too many early returns for stage()).
struct ScopedDragPerf {
  ScopedDragPerf(double &bucket, qint64 &calls)
      : m_bucket(bucket), m_calls(calls), m_on(perfOn()) {
    if (m_on) {
      m_t.start();
      ++m_calls;
    }
  }
  ~ScopedDragPerf() {
    if (m_on && m_t.isValid())
      m_bucket += m_t.nsecsElapsed() * 1e-6;
  }

  double &m_bucket;
  qint64 &m_calls;
  bool m_on = false;
  QElapsedTimer m_t;
};

static inline quint64 mixSig(quint64 a, const quint64 b) {
  return a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2));
}

static inline quint64 hashFloats(const float *v, const size_t count) {
  quint64 h = 1469598103934665603ull;
  const auto *bytes = reinterpret_cast<const unsigned char *>(v);
  for (size_t i = 0; i < count * sizeof(float); ++i) {
    h ^= bytes[i];
    h *= 1099511628211ull;
  }
  return h;
}

void Engine3D::computeFrameSignatures() {
  if (!m_scene) {
    m_sigGeometry = 0;
    return;
  }

  // O(1): global epochs instead of a sweep of every object's counters. Includes the face epoch.
  refreshCollisionSig();
  m_sigGeometry = mixSig(mixSig(mixSig(mixSig(m_scene->structureVersion(),
                                             eng::GameObject::globalTransformEpoch()),
                                      eng::Mesh::globalGeometryEpoch()),
                               eng::Mesh::globalFaceEpoch()),
                        mixSig(m_collisionSig, m_physPoseEpoch));
}

// Drops every derived signature. Deliberately not the per-face block map: it is reconciled against the
// structure version, and clearing it would make every delete a full ~4 MB rewrite.
void Engine3D::invalidateDerivedCaches() {
  m_uploadedTransformEpoch = ~0ull;
  m_shadowMapKey = ~0ull;
  m_visibleKey = ~0ull;
}

void Engine3D::clearObjectSlotState() {
  m_objectSlotAlloc.clear();
  m_objectSlots.clear();
  m_objectUploadedVersion.clear();
  m_uploadedStructureVersion = ~0ull;
}

// Object UBO slots for live objects, freed for gone ones. Runs before the UBO is sized (to the high-water mark).
void Engine3D::syncObjectSlots() {
  if (!m_scene || m_uploadedStructureVersion == m_scene->structureVersion())
    return;

  m_uploadedStructureVersion = m_scene->structureVersion();

  QHash<quint32, bool> present;
  present.reserve(qsizetype(m_scene->gameObjects.size()) * 2);
  for (const auto &obj: m_scene->gameObjects)
    present.insert(obj->stableId(), true);

  for (auto it = m_objectSlots.begin(); it != m_objectSlots.end();) {
    if (present.contains(it.key())) {
      ++it;
      continue;
    }
    m_objectSlotAlloc.release(it.value(), 1);
    m_objectUploadedVersion.remove(it.key());
    it = m_objectSlots.erase(it);
  }

  for (const auto &obj: m_scene->gameObjects) {
    if (m_objectSlots.contains(obj->stableId()))
      continue;
    m_objectSlots.insert(obj->stableId(), m_objectSlotAlloc.allocate(1));
  }
}

// Slots are keyed by stable id, so a delete does not rewrite the survivors' blocks.
void Engine3D::uploadObjectBlocks(QRhiResourceUpdateBatch *rub) {
  if (!m_scene || !m_objectUbo)
    return;

  // The block carries the first face slot, which moves when the pools re-allocate: face epoch in the key.
  const quint64 epoch = eng::GameObject::globalTransformEpoch() * 1000003ull ^ quint64(m_gpu.faceEpoch()) * 7919ull
                        ^ quint64(m_bufferGen) ^ m_physPoseEpoch * 0x9e3779b97f4a7c15ull;
  if (epoch == m_uploadedTransformEpoch)
    return;

  for (const auto &obj: m_scene->gameObjects) {
    const quint32 stableId = obj->stableId();
    const auto it = m_objectSlots.constFind(stableId);
    if (it == m_objectSlots.constEnd())
      continue;
    const MeshGpuRecord *rec = m_gpu.record(stableId);
    const quint32 faceOffset = rec ? rec->faceOffset : 0u;
    const glm::mat4 *pose = physicsPose(stableId);
    const quint64 version = (quint64(obj->transformVersion()) * 2654435761ull ^ quint64(faceOffset))
                            + (pose ? (m_physPoseEpoch + 1) * 0x9e3779b97f4a7c15ull : 0ull);
    if (m_objectUploadedVersion.value(stableId, 0) == version)
      continue;

    ObjectBlock block;
    block.model = pose ? *pose * obj->getModelMatrix() : obj->getModelMatrix();
    block.normalMatrix = glm::mat4(glm::inverseTranspose(glm::mat3(block.model)));
    float offsetBits;
    std::memcpy(&offsetBits, &faceOffset, sizeof(float));
    block.info = glm::vec4(offsetBits, 0.0f, 0.0f, 0.0f);

    rub->updateDynamicBuffer(m_objectUbo, it.value() * m_objectStride, sizeof(ObjectBlock), &block);
    m_objectUploadedVersion.insert(stableId, version);
  }

  m_uploadedTransformEpoch = epoch;
}

void Engine3D::uploadFaceBlocks(QRhiResourceUpdateBatch *rub) {
  if (!m_scene || !m_drawUbo)
    return;

  auto fillFace = [this](const eng::Mesh &mesh, int face) {
    const auto material = mesh.faceMaterial(face);
    DrawBlock block;
    // The texture basis is the whole UV transform (gradient in xyz, offset in w): UV = f(world position).
    const eng::FaceTexBasis &basis = mesh.faceTexBasis(face);
    block.uvWorldU = glm::vec4(basis.uAxis * basis.tilesPerUnitU, basis.offsetU);
    block.uvWorldV = glm::vec4(basis.vAxis * basis.tilesPerUnitV, basis.offsetV);
    quint32 flags = 0;
    if (material) {
      if (material->hasNormalMap())
        flags |= DrawFlagHasNormalMap;
      if (material->hasMetallicRoughnessMap())
        flags |= DrawFlagHasMRMap;
      if (material->hasEmissionMap())
        flags |= DrawFlagHasEmissionMap;
      // Godot StandardMaterial3D parameters (resolved property stack).
      const MaterialProps &p = material->props;
      block.faceInfo = glm::vec4(p.metallic, p.roughness, p.metallicSpecular, p.normalScale);
      block.matInfo = glm::vec4(p.aoLightAffect, p.emissionEnergy, 0.0f, 0.0f);
      auto toLinear = [](int c8) {
        const float c = float(c8) / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
      };
      block.emission = glm::vec4(toLinear(qRed(p.emissionColor)), toLinear(qGreen(p.emissionColor)),
                                 toLinear(qBlue(p.emissionColor)), 0.0f);
      if (p.transparency)
        flags |= DrawFlagTransparent;
      if (p.shadingMode == matprops::Shading::Unshaded)
        flags |= DrawFlagUnshaded;
      if (p.emissionEnabled)
        flags |= DrawFlagEmission;
    }
    if (mesh.faceUsesVertexUV(face))
      flags |= DrawFlagVertexUV;
    const quint32 pickingId = mesh.getFacePickingID(face);
    const quint32 stableId = pickingId > 0 ? eng::stableIdFromPicking(pickingId) : 0u;
    if (m_selectedFaces.count(pickingId) && !m_maskHidden) {
      flags |= DrawFlagSelectedFace;
    } else if (m_drawSelection.count(stableId) && !m_maskHidden) {
      flags |= DrawFlagSelected;
    }
    block.pickInfo = glm::uvec4(pickingId, 0, flags, 0);
    return block;
  };

  // Up to the high-water mark: deletes leave holes in the pool.
  const quint32 faceCount = m_gpu.faceSlots();
  if (faceCount == 0 || m_gpu.faceCount() == 0)
    return;

  // Staged at m_drawStride (what the draw indexes with), not sizeof(DrawBlock).
  const size_t stride = m_drawStride;
  const size_t bytes = size_t(faceCount) * stride;
  if (bytes > size_t(m_drawCapacity) * stride)
    return;

  if (m_drawScratch.size() < bytes)
    m_drawScratch.resize(bytes, 0);

  // Face blocks keyed by stable id: a delete uploads nothing for the survivors. The signature covers every input
  // of fillFace: face state, material epoch (a lazily loaded normal map changes the flags), selection.
  if (m_faceStructVersion != m_scene->structureVersion()) {
    m_faceStructVersion = m_scene->structureVersion();
    for (auto it = m_faceSigById.begin(); it != m_faceSigById.end();) {
      if (m_scene->indexOf(it.key()) < 0)
        it = m_faceSigById.erase(it);
      else
        ++it;
    }
  }

  QHash<quint32, quint32> selectedFaceCount;
  selectedFaceCount.reserve(qsizetype(m_selectedFaces.size()) * 2);
  for (const quint32 pickingId: m_selectedFaces)
    ++selectedFaceCount[eng::stableIdFromPicking(pickingId)];

  struct Range {
    quint32 start;
    quint32 count;
  };
  std::vector<Range> dirty;

  for (const auto &obj: m_scene->gameObjects) {
    const auto mesh = obj->getMesh();
    const MeshGpuRecord *rec = m_gpu.record(obj->stableId());
    if (!rec || rec->faceCount == 0)
      continue;
    // A record from a pool generation the buffers have not caught up with yet.
    if (quint64(rec->faceOffset) + rec->faceCount > faceCount)
      continue;

    const quint64 sel = m_drawSelection.count(obj->stableId()) ? 1ull : 0ull;
    const quint64 mask = m_maskHidden ? 1ull : 0ull;
    // geometryVersion: a mesh whose counts changed gets a new face range.
    quint64 sig = mixSig(mixSig(mesh->faceVersion(), mesh->geometryVersion()),
                              mixSig(eng::Material::globalEpoch(),
                                     mixSig(sel,
                                            mixSig(quint64(selectedFaceCount.value(obj->stableId(), 0)),
                                                   mask))));
    const glm::mat4 *pose = physicsPose(obj->stableId());
    if (pose)
      sig = mixSig(sig, m_physPoseEpoch + 1);
    const auto prev = m_faceSigById.constFind(obj->stableId());
    if (prev != m_faceSigById.constEnd() && prev.value() == sig)
      continue;
    m_faceSigById.insert(obj->stableId(), sig);

    for (quint32 f = 0; f < rec->faceCount; ++f) {
      DrawBlock block = fillFace(*mesh, static_cast<int>(f));
      if (pose) {
        // The texture moves with the body: U = dot(p, g) + o, p = Rᵀ(p' − t),
        // so g' = R·g and o' = o − dot(t, g').
        const glm::mat3 r(*pose);
        const glm::vec3 t((*pose)[3]);
        for (glm::vec4 *uv: {&block.uvWorldU, &block.uvWorldV}) {
          const glm::vec3 g = r * glm::vec3(*uv);
          *uv = glm::vec4(g, uv->w - glm::dot(t, g));
        }
      }
      std::memcpy(m_drawScratch.data() + size_t(rec->faceOffset + f) * stride, &block,
                  sizeof(DrawBlock));
    }
    dirty.push_back({rec->faceOffset, rec->faceCount});
  }

  if (dirty.empty()) {
    ++m_perf.uploadSkips;
    return;
  }

  // Contiguous ranges merge into one upload.
  std::sort(dirty.begin(), dirty.end(),
            [](const Range &a, const Range &b) { return a.start < b.start; });
  quint32 runStart = dirty.front().start;
  quint32 runEnd = dirty.front().start + dirty.front().count;
  for (size_t i = 1; i <= dirty.size(); ++i) {
    const bool merged = i < dirty.size() && dirty[i].start <= runEnd;
    if (merged) {
      runEnd = std::max(runEnd, dirty[i].start + dirty[i].count);
      continue;
    }
    const size_t off = size_t(runStart) * stride;
    const size_t len = size_t(runEnd - runStart) * stride;
    rub->uploadStaticBuffer(m_drawUbo, quint32(off), quint32(len), m_drawScratch.data() + off);
    if (i < dirty.size()) {
      runStart = dirty[i].start;
      runEnd = runStart + dirty[i].count;
    }
  }
}

QRhiShaderResourceBindings *Engine3D::materialSrb(eng::Material *material) {
  if (!material || !material->albedo)
    return nullptr;

  if (material->srb && material->srbOwner == this && material->srbVersion == material->version
      && material->srbBufferGen == m_bufferGen)
    return material->srb;

  ++m_perf.srbRebuilds;
  // An earlier frame in flight may still be bound to the old SRB.
  if (material->srb)
    Shared::instance().retireShaderResourceBindings(material->srb);
  material->srb = nullptr;
  material->srb = makeSrb(Shared::instance().rhi(), m_sceneUbo, m_objectUbo, m_drawUbo, nullptr,
                          material, nullptr, m_shadowTexture, true, m_lightAtlasTex, m_lutTex);
  material->srbOwner = this;
  material->srbVersion = material->version;
  material->srbBufferGen = m_bufferGen;
  return material->srb;
}

void Engine3D::updateSceneUniforms(const QSize &pixelSize) {
  QRhi *rhi = Shared::instance().rhi();
  const glm::mat4 corr = qtToGlm(rhi->clipSpaceCorrMatrix());

  const float aspect = pixelSize.height() > 0
                          ? static_cast<float>(pixelSize.width()) / static_cast<float>(pixelSize.height())
                          : 1.0f;
  const glm::mat4 view = m_camera->getViewMatrix();
  const glm::mat4 proj = m_camera->getProjectionMatrix(aspect);

  m_sceneUniforms.viewProj = corr * proj * view;
  m_sceneUniforms.view = view;
  // w: active colour LUT size (0 = none), read by scene.frag and sky.frag.
  m_sceneUniforms.cameraPos = glm::vec4(m_camera->getPosition(), float(m_lutActiveSize));
  // Strength 0 = the geometric normal (scene.frag shadeNormal).
  m_sceneUniforms.sunDir = glm::vec4(m_sunDirection, m_normalMaps ? m_normalStrength : 0.0f);
  m_sceneUniforms.sunColor = glm::vec4(m_sunColor, m_sunIntensity);
  m_sceneUniforms.ambient = glm::vec4(m_ambientColor, m_ambientIntensity);
  m_sceneUniforms.fogColor = glm::vec4(m_fogColor, float(m_tonemapper));

  const bool shadowsOn = m_shadowsEnabled && m_renderMode == RenderMode::Component && shadowUsable();
  m_sceneUniforms.params0 = glm::vec4(
      static_cast<float>(m_renderMode),
      shadowsOn ? 1.0f : 0.0f,
      1.0f / static_cast<float>(m_shadowMapSize),
      m_shadowDepthScale);

  m_sceneUniforms.params1 = glm::vec4(
      m_renderMode == RenderMode::Component ? m_fogDensity : m_fogDensity * 0.5f,
      m_shadowBias,
      m_exposure,
      1.0f / m_camera->getFar());

  m_sceneUniforms.shadowParams = glm::vec4(m_shadowRadius, m_shadowStrength, m_shadowNormalBias,
                                         float(m_shadowTaps + 100 * m_shadowFilter));
  m_sceneUniforms.shadowFit = glm::vec4(m_shadowWorldTexel, m_shadowFarPlane, m_shadowNearPlane,
                                       shadowRenderDistanceWorld());

  const float aoW = float(std::max(1, m_aoSize.width()));
  const float aoH = float(std::max(1, m_aoSize.height()));
  m_ssaoUniforms.clipFromView = corr * proj;
  m_ssaoUniforms.params = glm::vec4(std::max(0.05f, m_ssaoRadius),
                                   std::max(0.001f, m_ssaoRadius * 0.04f),
                                   std::max(0.0f, m_ssaoIntensity),
                                   0.012f);
  // z = kernel samples (quality), w = blur radius in AO texels.
  m_ssaoUniforms.info = glm::vec4(1.0f / aoW, 1.0f / aoH, float(m_ssaoSamples), m_ssaoSamples >= 32 ? 3.0f
                                                                                : m_ssaoSamples >= 16 ? 2.0f : 1.0f);
}

void Engine3D::updateLightTransform() {
  if (!m_scene || m_scene->gameObjects.empty())
    return;

  QRhi *rhi = Shared::instance().rhi();
  const glm::mat4 corr = qtToGlm(rhi->clipSpaceCorrMatrix());

  // Scene bounds are cached: the sweep is costly on big maps.
  const quint64 boundsKey = m_scene->structureVersion() * 1000003ull ^ eng::GameObject::globalTransformEpoch()
                            ^ (quint64(reinterpret_cast<quintptr>(m_scene.data())) << 1);
  if (boundsKey != m_sceneBoundsKey) {
    m_sceneBoundsKey = boundsKey;
    m_sceneBoundsMin = glm::vec3(FLT_MAX);
    m_sceneBoundsMax = glm::vec3(-FLT_MAX);
    for (const auto &obj: m_scene->gameObjects) {
      if (obj->hidden())
        continue;
      const auto [lo, hi] = obj->getWorldAABB();
      m_sceneBoundsMin = glm::min(m_sceneBoundsMin, lo);
      m_sceneBoundsMax = glm::max(m_sceneBoundsMax, hi);
    }
  }
  glm::vec3 sceneMin = m_sceneBoundsMin;
  glm::vec3 sceneMax = m_sceneBoundsMax;
  if (sceneMin.x > sceneMax.x)
    return;
  // Slack so a small nudge does not push a mesh out of the shadow volume.
  sceneMin -= glm::vec3(2.0f);
  sceneMax += glm::vec3(2.0f);

  const glm::vec3 sunDir = glm::normalize(m_sunDirection);
  glm::vec3 up(0.0f, 1.0f, 0.0f);
  if (std::fabs(glm::dot(sunDir, up)) > 0.999f)
    up = glm::vec3(0.0f, 0.0f, 1.0f);
  const glm::vec3 right = glm::normalize(glm::cross(sunDir, up));
  const glm::vec3 lightUp = glm::normalize(glm::cross(right, sunDir));

  // Fit the whole map, not the frustum: off-screen casters still cast and the texels do not crawl.
  // Camera-centred only when the map is too big for a usable texel size.
  glm::vec3 boxMin = sceneMin;
  glm::vec3 boxMax = sceneMax;
  const glm::vec3 span = sceneMax - sceneMin;
  const float maxSpan = std::max(20.0f, m_shadowFar);
  bool cameraFit = false;
  if (std::max({span.x, span.y, span.z}) > maxSpan) {
    cameraFit = true;
    const glm::vec3 cam = m_camera->getPosition();
    const glm::vec3 cutMin = cam - glm::vec3(maxSpan);
    const glm::vec3 cutMax = cam + glm::vec3(maxSpan);
    const glm::vec3 inMin = glm::max(boxMin, cutMin);
    const glm::vec3 inMax = glm::min(boxMax, cutMax);
    if (inMin.x < inMax.x && inMin.y < inMax.y && inMin.z < inMax.z) {
      boxMin = inMin;
      boxMax = inMax;
    }
  }

  const glm::vec3 centre = (boxMin + boxMax) * 0.5f;

  float tMin = FLT_MAX;
  float tMax = -FLT_MAX;
  float rMin = FLT_MAX;
  float rMax = -FLT_MAX;
  float uMin = FLT_MAX;
  float uMax = -FLT_MAX;
  for (int i = 0; i < 8; ++i) {
    const glm::vec3 corner((i & 1) != 0 ? boxMax.x : boxMin.x,
                         (i & 2) != 0 ? boxMax.y : boxMin.y,
                         (i & 4) != 0 ? boxMax.z : boxMin.z);
    const glm::vec3 d = corner - centre;
    tMin = std::min(tMin, glm::dot(d, sunDir));
    tMax = std::max(tMax, glm::dot(d, sunDir));
    rMin = std::min(rMin, glm::dot(d, right));
    rMax = std::max(rMax, glm::dot(d, right));
    uMin = std::min(uMin, glm::dot(d, lightUp));
    uMax = std::max(uMax, glm::dot(d, lightUp));
  }

  // Square footprint: equal texel sizes on both axes for the PCF blur.
  float half = std::max({std::fabs(rMin), std::fabs(rMax), std::fabs(uMin), std::fabs(uMax), 1.0f});
  // Big map: follows the camera at a constant size (the camera cube's radius); a varying size defeats the
  // texel snapping below.
  glm::vec3 fitCentre = centre;
  if (cameraFit) {
    half = maxSpan * 1.7320508f;
    fitCentre = m_camera->getPosition();
  }
  const float texel = 2.0f * half / float(m_shadowMapSize);

  // Snap the centre to whole texels so the texel grid stays pinned to the world while the sun does not move.
  const float centreR = glm::dot(fitCentre, right);
  const float centreU = glm::dot(fitCentre, lightUp);
  const float snapR = std::floor(centreR / texel) * texel;
  const float snapU = std::floor(centreU / texel) * texel;
  // Keep the depth centre on the box, the lateral centre on the snapped grid.
  const glm::vec3 boxCentre = fitCentre + right * (snapR - centreR) + lightUp * (snapU - centreU)
                              + sunDir * glm::dot(centre - fitCentre, sunDir);

  // A point offset by t from the centre sits at depth eyeDist + t; the eye a full box depth plus a margin away
  // keeps the near plane positive.
  const float boxDepth = std::max(1.0f, tMax - tMin);
  const float eyeDist = boxDepth * 2.0f + 200.0f;
  const float margin = boxDepth * 0.1f + 5.0f;
  const float nearPlane = std::max(0.05f, eyeDist + tMin - margin);
  const float farPlane = std::max(nearPlane + 1.0f, eyeDist + tMax + margin);

  const glm::vec3 eye = boxCentre - sunDir * eyeDist;
  const glm::mat4 lightView = glm::lookAt(eye, boxCentre, lightUp);
  const glm::mat4 lightProj = glm::ortho(-half, half, -half, half, nearPlane, farPlane);

  m_lightView = lightView;
  m_lightClip = corr * lightProj * lightView;
  m_shadowDepthScale = 1.0f / farPlane;
  m_shadowWorldTexel = texel;
  m_shadowNearPlane = nearPlane;
  m_shadowFarPlane = farPlane;

  // The shader reads these from the scene UBO.
  m_sceneUniforms.lightView = m_lightView;
  m_sceneUniforms.lightClip = m_lightClip;

  // Cascade 0 is this whole-map fit when there is a single cascade.
  m_cascades[0].lightView = m_lightView;
  m_cascades[0].lightClip = m_lightClip;
  m_cascades[0].params = glm::vec4(m_shadowDepthScale, 0.0f, 0.0f, 0.0f);
  m_sceneUniforms.cascadeView[0] = m_lightView;
  m_sceneUniforms.cascadeClip[0] = m_lightClip;
  m_sceneUniforms.cascadeUv[0] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
  m_sceneUniforms.cascadeSplits = glm::vec4(1e9f);
  m_sceneUniforms.cascadeTexel = glm::vec4(m_shadowWorldTexel);
  m_sceneUniforms.cascadeFar = glm::vec4(m_shadowFarPlane);
  m_sceneUniforms.cascadeInfo = glm::vec4(1.0f, 0.12f, m_cascadeDebug ? 1.0f : 0.0f, 0.0f);
  if (m_cascadeCount > 1)
    fitCascades(sceneMin, sceneMax, sunDir, right, lightUp, corr);
}

// Splits the shadow distance into slices (log / linear blend), one shadow tile per slice. Tile size depends only
// on the split distances, so snapped centres do not crawl. Depth spans the whole map along the sun, so casters
// outside the slice still cast into it.
void Engine3D::fitCascades(const glm::vec3 &sceneMin, const glm::vec3 &sceneMax, const glm::vec3 &sunDir,
                           const glm::vec3 &right, const glm::vec3 &lightUp, const glm::mat4 &corr) {
  const int count = std::clamp(m_cascadeCount, 2, 4);
  const float nearD = std::max(0.5f, m_camera->getNear());
  const float farD = std::max(nearD * 4.0f, std::min(shadowRenderDistanceWorld(), m_camera->getFar()));
  const float lambda = float(std::clamp(m_cascadeLambda, 0.0, 1.0));
  float splits[5];
  splits[0] = nearD;
  for (int i = 1; i <= count; ++i) {
    const float f = float(i) / float(count);
    const float logS = nearD * std::pow(farD / nearD, f);
    const float linS = nearD + (farD - nearD) * f;
    splits[i] = lambda * logS + (1.0f - lambda) * linS;
  }

  const glm::vec3 camPos = m_camera->getPosition();
  const int tile = std::max(64, m_shadowMapSize / 2);

  for (int i = 0; i < count; ++i) {
    // A sphere around the camera position up to the split distance: turning the view never moves a cascade
    // (the shader picks cascades by distance too).
    const float d1 = splits[i + 1];
    // The centre snaps in steps of several texels (no crawling, fewer re-renders). It can sit up to sqrt(2) steps
    // from the camera, so the radius carries 1.5 steps of margin (one step lets edge points flash lit).
    const float baseStep = 2.0f * d1 / float(tile) * 8.0f;
    const float radius = std::ceil((d1 + baseStep * 1.5f) * 16.0f) / 16.0f;
    const float texel = 2.0f * radius / float(tile);
    const float snapStep = std::max(texel, std::round(baseStep / texel) * texel);

    const glm::vec3 centre = camPos;
    const float cr = glm::dot(centre, right), cu = glm::dot(centre, lightUp);
    const float sr = std::floor(cr / snapStep) * snapStep, su = std::floor(cu / snapStep) * snapStep;
    const float cd = glm::dot(centre, sunDir);
    const float sd = std::floor(cd / snapStep) * snapStep;
    const glm::vec3 snapped = centre + right * (sr - cr) + lightUp * (su - cu) + sunDir * (sd - cd);

    float tMin = -radius, tMax = radius;
    for (int c = 0; c < 8; ++c) {
      const glm::vec3 corner((c & 1) ? sceneMax.x : sceneMin.x, (c & 2) ? sceneMax.y : sceneMin.y,
                             (c & 4) ? sceneMax.z : sceneMin.z);
      const float t = glm::dot(corner - snapped, sunDir);
      tMin = std::min(tMin, t);
      tMax = std::max(tMax, t);
    }
    const float depth = std::max(1.0f, tMax - tMin);
    const float eyeDist = depth + 50.0f;
    const float nearPlane = std::max(0.05f, eyeDist + tMin - 5.0f);
    const float farPlane = std::max(nearPlane + 1.0f, eyeDist + tMax + 5.0f);
    const glm::vec3 eye = snapped - sunDir * eyeDist;
    const glm::mat4 view = glm::lookAt(eye, snapped, lightUp);
    const glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, nearPlane, farPlane);
    const glm::mat4 clip = corr * proj * view;

    m_cascades[i].lightView = view;
    m_cascades[i].lightClip = clip;
    m_cascades[i].params = glm::vec4(1.0f / farPlane, 0.0f, 0.0f, 0.0f);
    m_sceneUniforms.cascadeView[i] = view;
    m_sceneUniforms.cascadeClip[i] = clip;
    // Tile (col,row) with a bottom-left viewport origin; texture v runs top-down.
    const int col = i % 2, row = i / 2;
    m_sceneUniforms.cascadeUv[i] = glm::vec4(0.5f * float(col), 1.0f - 0.5f * float(row + 1), 0.5f, 0.0f);
    m_sceneUniforms.cascadeSplits[i] = d1;
    m_sceneUniforms.cascadeTexel[i] = texel;
    m_sceneUniforms.cascadeFar[i] = farPlane;
  }
  m_sceneUniforms.cascadeInfo = glm::vec4(float(count), 0.12f, m_cascadeDebug ? 1.0f : 0.0f, 0.0f);
  // The single-map fields describe cascade 0 (caching, culling).
  m_shadowWorldTexel = m_sceneUniforms.cascadeTexel[0];
}

void Engine3D::ensureShadowTarget() {
  if (m_shadowRenderPass && m_builtShadowMapSize == m_shadowMapSize)
    return;

  QRhi *rhi = Shared::instance().rhi();
  const QSize map(m_shadowMapSize, m_shadowMapSize);

  // R32F: linear depth in half float errs by about the bias at a few hundred units of range.
  auto *shadowTex = rhi->newTexture(QRhiTexture::R32F, map, 1, {});
  if (!shadowTex->create()) {
    qWarning(engLog, "shadow map allocation failed (%d)", m_shadowMapSize);
    delete shadowTex;
    return;
  }

  // The depth attachment makes the nearest surface win (without it, draw order decides).
  auto *depthBuf = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, map, 1);
  if (!depthBuf->create()) {
    qWarning(engLog, "shadow depth buffer allocation failed (%d)", m_shadowMapSize);
    delete depthBuf;
    delete shadowTex;
    return;
  }

  QRhiTextureRenderTargetDescription desc;
  desc.setColorAttachments({QRhiColorAttachment(shadowTex)});
  desc.setDepthStencilBuffer(depthBuf);
  auto *rt = rhi->newTextureRenderTarget(desc);
  rt->setRenderPassDescriptor(rt->newCompatibleRenderPassDescriptor());
  if (!rt->create()) {
    qWarning(engLog, "shadow render target failed");
    delete rt;
    delete depthBuf;
    delete shadowTex;
    return;
  }

  // Material SRBs and the scene pipeline bound the old texture (generation bump below). Retired, not freed:
  // an earlier frame may still sample it.
  if (m_shadowTexture)
    Shared::instance().retireTexture(m_shadowTexture);
  if (m_shadowTarget)
    Shared::instance().retireRenderTarget(m_shadowTarget);
  if (m_shadowDepthBuffer)
    Shared::instance().retireRenderBuffer(m_shadowDepthBuffer);
  Shared::instance().retireRenderPass(m_shadowRenderPass);

  m_shadowTexture = shadowTex;
  m_shadowDepthBuffer = depthBuf;
  m_shadowTarget = rt;
  m_shadowRenderPass = rt->renderPassDescriptor();
  m_builtShadowMapSize = m_shadowMapSize;
  ++m_bufferGen;
  m_shadowMapKey = ~0ull;
}

void Engine3D::releaseSsaoTargets(bool retire) {
  // retire: deferred release (an earlier frame may still read these); only teardown frees at once.
  auto &shared = Shared::instance();

  auto kill = [&](auto *&obj) {
    if (!obj)
      return;
    if (retire)
      shared.retireTexture(obj);
    else
      delete obj;
    obj = nullptr;
  };

  if (retire) {
    shared.retireRenderTarget(m_aoTargetB);
    shared.retireRenderTarget(m_aoTargetA);
    shared.retireRenderTarget(m_gbufferTarget);
  } else {
    delete m_aoTargetB;
    delete m_aoTargetA;
    delete m_gbufferTarget;
  }
  m_aoTargetB = m_aoTargetA = m_gbufferTarget = nullptr;

  kill(m_aoTexB);
  kill(m_aoTexA);
  kill(m_gposTex);
  kill(m_gnormTex);

  if (retire)
    shared.retireRenderBuffer(m_gbufferDepth);
  else
    delete m_gbufferDepth;
  m_gbufferDepth = nullptr;

  if (retire)
    shared.retireRenderPass(m_gbufferRp);
  else
    delete m_gbufferRp;
  m_gbufferRp = nullptr;

  if (retire) {
    shared.retireShaderResourceBindings(m_ssaoSrb);
    shared.retireShaderResourceBindings(m_aoBlurSrb);
    shared.retireShaderResourceBindings(m_aoApplySrb);
    shared.retireShaderResourceBindings(m_gbufferSrb);
  } else {
    delete m_ssaoSrb;
    delete m_aoBlurSrb;
    delete m_aoApplySrb;
    delete m_gbufferSrb;
  }
  m_ssaoSrb = m_aoBlurSrb = m_aoApplySrb = m_gbufferSrb = nullptr;

  if (retire)
    shared.retireRenderPass(m_aoRp);
  else
    delete m_aoRp;
  m_aoRp = nullptr;

  m_ssaoSize = QSize();
  m_aoSize = QSize();
}

void Engine3D::ensureSsaoTargets(const QSize &pixelSize, QRhiResourceUpdateBatch *rub) {
  if (pixelSize.isEmpty())
    return;

  if (pixelSize == m_ssaoFailedSize && m_ssaoRetry > 0) {
    --m_ssaoRetry;
    return;
  }

  const int div = m_ssaoHalfRes ? 2 : 1;
  const QSize aoSize = QSize(std::max(1, pixelSize.width() / div), std::max(1, pixelSize.height() / div));
  if (m_ssaoSize == pixelSize && m_aoSize == aoSize && m_gbufferTarget && m_aoTargetB && m_quadVb)
    return;
  // New AO textures: rebuilds every SRB/pipeline that bound the old ones.
  ++m_bufferGen;

  releaseSsaoTargets(true);

  QRhi *rhi = Shared::instance().rhi();
  if (!rhi)
    return;

  // Fullscreen quad shared by every pass in the chain.
  static const float kQuad[] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
  static const quint32 kQuadIndex[] = {0, 1, 2, 0, 2, 3};
  if (!m_quadVb) {
    m_quadVb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, sizeof(kQuad));
    if (!m_quadVb->create()) {
      qWarning(engLog, "ssao: quad vertex buffer failed");
      delete m_quadVb;
      m_quadVb = nullptr;
      return;
    }
    rub->updateDynamicBuffer(m_quadVb, 0, sizeof(kQuad), kQuad);
  }
  if (!m_quadIb) {
    m_quadIb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::IndexBuffer, sizeof(kQuadIndex));
    if (!m_quadIb->create()) {
      qWarning(engLog, "ssao: quad index buffer failed");
      delete m_quadIb;
      m_quadIb = nullptr;
      return;
    }
    rub->updateDynamicBuffer(m_quadIb, 0, sizeof(kQuadIndex), kQuadIndex);
  }

  if (!rhi->isTextureFormatSupported(QRhiTexture::RGBA32F)
      || !rhi->isTextureFormatSupported(QRhiTexture::RGBA16F)) {
    qWarning(engLog, "ssao: g-buffer formats unsupported");
    return;
  }

  // Full float: the occlusion test compares surfaces a fraction of a unit apart.
  m_gposTex = rhi->newTexture(QRhiTexture::RGBA32F, pixelSize, 1, {});
  m_gnormTex = rhi->newTexture(QRhiTexture::RGBA16F, pixelSize, 1, {});
  m_gbufferDepth = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, 1);
  if (!m_gposTex->create() || !m_gnormTex->create() || !m_gbufferDepth->create()) {
    qWarning(engLog, "ssao: g-buffer allocation failed (%dx%d)", pixelSize.width(),
             pixelSize.height());
    releaseSsaoTargets(true);
    m_ssaoFailedSize = pixelSize;
    m_ssaoRetry = kAllocRetryFrames;
    return;
  }

  QRhiTextureRenderTargetDescription gbDesc;
  gbDesc.setColorAttachments({QRhiColorAttachment(m_gposTex), QRhiColorAttachment(m_gnormTex)});
  gbDesc.setDepthStencilBuffer(m_gbufferDepth);
  m_gbufferTarget = rhi->newTextureRenderTarget(gbDesc);
  m_gbufferTarget->setRenderPassDescriptor(m_gbufferTarget->newCompatibleRenderPassDescriptor());
  if (!m_gbufferTarget->create()) {
    qWarning(engLog, "ssao: g-buffer render target failed");
    releaseSsaoTargets(true);
    return;
  }
  m_gbufferRp = m_gbufferTarget->renderPassDescriptor();

  // Both AO textures share one render pass.
  auto makeAo = [&](QRhiTexture *&tex, QRhiTextureRenderTarget *&target) {
    tex = rhi->newTexture(QRhiTexture::R16F, aoSize, 1, {});
    if (!tex->create()) {
      qWarning(engLog, "ssao: AO texture failed (%dx%d)", aoSize.width(), aoSize.height());
      delete tex;
      tex = nullptr;
      return false;
    }
    QRhiTextureRenderTargetDescription d;
    d.setColorAttachments({QRhiColorAttachment(tex)});
    target = rhi->newTextureRenderTarget(d);
    if (!m_aoRp)
      m_aoRp = target->newCompatibleRenderPassDescriptor();
    target->setRenderPassDescriptor(m_aoRp);
    if (!target->create()) {
      qWarning(engLog, "ssao: AO render target failed");
      delete target;
      delete tex;
      target = nullptr;
      tex = nullptr;
      return false;
    }
    return true;
  };

  if (!makeAo(m_aoTexA, m_aoTargetA) || !makeAo(m_aoTexB, m_aoTargetB)) {
    releaseSsaoTargets(true);
    return;
  }

  m_ssaoSize = pixelSize;
  m_aoSize = aoSize;
  m_ssaoFailedSize = QSize();
  m_ssaoRetry = 0;
}

void Engine3D::ensureOverlayTargets(const QSize &pixelSize) {
  QRhi *rhi = Shared::instance().rhi();
  if (!rhi)
    return;
  if (m_overlaySize == pixelSize && m_selDepthTarget)
    return;

  if (pixelSize == m_overlayFailedSize && m_overlayRetry > 0) {
    --m_overlayRetry;
    return;
  }

  auto &shared = Shared::instance();
  shared.retireRenderTarget(m_selDepthTarget);
  shared.retireRenderPass(m_selDepthRp);
  shared.retireRenderBuffer(m_selDepthDs);
  shared.retireTexture(m_selDepthTex);
  m_selDepthTarget = nullptr;
  m_selDepthRp = nullptr;
  m_selDepthDs = nullptr;
  m_selDepthTex = nullptr;
  m_overlaySize = QSize();

  if (pixelSize.isEmpty())
    return;

  // Linear depth scratch. R32F: half float over a 15000-unit far plane is too coarse for the count pass.
  // The depth buffer only does nearest-wins; the count pass reads the colour attachment.
  m_selDepthTex = rhi->newTexture(QRhiTexture::R32F, pixelSize, 1, {});
  m_selDepthDs = rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, 1);
  if (!m_selDepthTex->create() || !m_selDepthDs->create()) {
    qWarning(engLog, "overlay: scratch depth allocation failed (%dx%d)", pixelSize.width(),
             pixelSize.height());
    delete m_selDepthTex;
    delete m_selDepthDs;
    m_selDepthTex = nullptr;
    m_selDepthDs = nullptr;
    m_overlayFailedSize = pixelSize;
    m_overlayRetry = kAllocRetryFrames;
    return;
  }

  QRhiTextureRenderTargetDescription selDesc;
  selDesc.setColorAttachments({ QRhiColorAttachment(m_selDepthTex) });
  selDesc.setDepthStencilBuffer(m_selDepthDs);
  m_selDepthTarget = rhi->newTextureRenderTarget(selDesc);
  m_selDepthTarget->setRenderPassDescriptor(m_selDepthTarget->newCompatibleRenderPassDescriptor());
  if (!m_selDepthTarget->create()) {
    qWarning(engLog, "overlay: scratch depth render target failed");
    delete m_selDepthTarget;
    m_selDepthTarget = nullptr;
    m_overlayFailedSize = pixelSize;
    m_overlayRetry = kAllocRetryFrames;
    return;
  }
  m_selDepthRp = m_selDepthTarget->renderPassDescriptor();
  m_overlaySize = pixelSize;
  m_overlayFailedSize = QSize();
  m_overlayRetry = 0;
}

int Engine3D::drawWholeObjects(QRhiCommandBuffer *cb, QRhiShaderResourceBindings *srb,
                              const std::function<bool(quint32)> &include) {
  int draws = 0;
  if (!m_scene || !srb)
    return draws;

  const auto &objects = m_scene->gameObjects;
  for (qsizetype i = 0; i < objects.size(); ++i) {
    const quint32 idx = static_cast<quint32>(i);
    if (!include(idx))
      continue;
    const auto mesh = objects[i]->getMesh();
    if (!mesh || objects[i]->hidden() || isToolBrush(*objects[i]))
      continue;
    const MeshGpuRecord *rec = m_gpu.record(objects[i]->stableId());
    if (!rec || rec->indexCount == 0)
      continue;

    const QRhiCommandBuffer::DynamicOffset offset(1, objectSlotOf(objects[i]->stableId()) * m_objectStride);
    cb->setShaderResources(srb, 1, &offset);
    // Pool indices are mesh-local: vertexOffset is the base.
    cb->drawIndexed(rec->indexCount, 1, rec->indexOffset, static_cast<qint32>(rec->vertexOffset));
    ++draws;
  }
  return draws;
}

// One pass over the level into the scene-depth target, nearest wins; does not scale with the selection.
int Engine3D::renderSceneDepth(QRhiCommandBuffer *cb, const QSize &pixelSize,
                               QRhiResourceUpdateBatch *rub) {
  if (!m_selDepthTarget || !m_selDepthPipeline)
    return 0;

  cb->beginPass(m_selDepthTarget, QColor::fromRgbF(0.0f, 0.0f, 0.0f, 0.0f), { 1.0f, 0 }, rub);
  cb->setGraphicsPipeline(m_selDepthPipeline.get());
  cb->setViewport(QRhiViewport(0, 0, pixelSize.width(), pixelSize.height()));

  if (m_gpu.vertexBuffer() && m_gpu.indexBuffer()) {

    const QRhiCommandBuffer::VertexInput vertexInput(m_gpu.vertexBuffer(), 0);

    cb->setVertexInput(0, 1, &vertexInput, m_gpu.indexBuffer(), 0, QRhiCommandBuffer::IndexUInt32);

  }
  const int draws = drawWholeObjects(cb, m_selDepthSrb, [this](const quint32 idx) {
    return withinRenderDistance(*m_scene->gameObjects[idx]);
  });
  cb->endPass();
  return draws;
}

void Engine3D::setupPipelines(QRhiRenderPassDescriptor *rp, int sampleCount) {
  QRhi *rhi = Shared::instance().rhi();
  auto &shared = Shared::instance();

  // Pipelines reference their layout SRB: a grown object / face buffer or recreated overlay textures rebuild them.
  if (m_builtRenderPass == rp && m_builtSampleCount == sampleCount && m_builtBufferGen == m_bufferGen
      && m_builtOverlaySize == m_overlaySize && m_builtSsaoSize == m_ssaoSize && m_scenePipeline
      && m_shadowPipeline && m_wirePipeline && m_selDepthPipeline
      && m_cutWirePipeline && m_cutFillPipeline && m_collisionFillPipeline && m_collisionEdgePipeline
      && (!entityPipelinesWanted()
          || (m_entityTriDepthPipeline && m_entityTriOverlayPipeline && m_entityLinePipeline)))
    return;

  ++m_perf.pipelineRebuilds;

  m_scenePipeline.reset();
  m_shadowPipeline.reset();
  m_wirePipeline.reset();
  m_selDepthPipeline.reset();
  m_gbufferPipeline.reset();
  m_ssaoPipeline.reset();
  m_aoBlurPipeline.reset();
  m_aoApplyPipeline.reset();
  m_skyPipeline.reset();
  m_entityTriDepthPipeline.reset();
  m_entityTriOverlayPipeline.reset();
  m_entityLinePipeline.reset();
  // Deferred release: frames in flight may still read these.
  const auto dropSrb = [&shared](QRhiShaderResourceBindings *&srb) {
    shared.retireShaderResourceBindings(srb);
    srb = nullptr;
  };
  dropSrb(m_entitySrb);
  dropSrb(m_sceneTemplateSrb);
  dropSrb(m_shadowSrb);
  dropSrb(m_wireSrb);
  dropSrb(m_selDepthSrb);
  dropSrb(m_gbufferSrb);
  dropSrb(m_ssaoSrb);
  dropSrb(m_aoBlurSrb);
  dropSrb(m_aoApplySrb);
  // The lazily built overlays must be rebuilt for a new render pass too: the driver silently drops a pipeline
  // bound to a pass it was not built for.
  delete m_cutWirePipeline;
  m_cutWirePipeline = nullptr;
  delete m_cutFillPipeline;
  m_cutFillPipeline = nullptr;
  releaseCollisionPipelines();
  releaseIOLinePipelines();
  dropSrb(m_cutWireSrb);
  dropSrb(m_layerSrbWall);
  dropSrb(m_layerSrbSpace);

  // Built here, not with the textures: the g-buffer SRB binds the object UBO, which is replaced when outgrown.
  if (m_gposTex && m_gnormTex) {
    m_gbufferSrb = makeSrb(rhi, m_sceneUbo, m_objectUbo, nullptr, nullptr, nullptr);
    m_ssaoSrb = makeSsaoSrb(rhi, m_ssaoUbo, m_gposTex, m_gnormTex, nullptr);
    m_aoBlurSrb = makeSsaoSrb(rhi, m_ssaoUbo, m_gposTex, nullptr, m_aoTexA);
    m_aoApplySrb = makeSsaoSrb(rhi, nullptr, nullptr, nullptr, m_aoTexB);
  }

  if (!m_sceneTemplateSrb)
    m_sceneTemplateSrb = makeSrb(rhi, m_sceneUbo, m_objectUbo, m_drawUbo, nullptr, nullptr, nullptr,
                                 m_shadowTexture, true, m_lightAtlasTex, m_lutTex);
  if (!m_sceneTemplateSrb) {
    qWarning(engLog, "scene layout SRB failed");
    return;
  }
  if (m_cascadeUbo) {
    m_shadowSrb = rhi->newShaderResourceBindings();
    m_shadowSrb->setBindings({QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                                  0, QRhiShaderResourceBinding::VertexStage, m_cascadeUbo, sizeof(CascadeBlock)),
                              QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                                  1, QRhiShaderResourceBinding::VertexStage, m_objectUbo, sizeof(ObjectBlock))});
    if (!m_shadowSrb->create()) {
      delete m_shadowSrb;
      m_shadowSrb = nullptr;
    }
  }
  // Shared by all selected wires; binding 9 is the scene depth texture.
  m_wireSrb = makeSrb(rhi, m_sceneUbo, m_objectUbo, nullptr, m_overlayUbo, nullptr,
                      m_selDepthTex ? m_selDepthTex : Shared::instance().fallbackWhite());
  // Layer bands: wire pipeline, identity object block, one overlay block (colour) per band kind.
  if (m_layerObjUbo && m_layerOverlayWallUbo && m_layerOverlaySpaceUbo) {
    auto &sh = Shared::instance();
    m_layerSrbWall = makeSrb(rhi, m_sceneUbo, m_layerObjUbo, nullptr, m_layerOverlayWallUbo,
                             nullptr, sh.fallbackWhite());
    m_layerSrbSpace = makeSrb(rhi, m_sceneUbo, m_layerObjUbo, nullptr, m_layerOverlaySpaceUbo,
                              nullptr, sh.fallbackWhite());
    if (!m_layerSrbWall || !m_layerSrbSpace)
      qWarning(engLog, "layer overlay SRB failed");
  }
  m_selDepthSrb = makeSrb(rhi, m_sceneUbo, m_objectUbo, nullptr, nullptr, nullptr);
  if (entityPipelinesWanted())
    m_entitySrb = makeEntitySrb(rhi, m_sceneUbo, m_entityUbo);
  if (!m_shadowSrb)
    qWarning(engLog, "shadow SRB failed");
  if (!m_wireSrb)
    qWarning(engLog, "wire SRB failed");

  m_scenePipeline.reset(rhi->newGraphicsPipeline());
  m_scenePipeline->setShaderStages({
      {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/scene.vert.qsb"))},
      {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/scene.frag.qsb"))},
  });
  {
    QRhiVertexInputLayout layout;
    layout.setBindings({{sizeof(eng::Vertex)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::Vertex, position))},
        {0, 1, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::Vertex, normal))},
        {0, 2, QRhiVertexInputAttribute::Float2, static_cast<quint32>(offsetof(eng::Vertex, texCoord))},
        {0, 3, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::Vertex, tangent))},
        {0, 4, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::Vertex, bitangent))},
        {0, 5, QRhiVertexInputAttribute::Float, static_cast<quint32>(offsetof(eng::Vertex, faceIndex))},
    });
    m_scenePipeline->setVertexInputLayout(layout);
  }
  m_scenePipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  m_scenePipeline->setCullMode(QRhiGraphicsPipeline::Back);
  m_scenePipeline->setFrontFace(QRhiGraphicsPipeline::CW);
  m_scenePipeline->setDepthTest(true);
  m_scenePipeline->setDepthWrite(true);
  m_scenePipeline->setDepthOp(QRhiGraphicsPipeline::Less);
  m_scenePipeline->setSampleCount(sampleCount);
  m_scenePipeline->setRenderPassDescriptor(rp);
  m_scenePipeline->setShaderResourceBindings(m_sceneTemplateSrb);
  if (!m_scenePipeline->create())
    qWarning(engLog, "scene pipeline failed");

  if (m_shadowRenderPass) {
    m_shadowPipeline.reset(rhi->newGraphicsPipeline());
    m_shadowPipeline->setShaderStages({
        {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/shadow.vert.qsb"))},
        {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/shadow.frag.qsb"))},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{sizeof(eng::Vertex)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::Vertex, position))},
    });
    m_shadowPipeline->setVertexInputLayout(layout);
    m_shadowPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    m_shadowPipeline->setCullMode(QRhiGraphicsPipeline::Back);
    m_shadowPipeline->setFrontFace(QRhiGraphicsPipeline::CW);
    m_shadowPipeline->setDepthTest(true);
    m_shadowPipeline->setDepthWrite(true);
    m_shadowPipeline->setDepthOp(QRhiGraphicsPipeline::Less);
    m_shadowPipeline->setSampleCount(1);
    m_shadowPipeline->setRenderPassDescriptor(m_shadowRenderPass);
    if (m_shadowSrb)
      m_shadowPipeline->setShaderResourceBindings(m_shadowSrb);
    if (!m_shadowPipeline->create())
      qWarning(engLog, "shadow pipeline failed");
  }

  m_wirePipeline.reset(rhi->newGraphicsPipeline());
  m_wirePipeline->setShaderStages({
      {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/wire.vert.qsb"))},
      {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/wire.frag.qsb"))},
  });
  {
    QRhiVertexInputLayout layout;
    layout.setBindings({{sizeof(eng::WireVertex)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::WireVertex, position))},
        {0, 1, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::WireVertex, barycentric))},
        {0, 2, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::WireVertex, normal))},
    });
    m_wirePipeline->setVertexInputLayout(layout);
  }
  m_wirePipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  m_wirePipeline->setCullMode(QRhiGraphicsPipeline::None);
  // No depth test: seen through occluders; the occluder count texture decides how faint it gets.
  m_wirePipeline->setDepthTest(false);
  m_wirePipeline->setDepthWrite(false);
  {
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    m_wirePipeline->setTargetBlends({blend});
  }
  m_wirePipeline->setSampleCount(sampleCount);
  m_wirePipeline->setRenderPassDescriptor(rp);
  if (m_wireSrb)
    m_wirePipeline->setShaderResourceBindings(m_wireSrb);
  if (!m_wirePipeline->create())
    qWarning(engLog, "wire pipeline failed");

  // marquee (selection box): solid yellow, camera-facing thick edges
  if (!m_marqueeVb) {
    m_marqueeVb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 1 << 14);
    if (!m_marqueeVb->create())
      qWarning(engLog, "marquee vertex buffer failed");
  }

  if (!m_marqueeSrb) {
    m_marqueeSrb = rhi->newShaderResourceBindings();
    m_marqueeSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            m_sceneUbo),
    });
    if (!m_marqueeSrb->create())
      qWarning(engLog, "marquee srb failed");
  }

  if (!m_marqueePipeline) {
    m_marqueePipeline = rhi->newGraphicsPipeline();
    m_marqueePipeline->setShaderStages({
        {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/marquee3d.vert.qsb"))},
        {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/marquee3d.frag.qsb"))},
    });
    {
      QRhiVertexInputLayout layout;
      layout.setBindings({{3 * sizeof(float)}});
      layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}});
      m_marqueePipeline->setVertexInputLayout(layout);
    }
    m_marqueePipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    m_marqueePipeline->setCullMode(QRhiGraphicsPipeline::None);
    m_marqueePipeline->setDepthTest(false);
    m_marqueePipeline->setDepthWrite(false);
    {
      QRhiGraphicsPipeline::TargetBlend blend;
      blend.enable = true;
      blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
      blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
      blend.srcAlpha = QRhiGraphicsPipeline::One;
      blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
      m_marqueePipeline->setTargetBlends({blend});
    }
    m_marqueePipeline->setSampleCount(sampleCount);
    m_marqueePipeline->setRenderPassDescriptor(rp);
    m_marqueePipeline->setShaderResourceBindings(m_marqueeSrb);
    if (!m_marqueePipeline->create()) {
      qWarning(engLog, "marquee pipeline failed");
      delete m_marqueePipeline;
      m_marqueePipeline = nullptr;
    }
  }

  // The cut-out preview uses the marquee's vertex stage in another colour.
  if (!m_cutWireVb) {
    m_cutWireVb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 1 << 17);
    if (!m_cutWireVb->create())
      qWarning(engLog, "cut overlay vertex buffer failed");
  }

  if (!m_cutWireSrb && m_sceneUbo) {
    m_cutWireSrb = rhi->newShaderResourceBindings();
    m_cutWireSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            m_sceneUbo),
    });
    if (!m_cutWireSrb->create())
      qWarning(engLog, "cut overlay srb failed");
  }

  if (!m_cutWirePipeline && m_cutWireVb && m_cutWireSrb) {
    m_cutWirePipeline = rhi->newGraphicsPipeline();
    m_cutWirePipeline->setShaderStages({
        {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/marquee3d.vert.qsb"))},
        {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/cutwire.frag.qsb"))},
    });
    {
      QRhiVertexInputLayout layout;
      layout.setBindings({{3 * sizeof(float)}});
      layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}});
      m_cutWirePipeline->setVertexInputLayout(layout);
    }
    m_cutWirePipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    m_cutWirePipeline->setCullMode(QRhiGraphicsPipeline::None);
    m_cutWirePipeline->setDepthTest(false);
    m_cutWirePipeline->setDepthWrite(false);
    {
      QRhiGraphicsPipeline::TargetBlend blend;
      blend.enable = true;
      blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
      blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
      blend.srcAlpha = QRhiGraphicsPipeline::One;
      blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
      m_cutWirePipeline->setTargetBlends({blend});
    }
    m_cutWirePipeline->setSampleCount(sampleCount);
    m_cutWirePipeline->setRenderPassDescriptor(rp);
    m_cutWirePipeline->setShaderResourceBindings(m_cutWireSrb);
    if (!m_cutWirePipeline->create()) {
      qWarning(engLog, "cut overlay pipeline failed");
      delete m_cutWirePipeline;
      m_cutWirePipeline = nullptr;
    }
  }
  if (!m_cutFillPipeline && m_cutWireVb && m_cutWireSrb) {
    m_cutFillPipeline = rhi->newGraphicsPipeline();
    m_cutFillPipeline->setShaderStages({
        {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/marquee3d.vert.qsb"))},
        {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/cutfill.frag.qsb"))},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{3 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, 0}});
    m_cutFillPipeline->setVertexInputLayout(layout);
    m_cutFillPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    m_cutFillPipeline->setCullMode(QRhiGraphicsPipeline::None);
    m_cutFillPipeline->setDepthTest(false);
    m_cutFillPipeline->setDepthWrite(false);
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    m_cutFillPipeline->setTargetBlends({blend});
    m_cutFillPipeline->setSampleCount(sampleCount);
    m_cutFillPipeline->setRenderPassDescriptor(rp);
    m_cutFillPipeline->setShaderResourceBindings(m_cutWireSrb);
    if (!m_cutFillPipeline->create()) {
      qWarning(engLog, "cut fill pipeline failed");
      delete m_cutFillPipeline;
      m_cutFillPipeline = nullptr;
    }
  }
  setupCollisionPipelines(rp, sampleCount);
  setupIOLinePipelines(rp, sampleCount);
  // The layer preview draws through the selection wire pipeline.
  if (!m_fillWireVb) {
    m_fillWireVb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 1 << 15);
    if (!m_fillWireVb->create())
      qWarning(engLog, "layer overlay vertex buffer failed");
  }
  if (!m_fillWireIb) {
    m_fillWireIb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::IndexBuffer, 1 << 14);
    if (!m_fillWireIb->create())
      qWarning(engLog, "layer overlay index buffer failed");
  }

  if (m_selDepthRp && m_selDepthSrb) {
    m_selDepthPipeline.reset(rhi->newGraphicsPipeline());
    m_selDepthPipeline->setShaderStages({
        {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/seldepth.vert.qsb"))},
        {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/seldepth.frag.qsb"))},
    });
    QRhiVertexInputLayout selLayout;
    selLayout.setBindings({{sizeof(eng::Vertex)}});
    selLayout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::Vertex, position))},
    });
    m_selDepthPipeline->setVertexInputLayout(selLayout);
    m_selDepthPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    m_selDepthPipeline->setCullMode(QRhiGraphicsPipeline::Back);
    m_selDepthPipeline->setFrontFace(QRhiGraphicsPipeline::CW);
    // Nearest wins among the object's own faces; the count pass reads the colour attachment.
    m_selDepthPipeline->setDepthTest(true);
    m_selDepthPipeline->setDepthWrite(true);
    m_selDepthPipeline->setDepthOp(QRhiGraphicsPipeline::Less);
    m_selDepthPipeline->setSampleCount(1);
    m_selDepthPipeline->setRenderPassDescriptor(m_selDepthRp);
    m_selDepthPipeline->setShaderResourceBindings(m_selDepthSrb);
    if (!m_selDepthPipeline->create())
      qWarning(engLog, "selected-depth pipeline failed");
  }

  if (m_gbufferRp && m_gbufferSrb) {
    m_gbufferPipeline.reset(rhi->newGraphicsPipeline());
    m_gbufferPipeline->setShaderStages({
        {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/gbuffer.vert.qsb"))},
        {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/gbuffer.frag.qsb"))},
    });
    QRhiVertexInputLayout gbLayout;
    gbLayout.setBindings({{sizeof(eng::Vertex)}});
    gbLayout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::Vertex, position))},
        {0, 1, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::Vertex, normal))},
    });
    m_gbufferPipeline->setVertexInputLayout(gbLayout);
    m_gbufferPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    m_gbufferPipeline->setCullMode(QRhiGraphicsPipeline::Back);
    m_gbufferPipeline->setFrontFace(QRhiGraphicsPipeline::CW);
    m_gbufferPipeline->setDepthTest(true);
    m_gbufferPipeline->setDepthWrite(true);
    m_gbufferPipeline->setDepthOp(QRhiGraphicsPipeline::Less);
    m_gbufferPipeline->setSampleCount(1);
    // One blend state per colour attachment (position, normal), or the normal target is never written.
    m_gbufferPipeline->setTargetBlends({QRhiGraphicsPipeline::TargetBlend(), QRhiGraphicsPipeline::TargetBlend()});
    m_gbufferPipeline->setRenderPassDescriptor(m_gbufferRp);
    m_gbufferPipeline->setShaderResourceBindings(m_gbufferSrb);
    if (!m_gbufferPipeline->create())
      qWarning(engLog, "g-buffer pipeline failed");
  }

  auto makeFsPipeline = [&](const QString &fragPath, QRhiRenderPassDescriptor *passRp,
                           QRhiShaderResourceBindings *srb, int samples, bool multiply) {
    if (!passRp || !srb)
      return std::unique_ptr<QRhiGraphicsPipeline>();

    std::unique_ptr<QRhiGraphicsPipeline> p(rhi->newGraphicsPipeline());
    p->setShaderStages({
        {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/fsquad.vert.qsb"))},
        {QRhiShaderStage::Fragment, shared.shader(fragPath)},
    });
    QRhiVertexInputLayout fsLayout;
    fsLayout.setBindings({{2 * sizeof(float)}});
    fsLayout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    p->setVertexInputLayout(fsLayout);
    p->setTopology(QRhiGraphicsPipeline::Triangles);
    p->setCullMode(QRhiGraphicsPipeline::None);
    p->setDepthTest(false);
    p->setDepthWrite(false);
    if (multiply) {
      // dst * src: the pass writes the occlusion factor, the blend darkens.
      QRhiGraphicsPipeline::TargetBlend b;
      b.enable = true;
      b.srcColor = QRhiGraphicsPipeline::DstColor;
      b.dstColor = QRhiGraphicsPipeline::Zero;
      b.srcAlpha = QRhiGraphicsPipeline::Zero;
      b.dstAlpha = QRhiGraphicsPipeline::One;
      p->setTargetBlends({b});
    }
    p->setSampleCount(samples);
    p->setRenderPassDescriptor(passRp);
    p->setShaderResourceBindings(srb);
    if (!p->create()) {
      qWarning(engLog, "ssao pipeline failed for %s", qPrintable(fragPath));
      return std::unique_ptr<QRhiGraphicsPipeline>();
    }
    return p;
  };

  m_ssaoPipeline = makeFsPipeline(QStringLiteral(":/shaders/ssao.frag.qsb"), m_aoRp, m_ssaoSrb, 1,
                                  false);
  m_aoBlurPipeline = makeFsPipeline(QStringLiteral(":/shaders/ssao_blur.frag.qsb"), m_aoRp,
                                   m_aoBlurSrb, 1, false);
  m_aoApplyPipeline = makeFsPipeline(QStringLiteral(":/shaders/ssao_apply.frag.qsb"), rp,
                                    m_aoApplySrb, sampleCount, true);

  // Sky: the scene UBO, the LUT and the skybox panorama.
  if (m_skySrb)
    shared.retireShaderResourceBindings(m_skySrb);
  m_skySrb = nullptr;
  if (m_sceneUbo) {
    m_skySrb = rhi->newShaderResourceBindings();
    QVector<QRhiShaderResourceBinding> skyBindings{
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, m_sceneUbo)};
    if (m_lutTex)
      skyBindings << QRhiShaderResourceBinding::sampledTexture(15, QRhiShaderResourceBinding::FragmentStage,
                                                               m_lutTex, shared.clampLinearSampler());
    // Skybox panorama (vk_sky.cpp), black while there is none (u_lightCount.w says which).
    skyBindings << QRhiShaderResourceBinding::sampledTexture(
        13, QRhiShaderResourceBinding::FragmentStage, skyboxLoaded() ? m_skyTex : shared.fallbackBlack(),
        shared.panoramaSampler());
    m_skySrb->setBindings(skyBindings.cbegin(), skyBindings.cend());
    if (!m_skySrb->create()) {
      delete m_skySrb;
      m_skySrb = nullptr;
    }
  }
  m_skyPipeline = makeFsPipeline(QStringLiteral(":/shaders/sky.frag.qsb"), rp, m_skySrb, sampleCount, false);

  // entity markers: one vertex pool, three pipelines, flat colour. Two triangle pipelines: the marker box
  // is depth-tested like level geometry, the gizmo never is (Godot's depth_test_disabled handles).
  if (entityPipelinesWanted()) {
    auto makeEntityPipeline = [&](const QRhiGraphicsPipeline::Topology topology, const bool depthTest,
                                 const bool depthWrite) {
      std::unique_ptr<QRhiGraphicsPipeline> p(rhi->newGraphicsPipeline());
      p->setShaderStages({
          {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/entity.vert.qsb"))},
          {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/entity.frag.qsb"))},
      });
      QRhiVertexInputLayout layout;
      layout.setBindings({{sizeof(glm::vec3)}});
      layout.setAttributes({
          {0, 0, QRhiVertexInputAttribute::Float3, 0},
      });
      p->setVertexInputLayout(layout);
      p->setTopology(topology);
      p->setCullMode(QRhiGraphicsPipeline::None);
      p->setDepthTest(depthTest);
      p->setDepthWrite(depthWrite);
      if (depthTest)
        p->setDepthOp(QRhiGraphicsPipeline::Less);
      {
        QRhiGraphicsPipeline::TargetBlend blend;
        blend.enable = true;
        blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
        blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        p->setTargetBlends({blend});
      }
      p->setSampleCount(sampleCount);
      p->setRenderPassDescriptor(rp);
      p->setShaderResourceBindings(m_entitySrb);
      if (!p->create()) {
        qWarning(engLog, "entity pipeline failed (topology %d, depth %d)", int(topology),
                 int(depthTest));
        return std::unique_ptr<QRhiGraphicsPipeline>();
      }
      return p;
    };

    m_entityTriDepthPipeline = makeEntityPipeline(QRhiGraphicsPipeline::Triangles, true, true);
    m_entityTriOverlayPipeline = makeEntityPipeline(QRhiGraphicsPipeline::Triangles, false, false);
    m_entityLinePipeline = makeEntityPipeline(QRhiGraphicsPipeline::Lines, false, false);
    buildBillboardPipeline(rp, sampleCount);
  }

  // Entity light shadow atlas: tile matrices by dynamic offset.
  if (m_lightAtlasSrb)
    shared.retireShaderResourceBindings(m_lightAtlasSrb);
  m_lightAtlasSrb = nullptr;
  m_lightAtlasPipeline.reset();
  if (m_lightAtlasRp && m_objectUbo && m_lightFaceUbo) {
    m_lightAtlasSrb = rhi->newShaderResourceBindings();
    m_lightAtlasSrb->setBindings({QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                                      1, QRhiShaderResourceBinding::VertexStage, m_objectUbo, sizeof(ObjectBlock)),
                                  QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                                      15, QRhiShaderResourceBinding::VertexStage, m_lightFaceUbo,
                                      sizeof(LightShadowFace))});
    if (m_lightAtlasSrb->create()) {
      m_lightAtlasPipeline.reset(rhi->newGraphicsPipeline());
      m_lightAtlasPipeline->setShaderStages({
          {QRhiShaderStage::Vertex, shared.shader(QStringLiteral(":/shaders/shadow_point.vert.qsb"))},
          {QRhiShaderStage::Fragment, shared.shader(QStringLiteral(":/shaders/shadow_point.frag.qsb"))},
      });
      QRhiVertexInputLayout layout;
      layout.setBindings({{sizeof(eng::Vertex)}});
      layout.setAttributes(
          {{0, 0, QRhiVertexInputAttribute::Float3, static_cast<quint32>(offsetof(eng::Vertex, position))}});
      m_lightAtlasPipeline->setVertexInputLayout(layout);
      m_lightAtlasPipeline->setCullMode(QRhiGraphicsPipeline::Back);
      m_lightAtlasPipeline->setFrontFace(QRhiGraphicsPipeline::CW);
      m_lightAtlasPipeline->setDepthTest(true);
      m_lightAtlasPipeline->setDepthWrite(true);
      m_lightAtlasPipeline->setSampleCount(1);
      m_lightAtlasPipeline->setRenderPassDescriptor(m_lightAtlasRp);
      m_lightAtlasPipeline->setShaderResourceBindings(m_lightAtlasSrb);
      if (!m_lightAtlasPipeline->create()) {
        qWarning(engLog, "light shadow atlas pipeline failed");
        m_lightAtlasPipeline.reset();
      }
      m_lightAtlasKey = ~0ull;
    } else {
      delete m_lightAtlasSrb;
      m_lightAtlasSrb = nullptr;
    }
  }

  m_builtRenderPass = rp;
  m_builtSampleCount = sampleCount;
  m_builtBufferGen = m_bufferGen;
  m_builtOverlaySize = m_overlaySize;
  m_builtSsaoSize = m_ssaoSize;
}

void Engine3D::sortByMaterial() {
  const int n = m_visible.size();
  if (n < 2)
    return;

  m_visBuckets.clear();
  m_visBucketOf.resize(size_t(n));

  for (int i = 0; i < n; ++i) {
    eng::Material *const mat = m_visible[i].material;
    int slot = -1;
    for (size_t b = 0; b < m_visBuckets.size(); ++b) {
      if (m_visBuckets[b].mat == mat) {
        slot = static_cast<int>(b);
        break;
      }
    }
    if (slot < 0) {
      m_visBuckets.push_back({mat, 0u, 0u});
      slot = static_cast<int>(m_visBuckets.size()) - 1;
    }
    ++m_visBuckets[size_t(slot)].count;
    m_visBucketOf[size_t(i)] = static_cast<quint32>(slot);
  }

  quint32 running = 0;
  for (MatBucket &b: m_visBuckets) {
    b.offset = running;
    running += b.count;
  }
  m_visCursor.assign(m_visBuckets.size(), 0u);

  m_visibleTmp = m_visible;
  for (int i = 0; i < n; ++i) {
    const quint32 slot = m_visBucketOf[size_t(i)];
    const MatBucket &b = m_visBuckets[size_t(slot)];
    m_visible[b.offset + m_visCursor[size_t(slot)]++] = m_visibleTmp[size_t(i)];
  }
}

void Engine3D::collectVisibleFaces() {
  // Depends on camera and geometry only (not the selection): a static camera skips the sweep and sort.
  const float renderDistance = renderDistanceWorld();
  const quint64 visKey = mixSig(
      mixSig(mixSig(m_sigGeometry, quint64(m_gpu.faceEpoch())),
             hashFloats(&m_sceneUniforms.viewProj[0][0], 16)),
      hashFloats(&renderDistance, 1));
  if (visKey == m_visibleKey)
    return;
  m_visibleKey = visKey;

  m_visible.clear();
  if (!m_scene)
    return;

  Frustum frustum;
  frustum.extract(m_sceneUniforms.viewProj);

  const auto &objects = m_scene->gameObjects;

  for (qsizetype i = 0; i < objects.size(); ++i) {
    const auto &obj = objects[i];
    const auto mesh = obj->getMesh();
    if (!mesh || obj->hidden() || isToolBrush(*obj))
      continue;

    const auto [lo, hi] = drawnAABB(*obj);
    if (!frustum.isBoxVisible(lo, hi))
      continue;

    const glm::vec3 closest = glm::clamp(m_camera->getPosition(), lo, hi);
    if (glm::distance(m_camera->getPosition(), closest) > renderDistance) {
      ++m_perf.distanceCulled;
      continue;
    }

    const MeshGpuRecord *rec = m_gpu.record(obj->stableId());
    if (!rec || rec->indexCount == 0)
      continue;

    // Faces read their data from the per-face storage buffer, so consecutive faces sharing a material are one draw.
    const quint32 objectSlot = objectSlotOf(obj->stableId());
    for (quint32 f = 0; f < rec->faceCount; ++f) {
      eng::Material *material = mesh->faceMaterialRef(static_cast<int>(f)).data();
      const quint32 start = rec->indexOffset + mesh->faceIndexStart(int(f));
      const quint32 count = mesh->faceIndexCount(int(f));
      if (!m_visible.empty()) {
        VisibleFace &last = m_visible.back();
        if (last.objectSlot == objectSlot && last.material == material && last.vertexBase == rec->vertexOffset
            && last.indexStart + last.indexCount == start) {
          last.indexCount += count;
          continue;
        }
      }
      VisibleFace vf;
      vf.material = material;
      vf.objectSlot = objectSlot;
      vf.indexStart = start;
      vf.indexCount = count;
      vf.vertexBase = rec->vertexOffset;
      m_visible.push_back(vf);
    }
  }

  const bool perf = perfOn();
  QElapsedTimer sortTimer;
  if (perf)
    sortTimer.start();
  sortByMaterial();
  if (perf)
    m_perf.msSort += sortTimer.nsecsElapsed() * 1e-6;
  m_perf.visibleFaces += m_visible.size();
}

void Engine3D::render(QRhiCommandBuffer *cb, QRhiRenderTarget *target, const QSize &pixelSize,
                     const int sampleCount) {
  QRhi *rhi = Shared::instance().rhi();
  // A target that failed to rebuild after a resize has no render pass.
  if (!rhi || !target || !target->renderPassDescriptor())
    return;
  if (m_rhi != rhi) {
    if (Shared::forRhi(m_rhi)) {
      Shared::Scope scope(m_rhi);
      releaseResources();
    }
    m_rhi = rhi;
    Shared::onRhiCleanup(rhi, this, [this] {
      releaseResources();
      m_rhi = nullptr;
    });
  }
  updateZenBox();
  // Logs the first frames after a level switch.
  if (m_switchFramesToLog > 0) {
    if (m_switchFramesToLog < 3)
      qInfo().noquote() << QStringLiteral("[docs] frame %1 after the switch: %2 ms")
                               .arg(3 - m_switchFramesToLog).arg(double(m_lastRenderMs), 0, 'f', 1);
    --m_switchFramesToLog;
  }

  struct RenderClock {
    QElapsedTimer t;
    float &out;
    explicit RenderClock(float &o) : out(o) { t.start(); }
    ~RenderClock() { out = float(t.nsecsElapsed()) * 1e-6f; }
  } renderClock(m_lastRenderMs);

  // Frame profiler: stage() closes its bucket and restarts the clock, so the buckets add up to the frame.
  const bool perf = perfOn();
  QElapsedTimer stageTimer;
  QElapsedTimer subTimer;
  if (perf) {
    if (!m_perfClock.isValid())
      m_perfClock.start();
    if (!m_gapTimer.isValid())
      m_gapTimer.start();
    else
      m_perf.msGap += double(m_gapTimer.nsecsElapsed()) * 1e-6;
    m_gapTimer.restart();
    stageTimer.start();
    subTimer.start();
  }
  auto stage = [perf, &stageTimer](const char *name, double &bucket) {
    if (!perf)
      return;
    const double ms = double(stageTimer.nsecsElapsed()) * 1e-6;
    bucket += ms;
    if (ms > kSlowStageMs) {
      qInfo(perfLog(), "SLOW stage %s = %.1f ms", name, ms);
    }
    stageTimer.restart();
  };
  // Splits the pre-pass without stealing ticks from the top-level buckets.
  auto substage = [perf, &subTimer](const char *name, double &bucket) {
    if (!perf)
      return;
    const double ms = double(subTimer.nsecsElapsed()) * 1e-6;
    bucket += ms;
    if (ms > kSlowStageMs) {
      qInfo(perfLog(), "SLOW pre-stage %s = %.1f ms", name, ms);
    }
    subTimer.restart();
  };

  float frameSeconds = std::clamp(m_frameTimer->restart() / 1000.0f, 1.0f / 1000.0f, 0.25f);
  // Redraws are on demand: the gap before the first frame of a move is idle time, not frame time.
  if (!m_prevFrameAnimating)
    frameSeconds = std::min(frameSeconds, 1.0f / 60.0f);
  m_lastViewSize = pixelSize;

  updateCamera(frameSeconds);
  m_prevFrameAnimating = wantsContinuousFrames();

  const QColor clear = m_clearColor;

  // The uploader is only valid for the frame that created it (last frame's batch is already submitted).
  Shared::instance().setUploader(nullptr);

  syncPropObjects();
  pumpModifiers(ModPump::Frame);
  if (m_scene)
    eng::refreshSmoothUnits(*m_scene, m_smoothUnits);
  if (!m_scene) {
    cb->beginPass(target, clear, {1.0f, 0}, nullptr);
    cb->endPass();
    return;
  }

  QRhiResourceUpdateBatch *rub = rhi->nextResourceUpdateBatch();
  Shared::instance().setUploader(rub);
  TextureManager::instance().pumpUploads(rub);


  m_gpu.sync(m_scene.data(), rub);
  // After the sync: it needs the face pool's layout.
  computeFrameSignatures();
  substage("gpusync", m_perf.msGpuSync);
  ensureBuffers();
  ensureShadowTarget();
  ensureLightAtlas();
  ensureOverlayTargets(pixelSize);
  ensureSsaoTargets(pixelSize, rub);
  ensureLutTexture(rub);
  ensureSkyTexture(rub);
  ensureEntityResources(rub);
  setupPipelines(target->renderPassDescriptor(), sampleCount);
  substage("ensure", m_perf.msEnsure);
  uploadObjectBlocks(rub);
  uploadFaceBlocks(rub);
  substage("upload", m_perf.msUpload);

  applySunEntity();
  updateLightTransform();
  updateSceneUniforms(pixelSize);

  prepareLightShadows();
  fillLightShadowUniforms();
  std::vector<quint32> lightIds;
  // y: world units per metre (Godot's attenuation over metres); z: specular; w: skybox bound (sky.frag).
  m_sceneUniforms.lightCount = glm::vec4(float(fillLightUniforms(m_sceneUniforms.lights, kMaxSceneLights, &lightIds)),
                                         float(eng::kWorldUnitsPerMeter), m_specular && !m_noSpecular ? m_specularStrength : 0.0f,
                                         skyboxLoaded() ? 1.0f : 0.0f);
  for (size_t li = 0; li < lightIds.size(); ++li)
    assignLightShadowSlot(lightIds[li], int(li));
  rub->updateDynamicBuffer(m_sceneUbo, 0, sizeof(SceneUniforms), &m_sceneUniforms);
  // Each cascade's matrices, read by dynamic offset.
  if (m_cascadeUbo)
    for (int c = 0; c < 4; ++c)
      rub->updateDynamicBuffer(m_cascadeUbo, c * m_cascadeStride, sizeof(CascadeBlock), &m_cascades[c]);
  if (m_ssaoUbo)
    rub->updateDynamicBuffer(m_ssaoUbo, 0, sizeof(SsaoUniforms), &m_ssaoUniforms);

  m_overlayUniforms.wireColor = glm::vec4(1.0f, 1.0f, 1.0f, 0.95f);
  // z: depth gap (world units) over which an occluded wire fades down to the floor in w.
  m_overlayUniforms.wireParams = glm::vec4(m_wireframeThicknessPx, 0.0018f, 6.0f, 0.12f);
  rub->updateDynamicBuffer(m_overlayUbo, 0, sizeof(OverlayUniforms), &m_overlayUniforms);
  substage("light", m_perf.msLight);

  // Before any pass submits this batch: later updates on a submitted batch are lost.
  if (!m_preview)
    buildEntityDraws(rub);
  substage("entbuild", m_perf.msEntityBuild);

  collectVisibleFaces();

  auto &textures = TextureManager::instance();
  for (const auto &vf: m_visible) {
    if (vf.material)
      textures.ensureGpu(vf.material, rub);
  }
  for (auto &vf: m_visible)
    vf.srb = materialSrb(vf.material);
  substage("vis", m_perf.msVis);

  // Selection wire geometry rides the same batch: before the pass opens.
  for (const quint32 id: m_drawSelection) {
    const auto obj = m_scene->findById(id);
    if (obj) {
      m_gpu.syncWire(obj->stableId(), obj->getMesh(), rub);
      ++m_perf.syncWireCalls;
    }
  }

  if (m_showLabels && !m_preview)
    m_perf.labelDecals += buildLabelDecals();
  m_perf.labelDecals += buildFillLayerLabels();
  // Before the text renderer takes this frame's decals.
  buildIOLineGeometry();
  m_perf.labelDecals += buildIOLabels();
  m_text->prepare(rhi, rub, m_sceneUbo, target->renderPassDescriptor(), sampleCount);
  substage("labels", m_perf.msLabels);

  bool rubSubmitted = false;
  stage("pre", m_perf.msPre);

  // Whichever overlay pass runs first carries this frame's resource updates. The selected ids are resolved to
  // object UBO slots once here.
  m_selList.clear();
  m_selList.reserve(m_drawSelection.size());
  for (const quint32 id: m_drawSelection) {
    const int slot = m_scene->indexOf(id);
    if (slot >= 0)
      m_selList.push_back(quint32(slot));
  }
  m_perf.selObjects = qint64(m_selList.size());
  if (perf && m_perf.lastSelCount != qint64(m_selList.size())) {
    qInfo(perfLog(), "selection now %zu objects", m_selList.size());
    m_perf.lastSelCount = qint64(m_selList.size());
  }
  if (!m_selList.empty() && overlayUsable()) {
    m_perf.sceneDepthDraws += renderSceneDepth(cb, pixelSize, rubSubmitted ? nullptr : rub);
    rubSubmitted = true;
  }
  stage("overlay", m_perf.msOverlay);

  if (m_renderMode == RenderMode::Component && shadowUsable()) {
    // Keyed: an unchanged map is reused instead of re-issuing every draw.
    const int cascades = std::clamp(m_cascadeCount, 1, 4);
    quint64 shadowKey = mixSig(m_sigGeometry, quint64(cascades));
    for (int c = 0; c < cascades; ++c)
      shadowKey = mixSig(shadowKey, hashFloats(&m_cascades[c].lightClip[0][0], 16));
    if (shadowKey != m_shadowMapKey) {
      cb->beginPass(m_shadowTarget, QColor::fromRgbF(1.0f, 1.0f, 1.0f, 1.0f), {1.0f, 0},
                   rubSubmitted ? nullptr : rub);
      rubSubmitted = true;
      for (int c = 0; c < cascades; ++c)
        m_perf.shadowDraws += renderShadowPass(cb, QSize(m_shadowMapSize, m_shadowMapSize), c);
      cb->endPass();
      m_shadowMapKey = shadowKey;
    } else {
      ++m_perf.shadowSkips;
    }
  }
  stage("shadow", m_perf.msShadow);

  if (m_renderMode == RenderMode::Component)
    renderLightShadowAtlas(cb, rub, rubSubmitted);
  stage("lightShadow", m_perf.msLightShadow);

  if (m_renderMode == RenderMode::Component && ssaoUsable() && !noSsao()) {
    renderSsaoChain(cb, rubSubmitted ? nullptr : rub);
    rubSubmitted = true;
  }
  stage("ssao", m_perf.msSsao);

  buildMarqueeGeometry();
  buildCutOverlayGeometry();
  buildFillOverlayGeometry();
  buildCollisionOverlay();
  buildMirrorOverlay();
  buildPathOverlay();

  // The overlay builders' own batches must be committed outside a render pass, so before it opens.
  if (m_cutWireBatch) {
    cb->resourceUpdate(m_cutWireBatch);
    m_cutWireBatch = nullptr;
  }
  if (m_fillWireBatch) {
    cb->resourceUpdate(m_fillWireBatch);
    m_fillWireBatch = nullptr;
  }
  if (m_collisionBatch) {
    cb->resourceUpdate(m_collisionBatch);
    m_collisionBatch = nullptr;
  }
  if (m_mirrorBatch) {
    cb->resourceUpdate(m_mirrorBatch);
    m_mirrorBatch = nullptr;
  }
  if (m_pathBatch) {
    cb->resourceUpdate(m_pathBatch);
    m_pathBatch = nullptr;
  }
  if (m_marqueeBatch) {
    cb->resourceUpdate(m_marqueeBatch);
    m_marqueeBatch = nullptr;
  }
  if (m_ioLineBatch) {
    cb->resourceUpdate(m_ioLineBatch);
    m_ioLineBatch = nullptr;
  }

  QRhiRenderTarget *const sceneTarget = fxaaSceneTarget(target, pixelSize, sampleCount);
  cb->beginPass(sceneTarget, clear, {1.0f, 0}, rubSubmitted ? nullptr : rub);
  static const bool moveTest = qEnvironmentVariableIsSet("EDITOR_MOVETEST");
  const qint64 drawsBefore = m_perf.sceneDraws;
  renderScenePass(cb, pixelSize);
  if (moveTest)
    qInfo("[movetest] cam=(%.2f %.2f %.2f) runs=%d drawn=%lld shadowKey=%llx",
          m_camera->getPosition().x, m_camera->getPosition().y, m_camera->getPosition().z, int(m_visible.size()),
          static_cast<long long>(m_perf.sceneDraws - drawsBefore), static_cast<unsigned long long>(m_shadowMapKey));
  applyAmbientOcclusion(cb);
  drawCollisionOverlay(cb);
  drawMirrorOverlay(cb);
  drawPathOverlay(cb);
  drawIOLines(cb);
  m_perf.wireOverlayDraws += renderWireOverlay(cb);
  if (!m_preview)
    drawEntities(cb);
  m_text->draw(cb);
  drawMarquee(cb);
  drawCutOverlay(cb);
  drawFillOverlay(cb);
  cb->endPass();
  if (sceneTarget != target)
    finishFxaa(cb, target, pixelSize, sampleCount);
  stage("main", m_perf.msMain);

  if (perf) {
    ++m_perf.frames;
    m_perf.msTotal = m_perf.msPre + m_perf.msOverlay + m_perf.msShadow + m_perf.msLightShadow
        + m_perf.msSsao + m_perf.msMain;
    const qint64 now = m_perfClock.elapsed();
    const qint64 windowMs = now - m_perfWindowMs;
    if (windowMs >= 1000) {
      const double sec = std::max<qint64>(1, windowMs) / 1000.0;
      qWarning(engLog,
               "PERF fps=%6.1f | cpu ms/frame total=%6.2f pre=%6.2f overlay=%6.2f shadow=%6.2f "
               "lightShadow=%6.2f ssao=%6.2f main=%6.2f | frame period=%7.2f inDrag=%5.2f(%5.1f/s) | "
               "pre: gpusync=%5.2f ensure=%5.2f "
               "upload=%5.2f light=%5.2f ent=%5.2f vis=%5.2f sort=%5.2f labels=%5.2f | draws scene=%6.1f "
               "shadow=%6.1f faces=%7.1f | skips shadow=%5.1f upload=%5.1f | "
               "per-sec pipelineRebuilds=%6.1f "
               "srbRebuilds=%6.1f hoverCalls=%6.1f | entityDraws/frame=%4.1f "
               "| sel=%lld sceneDepthDraws/frame=%9.1f "
               "wireOvl/frame=%5.1f labels=%5.1f syncWire=%5.1f/s distCulled=%7.1f",
               double(m_perf.frames) / sec, m_perf.msTotal / double(m_perf.frames),
               m_perf.msPre / double(m_perf.frames), m_perf.msOverlay / double(m_perf.frames),
               m_perf.msShadow / double(m_perf.frames),
               m_perf.msLightShadow / double(m_perf.frames),
               m_perf.msSsao / double(m_perf.frames), m_perf.msMain / double(m_perf.frames),
               (m_perf.msTotal / double(m_perf.frames)) + (m_perf.msGap / double(m_perf.frames)),
               m_perf.msDrag / double(std::max<qint64>(1, m_perf.frames)),
               double(m_perf.dragCalls) / sec,
               m_perf.msGpuSync / double(m_perf.frames), m_perf.msEnsure / double(m_perf.frames),
               m_perf.msUpload / double(m_perf.frames), m_perf.msLight / double(m_perf.frames),
               m_perf.msEntityBuild / double(m_perf.frames), m_perf.msVis / double(m_perf.frames),
               m_perf.msSort / double(m_perf.frames),
               m_perf.msLabels / double(m_perf.frames),
               m_perf.frames ? double(m_perf.sceneDraws) / double(m_perf.frames) : 0.0,
               m_perf.frames ? double(m_perf.shadowDraws) / double(m_perf.frames) : 0.0,
               m_perf.frames ? double(m_perf.visibleFaces) / double(m_perf.frames) : 0.0,
               m_perf.frames ? double(m_perf.shadowSkips) / double(m_perf.frames) : 0.0,
               m_perf.frames ? double(m_perf.uploadSkips) / double(m_perf.frames) : 0.0,
               double(m_perf.pipelineRebuilds) / sec, double(m_perf.srbRebuilds) / sec,
               double(m_perf.hoverCalls) / sec,
               m_perf.frames ? double(m_perf.entityDraws) / double(m_perf.frames) : 0.0,
               m_perf.selObjects,
               m_perf.frames ? double(m_perf.sceneDepthDraws) / double(m_perf.frames) : 0.0,
               m_perf.frames ? double(m_perf.wireOverlayDraws) / double(m_perf.frames) : 0.0,
               m_perf.frames ? double(m_perf.labelDecals) / double(m_perf.frames) : 0.0,
               double(m_perf.syncWireCalls) / sec,
               m_perf.frames ? double(m_perf.distanceCulled) / double(m_perf.frames) : 0.0);
      m_perfWindowMs = now;
      m_perf = PerfCounters {};
    }
  }
}

int Engine3D::renderShadowPass(QRhiCommandBuffer *cb, const QSize &pixelSize, const int cascade) {
  int draws = 0;
  if (!m_shadowPipeline || !m_shadowSrb || !m_scene)
    return draws;

  cb->setGraphicsPipeline(m_shadowPipeline.get());
  if (m_cascadeCount <= 1) {
    cb->setViewport(QRhiViewport(0, 0, pixelSize.width(), pixelSize.height()));
  } else {
    // 2x2 atlas; QRhi viewports have a bottom-left origin.
    const float t = float(pixelSize.width()) * 0.5f;
    cb->setViewport(QRhiViewport(float(cascade % 2) * t, float(cascade / 2) * t, t, t));
  }

  if (m_gpu.vertexBuffer() && m_gpu.indexBuffer()) {

    const QRhiCommandBuffer::VertexInput vertexInput(m_gpu.vertexBuffer(), 0);

    cb->setVertexInput(0, 1, &vertexInput, m_gpu.indexBuffer(), 0, QRhiCommandBuffer::IndexUInt32);

  }

  // Casters outside the light's ortho box are skipped.
  Frustum lightFrustum;
  lightFrustum.extract(m_cascades[cascade].lightClip);

  const auto &objects = m_scene->gameObjects;
  for (qsizetype i = 0; i < objects.size(); ++i) {
    const auto mesh = objects[i]->getMesh();
    if (!mesh || objects[i]->hidden() || isToolBrush(*objects[i]))
      continue;
    const MeshGpuRecord *rec = m_gpu.record(objects[i]->stableId());
    if (!rec || rec->indexCount == 0)
      continue;

    const auto [lo, hi] = objects[i]->getWorldAABB();
    if (!lightFrustum.isBoxVisible(lo, hi))
      continue;

    const QRhiCommandBuffer::DynamicOffset offsets[2] = {
        {0, quint32(cascade * m_cascadeStride)},
        {1, objectSlotOf(objects[i]->stableId()) * m_objectStride}};
    cb->setShaderResources(m_shadowSrb, 2, offsets);
    // Pool indices are mesh-local: vertexOffset is the base.
    cb->drawIndexed(rec->indexCount, 1, rec->indexOffset, static_cast<qint32>(rec->vertexOffset));
    ++draws;
  }
  return draws;
}


void Engine3D::drawFullscreen(QRhiCommandBuffer *cb, QRhiGraphicsPipeline *pipeline,
                             QRhiShaderResourceBindings *srb, const QSize &size) {
  if (!pipeline || !srb || !m_quadVb || !m_quadIb || size.isEmpty())
    return;

  // Always set the viewport (otherwise the last one set in the frame applies), and after setGraphicsPipeline():
  // QRhi ignores one set before it.
  cb->setGraphicsPipeline(pipeline);
  cb->setViewport(QRhiViewport(0, 0, size.width(), size.height()));
  const QRhiCommandBuffer::VertexInput vin(m_quadVb, 0);
  cb->setVertexInput(0, 1, &vin, m_quadIb, 0, QRhiCommandBuffer::IndexUInt32);
  cb->setShaderResources(srb);
  cb->drawIndexed(6);
}

void Engine3D::renderGBufferPass(QRhiCommandBuffer *cb, const QSize &pixelSize) {
  if (!m_gbufferPipeline || !m_gbufferSrb || !m_scene)
    return;

  cb->setGraphicsPipeline(m_gbufferPipeline.get());
  cb->setViewport(QRhiViewport(0, 0, pixelSize.width(), pixelSize.height()));

  if (m_gpu.vertexBuffer() && m_gpu.indexBuffer()) {

    const QRhiCommandBuffer::VertexInput vertexInput(m_gpu.vertexBuffer(), 0);

    cb->setVertexInput(0, 1, &vertexInput, m_gpu.indexBuffer(), 0, QRhiCommandBuffer::IndexUInt32);

  }

  drawWholeObjects(cb, m_gbufferSrb, [this](const quint32 idx) {
    return withinRenderDistance(*m_scene->gameObjects[idx]);
  });
}

void Engine3D::renderSsaoChain(QRhiCommandBuffer *cb, QRhiResourceUpdateBatch *rub) {
  cb->beginPass(m_gbufferTarget, QColor::fromRgbF(0.0f, 0.0f, 0.0f, 0.0f), { 1.0f, 0 }, rub);
  renderGBufferPass(cb, m_ssaoSize);
  cb->endPass();

  cb->beginPass(m_aoTargetA, QColor::fromRgbF(1.0f, 1.0f, 1.0f, 1.0f), { 1.0f, 0 }, nullptr);
  drawFullscreen(cb, m_ssaoPipeline.get(), m_ssaoSrb, m_aoSize);
  cb->endPass();

  cb->beginPass(m_aoTargetB, QColor::fromRgbF(1.0f, 1.0f, 1.0f, 1.0f), { 1.0f, 0 }, nullptr);
  drawFullscreen(cb, m_aoBlurPipeline.get(), m_aoBlurSrb, m_aoSize);
  cb->endPass();

  // EDITOR_AODEBUG=1: reads the g-buffer and AO result back once and prints their stats.
  static int aoDebugFrame = qEnvironmentVariableIsSet("EDITOR_AODEBUG") ? 3 : -1;
  if (aoDebugFrame > 0 && --aoDebugFrame == 0) {
    auto *batch = Shared::instance().rhi()->nextResourceUpdateBatch();
    auto *posRes = new QRhiReadbackResult;
    auto *aoRes = new QRhiReadbackResult;
    posRes->completed = [posRes] {
      const auto *p = reinterpret_cast<const float *>(posRes->data.constData());
      const qsizetype n = posRes->data.size() / 16;
      qsizetype written = 0;
      double zmin = 1e9, zmax = -1e9;
      for (qsizetype i = 0; i < n; ++i)
        if (p[i * 4 + 2] != 0.0f) {
          ++written;
          zmin = std::min(zmin, double(p[i * 4 + 2]));
          zmax = std::max(zmax, double(p[i * 4 + 2]));
        }
      qInfo("[aodebug] gbuffer %dx%d: %lld/%lld pixels written, view z %.2f .. %.2f", posRes->pixelSize.width(),
            posRes->pixelSize.height(), static_cast<long long>(written), static_cast<long long>(n), zmin, zmax);
      delete posRes;
    };
    aoRes->completed = [aoRes] {
      const auto *h = reinterpret_cast<const qfloat16 *>(aoRes->data.constData());
      const qsizetype n = aoRes->data.size() / 2;
      double sum = 0.0, mn = 1e9;
      qsizetype darker = 0;
      for (qsizetype i = 0; i < n; ++i) {
        const double v = double(float(h[i]));
        sum += v;
        mn = std::min(mn, v);
        darker += v < 0.95 ? 1 : 0;
      }
      qInfo("[aodebug] ao %dx%d: mean %.3f min %.3f, %lld/%lld texels below 0.95", aoRes->pixelSize.width(),
            aoRes->pixelSize.height(), n ? sum / double(n) : 0.0, mn, static_cast<long long>(darker),
            static_cast<long long>(n));
      delete aoRes;
    };
    auto *nrmRes = new QRhiReadbackResult;
    nrmRes->completed = [nrmRes] {
      const auto *h = reinterpret_cast<const qfloat16 *>(nrmRes->data.constData());
      const qsizetype n = nrmRes->data.size() / 8;
      qsizetype unit = 0;
      for (qsizetype i = 0; i < n; ++i) {
        const float x = float(h[i * 4]), y = float(h[i * 4 + 1]), z = float(h[i * 4 + 2]);
        unit += (x * x + y * y + z * z) > 0.5f ? 1 : 0;
      }
      qInfo("[aodebug] normals %dx%d format bytes/px %lld: %lld unit-length", nrmRes->pixelSize.width(),
            nrmRes->pixelSize.height(), static_cast<long long>(nrmRes->data.size() / std::max<qsizetype>(1, qsizetype(nrmRes->pixelSize.width()) * nrmRes->pixelSize.height())),
            static_cast<long long>(unit));
      delete nrmRes;
    };
    batch->readBackTexture(QRhiReadbackDescription(m_gnormTex), nrmRes);
    qInfo("[aodebug] pipeline=%p srb=%p aoSize=%dx%d", static_cast<void *>(m_ssaoPipeline.get()),
          static_cast<void *>(m_ssaoSrb), m_aoSize.width(), m_aoSize.height());
    batch->readBackTexture(QRhiReadbackDescription(m_gposTex), posRes);
    batch->readBackTexture(QRhiReadbackDescription(qEnvironmentVariableIsSet("EDITOR_AODEBUG_RAW") ? m_aoTexA : m_aoTexB), aoRes);
    cb->resourceUpdate(batch);
  }
}

void Engine3D::applyAmbientOcclusion(QRhiCommandBuffer *cb) {
  if (m_renderMode != RenderMode::Component || !ssaoUsable() || noSsao())
    return;
  drawFullscreen(cb, m_aoApplyPipeline.get(), m_aoApplySrb, m_lastViewSize);
}

void Engine3D::renderScenePass(QRhiCommandBuffer *cb, const QSize &pixelSize) {
  if (!m_scenePipeline)
    return;
  // Sky first; the level draws over it.
  if (m_skyEnabled && m_renderMode == RenderMode::Component && m_skyPipeline && m_skySrb)
    drawFullscreen(cb, m_skyPipeline.get(), m_skySrb, pixelSize);

  cb->setGraphicsPipeline(m_scenePipeline.get());
  cb->setViewport(QRhiViewport(0, 0, pixelSize.width(), pixelSize.height()));

  if (m_gpu.vertexBuffer() && m_gpu.indexBuffer()) {

    const QRhiCommandBuffer::VertexInput vertexInput(m_gpu.vertexBuffer(), 0);

    cb->setVertexInput(0, 1, &vertexInput, m_gpu.indexBuffer(), 0, QRhiCommandBuffer::IndexUInt32);

  }

  for (const auto &vf: m_visible) {
    if (!vf.srb)
      continue;
    const QRhiCommandBuffer::DynamicOffset offset(1, vf.objectSlot * m_objectStride);
    cb->setShaderResources(vf.srb, 1, &offset);
    cb->drawIndexed(vf.indexCount, 1, vf.indexStart, static_cast<qint32>(vf.vertexBase));
    ++m_perf.sceneDraws;
  }
}

int Engine3D::renderWireOverlay(QRhiCommandBuffer *cb) {
  if (m_drawSelection.empty() || m_maskHidden || !m_wirePipeline || !m_wireSrb || !m_scene)
    return 0;

  cb->setGraphicsPipeline(m_wirePipeline.get());
  int draws = 0;
  QRhiCommandBuffer::VertexInput vertexInput(m_gpu.wireVertexBuffer(), 0);
  cb->setVertexInput(0, 1, &vertexInput, m_gpu.wireIndexBuffer(), 0, QRhiCommandBuffer::IndexUInt32);

  // The shader turns the gap to the scene-depth texture into the see-through fade.
  for (size_t k = 0; k < m_selList.size(); ++k) {
    const quint32 objSlot = m_selList[k];
    const auto obj = m_scene->at(objSlot);
    if (!obj)
      continue;
    const auto mesh = obj->getMesh();
    if (!mesh)
      continue;
    if (toolBrushHidden(*obj))
      continue;
    const MeshGpuRecord *rec = m_gpu.record(obj->stableId());
    // Stale records: their pool indices no longer describe this mesh.
    if (!rec || rec->wireIndexCount == 0 || rec->wireVersion != mesh->geometryVersion())
      continue;

    QRhiShaderResourceBindings *srb = m_wireSrb;
    const QRhiCommandBuffer::DynamicOffset offset(1, objectSlotOf(obj->stableId()) * m_objectStride);
    cb->setShaderResources(srb, 1, &offset);
    cb->drawIndexed(rec->wireIndexCount, 1, rec->wireIndexOffset,
                    static_cast<qint32>(rec->wireVertexOffset));
    ++draws;
  }
  return draws;
}

int Engine3D::buildLabelDecals() {
  // Dimension labels: single selection only.
  if (!m_text || m_selectedObjects.size() != 1 || !m_scene || m_physPose.contains(*m_selectedObjects.begin()))
    return 0;

  // "width x height" painted in the top-left corner (on screen) of the face most facing the camera.
  // Upright, never mirrored: u = the face direction most to the right on screen, v = the in-face perpendicular
  // clockwise from it (projectToScreen is top-down). u and v share one world scale (from the pixel size at the
  // face centre along the camera's right), capped to fit the face.
  int decals = 0;
  const double kUnits = kSourceUnitsPerWorldUnit;
  const glm::vec3 camPos = m_camera->getPosition();
  const glm::vec3 camFwd = m_camera->getDirection();
  const glm::vec3 camRight = m_camera->getRight();

  for (const quint32 id: m_selectedObjects) {
    const auto gameObject = m_scene->findById(id);
    if (!gameObject)
      continue;
    const auto mesh = gameObject->getMesh();
    if (!mesh)
      continue;

    const auto [bmin, bmax] = gameObject->getWorldAABB();
    const float meshScale = std::max({bmax.x - bmin.x, bmax.y - bmin.y, bmax.z - bmin.z});
    if (meshScale <= 1e-4f)
      continue;
    const glm::mat4 model = gameObject->getModelMatrix();
    const glm::vec3 meshCenter = (bmin + bmax) * 0.5f;

    std::vector<glm::vec3> poly, best;
    glm::vec3 bestNormal(0.0f), bestCenter(0.0f);
    float bestFacing = 0.0f;
    for (int face = 0; face < mesh->faceCount(); ++face) {
      if (!mesh->getFacePolygon(face, poly) || poly.size() < 3)
        continue;
      glm::vec3 center(0.0f), normal(0.0f);
      for (auto &p: poly) {
        p = glm::vec3(model * glm::vec4(p, 1.0f));
        center += p;
      }
      center /= float(poly.size());
      for (size_t i = 0; i < poly.size(); ++i) // Newell: robust for any convex polygon
        normal += glm::cross(poly[i], poly[(i + 1) % poly.size()]);
      const float len = glm::length(normal);
      if (len <= 1e-8f)
        continue;
      normal /= len;
      if (glm::dot(normal, center - meshCenter) < 0.0f)
        normal = -normal;
      const glm::vec3 toCam = camPos - center;
      const float dist = glm::length(toCam);
      if (dist <= 1e-6f)
        continue;
      const float facing = glm::dot(normal, toCam / dist);
      if (facing > bestFacing) {
        bestFacing = facing;
        best = poly;
        bestNormal = normal;
        bestCenter = center;
      }
    }
    if (best.empty() || glm::dot(bestCenter - camPos, camFwd) <= 1e-3f)
      continue;

    const glm::vec2 c2 = projectToScreen(bestCenter);
    auto screenDir = [&](const glm::vec3 &d) { return projectToScreen(bestCenter + d) - c2; };

    // u: the face edge direction (either sense) most to the right on screen.
    glm::vec3 uUnit(0.0f);
    float bestRight = -2.0f;
    for (size_t i = 0; i < best.size(); ++i) {
      const glm::vec3 e = best[(i + 1) % best.size()] - best[i];
      const float len = glm::length(e);
      if (len <= 1e-5f)
        continue;
      for (const float sgn: {1.0f, -1.0f}) {
        const glm::vec3 d = e / len * sgn;
        const glm::vec2 sd = screenDir(d);
        const float sl = glm::length(sd);
        if (sl <= 1e-6f)
          continue;
        if (sd.x / sl > bestRight) {
          bestRight = sd.x / sl;
          uUnit = d;
        }
      }
    }
    if (bestRight < -1.5f)
      continue;
    // v: perpendicular to u in the face, clockwise from u on screen (top-down y).
    glm::vec3 vUnit = glm::normalize(glm::cross(bestNormal, uUnit));
    {
      const glm::vec2 du = screenDir(uUnit), dv = screenDir(vUnit);
      if (du.x * dv.y - du.y * dv.x < 0.0f)
        vUnit = -vUnit;
    }

    // Extent along u and v: the label's numbers and its corner.
    float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
    for (const glm::vec3 &p: best) {
      const float pu = glm::dot(p - bestCenter, uUnit), pv = glm::dot(p - bestCenter, vUnit);
      u0 = std::min(u0, pu);
      u1 = std::max(u1, pu);
      v0 = std::min(v0, pv);
      v1 = std::max(v1, pv);
    }
    const float edgeU = u1 - u0, edgeV = v1 - v0;
    if (edgeU <= 1e-4f || edgeV <= 1e-4f)
      continue;

    const QString label = QStringLiteral("%1x%2")
                              .arg(std::llround(double(edgeU) * kUnits))
                              .arg(std::llround(double(edgeV) * kUnits));
    const QSize spriteSize = m_text->size(label);
    if (spriteSize.isEmpty())
      continue;

    // World size per sprite pixel, shrunk (down to half) when the label would not fit the face.
    const float pxPerWorld = glm::length(screenDir(camRight));
    if (pxPerWorld <= 1e-4f)
      continue;
    const float sw = float(spriteSize.width()), sh = float(spriteSize.height());
    float worldPerPx = 1.0f / pxPerWorld;
    const float fit = std::min(edgeU * 0.9f / sw, edgeV * 0.9f / sh);
    worldPerPx = std::max(std::min(worldPerPx, fit), worldPerPx * 0.5f);
    const float uLen = sw * worldPerPx, vLen = sh * worldPerPx;
    const float margin = std::min({vLen * 0.2f, edgeU * 0.05f, edgeV * 0.05f});

    const glm::vec3 origin = bestCenter + uUnit * (u0 + margin) + vUnit * (v0 + margin)
                             + bestNormal * (meshScale * 0.004f);
    m_text->drawDecal(label, origin, uUnit, vUnit, uLen, vLen, QColor(255, 255, 255), 1.0f);
    ++decals;
  }
  return decals;
}

namespace {
// One undo step for the selection change of a click (Object mode only).
struct SelectionStepGuard {
  Engine3D *e;
  bool on;
  SelectionStepGuard(Engine3D *engine, const bool record) : e(engine), on(record) {
    if (on)
      e->beginSelectionStep();
  }
  ~SelectionStepGuard() {
    if (on)
      e->endSelectionStep();
  }
};
} // namespace

void Engine3D::pick(const float x, const float y, const float viewWidth, const float viewHeight,
                    const bool additive, const bool materialMode) {
  if (!m_scene || viewWidth <= 0.0f || viewHeight <= 0.0f)
    return;
  const SelectionStepGuard step(this, !materialMode && !m_editMode);

  const float ndcX = 2.0f * (x / viewWidth) - 1.0f;
  const float ndcY = 1.0f - 2.0f * (y / viewHeight);

  const float aspect = viewWidth / viewHeight;
  const float tanHalf = std::tan(glm::radians(m_camera->getFov()) * 0.5f);

  const glm::vec3 forward = m_camera->getDirection();
  const glm::vec3 right = m_camera->getRight();
  const glm::vec3 up = m_camera->getUp();

  const glm::vec3 dir = glm::normalize(forward + right * (ndcX * tanHalf * aspect) + up * (ndcY * tanHalf));
  const glm::vec3 origin = m_camera->getPosition();
  const glm::vec3 invDir(1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z);

  float bestT = 1e30f;
  uint32_t bestId = 0;

  const auto &objects = m_scene->gameObjects;
  for (qsizetype i = 0; i < objects.size(); ++i) {
    const auto &obj = objects[i];
    const auto mesh = obj->getMesh();
    if (!mesh)
      continue;

    // Culled past the render distance: not pickable.
    if (!withinRenderDistance(*obj))
      continue;
    // The output of the host whose sources are being edited is only a preview.
    if (obj->modOutput() && obj->modOutput() == m_editSourceHost)
      continue;

    // Where it is drawn (physics poses included).
    const auto [lo, hi] = drawnAABB(*obj);
    float boxEnter = 0.0f;
    float boxExit = 0.0f;
    if (!rayAabb(origin, invDir, lo, hi, boxEnter, boxExit) || boxEnter > bestT)
      continue;

    const glm::mat4 *pose = physicsPose(obj->stableId());
    const glm::mat4 model = pose ? *pose * obj->getModelMatrix() : obj->getModelMatrix();
    const auto &indices = mesh->indexData();
    const auto &vertices = mesh->vertexData();

    const int faceCount = mesh->faceCount();
    for (int face = 0; face < faceCount; ++face) {
      const uint32_t pickingId = mesh->getFacePickingID(face);
      if (pickingId == 0)
        continue;

      const size_t base = mesh->faceIndexStart(face);
      const size_t count = mesh->faceIndexCount(face);
      if (base + count > indices.size())
        break;

      for (size_t t = 0; t < count / 3; ++t) {
        const glm::vec3 v0 = glm::vec3(model * glm::vec4(vertices[indices[base + t * 3 + 0]].position, 1.0f));
        const glm::vec3 v1 = glm::vec3(model * glm::vec4(vertices[indices[base + t * 3 + 1]].position, 1.0f));
        const glm::vec3 v2 = glm::vec3(model * glm::vec4(vertices[indices[base + t * 3 + 2]].position, 1.0f));

        float hit = 0.0f;
        if (rayTriangle(origin, dir, v0, v1, v2, hit) && hit < bestT) {
          bestT = hit;
          bestId = pickingId;
        }
      }
    }
  }

  // The I/O target picker takes the click (an entity, or cancel); the selection stays.
  if (m_targetPick) {
    quint32 entHit = 0;
    const float sx = x * float(m_lastViewSize.width()) / std::max(1.0f, viewWidth);
    const float sy = y * float(m_lastViewSize.height()) / std::max(1.0f, viewHeight);
    if (pickBillboard(sx, sy, bestId == 0 ? 1e30f : bestT, entHit)) {
      deliverTargetPick(entHit);
      return;
    }
    if (bestId)
      if (const auto o = m_scene->findById(eng::stableIdFromPicking(bestId))) {
        if (const quint32 owner = o->brushEntity() ? o->brushEntity() : o->ownerEntity()) {
          deliverTargetPick(owner);
          return;
        }
      }
    qInfo("[io] target pick cancelled (no entity under the cursor)");
    cancelTargetPick();
    return;
  }

  // Entity icons win when in front of the geometry hit, nearest first. Object mode only.
  if (!materialMode) {
    quint32 entHit = 0;
    const float sx = x * float(m_lastViewSize.width()) / std::max(1.0f, viewWidth);
    const float sy = y * float(m_lastViewSize.height()) / std::max(1.0f, viewHeight);
    if (pickBillboard(sx, sy, bestId == 0 ? 1e30f : bestT, entHit)) {
      selectMarker(entHit, additive);
      return;
    }
  }

  if (bestId == 0) {
    if (!additive)
      clearSelection();
    return;
  }
  // Locked brushes cannot be picked; a model's render object stands for its entity.
  if (const auto hitObj = m_scene->findById(eng::stableIdFromPicking(bestId))) {
    if (hitObj->locked())
      return;
    if (hitObj->ownerEntity() != 0) {
      if (!materialMode)
        selectMarker(hitObj->ownerEntity(), additive);
      return;
    }
    // Modifier output stands for its modified object.
    if (const quint32 host = hitObj->modOutput()) {
      if (materialMode) {
        // Remembered for Edit Source and the Modifiers panel.
        if (!additive)
          m_selectedFaces.clear();
        m_materialPickHost = host;
        noteFillSelection();
        emit selectionChanged();
        emit toastRequested(tr("Generated by modifiers: use Edit Source (Shift+Tab) to texture it"));
      } else {
        selectModHost(host, additive);
      }
      return;
    }
  }
  if (materialMode && m_materialPickHost) {
    m_materialPickHost = 0;
    emit selectionChanged();
  }

  const auto toggle = [](std::set<uint32_t> &set, const uint32_t id) {
    if (set.count(id))
      set.erase(id);
    else
      set.insert(id);
  };

  // Selecting in one mode drops the other's selection. Object mode: a plain click replaces, Shift adds.
  // Material mode: a plain click toggles a face, Shift takes the whole brush.
  if (materialMode) {
    m_selectedObjects.clear();
    if (additive) {
      // Every face of the clicked brush; with all of them selected, takes them out.
      std::vector<quint32> faces;
      if (const auto o = m_scene->findById(eng::stableIdFromPicking(bestId)); o && o->getMesh())
        for (int f = 0; f < o->getMesh()->faceCount(); ++f)
          if (const quint32 pid = o->getMesh()->getFacePickingID(f))
            faces.push_back(pid);
      const bool all = !faces.empty() && std::all_of(faces.begin(), faces.end(),
                                                     [this](quint32 pid) { return m_selectedFaces.count(pid) != 0; });
      for (const quint32 pid: faces) {
        if (all)
          m_selectedFaces.erase(pid);
        else
          m_selectedFaces.insert(pid);
      }
    } else {
      toggle(m_selectedFaces, bestId);
    }
    noteFillSelection();
  } else {
    const quint32 stableId = eng::stableIdFromPicking(bestId);
    if (const auto obj = m_scene->findById(stableId)) {
      const glm::vec3 pos = obj->getPosition();
      qInfo("[pick] object id=%u facePickId=%u pos=(%.3f, %.3f, %.3f)",
            stableId, bestId, pos.x, pos.y, pos.z);
    }
    m_selectedFaces.clear();
    // Shift keeps the entities, so one drag moves meshes and entities together.
    if (!additive) {
      m_selectedEntities.clear();
      m_selectedMods.clear();
      m_selectedPaths.clear();
    }
    if (additive)
      toggle(m_selectedObjects, stableId);
    else {
      m_selectedObjects.clear();
      m_selectedObjects.insert(stableId);
    }
    if (m_selectedObjects.count(stableId))
      expandSelectionToBrushEntities();
    else
      dropSelectionUnitOf(stableId);
  }

  emit selectionChanged();
}

// marquee (selection box) and the shared grid-snapped drag

namespace {

double snapTo(const double v, const double step) {
  return step > 0.0 ? std::round(v / step) * step : v;
}

} // namespace

void Engine3D::selectObject(const quint32 stableId, const bool additive) {
  if (!m_scene || m_scene->indexOf(stableId) < 0)
    return;
  const SelectionStepGuard step(this, !m_materialMode && !m_editMode);
  if (const auto o = m_scene->findById(stableId); o && o->modOutput()) {
    if (o->modOutput() != m_editSourceHost)
      selectModHost(o->modOutput(), additive);
    return;
  }
  if (!additive) {
    m_selectedMods.clear();
    m_selectedPaths.clear();
  }

  // An object selection drops the faces; entities stay unless this click replaces.
  m_selectedFaces.clear();
  if (additive) {
    if (m_selectedObjects.count(stableId))
      m_selectedObjects.erase(stableId);
    else
      m_selectedObjects.insert(stableId);
  } else {
    m_selectedObjects.clear();
    m_selectedObjects.insert(stableId);
  }
  if (m_selectedObjects.count(stableId))
    expandSelectionToBrushEntities();
  else
    dropSelectionUnitOf(stableId);
  emit selectionChanged();
}

void Engine3D::clearMarquee() {
  if (!m_marquee.valid)
    return;
  m_marquee = {};
  emit marqueeChanged();
}

void Engine3D::commitMarquee() {
  if (!m_marquee.valid || !m_scene) {
    qDebug().noquote() << QStringLiteral("[marquee] ENTER ignored: valid=%1 scene=%2")
                             .arg(m_marquee.valid ? 1 : 0)
                             .arg(m_scene ? 1 : 0);
    return;
  }

  // Material mode: the box is an auto-select region; Enter runs the last intent inside it.
  if (m_materialMode) {
    if (!m_autoSel.boxIsOurs && m_autoSel.hasRun) {
      const auto &q = m_autoSel.query;
      runAutoSelect(int(q.intent), int(q.exposure), q.allSides, q.groupId, q.contiguous, false, q.sameMaterial);
    }
    return;
  }
  // Object mode: the auto-select result box is not a box selection.
  if (m_autoSel.boxIsOurs)
    return;

  const ViewBasis bs = viewBasisFor(m_marquee.plane);
  const double a0 = std::min(m_marquee.a0, m_marquee.a1);
  const double a1 = std::max(m_marquee.a0, m_marquee.a1);
  const double b0 = std::min(m_marquee.b0, m_marquee.b1);
  const double b1 = std::max(m_marquee.b0, m_marquee.b1);

  m_selectedObjects.clear();
  m_selectedFaces.clear();
  m_selectedMods.clear();
  m_selectedPaths.clear();

  QElapsedTimer commitTimer;
  if (perfOn())
    commitTimer.start();

  const auto &objects = m_scene->gameObjects;
  for (qsizetype i = 0; i < objects.size(); ++i) {
    const auto [lo, hi] = objects[i]->getWorldAABB();
    const glm::dvec3 lo3(lo);
    const glm::dvec3 hi3(hi);
    // Sorted: a view axis can point down a world axis (the top view's up is -Z).
    const double pa0 = glm::dot(bs.right, lo3), pa1 = glm::dot(bs.right, hi3);
    const double pb0 = glm::dot(bs.up, lo3), pb1 = glm::dot(bs.up, hi3);
    const double loA = std::min(pa0, pa1), hiA = std::max(pa0, pa1);
    const double loB = std::min(pb0, pb1), hiB = std::max(pb0, pb1);
    // Overlapping, not merely touching.
    constexpr double eps = 1e-4;
    if (hiA > a0 + eps && loA < a1 - eps && hiB > b0 + eps && loB < b1 - eps)
    {
      const auto &o = objects[i];
      // Modifier output selects its modified object (not the one whose sources are being edited).
      if (o->hidden() || o->locked() || zenHides(*o))
        continue;
      if (o->modOutput()) {
        if (o->modOutput() != m_editSourceHost)
          m_selectedMods.insert(o->modOutput());
        continue;
      }
      m_selectedObjects.insert(o->stableId());
    }
  }

  m_marquee = {};
  qDebug().noquote() << QStringLiteral("[marquee] ENTER committed %1 object(s)")
                           .arg(m_selectedObjects.size());
  if (perfOn()) {
    qInfo(perfLog(), "commitMarquee: %zu objects matched in %.2f ms",
             m_selectedObjects.size(), commitTimer.nsecsElapsed() * 1e-6);
  }
  emit marqueeChanged();
  emit selectionChanged();
}

void Engine3D::beginDrag(const DragTarget target, const int plane, const glm::dvec2 &grabPlane,
                        const double step, const double depth0, const double depth1) {
  m_dragTarget = target;
  m_dragPlane = plane;
  m_dragStep = step > 0.0 ? step : 1.0;
  m_dragGrab = grabPlane;
  m_dragObjects.clear();
  m_dragBasePos.clear();
  // Stale scales would become this move's undo "before" state.
  m_dragBaseScale.clear();
  m_dragDuplicate = {};

  if (target == DragTarget::CreateMarquee) {
    const double a = snapTo(grabPlane.x, m_dragStep);
    const double b = snapTo(grabPlane.y, m_dragStep);
    const bool droppedLive = m_marquee.valid;
    m_marquee = {};
    m_marquee.plane = plane;
    m_marquee.a0 = a;
    m_marquee.a1 = a;
    m_marquee.b0 = b;
    m_marquee.b1 = b;
    m_marquee.d0 = depth0;
    m_marquee.d1 = depth1;
    // Invalid until updateDrag gives it a real extent, so a click leaves no box. The dropped live box needs the
    // signal, or the other views keep drawing it.
    if (droppedLive) {
      qDebug().noquote() << QStringLiteral("[marquee] dropped live box, new press in plane %1").arg(plane);
      emit marqueeChanged();
    }
  } else if (target == DragTarget::MoveMarquee) {
    m_dragBase = m_marquee;
  } else if (target == DragTarget::MoveSelection) {
    if (m_scene) {
      // Modified objects move through their sources (one bound to a path stays).
      m_dragObjects = moveTargets();
      if (const QString why = modMoveBlocked(); !why.isEmpty())
        emit toastRequested(why);
      for (const quint32 id: m_dragObjects) {
        const auto obj = m_scene->findById(id);
        m_dragBasePos.push_back(obj ? obj->getPosition() : glm::vec3(0.0f));
      }
    }
    captureFaceUv(m_dragObjects, m_dragBaseUv);
    beginMirrorPreview(m_dragObjects);

    // The bounding box's start corner: snapping applies to where it ends up, not to the delta, so an off-grid
    // selection lands on the grid.
    const ViewBasis bs = viewBasisFor(plane);
    m_dragBaseMinA = DBL_MAX;
    m_dragBaseMinB = DBL_MAX;
    for (const quint32 id: m_dragObjects) {
      const auto obj = m_scene ? m_scene->findById(id) : nullptr;
      if (!obj || (isProcRamp(*obj) && m_dragObjects.size() > 1))
        continue;
      const auto [lo, hi] = obj->getWorldAABB();
      const glm::dvec3 lo3(lo);
      const glm::dvec3 hi3(hi);
      double a0 = 0.0;
      double a1 = 0.0;
      double b0 = 0.0;
      double b1 = 0.0;
      projectOntoAxis(bs.right, lo3, hi3, a0, a1);
      projectOntoAxis(bs.up, lo3, hi3, b0, b1);
      m_dragBaseMinA = std::min(m_dragBaseMinA, a0);
      m_dragBaseMinB = std::min(m_dragBaseMinB, b0);
    }
    if (m_dragBaseMinA == DBL_MAX)
      m_dragBaseMinA = 0.0;
    if (m_dragBaseMinB == DBL_MAX)
      m_dragBaseMinB = 0.0;

    // Entities move along. With only entities selected, the snap anchor is an entity origin (as the 3D gizmo).
    m_dragEntBase = selectedEntityOrigins();
    if (m_dragObjects.empty() && !m_dragEntBase.empty()) {
      const glm::dvec3 o(m_dragEntBase.begin()->second);
      m_dragBaseMinA = glm::dot(bs.right, o);
      m_dragBaseMinB = glm::dot(bs.up, o);
    }
  }
}

void Engine3D::updateDrag(const glm::dvec2 &curPlane, const bool duplicate) {
  ScopedDragPerf drag(m_perf.msDrag, m_perf.dragCalls);
  if (m_dragTarget == DragTarget::None)
    return;

  // Before the scene guard, so an entity-only selection drags with nothing loaded. Ctrl: off the grid.
  const bool free = QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier);
  if (m_dragTarget == DragTarget::MoveSelection) {
    const ViewBasis bs = viewBasisFor(m_dragPlane);
    const double snapA = snapTo(m_dragBaseMinA + (curPlane.x - m_dragGrab.x), m_dragStep);
    const double snapB = snapTo(m_dragBaseMinB + (curPlane.y - m_dragGrab.y), m_dragStep);
    const glm::dvec2 s = free ? curPlane - m_dragGrab : glm::dvec2(snapA - m_dragBaseMinA, snapB - m_dragBaseMinB);
    const glm::vec3 worldDelta(float(s.x * bs.right.x + s.y * bs.up.x),
                              float(s.x * bs.right.y + s.y * bs.up.y),
                              float(s.x * bs.right.z + s.y * bs.up.z));

    // Shift duplicates once the move has landed on a new cell.
    if (duplicate && !m_dragDuplicate.triggered && (s.x != 0.0 || s.y != 0.0)) {
      spawnDuplicates(m_dragObjects, m_dragBasePos, m_dragBaseUv, m_dragDuplicate);
      spawnEntityDuplicates(m_dragEntBase, m_dragDuplicate);
    }

    if (m_scene) {
      for (size_t k = 0; k < m_dragObjects.size(); ++k) {
        const auto obj = m_scene->findById(m_dragObjects[k]);
        if (obj)
          obj->setPosition(m_dragBasePos[k] + worldDelta);
      }
      applyTextureLockMove(m_dragObjects, m_dragBaseUv, worldDelta);
      // Linked twins follow live; a duplicate drag moves new brushes instead.
      if (m_dragDuplicate.triggered)
        endMirrorPreview();
      else
        updateMirrorPreview();
      emit sceneChanged();
    }

    if (!m_dragEntBase.empty())
      moveSelectedEntities(m_dragEntBase, worldDelta);
    return;
  }

  if (!m_scene)
    return;

  if (m_dragTarget == DragTarget::ResizeMarquee || m_dragTarget == DragTarget::ResizeMeshes) {
    updateResize(curPlane);
    if (m_dragTarget == DragTarget::ResizeMeshes)
      updateMirrorPreview();
    return;
  }

  if (m_dragTarget == DragTarget::CreateMarquee) {
    const double a1 = snapTo(m_marquee.a0 + (curPlane.x - m_dragGrab.x), m_dragStep);
    const double b1 = snapTo(m_marquee.b0 + (curPlane.y - m_dragGrab.y), m_dragStep);
    if (a1 == m_marquee.a1 && b1 == m_marquee.b1)
      return;
    m_marquee.a1 = a1;
    m_marquee.b1 = b1;
    // Valid once it covers a cell.
    m_marquee.valid = std::fabs(a1 - m_marquee.a0) >= m_dragStep
        || std::fabs(b1 - m_marquee.b0) >= m_dragStep;
    emit marqueeChanged();
    return;
  }

  const glm::dvec2 d = free ? curPlane - m_dragGrab
                            : glm::dvec2(snapTo(curPlane.x - m_dragGrab.x, m_dragStep),
                                         snapTo(curPlane.y - m_dragGrab.y, m_dragStep));

  if (m_dragTarget == DragTarget::MoveMarquee) {
    // The delta is in this view's plane, the box in the plane that created it: lift to world, project back.
    const ViewBasis cur = viewBasisFor(m_dragPlane);
    const ViewBasis own = viewBasisFor(m_dragBase.plane);
    const glm::dvec3 world = d.x * cur.right + d.y * cur.up;
    const double da = glm::dot(world, own.right);
    const double db = glm::dot(world, own.up);
    const double dd = glm::dot(world, own.depth);

    m_marquee = m_dragBase;
    m_marquee.a0 += da;
    m_marquee.a1 += da;
    m_marquee.b0 += db;
    m_marquee.b1 += db;
    m_marquee.d0 += dd;
    m_marquee.d1 += dd;
    emit marqueeChanged();
  }
}

void Engine3D::endDrag() {
  const DragTarget target = m_dragTarget;
  m_dragTarget = DragTarget::None;
  m_dragHandle = -1;
  // The twins go back first: the commit's sync rebuilds them from the result.
  endMirrorPreview();

  // The drag wrote through to the scene; record it here (unless nothing moved).
  if (target == DragTarget::MoveSelection) {
    if (m_dragDuplicate.triggered) {
      m_dragEntBase.clear(); // the copies moved, recorded with the duplicate
      pushDuplicateUndoMirrored(m_dragDuplicate, QStringLiteral("Duplicate selection"));
      return;
    }
    recordEntityMoves(m_dragEntBase, QStringLiteral("Move entities"));
    m_dragEntBase.clear();
    recordTransformEdit(QStringLiteral("Move selection"), m_dragObjects, m_dragBasePos, {}, m_dragBaseUv);
  } else if (target == DragTarget::ResizeMeshes) {
    if (regenerateResizedProc())
      return;
    recordTransformEdit(QStringLiteral("Resize selection"), m_dragObjects, m_dragBasePos,
                        m_dragBaseScale, m_dragBaseUv);
  }
}

void Engine3D::beginResize(const bool marqueeTarget, const int plane, const int handle,
                         const glm::dvec2 &grabPlane, const double step) {
  m_dragTarget = marqueeTarget ? DragTarget::ResizeMarquee : DragTarget::ResizeMeshes;
  m_dragPlane = plane;
  m_dragStep = step > 0.0 ? step : 1.0;
  m_dragGrab = grabPlane;
  m_dragHandle = handle;
  m_dragObjects.clear();
  m_dragBasePos.clear();
  m_dragBaseScale.clear();
  m_dragBox = glm::dvec4(0.0);
  m_dragDuplicate = {};

  if (marqueeTarget) {
    if (!m_marquee.valid) {
      m_dragTarget = DragTarget::None;
      return;
    }
    const MarqueeBox seen = marqueeAsSeenFrom(m_marquee, plane);
    m_dragBox = glm::dvec4(std::min(seen.a0, seen.a1), std::min(seen.b0, seen.b1),
                           std::max(seen.a0, seen.a1), std::max(seen.b0, seen.b1));
    return;
  }

  if (!m_scene || m_selectedObjects.empty()) {
    m_dragTarget = DragTarget::None;
    return;
  }

  m_dragObjects.assign(m_selectedObjects.begin(), m_selectedObjects.end());
  m_resizeBaseMin = glm::vec3(FLT_MAX);
  m_resizeBaseMax = glm::vec3(-FLT_MAX);

  for (const quint32 id: m_dragObjects) {
    const auto obj = m_scene->findById(id);
    if (!obj)
      continue;
    m_dragBasePos.push_back(obj->getPosition());
    m_dragBaseScale.push_back(obj->getScale());
  }
  if (glm::dvec3 lo, hi; objectsBounds(m_dragObjects, lo, hi)) {
    m_resizeBaseMin = glm::vec3(lo);
    m_resizeBaseMax = glm::vec3(hi);
  }

  if (m_dragObjects.empty()) {
    m_dragTarget = DragTarget::None;
    return;
  }
  captureFaceUv(m_dragObjects, m_dragBaseUv);
  beginMirrorPreview(m_dragObjects);

  const ViewBasis bs = viewBasisFor(plane);
  const glm::dvec3 mn(m_resizeBaseMin);
  const glm::dvec3 mx(m_resizeBaseMax);
  double a0 = 0.0;
  double a1 = 0.0;
  double b0 = 0.0;
  double b1 = 0.0;
  projectOntoAxis(bs.right, mn, mx, a0, a1);
  projectOntoAxis(bs.up, mn, mx, b0, b1);
  m_dragBox = glm::dvec4(a0, b0, a1, b1);
}

void Engine3D::setTextureLock(const bool lock) {
  if (m_textureLock == lock)
    return;
  m_textureLock = lock;
  emit textureLockChanged();
}

void Engine3D::setTextureScaleLock(const bool lock) {
  if (m_textureScaleLock == lock)
    return;
  m_textureScaleLock = lock;
  emit textureScaleLockChanged();
}

void Engine3D::captureFaceUv(const std::vector<quint32> &ids,
                            std::vector<DragFaceUv> &out) const {
  out.clear();
  if (!m_scene)
    return;
  out.reserve(ids.size());

  for (const quint32 id: ids) {
    DragFaceUv captured;
    if (const auto obj = m_scene->findById(id)) {
      const auto mesh = obj->getMesh();
      if (mesh) {
        const int faces = mesh->faceCount();
        captured.bases.reserve(static_cast<size_t>(faces));
        captured.normals.reserve(static_cast<size_t>(faces));
        const glm::dmat3 nm = glm::inverseTranspose(glm::dmat3(glm::dmat4(obj->getModelMatrix())));
        for (int f = 0; f < faces; ++f) {
          captured.bases.push_back(mesh->faceTexBasis(f));
          glm::vec3 n(0.0f);
          float d = 0.0f;
          glm::dvec3 wn(0.0);
          if (mesh->getFacePlane(f, n, d)) {
            wn = nm * glm::dvec3(n);
            const double len = glm::length(wn);
            wn = len > 1e-12 ? wn / len : glm::dvec3(0.0);
          }
          captured.normals.push_back(wn);
        }
      }
    }
    out.push_back(std::move(captured));
  }
}

std::shared_ptr<eng::GameObject>
Engine3D::cloneBrush(const std::shared_ptr<eng::GameObject> &src, const glm::vec3 &pos,
                     const std::vector<eng::FaceTexBasis> &baseBases) {
  const auto srcMesh = src ? src->getMesh() : nullptr;
  if (!srcMesh || srcMesh->vertexData().empty() || srcMesh->indexData().empty())
    return nullptr;

  const auto mesh = std::make_shared<eng::Mesh>();
  mesh->addVertices(srcMesh->vertexData());
  mesh->addIndices(srcMesh->indexData());
  if (srcMesh->hasFaceRanges())
    mesh->setFaceRanges(srcMesh->faceFirstArray(), srcMesh->faceCountArray());

  const auto clone = std::make_shared<eng::GameObject>(mesh);
  const quint32 cloneId = clone->stableId();
  const int faces = mesh->faceCount();
  for (int f = 0; f < faces; ++f) {
    mesh->setFaceMaterial(f, srcMesh->faceMaterialRef(f));
    mesh->setFaceTexBasis(f, f < static_cast<int>(baseBases.size()) ? baseBases[size_t(f)]
                                                                  : srcMesh->faceTexBasis(f));
    mesh->setFacePickingID(f, eng::makePickingId(cloneId, uint32_t(f)));
    mesh->setFaceGroups(f, srcMesh->faceGroups(f));
  }

  clone->setName(src->name());
  clone->setLayer(src->layer());
  clone->setGroup(src->group());
  clone->setBrushEntity(src->brushEntity());
  clone->setOrientation(src->orientation());
  clone->setModHost(src->modHost());
  if (src->smoothShading())
    clone->setSmoothShading(true);
  clone->setPosition(pos);
  clone->setRotation(src->getRotation());
  clone->setScale(src->getScale());
  clone->computeWorldAABB();
  return clone;
}

// Faces wound with the winding normal into the box, like the VMF importer: the cull mode, the shader's eye-flip
// and the wireframe depend on it.
std::shared_ptr<eng::Mesh>
Engine3D::buildBoxMesh(const glm::vec3 &mn, const glm::vec3 &mx,
                       const QSharedPointer<eng::Material> &material,
                       const eng::FaceTexBasis *faceBases,
                       const QSharedPointer<eng::Material> *faceMaterials) {
  const glm::vec3 c[2][2][2] = {
      {{{mn.x, mn.y, mn.z}, {mn.x, mn.y, mx.z}},
       {{mn.x, mx.y, mn.z}, {mn.x, mx.y, mx.z}}},
      {{{mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}},
       {{mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}}},
  };

  static const int kQuad[6][4][3] = {
      {{1, 0, 1}, {1, 1, 1}, {1, 1, 0}, {1, 0, 0}},
      {{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}},
      {{0, 1, 1}, {0, 1, 0}, {1, 1, 0}, {1, 1, 1}},
      {{0, 0, 0}, {0, 0, 1}, {1, 0, 1}, {1, 0, 0}},
      {{1, 0, 1}, {0, 0, 1}, {0, 1, 1}, {1, 1, 1}},
      {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}},
  };
  // Faces in kQuad order: +X, -X, +Y, -Y, +Z, -Z. Texture axes from eng::defaultTexAxes.
  static const glm::dvec3 kNormal[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  glm::vec3 kU[6], kV[6];
  for (int f = 0; f < 6; ++f)
    eng::defaultTexAxes(kNormal[f], kU[f], kV[f]);

  const double texW = material ? material->texWidth : 512;
  const double texH = material ? material->texHeight : 512;
  const double scale = kDefaultBrushTextureScale;

  std::array<eng::Side, 6> sides {};
  for (int f = 0; f < 6; ++f) {
    eng::Side &side = sides[f];
    const QSharedPointer<eng::Material> &mat = faceMaterials ? faceMaterials[f] : material;
    side.id = f;
    side.material = mat ? mat->name.toStdString() : std::string();
    side.uaxis = {kU[f].x, kU[f].y, kU[f].z, 0.0f, float(scale)};
    side.vaxis = {kV[f].x, kV[f].y, kV[f].z, 0.0f, float(scale)};
    for (int i = 0; i < 4; ++i) {
      const glm::vec3 &p = c[kQuad[f][i][0]][kQuad[f][i][1]][kQuad[f][i][2]];
      side.vertices_plus[i] = QVector3D(p.x, p.y, p.z);
    }
  }

  const auto mesh = eng::Mesh::createFromCubeSides(sides);
  if (!mesh)
    return nullptr;

  const float tilesU = static_cast<float>(kSourceUnitsPerWorldUnit / (texW * scale));
  const float tilesV = static_cast<float>(kSourceUnitsPerWorldUnit / (texH * scale));
  for (int f = 0; f < 6; ++f) {
    eng::FaceTexBasis basis;
    if (faceBases && faceBases[f].valid) {
      basis = faceBases[f];
    } else {
      basis.uAxis = kU[f];
      basis.vAxis = kV[f];
      basis.tilesPerUnitU = tilesU;
      basis.tilesPerUnitV = tilesV;
      basis.valid = true;
    }
    mesh->setFaceMaterial(f, faceMaterials ? faceMaterials[f] : material);
    mesh->setFaceTexBasis(f, basis);
  }
  return mesh;
}

void Engine3D::noteCutOriginPlane(const int plane) {
  if (m_cutMode != CutMode::Off || plane == m_cutOriginPlane)
    return;
  m_cutOriginPlane = plane;
  emit cutOriginChanged();
}

QString Engine3D::cutModeLabel() const {
  if (isCutout())
    return QStringLiteral("cut \u00b7 %1").arg(cutShapeLabel());
  switch (m_cutMode) {
    case CutMode::Positive: return QStringLiteral("cut \u00b7 positive");
    case CutMode::Negative: return QStringLiteral("cut \u00b7 negative");
    case CutMode::KeepBoth: return QStringLiteral("cut \u00b7 keep both");
    case CutMode::Off: break;
  }
  return QString();
}

QString Engine3D::cutShapeName(const int shape) const {
  switch (CutShape(shape)) {
    case CutShape::Door: return QStringLiteral("door");
    case CutShape::Window: return QStringLiteral("window");
    case CutShape::AirVent: return QStringLiteral("air vent");
    case CutShape::Slice: break;
  }
  return QStringLiteral("slice");
}

QString Engine3D::cutShapeLabel() const {
  return cutShapeName(static_cast<int>(m_cutShape));
}

// The first entry is the shape's default. Sizes in source units.
const std::vector<Engine3D::CutTemplate> &Engine3D::cutTemplatesFor(const CutShape shape) {
  static const std::vector<CutTemplate> kSlice = {{"slice", 0.0, 0.0}};
  static const std::vector<CutTemplate> kDoor = {
      {"regular", 56.0, 112.0},
      {"large", 80.0, 112.0},
      {"double", 112.0, 112.0},
      {"wide", 160.0, 112.0},
  };
  static const std::vector<CutTemplate> kWindow = {
      {"standard", 64.0, 48.0},
      {"small", 40.0, 32.0},
      {"wide", 96.0, 48.0},
      {"tall", 64.0, 80.0},
  };
  static const std::vector<CutTemplate> kVent = {
      {"standard", 40.0, 40.0},
      {"small", 24.0, 24.0},
      {"large", 64.0, 64.0},
  };
  switch (shape) {
    case CutShape::Door: return kDoor;
    case CutShape::Window: return kWindow;
    case CutShape::AirVent: return kVent;
    case CutShape::Slice: break;
  }
  return kSlice;
}

QStringList Engine3D::cutoutTemplateNames() const {
  QStringList names;
  for (const CutTemplate &t: cutTemplatesFor(m_cutShape))
    names << QString::fromLatin1(t.name);
  return names;
}

// Source units; a fraction of the selected wall's height, not a fixed height.
double Engine3D::cutoutSillSource() const {
  if (m_cutShape != CutShape::Window)
    return 0.0;
  glm::dvec3 mn {};
  glm::dvec3 mx {};
  if (!selectionWorldAABB(mn, mx))
    return 0.0;
  const ViewBasis bs = viewBasisFor(m_cutHolePlane);
  double u0 = 0.0;
  double u1 = 0.0;
  projectOntoAxis(bs.up, mn, mx, u0, u1);
  return (u1 - u0) * m_cutoutSill * GridView::kUnitsPerWorldUnit;
}

bool Engine3D::selectionWorldAABB(glm::dvec3 &mn, glm::dvec3 &mx) const {
  if (!m_scene)
    return false;
  // Brushes plus the selected modified objects' output.
  std::vector<quint32> ids(m_selectedObjects.begin(), m_selectedObjects.end());
  for (const quint32 h: m_selectedMods)
    for (const quint32 id: modOutputs(h))
      ids.push_back(id);
  if (ids.empty())
    return false;
  return objectsBounds(ids, mn, mx);
}

void Engine3D::setCutShape(const int shape) {
  const CutShape next = shape >= 0 && shape <= static_cast<int>(CutShape::AirVent)
                           ? CutShape(shape)
                           : CutShape::Slice;
  if (next == m_cutShape)
    return;

  m_cutShape = next;
  m_cutTemplate = 0;
  const CutTemplate &t = cutTemplatesFor(next).front();
  m_cutoutW = t.wSource;
  m_cutoutH = t.hSource;

  qDebug().noquote() << QStringLiteral("[cut] shape=%1 '%2' %3x%4 source units")
                           .arg(cutShapeLabel())
                           .arg(QString::fromLatin1(t.name))
                           .arg(m_cutoutW, 0, 'f', 1)
                           .arg(m_cutoutH, 0, 'f', 1);

  m_cutHoles.clear();
  m_cutHoleAimed = false;
  m_cutHoleDrag = false;
  emit cutShapeChanged();
  emit cutoutTemplateChanged();
  emit cutoutSizeChanged();
  emit cutoutLayoutChanged();
  emit cutHoleChanged();
  emit cutModeChanged();
  emit cutLineChanged();
}

void Engine3D::setCutoutTemplate(const int index) {
  const auto &list = cutTemplatesFor(m_cutShape);
  if (index < 0 || index >= int(list.size()) || index == m_cutTemplate)
    return;
  m_cutTemplate = index;
  m_cutoutW = list[size_t(index)].wSource;
  m_cutoutH = list[size_t(index)].hSource;
  qDebug().noquote() << QStringLiteral("[cut] template=%1 %2x%3 source units")
                           .arg(QString::fromLatin1(list[size_t(index)].name))
                           .arg(m_cutoutW, 0, 'f', 1)
                           .arg(m_cutoutH, 0, 'f', 1);
  emit cutoutTemplateChanged();
  emit cutoutSizeChanged();
  relayoutCutHoles();
}

void Engine3D::setCutoutWidthSource(const double v) {
  const double next = std::clamp(v, 1.0, 2048.0);
  if (std::fabs(next - m_cutoutW) < 1e-9)
    return;
  m_cutoutW = next;
  emit cutoutSizeChanged();
  relayoutCutHoles();
}

void Engine3D::setCutoutHeightSource(const double v) {
  const double next = std::clamp(v, 1.0, 2048.0);
  if (std::fabs(next - m_cutoutH) < 1e-9)
    return;
  m_cutoutH = next;
  emit cutoutSizeChanged();
  relayoutCutHoles();
}

void Engine3D::setCutoutSillFraction(const double v) {
  const double next = std::clamp(v, 0.0, 0.9);
  if (std::fabs(next - m_cutoutSill) < 1e-9)
    return;
  m_cutoutSill = next;
  emit cutoutSizeChanged();
  relayoutCutHoles();
}

void Engine3D::setCutoutAutoFill(const bool on) {
  if (on == m_cutoutAutoFill)
    return;
  m_cutoutAutoFill = on;
  qDebug().noquote() << QStringLiteral("[cut] auto-fill %1").arg(on ? "on" : "off");
  emit cutoutLayoutChanged();
  relayoutCutHoles();
}

void Engine3D::setCutoutSpacingSource(const double v) {
  const double next = std::clamp(v, 0.0, 1024.0);
  if (std::fabs(next - m_cutoutSpacing) < 1e-9)
    return;
  m_cutoutSpacing = next;
  emit cutoutLayoutChanged();
  relayoutCutHoles();
}

void Engine3D::setCutoutMarginSource(const double v) {
  const double next = std::clamp(v, 0.0, 1024.0);
  if (std::fabs(next - m_cutoutMargin) < 1e-9)
    return;
  m_cutoutMargin = next;
  emit cutoutLayoutChanged();
  relayoutCutHoles();
}

void Engine3D::relayoutCutHoles() {
  if (!isCutout() || !m_cutHoleAimed)
    return;
  const bool wasPlaced = cutoutPlaced();
  trackCutHole(m_cutHolePlane, m_cutHoleAim, m_cutHoleStep);
  if (wasPlaced) {
    for (auto &hole: m_cutHoles)
      hole.placed = true;
    emit cutHoleChanged();
  }
}

void Engine3D::cutHoleWorld(const CutHole &hole, glm::dvec3 &mn, glm::dvec3 &mx) {
  const ViewBasis bs = viewBasisFor(hole.plane);
  const glm::dvec3 c0 = (hole.cx - hole.w * 0.5) * bs.right + (hole.cy - hole.h * 0.5) * bs.up
      + hole.d0 * bs.depth;
  const glm::dvec3 c1 = (hole.cx + hole.w * 0.5) * bs.right + (hole.cy + hole.h * 0.5) * bs.up
      + hole.d1 * bs.depth;
  mn = glm::min(c0, c1);
  mx = glm::max(c0, c1);
}

bool Engine3D::cutHoleContains(const int plane, const glm::dvec2 &p) const {
  for (const CutHole &h: m_cutHoles) {
    if (h.plane != plane)
      continue;
    if (p.x >= h.cx - h.w * 0.5 && p.x <= h.cx + h.w * 0.5 && p.y >= h.cy - h.h * 0.5
        && p.y <= h.cy + h.h * 0.5)
      return true;
  }
  return false;
}

// The opening follows the pointer. Its depth spans the selection; a door or vent stands on the floor, a window
// on its sill.
void Engine3D::trackCutHole(const int plane, const glm::dvec2 &p, const double step) {
  if (m_cutMode == CutMode::Off || !isCutout())
    return;

  const double k = GridView::kUnitsPerWorldUnit;
  const double s = step > 0.0 ? step : 1.0;
  const double w = m_cutoutW / k;
  const double h = m_cutoutH / k;
  const ViewBasis bs = viewBasisFor(plane);

  m_cutHoleAimed = true;
  m_cutHolePlane = plane;
  m_cutHoleAim = p;
  m_cutHoleStep = s;

  glm::dvec3 selMin {};
  glm::dvec3 selMax {};
  const bool haveSel = selectionWorldAABB(selMin, selMax);

  double d0 = -1.0;
  double d1 = 1.0;
  double u0 = 0.0;
  double u1 = 0.0;
  double r0 = 0.0;
  double r1 = 0.0;
  if (haveSel) {
    projectOntoAxis(bs.depth, selMin, selMax, d0, d1);
    d0 -= 1.0;
    d1 += 1.0;
    projectOntoAxis(bs.up, selMin, selMax, u0, u1);
    projectOntoAxis(bs.right, selMin, selMax, r0, r1);
  }

  // Only a view whose vertical is the world's has a floor to stand on.
  const bool vertical = std::fabs(bs.up.y) > 0.5;
  double cy = snapTo(p.y, s);
  if (haveSel && vertical) {
    double bottom = u0;
    if (m_cutShape == CutShape::Window)
      bottom = u0 + m_cutoutSill * (u1 - u0);
    bottom = std::clamp(bottom, u0, std::max(u0, u1 - h));
    cy = bottom + h * 0.5;
  }

  std::vector<double> centers;
  if (m_cutoutAutoFill && autoFillAllowed() && haveSel) {
    const double gap = m_cutoutSpacing / k;
    const double margin = m_cutoutMargin / k;
    const double usable = (r1 - r0) - 2.0 * margin;
    if (usable >= w) {
      const int n = int((usable + gap) / (w + gap));
      if (n >= 1) {
        const double total = n * w + (n - 1) * gap;
        const double first = r0 + margin + (usable - total) * 0.5 + w * 0.5;
        for (int i = 0; i < n; ++i)
          centers.push_back(first + double(i) * (w + gap));
      }
    }
  }
  if (centers.empty())
    centers.push_back(snapTo(p.x, s));

  m_cutHoles.clear();
  for (const double c: centers) {
    CutHole hole {};
    hole.valid = true;
    hole.plane = plane;
    hole.cx = c;
    hole.cy = cy;
    hole.w = w;
    hole.h = h;
    hole.d0 = d0;
    hole.d1 = d1;
    m_cutHoles.push_back(hole);
  }

  emit cutHoleChanged();
  emit cutLineChanged();
}

void Engine3D::placeCutHole() {
  if (m_cutHoles.empty())
    return;
  for (auto &hole: m_cutHoles)
    hole.placed = true;
  qDebug().noquote()
      << QStringLiteral("[cut] %1 placed: %2 opening(s) of %3 x %4 source units, sill %5")
             .arg(cutShapeLabel())
             .arg(m_cutHoles.size())
             .arg(m_cutoutW, 0, 'f', 1)
             .arg(m_cutoutH, 0, 'f', 1)
             .arg(cutoutSillSource(), 0, 'f', 1);
  emit cutHoleChanged();
}

// The whole row shifts by the snapped horizontal delta; the vertical belongs to the shape's rule.
void Engine3D::beginCutHoleDrag(const int plane, const glm::dvec2 &p) {
  if (!cutoutPlaced() || !cutHoleContains(plane, p))
    return;
  m_cutHoleDrag = true;
  m_cutHoleGrab = p;
  m_cutHoleGrabCenters.clear();
  for (const CutHole &h: m_cutHoles)
    m_cutHoleGrabCenters.push_back(h.cx);
}

void Engine3D::dragCutHole(const glm::dvec2 &p) {
  if (!m_cutHoleDrag || m_cutHoles.empty())
    return;
  const double dx = snapTo(p.x - m_cutHoleGrab.x, m_cutHoleStep);
  for (size_t i = 0; i < m_cutHoles.size(); ++i) {
    const double base = i < m_cutHoleGrabCenters.size() ? m_cutHoleGrabCenters[i]
                                                      : m_cutHoles[i].cx;
    m_cutHoles[i].cx = base + dx;
  }
  emit cutHoleChanged();
  emit cutLineChanged();
}

void Engine3D::endCutHoleDrag() {
  if (!m_cutHoleDrag)
    return;
  m_cutHoleDrag = false;
  if (!m_cutHoles.empty())
    qDebug().noquote() << QStringLiteral("[cut] %1 row moved to cx=%2")
                             .arg(cutShapeLabel())
                             .arg(m_cutHoles.front().cx, 0, 'f', 2);
}

void Engine3D::enterCutMode() {
  if (m_cutMode != CutMode::Off) {
    cycleCutMode();
    return;
  }
  if (m_selectedObjects.empty()) {
    qDebug() << "[cut] CTRL-X ignored: nothing selected";
    return;
  }
  clearMarquee();
  m_cutMode = CutMode::Positive;
  m_cutLine = {};
  m_cutLineFrom2D = false;
  m_cutTurned = false;
  m_cutHoverValid = false;
  m_cutDrag = {};
  m_cutHoles.clear();
  m_cutHoleAimed = false;
  m_cutHoleDrag = false;
  qDebug().noquote()
      << QStringLiteral("[cut] entered, mode=positive, %1 brush(es)").arg(m_selectedObjects.size());
  emit cutModeChanged();
  emit cutLineChanged();
}

void Engine3D::cycleCutMode() {
  if (m_cutMode == CutMode::Off) {
    enterCutMode();
    return;
  }
  m_cutMode = m_cutMode == CutMode::Positive
      ? CutMode::Negative
      : (m_cutMode == CutMode::Negative ? CutMode::KeepBoth : CutMode::Positive);
  qDebug().noquote() << QStringLiteral("[cut] mode=%1").arg(cutModeLabel());
  emit cutModeChanged();
}

void Engine3D::setCutMode(const int mode) {
  if (m_cutMode == CutMode::Off || mode < 1 || mode > 3 || int(m_cutMode) == mode)
    return;
  m_cutMode = CutMode(mode);
  emit cutModeChanged();
}

void Engine3D::exitCutMode() {
  if (m_cutMode == CutMode::Off && !m_cutLine.valid && m_cutHoles.empty())
    return;
  m_cutMode = CutMode::Off;
  m_cutLine = {};
  m_cutLineFrom2D = false;
  m_cutHoverValid = false;
  m_cutDrag = {};
  m_cutHoles.clear();
  m_cutHoleAimed = false;
  m_cutHoleDrag = false;
  qDebug() << "[cut] exited, line discarded, selection untouched";
  emit cutModeChanged();
  emit cutLineChanged();
  emit cutHoleChanged();
}

void Engine3D::beginCutDrag(const int plane, const glm::dvec2 &grabPlane, const double step) {
  m_cutDrag = {};
  m_cutDrag.plane = plane;
  m_cutDrag.step = step > 0.0 ? step : 1.0;
  m_cutDrag.start = glm::dvec2(snapTo(grabPlane.x, m_cutDrag.step), snapTo(grabPlane.y, m_cutDrag.step));
  qDebug().noquote() << QStringLiteral("[cut] drag begin plane=%1 a=%2 b=%3 step=%4")
                            .arg(plane)
                            .arg(m_cutDrag.start.x, 0, 'f', 2)
                            .arg(m_cutDrag.start.y, 0, 'f', 2)
                            .arg(m_cutDrag.step, 0, 'f', 2);
}

void Engine3D::updateCutDrag(const glm::dvec2 &curPlane) {
  if (m_cutMode == CutMode::Off)
    return;

  // Any angle, ends on the grid. A straight line is kept axis-aligned (shown in the other views too).
  const glm::dvec2 end(snapTo(curPlane.x, m_cutDrag.step), snapTo(curPlane.y, m_cutDrag.step));
  if (glm::length(end - m_cutDrag.start) < m_cutDrag.step * 0.5)
    return;
  m_cutDrag.active = true;
  CutLine line;
  line.valid = true;
  line.plane = m_cutDrag.plane;
  if (end.y == m_cutDrag.start.y || end.x == m_cutDrag.start.x) {
    line.alongRight = end.y == m_cutDrag.start.y; // horizontal: b = k
    line.k = line.alongRight ? end.y : end.x;
  } else {
    line.angled = true;
    line.a = m_cutDrag.start;
    line.b = end;
  }
  if (m_cutLine.valid && m_cutLine.plane == line.plane && m_cutLine.angled == line.angled
      && (line.angled ? m_cutLine.a == line.a && m_cutLine.b == line.b
                      : m_cutLine.alongRight == line.alongRight && m_cutLine.k == line.k))
    return;
  m_cutLineFrom2D = true;
  m_cutLine = line;
  emit cutLineChanged();
}

void Engine3D::endCutDrag() {
  if (m_cutDrag.active)
    qDebug().noquote() << QStringLiteral("[cut] drag end, line at k=%1 (%2)")
                              .arg(m_cutLine.k, 0, 'f', 2)
                              .arg(m_cutLine.alongRight ? QStringLiteral("along right")
                                                       : QStringLiteral("along up"));
  else
    qDebug() << "[cut] drag produced no line";
  m_cutDrag = {};
}

namespace {

// The solver works in world units; the panel speaks Source.
eng::FillParams fillParamsFor(const Engine3D *e) {
  eng::FillParams p;
  p.facade = e->fillMode() == int(Engine3D::FillMode::Facade);
  p.wrap = e->fillWrap();
  p.minArea = e->fillMinAreaSource() / 100.0;
  p.minSpanV = e->fillMinSpanSource() / 10.0;
  p.outsideClear = e->fillClearSource() / 10.0;
  p.storeySpanFrac = e->fillStoreySpanPct() / 100.0;
  p.detectFloors = e->fillDetectFloors();
  p.floorMinArea = std::max(20.0, p.minArea * 6.0);
  p.floorProbe = std::max(32.0, p.outsideClear * 3.0);
  return p;
}

int bandOf(const std::vector<double> &bounds, const double y) {
  if (bounds.size() < 2)
    return 0;
  const double lo = bounds.front();
  const double hi = bounds.back();
  const double tol = 1e-3;
  const double clamped = std::clamp(y, lo + tol, hi - tol);
  for (size_t i = 0; i + 1 < bounds.size(); ++i) {
    if (clamped >= bounds[i] - tol && clamped <= bounds[i + 1] + tol)
      return int(i);
  }
  return int(bounds.size()) - 2;
}

} // namespace

void Engine3D::noteFillSelection() {
  const bool avail = m_materialMode && m_selectedFaces.size() == 1;
  if (avail != m_fillAvailable) {
    m_fillAvailable = avail;
    emit fillChanged();
  }
}

void Engine3D::setFillMinAreaSource(const double v) {
  if (qFuzzyCompare(v, m_fillMinAreaSource))
    return;
  m_fillMinAreaSource = v;
  emit fillOptionsChanged();
}

void Engine3D::setFillMinSpanSource(const double v) {
  if (qFuzzyCompare(v, m_fillMinSpanSource))
    return;
  m_fillMinSpanSource = v;
  emit fillOptionsChanged();
}

void Engine3D::setFillClearSource(const double v) {
  if (qFuzzyCompare(v, m_fillClearSource))
    return;
  m_fillClearSource = v;
  emit fillOptionsChanged();
}

void Engine3D::setFillStoreySpanPct(const int v) {
  if (v == m_fillStoreySpanPct)
    return;
  m_fillStoreySpanPct = v;
  emit fillOptionsChanged();
}

void Engine3D::setFillWireVisible(const bool on) {
  if (on == m_fillWireVisible)
    return;
  m_fillWireVisible = on;
  qDebug().noquote() << QStringLiteral("[fill] wireVisible=%1").arg(on ? 1 : 0);
  emit fillChanged();
}

void Engine3D::setMaskHidden(const bool hide) {
  if (hide == m_maskHidden)
    return;
  m_maskHidden = hide;
  qDebug().noquote() << QStringLiteral("[mask] hidden=%1").arg(hide ? 1 : 0);
  emit fillChanged();
}

QStringList Engine3D::fillLevelLabels() const {
  QStringList out;
  const double k = vk::GridView::kUnitsPerWorldUnit;
  for (size_t i = 0; i < m_fillLayers.size(); ++i) {
    const FillLayer &l = m_fillLayers[i];
    QString label = QStringLiteral("%1  %2 - %3 u  (%4)")
                       .arg(l.kind == eng::LayerKind::Space ? QStringLiteral("space")
                                                          : QStringLiteral("wall"))
                       .arg(l.y0 * k, 0, 'f', 1)
                       .arg(l.y1 * k, 0, 'f', 1)
                       .arg((l.y1 - l.y0) * k, 0, 'f', 0);
    if (i + 1 == m_fillLayers.size())
      label += QStringLiteral("  (top)");
    if (!l.material.isEmpty())
      label += QStringLiteral("  [%1]").arg(l.material);
    out.push_back(label);
  }
  return out;
}

double Engine3D::fillBound(const int i) const {
  if (i < 0 || i >= int(m_fillBounds.size()))
    return 0.0;
  return m_fillBounds[size_t(i)];
}

bool Engine3D::fillExtent(glm::dvec3 &mn, glm::dvec3 &mx) const {
  if (!m_fillExtentValid)
    return false;
  mn = m_fillMin;
  mx = m_fillMax;
  return true;
}

int Engine3D::fillBoundHit(const int plane, const glm::dvec2 &p, const double tolWorld) const {
  const ViewBasis bs = viewBasisFor(plane);
  // Layer bounds are horizontal planes: only views with Y up show them as lines.
  if (std::fabs(bs.up.y) < 0.5)
    return -1;

  glm::dvec3 mn {};
  glm::dvec3 mx {};
  double aLo = 0.0;
  double aHi = 0.0;
  if (fillExtent(mn, mx)) {
    projectOntoAxis(bs.right, mn, mx, aLo, aHi);
    aLo -= tolWorld;
    aHi += tolWorld;
    if (p.x < aLo || p.x > aHi)
      return -1;
  }

  int best = -1;
  double bestD = tolWorld + 1e-9;
  // The bottom bound cannot move.
  for (size_t i = 1; i < m_fillBounds.size(); ++i) {
    const double line = m_fillBounds[i] * bs.up.y;
    const double d = std::fabs(p.y - line);
    if (d <= bestD) {
      bestD = d;
      best = int(i);
    }
  }
  return best;
}

void Engine3D::setFillBound(const int i, const double worldY) {
  if (i <= 0 || i >= int(m_fillBounds.size()))
    return;

  const double lo = m_fillBounds[size_t(i) - 1];
  const double hi = i + 1 < m_fillBounds.size() ? m_fillBounds[size_t(i) + 1] : worldY;
  const double clamped = std::clamp(worldY, lo + 0.01, std::max(lo + 0.01, hi - 0.01));
  if (std::fabs(clamped - m_fillBounds[size_t(i)]) < 1e-4)
    return;

  m_fillBounds[size_t(i)] = clamped;
  // The layer list is unchanged: keep each band's material.
  std::vector<QString> kept;
  kept.reserve(m_fillLayers.size());
  for (const auto &l: m_fillLayers)
    kept.push_back(l.material);

  m_fillLayers.clear();
  for (size_t k = 0; k + 1 < m_fillBounds.size(); ++k) {
    FillLayer l;
    l.y0 = m_fillBounds[k];
    l.y1 = m_fillBounds[k + 1];
    if (k < kept.size())
      l.material = kept[k];
    m_fillLayers.push_back(l);
  }
  qDebug().noquote() << QStringLiteral("[fill] bound %1 -> %2 world (%3 u)")
                           .arg(i).arg(clamped, 0, 'f', 3).arg(clamped * 10.0, 0, 'f', 1);
  emit fillChanged();
}

void Engine3D::beginFillBoundDrag(const int plane, const glm::dvec2 &p, const double step) {
  const ViewBasis bs = viewBasisFor(plane);
  if (std::fabs(bs.up.y) < 0.5)
    return;
  m_fillDragIndex = fillBoundHit(plane, p, step * 2.0);
  if (m_fillDragIndex < 0)
    return;
  m_fillDragPlane = plane;
  m_fillDragStep = step > 0.0 ? step : 1.0;
  qDebug().noquote() << QStringLiteral("[fill] grab seam %1 at %2 world")
                           .arg(m_fillDragIndex).arg(m_fillBounds[size_t(m_fillDragIndex)], 0, 'f', 3);
}

void Engine3D::updateFillBoundDrag(const glm::dvec2 &p) {
  if (m_fillDragIndex < 0)
    return;
  const ViewBasis bs = viewBasisFor(m_fillDragPlane);
  const double worldY = snapTo(p.y / bs.up.y, m_fillDragStep);
  setFillBound(m_fillDragIndex, worldY);
}

void Engine3D::endFillBoundDrag() {
  if (m_fillDragIndex < 0)
    return;
  qDebug().noquote() << QStringLiteral("[fill] seam %1 released at %2 world")
                           .arg(m_fillDragIndex).arg(m_fillBounds[size_t(m_fillDragIndex)], 0, 'f', 3);
  m_fillDragIndex = -1;
}

bool Engine3D::solveStoreys(const quint32 seed, const eng::FillParams &p, const bool replaceSelection) {
  if (!m_scene)
    return false;
  const auto faces = eng::collectFaces(*m_scene);
  const eng::FillResult r = p.facade ? eng::solveFacadeFill(faces, seed, p)
                               : eng::solveWallFill(faces, seed, p);
  if (!r.ok || r.faces.empty()) {
    m_fillNote = r.note.isEmpty() ? QStringLiteral("nothing matched") : r.note;
    qWarning().noquote() << QStringLiteral("[fill] %1 (seed=%2)").arg(m_fillNote).arg(seed);
    emit fillChanged();
    return false;
  }

  m_fillFaces = r.faces;
  m_fillBounds = r.bounds;
  m_fillSeed = seed;
  m_fillNote.clear();

  m_fillFacades = r.facades;

  m_fillMin = glm::dvec3(1e30);
  m_fillMax = glm::dvec3(-1e30);
  for (const auto &f: faces) {
    if (std::find(r.faces.begin(), r.faces.end(), f.pickId) == r.faces.end())
      continue;
    const glm::dvec3 qmn = f.xzMin();
    const glm::dvec3 qmx = f.xzMax();
    m_fillMin.x = std::min(m_fillMin.x, qmn.x);
    m_fillMin.z = std::min(m_fillMin.z, qmn.z);
    m_fillMin.y = std::min(m_fillMin.y, f.v0);
    m_fillMax.x = std::max(m_fillMax.x, qmx.x);
    m_fillMax.z = std::max(m_fillMax.z, qmx.z);
    m_fillMax.y = std::max(m_fillMax.y, f.v1);
  }
  m_fillExtentValid = true;

  m_fillPlanes.clear();
  for (const auto &fc: m_fillFacades)
    m_fillPlanes.emplace_back(fc.axis, fc.plane);

  qDebug().noquote() << QStringLiteral("[fill] facades=%1 groundY=%2 groundObj=%3")
                           .arg(m_fillFacades.size())
                           .arg(r.groundFromSeed ? QString::number(r.groundY * 10.0, 'f', 1)
                                                : QStringLiteral("?"))
                           .arg(r.groundObj);

  m_fillLayers.clear();
  for (size_t i = 0; i + 1 < m_fillBounds.size(); ++i) {
    FillLayer l;
    l.y0 = m_fillBounds[i];
    l.y1 = m_fillBounds[i + 1];
    if (i < r.layerKinds.size())
      l.kind = r.layerKinds[i];
    m_fillLayers.push_back(l);
  }

  if (replaceSelection)
    m_selectedFaces = std::set<quint32>(r.faces.begin(), r.faces.end());
  else
    m_fillFaces.assign(m_selectedFaces.begin(), m_selectedFaces.end());

  qDebug().noquote() << QStringLiteral(
      "[fill] %1 mode=%2 wrap=%3 faces=%4 layers=%5 floors=%6 gaps=%7 y=[%8, %9]")
      .arg(p.facade ? QStringLiteral("facade") : QStringLiteral("wall"))
      .arg(p.facade ? 1 : 0)
      .arg(p.wrap ? 1 : 0)
      .arg(r.faces.size())
      .arg(m_fillLayers.size())
      .arg(r.levelYs.size())
      .arg(r.gaps.size())
      .arg(m_fillMin.y * 10.0, 0, 'f', 1)
      .arg(m_fillMax.y * 10.0, 0, 'f', 1);

  if (replaceSelection)
    emit selectionChanged();
  emit fillChanged();
  return true;
}

void Engine3D::clearFill() {
  if (m_fillFaces.empty() && m_fillBounds.empty() && m_fillNote.isEmpty() && m_fillSeed == 0)
    return;
  m_fillFaces.clear();
  m_fillBounds.clear();
  m_fillLayers.clear();
  m_fillPlanes.clear();
  m_fillFacades.clear();
  m_fillNote.clear();
  m_fillSeed = 0; // a picking id of this map's faces
  m_fillExtentValid = false;
  qDebug() << "[fill] cleared";
  emit fillChanged();
}

void Engine3D::setLayerMaterial(const int layer, const QString &name) {
  if (layer < 0 || layer >= int(m_fillLayers.size()))
    return;
  m_fillLayers[size_t(layer)].material = name.trimmed();
  qDebug().noquote() << QStringLiteral("[fill] layer %1 -> '%2'").arg(layer).arg(name);
  emit fillChanged();
}

QString Engine3D::layerMaterial(const int layer) const {
  if (layer < 0 || layer >= int(m_fillLayers.size()))
    return QString();
  return m_fillLayers[size_t(layer)].material;
}

void Engine3D::applyFillMaterial(const QString &name) {
  if (name.trimmed().isEmpty())
    return;
  const auto mat = TextureManager::instance().materialFor(name.trimmed());
  if (!mat) {
    m_fillNote = QStringLiteral("unknown material '%1'").arg(name);
    emit fillChanged();
    return;
  }

  std::set<quint32> owners;
  for (const quint32 pickId: m_fillFaces)
    owners.insert(eng::stableIdFromPicking(pickId));
  std::vector<ObjectState> before, after;
  for (const quint32 id: owners)
    before.push_back(captureObjectState(id, true));
  int touched = 0;
  for (const quint32 pickId: m_fillFaces) {
    const auto obj = m_scene ? m_scene->findById(eng::stableIdFromPicking(pickId)) : nullptr;
    if (!obj)
      continue;
    const auto mesh = obj->getMesh();
    const int face = faceIndexOfPicking(mesh, pickId);
    if (face < 0)
      continue;
    mesh->setFaceMaterial(face, mat);
    ++touched;
  }

  for (const quint32 id: owners)
    after.push_back(captureObjectState(id, true));
  if (m_undoStack && !m_undoStack->isApplying() && touched > 0)
    m_undoStack->push(std::make_unique<ObjectStatesCommand>(this, QStringLiteral("Apply material to storeys"),
                                                            std::move(before), std::move(after)));
  m_fillNote.clear();
  qDebug().noquote() << QStringLiteral("[fill] '%1' applied to %2 face(s)")
                           .arg(name).arg(touched);
  emit fillChanged();
  emit sceneChanged();
}

void Engine3D::applyFillMaterials() {
  if (!m_scene || m_fillLayers.empty())
    return;

  std::set<quint32> owners;
  for (const quint32 pickId: m_fillFaces)
    owners.insert(eng::stableIdFromPicking(pickId));
  std::vector<ObjectState> before, after;
  for (const quint32 id: owners)
    before.push_back(captureObjectState(id, true));
  const std::set<quint32> wanted(m_fillFaces.begin(), m_fillFaces.end());
  const eng::FaceGraph &graph = faceGraph();
  int touched = 0;
  int skipped = 0;
  for (const auto &f: graph.nodes) {
    if (!wanted.count(f.pickId))
      continue;
    const int band = bandOf(m_fillBounds, f.centroid.y);
    if (band < 0 || band >= int(m_fillLayers.size()))
      continue;
    const QString &name = m_fillLayers[size_t(band)].material;
    if (name.isEmpty()) {
      ++skipped;
      continue;
    }
    const auto mat = TextureManager::instance().materialFor(name);
    if (!mat) {
      ++skipped;
      continue;
    }
    const auto obj = m_scene->findById(f.objId);
    if (!obj)
      continue;
    const auto mesh = obj->getMesh();
    const int face = faceIndexOfPicking(mesh, f.pickId);
    if (face < 0)
      continue;
    mesh->setFaceMaterial(face, mat);
    ++touched;
  }

  for (const quint32 id: owners)
    after.push_back(captureObjectState(id, true));
  if (m_undoStack && !m_undoStack->isApplying() && touched > 0)
    m_undoStack->push(std::make_unique<ObjectStatesCommand>(this, QStringLiteral("Apply per storey"),
                                                            std::move(before), std::move(after)));
  qDebug().noquote() << QStringLiteral("[fill] per-layer apply: %1 face(s), %2 left alone")
                           .arg(touched).arg(skipped);
  emit fillChanged();
  emit sceneChanged();
}

// Splits the covered brushes at the layer seams into box brushes built like cut pieces (texture bases carry over).
bool Engine3D::sliceFillToLayers() {
  if (!m_scene || m_fillBounds.size() < 3) {
    m_fillNote = QStringLiteral("no layer seams to slice on");
    qWarning().noquote() << QStringLiteral("[fill] %1").arg(m_fillNote);
    emit fillChanged();
    return false;
  }

  std::vector<double> cuts(m_fillBounds.begin() + 1, m_fillBounds.end() - 1);
  if (cuts.empty()) {
    m_fillNote = QStringLiteral("only one layer, nothing to slice");
    emit fillChanged();
    return false;
  }
  std::sort(cuts.begin(), cuts.end());

  std::set<quint32> objIds;
  for (const quint32 pickId: m_fillFaces)
    objIds.insert(eng::stableIdFromPicking(pickId));

  const std::vector<quint32> prevObjects(m_selectedObjects.begin(), m_selectedObjects.end());
  const std::vector<quint32> prevFaces(m_selectedFaces.begin(), m_selectedFaces.end());

  std::vector<std::shared_ptr<eng::GameObject>> removed;
  std::vector<int> removedSlots;
  std::vector<std::shared_ptr<eng::GameObject>> pieces;
  std::vector<quint32> survivors;
  int refused = 0;

  for (const quint32 id: objIds) {
    const auto obj = m_scene->findById(id);
    if (!obj)
      continue;

    std::array<int, 6> faceSlots {};
    if (!isBoxBrush(obj, faceSlots)) {
      qDebug().noquote() << QStringLiteral("[fill] slice skipped %1: not a box brush").arg(id);
      ++refused;
      continue;
    }

    const auto [loF, hiF] = obj->getWorldAABB();
    const glm::dvec3 lo(loF);
    const glm::dvec3 hi(hiF);

    std::vector<double> ys {lo.y};
    for (const double c: cuts)
      if (c > lo.y + 1e-3 && c < hi.y - 1e-3)
        ys.push_back(c);
    ys.push_back(hi.y);
    if (ys.size() < 3) {
      qDebug().noquote() << QStringLiteral("[fill] slice skipped %1: no seam crosses it").arg(id);
      continue;
    }

    for (size_t k = 0; k + 1 < ys.size(); ++k) {
      const auto piece = buildCutPiece(obj, faceSlots, glm::vec3(float(lo.x), float(ys[k]), float(lo.z)),
                                     glm::vec3(float(hi.x), float(ys[k + 1]), float(hi.z)));
      if (!piece) {
        qWarning().noquote() << QStringLiteral("[fill] slice piece failed for %1").arg(id);
        continue;
      }
      pieces.push_back(piece);
      survivors.push_back(piece->stableId());
    }

    removed.push_back(obj);
    removedSlots.push_back(m_scene->indexOf(id));
  }

  if (removed.empty()) {
    m_fillNote = QStringLiteral("no covered brush could be sliced");
    qWarning().noquote() << QStringLiteral("[fill] %1 (%2 refused)").arg(m_fillNote).arg(refused);
    emit fillChanged();
    return false;
  }

  std::set<quint32> removedIds;
  for (const auto &obj: removed)
    removedIds.insert(obj->stableId());

  m_scene->removeObjects(removedIds);
  m_scene->addObjects(pieces);

  // The old picking ids died with the originals: rebuild from the plane set.
  const auto faces = eng::collectFaces(*m_scene);
  const eng::FillParams p = fillParamsFor(this);
  std::vector<quint32> rebuilt;
  for (const auto &f: faces) {
    if (!f.vertical || isTrimFace(f, p))
      continue;
    // Stay inside the fill's previous extent, or every slice grows it along the wall.
    if (m_fillExtentValid
        && (f.center.x < m_fillMin.x - 1e-3 || f.center.x > m_fillMax.x + 1e-3
            || f.center.z < m_fillMin.z - 1e-3 || f.center.z > m_fillMax.z + 1e-3
            || f.v1 < m_fillMin.y - 1e-3 || f.v0 > m_fillMax.y + 1e-3))
      continue;
    // Re-apply the storey-span test: rejected trim lies on the same planes as the wall.
    if (!eng::spansStorey(f, m_fillBounds, p.storeySpanFrac))
      continue;
    for (const auto &[axis, plane]: m_fillPlanes) {
      if (f.axis == axis && std::fabs(f.plane - plane) <= p.planeTol) {
        rebuilt.push_back(f.pickId);
        break;
      }
    }
  }

  m_fillFaces = rebuilt;
  m_selectedObjects = std::set<quint32>(survivors.begin(), survivors.end());
  m_selectedFaces.clear();
  notifyStructureChanged();

  if (m_undoStack && !m_undoStack->isApplying()) {
    auto command = std::make_unique<CutCommand>(
        this, QStringLiteral("Slice %1 brush(es) into layers").arg(removed.size()));
    command->rememberSelection(prevObjects, prevFaces);
    command->setNewSelection(survivors);
    for (size_t k = 0; k < removed.size(); ++k)
      command->addRemoved(removed[k], removedSlots[k]);
    for (const auto &piece: pieces)
      command->addPiece(piece);
    m_undoStack->push(std::move(command));
  }

  m_fillNote.clear();
  qDebug().noquote() << QStringLiteral("[fill] sliced %1 brush(es) into %2 piece(s), %3 face(s) refilled")
                           .arg(removed.size()).arg(pieces.size()).arg(rebuilt.size());
  emit selectionChanged();
  emit fillChanged();
  return true;
}

// From the geometry, not the face order (VMF keeps the file's order; buildBoxMesh indexes by axis and side).
bool Engine3D::isBoxBrush(const std::shared_ptr<eng::GameObject> &obj,
                          std::array<int, 6> &faceSlots) {
  faceSlots.fill(-1);
  if (!obj)
    return false;
  const auto mesh = obj->getMesh();
  if (!mesh || mesh->faceCount() != 6) {
    qDebug().noquote()
        << QStringLiteral("[cut] box check id=%1: %2 faces (want 6)")
               .arg(obj->stableId())
               .arg(mesh ? mesh->faceCount() : -1);
    return false;
  }

  const glm::vec3 rot = obj->getRotation();
  if (std::fabs(rot.x) > 1e-3f || std::fabs(rot.y) > 1e-3f || std::fabs(rot.z) > 1e-3f) {
    qDebug().noquote() << QStringLiteral("[cut] box check id=%1: rotated %2 %3 %4")
                             .arg(obj->stableId())
                             .arg(rot.x, 0, 'f', 3)
                             .arg(rot.y, 0, 'f', 3)
                             .arg(rot.z, 0, 'f', 3);
    return false;
  }

  const auto [loF, hiF] = obj->getWorldAABB();
  const glm::vec3 lo(loF);
  const glm::vec3 hi(hiF);
  const glm::mat4 model = obj->getModelMatrix();

  for (int f = 0; f < 6; ++f) {
    std::array<glm::vec3, 4> localQuad {};
    if (!mesh->getFaceQuad(f, localQuad)) {
      qDebug().noquote()
          << QStringLiteral("[cut] box check id=%1: face %2 has no quad").arg(obj->stableId()).arg(f);
      return false;
    }
    // The AABB is world space, getFaceQuad() mesh local: transform the quad.
    std::array<glm::vec3, 4> quad {};
    for (int k = 0; k < 4; ++k)
      quad[k] = glm::vec3(model * glm::vec4(localQuad[k], 1.0f));
    const glm::vec3 n = glm::cross(quad[1] - quad[0], quad[2] - quad[1]);
    const float len = glm::length(n);
    if (len < 1e-6f) {
      qDebug().noquote()
          << QStringLiteral("[cut] box check id=%1: face %2 is degenerate").arg(obj->stableId()).arg(f);
      return false;
    }
    const glm::vec3 u = n / len;

    int axis = 0;
    if (std::fabs(u.y) > std::fabs(u[axis]))
      axis = 1;
    if (std::fabs(u.z) > std::fabs(u[axis]))
      axis = 2;
    if (std::fabs(u[axis]) < 0.99f) {
      qDebug().noquote()
          << QStringLiteral("[cut] box check id=%1: face %2 normal (%3, %4, %5) is not axis aligned")
                 .arg(obj->stableId())
                 .arg(f)
                 .arg(u.x, 0, 'f', 3)
                 .arg(u.y, 0, 'f', 3)
                 .arg(u.z, 0, 'f', 3);
      return false;
    }

    const float coord = quad[0][axis];
    const bool maxSide = std::fabs(coord - hi[axis]) <= std::fabs(coord - lo[axis]);
    const size_t slot = static_cast<size_t>(axis) * 2u + (maxSide ? 0u : 1u);
    if (faceSlots[slot] >= 0) {
      qDebug().noquote()
          << QStringLiteral("[cut] box check id=%1: faces %2 and %3 both claim slot %4")
                 .arg(obj->stableId())
                 .arg(f)
                 .arg(faceSlots[slot])
                 .arg(slot);
      return false;
    }
    faceSlots[slot] = f;
  }
  return true;
}

// The cap takes the material and basis of the source face on the same axis and side.
std::shared_ptr<eng::GameObject>
Engine3D::buildCutPiece(const std::shared_ptr<eng::GameObject> &src,
                        const std::array<int, 6> &faceSlots, const glm::vec3 &mn,
                        const glm::vec3 &mx) {
  const auto srcMesh = src ? src->getMesh() : nullptr;
  if (!srcMesh)
    return nullptr;

  const auto fallback =
      TextureManager::instance().materialFor(QString::fromLatin1(kDefaultBrushMaterial));

  QSharedPointer<eng::Material> mats[6];
  eng::FaceTexBasis bases[6];
  for (int i = 0; i < 6; ++i) {
    const int f = faceSlots[static_cast<size_t>(i)];
    mats[i] = f >= 0 ? srcMesh->faceMaterialRef(f) : fallback;
    if (!mats[i])
      mats[i] = fallback;
    if (f >= 0)
      bases[i] = srcMesh->faceTexBasis(f);
  }

  const auto mesh = buildBoxMesh(mn, mx, fallback, bases, mats);
  if (!mesh)
    return nullptr;

  const auto piece = std::make_shared<eng::GameObject>(mesh);
  const quint32 id = piece->stableId();
  for (int f = 0; f < mesh->faceCount(); ++f)
    mesh->setFacePickingID(f, eng::makePickingId(id, uint32_t(f)));
  for (int i = 0; i < 6 && i < mesh->faceCount(); ++i)
    if (const int f = faceSlots[static_cast<size_t>(i)]; f >= 0)
      mesh->setFaceGroups(i, srcMesh->faceGroups(f));
  piece->setName(src->name());
  piece->setLayer(src->layer());
  piece->setGroup(src->group());
  piece->setBrushEntity(src->brushEntity());
  // Stays in its mirror link, so the twin gets the same cut.
  piece->setMirrorLink(src->mirrorLink(), src->mirrorSide());
  piece->setOrientation(src->orientation());
  piece->setModHost(src->modHost());
  piece->computeWorldAABB();
  return piece;
}

// Box minus rectangular prism, through the wall's full thickness (the view's depth axis). Empty strips are not built.
bool Engine3D::applyCutout() {
  if (m_cutHoles.empty()) {
    qDebug() << "[cut] ENTER ignored: no cut-out placed yet";
    return false;
  }
  if (!m_scene) {
    qWarning() << "[cut] no scene";
    return false;
  }

  const ViewBasis bs = viewBasisFor(m_cutHoles.front().plane);
  const auto dominantAxis = [](const glm::dvec3 &v) {
    int axis = 0;
    if (std::fabs(v.y) > std::fabs(v[axis]))
      axis = 1;
    if (std::fabs(v.z) > std::fabs(v[axis]))
      axis = 2;
    return axis;
  };
  const int axisA = dominantAxis(bs.right);
  const int axisB = dominantAxis(bs.up);
  const int axisD = dominantAxis(bs.depth);
  if (axisA == axisB || axisA == axisD || axisB == axisD) {
    qWarning() << "[cut] cut-out basis is not a frame, nothing applied";
    return false;
  }

  struct Rect {
    double a0 = 0.0, a1 = 0.0, b0 = 0.0, b1 = 0.0;
  };
  std::vector<Rect> openings;
  openings.reserve(m_cutHoles.size());
  for (const CutHole &hole: m_cutHoles) {
    glm::dvec3 hmn {};
    glm::dvec3 hmx {};
    cutHoleWorld(hole, hmn, hmx);
    openings.push_back({hmn[axisA], hmx[axisA], hmn[axisB], hmx[axisB]});
  }

  const std::vector<quint32> prevObjects(m_selectedObjects.begin(), m_selectedObjects.end());
  const std::vector<quint32> prevFaces(m_selectedFaces.begin(), m_selectedFaces.end());

  std::vector<std::shared_ptr<eng::GameObject>> removed;
  std::vector<int> removedSlots;
  std::vector<std::shared_ptr<eng::GameObject>> pieces;
  std::vector<quint32> survivors;
  int untouched = 0;
  int refused = 0;

  for (const quint32 id: m_selectedObjects) {
    const auto obj = m_scene->findById(id);
    if (!obj)
      continue;

    std::array<int, 6> faceSlots {};
    if (!isBoxBrush(obj, faceSlots)) {
      qDebug().noquote() << QStringLiteral("[cut] skipped %1: not axis aligned").arg(id);
      ++refused;
      survivors.push_back(id);
      continue;
    }

    const auto [loF, hiF] = obj->getWorldAABB();
    const glm::dvec3 lo(loF);
    const glm::dvec3 hi(hiF);

    std::vector<Rect> hits;
    for (const Rect &r: openings) {
      const Rect c {std::max(r.a0, lo[axisA]), std::min(r.a1, hi[axisA]), std::max(r.b0, lo[axisB]),
                   std::min(r.b1, hi[axisB])};
      if (c.a1 > c.a0 && c.b1 > c.b0)
        hits.push_back(c);
    }
    if (hits.empty()) {
      qDebug().noquote() << QStringLiteral("[cut] %1 untouched: the %2 misses it").arg(id).arg(cutShapeLabel());
      ++untouched;
      survivors.push_back(id);
      continue;
    }

    double bandLo = hits.front().b0;
    double bandHi = hits.front().b1;
    for (const Rect &h: hits) {
      bandLo = std::min(bandLo, h.b0);
      bandHi = std::max(bandHi, h.b1);
    }

    const auto strip = [&](const double s0, const double s1, const double t0, const double t1) {
      if (s1 <= s0 || t1 <= t0)
        return;
      glm::vec3 mn(lo);
      glm::vec3 mx(hi);
      mn[axisA] = float(s0);
      mx[axisA] = float(s1);
      mn[axisB] = float(t0);
      mx[axisB] = float(t1);
      const auto piece = buildCutPiece(obj, faceSlots, mn, mx);
      if (!piece) {
        qWarning().noquote() << QStringLiteral("[cut] cut-out piece build failed for %1").arg(id);
        return;
      }
      pieces.push_back(piece);
      survivors.push_back(piece->stableId());
    };

    // Full-width bands under and over the openings, then the piers between them (disjoint pieces).
    strip(lo[axisA], hi[axisA], lo[axisB], bandLo);
    strip(lo[axisA], hi[axisA], bandHi, hi[axisB]);

    std::sort(hits.begin(), hits.end(),
              [](const Rect &x, const Rect &y) { return x.a0 < y.a0; });
    double cursor = lo[axisA];
    for (const Rect &h: hits) {
      if (h.a0 > cursor)
        strip(cursor, h.a0, bandLo, bandHi);
      cursor = std::max(cursor, h.a1);
    }
    if (hi[axisA] > cursor)
      strip(cursor, hi[axisA], bandLo, bandHi);

    removed.push_back(obj);
    removedSlots.push_back(m_scene->indexOf(id));
  }

  if (removed.empty()) {
    qDebug().noquote()
        << QStringLiteral("[cut] %1 hits no selected mesh (%2 refused)").arg(cutShapeLabel()).arg(refused);
    return false;
  }

  std::set<quint32> removedIds;
  for (const auto &obj: removed)
    removedIds.insert(obj->stableId());

  m_scene->removeObjects(removedIds);
  m_scene->addObjects(pieces);
  setSelected(survivors, {});
  notifyStructureChanged();

  if (m_undoStack && !m_undoStack->isApplying()) {
    auto command = std::make_unique<CutCommand>(
        this, QStringLiteral("Cut %1 %2").arg(cutShapeLabel()).arg(removed.size()));
    command->rememberSelection(prevObjects, prevFaces);
    command->setNewSelection(survivors);
    for (size_t k = 0; k < removed.size(); ++k)
      command->addRemoved(removed[k], removedSlots[k]);
    for (const auto &piece: pieces)
      command->addPiece(piece);
    m_undoStack->push(std::move(command));
  }

  qDebug().noquote()
      << QStringLiteral("[cut] %1 applied: %2 brush(es) replaced by %3 piece(es), %4 untouched, "
                       "%5 refused")
             .arg(cutShapeLabel())
             .arg(removed.size())
             .arg(pieces.size())
             .arg(untouched)
             .arg(refused);

  exitCutMode();
  return true;
}

bool Engine3D::applyCut() {
  if (m_cutMode == CutMode::Off) {
    qDebug() << "[cut] ENTER ignored: not in cut mode";
    return false;
  }
  if (isCutout())
    return applyCutout();
  if (!m_cutLine.valid) {
    qDebug() << "[cut] ENTER ignored: no cut line drawn yet";
    return false;
  }
  if (!m_scene) {
    qWarning() << "[cut] no scene";
    return false;
  }

  if (m_cutLine.angled) {
    // The cut plane contains the drawn line and the view's depth axis.
    const ViewBasis vb = viewBasisFor(m_cutLine.plane);
    const glm::dvec2 d = m_cutLine.b - m_cutLine.a;
    const glm::dvec3 along = glm::normalize(vb.right * d.x + vb.up * d.y);
    const glm::dvec3 n = glm::normalize(glm::cross(along, vb.depth));
    const glm::dvec3 p = vb.right * m_cutLine.a.x + vb.up * m_cutLine.a.y;
    const int keep = m_cutMode == CutMode::KeepBoth ? 2 : (m_cutMode == CutMode::Positive ? 0 : 1);
    const int pieces = clipSelection(glm::vec3(n), float(glm::dot(n, p)), keep);
    m_cutLine = {};
    emit cutLineChanged();
    return pieces > 0;
  }

  const ViewBasis bs = viewBasisFor(m_cutLine.plane);
  const glm::dvec3 n = m_cutLine.alongRight ? bs.up : bs.right;
  int axis = 0;
  if (std::fabs(n.y) > std::fabs(n[axis]))
    axis = 1;
  if (std::fabs(n.z) > std::fabs(n[axis]))
    axis = 2;
  if (std::fabs(n[axis]) < 0.99) {
    qWarning() << "[cut] cut normal is not a world axis, nothing applied";
    return false;
  }

  // World coordinate along `axis`, and the AABB side of the positive half-space.
  const double sgn = n[axis] > 0.0 ? 1.0 : -1.0;
  const double kk = m_cutLine.k * sgn;
  const bool positiveIsHigh = sgn > 0.0;
  bool keepLow = true;
  bool keepHigh = true;
  if (m_cutMode == CutMode::Positive) {
    keepLow = positiveIsHigh;
    keepHigh = !positiveIsHigh;
  } else if (m_cutMode == CutMode::Negative) {
    keepLow = !positiveIsHigh;
    keepHigh = positiveIsHigh;
  }

  qDebug().noquote()
      << QStringLiteral("[cut] applying mode=%1 plane=%2 alongRight=%3 k=%4 -> axis=%5 at %6 "
                        "keepLow=%7 keepHigh=%8 over %9 selected brush(es)")
             .arg(cutModeLabel())
             .arg(m_cutLine.plane)
             .arg(m_cutLine.alongRight ? QStringLiteral("yes") : QStringLiteral("no"))
             .arg(m_cutLine.k, 0, 'f', 2)
             .arg(axis)
             .arg(kk, 0, 'f', 2)
             .arg(keepLow ? QStringLiteral("yes") : QStringLiteral("no"))
             .arg(keepHigh ? QStringLiteral("yes") : QStringLiteral("no"))
             .arg(m_selectedObjects.size());

  const std::vector<quint32> prevObjects(m_selectedObjects.begin(), m_selectedObjects.end());
  const std::vector<quint32> prevFaces(m_selectedFaces.begin(), m_selectedFaces.end());

  std::vector<std::shared_ptr<eng::GameObject>> removed;
  std::vector<int> removedSlots;
  std::vector<std::shared_ptr<eng::GameObject>> pieces;
  std::vector<quint32> survivors;
  int untouched = 0;
  int refused = 0;

  for (const quint32 id: m_selectedObjects) {
    const auto obj = m_scene->findById(id);
    if (!obj)
      continue;

    std::array<int, 6> faceSlots {};
    if (!isBoxBrush(obj, faceSlots)) {
      // Other convex brushes: split through their planes; caps take the nearest face's texture.
      auto cb = eng::ConvexBrush::fromObject(*obj);
      if (!cb) {
        // No volume (a single plane): cut through its face polygons instead.
        glm::dvec3 axisN(0.0);
        axisN[axis] = 1.0;
        FlatSplit split = splitFlatObject(*obj, axisN, kk, keepLow, keepHigh);
        const bool keepIt = split.sides == 1 ? keepLow : split.sides == 2 ? keepHigh : false;
        if (split.sides != 3 && (keepIt || split.sides == 0)) {
          ++untouched;
          survivors.push_back(id);
          continue;
        }
        qDebug().noquote() << QStringLiteral("[cut] %1 has no volume: %2").arg(id).arg(
            split.sides == 3 ? QStringLiteral("cut through its faces") : QStringLiteral("removed with its side"));
        removed.push_back(obj);
        removedSlots.push_back(m_scene->indexOf(id));
        for (const auto &piece: split.pieces) {
          pieces.push_back(piece);
          survivors.push_back(piece->stableId());
        }
        continue;
      }
      glm::dvec3 axisN(0.0);
      axisN[axis] = 1.0;
      std::optional<eng::ConvexBrush> highPart, lowPart;
      cb->split(axisN, kk, highPart, lowPart);
      if (!highPart || !lowPart) {
        const bool isHigh = highPart.has_value();
        if ((isHigh && keepHigh) || (!isHigh && keepLow)) {
          ++untouched;
          survivors.push_back(id);
        } else {
          removed.push_back(obj);
          removedSlots.push_back(m_scene->indexOf(id));
        }
        continue;
      }
      removed.push_back(obj);
      removedSlots.push_back(m_scene->indexOf(id));
      for (auto *part: {keepLow ? &lowPart : nullptr, keepHigh ? &highPart : nullptr}) {
        if (!part)
          continue;
        if (auto piece = objectFromBrush(**part, obj.get())) {
          pieces.push_back(piece);
          survivors.push_back(piece->stableId());
        }
      }
      continue;
    }

    const auto [loF, hiF] = obj->getWorldAABB();
    const glm::dvec3 lo(loF);
    const glm::dvec3 hi(hiF);

    qDebug().noquote()
        << QStringLiteral("[cut] id=%1 aabb axis%2 [%3, %4] vs plane %5 -> %6")
               .arg(id)
               .arg(axis)
               .arg(lo[axis], 0, 'f', 2)
               .arg(hi[axis], 0, 'f', 2)
               .arg(kk, 0, 'f', 2)
               .arg(lo[axis] >= kk
                        ? (keepHigh ? QStringLiteral("entirely high, untouched")
                                   : QStringLiteral("entirely high, doomed"))
                        : (hi[axis] <= kk
                               ? (keepLow ? QStringLiteral("entirely low, untouched")
                                         : QStringLiteral("entirely low, doomed"))
                               : QStringLiteral("straddles the plane")));

    std::vector<std::pair<glm::dvec3, glm::dvec3>> kept;
    if (lo[axis] >= kk) {
      if (keepHigh) {
        ++untouched;
        survivors.push_back(id);
        continue;
      }
    } else if (hi[axis] <= kk) {
      if (keepLow) {
        ++untouched;
        survivors.push_back(id);
        continue;
      }
    } else {
      if (keepLow) {
        glm::dvec3 pieceMax = hi;
        pieceMax[axis] = kk;
        kept.emplace_back(lo, pieceMax);
      }
      if (keepHigh) {
        glm::dvec3 pieceMin = lo;
        pieceMin[axis] = kk;
        kept.emplace_back(pieceMin, hi);
      }
    }

    removed.push_back(obj);
    removedSlots.push_back(m_scene->indexOf(id));
    // No thickness (a single plane): cut through its faces.
    if (!kept.empty() && !eng::ConvexBrush::fromObject(*obj)) {
      glm::dvec3 axisN(0.0);
      axisN[axis] = 1.0;
      for (const auto &piece: splitFlatObject(*obj, axisN, kk, keepLow, keepHigh).pieces) {
        pieces.push_back(piece);
        survivors.push_back(piece->stableId());
      }
      continue;
    }
    for (const auto &[pieceMin, pieceMax]: kept) {
      const auto piece = buildCutPiece(obj, faceSlots, glm::vec3(pieceMin), glm::vec3(pieceMax));
      if (!piece) {
        qWarning().noquote() << QStringLiteral("[cut] piece build failed for %1").arg(id);
        continue;
      }
      pieces.push_back(piece);
      survivors.push_back(piece->stableId());
    }
  }

  if (removed.empty()) {
    qDebug().noquote() << QStringLiteral("[cut] line touches no selected mesh (%1 refused)").arg(refused);
    return false;
  }

  std::set<quint32> removedIds;
  for (const auto &obj: removed)
    removedIds.insert(obj->stableId());

  m_scene->removeObjects(removedIds);
  m_scene->addObjects(pieces);
  setSelected(survivors, {});
  notifyStructureChanged();

  if (m_undoStack && !m_undoStack->isApplying()) {
    auto command = std::make_unique<CutCommand>(
        this, QStringLiteral("Cut %1 brush(es)").arg(removed.size()));
    command->rememberSelection(prevObjects, prevFaces);
    command->setNewSelection(survivors);
    for (size_t k = 0; k < removed.size(); ++k)
      command->addRemoved(removed[k], removedSlots[k]);
    for (const auto &piece: pieces)
      command->addPiece(piece);
    m_undoStack->push(std::move(command));
  }

  qDebug().noquote() << QStringLiteral("[cut] applied (%1): %2 brush(es) replaced by %3 piece(es), "
                                     "%4 untouched, %5 refused")
                            .arg(cutModeLabel())
                            .arg(removed.size())
                            .arg(pieces.size())
                            .arg(untouched)
                            .arg(refused);

  exitCutMode();
  return true;
}

bool Engine3D::spawnDuplicates(std::vector<quint32> &ids, std::vector<glm::vec3> &basePos,
                              std::vector<DragFaceUv> &baseUv, DuplicateSet &set) {
  set = {};
  if (!m_scene || ids.empty())
    return false;

  const std::vector<quint32> originals = ids;
  const std::vector<glm::vec3> origBase = basePos;
  const std::vector<DragFaceUv> origUv = baseUv;

  set.prevObjects.assign(m_selectedObjects.begin(), m_selectedObjects.end());
  set.prevFaces.assign(m_selectedFaces.begin(), m_selectedFaces.end());
  set.wholeHosts = m_selectedMods;

  std::vector<quint32> cloneIds;
  std::vector<quint32> cloneSources;
  std::vector<glm::vec3> cloneBase;
  std::vector<DragFaceUv> cloneUv;
  cloneIds.reserve(originals.size());
  cloneBase.reserve(origBase.size());
  cloneUv.reserve(origUv.size());
  const std::vector<eng::FaceTexBasis> kNoBases;

  for (size_t k = 0; k < originals.size(); ++k) {
    const auto src = m_scene->findById(originals[k]);
    if (!src)
      continue;
    const glm::vec3 at = k < origBase.size() ? origBase[k] : src->getPosition();
    const std::vector<eng::FaceTexBasis> &bases = k < origUv.size() ? origUv[k].bases : kNoBases;
    const auto clone = cloneBrush(src, at, bases);
    if (!clone)
      continue;
    set.clones.push_back(clone);
    cloneSources.push_back(originals[k]);
    cloneIds.push_back(clone->stableId());
    cloneBase.push_back(at);
    cloneUv.push_back(k < origUv.size() ? origUv[k] : DragFaceUv {});
  }

  if (cloneIds.empty()) {
    set = {};
    return false;
  }

  // The originals go back where the grab started (mapping included); the copies move from here.
  for (size_t k = 0; k < originals.size(); ++k) {
    const auto obj = m_scene->findById(originals[k]);
    if (obj && k < origBase.size())
      obj->setPosition(origBase[k]);
  }
  applyTextureLockMove(originals, origUv, glm::vec3(0.0f));

  cloneProcObjects(cloneSources, set.clones, set);
  cloneBrushEntities(cloneSources, set.clones, set);
  set.sources = cloneSources;
  m_scene->addObjects(set.clones);
  setSelected(cloneIds, {});
  notifyStructureChanged();
  set.triggered = true;

  ids = std::move(cloneIds);
  basePos = std::move(cloneBase);
  baseUv = std::move(cloneUv);

  qDebug().noquote() << QStringLiteral("[duplicate] %1 brush(es) cloned, originals parked")
                          .arg(set.clones.size());
  return true;
}

bool Engine3D::spawnEntityDuplicates(std::map<quint32, glm::vec3> &entBase, DuplicateSet &set) {
  if (!m_scene || entBase.empty())
    return false;
  std::map<quint32, glm::vec3> copies;
  std::vector<quint32> prev(m_selectedEntities.begin(), m_selectedEntities.end());
  for (const auto &[id, base]: entBase) {
    eng::Entity *e = m_scene->findEntity(id);
    if (!e)
      continue;
    // Back where the grab started: the copies move from here, what cannot be copied stays put.
    e->origin = base;
    const eng::EntityClass *cls = e->cls();
    if (!cls || e->classname == eng::kWorldEnvClass || cls->isBrushEntity())
      continue;
    eng::Entity copy = *e;
    copy.id = m_scene->mintEntityId();
    m_scene->entities.push_back(copy);
    copies[copy.id] = base;
    set.entityClones.push_back(copy.id);
  }
  ++m_entityEpoch;
  emit entitiesChanged();
  if (copies.empty())
    return false;
  set.prevEntities = std::move(prev);
  set.triggered = true;
  std::vector<quint32> sel;
  for (const auto &[id, base]: copies)
    sel.push_back(id);
  setSelectedEntities(sel);
  entBase = std::move(copies);
  return true;
}

void Engine3D::pushDuplicateUndo(DuplicateSet &set, const QString &label) {
  if (set.clones.empty()) {
    set = {};
    return;
  }
  if (m_undoStack && !m_undoStack->isApplying()) {
    const bool extra = set.procAdded || !set.entitiesAdded.empty();
    if (extra) {
      m_undoStack->beginMacro(label);
      if (set.procAdded)
        pushProcTable(label, set.procBefore);
      if (!set.entitiesAdded.empty()) {
        auto apply = [this, ents = set.entitiesAdded](bool add) {
          if (!m_scene)
            return;
          auto &v = m_scene->entities;
          for (const auto &e: ents) {
            v.erase(std::remove_if(v.begin(), v.end(), [&e](const eng::Entity &x) { return x.id == e.id; }), v.end());
            if (add)
              v.push_back(e);
            else
              m_selectedEntities.erase(e.id);
          }
          ++m_entityEpoch;
          emit entitiesChanged();
        };
        m_undoStack->push(std::make_unique<eng::LambdaCommand>(label, [apply] { apply(false); },
                                                               [apply] { apply(true); }));
      }
    }
    auto command = std::make_unique<DuplicateCommand>(this, label);
    command->rememberSelection(set.prevObjects, set.prevFaces);
    for (const auto &clone: set.clones)
      command->addClone(clone);
    m_undoStack->push(std::move(command));
    if (extra)
      m_undoStack->endMacro();
    qDebug().noquote() << QStringLiteral("[duplicate] recorded '%1' (%2 brush(es))").arg(label)
                                                                            .arg(set.clones.size());
  }
  set = {};
}

// Texture lock: U = dot(p, uAxis) * tilesPerUnit + offset, so a move by delta pulls the offset back by
// dot(delta, axis) * tilesPerUnit (correct for any axis direction).
void Engine3D::applyTextureLockMove(const std::vector<quint32> &ids,
                                   const std::vector<DragFaceUv> &base,
                                   const glm::vec3 &delta) const {
  if (!m_textureLock || !m_scene)
    return;

  for (size_t k = 0; k < ids.size() && k < base.size(); ++k) {
    const auto obj = m_scene->findById(ids[k]);
    if (!obj)
      continue;
    const auto mesh = obj->getMesh();
    if (!mesh)
      continue;

    const auto &faces = base[k];
    for (size_t f = 0; f < faces.bases.size() && f < static_cast<size_t>(mesh->faceCount()); ++f) {
      const eng::FaceTexBasis &b = faces.bases[f];
      eng::FaceTexBasis next = b;
      next.offsetU = b.offsetU - glm::dot(delta, b.uAxis) * b.tilesPerUnitU;
      next.offsetV = b.offsetV - glm::dot(delta, b.vAxis) * b.tilesPerUnitV;
      mesh->setFaceTexBasis(static_cast<int>(f), next);
    }
  }
}

void Engine3D::applyFaceUvTransform(const std::vector<quint32> &ids, const std::vector<DragFaceUv> &base,
                                    const glm::dmat4 &m) const {
  if (!m_scene)
    return;
  const glm::dmat3 nm = glm::inverseTranspose(glm::dmat3(m));
  for (size_t k = 0; k < ids.size() && k < base.size(); ++k) {
    const auto obj = m_scene->findById(ids[k]);
    const auto mesh = obj ? obj->getMesh() : nullptr;
    if (!mesh)
      continue;
    const DragFaceUv &faces = base[k];
    for (size_t f = 0; f < faces.bases.size() && f < static_cast<size_t>(mesh->faceCount()); ++f) {
      eng::FaceTexBasis b = faces.bases[f];
      const glm::dvec3 oldN = f < faces.normals.size() ? faces.normals[f] : glm::dvec3(0.0);
      glm::dvec3 newN = nm * oldN;
      const double len = glm::length(newN);
      newN = len > 1e-12 ? newN / len : glm::dvec3(0.0);
      eng::transformFaceBasis(b, m, oldN, newN, m_textureLock);
      mesh->setFaceTexBasis(static_cast<int>(f), b);
    }
  }
}

void Engine3D::restoreFaceUv(const std::vector<quint32> &ids, const std::vector<DragFaceUv> &base) const {
  if (!m_scene)
    return;
  for (size_t k = 0; k < ids.size() && k < base.size(); ++k) {
    const auto obj = m_scene->findById(ids[k]);
    const auto mesh = obj ? obj->getMesh() : nullptr;
    if (!mesh)
      continue;
    const auto &faces = base[k];
    for (size_t f = 0; f < faces.bases.size() && f < static_cast<size_t>(mesh->faceCount()); ++f)
      mesh->setFaceTexBasis(static_cast<int>(f), faces.bases[f]);
  }
}

// Texture scale lock: the texture stretches with the brush. The resize is one affine map
// p_new = C + F * p_old (F the per-world-axis stretch); U_new(p_new) == U_old(p_old) gives
//
//   tilesPerUnit_new = tilesPerUnit / stretchAlong(axis)
//   offset_new       = offset - dot(C, axis) * tilesPerUnit_new
//
// where stretchAlong(axis) = sum_i f_i * axis_i^2.
void Engine3D::applyTextureScaleLock(const std::vector<quint32> &ids,
                                    const std::vector<DragFaceUv> &base, const int axisA,
                                    const double factorA, const int axisB, const double factorB,
                                    const glm::vec3 &anchor) const {
  if (!m_textureScaleLock || !m_scene)
    return;

  auto stretchAlong = [axisA, factorA, axisB, factorB](const glm::vec3 &axis) {
    glm::vec3 f(1.0f);
    if (axisA >= 0 && axisA <= 2)
      f[axisA] = float(factorA);
    if (axisB >= 0 && axisB <= 2)
      f[axisB] = float(factorB);
    return double(glm::dot(f * axis, axis));
  };

  for (size_t k = 0; k < ids.size() && k < base.size(); ++k) {
    const auto obj = m_scene->findById(ids[k]);
    if (!obj)
      continue;
    const auto mesh = obj->getMesh();
    if (!mesh)
      continue;

    const auto &faces = base[k];
    for (size_t f = 0; f < faces.bases.size() && f < static_cast<size_t>(mesh->faceCount()); ++f) {
      const eng::FaceTexBasis &b = faces.bases[f];
      eng::FaceTexBasis next = b;

      const double sU = stretchAlong(b.uAxis);
      const double sV = stretchAlong(b.vAxis);
      if (sU > 1e-6 && std::fabs(sU - 1.0) > 1e-9) {
        next.tilesPerUnitU = float(double(b.tilesPerUnitU) / sU);
        next.offsetU =
            float(double(b.offsetU) - double(glm::dot(anchor, b.uAxis)) * double(next.tilesPerUnitU));
      }
      if (sV > 1e-6 && std::fabs(sV - 1.0) > 1e-9) {
        next.tilesPerUnitV = float(double(b.tilesPerUnitV) / sV);
        next.offsetV =
            float(double(b.offsetV) - double(glm::dot(anchor, b.vAxis)) * double(next.tilesPerUnitV));
      }

      mesh->setFaceTexBasis(static_cast<int>(f), next);
    }
  }
}

// material mode: UV scale and offset of the picked faces (Mesh::setFaceTexBasis is all it takes)

int Engine3D::faceIndexOfPicking(const std::shared_ptr<eng::Mesh> &mesh, const quint32 pickingId) {
  if (!mesh || pickingId == 0)
    return -1;

  const int decoded = int(eng::faceFromPicking(pickingId));
  if (decoded >= 0 && decoded < mesh->faceCount() && mesh->getFacePickingID(decoded) == pickingId)
    return decoded;

  for (int f = 0; f < mesh->faceCount(); ++f)
    if (mesh->getFacePickingID(f) == pickingId)
      return f;
  return -1;
}

bool Engine3D::firstPickedBasis(eng::FaceTexBasis &out) const {
  if (!m_scene)
    return false;

  for (const quint32 pickingId: m_selectedFaces) {
    const auto obj = m_scene->findById(eng::stableIdFromPicking(pickingId));
    if (!obj)
      continue;
    const auto mesh = obj->getMesh();
    const int face = faceIndexOfPicking(mesh, pickingId);
    if (face < 0)
      continue;
    out = mesh->faceTexBasis(face);
    return true;
  }
  return false;
}

void Engine3D::eachPickedBasis(const std::function<void(eng::FaceTexBasis &)> &edit) {
  if (!m_scene)
    return;

  int touched = 0;
  for (const quint32 pickingId: m_selectedFaces) {
    const auto obj = m_scene->findById(eng::stableIdFromPicking(pickingId));
    if (!obj)
      continue;
    const auto mesh = obj->getMesh();
    const int face = faceIndexOfPicking(mesh, pickingId);
    if (face < 0)
      continue;
    eng::FaceTexBasis next = mesh->faceTexBasis(face);
    edit(next);
    mesh->setFaceTexBasis(face, next);
    ++touched;
  }

  qDebug().noquote() << QStringLiteral("[uv] %1 picked face(s) updated").arg(touched);
  if (touched > 0)
    emit uvTransformChanged();
}

double Engine3D::uvScaleU() const {
  eng::FaceTexBasis b;
  if (!firstPickedBasis(b) || b.tilesPerUnitU <= 1e-6f)
    return 0.0;
  return vk::GridView::kUnitsPerWorldUnit / double(b.tilesPerUnitU);
}

double Engine3D::uvScaleV() const {
  eng::FaceTexBasis b;
  if (!firstPickedBasis(b) || b.tilesPerUnitV <= 1e-6f)
    return 0.0;
  return vk::GridView::kUnitsPerWorldUnit / double(b.tilesPerUnitV);
}

double Engine3D::uvOffsetU() const {
  eng::FaceTexBasis b;
  return firstPickedBasis(b) ? double(b.offsetU) : 0.0;
}

double Engine3D::uvOffsetV() const {
  eng::FaceTexBasis b;
  return firstPickedBasis(b) ? double(b.offsetV) : 0.0;
}

void Engine3D::setUvScaleU(const double scale) {
  if (scale <= 1e-6)
    return;
  const float tiles = float(vk::GridView::kUnitsPerWorldUnit / scale);
  eachPickedBasis([tiles](eng::FaceTexBasis &b) { b.tilesPerUnitU = tiles; });
}

void Engine3D::setUvScaleV(const double scale) {
  if (scale <= 1e-6)
    return;
  const float tiles = float(vk::GridView::kUnitsPerWorldUnit / scale);
  eachPickedBasis([tiles](eng::FaceTexBasis &b) { b.tilesPerUnitV = tiles; });
}

void Engine3D::setUvOffsetU(const double offset) {
  const float tiles = float(offset);
  eachPickedBasis([tiles](eng::FaceTexBasis &b) { b.offsetU = tiles; });
}

void Engine3D::setUvOffsetV(const double offset) {
  const float tiles = float(offset);
  eachPickedBasis([tiles](eng::FaceTexBasis &b) { b.offsetV = tiles; });
}

void Engine3D::updateResize(const glm::dvec2 &curPlane) {
  bool dragA0 = false;
  bool dragA1 = false;
  bool dragB0 = false;
  bool dragB1 = false;
  handleSides(m_dragHandle, dragA0, dragA1, dragB0, dragB1);

  const double minSize = m_dragStep;

  // Snap where the dragged side ends up, not the increment; the other side stays where it was, off-grid included.
  double a0 = m_dragBox.x;
  double a1 = m_dragBox.z;
  double b0 = m_dragBox.y;
  double b1 = m_dragBox.w;

  if (dragA0)
    a0 = snapTo(m_dragBox.x + (curPlane.x - m_dragGrab.x), m_dragStep);
  if (dragA1)
    a1 = snapTo(m_dragBox.z + (curPlane.x - m_dragGrab.x), m_dragStep);
  if (dragB0)
    b0 = snapTo(m_dragBox.y + (curPlane.y - m_dragGrab.y), m_dragStep);
  if (dragB1)
    b1 = snapTo(m_dragBox.w + (curPlane.y - m_dragGrab.y), m_dragStep);

  if (dragA0)
    a0 = std::min(a0, m_dragBox.z - minSize);
  if (dragA1)
    a1 = std::max(a1, m_dragBox.x + minSize);
  if (dragB0)
    b0 = std::min(b0, m_dragBox.w - minSize);
  if (dragB1)
    b1 = std::max(b1, m_dragBox.y + minSize);

  if (m_dragTarget == DragTarget::ResizeMarquee)
    applyMarqueeResize(a0, b0, a1, b1);
  else
    applyMeshResize(a0, b0, a1, b1);
}

// The marquee keeps its depth span and is re-expressed in the plane being dragged from.
void Engine3D::applyMarqueeResize(const double a0, const double b0, const double a1,
                                 const double b1) {
  const ViewBasis bs = viewBasisFor(m_dragPlane);
  glm::dvec3 mn {};
  glm::dvec3 mx {};
  marqueeToWorld(m_marquee, mn, mx);

  double d0 = 0.0;
  double d1 = 0.0;
  projectOntoAxis(bs.depth, mn, mx, d0, d1);

  m_marquee.plane = m_dragPlane;
  m_marquee.a0 = a0;
  m_marquee.a1 = a1;
  m_marquee.b0 = b0;
  m_marquee.b1 = b1;
  m_marquee.d0 = d0;
  m_marquee.d1 = d1;
  m_marquee.valid = true;
  emit marqueeChanged();
}

// One affine map of the selection's bounding box onto the new one, applied to every mesh's position and scale.
void Engine3D::applyMeshResize(const double a0, const double b0, const double a1, const double b1) {
  if (!m_scene)
    return;

  const ViewBasis bs = viewBasisFor(m_dragPlane);
  const int axisA = std::fabs(bs.right.x) > 0.5 ? 0 : (std::fabs(bs.right.y) > 0.5 ? 1 : 2);
  const int axisB = std::fabs(bs.up.x) > 0.5 ? 0 : (std::fabs(bs.up.y) > 0.5 ? 1 : 2);
  const double signA = bs.right[axisA];
  const double signB = bs.up[axisB];

  const double oldA = m_dragBox.z - m_dragBox.x;
  const double oldB = m_dragBox.w - m_dragBox.y;
  const double factorA = oldA > 1e-6 ? (a1 - a0) / oldA : 1.0;
  const double factorB = oldB > 1e-6 ? (b1 - b0) / oldB : 1.0;

  // A negative basis component mirrors the axis (low and high ends swap).
  const double newMinA = signA > 0.0 ? a0 : -a1;
  const double newMinB = signB > 0.0 ? b0 : -b1;
  const double baseMinA = m_resizeBaseMin[axisA];
  const double baseMinB = m_resizeBaseMin[axisB];

  for (size_t k = 0; k < m_dragObjects.size(); ++k) {
    auto *obj = m_scene->findById(m_dragObjects[k]).get();
    if (!obj)
      continue;
    const glm::vec3 basePos = k < m_dragBasePos.size() ? m_dragBasePos[k] : glm::vec3(0.0f);
    glm::vec3 scale = k < m_dragBaseScale.size() ? m_dragBaseScale[k] : glm::vec3(1.0f);

    glm::vec3 pos = basePos;
    pos[axisA] = float(newMinA + (basePos[axisA] - baseMinA) * factorA);
    pos[axisB] = float(newMinB + (basePos[axisB] - baseMinB) * factorB);
    scale[axisA] = float(scale[axisA] * factorA);
    scale[axisB] = float(scale[axisB] * factorB);

    obj->setPosition(pos);
    obj->setScale(scale);
  }

  // C = newMin - f*baseMin, for the scale lock.
  glm::vec3 anchor(0.0f);
  anchor[axisA] = float(newMinA - factorA * baseMinA);
  anchor[axisB] = float(newMinB - factorB * baseMinB);

  applyTextureScaleLock(m_dragObjects, m_dragBaseUv, axisA, factorA, axisB, factorB, anchor);

  emit sceneChanged();
}

void Engine3D::buildMarqueeGeometry() {
  m_marqueeVertexCount = 0;
  m_marqueeVerts.clear();
  if (!m_marquee.valid || !m_marqueeVb)
    return;

  std::vector<std::pair<glm::dvec3, glm::dvec3>> boxes(1);
  marqueeToWorld(m_marquee, boxes[0].first, boxes[0].second);

  static const int edges[12][2] = {
      {0, 1}, {1, 3}, {3, 2}, {2, 0},
      {4, 5}, {5, 7}, {7, 6}, {6, 4},
      {0, 4}, {1, 5}, {2, 6}, {3, 7},
  };

  const glm::vec3 cam = m_camera->getPosition();
  const float viewH = float(std::max(1, m_lastViewSize.height()));
  const float halfFov = 0.5f * m_camera->getFov() * 3.14159265f / 180.0f;
  const float worldPerPx = (2.0f * std::tan(halfFov)) / viewH;

  m_marqueeVerts.clear();
  for (const auto &[mn, mx]: boxes) {
    const glm::vec3 c[8] = {
        {float(mn.x), float(mn.y), float(mn.z)},
        {float(mx.x), float(mn.y), float(mn.z)},
        {float(mn.x), float(mx.y), float(mn.z)},
        {float(mx.x), float(mx.y), float(mn.z)},
        {float(mn.x), float(mn.y), float(mx.z)},
        {float(mx.x), float(mn.y), float(mx.z)},
        {float(mn.x), float(mx.y), float(mx.z)},
        {float(mx.x), float(mx.y), float(mx.z)},
    };
    for (const auto &e: edges) {
      const glm::vec3 &a = c[e[0]];
      const glm::vec3 &b = c[e[1]];
      const glm::vec3 dir = b - a;
      const float len = glm::length(dir);
      if (len < 1e-6f)
        continue;

      const glm::vec3 mid = (a + b) * 0.5f;
      const glm::vec3 toCam = cam - mid;
      const float dist = std::max(glm::length(toCam), 1e-3f);
      glm::vec3 side = glm::cross(dir / len, toCam / dist);
      if (glm::length(side) < 1e-5f)
        side = glm::vec3(0.0f, 1.0f, 0.0f);
      side = glm::normalize(side) * (worldPerPx * dist);

      const glm::vec3 p0 = a + side;
      const glm::vec3 p1 = a - side;
      const glm::vec3 p2 = b - side;
      const glm::vec3 p3 = b + side;
      m_marqueeVerts << p0.x << p0.y << p0.z << p1.x << p1.y << p1.z << p2.x << p2.y << p2.z
                    << p0.x << p0.y << p0.z << p2.x << p2.y << p2.z << p3.x << p3.y << p3.z;
    }
  }

  const int bytes = static_cast<int>(m_marqueeVerts.size() * sizeof(float));
  if (bytes == 0)
    return;

  QRhi *rhi = Shared::instance().rhi();
  QRhiResourceUpdateBatch *mb = rhi->nextResourceUpdateBatch();
  mb->updateDynamicBuffer(m_marqueeVb, 0, bytes, m_marqueeVerts.data());
  m_marqueeBatch = mb;
  m_marqueeVertexCount = static_cast<quint32>(m_marqueeVerts.size() / 3);
}

void Engine3D::drawMarquee(QRhiCommandBuffer *cb) {
  if (!m_marqueeVertexCount || !m_marqueePipeline || !m_marqueeSrb)
    return;

  if (m_marqueeBatch) {
    cb->resourceUpdate(m_marqueeBatch);
    m_marqueeBatch = nullptr;
  }

  cb->setGraphicsPipeline(m_marqueePipeline);
  const QRhiCommandBuffer::VertexInput vin(m_marqueeVb, 0);
  cb->setVertexInput(0, 1, &vin);
  cb->setShaderResources(m_marqueeSrb);
  cb->draw(m_marqueeVertexCount);
}

// The preview box is the opening clipped to the material it goes through.
void Engine3D::buildCutOverlayGeometry() {
  m_cutWireVertexCount = 0;
  m_cutFillVertexCount = 0;
  m_cutWireVerts.clear();
  if ((m_cutMode == CutMode::Off && !m_bevel.open) || !m_cutWireVb)
    return;

  glm::dvec3 selMin {};
  glm::dvec3 selMax {};
  if (m_cutMode != CutMode::Off && !selectionWorldAABB(selMin, selMax))
    return;

  const glm::vec3 cam = m_camera->getPosition();
  const float viewH = float(std::max(1, m_lastViewSize.height()));
  const float halfFov = 0.5f * m_camera->getFov() * 3.14159265f / 180.0f;
  const float worldPerPx = (2.0f * std::tan(halfFov)) / viewH;

  // A camera-facing strip two pixels wide.
  auto edge = [&](const glm::vec3 &a, const glm::vec3 &b) {
    const glm::vec3 dir = b - a;
    const float len = glm::length(dir);
    if (len < 1e-6f)
      return;
    const glm::vec3 mid = (a + b) * 0.5f;
    const glm::vec3 toCam = cam - mid;
    const float dist = std::max(glm::length(toCam), 1e-3f);
    glm::vec3 side = glm::cross(dir / len, toCam / dist);
    if (glm::length(side) < 1e-5f)
      side = glm::vec3(0.0f, 1.0f, 0.0f);
    side = glm::normalize(side) * (worldPerPx * dist);
    const glm::vec3 p0 = a + side, p1 = a - side, p2 = b - side, p3 = b + side;
    m_cutWireVerts << p0.x << p0.y << p0.z << p1.x << p1.y << p1.z << p2.x << p2.y << p2.z
                   << p0.x << p0.y << p0.z << p2.x << p2.y << p2.z << p3.x << p3.y << p3.z;
  };

  std::vector<glm::vec3> fill;
  if (m_cutMode == CutMode::Off) {
    appendBevelOverlay(edge, fill);
  } else if (isCutout()) {
    static const int edges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
    };
    for (const CutHole &hole: m_cutHoles) {
      glm::dvec3 hmn {};
      glm::dvec3 hmx {};
      cutHoleWorld(hole, hmn, hmx);
      const glm::dvec3 mn = glm::max(hmn, selMin);
      const glm::dvec3 mx = glm::min(hmx, selMax);
      if (mx.x <= mn.x || mx.y <= mn.y || mx.z <= mn.z)
        continue;
      const glm::vec3 c[8] = {
          {float(mn.x), float(mn.y), float(mn.z)}, {float(mx.x), float(mn.y), float(mn.z)},
          {float(mn.x), float(mx.y), float(mn.z)}, {float(mx.x), float(mx.y), float(mn.z)},
          {float(mn.x), float(mn.y), float(mx.z)}, {float(mx.x), float(mn.y), float(mx.z)},
          {float(mn.x), float(mx.y), float(mx.z)}, {float(mx.x), float(mx.y), float(mx.z)},
      };
      for (const auto &e: edges)
        edge(c[e[0]], c[e[1]]);
    }
  } else if (m_cutLine.valid) {
    // The cut plane in world space, as applyCut reads it.
    const ViewBasis vb = viewBasisFor(m_cutLine.plane);
    glm::dvec3 n;
    double k;
    if (m_cutLine.angled) {
      const glm::dvec2 d = m_cutLine.b - m_cutLine.a;
      const glm::dvec3 along = glm::normalize(vb.right * d.x + vb.up * d.y);
      n = glm::normalize(glm::cross(along, vb.depth));
      k = glm::dot(n, vb.right * m_cutLine.a.x + vb.up * m_cutLine.a.y);
    } else {
      n = m_cutLine.alongRight ? vb.up : vb.right;
      k = m_cutLine.k;
    }
    // Grow the box a little so a cut on its face still shows.
    const glm::dvec3 pad(0.01);
    eng::ConvexBrush box = eng::ConvexBrush::box(selMin - pad, selMax + pad, {}, 1.0);
    // The cut face: the box clipped to the back of the plane, its cap is the cut.
    eng::ConvexBrush back = box;
    eng::BrushFace cap;
    cap.normal = n;
    cap.distance = k;
    if (back.clip(cap))
      for (size_t i = 0; i < back.faces.size() && i < back.polygons.size(); ++i) {
        if (glm::dot(back.faces[i].normal, n) < 0.999 || std::fabs(back.faces[i].distance - k) > 1e-6)
          continue;
        const auto &poly = back.polygons[i];
        for (size_t j = 0; j < poly.size(); ++j)
          edge(glm::vec3(poly[j]), glm::vec3(poly[(j + 1) % poly.size()]));
      }
    // The removed part: positive side for Positive, negative for Negative.
    if (m_cutMode != CutMode::KeepBoth) {
      eng::ConvexBrush gone = box;
      eng::BrushFace keep;
      keep.normal = m_cutMode == CutMode::Positive ? -n : n;
      keep.distance = m_cutMode == CutMode::Positive ? -k : k;
      if (gone.clip(keep))
        for (const auto &poly: gone.polygons)
          for (size_t j = 1; j + 1 < poly.size(); ++j) {
            fill.push_back(glm::vec3(poly[0]));
            fill.push_back(glm::vec3(poly[j]));
            fill.push_back(glm::vec3(poly[j + 1]));
          }
    }
  }

  m_cutWireVertexCount = static_cast<quint32>(m_cutWireVerts.size() / 3);
  for (const glm::vec3 &p: fill)
    m_cutWireVerts << p.x << p.y << p.z;
  m_cutFillVertexCount = static_cast<quint32>(fill.size());
  const int bytes = static_cast<int>(m_cutWireVerts.size() * sizeof(float));
  if (bytes == 0 || bytes > int(m_cutWireVb->size())) {
    m_cutWireVertexCount = m_cutFillVertexCount = 0;
    return;
  }

  QRhi *rhi = Shared::instance().rhi();
  if (!rhi)
    return;
  QRhiResourceUpdateBatch *mb = rhi->nextResourceUpdateBatch();
  mb->updateDynamicBuffer(m_cutWireVb, 0, bytes, m_cutWireVerts.data());
  m_cutWireBatch = mb;
}

void Engine3D::drawCutOverlay(QRhiCommandBuffer *cb) {
  if (!m_cutWireSrb)
    return;
  const QRhiCommandBuffer::VertexInput vin(m_cutWireVb, 0);
  if (m_cutFillVertexCount && m_cutFillPipeline) {
    cb->setGraphicsPipeline(m_cutFillPipeline);
    cb->setVertexInput(0, 1, &vin);
    cb->setShaderResources(m_cutWireSrb);
    cb->draw(m_cutFillVertexCount, 1, m_cutWireVertexCount);
  }
  if (m_cutWireVertexCount && m_cutWirePipeline) {
    cb->setGraphicsPipeline(m_cutWirePipeline);
    cb->setVertexInput(0, 1, &vin);
    cb->setShaderResources(m_cutWireSrb);
    cb->draw(m_cutWireVertexCount);
  }
}

// A facade's `plane` is the unsigned coordinate along its axis: the point on it is axis unit * plane, not normal *
// plane.
static glm::vec3 fillAxisUnit(const int axis) {
  return axis == 0 ? glm::vec3(1.0f, 0.0f, 0.0f)
                  : (axis == 1 ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f));
}

// One quad per layer band per facade, drawn through the selection wire pipeline. The wire colour is a UBO value,
// so wall and space bands are two contiguous index runs, drawn as two draws.
void Engine3D::buildFillOverlayGeometry() {
  m_fillWireWallIdxCount = 0;
  m_fillWireSpaceIdxCount = 0;
  m_fillWireVerts.clear();
  m_fillWireIdx.clear();
  if (!m_materialMode || !m_fillWireVisible || m_fillLayers.empty())
    return;
  if (!m_fillWireVb || !m_fillWireIb || !m_layerOverlayWallUbo || !m_layerOverlaySpaceUbo)
    return;
  if (m_fillFacades.empty())
    return;

  const glm::vec3 up(0.0f, 1.0f, 0.0f);

  // Hides the quad's diagonal: the opposite corner's barycentric stays 1 (as Mesh::rebuildWireframe).
  auto addBand = [&](const eng::Facade &fac, const float u0, const float u1,
                    const float y0, const float y1) {
    const glm::vec3 base = fillAxisUnit(fac.axis) * float(fac.plane);
    const glm::vec3 u = fac.uDir;
    const auto at = [&](const float uu, const float yy) { return base + u * uu + up * yy; };
    const quint32 a = quint32(m_fillWireVerts.size());
    m_fillWireVerts.push_back({at(u0, y0), glm::vec3(1.0f, 1.0f, 0.0f), up});
    m_fillWireVerts.push_back({at(u1, y0), glm::vec3(0.0f, 1.0f, 0.0f), up});
    m_fillWireVerts.push_back({at(u1, y1), glm::vec3(0.0f, 1.0f, 1.0f), up});
    m_fillWireVerts.push_back({at(u0, y1), glm::vec3(0.0f, 0.0f, 1.0f), up});
    m_fillWireIdx.insert(m_fillWireIdx.end(), {a, a + 1, a + 2, a, a + 2, a + 3});
  };

  for (int pass = 0; pass < 2; ++pass) {
    const bool wantSpace = pass == 1;
    const quint32 first = quint32(m_fillWireIdx.size());
    for (const auto &fac: m_fillFacades) {
      for (size_t i = 0; i < m_fillLayers.size(); ++i) {
        if ((m_fillLayers[i].kind == eng::LayerKind::Space) != wantSpace)
          continue;
        // As wide as the wall at that height, not the facade's whole run (a plinth can reach further).
        double lo = 0.0;
        double hi = 0.0;
        if (!fac.uRangeAt(m_fillLayers[i].y0, m_fillLayers[i].y1, lo, hi))
          continue;
        if (std::fabs(hi - lo) < 1e-4)
          continue;
        addBand(fac, float(lo), float(hi), float(m_fillLayers[i].y0), float(m_fillLayers[i].y1));
      }
    }
    const quint32 n = quint32(m_fillWireIdx.size()) - first;
    if (wantSpace)
      m_fillWireSpaceIdxCount = n;
    else
      m_fillWireWallIdxCount = n;
  }

  const quint32 vBytes = quint32(m_fillWireVerts.size() * sizeof(eng::WireVertex));
  const quint32 iBytes = quint32(m_fillWireIdx.size() * sizeof(uint32_t));
  if (vBytes == 0 || iBytes == 0)
    return;

  QRhi *rhi = Shared::instance().rhi();
  if (!rhi)
    return;

  if (vBytes > m_fillWireCapacity) {
    if (m_fillWireVb)
      Shared::instance().retire(m_fillWireVb);
    const quint32 cap = vBytes * 2;
    auto *vb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, int(cap));
    if (!vb->create()) {
      qWarning(engLog, "layer overlay vertex buffer grow failed");
      delete vb;
      m_fillWireVb = nullptr;
      m_fillWireCapacity = 0;
      return;
    }
    m_fillWireVb = vb;
    m_fillWireCapacity = cap;
  }
  if (iBytes > m_fillWireIbCapacity) {
    if (m_fillWireIb)
      Shared::instance().retire(m_fillWireIb);
    const quint32 cap = iBytes * 2;
    auto *ib = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::IndexBuffer, int(cap));
    if (!ib->create()) {
      qWarning(engLog, "layer overlay index buffer grow failed");
      delete ib;
      m_fillWireIb = nullptr;
      m_fillWireIbCapacity = 0;
      return;
    }
    m_fillWireIb = ib;
    m_fillWireIbCapacity = cap;
  }

  QRhiResourceUpdateBatch *mb = rhi->nextResourceUpdateBatch();
  const ObjectBlock identity;
  mb->updateDynamicBuffer(m_layerObjUbo, 0, sizeof(ObjectBlock), &identity);
  mb->updateDynamicBuffer(m_fillWireVb, 0, int(vBytes), m_fillWireVerts.data());
  mb->updateDynamicBuffer(m_fillWireIb, 0, int(iBytes), m_fillWireIdx.data());

  // Depth testing is off: no push toward the eye.
  OverlayUniforms wallU;
  wallU.wireColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
  // Twice the selection wire's thickness.
  wallU.wireParams = glm::vec4(m_wireframeThicknessPx * 2.0f, 0.0f, 6.0f, 0.12f);
  mb->updateDynamicBuffer(m_layerOverlayWallUbo, 0, sizeof(OverlayUniforms), &wallU);

  OverlayUniforms spaceU = wallU;
  spaceU.wireColor = glm::vec4(0.20f, 0.90f, 0.32f, 1.0f);
  mb->updateDynamicBuffer(m_layerOverlaySpaceUbo, 0, sizeof(OverlayUniforms), &spaceU);

  m_fillWireBatch = mb;
}

// Two draws (the wire colour is a UBO value): wall bands, then the space bands after them in the index buffer.
void Engine3D::drawFillOverlay(QRhiCommandBuffer *cb) {
  if (!m_wirePipeline || !m_fillWireVb || !m_fillWireIb)
    return;
  if (!m_layerSrbWall || !m_layerSrbSpace)
    return;
  const quint32 total = m_fillWireWallIdxCount + m_fillWireSpaceIdxCount;
  if (total == 0)
    return;

  if (m_fillWireBatch) {
    cb->resourceUpdate(m_fillWireBatch);
    m_fillWireBatch = nullptr;
  }

  cb->setGraphicsPipeline(m_wirePipeline.get());
  const QRhiCommandBuffer::VertexInput vin(m_fillWireVb, 0);
  const QRhiCommandBuffer::DynamicOffset identity(1, 0);
  cb->setVertexInput(0, 1, &vin, m_fillWireIb, 0, QRhiCommandBuffer::IndexUInt32);

  if (m_fillWireWallIdxCount) {
    cb->setShaderResources(m_layerSrbWall, 1, &identity);
    cb->drawIndexed(m_fillWireWallIdxCount, 1, 0, 0);
  }
  if (m_fillWireSpaceIdxCount) {
    cb->setShaderResources(m_layerSrbSpace, 1, &identity);
    cb->drawIndexed(m_fillWireSpaceIdxCount, 1, m_fillWireWallIdxCount, 0);
  }
}

// One 'wall' / 'space' label per layer band per facade, centred on the part that carries the band.
int Engine3D::buildFillLayerLabels() {
  if (!m_materialMode || !m_text || m_fillLayers.empty())
    return 0;

  const glm::mat4 viewProj = m_sceneUniforms.viewProj;
  const glm::vec3 camPos = m_camera->getPosition();
  const float viewW = float(std::max(1, m_lastViewSize.width()));
  const float viewH = float(std::max(1, m_lastViewSize.height()));

  auto projectToScreen = [&](const glm::vec3 &p) -> glm::vec2 {
    const glm::vec4 clip = viewProj * glm::vec4(p, 1.0f);
    if (std::fabs(clip.w) <= 1e-6f)
      return glm::vec2(0.0f);
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return glm::vec2((ndc.x * 0.5f + 0.5f) * viewW, (ndc.y * 0.5f + 0.5f) * viewH);
  };

  static const QColor kText(255, 255, 255);

  int drawn = 0;
  for (const auto &fac: m_fillFacades) {
    for (const FillLayer &l: m_fillLayers) {
      double lo = 0.0;
      double hi = 0.0;
      if (!fac.uRangeAt(l.y0, l.y1, lo, hi))
        continue;

      const glm::vec3 nrm = fac.normal;
      const double vCenter = (l.y0 + l.y1) * 0.5;

      // The sprite runs top-to-bottom along vAxis: choose the axes from the projection so the word is upright.
      glm::vec3 uAxis = fac.uDir;
      glm::vec3 vAxis(0.0f, 1.0f, 0.0f);

      const glm::vec3 base = uAxis * float((lo + hi) * 0.5) + vAxis * float(vCenter)
          + fillAxisUnit(fac.axis) * float(fac.plane);

      if (projectToScreen(base + uAxis).x < projectToScreen(base).x)
        uAxis = -uAxis;
      if (projectToScreen(base + vAxis).y > projectToScreen(base).y)
        vAxis = -vAxis;

      const float pxU = glm::length(projectToScreen(base + uAxis) - projectToScreen(base));
      const float pxV = glm::length(projectToScreen(base + vAxis) - projectToScreen(base));
      if (pxU <= 1e-3f || pxV <= 1e-3f)
        continue;

      const QString label = l.kind == eng::LayerKind::Space ? QStringLiteral("space")
                                                         : QStringLiteral("wall");
      const QSize sprite = m_text->size(label);
      if (sprite.isEmpty())
        continue;

      const float w = float(sprite.width()) / pxU;
      const float h = float(sprite.height()) / pxV;
      if (w <= 0.0f || h <= 0.0f)
        continue;

      // Just off the wall toward the viewer (no z-fighting).
      const float lift = glm::dot(camPos - base, nrm) >= 0.0f ? 0.06f : -0.06f;
      const glm::vec3 origin = base + nrm * lift - uAxis * (w * 0.5f) - vAxis * (h * 0.5f);

      m_text->drawDecal(label, origin, uAxis, vAxis, w, h, kText, 1.0f);
      ++drawn;
    }
  }
  return drawn;
}

// entity markers and the 3D translate gizmo: one vertex pool, a model matrix per draw, main pass only

void Engine3D::releaseEntityResources(const bool retire) {
  if (retire) {
    if (m_entityVb)
      Shared::instance().retire(m_entityVb);
    if (m_entityUbo)
      Shared::instance().retire(m_entityUbo);
  } else {
    delete m_entityVb;
    delete m_entityUbo;
  }
  m_entityVb = nullptr;
  m_entityUbo = nullptr;
  m_entityCapacity = 0;
}

void Engine3D::ensureEntityResources(QRhiResourceUpdateBatch *rub) {
  QRhi *rhi = Shared::instance().rhi();
  if (!rhi || !rub || noEntities())
    return;

  m_entityStride = rhi->ubufAligned(sizeof(EntityUniforms));
  ensureIconAtlas(rub);

  if (!m_entityGeomBuilt) {
    std::vector<glm::vec3> verts;
    verts.reserve(2400);

    const size_t boxStart = verts.size();
    buildBoxTriangles(verts, 2.0f);
    m_geoMarkerBox = {quint32(boxStart), quint32(verts.size() - boxStart)};

    const size_t sphereStart = verts.size();
    buildSphereLines(verts, 12, 24);
    m_geoRangeSphere = {quint32(sphereStart), quint32(verts.size() - sphereStart)};

    // Spot light cone: rim circle of radius 1 at z = 1, eight edges from the apex.
    {
      const size_t start = verts.size();
      constexpr int kRim = 48;
      auto rim = [](int k) {
        const float t = 2.0f * 3.14159265358979f * float(k) / float(kRim);
        return glm::vec3(std::cos(t), std::sin(t), 1.0f);
      };
      for (int k = 0; k < kRim; ++k) {
        verts.push_back(rim(k));
        verts.push_back(rim(k + 1));
      }
      for (int k = 0; k < kRim; k += kRim / 8) {
        verts.push_back(glm::vec3(0.0f));
        verts.push_back(rim(k));
      }
      m_geoSpotCone = {quint32(start), quint32(verts.size() - start)};
    }

    // Camera3D frustum: apex at the origin looking down -Z, far rectangle at z = -1, Godot's "up" triangle.
    {
      const size_t start = verts.size();
      const glm::vec3 apex(0.0f), c[4] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}};
      for (int i = 0; i < 4; ++i)
        verts.insert(verts.end(), {apex, c[i], c[i], c[(i + 1) % 4]});
      const glm::vec3 t0(-0.45f, 1.15f, -1.0f), t1(0.45f, 1.15f, -1.0f), t2(0.0f, 1.6f, -1.0f);
      verts.insert(verts.end(), {t0, t1, t1, t2, t2, t0});
      m_geoFrustum = {quint32(start), quint32(verts.size() - start)};
    }

    // One revolved arrow per axis (separate ranges, highlighted separately).
    const std::vector<std::pair<float, float>> arrowProfile = {
        {0.0f, 0.0f},
        {0.0f, kArrowShaftRadius},
        {kArrowShaftEnd, kArrowShaftRadius},
        {kArrowShaftEnd, kArrowTipRadius},
        {kArrowTipEnd, 0.0f},
    };
    for (int i = 0; i < 3; ++i) {
      const size_t start = verts.size();
      revolveProfile(verts, unitAxis(i), arrowProfile, kArrowSides);
      m_geoArrow[i] = {quint32(start), quint32(verts.size() - start)};
    }
    for (int i = 0; i < 3; ++i) {
      const size_t start = verts.size();
      buildPlaneHandle(verts, unitAxis((i + 1) % 3), unitAxis((i + 2) % 3));
      m_geoPlane[i] = {quint32(start), quint32(verts.size() - start)};
    }
    // Rotation rings: thin tori of radius 1 around each axis.
    for (int i = 0; i < 3; ++i) {
      const size_t start = verts.size();
      const glm::vec3 ax = unitAxis(i), e1 = unitAxis((i + 1) % 3), e2 = unitAxis((i + 2) % 3);
      const int major = 72, minor = 6;
      const float tube = 0.022f;
      auto at = [&](int a, int b) {
        const float t = 2.0f * 3.14159265f * float(a) / float(major);
        const float p = 2.0f * 3.14159265f * float(b) / float(minor);
        const glm::vec3 radial = e1 * std::cos(t) + e2 * std::sin(t);
        return radial * (1.0f + tube * std::cos(p)) + ax * (tube * std::sin(p));
      };
      for (int a = 0; a < major; ++a)
        for (int b = 0; b < minor; ++b) {
          const glm::vec3 p00 = at(a, b), p10 = at(a + 1, b), p01 = at(a, b + 1), p11 = at(a + 1, b + 1);
          verts.insert(verts.end(), {p00, p10, p11, p00, p11, p01});
        }
      m_geoRing[i] = {quint32(start), quint32(verts.size() - start)};
    }

    const int bytes = static_cast<int>(verts.size() * sizeof(glm::vec3));
    auto *vb = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, bytes);
    if (!vb->create()) {
      qWarning(engLog, "entity: vertex buffer allocation failed (%d bytes)", bytes);
      delete vb;
      return;
    }
    rub->updateDynamicBuffer(vb, 0, bytes, verts.data());
    m_entityVb = vb;
    m_entityGeomBuilt = true;
  }

  if (!m_entityUbo) {
    m_entityCapacity = kEntityCapacity;
    auto *ubo = rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                              int(m_entityCapacity * m_entityStride));
    if (!ubo->create()) {
      qWarning(engLog, "entity: uniform buffer allocation failed (%d bytes)",
               int(m_entityCapacity * m_entityStride));
      delete ubo;
      m_entityCapacity = 0;
      return;
    }
    m_entityUbo = ubo;
    // Every SRB binds this buffer.
    ++m_bufferGen;
  }
}

void Engine3D::buildEntityDraws(QRhiResourceUpdateBatch *rub) {
  m_entityDraws.clear();
  if (noEntities() || !m_entityGeomBuilt || !m_entityUbo || !rub)
    return;

  auto add = [this](const EntityGeom &g, const EntityPass pass, const glm::mat4 &model,
                   const glm::vec4 &color, const glm::vec4 &info = glm::vec4(0.0f)) {
    if (g.count == 0 || m_entityDraws.size() >= static_cast<size_t>(m_entityCapacity))
      return;
    EntityDraw d;
    d.vbOffset = g.offset;
    d.vbCount = g.count;
    d.pass = pass;
    d.model = model;
    d.color = color;
    d.info = info;
    m_entityDraws.push_back(d);
  };

  // Entities are icons in 3D (vk_billboards.cpp); the 2D views draw boxes.
  appendBillboardDraws(entityMarkers());

  // Edit-mode handles in Hammer colours.
  if (m_editMode) {
    for (const EditHandleDraw &h: editHandleDraws()) {
      const float base = gizmoScale(h.pos) * 0.035f;
      glm::vec4 col(0.92f, 0.95f, 1.0f, 0.95f);
      float sz = base;
      switch (EditHandleKind(h.kind)) {
        case EditHandleKind::Edge: col = glm::vec4(1.0f, 0.86f, 0.2f, 0.95f); sz = base * 0.85f; break;
        case EditHandleKind::Face: col = glm::vec4(0.3f, 0.9f, 1.0f, 0.95f); sz = base * 0.9f; break;
        case EditHandleKind::Centre: col = glm::vec4(0.85f, 0.85f, 0.85f, 0.55f); sz = base * 0.7f; break;
        case EditHandleKind::Pivot: col = glm::vec4(1.0f, 0.3f, 0.9f, 1.0f); sz = base * 1.3f; break;
        default: break;
      }
      if (h.selected && EditHandleKind(h.kind) != EditHandleKind::Pivot)
        col = glm::vec4(1.0f, 0.22f, 0.18f, 1.0f);
      const glm::mat4 m = glm::translate(glm::mat4(1.0f), h.pos) * glm::scale(glm::mat4(1.0f), glm::vec3(sz));
      add(m_geoMarkerBox, EntityPass::TriOverlay, m, col);
    }
  }

  if (!noEntities())
    addCameraFrustumDraws([&add](const EntityGeom &g, const glm::mat4 &m, const glm::vec4 &c) {
      add(g, EntityPass::Line, m, c);
    });

  // Selected lights' range: a sphere, or a spot light's cone.
  for (const EntityMarker &marker: entityMarkers()) {
    if (marker.kind != EntityKind::Generic || !marker.selected || marker.range <= 0.0f || noRangeSphere())
      continue;
    if (marker.coneAngle > 0.0f) {
      const float a = glm::radians(marker.coneAngle);
      const glm::mat4 cone = glm::translate(glm::mat4(1.0f), marker.position) * glm::mat4(basisAlong(marker.direction))
          * glm::scale(glm::mat4(1.0f), glm::vec3(glm::vec2(marker.range * std::sin(a)), marker.range * std::cos(a)));
      add(m_geoSpotCone, EntityPass::Line, cone, glm::vec4(glm::vec3(marker.color), 0.45f),
          glm::vec4(marker.range, 0.0f, 0.0f, 0.0f));
      continue;
    }
    const glm::mat4 sphere = glm::translate(glm::mat4(1.0f), marker.position)
        * glm::scale(glm::mat4(1.0f), glm::vec3(std::max(0.5f, marker.range)));
    add(m_geoRangeSphere, EntityPass::Line, sphere, glm::vec4(glm::vec3(marker.color), 0.45f),
        glm::vec4(marker.range, 0.0f, 0.0f, 0.0f));
  }

  if (gizmoVisible() && !noGizmo() && m_gizmoMode != GizmoMode::Translate) {
    addGizmoModeDraws([&add](const EntityGeom &g, int pass, const glm::mat4 &m, const glm::vec4 &c) {
      add(g, static_cast<EntityPass>(pass), m, c);
    });
  } else if (gizmoVisible() && !noGizmo()) {
    const glm::vec3 center = gizmoCenter();
    const float s = gizmoScale(center);
    const glm::mat4 base = glm::translate(glm::mat4(1.0f), center)
        * glm::scale(glm::mat4(1.0f), glm::vec3(s));
    // Plane handles get the camera-facing mirror; must match the hit test.
    const glm::vec3 signs = gizmoPlaneSigns(center - m_camera->getPosition());
    const glm::mat4 planeBase = base * scaleDiag(signs);
    for (int i = 0; i < 3; ++i) {
      const auto axisHandle = static_cast<GizmoHandle>(static_cast<int>(GizmoHandle::AxisX) + i);
      const auto planeHandle = static_cast<GizmoHandle>(static_cast<int>(GizmoHandle::PlaneX) + i);
      const bool hotAxis = m_gizmoHover == axisHandle || m_gizmoDrag == axisHandle;
      const bool hotPlane = m_gizmoHover == planeHandle || m_gizmoDrag == planeHandle;
      const glm::vec3 hot = glm::mix(kAxisColor[i], glm::vec3(1.0f), 0.55f);
      // Overlay: never hidden by geometry.
      add(m_geoArrow[i], EntityPass::TriOverlay, base,
          glm::vec4(hotAxis ? hot : kAxisColor[i], hotAxis ? 1.0f : 0.92f));
      add(m_geoPlane[i], EntityPass::TriOverlay, planeBase,
          glm::vec4(hotPlane ? hot : kAxisColor[i], hotPlane ? 0.60f : 0.26f));
    }
  }

  // One upload at m_entityStride (what the draw indexes with), not sizeof(EntityUniforms).
  const size_t bytes = size_t(m_entityDraws.size()) * m_entityStride;
  m_entityScratch.assign(bytes, 0);
  for (size_t k = 0; k < m_entityDraws.size(); ++k) {
    const EntityDraw &d = m_entityDraws[k];
    EntityUniforms u;
    u.model = d.model;
    u.color = d.color;
    u.info = d.info;
    std::memcpy(m_entityScratch.data() + k * m_entityStride, &u, sizeof(EntityUniforms));
  }
  if (!m_entityScratch.empty())
    rub->updateDynamicBuffer(m_entityUbo, 0, int(m_entityScratch.size()), m_entityScratch.data());
}

void Engine3D::drawEntities(QRhiCommandBuffer *cb) {
  if (m_entityDraws.empty() || !m_entitySrb || !m_entityVb)
    return;

  // Icons (depth tested) before the always-on-top gizmo.
  drawBillboards(cb);

  const QRhiCommandBuffer::VertexInput vin(m_entityVb, 0);
  cb->setVertexInput(0, 1, &vin);

  // Order matters: the depth-writing pass first; the gizmo and range sphere never test depth.
  const EntityPass order[3] = {EntityPass::TriDepth, EntityPass::TriOverlay, EntityPass::Line};
  for (const EntityPass pass: order) {
    QRhiGraphicsPipeline *pipeline = pass == EntityPass::TriDepth
        ? m_entityTriDepthPipeline.get()
        : pass == EntityPass::TriOverlay ? m_entityTriOverlayPipeline.get()
                                       : m_entityLinePipeline.get();
    if (!pipeline)
      continue;
    cb->setGraphicsPipeline(pipeline);
    for (size_t k = 0; k < m_entityDraws.size(); ++k) {
      const EntityDraw &d = m_entityDraws[k];
      if (d.pass != pass)
        continue;
      const QRhiCommandBuffer::DynamicOffset offset(14, quint32(k) * m_entityStride);
      cb->setShaderResources(m_entitySrb, 1, &offset);
      cb->draw(d.vbCount, 1, quint32(d.vbOffset));
      ++m_perf.entityDraws;
    }
  }
}

void Engine3D::makeRay(const float x, const float y, const float viewWidth, const float viewHeight,
                       glm::vec3 &origin, glm::vec3 &dir) const {
  const float ndcX = 2.0f * (x / viewWidth) - 1.0f;
  const float ndcY = 1.0f - 2.0f * (y / viewHeight);

  const float aspect = viewWidth / viewHeight;
  const float tanHalf = std::tan(glm::radians(m_camera->getFov()) * 0.5f);

  origin = m_camera->getPosition();
  dir = glm::normalize(m_camera->getDirection()
                       + m_camera->getRight() * (ndcX * tanHalf * aspect)
                       + m_camera->getUp() * (ndcY * tanHalf));
}

// Top-down screen pixels (y = 0 at the top), as pick() uses.
glm::vec2 Engine3D::projectToScreen(const glm::vec3 &point) const {
  const glm::vec4 clip = m_sceneUniforms.viewProj * glm::vec4(point, 1.0f);
  const float w = std::max(1.0f, float(m_lastViewSize.width()));
  const float h = std::max(1.0f, float(m_lastViewSize.height()));
  if (std::fabs(clip.w) <= 1e-6f)
    return glm::vec2(0.0f);
  const glm::vec3 ndc = glm::vec3(clip) / clip.w;
  // viewProj carries clipSpaceCorrMatrix(): with Y-down NDC (Vulkan) the pixel row is ndc.y; only Y-up flips.
  QRhi *rhi = Shared::instance().rhi();
  const bool yUp = rhi && rhi->isYUpInNDC();
  const float sy = yUp ? 1.0f - (ndc.y * 0.5f + 0.5f) : ndc.y * 0.5f + 0.5f;
  return glm::vec2((ndc.x * 0.5f + 0.5f) * w, sy * h);
}

bool Engine3D::gizmoVisible() const {
  // Looking through a Camera3D: no gizmo.
  if (m_previewCamera)
    return false;
  if (m_editMode)
    return !m_selectedVerts.empty();
  // No editing while the physics simulation runs.
  if (physicsSimState() == PhysicsSimState::Running)
    return false;
  if (!m_selectedEntities.empty())
    return true;
  return m_scene && (!m_selectedObjects.empty() || !m_selectedMods.empty());
}

glm::vec3 Engine3D::gizmoCenter() const {
  if (m_editMode && !m_selectedVerts.empty())
    return selectedVertexCentre();
  if (m_scene && m_selectedObjects.empty() && !m_selectedEntities.empty()) {
    glm::vec3 c(0.0f);
    for (const auto &[id, o]: selectedEntityOrigins())
      c += o;
    return c / float(std::max<size_t>(1, m_selectedEntities.size()));
  }
  if (!m_scene || m_selectedObjects.empty())
    return glm::vec3(0.0f);

  glm::dvec3 lo, hi;
  if (!selectionWorldAABB(lo, hi))
    return glm::vec3(0.0f);
  return glm::vec3((lo + hi) * 0.5);
}

// One gizmo unit spans m_gizmoScreenPx pixels at any distance; scaled down below kGizmoViewportBasePx of view height.
float Engine3D::gizmoScale(const glm::vec3 &center) const {
  const float viewH = std::max(1.0f, float(m_lastViewSize.height()));
  const float halfFov = 0.5f * m_camera->getFov() * 3.14159265f / 180.0f;
  const float worldPerPixelAtUnit = 2.0f * std::tan(halfFov) / viewH;
  const float dist = std::max(1.0f, glm::length(m_camera->getPosition() - center));
  const float heightClamp = std::min(kGizmoViewportBasePx, viewH) / kGizmoViewportBasePx;
  return worldPerPixelAtUnit * dist * m_gizmoScreenPx * heightClamp;
}

double Engine3D::gizmoSnapStep() const {
  // As the 2D views: the grid level step.
  const double stepSource =
      GridView::kBaseGridStepSource * std::pow(2.0, static_cast<double>(m_gridLevelOffset));
  return stepSource / GridView::kUnitsPerWorldUnit;
}

Engine3D::GizmoHandle Engine3D::gizmoHitTest(const float x, const float y) const {
  if (!gizmoVisible())
    return GizmoHandle::None;
  if (m_gizmoMode != GizmoMode::Translate)
    return gizmoHitTestModes(x, y);

  const float viewW = std::max(1.0f, float(m_lastViewSize.width()));
  const float viewH = std::max(1.0f, float(m_lastViewSize.height()));
  const glm::vec3 center = gizmoCenter();
  const float s = gizmoScale(center);

  glm::vec3 ro {};
  glm::vec3 rd {};
  makeRay(x, y, viewW, viewH, ro, rd);
  const glm::vec3 camUp = m_camera->getUp();
  const glm::vec3 signs = gizmoPlaneSigns(center - ro);

  // Screen-relative cost: an edge-on handle loses to one facing the camera.
  auto cost = [&](const glm::vec3 &grabber, const float radius, const glm::vec3 &hit) {
    const glm::vec2 sc = projectToScreen(grabber);
    const float screenRadius =
        std::max(1.0f, glm::length(projectToScreen(grabber + camUp * radius) - sc));
    const float d = glm::length(projectToScreen(hit) - sc);
    return (d / screenRadius) * (d / screenRadius);
  };

  GizmoHandle best = GizmoHandle::None;
  float bestCost = 1e20f;

  for (int i = 0; i < 3; ++i) {
    const glm::vec3 g = center + unitAxis(i) * ((kArrowShaftEnd + kArrowTipEnd) * 0.5f) * s;
    const float r = (kArrowTipEnd - kArrowShaftEnd) * 0.5f * s * kGrabTolerance;
    float t = 0.0f;
    if (raySphere(ro, rd, g, r, t)) {
      const float c = cost(g, r, ro + rd * t);
      if (c < bestCost) {
        bestCost = c;
        best = static_cast<GizmoHandle>(static_cast<int>(GizmoHandle::AxisX) + i);
      }
    }
  }

  for (int i = 0; i < 3; ++i) {
    // Mirrored as the draw path does.
    const glm::vec3 ej = unitAxis((i + 1) % 3) * signs[(i + 1) % 3];
    const glm::vec3 ek = unitAxis((i + 2) % 3) * signs[(i + 2) % 3];
    const glm::vec3 g = center + (ej - ek) * (kPlaneDst + kPlaneSize * 0.5f) * s;
    const float r = kPlaneSize * 0.5f * s * kGrabTolerance;
    float t = 0.0f;
    if (raySphere(ro, rd, g, r, t)) {
      const float c = cost(g, r, ro + rd * t);
      if (c < bestCost) {
        bestCost = c;
        best = static_cast<GizmoHandle>(static_cast<int>(GizmoHandle::PlaneX) + i);
      }
    }
  }

  return best;
}

bool Engine3D::beginGizmoDrag(const float x, const float y) {
  if (noEntities() || noGizmo())
    return false;
  const GizmoHandle handle = gizmoHitTest(x, y);
  if (handle == GizmoHandle::None)
    return false;
  if (handle >= GizmoHandle::RingX)
    return beginGizmoModeDrag(handle, x, y);

  const float viewW = std::max(1.0f, float(m_lastViewSize.width()));
  const float viewH = std::max(1.0f, float(m_lastViewSize.height()));

  m_gizmoDrag = handle;
  m_gizmoHover = handle;
  m_gizmoDragCenter = gizmoCenter();
  makeRay(x, y, viewW, viewH, m_gizmoClickOrigin, m_gizmoClickDir);

  m_gizmoMeshIds.clear();
  m_gizmoMeshBasePos.clear();
  m_gizmoBaseUv.clear();
  m_gizmoDuplicate = {};
  m_gizmoEntBase = selectedEntityOrigins();
  if (m_editMode && !m_selectedVerts.empty()) {
    m_vertexDragging = true;
    m_vertexDragDelta = glm::vec3(0.0f);
    m_gizmoEntBase.clear();
    m_gizmoBaseMin = selectedVertexCentre();
    beginVertexDrag();
    return true;
  }
  if (!m_gizmoEntBase.empty() && m_selectedObjects.empty())
    m_gizmoBaseMin = m_gizmoEntBase.begin()->second;

  if (!m_scene)
    return true;

  m_gizmoMeshIds = moveTargets();
  if (const QString why = modMoveBlocked(); !why.isEmpty())
    emit toastRequested(why);
  glm::vec3 lo(FLT_MAX);
  // Parallel arrays: an id that no longer resolves is removed, not skipped.
  for (size_t k = 0; k < m_gizmoMeshIds.size();) {
    const auto obj = m_scene->findById(m_gizmoMeshIds[k]);
    if (!obj) {
      m_gizmoMeshIds.erase(m_gizmoMeshIds.begin() + qsizetype(k));
      continue;
    }
    m_gizmoMeshBasePos.push_back(obj->getPosition());
    ++k;
  }
  if (glm::dvec3 bmn, bmx; objectsBounds(m_gizmoMeshIds, bmn, bmx))
    lo = glm::vec3(bmn);
  m_gizmoBaseMin = lo.x == FLT_MAX ? glm::vec3(0.0f) : lo;
  captureFaceUv(m_gizmoMeshIds, m_gizmoBaseUv);
  beginMirrorPreview(m_gizmoMeshIds);
  return true;
}

void Engine3D::updateGizmoDrag(const float x, const float y, const bool duplicate) {
  ScopedDragPerf drag(m_perf.msDrag, m_perf.dragCalls);
  if (m_gizmoDrag == GizmoHandle::None)
    return;
  if (m_gizmoDrag >= GizmoHandle::RingX) {
    updateGizmoModeDrag(x, y);
    return;
  }

  const bool isPlane = m_gizmoDrag >= GizmoHandle::PlaneX;
  const int a = isPlane ? static_cast<int>(m_gizmoDrag) - static_cast<int>(GizmoHandle::PlaneX)
                        : static_cast<int>(m_gizmoDrag) - static_cast<int>(GizmoHandle::AxisX);
  const glm::vec3 axis = unitAxis(a);
  const glm::vec3 camNormal = -m_camera->getDirection();

  // Axis drag: in the camera-facing plane containing the axis. Plane drag: perpendicular to the locked axis.
  glm::vec3 planeNormal = axis;
  if (!isPlane) {
    planeNormal = camNormal - axis * glm::dot(camNormal, axis);
    if (glm::length(planeNormal) < 1e-4f) {
      glm::vec3 seed = std::fabs(axis.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f)
                                               : glm::vec3(0.0f, 1.0f, 0.0f);
      planeNormal = glm::cross(axis, seed);
    }
    planeNormal = glm::normalize(planeNormal);
  }

  glm::vec3 clickPt {};
  glm::vec3 curPt {};
  if (!rayPlane(m_gizmoClickOrigin, m_gizmoClickDir, planeNormal, m_gizmoDragCenter, clickPt))
    return;
  const float viewW = std::max(1.0f, float(m_lastViewSize.width()));
  const float viewH = std::max(1.0f, float(m_lastViewSize.height()));
  glm::vec3 ro {};
  glm::vec3 rd {};
  makeRay(x, y, viewW, viewH, ro, rd);
  if (!rayPlane(ro, rd, planeNormal, m_gizmoDragCenter, curPt))
    return;

  glm::vec3 motion = curPt - clickPt;
  if (!isPlane)
    motion = axis * glm::dot(motion, axis);

  // Snap where the bounding-box minimum ends up (as the 2D drag), not the delta. Ctrl: off the grid.
  const double step = gizmoSnapStep();
  const bool free = QGuiApplication::queryKeyboardModifiers().testFlag(Qt::ControlModifier);
  glm::vec3 delta(0.0f);
  for (int k = 0; k < 3; ++k) {
    const bool allowed = isPlane ? (k != a) : (k == a);
    if (!allowed)
      continue;
    const double base = static_cast<double>(m_gizmoBaseMin[k]);
    delta[k] = free ? motion[k] : float(snapTo(base + static_cast<double>(motion[k]), step) - base);
  }

  if (m_vertexDragging) {
    if (delta != m_vertexDragDelta) {
      m_vertexDragDelta = delta;
      updateVertexDrag(delta);
    }
    return;
  }
  if (duplicate && !m_gizmoDuplicate.triggered
      && (delta.x != 0.0f || delta.y != 0.0f || delta.z != 0.0f)) {
    spawnDuplicates(m_gizmoMeshIds, m_gizmoMeshBasePos, m_gizmoBaseUv, m_gizmoDuplicate);
    spawnEntityDuplicates(m_gizmoEntBase, m_gizmoDuplicate);
  }
  if (!m_gizmoEntBase.empty())
    moveSelectedEntities(m_gizmoEntBase, delta);

  for (size_t k = 0; m_scene && k < m_gizmoMeshIds.size() && k < m_gizmoMeshBasePos.size(); ++k) {
    const auto obj = m_scene->findById(m_gizmoMeshIds[k]);
    if (obj)
      obj->setPosition(m_gizmoMeshBasePos[k] + delta);
  }
  applyTextureLockMove(m_gizmoMeshIds, m_gizmoBaseUv, glm::vec3(delta.x, delta.y, delta.z));
  if (m_gizmoDuplicate.triggered)
    endMirrorPreview();
  else
    updateMirrorPreview();
  if (m_scene)
    emit sceneChanged();
}

void Engine3D::endGizmoDrag() {
  const GizmoHandle handle = m_gizmoDrag;
  if (handle >= GizmoHandle::RingX) {
    endGizmoModeDrag();
    m_gizmoDrag = GizmoHandle::None;
    return;
  }
  m_gizmoDrag = GizmoHandle::None;
  if (handle == GizmoHandle::None)
    return;
  endMirrorPreview();
  if (m_vertexDragging) {
    m_vertexDragging = false;
    endVertexDrag(m_vertexDragDelta);
    return;
  }
  if (m_gizmoDuplicate.triggered) {
    m_gizmoEntBase.clear(); // the copies moved, recorded with the duplicate
    pushDuplicateUndoMirrored(m_gizmoDuplicate, QStringLiteral("Duplicate selection"));
    return;
  }
  recordEntityMoves(m_gizmoEntBase, QStringLiteral("Move entities"));
  m_gizmoEntBase.clear();

  recordTransformEdit(QStringLiteral("Move selection"), m_gizmoMeshIds, m_gizmoMeshBasePos, {},
                      m_gizmoBaseUv);
}

void Engine3D::hoverGizmo(const float x, const float y) {
  if (perfOn())
    ++m_perf.hoverCalls;
  if (noEntities() || noGizmo())
    return;
  if (m_gizmoDrag != GizmoHandle::None)
    return;
  const GizmoHandle handle = gizmoHitTest(x, y);
  if (handle == m_gizmoHover)
    return;
  m_gizmoHover = handle;
}

} // namespace vk
