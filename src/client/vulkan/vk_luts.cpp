// The level's colour LUT: a LUT of the asset server by ref ("pack/Name" in a "lut" pack). Its strip is downloaded
// once per checksum and uploaded as the 3D texture scene.frag / sky.frag sample after tone mapping.

#include "vk_engine.h"
#include "vk_shared.h"

#include <QHash>
#include <QNetworkReply>
#include <QPointer>

#include <rhi/qrhi.h>

#include "client/engine/undo_stack.h"
#include "client/net/asset_client.h"
#include "client/net/texture_catalog.h"

namespace vk {

namespace {

// Strips by checksum, shared by every engine (the 3D view, previews).
QHash<QString, QImage> &stripCache() {
  static QHash<QString, QImage> cache;
  return cache;
}

QImage validStrip(const QImage &img) {
  const int n = img.height();
  if (img.isNull() || n < 2 || n > 256 || img.width() != n * n)
    return {};
  return img.convertToFormat(QImage::Format_RGB888);
}

} // namespace

void Engine3D::setColorLut(const QString &ref) {
  const QString r = ref.trimmed();
  if (r == m_lutRef)
    return;
  recordValue(QStringLiteral("lut.ref"), QStringLiteral("Colour LUT"), m_lutRef, r, &Engine3D::setColorLut);
  m_lutRef = r;
  loadColorLut();
  emit lutsChanged();
  requestRedraw();
}

QImage Engine3D::colorLutStripFor(const QString &ref) const {
  if (ref.isEmpty())
    return {};
  if (!m_lutStripRef.isEmpty() && refKey(ref) == refKey(m_lutStripRef))
    return m_lutStrip;
  const TextureEntry *e = net::TextureCatalog::instance().find(ref, true);
  const MapFile *f = e ? e->map(MapType::Lut) : nullptr;
  return f ? stripCache().value(f->checksum) : QImage();
}

void Engine3D::setLutStrip(const QString &ref, const QImage &strip) {
  m_lutStrip = strip;
  m_lutStripRef = ref;
  ++m_lutEpoch;
  emit lutsChanged();
  requestRedraw();
}

void Engine3D::previewColorLutStrip(const QImage &strip) {
  m_lutPreviewOn = true;
  m_lutPreview = validStrip(strip);
  ++m_lutEpoch;
  requestRedraw();
}

void Engine3D::endColorLutPreview() {
  if (!m_lutPreviewOn)
    return;
  m_lutPreviewOn = false;
  m_lutPreview = {};
  ++m_lutEpoch;
  requestRedraw();
}

void Engine3D::setColorLutEnabled(const bool on) {
  if (on == m_lutEnabled)
    return;
  m_lutEnabled = on;
  ++m_lutEpoch;
  emit lutsChanged();
  requestRedraw();
}

void Engine3D::previewColorLut(const QString &ref, const QImage &strip) {
  if (m_lutRef.isEmpty() || refKey(ref) != refKey(m_lutRef))
    return;
  if (const QImage s = validStrip(strip); !s.isNull())
    setLutStrip(m_lutRef, s);
}

void Engine3D::loadColorLut() {
  const quint64 gen = ++m_lutLoadGen;
  const QString ref = m_lutRef;
  if (ref.isEmpty()) {
    if (!m_lutStrip.isNull() || !m_lutStripRef.isEmpty())
      setLutStrip(QString(), QImage());
    return;
  }
  QPointer<Engine3D> self(this);
  const auto fetch = [self, gen, ref] {
    if (!self || self->m_lutLoadGen != gen)
      return;
    const TextureEntry *e = net::TextureCatalog::instance().find(ref, true);
    const MapFile *f = e ? e->map(MapType::Lut) : nullptr;
    if (!f) {
      qWarning("lut: %s is not a LUT the server knows", qPrintable(ref));
      self->setLutStrip(ref, QImage());
      return;
    }
    const QString checksum = f->checksum;
    if (auto it = stripCache().constFind(checksum); it != stripCache().constEnd()) {
      self->setLutStrip(ref, *it);
      return;
    }
    const MaterialRef r = MaterialRef::parse(e->ref);
    net::AssetClient::instance().httpGet(
        QStringLiteral("/api/2/source/%1/%2/lut").arg(r.pack, r.name), self,
        [self, gen, ref, checksum](QNetworkReply *reply) {
          if (!self || self->m_lutLoadGen != gen)
            return;
          const QImage strip = reply->error() == QNetworkReply::NoError
                                   ? validStrip(QImage::fromData(reply->readAll(), "PNG"))
                                   : QImage();
          if (strip.isNull()) {
            qWarning("lut: cannot load %s: %s", qPrintable(ref), qPrintable(reply->errorString()));
            self->setLutStrip(ref, QImage());
            return;
          }
          stripCache().insert(checksum, strip);
          self->setLutStrip(ref, strip);
        });
  };
  if (net::TextureCatalog::instance().find(ref, true))
    fetch();
  else
    net::TextureCatalog::instance().resolve({ref}, this, fetch, true);
}

void Engine3D::watchLutCatalog() {
  auto &cat = net::TextureCatalog::instance();
  const auto reloadIf = [this](const QString &ref) {
    if (!m_lutRef.isEmpty() && refKey(ref) == refKey(m_lutRef))
      loadColorLut();
  };
  connect(&cat, &net::TextureCatalog::entryChanged, this, reloadIf);
  connect(&cat, &net::TextureCatalog::entryRemoved, this, reloadIf);
  connect(&cat, &net::TextureCatalog::entryRenamed, this, [this](const QString &oldRef, const QString &newRef) {
    if (m_lutRef.isEmpty() || refKey(oldRef) != refKey(m_lutRef))
      return;
    m_lutRef = newRef;
    loadColorLut();
    emit lutsChanged();
  });
  connect(&cat, &net::TextureCatalog::serverChanged, this, [this] { loadColorLut(); });
  connect(&net::AssetClient::instance(), &net::AssetClient::stateChanged, this, [this](net::AssetClient::State st) {
    if (st == net::AssetClient::State::Online && !m_lutRef.isEmpty() && colorLutStrip().isNull())
      loadColorLut();
  });
}

void Engine3D::releaseLutTexture() {
  if (m_lutTex)
    Shared::instance().retireTexture(m_lutTex);
  m_lutTex = nullptr;
  m_lutTexKey = 0;
  m_lutTexSize = 0;
  m_lutActiveSize = 0;
}

int Engine3D::ensureLutTexture(QRhiResourceUpdateBatch *rub) {
  const QImage current = !m_lutEnabled ? QImage() : m_lutPreviewOn ? m_lutPreview : colorLutStrip();
  const QImage *strip = current.isNull() ? nullptr : &current;

  const quint64 key = m_lutEpoch * 2ull + (strip ? 1ull : 0ull) + 1ull;
  if (m_lutTex && key == m_lutTexKey)
    return m_lutActiveSize;

  const int n = strip ? strip->height() : 2;
  QRhi *rhi = Shared::instance().rhi();
  if (!m_lutTex || m_lutTexSize != n) {
    if (m_lutTex)
      Shared::instance().retireTexture(m_lutTex);
    m_lutTex = rhi->newTexture(QRhiTexture::RGBA8, n, n, n, 1, QRhiTexture::ThreeDimensional);
    if (!m_lutTex->create()) {
      qWarning("lut: 3D texture %dx%dx%d failed", n, n, n);
      delete m_lutTex;
      m_lutTex = nullptr;
      m_lutTexSize = 0;
      m_lutActiveSize = 0;
      return 0;
    }
    m_lutTexSize = n;
    // New texture object: the scene/sky bindings are rebuilt.
    ++m_bufferGen;
  }

  const QImage src = strip ? strip->convertToFormat(QImage::Format_RGBA8888) : QImage();
  QVarLengthArray<QRhiTextureUploadEntry, 64> entries;
  for (int b = 0; b < n; ++b) {
    QImage slice;
    if (strip) {
      slice = src.copy(b * n, 0, n, n);
    } else {
      slice = QImage(n, n, QImage::Format_RGBA8888);
      for (int g = 0; g < n; ++g)
        for (int r = 0; r < n; ++r)
          slice.setPixel(r, g, qRgba(r * 255, g * 255, b * 255, 255));
    }
    entries.append(QRhiTextureUploadEntry(b, 0, QRhiTextureSubresourceUploadDescription(slice)));
  }
  QRhiTextureUploadDescription desc;
  desc.setEntries(entries.cbegin(), entries.cend());
  rub->uploadTexture(m_lutTex, desc);
  m_lutTexKey = key;
  m_lutActiveSize = strip ? n : 0;
  return m_lutActiveSize;
}

} // namespace vk
