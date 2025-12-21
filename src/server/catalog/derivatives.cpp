#include "derivatives.h"

#include <QDateTime>

#include <algorithm>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSaveFile>
#include <QtDebug>

#include "server_db.h"
#include "shared/lib/hdr_image.h"

namespace {

constexpr int kThumbEdge = 256;
constexpr int kEditorEdge = 512;

bool hasTransparentPixels(const QImage &img) {
  if (!img.hasAlphaChannel())
    return false;
  const QImage a = img.convertToFormat(QImage::Format_ARGB32);
  for (int y = 0; y < a.height(); ++y) {
    const QRgb *row = reinterpret_cast<const QRgb *>(a.constScanLine(y));
    for (int x = 0; x < a.width(); ++x)
      if (qAlpha(row[x]) < 250)
        return true;
  }
  return false;
}

QImage scaledTo(const QImage &src, int edge) {
  if (src.width() <= edge && src.height() <= edge)
    return src;
  return src.scaled(QSize(edge, edge), Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

bool writeImage(const QImage &img, const QString &path, bool png, int quality) {
  // QSaveFile: the derivative appears atomically, never half-written.
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    return false;
  if (!img.save(&f, png ? "PNG" : "JPG", png ? -1 : quality)) {
    f.cancelWriting();
    return false;
  }
  return f.commit();
}

bool writeBytes(const QByteArray &bytes, const QString &path) {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) {
    f.cancelWriting();
    return false;
  }
  return f.commit();
}

// Skybox panoramas: one decode at a time (a 16K x 8K source is about 1 GB
// while it is read), area-averaged straight down to the editor's sky size.
QMutex g_exrDecode;
constexpr int kSkyWidth = 2048;

} // namespace

DerivativeBuilder &DerivativeBuilder::instance() {
  static DerivativeBuilder b;
  return b;
}

DerivativeBuilder::DerivativeBuilder() {
  m_pool.setMaxThreadCount(std::max(1, QThread::idealThreadCount() - 1));
  m_logTimer.setInterval(5000);
  connect(&m_logTimer, &QTimer::timeout, this, [this] {
    const Progress p = progress();
    if (p.done + p.failed == m_lastLoggedDone)
      return;
    m_lastLoggedDone = p.done + p.failed;
    qInfo().noquote() << QStringLiteral("derivatives: %1 / %2 built, %3 failed")
                             .arg(p.done)
                             .arg(p.queued)
                             .arg(p.failed);
    if (p.done + p.failed >= p.queued)
      m_logTimer.stop();
  });
}

void DerivativeBuilder::setThreadCount(int n) { m_pool.setMaxThreadCount(std::max(1, n)); }

QString DerivativeBuilder::pathFor(const DerivativeSource &src) const {
  const Catalog &c = Catalog::instance();
  return QDir(src.thumb ? c.thumbsDir() : c.editorDir()).absoluteFilePath(src.file);
}

DerivativeBuilder::Progress DerivativeBuilder::progress() const {
  return {m_queued.load(), m_done.load(), m_failed.load()};
}

void DerivativeBuilder::enqueue(const QVector<DerivativeSource> &jobs) {
  if (jobs.isEmpty())
    return;
  // Group by source so each image is decoded once for its thumb + editor map.
  QHash<QString, QVector<DerivativeSource>> bySource;
  QStringList order;
  for (const auto &j: jobs) {
    if (!bySource.contains(j.srcAbs))
      order.append(j.srcAbs);
    bySource[j.srcAbs].append(j);
  }
  m_queued += jobs.size();
  for (const auto &src: order) {
    const QVector<DerivativeSource> targets = bySource.value(src);
    m_pool.start([this, targets] {
      QString err;
      build(targets, &err, true);
      emit progressChanged();
    });
  }
  QMetaObject::invokeMethod(this, [this] {
    if (!m_logTimer.isActive())
      m_logTimer.start();
  }, Qt::QueuedConnection);
}

void DerivativeBuilder::claim(const QString &file) {
  QMutexLocker lock(&m_mutex);
  while (m_inFlight.contains(file))
    m_doneCond.wait(&m_mutex);
  m_inFlight.insert(file);
}

void DerivativeBuilder::release(const QString &file) {
  QMutexLocker lock(&m_mutex);
  m_inFlight.remove(file);
  m_doneCond.wakeAll();
}

bool DerivativeBuilder::ensure(const DerivativeSource &src, QString *error) {
  const QString path = pathFor(src);
  if (QFileInfo::exists(path))
    return true;
  return build({src}, error, false);
}

bool DerivativeBuilder::build(const QVector<DerivativeSource> &targets, QString *error, bool counted) {
  if (targets.isEmpty())
    return true;
  // Claim every target; drop the ones that exist by the time we hold them.
  QVector<DerivativeSource> todo;
  for (const auto &t: targets) {
    const QString key = (t.thumb ? QStringLiteral("t/") : QStringLiteral("e/")) + t.file;
    claim(key);
    if (QFileInfo::exists(pathFor(t))) {
      release(key);
      if (counted)
        ++m_done;
      continue;
    }
    todo.append(t);
  }
  if (todo.isEmpty())
    return true;

  auto releaseAll = [&] {
    for (const auto &t: todo)
      release((t.thumb ? QStringLiteral("t/") : QStringLiteral("e/")) + t.file);
  };

  const DerivativeSource &first = todo.first();
  if (!QFileInfo::exists(first.srcAbs)) {
    // The texture was renamed / deleted after the job was queued.
    releaseAll();
    if (error)
      *error = QStringLiteral("source gone: %1").arg(first.relPath);
    return false;
  }
  if (first.type == MapType::Panorama) {
    const bool ok = buildPanorama(todo, error, counted);
    releaseAll();
    return ok;
  }
  QImageReader reader(first.srcAbs);
  reader.setAutoTransform(false);
  QImage img = reader.read();
  if (img.isNull()) {
    if (error)
      *error = QStringLiteral("cannot decode %1: %2").arg(first.relPath, reader.errorString());
    qWarning().noquote() << "derivatives:" << (error ? *error : QString());
    if (counted)
      m_failed += todo.size();
    releaseAll();
    return false;
  }

  // Real transparency of color maps: drives the transparency default.
  if (first.type == MapType::Color && img.hasAlphaChannel()) {
    const bool transparent = hasTransparentPixels(img);
    const QFileInfo fi(first.srcAbs);
    serverdb::ImageInfo info;
    info.size = fi.size();
    info.mtime = fi.lastModified().toMSecsSinceEpoch();
    info.w = img.width();
    info.h = img.height();
    info.alphaChannel = true;
    info.alphaReal = transparent ? 1 : 0;
    info.channels = 4;
    serverdb::storeImageInfo(first.relPath, info);
    Catalog::instance().setImageAlpha(first.relPath, transparent);
  }

  bool ok = true;
  for (const auto &t: todo) {
    QImage out = scaledTo(img, t.thumb ? kThumbEdge : kEditorEdge);
    if (t.flipGreen) {
      out = out.convertToFormat(QImage::Format_RGB888);
      for (int y = 0; y < out.height(); ++y) {
        uchar *row = out.scanLine(y);
        for (int x = 0; x < out.width(); ++x)
          row[x * 3 + 1] = uchar(255 - row[x * 3 + 1]);
      }
    }
    if (t.png)
      out = out.convertToFormat(QImage::Format_ARGB32);
    else if (mapTypeGrayscale(t.type))
      out = out.convertToFormat(QImage::Format_Grayscale8);
    else
      out = out.convertToFormat(QImage::Format_RGB888);
    if (writeImage(out, pathFor(t), t.png, t.thumb ? 85 : 90)) {
      if (counted)
        ++m_done;
    } else {
      ok = false;
      if (counted)
        ++m_failed;
      if (error)
        *error = QStringLiteral("cannot write %1").arg(pathFor(t));
      qWarning().noquote() << "derivatives: cannot write" << pathFor(t);
    }
  }
  releaseAll();
  return ok;
}

bool DerivativeBuilder::buildPanorama(const QVector<DerivativeSource> &todo, QString *error, bool counted) {
  const DerivativeSource &first = todo.first();
  hdr::Image sky;
  QString err;
  {
    QMutexLocker lock(&g_exrDecode);
    const QSize src = hdr::exrSize(first.srcAbs, &err);
    if (src.isValid()) {
      const int w = std::min(kSkyWidth, src.width());
      hdr::loadExr(first.srcAbs, sky, &err, QSize(w, std::min(w / 2, src.height())));
    }
  }
  if (sky.isNull()) {
    if (error)
      *error = QStringLiteral("cannot decode %1: %2").arg(first.relPath, err);
    qWarning().noquote() << "derivatives:" << QStringLiteral("cannot decode %1: %2").arg(first.relPath, err);
    if (counted)
      m_failed += todo.size();
    return false;
  }
  bool ok = true;
  for (const auto &t: todo) {
    bool written = false;
    if (t.thumb) {
      // The whole panorama at 2:1, tone mapped.
      written = writeImage(hdr::tonemapped(sky, kThumbEdge, kThumbEdge / 2), pathFor(t), false, 85);
    } else {
      const QByteArray bytes = hdr::encodeExr(sky, &err);
      written = !bytes.isEmpty() && writeBytes(bytes, pathFor(t));
    }
    if (written) {
      if (counted)
        ++m_done;
      continue;
    }
    ok = false;
    if (counted)
      ++m_failed;
    if (error)
      *error = QStringLiteral("cannot write %1").arg(pathFor(t));
    qWarning().noquote() << "derivatives: cannot write" << pathFor(t);
  }
  return ok;
}

int DerivativeBuilder::collectGarbage(const QSet<QString> &keep) {
  const Catalog &c = Catalog::instance();
  if (c.loadHadErrors()) {
    qWarning() << "derivatives: catalogue had load errors - garbage collection skipped";
    return 0;
  }
  int removed = 0;
  // Only these two generated directories, never anything under data/.
  for (const QString &dirPath: {c.thumbsDir(), c.editorDir()}) {
    QDir dir(dirPath);
    if (dirPath.isEmpty() || !dir.exists() || !dirPath.contains(QLatin1String("/cache/")))
      continue;
    for (const QString &fn: dir.entryList(QDir::Files)) {
      if (keep.contains(fn))
        continue;
      if (QFile::remove(dir.absoluteFilePath(fn)))
        ++removed;
    }
  }
  if (removed)
    qInfo() << "derivatives: removed" << removed << "unreferenced files";
  return removed;
}
