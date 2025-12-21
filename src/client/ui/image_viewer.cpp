#include "image_viewer.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QImageReader>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QWheelEvent>

#include "client/ui/external_apps.h"
#include "client/ui/scale.h"
#include "shared/lib/hdr_image.h"

namespace ui {

namespace {

struct Decoded {
  QImage image;
  QString format;
  QString error;
};

Decoded decode(const QString &path) {
  Decoded d;
  if (path.endsWith(QLatin1String(".exr"), Qt::CaseInsensitive)) {
    hdr::Image img;
    QString err;
    if (!hdr::loadExr(path, img, &err) || img.isNull()) {
      d.error = err.isEmpty() ? QObject::tr("Cannot read the image") : err;
      return d;
    }
    d.image = hdr::tonemapped(img, img.w, img.h);
    d.format = QStringLiteral("EXR · HDR, tone-mapped");
    return d;
  }
  QImageReader r(path);
  r.setAutoTransform(true);
  r.setAllocationLimit(1024); // MB: large textures are fine
  d.format = QString::fromLatin1(r.format()).toUpper();
  d.image = r.read();
  if (d.image.isNull())
    d.error = r.errorString();
  else if (d.image.hasAlphaChannel())
    d.format += QStringLiteral(" · alpha");
  return d;
}

constexpr double kMinZoom = 0.01, kMaxZoom = 64.0;

} // namespace

ImageViewer::ImageViewer(const QString &path, QWidget *parent) : QWidget(parent) {
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_OpaquePaintEvent);
  m_watcher = new QFileSystemWatcher(this);
  connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this] {
    // Editors save in steps (write, rename): read again once it has settled.
    QTimer::singleShot(300, this, [this] {
      if (!m_watcher->files().contains(m_path) && QFileInfo::exists(m_path))
        m_watcher->addPath(m_path);
      load();
    });
  });
  setPath(path);
}

void ImageViewer::setPath(const QString &path) {
  if (!m_watcher->files().isEmpty())
    m_watcher->removePaths(m_watcher->files());
  m_path = path;
  if (QFileInfo::exists(m_path))
    m_watcher->addPath(m_path);
  m_fitted = true;
  load();
}

void ImageViewer::load() {
  const quint64 gen = ++m_loadGen;
  m_loading = true;
  update();
  QPointer<ImageViewer> self(this);
  const QString path = m_path;
  QThreadPool::globalInstance()->start([self, gen, path] {
    Decoded d = decode(path);
    QMetaObject::invokeMethod(qApp, [self, gen, d = std::move(d)]() mutable {
      if (!self || self->m_loadGen != gen)
        return;
      self->m_loading = false;
      const QSize before = self->m_image.size();
      self->m_image = std::move(d.image);
      self->m_format = d.format;
      self->m_error = d.error;
      // A reload keeps the view unless the size changed.
      if (self->m_fitted || before != self->m_image.size())
        self->fit();
      self->update();
    });
  });
}

void ImageViewer::fit() {
  m_fitted = true;
  if (m_image.isNull() || width() <= 0 || height() <= 0) {
    update();
    return;
  }
  const double margin = scale::px(24);
  const double zx = (width() - 2 * margin) / double(m_image.width());
  const double zy = (height() - 2 * margin) / double(m_image.height());
  // No zoom past 100% on open.
  m_zoom = std::clamp(std::min({zx, zy, 1.0}), kMinZoom, kMaxZoom);
  m_offset = QPointF((width() - m_image.width() * m_zoom) / 2.0, (height() - m_image.height() * m_zoom) / 2.0);
  update();
}

void ImageViewer::actualSize() {
  m_fitted = false;
  zoomAt(1.0 / m_zoom, QPointF(width() / 2.0, height() / 2.0));
}

void ImageViewer::zoomAt(const double factor, const QPointF &at) {
  const double z = std::clamp(m_zoom * factor, kMinZoom, kMaxZoom);
  // Keep the image point under `at` fixed.
  const QPointF img = (at - m_offset) / m_zoom;
  m_zoom = z;
  m_offset = at - img * m_zoom;
  update();
}

QString ImageViewer::infoText() const {
  if (m_loading && m_image.isNull())
    return tr("Loading…");
  if (m_image.isNull())
    return m_error.isEmpty() ? tr("Cannot read the image") : m_error;
  return QStringLiteral("%1 × %2  ·  %3  ·  %4%")
      .arg(m_image.width())
      .arg(m_image.height())
      .arg(m_format)
      .arg(qRound(m_zoom * 100.0));
}

void ImageViewer::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), QColor(QStringLiteral("#101428")));
  if (!m_image.isNull()) {
    const QRectF target(m_offset, QSizeF(m_image.width() * m_zoom, m_image.height() * m_zoom));
    // Checkerboard for transparency.
    const int cell = scale::px(8);
    QPixmap checker(cell * 2, cell * 2);
    checker.fill(QColor(QStringLiteral("#2a2f45")));
    {
      QPainter cp(&checker);
      cp.fillRect(0, 0, cell, cell, QColor(QStringLiteral("#3a4060")));
      cp.fillRect(cell, cell, cell, cell, QColor(QStringLiteral("#3a4060")));
    }
    p.fillRect(target.intersected(QRectF(rect())), QBrush(checker));
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 1.0);
    p.drawImage(target, m_image);
    p.setPen(QColor(QStringLiteral("#232A4E")));
    p.drawRect(target.adjusted(-0.5, -0.5, 0.5, 0.5));
  }
  // Info pill, lower left (only errors when info is off).
  if (!m_showInfo && (!m_image.isNull() || m_loading))
    return;
  const QString text = infoText();
  QFont f = font();
  f.setPixelSize(scale::tpx(12));
  p.setFont(f);
  const QFontMetrics fm(f);
  const int pad = scale::px(8);
  const QRect box(scale::px(12), height() - scale::px(12) - fm.height() - pad,
                  fm.horizontalAdvance(text) + 2 * pad, fm.height() + pad);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(12, 16, 34, 220));
  p.drawRoundedRect(box, box.height() / 2.0, box.height() / 2.0);
  p.setPen(m_image.isNull() && !m_loading ? QColor(QStringLiteral("#f0b45a")) : QColor(QStringLiteral("#c7cce7")));
  p.drawText(box, Qt::AlignCenter, text);
}

void ImageViewer::resizeEvent(QResizeEvent *) {
  if (m_fitted)
    fit();
}

void ImageViewer::wheelEvent(QWheelEvent *event) {
  const double steps = event->angleDelta().y() / 120.0;
  if (steps == 0.0)
    return;
  m_fitted = false;
  zoomAt(std::pow(1.25, steps), event->position());
  event->accept();
}

void ImageViewer::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
    m_panning = true;
    m_panFrom = event->position();
    setCursor(Qt::ClosedHandCursor);
  }
}

void ImageViewer::mouseMoveEvent(QMouseEvent *event) {
  if (!m_panning)
    return;
  m_offset += event->position() - m_panFrom;
  m_panFrom = event->position();
  m_fitted = false;
  update();
}

void ImageViewer::mouseReleaseEvent(QMouseEvent *) {
  m_panning = false;
  unsetCursor();
}

void ImageViewer::mouseDoubleClickEvent(QMouseEvent *) {
  fit();
}

void ImageViewer::keyPressEvent(QKeyEvent *event) {
  switch (event->key()) {
    case Qt::Key_F:
    case Qt::Key_0:
      fit();
      return;
    case Qt::Key_1:
      actualSize();
      return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
      m_fitted = false;
      zoomAt(1.25, QPointF(width() / 2.0, height() / 2.0));
      return;
    case Qt::Key_Minus:
      m_fitted = false;
      zoomAt(1.0 / 1.25, QPointF(width() / 2.0, height() / 2.0));
      return;
    default:
      QWidget::keyPressEvent(event);
  }
}

void ImageViewer::contextMenuEvent(QContextMenuEvent *event) {
  QMenu menu(this);
  menu.addAction(tr("Fit"), this, &ImageViewer::fit);
  menu.addAction(tr("Actual Size"), this, &ImageViewer::actualSize);
  menu.addSeparator();
  const QString path = m_path;
  if (asReference && !path.isEmpty())
    menu.addAction(tr("As Reference Image"), this, [this, path] { asReference(path); });
  menu.addAction(tr("Open With External Program"), this, [this, path] { external::openImage(path, this); });
  menu.addAction(tr("Show in File Manager"), this, [path] {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
  });
  menu.addAction(tr("Copy Path"), this, [path] { QApplication::clipboard()->setText(QDir::toNativeSeparators(path)); });
  menu.exec(event->globalPos());
}

} // namespace ui
