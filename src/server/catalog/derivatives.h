#pragma once

// Generated images the editor downloads instead of full-resolution sources:
//   cache/thumbs/<checksum>_256.jpg|png   256 px, texture browser thumbnails
//   cache/editor/<checksum>_512.jpg|png   512 px (longest edge), drawn on faces
//
// Built on a dedicated thread pool at startup for every missing file, and on demand (ensure()). Names change with
// the source checksum, so a stale derivative is never served.
//
// DATA SAFETY: this only ever writes and deletes inside cache/thumbs and
// cache/editor. Source images are opened read-only.

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QWaitCondition>

#include <atomic>

#include "catalog.h"

class DerivativeBuilder : public QObject {
  Q_OBJECT

public:
  static DerivativeBuilder &instance();

  void setThreadCount(int n);

  // Background build; jobs for the same source are decoded once.
  void enqueue(const QVector<DerivativeSource> &jobs);

  // Builds `src` now unless it exists (waits if a worker is already on it).
  bool ensure(const DerivativeSource &src, QString *error = nullptr);

  [[nodiscard]] QString pathFor(const DerivativeSource &src) const;

  struct Progress {
    qint64 queued = 0;
    qint64 done = 0;
    qint64 failed = 0;
  };
  [[nodiscard]] Progress progress() const;

  // Deletes unreferenced files in cache/thumbs and cache/editor. Skipped when the catalogue load reported errors
  // (a pack that failed to load would lose its derivatives).
  int collectGarbage(const QSet<QString> &keep);

signals:
  void progressChanged();

private:
  DerivativeBuilder();
  bool build(const QVector<DerivativeSource> &targets, QString *error, bool counted);
  // Skybox panoramas (.exr): the tone-mapped thumbnail and the sky file.
  bool buildPanorama(const QVector<DerivativeSource> &todo, QString *error, bool counted);
  void claim(const QString &key);  // waits while another thread builds it
  void release(const QString &file);

  QThreadPool m_pool;
  mutable QMutex m_mutex;
  QWaitCondition m_doneCond;
  QSet<QString> m_inFlight;
  std::atomic<qint64> m_queued{0};
  std::atomic<qint64> m_done{0};
  std::atomic<qint64> m_failed{0};
  QTimer m_logTimer;
  qint64 m_lastLoggedDone = -1;
};
