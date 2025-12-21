#include "installer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QThreadPool>
#include <QUuid>
#include <QtConcurrent>
#include <QtDebug>

#include "catalog.h"
#include "shared/models/pack_archive.h"

namespace PackInstall {

namespace {

constexpr qint64 kSpareBytes = qint64(512) << 20;

QThreadPool &pool() {
  static QThreadPool *p = [] {
    auto *tp = new QThreadPool;
    tp->setMaxThreadCount(1);
    tp->setExpiryTimeout(-1);
    return tp;
  }();
  return *p;
}

qint64 freeBytes() {
  QStorageInfo info(Catalog::instance().texturesDir());
  info.refresh();
  return info.isValid() ? info.bytesAvailable() : -1;
}

MutationResult installNow(const QString &zipPath, const ClientRef &by) {
  const QString staging = QDir(incomingDir()).absoluteFilePath(QUuid::createUuid().toString(QUuid::Id128));
  auto cleanup = qScopeGuard([&] {
    QFile::remove(zipPath);
    QDir(staging).removeRecursively();
  });

  ZipReader zip(zipPath);
  const PackArchive a = inspectPackArchive(zip);
  if (!a.ok)
    return MutationResult::fail(QStringLiteral("invalid"), QStringLiteral("not a valid asset pack: %1").arg(a.error));
  Catalog &cat = Catalog::instance();
  if (cat.pack(a.name, a.type))
    return MutationResult::fail(QStringLiteral("exists"),
                                QStringLiteral("%1 '%2' is already installed").arg(packTypeLabel(a.type), a.name), 409);
  const qint64 room = freeBytes();
  if (room >= 0 && room < qint64(a.bytes) + kSpareBytes)
    return MutationResult::fail(QStringLiteral("no_space"), QStringLiteral("not enough disk space on the asset server"),
                                507);

  const QString packDir = QDir(staging).absoluteFilePath(QStringLiteral("pack"));
  if (!QDir().mkpath(packDir))
    return MutationResult::fail(QStringLiteral("io"), QStringLiteral("cannot create %1").arg(packDir), 500);
  for (const ZipEntry &e: a.files) {
    QFile out(QDir(packDir).absoluteFilePath(e.name.mid(a.folder.size() + 1)));
    if (!out.open(QIODevice::WriteOnly) || !zip.extract(e, out))
      return MutationResult::fail(QStringLiteral("io"),
                                  QStringLiteral("cannot extract %1: %2").arg(e.name, zip.error()), 500);
  }

  const MutationResult placed = Mutations::instance().placeInstalledPack(packDir, a.json, a.name, a.type).result();
  if (!placed.ok)
    return placed;
  const QString dir = placed.data.value(QStringLiteral("dir")).toString();
  const QString json = placed.data.value(QStringLiteral("json")).toString();
  const PackScan scan = cat.scanPackFile(json);
  if (!scan.error.isEmpty() || scan.entries.isEmpty()) {
    Mutations::instance().abandonInstalledPack(a.name, a.type, dir, json).waitForFinished();
    return MutationResult::fail(QStringLiteral("invalid"),
                                scan.error.isEmpty() ? QStringLiteral("the pack has no usable assets") : scan.error);
  }
  return Mutations::instance().addInstalledPack(scan, by).result();
}

} // namespace

QString incomingDir() {
  return QDir(Catalog::instance().texturesDir()).absoluteFilePath(QStringLiteral(".incoming"));
}

void cleanIncoming() {
  QDir dir(incomingDir());
  if (dir.exists())
    dir.removeRecursively();
  QDir().mkpath(incomingDir());
}

bool hasRoomFor(qint64 bytes) {
  const qint64 room = freeBytes();
  return room < 0 || room >= 2 * bytes + kSpareBytes;
}

MutationResult precheck(const QString &name, const QString &type, qint64 bytes) {
  if (!isValidAssetName(name) || (type != QLatin1String("texture") && type != QLatin1String("lut")))
    return MutationResult::fail(QStringLiteral("invalid"), QStringLiteral("invalid pack name or type"));
  if (Catalog::instance().pack(name, type))
    return MutationResult::fail(QStringLiteral("exists"),
                                QStringLiteral("%1 '%2' is already installed").arg(packTypeLabel(type), name), 409);
  if (bytes > 0 && !hasRoomFor(bytes))
    return MutationResult::fail(QStringLiteral("no_space"), QStringLiteral("not enough disk space on the asset server"),
                                507);
  return {};
}

QFuture<MutationResult> install(const QString &zipPath, const ClientRef &by) {
  return QtConcurrent::run(&pool(), [zipPath, by] {
    try {
      return installNow(zipPath, by);
    } catch (const std::exception &) {
      QFile::remove(zipPath);
      return MutationResult::fail(QStringLiteral("io"), QStringLiteral("install failed"), 500);
    }
  });
}

} // namespace PackInstall
