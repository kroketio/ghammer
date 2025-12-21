#include "texture_edit_sessions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QNetworkReply>

#include "client/ui/external_apps.h"
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtDebug>

#include "asset_client.h"
#include "client/prefs/preferences.h"
#include "client/ui/texture_actions.h"
#include "texture_catalog.h"

namespace net {

TextureEditSessions &TextureEditSessions::instance() {
  static TextureEditSessions *s = new TextureEditSessions();
  return *s;
}

TextureEditSessions::TextureEditSessions() : QObject(nullptr) {
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &TextureEditSessions::onDirChanged);
  connect(&m_watcher, &QFileSystemWatcher::fileChanged, this,
          [this](const QString &file) { onDirChanged(QFileInfo(file).absolutePath()); });
  // Lock taken over by someone else: tell the user, keep the file.
  connect(&TextureCatalog::instance(), &TextureCatalog::lockChanged, this, [this](const QString &ref) {
    const LockInfo *l = TextureCatalog::instance().lockFor(ref);
    if (!l || l->clientId == AssetClient::instance().clientId())
      return;
    for (auto &s: m_sessions)
      if (refKey(s.ref) == refKey(MaterialRef::parse(ref).base())) {
        s.state = tr("lock taken over by %1").arg(l->name);
        emit message(tr("%1 took over editing %2. Your file is kept; saving it now asks what to do.").arg(l->name, s.ref));
        emit sessionsChanged();
      }
  });
}

QString TextureEditSessions::workDir() {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/texture_edits");
}

bool TextureEditSessions::isEditing(const QString &ref) const {
  for (const auto &s: m_sessions)
    if (refKey(s.ref) == refKey(ref))
      return true;
  return false;
}

void TextureEditSessions::start(const QString &ref, const QString &mapType, QWidget *parent) {
  m_parent = parent;
  const TextureEntry *e = TextureCatalog::instance().find(ref);
  if (!e) {
    QMessageBox::warning(parent, tr("Edit texture"), tr("%1 is not on the asset server.").arg(ref));
    return;
  }
  if (e->base) {
    // Base packs are never modified.
    QMessageBox box(QMessageBox::Information, tr("Base textures are read-only"),
                    tr("%1 is in a base pack, which is never modified.\n\nMake a copy in your project pack and edit "
                       "that instead?")
                        .arg(e->ref),
                    QMessageBox::Cancel, parent);
    QPushButton *copy = box.addButton(tr("Copy into pack…"), QMessageBox::AcceptRole);
    box.setDefaultButton(copy);
    box.exec();
    if (box.clickedButton() == copy)
      emit ui::TextureHub::instance().copyRequested(e->ref);
    return;
  }
  if (m_sessions.contains(key(e->ref, mapType))) {
    launchEditor(m_sessions.value(key(e->ref, mapType)).path, parent);
    return;
  }
  lockThenDownload(e->ref, mapType, parent, false);
}

void TextureEditSessions::lockThenDownload(const QString &ref, const QString &mapType, QWidget *parent, bool takeOver) {
  QJsonObject args;
  args.insert(QStringLiteral("ref"), ref);
  args.insert(QStringLiteral("purpose"), QStringLiteral("edit %1").arg(mapType));
  if (takeOver)
    args.insert(QStringLiteral("take_over"), true);
  QPointer<QWidget> p(parent);
  AssetClient::instance().call(QStringLiteral("textures.lock"), args, this, [this, ref, mapType, p](const Result &r) {
    if (r.ok) {
      download(ref, mapType, p);
      return;
    }
    if (r.code != QLatin1String("locked")) {
      QMessageBox::warning(p, tr("Edit texture"), tr("Could not start editing: %1").arg(r.message));
      return;
    }
    const QString who = r.current.value(QStringLiteral("by")).toObject().value(QStringLiteral("name")).toString();
    QMessageBox box(QMessageBox::Question, tr("Someone is editing this texture"),
                    tr("%1 is editing %2 right now.").arg(who, ref), QMessageBox::Cancel, p);
    box.setInformativeText(tr("Make your own copy, or take over the edit lock (their next upload will then ask "
                              "them what to do)."));
    QPushButton *copy = box.addButton(tr("Make a copy instead"), QMessageBox::AcceptRole);
    QPushButton *take = box.addButton(tr("Take over"), QMessageBox::DestructiveRole);
    box.setDefaultButton(copy);
    box.exec();
    if (box.clickedButton() == copy)
      emit ui::TextureHub::instance().copyRequested(ref);
    else if (box.clickedButton() == take)
      lockThenDownload(ref, mapType, p, true);
  });
}

void TextureEditSessions::download(const QString &ref, const QString &mapType, QWidget *parent) {
  const MaterialRef r = MaterialRef::parse(ref);
  QPointer<QWidget> p(parent);
  AssetClient::instance().httpGet(
      QStringLiteral("/api/2/source/%1/%2/%3").arg(r.pack, r.name, mapType), this,
      [this, ref, mapType, p](QNetworkReply *reply) {
        if (reply->error() != QNetworkReply::NoError) {
          const Result res = AssetClient::resultFromReply(reply);
          QMessageBox::warning(p, tr("Edit texture"), tr("Could not download the map: %1").arg(res.message));
          AssetClient::instance().call(QStringLiteral("textures.unlock"), QJsonObject{{"ref", ref}}, this, {});
          return;
        }
        const QByteArray bytes = reply->readAll();
        // Names from the server never leave the work dir.
        const auto component = [](const QString &s) {
          const QString n = QFileInfo(s).fileName();
          return n.isEmpty() || n.startsWith(QLatin1Char('.')) ? QString() : n;
        };
        QString fileName = component(QString::fromUtf8(reply->rawHeader("X-File-Name")));
        if (fileName.isEmpty())
          fileName = mapType + QStringLiteral(".png");
        const qint64 rev = reply->rawHeader("X-Rev").toLongLong();
        const MaterialRef r = MaterialRef::parse(ref);
        const QString pack = component(r.pack), name = component(r.name);
        if (pack.isEmpty() || name.isEmpty()) {
          AssetClient::instance().call(QStringLiteral("textures.unlock"), QJsonObject{{"ref", ref}}, this, {});
          return;
        }
        const QString dir = workDir() + QStringLiteral("/%1/%2").arg(pack, name);
        QDir().mkpath(dir);
        const QString path = dir + QLatin1Char('/') + fileName;
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
          QMessageBox::warning(p, tr("Edit texture"), tr("Cannot write %1").arg(path));
          return;
        }
        EditSession s;
        s.ref = ref;
        s.mapType = mapType;
        s.path = path;
        s.rev = rev;
        s.size = QFileInfo(path).size();
        s.state = tr("editing");
        s.lastSync = QDateTime::currentDateTime();
        m_sessions.insert(key(ref, mapType), s);
        if (!m_watcher.directories().contains(dir))
          m_watcher.addPath(dir);
        m_watcher.addPath(path);
        emit sessionsChanged();
        emit message(tr("Editing %1 (%2). Save the file to upload it.").arg(ref, mapType));
        launchEditor(path, p);
      });
}

void TextureEditSessions::launchEditor(const QString &path, QWidget *parent) {
  ui::external::openImage(path, parent);
}

void TextureEditSessions::onDirChanged(const QString &dir) {
  for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
    if (QFileInfo(it->path).absolutePath() != QFileInfo(dir).absoluteFilePath() &&
        QFileInfo(it->path).absolutePath() != dir)
      continue;
    // GIMP replaces the file: watch the new inode too.
    if (QFileInfo::exists(it->path) && !m_watcher.files().contains(it->path))
      m_watcher.addPath(it->path);
    const QString k = it.key();
    QTimer *t = m_debounce.value(k);
    if (!t) {
      t = new QTimer(this);
      t->setSingleShot(true);
      t->setInterval(800);
      connect(t, &QTimer::timeout, this, [this, k] { checkStable(k); });
      m_debounce.insert(k, t);
    }
    t->start();
  }
}

void TextureEditSessions::checkStable(const QString &k) {
  auto it = m_sessions.find(k);
  if (it == m_sessions.end())
    return;
  const QFileInfo fi(it->path);
  if (!fi.exists())
    return;
  const qint64 size = fi.size();
  // Still being written? Look again shortly.
  QTimer::singleShot(300, this, [this, k, size] {
    auto it2 = m_sessions.find(k);
    if (it2 == m_sessions.end())
      return;
    const qint64 now = QFileInfo(it2->path).size();
    if (now != size || now <= 0) {
      if (QTimer *t = m_debounce.value(k))
        t->start();
      return;
    }
    if (now == it2->size && it2->state == tr("synced"))
      return;  // nothing new
    upload(k, false);
  });
}

void TextureEditSessions::put(const QString &ref, const QString &mapType, const QByteArray &bytes, qint64 ifRev,
                              QWidget *parent, std::function<void(bool, qint64)> done) {
  const MaterialRef r = MaterialRef::parse(ref);
  QHash<QByteArray, QByteArray> headers;
  if (ifRev >= 0)
    headers.insert("If-Match", QByteArray::number(ifRev));
  headers.insert("Content-Type", bytes.startsWith("\x89PNG") ? "image/png" : "image/jpeg");
  QPointer<QWidget> p(parent);
  AssetClient::instance().httpPut(
      QStringLiteral("/api/2/source/%1/%2/%3").arg(r.pack, r.name, mapType), bytes, headers, this,
      [this, ref, mapType, bytes, p, done](QNetworkReply *reply) {
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const Result res = AssetClient::resultFromReply(reply);
        if (res.ok) {
          const qint64 rev = qint64(res.data.value(QStringLiteral("entry")).toObject().value(QStringLiteral("rev")).toDouble());
          done(true, rev);
          return;
        }
        if (http == 409 || res.code == QLatin1String("conflict")) {
          QMessageBox box(QMessageBox::Warning, tr("Texture changed meanwhile"),
                          tr("Someone uploaded a new version of %1 (%2) after you started editing.").arg(ref, mapType),
                          QMessageBox::NoButton, p);
          QPushButton *mine = box.addButton(tr("Overwrite with mine"), QMessageBox::DestructiveRole);
          QPushButton *keep = box.addButton(tr("Keep theirs"), QMessageBox::RejectRole);
          box.setDefaultButton(keep);
          box.exec();
          if (box.clickedButton() == mine)
            put(ref, mapType, bytes, -1, p, done);
          else
            done(false, -1);
          return;
        }
        if (http == 423 || res.code == QLatin1String("locked")) {
          QMessageBox box(QMessageBox::Warning, tr("Edit lock lost"),
                          tr("Someone else holds the edit lock for %1 now, so your save was not uploaded.").arg(ref),
                          QMessageBox::NoButton, p);
          QPushButton *copy = box.addButton(tr("Save as a copy…"), QMessageBox::AcceptRole);
          box.addButton(QMessageBox::Close);
          box.exec();
          if (box.clickedButton() == copy)
            emit ui::TextureHub::instance().copyRequested(ref);
          done(false, -1);
          return;
        }
        QMessageBox::warning(p, tr("Upload failed"), tr("Could not upload %1: %2").arg(ref, res.message));
        done(false, -1);
      });
}

void TextureEditSessions::upload(const QString &k, bool force) {
  auto it = m_sessions.find(k);
  if (it == m_sessions.end())
    return;
  QFile f(it->path);
  if (!f.open(QIODevice::ReadOnly))
    return;
  const QByteArray bytes = f.readAll();
  it->state = tr("uploading");
  emit sessionsChanged();
  const qint64 size = bytes.size();
  put(it->ref, it->mapType, bytes, force ? -1 : it->rev, m_parent, [this, k, size](bool ok, qint64 rev) {
    auto it2 = m_sessions.find(k);
    if (it2 == m_sessions.end())
      return;
    if (ok) {
      it2->rev = rev;
      it2->size = size;
      it2->lastSync = QDateTime::currentDateTime();
      it2->state = tr("synced");
      emit message(tr("Uploaded %1 (%2)").arg(it2->ref, it2->mapType));
    } else {
      it2->state = tr("not uploaded");
    }
    emit sessionsChanged();
  });
}

void TextureEditSessions::uploadFile(const QString &ref, const QString &mapType, const QString &filePath,
                                     QWidget *parent) {
  QFile f(filePath);
  if (!f.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(parent, tr("Replace map"), tr("Cannot read %1").arg(filePath));
    return;
  }
  const TextureEntry *e = TextureCatalog::instance().find(ref);
  const QByteArray bytes = f.readAll();
  put(ref, mapType, bytes, e ? e->rev : -1, parent, [this, ref, mapType](bool ok, qint64) {
    if (ok)
      emit message(tr("Replaced the %1 map of %2").arg(mapType, ref));
  });
}

void TextureEditSessions::stop(const QString &ref, const QString &mapType) {
  const QString k = key(ref, mapType);
  const EditSession s = m_sessions.take(k);
  if (QTimer *t = m_debounce.take(k))
    t->deleteLater();
  if (!s.path.isEmpty())
    m_watcher.removePath(s.path);
  if (!isEditing(ref))
    AssetClient::instance().call(QStringLiteral("textures.unlock"), QJsonObject{{"ref", ref}}, this, {});
  emit sessionsChanged();
}

void TextureEditSessions::stopAll() {
  const auto all = m_sessions.values();
  for (const auto &s: all)
    stop(s.ref, s.mapType);
}

} // namespace net
