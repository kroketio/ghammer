#include "thumb_image_provider.h"

#include <QUrl>

namespace ui {

QString ThumbImageProvider::thumbId(const ThumbSource &source) {
  if (source.empty())
    return {};
  return (source.local ? QStringLiteral("l/") : QStringLiteral("s/"))
       + QString::fromLatin1(QUrl::toPercentEncoding(source.file));
}

QPixmap ThumbImageProvider::requestPixmap(const QString &id, QSize *size, const QSize &requestedSize) {
  QString body = id.section(QLatin1Char('?'), 0, 0);
  ThumbSource source;
  if (body.startsWith(QLatin1String("l/")))
    source.local = true;
  else if (!body.startsWith(QLatin1String("s/")))
    body.clear();
  if (!body.isEmpty())
    source.file = QUrl::fromPercentEncoding(body.mid(2).toLatin1());
  QPixmap pm = source.pixmap();
  if (pm.isNull()) {
    pm = QPixmap(1, 1); // still loading (or none)
    pm.fill(Qt::transparent);
  } else if (requestedSize.isValid() && (requestedSize.width() > 0 || requestedSize.height() > 0)) {
    pm = pm.scaled(requestedSize.width() > 0 ? requestedSize.width() : pm.width(),
                   requestedSize.height() > 0 ? requestedSize.height() : pm.height(), Qt::KeepAspectRatioByExpanding,
                   Qt::SmoothTransformation);
  }
  if (size)
    *size = pm.size();
  return pm;
}

} // namespace ui
