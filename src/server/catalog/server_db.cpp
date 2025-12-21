#include "server_db.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QMutex>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QtDebug>

namespace serverdb {

namespace {

QString s_path;
QMutex s_openMutex;

bool execOrWarn(QSqlQuery &q, const char *what) {
  if (q.exec())
    return true;
  qWarning() << "serverdb:" << what << q.lastError().text();
  return false;
}

void createSchema(QSqlDatabase &d) {
  QSqlQuery q(d);
  const char *stmts[] = {
      "CREATE TABLE IF NOT EXISTS image_info ("
      " path TEXT PRIMARY KEY, size INTEGER, mtime INTEGER, w INTEGER, h INTEGER,"
      " alpha_channel INTEGER, alpha_real INTEGER, channels INTEGER)",
      "CREATE TABLE IF NOT EXISTS material_props ("
      " texture_id TEXT PRIMARY KEY, ref TEXT, props TEXT, rev INTEGER, updated_at INTEGER)",
      "CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT)",
  };
  for (const char *s: stmts)
    if (!q.exec(QString::fromLatin1(s)))
      qWarning() << "serverdb: schema" << q.lastError().text();
}

} // namespace

bool init(const QString &path) {
  s_path = path;
  QSqlDatabase d = db();
  if (!d.isOpen())
    return false;
  createSchema(d);
  QSqlQuery q(d);
  q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
  return true;
}

QSqlDatabase db() {
  const QString name = QStringLiteral("serverdb_%1").arg(quintptr(QThread::currentThreadId()));
  if (QSqlDatabase::contains(name))
    return QSqlDatabase::database(name);
  QMutexLocker lock(&s_openMutex);
  QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
  d.setDatabaseName(s_path);
  d.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=10000"));
  if (!d.open())
    qWarning() << "serverdb: cannot open" << s_path << d.lastError().text();
  return d;
}

QHash<QString, ImageInfo> loadImageInfo() {
  QHash<QString, ImageInfo> out;
  QSqlQuery q(db());
  q.setForwardOnly(true);
  if (!q.exec(QStringLiteral("SELECT path, size, mtime, w, h, alpha_channel, alpha_real, channels FROM image_info")))
    return out;
  while (q.next()) {
    ImageInfo i;
    i.size = q.value(1).toLongLong();
    i.mtime = q.value(2).toLongLong();
    i.w = q.value(3).toInt();
    i.h = q.value(4).toInt();
    i.alphaChannel = q.value(5).toBool();
    i.alphaReal = q.value(6).isNull() ? -1 : q.value(6).toInt();
    i.channels = q.value(7).toInt();
    out.insert(q.value(0).toString(), i);
  }
  return out;
}

static void bindImageInfo(QSqlQuery &q, const QString &path, const ImageInfo &i) {
  q.bindValue(0, path);
  q.bindValue(1, i.size);
  q.bindValue(2, i.mtime);
  q.bindValue(3, i.w);
  q.bindValue(4, i.h);
  q.bindValue(5, i.alphaChannel ? 1 : 0);
  q.bindValue(6, i.alphaReal);
  q.bindValue(7, i.channels);
}

void storeImageInfo(const QHash<QString, ImageInfo> &items) {
  if (items.isEmpty())
    return;
  QSqlDatabase d = db();
  d.transaction();
  QSqlQuery q(d);
  q.prepare(QStringLiteral("INSERT OR REPLACE INTO image_info (path, size, mtime, w, h, alpha_channel, alpha_real, "
                           "channels) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
  for (auto it = items.begin(); it != items.end(); ++it) {
    bindImageInfo(q, it.key(), it.value());
    execOrWarn(q, "image_info");
  }
  d.commit();
}

void storeImageInfo(const QString &relPath, const ImageInfo &info) {
  QSqlQuery q(db());
  q.prepare(QStringLiteral("INSERT OR REPLACE INTO image_info (path, size, mtime, w, h, alpha_channel, alpha_real, "
                           "channels) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"));
  bindImageInfo(q, relPath, info);
  execOrWarn(q, "image_info");
}

QHash<QString, PropsRow> loadProps() {
  QHash<QString, PropsRow> out;
  QSqlQuery q(db());
  q.setForwardOnly(true);
  if (!q.exec(QStringLiteral("SELECT texture_id, ref, props, rev FROM material_props")))
    return out;
  while (q.next()) {
    PropsRow r;
    r.ref = q.value(1).toString();
    r.props = MaterialProps::fromJson(QJsonDocument::fromJson(q.value(2).toByteArray()).object());
    r.rev = q.value(3).toLongLong();
    out.insert(q.value(0).toString(), r);
  }
  return out;
}

bool storeProps(const QString &textureId, const QString &ref, const MaterialProps &props, qint64 rev) {
  QSqlQuery q(db());
  q.prepare(QStringLiteral(
      "INSERT OR REPLACE INTO material_props (texture_id, ref, props, rev, updated_at) VALUES (?, ?, ?, ?, ?)"));
  q.bindValue(0, textureId);
  q.bindValue(1, ref);
  q.bindValue(2, QString::fromUtf8(QJsonDocument(props.toJson()).toJson(QJsonDocument::Compact)));
  q.bindValue(3, rev);
  q.bindValue(4, QDateTime::currentSecsSinceEpoch());
  return execOrWarn(q, "store props");
}

bool deleteProps(const QString &textureId) {
  QSqlQuery q(db());
  q.prepare(QStringLiteral("DELETE FROM material_props WHERE texture_id = ?"));
  q.bindValue(0, textureId);
  return execOrWarn(q, "delete props");
}

qint64 metaInt(const QString &key, qint64 fallback) {
  QSqlQuery q(db());
  q.prepare(QStringLiteral("SELECT value FROM meta WHERE key = ?"));
  q.bindValue(0, key);
  if (q.exec() && q.next())
    return q.value(0).toLongLong();
  return fallback;
}

void setMetaInt(const QString &key, qint64 value) {
  QSqlQuery q(db());
  q.prepare(QStringLiteral("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)"));
  q.bindValue(0, key);
  q.bindValue(1, QString::number(value));
  execOrWarn(q, "meta");
}

} // namespace serverdb
