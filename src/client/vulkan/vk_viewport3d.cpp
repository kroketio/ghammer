#include "vk_viewport3d.h"

#include <QQuickWindow>
#include <rhi/qrhi.h>

#include "client/lib/globals.h"
#include "vk_engine.h"
#include "vk_shared.h"

#include <QTimer>

#include "client/engine/gameobject.h"
#include "client/engine/material.h"
#include "client/engine/mesh.h"
#include "client/prefs/preferences.h"

namespace vk {

Viewport3D::Viewport3D(QQuickItem *parent) : QQuickRhiItem(parent) {
  setSampleCount(m_msaaSamples);
  setFlag(ItemHasContents, true);

  prefs::get().watch(QStringLiteral("Render/ContinuousRedraw"), this, [this](const QVariant &v) {
    m_continuous = v.toBool();
    update();
  });
  m_poll = new QTimer(this);
  m_poll->setInterval(16);
  connect(m_poll, &QTimer::timeout, this, &Viewport3D::pollForChanges);
  m_poll->start();
  setEngine(nullptr);
}

Engine3D *Viewport3D::engine() const {
  return m_engine ? m_engine.data() : g::engine3D;
}

void Viewport3D::setEngine(Engine3D *engine) {
  if (m_engine == engine && m_redrawConn)
    return;
  m_engine = engine;
  disconnect(m_redrawConn);
  if (Engine3D *e = this->engine())
    m_redrawConn = connect(e, &Engine3D::redrawRequested, this, [this] { update(); });
  emit engineChanged();
  update();
}

QObject *Viewport3D::engineObject() const {
  return engine();
}

void Viewport3D::setEngineObject(QObject *engine) {
  setEngine(qobject_cast<Engine3D *>(engine));
}

void Viewport3D::pollForChanges() {
  if (!isVisible())
    return;
  quint64 sig = eng::Mesh::globalGeometryEpoch() * 1000003ull;
  sig ^= eng::Mesh::globalFaceEpoch() * 2654435761ull;
  sig ^= eng::GameObject::globalTransformEpoch() * 40503ull;
  sig ^= eng::Material::globalEpoch() * 97ull;
  Engine3D *e = engine();
  if (e)
    sig ^= e->viewSignature();
  const bool changed = sig != m_lastSignature;
  m_lastSignature = sig;
  if (changed || m_continuous || (e && e->wantsContinuousFrames()))
    update();
}

void Viewport3D::setMsaaSamples(const int samples) {
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
  clampMsaaToSupported(window() ? window()->rhi() : nullptr);
  emit msaaSamplesChanged();
}

void Viewport3D::clampMsaaToSupported(QRhi *rhi) {
  int target = m_msaaSamples;
  if (rhi) {
    const QList<int> supported = rhi->supportedSampleCounts();
    if (!supported.isEmpty()) {
      while (target > 1 && !supported.contains(target))
        target /= 2;
    }
  }
  if (target < 1)
    target = 1;

  if (sampleCount() != target)
    setSampleCount(target);
}

QQuickRhiItemRenderer *Viewport3D::createRenderer() {
  return new Viewport3DRenderer();
}

void Viewport3DRenderer::initialize(QRhiCommandBuffer *) {
}

void Viewport3DRenderer::synchronize(QQuickRhiItem *item) {
  if (auto *view = qobject_cast<Viewport3D *>(item)) {
    view->clampMsaaToSupported(rhi());
    m_engine = view->engine();
  }
}

void Viewport3DRenderer::render(QRhiCommandBuffer *cb) {
  Shared::makeCurrent(rhi());
  Shared::instance().beginFrame();
  Engine3D *engine = m_engine.data();

  if (engine && renderTarget()) {
    // With MSAA, colorTexture() is null and the multisample buffer is the attachment: reading only colorTexture()
    // would build every pipeline single-sampled against a multisample render pass.
    int samples = 1;
    if (auto *msaa = msaaColorBuffer())
      samples = msaa->sampleCount();
    else if (auto *color = colorTexture())
      samples = color->sampleCount();

    engine->render(cb, renderTarget(), renderTarget()->pixelSize(), samples);
  }

  // While animating, request the next frame here so frames follow presentation (vsync), not a drifting 16 ms
  // timer. Idle views use Viewport3D::pollForChanges().
  if (engine && (engine->wantsContinuousFrames() || prefs::b("Render/ContinuousRedraw")))
    update();
}

} // namespace vk
