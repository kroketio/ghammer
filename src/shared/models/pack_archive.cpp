#include "pack_archive.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include "shared/models/texture_entry.h"

namespace {

constexpr qint64 kMaxJson = qint64(64) << 20;

PackArchive failed(const QString &error) {
  PackArchive p;
  p.error = error;
  return p;
}

bool unsafeName(const QString &name) {
  if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
      name.contains(QLatin1Char(':')) || name.contains(QChar(0)))
    return true;
  for (const QStringView part: QStringView(name).split(QLatin1Char('/')))
    if (part == QLatin1String(".."))
      return true;
  return false;
}

bool validFolder(const QString &f) {
  static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-][A-Za-z0-9_.-]{0,79}$"));
  return re.match(f).hasMatch() && !f.contains(QLatin1String(".."));
}

} // namespace

QString packTypeLabel(const QString &type) {
  return type == QLatin1String("lut") ? QCoreApplication::translate("PackArchive", "LUT pack")
                                      : QCoreApplication::translate("PackArchive", "Texture pack");
}

PackArchive inspectPackArchive(const QString &zipPath) {
  ZipReader zip(zipPath);
  return inspectPackArchive(zip);
}

PackArchive inspectPackArchive(ZipReader &zip) {
  if (!zip.isOpen())
    return failed(zip.error());

  const ZipEntry *json = nullptr;
  for (const ZipEntry &e: zip.entries()) {
    if (unsafeName(e.name))
      return failed(QStringLiteral("unsafe file name in the archive: %1").arg(e.name));
    if (!e.name.contains(QLatin1Char('/')) && e.name.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)) {
      if (json)
        return failed(QStringLiteral("more than one pack JSON at the top of the archive"));
      json = &e;
    }
  }
  if (!json)
    return failed(QStringLiteral("no pack JSON at the top of the archive"));

  QByteArray bytes;
  if (!zip.readFile(*json, bytes, kMaxJson))
    return failed(zip.error());
  QJsonParseError pe{};
  const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
  if (pe.error != QJsonParseError::NoError || !doc.isObject())
    return failed(QStringLiteral("%1 is not valid JSON: %2").arg(json->name, pe.errorString()));

  PackArchive p;
  p.json = doc.object();
  p.jsonEntry = json->name;
  if (!p.json.value(QStringLiteral("meta")).isObject())
    return failed(QStringLiteral("%1 has no \"meta\" object").arg(json->name));
  p.name = p.json.value(QStringLiteral("name")).toString(QFileInfo(json->name).completeBaseName());
  if (!isValidAssetName(p.name))
    return failed(QStringLiteral("invalid pack name '%1' (letters, digits, '_' and '-')").arg(p.name));
  p.type = p.json.value(QStringLiteral("type")).toString(QStringLiteral("texture"));
  if (p.type != QLatin1String("texture") && p.type != QLatin1String("lut"))
    return failed(QStringLiteral("unknown pack type '%1'").arg(p.type));
  p.description = p.json.value(QStringLiteral("description")).toString();
  p.folder = p.json.value(QStringLiteral("path")).toString(p.name);
  if (!validFolder(p.folder))
    return failed(QStringLiteral("invalid pack folder '%1'").arg(p.folder));

  const QString prefix = p.folder + QLatin1Char('/');
  QSet<QString> assets;
  QSet<QString> seen;
  for (const ZipEntry &e: zip.entries()) {
    if (e.isDir() || !e.name.startsWith(prefix))
      continue;
    const QString leaf = e.name.mid(prefix.size());
    if (leaf.isEmpty() || leaf.contains(QLatin1Char('/')) || leaf.startsWith(QLatin1Char('.')))
      continue;
    if (seen.contains(leaf.toLower()))
      return failed(QStringLiteral("%1 is in the archive twice").arg(e.name));
    seen.insert(leaf.toLower());
    p.files.append(e);
    p.bytes += e.size;
    if (p.type == QLatin1String("lut")) {
      const QString base = QFileInfo(leaf).completeBaseName();
      if (leaf.endsWith(QLatin1String(".png"), Qt::CaseInsensitive) && isValidAssetName(base))
        assets.insert(base);
    } else {
      const ParsedMapFile m = parseMapFileName(leaf);
      if (m.ok && m.variant.isEmpty() && (m.type == MapType::Color || m.type == MapType::Panorama))
        assets.insert(m.textureName);
    }
  }
  if (p.files.isEmpty())
    return failed(QStringLiteral("the pack folder '%1' is missing or empty").arg(p.folder));
  p.assets = int(assets.size());
  if (p.assets == 0)
    return failed(p.type == QLatin1String("lut") ? QStringLiteral("no LUTs (<Name>.png) in '%1'").arg(p.folder)
                                                 : QStringLiteral("no textures or skyboxes in '%1'").arg(p.folder));
  p.bytes += json->size;
  p.ok = true;
  return p;
}
