#pragma once

#include <QQuickImageProvider>

#include "client/ui/texture_ui.h"

namespace ui {

// image://thumb/<thumbId>[?r=<n>] from ThumbCache; bumping r makes QML reload (Images set cache: false).
// A Pixmap provider: runs on the GUI thread, as ThumbCache must.
class ThumbImageProvider final : public QQuickImageProvider {
public:
  ThumbImageProvider() : QQuickImageProvider(QQuickImageProvider::Pixmap) {}
  QPixmap requestPixmap(const QString &id, QSize *size, const QSize &requestedSize) override;

  // "s/<file>" (server) or "l/<path>" (local library), percent-encoded.
  static QString thumbId(const ThumbSource &source);
};

} // namespace ui
