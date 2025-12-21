#include "material.h"

#include <QSet>

namespace eng {

Material::Material(const QString &name) : name(name), ref(name) {
}

bool isDevMaterial(const QString &name, const QString &defaultPref) {
  const QString key = name.trimmed().toLower();
  static const QSet<QString> names = {QStringLiteral("devtexture01"), QStringLiteral("dev/dev_measurewall01d"),
                                      QStringLiteral("dev_measurewall01d"), QStringLiteral("dev/dev_measurewall01a")};
  if (names.contains(key))
    return true;
  const QString pref = defaultPref.trimmed().toLower();
  return !pref.isEmpty() && key == pref;
}

} // namespace eng
