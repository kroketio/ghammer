// The map's skybox: World Environment's panorama behind the level.
// Draws the Panorama map's editor derivative (2048 x 1024 half-float EXR): fetched through FileFetcher, decoded on
// the thread pool, uploaded as RGBA16F. sky.frag samples it as Godot's PanoramaSkyMaterial does.

#include "vk_engine.h"
#include "vk_shared.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThreadPool>

#include <rhi/qrhi.h>

#include "client/engine/sky_info.h"
#include "client/engine/undo_stack.h"
#include "client/net/file_fetcher.h"
#include "client/net/texture_catalog.h"
#include "shared/lib/hdr_image.h"

namespace vk {

void Engine3D::setSkybox(const QString &ref) {
  const QString r = ref.trimmed();
  if (r == m_skyRef)
    return;
  recordValue(QStringLiteral("sky.ref"), QStringLiteral("Skybox"), m_skyRef, r, &Engine3D::setSkybox);
  m_skyRef = r;
  loadSkybox();
  emit skyChanged();
  emit sunChanged();
}

void Engine3D::applySkybox(const QString &ref, const eng::SkyInfo &info) {
  eng::UndoStack *undo = m_undoStack.get();
  if (undo)
    undo->beginMacro(QStringLiteral("Set skybox"));
  setSkybox(ref);
  if (info.valid) {
    const eng::SkyLighting l = eng::skyLighting(info);
    // Elevation first: the azimuth of a sun straight overhead is undefined.
    setSunElevation(std::max(1.0f, l.sunElevation));
    setSunAzimuth(l.sunAzimuth);
    setSunColor(l.sunColor);
    setSunIntensity(l.sunIntensity);
    setAmbientColor(l.ambientColor);
    setAmbientIntensity(l.ambientIntensity);
    if (l.fogColor.isValid())
      setFogColor(l.fogColor);
  }
  if (undo)
    undo->endMacro();
}

void Engine3D::loadSkybox() {
  const quint64 gen = ++m_skyLoadGen;
  const QString ref = m_skyRef;
  if (ref.isEmpty() || (ref == m_skyImageRef && !m_skyImage.isNull()))
    return;
  QPointer<Engine3D> self(this);
  const auto decode = [self, gen, ref](const QString &path) {
    if (!self || self->m_skyLoadGen != gen)
      return;
    if (path.isEmpty()) {
      qWarning("sky: cannot fetch the panorama of %s", qPrintable(ref));
      return;
    }
    QThreadPool::globalInstance()->start([self, gen, ref, path] {
      hdr::Image img;
      QString err;
      if (!hdr::loadExr(path, img, &err)) {
        qWarning("sky: %s: %s", qPrintable(path), qPrintable(err));
        return;
      }
      QImage tex = hdr::toRgba16F(img);
      QMetaObject::invokeMethod(
          qApp, [self, gen, ref, tex = std::move(tex)]() mutable {
            if (!self || self->m_skyLoadGen != gen || self->m_skyRef != ref)
              return;
            self->m_skyImage = std::move(tex);
            self->m_skyImageRef = ref;
            emit self->skyChanged();
          },
          Qt::QueuedConnection);
    });
  };
  // The entry names the sky file; resolve it first when it is not cached.
  const auto fetch = [self, gen, ref, decode] {
    if (!self || self->m_skyLoadGen != gen)
      return;
    const TextureEntry *e = net::TextureCatalog::instance().find(ref);
    const MapFile *pano = e ? e->map(MapType::Panorama) : nullptr;
    if (!pano || pano->editor.isEmpty()) {
      qWarning("sky: %s is not a skybox the server knows", qPrintable(ref));
      return;
    }
    net::FileFetcher::instance().fetch(net::FileKind::Editor, pano->editor, net::FetchPriority::Scene, self,
                                       decode);
  };
  if (net::TextureCatalog::instance().find(ref))
    fetch();
  else
    net::TextureCatalog::instance().resolve({ref}, this, fetch);
}

void Engine3D::ensureSkyTexture(QRhiResourceUpdateBatch *rub) {
  const bool want = !m_skyRef.isEmpty() && m_skyImageRef == m_skyRef && !m_skyImage.isNull();
  if (!want) {
    if (m_skyTex && m_skyTexRef != m_skyRef)
      releaseSkyTexture();
    return;
  }
  if (m_skyTex && m_skyTexRef == m_skyRef)
    return;
  QRhi *rhi = Shared::instance().rhi();
  if (!rhi || !rub)
    return;
  if (!rhi->isTextureFormatSupported(QRhiTexture::RGBA16F)) {
    qWarning("sky: RGBA16F textures are not supported - the procedural sky stays");
    m_skyImage = {};
    return;
  }
  releaseSkyTexture();
  m_skyTex = rhi->newTexture(QRhiTexture::RGBA16F, m_skyImage.size());
  if (!m_skyTex->create()) {
    qWarning("sky: %dx%d texture failed", m_skyImage.width(), m_skyImage.height());
    delete m_skyTex;
    m_skyTex = nullptr;
    return;
  }
  rub->uploadTexture(m_skyTex, m_skyImage);
  m_skyTexRef = m_skyRef;
  // New texture object: the sky bindings are rebuilt.
  ++m_bufferGen;
}

void Engine3D::releaseSkyTexture() {
  if (m_skyTex) {
    Shared::instance().retireTexture(m_skyTex);
    ++m_bufferGen;
  }
  m_skyTex = nullptr;
  m_skyTexRef.clear();
}

} // namespace vk
