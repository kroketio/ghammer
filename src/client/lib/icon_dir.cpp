#include "icon_dir.h"

#ifdef GHAMMER_ICON_DIR
#include <QDebug>
#include <QtCore/private/qabstractfileengine_p.h>
#include <QtCore/private/qfsfileengine_p.h>

namespace {
class IconDirHandler final : public QAbstractFileEngineHandler {
public:
  std::unique_ptr<QAbstractFileEngine> create(const QString &fileName) const override {
    if (!fileName.startsWith(QLatin1String(":/icons_blender/")) && !fileName.startsWith(QLatin1String(":/icons_godot/")))
      return {};
    return std::make_unique<QFSFileEngine>(QStringLiteral(GHAMMER_ICON_DIR) + fileName.mid(1));
  }
};
}

void icons::installIconDir() {
  static IconDirHandler handler;
  qInfo().noquote() << QStringLiteral("[icons] reading from %1").arg(QStringLiteral(GHAMMER_ICON_DIR));
}
#else
void icons::installIconDir() {}
#endif
