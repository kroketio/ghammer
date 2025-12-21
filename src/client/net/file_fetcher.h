#pragma once

// Downloads server-generated images (thumbnails, editor maps) into a disk cache.
// File names carry the source checksum, so a cached file never goes stale.
// Requests are merged and batched (one POST /api/2/files per <=48 files), served by priority.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>

#include <functional>
#include <map>
#include <vector>

namespace net {

enum class FileKind { Thumb, Editor };

enum class FetchPriority { Visible = 0, Scene = 1, Prefetch = 2 };

class FileFetcher final : public QObject {
  Q_OBJECT

public:
  using DoneFn = std::function<void(const QString &path)>;  // empty path = failed

  static FileFetcher &instance();

  // Local path when already cached, else empty. Never blocks on the network.
  [[nodiscard]] QString cachedPath(FileKind kind, const QString &file) const;

  // Calls `fn` on the main thread; dropped if `context` dies first.
  void fetch(FileKind kind, const QString &file, FetchPriority prio, QObject *context, DoneFn fn);


  // Removes the oldest cached files until the cache fits Textures/CacheLimitMB.
  void trimCache();

signals:
  void fileReady(net::FileKind kind, const QString &file, const QString &path);

private:
  FileFetcher();
  void schedule();
  void flush();
  void finish(FileKind kind, const QString &file, const QString &path);
  [[nodiscard]] QString dirFor(FileKind kind) const;

  struct Waiter {
    QPointer<QObject> context;
    bool hasContext = false;
    DoneFn fn;
  };
  struct Wanted {
    FileKind kind;
    QString file;
    FetchPriority prio;
  };

  QString m_root;
  QHash<QString, std::vector<Waiter>> m_waiters;  // key -> callbacks
  QHash<QString, FetchPriority> m_queuedPrio;     // key -> best priority (queued, not yet requested)
  QSet<QString> m_requested;                      // keys in an HTTP request right now
  QHash<QString, qint64> m_failedAt;              // key -> ms since epoch (back off)
  QTimer m_flush;
  int m_activeRequests = 0;
};

} // namespace net
