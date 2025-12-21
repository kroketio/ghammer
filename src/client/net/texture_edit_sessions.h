#pragma once

// Editing texture maps in an external image editor: take the server's soft edit lock, download the source map
// to a work folder, open it, watch the folder (GIMP saves by write + rename) and upload once the file stops
// changing, with If-Match: <revision>. Conflicts and lost locks ask instead of overwriting. Base packs are read-only.

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

class QWidget;

namespace net {

struct EditSession {
  QString ref;      // "pack/Name"
  QString mapType;  // "color", "normal", ...
  QString path;     // local working file
  qint64 rev = 0;   // revision the file is based on
  qint64 size = -1;
  QDateTime lastSync;
  QString state;    // "editing", "uploading", "synced", "error: ..."
};

class TextureEditSessions final : public QObject {
  Q_OBJECT

public:
  static TextureEditSessions &instance();

  // Asks about locks / base packs with `parent`.
  void start(const QString &ref, const QString &mapType, QWidget *parent);
  void stop(const QString &ref, const QString &mapType);
  void stopAll();
  [[nodiscard]] QList<EditSession> sessions() const { return m_sessions.values(); }
  [[nodiscard]] bool isEditing(const QString &ref) const;

  // Replaces (or adds) a map from a file on disk, with the same revision check.
  void uploadFile(const QString &ref, const QString &mapType, const QString &filePath, QWidget *parent);

  [[nodiscard]] static QString workDir();

signals:
  void sessionsChanged();
  void message(const QString &text);

private:
  TextureEditSessions();
  static QString key(const QString &ref, const QString &mapType) { return ref.toLower() + QLatin1Char('|') + mapType; }
  void lockThenDownload(const QString &ref, const QString &mapType, QWidget *parent, bool takeOver);
  void download(const QString &ref, const QString &mapType, QWidget *parent);
  void launchEditor(const QString &path, QWidget *parent);
  void onDirChanged(const QString &dir);
  void checkStable(const QString &k);
  void upload(const QString &k, bool force);
  void put(const QString &ref, const QString &mapType, const QByteArray &bytes, qint64 ifRev, QWidget *parent,
           std::function<void(bool ok, qint64 newRev)> done);

  QHash<QString, EditSession> m_sessions;
  QHash<QString, QTimer *> m_debounce;
  QFileSystemWatcher m_watcher;
  QPointer<QWidget> m_parent;
};

} // namespace net
