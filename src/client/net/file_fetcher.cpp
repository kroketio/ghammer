#include "file_fetcher.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtConcurrent>
#include <QtDebug>

#include <algorithm>

#include "asset_client.h"
#include "client/prefs/preferences.h"
#include "shared/lib/file_packer.h"

namespace net {

namespace {

constexpr int kBatch = 48;
constexpr int kMaxConcurrent = 3;
constexpr qint64 kFailBackoffMs = 30000;

QString keyOf(FileKind kind, const QString &file) {
  return (kind == FileKind::Thumb ? QStringLiteral("t/") : QStringLiteral("e/")) + file;
}

bool safeName(const QString &file) {
  return !file.isEmpty() && !file.contains(QLatin1Char('/')) && !file.contains(QLatin1Char('\\')) &&
         !file.contains(QLatin1String(".."));
}

} // namespace

FileFetcher &FileFetcher::instance() {
  static FileFetcher *f = new FileFetcher();
  return *f;
}

FileFetcher::FileFetcher() : QObject(nullptr) {
  m_root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/textures");
  QDir().mkpath(dirFor(FileKind::Thumb));
  QDir().mkpath(dirFor(FileKind::Editor));
  m_flush.setSingleShot(true);
  m_flush.setInterval(40);
  connect(&m_flush, &QTimer::timeout, this, &FileFetcher::flush);
}

QString FileFetcher::dirFor(FileKind kind) const {
  return m_root + (kind == FileKind::Thumb ? QStringLiteral("/thumbs") : QStringLiteral("/editor"));
}

QString FileFetcher::cachedPath(FileKind kind, const QString &file) const {
  if (!safeName(file))
    return {};
  const QString p = dirFor(kind) + QLatin1Char('/') + file;
  return QFileInfo::exists(p) ? p : QString();
}

void FileFetcher::fetch(FileKind kind, const QString &file, FetchPriority prio, QObject *context, DoneFn fn) {
  if (!safeName(file)) {
    if (fn)
      QTimer::singleShot(0, context ? context : this, [fn] { fn(QString()); });
    return;
  }
  if (const QString p = cachedPath(kind, file); !p.isEmpty()) {
    if (fn)
      QTimer::singleShot(0, context ? context : this, [fn, p] { fn(p); });
    return;
  }
  const QString key = keyOf(kind, file);
  if (fn)
    m_waiters[key].push_back(Waiter{context, context != nullptr, std::move(fn)});
  if (m_requested.contains(key))
    return;
  const auto failed = m_failedAt.constFind(key);
  if (failed != m_failedAt.constEnd() && QDateTime::currentMSecsSinceEpoch() - *failed < kFailBackoffMs) {
    QTimer::singleShot(0, this, [this, kind, file] { finish(kind, file, QString()); });
    return;
  }
  auto it = m_queuedPrio.find(key);
  if (it == m_queuedPrio.end())
    m_queuedPrio.insert(key, prio);
  else if (int(prio) < int(*it))
    *it = prio;
  schedule();
}

void FileFetcher::schedule() {
  if (!m_flush.isActive())
    m_flush.start();
}

void FileFetcher::flush() {
  if (m_queuedPrio.isEmpty() || m_activeRequests >= kMaxConcurrent)
    return;
  if (!AssetClient::instance().httpBase().isValid())
    return;
  // Pick the most urgent batch of one kind.
  std::vector<std::pair<FetchPriority, QString>> order;
  order.reserve(m_queuedPrio.size());
  for (auto it = m_queuedPrio.begin(); it != m_queuedPrio.end(); ++it)
    order.emplace_back(it.value(), it.key());
  std::stable_sort(order.begin(), order.end(),
                   [](const auto &a, const auto &b) { return int(a.first) < int(b.first); });
  const QString kindPrefix = order.front().second.left(2);
  const FileKind kind = kindPrefix == QLatin1String("t/") ? FileKind::Thumb : FileKind::Editor;
  QStringList files;
  QStringList keys;
  for (const auto &[prio, key]: order) {
    if (!key.startsWith(kindPrefix))
      continue;
    files.append(key.mid(2));
    keys.append(key);
    if (files.size() >= kBatch)
      break;
  }
  for (const auto &k: keys) {
    m_queuedPrio.remove(k);
    m_requested.insert(k);
  }

  QJsonObject body;
  body.insert(QStringLiteral("kind"), kind == FileKind::Thumb ? QStringLiteral("thumb") : QStringLiteral("editor"));
  body.insert(QStringLiteral("files"), QJsonArray::fromStringList(files));
  ++m_activeRequests;
  const QString dir = dirFor(kind);
  AssetClient::instance().httpPost(
      QStringLiteral("/api/2/files"), QJsonDocument(body).toJson(QJsonDocument::Compact), this,
      [this, kind, files, dir](QNetworkReply *reply) {
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QByteArray data = ok ? reply->readAll() : QByteArray();
        if (!ok)
          qWarning().noquote() << "[server] file download failed:" << reply->errorString();
        auto future = QtConcurrent::run([data, dir]() {
          QHash<QString, QString> written;
          for (const PackedFile &pf: unpackFiles(data)) {
            if (!safeName(pf.fileName))
              continue;
            const QString path = dir + QLatin1Char('/') + pf.fileName;
            QSaveFile f(path);
            if (f.open(QIODevice::WriteOnly) && f.write(pf.data) == pf.data.size() && f.commit())
              written.insert(pf.fileName, path);
          }
          return written;
        });
        future.then(this, [this, kind, files](const QHash<QString, QString> &written) {
          --m_activeRequests;
          for (const auto &f: files)
            finish(kind, f, written.value(f));
          if (!m_queuedPrio.isEmpty())
            schedule();
        });
      });
  if (!m_queuedPrio.isEmpty() && m_activeRequests < kMaxConcurrent)
    schedule();
}

void FileFetcher::finish(FileKind kind, const QString &file, const QString &path) {
  const QString key = keyOf(kind, file);
  m_requested.remove(key);
  if (path.isEmpty())
    m_failedAt.insert(key, QDateTime::currentMSecsSinceEpoch());
  else
    m_failedAt.remove(key);
  const auto waiters = m_waiters.take(key);
  for (const auto &w: waiters)
    if (w.fn && (!w.hasContext || w.context))
      w.fn(path);
  if (!path.isEmpty())
    emit fileReady(kind, file, path);
}

void FileFetcher::trimCache() {
  const qint64 limit = qint64(std::max(128, prefs::i("Textures/CacheLimitMB"))) * 1024 * 1024;
  const QString root = m_root;
  (void)QtConcurrent::run([root, limit] {
    struct F {
      QString path;
      qint64 size;
      qint64 time;
    };
    std::vector<F> files;
    qint64 total = 0;
    for (const QString &sub: {QStringLiteral("/thumbs"), QStringLiteral("/editor")}) {
      QDirIterator it(root + sub, QDir::Files);
      while (it.hasNext()) {
        it.next();
        const QFileInfo fi = it.fileInfo();
        const QDateTime t = fi.lastRead().isValid() ? fi.lastRead() : fi.lastModified();
        files.push_back({fi.absoluteFilePath(), fi.size(), t.toMSecsSinceEpoch()});
        total += fi.size();
      }
    }
    if (total <= limit)
      return;
    std::sort(files.begin(), files.end(), [](const F &a, const F &b) { return a.time < b.time; });
    const qint64 target = limit * 9 / 10;
    int removed = 0;
    for (const auto &f: files) {
      if (total <= target)
        break;
      // Only our own download cache, never anything else.
      if (!f.path.startsWith(root))
        continue;
      if (QFile::remove(f.path)) {
        total -= f.size;
        ++removed;
      }
    }
    qInfo().noquote() << QStringLiteral("[server] trimmed %1 cached texture files").arg(removed);
  });
}

} // namespace net
