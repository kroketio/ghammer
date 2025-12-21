#pragma once

// Every catalogue change goes through here, on ONE thread: changes are totally ordered and each gets the next
// catalogue revision. A mutation checks the base-pack guard, `if_rev` (optimistic concurrency) and locks; changes
// files / sqlite / the in-memory catalogue; then bumps the revision, journals the event and broadcasts it.
// Reconnecting editors send their last revision: eventsSince() replays what they missed while the journal covers it,
// otherwise they resync.
//
// DATA SAFETY (there is no backup of the texture files):
//   - base packs are never written (requireEditable)
//   - nothing under data/textures is ever deleted: user-pack deletes move files
//     to data/textures/.trash/, replaced maps move to data/textures/.history/
//   - files are only created with QSaveFile / QFile::copy onto new names

#include <QFuture>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

#include <deque>
#include <functional>

#include "catalog.h"

// A new pack's directory and JSON in data/textures: "<name>" + "<name>.json", or "<name>.<type>" + "<name>.<type>.json"
// when a pack of another type already uses the first. False when neither is free.
bool freePackPaths(const QString &name, const QString &type, QString *dir, QString *json);

struct ClientRef {
  int id = 0;
  QString name;
  [[nodiscard]] QJsonObject toJson() const;
};

struct MutationResult {
  bool ok = true;
  QString code;      // read_only | conflict | not_found | locked | invalid | exists | io
  QString message;
  QJsonObject data;  // reply payload
  QJsonObject current;  // for conflicts: the current entry / pack
  int httpStatus = 200;

  static MutationResult fail(const QString &code, const QString &message, int http = 400);
};

class Mutations : public QObject {
  Q_OBJECT

public:
  static Mutations &instance();

  // Runs `fn` on the mutation thread. Events it records are broadcast before
  // the future resolves.
  QFuture<MutationResult> submit(std::function<MutationResult()> fn);

  // Called by the hub (any thread).
  void clientConnected(const ClientRef &c);
  void clientDisconnected(int clientId);
  [[nodiscard]] QString clientName(int clientId) const;

  // Journal: catalogue events with rev > `rev`. `covered` is false when the
  // journal no longer reaches back that far (the client must resync).
  [[nodiscard]] QJsonArray eventsSince(qint64 rev, bool *covered) const;
  [[nodiscard]] QJsonArray locksJson() const;

  // operations (each wraps itself in submit())
  QFuture<MutationResult> createPack(const QJsonObject &args, const ClientRef &by);
  QFuture<MutationResult> updatePack(const QJsonObject &args, const ClientRef &by);
  QFuture<MutationResult> deletePack(const QJsonObject &args, const ClientRef &by);
  QFuture<MutationResult> copyTexture(const QJsonObject &args, const ClientRef &by);
  QFuture<MutationResult> updateTexture(const QJsonObject &args, const ClientRef &by);
  QFuture<MutationResult> deleteTexture(const QJsonObject &args, const ClientRef &by);
  QFuture<MutationResult> setProps(const QJsonObject &args, const ClientRef &by);
  QFuture<MutationResult> clearProps(const QJsonObject &args, const ClientRef &by);
  QFuture<MutationResult> lockTexture(const QJsonObject &args, const ClientRef &by);
  QFuture<MutationResult> unlockTexture(const QJsonObject &args, const ClientRef &by);
  QFuture<MutationResult> uploadMap(const QString &ref, const QString &mapType, const QByteArray &bytes,
                                    qint64 ifRev, const ClientRef &by);
  // Creates or replaces a colour LUT "<name>.png" in a user "lut" pack. `grade` (may be empty) is the adjustment
  // stack it was made from; without one the LUT counts as imported and a replaced strip goes to .history.
  QFuture<MutationResult> putLut(const QString &pack, const QString &name, const QByteArray &png,
                                 const QJsonObject &grade, qint64 ifRev, const ClientRef &by);

  // Installing an uploaded pack (installer.cpp): moves the extracted folder into data/textures under a free name and
  // writes its JSON there (the name + type stays reserved until add / abandon), then adds the scanned pack.
  QFuture<MutationResult> placeInstalledPack(const QString &stagedDir, const QJsonObject &json, const QString &name,
                                             const QString &type);
  QFuture<MutationResult> addInstalledPack(const PackScan &scan, const ClientRef &by);
  // A placed pack that could not be added: its files go to the trash.
  QFuture<MutationResult> abandonInstalledPack(const QString &name, const QString &type, const QString &dir,
                                               const QString &jsonPath);

  // Broadcast hook, set by the hub: called (from the mutation thread) with every
  // event, catalogue or not.
  std::function<void(const QJsonObject &)> broadcast;

  static constexpr int kJournalSize = 10000;
  static constexpr int kLockGraceSeconds = 60;

private:
  Mutations();

  // mutation-thread helpers
  void emitEvent(const QString &ev, qint64 rev, const ClientRef &by, const QJsonObject &data, bool journal = true);
  MutationResult requireEditable(const QString &packName, const QString &type = QStringLiteral("texture")) const;
  bool writePackJson(const CatalogPack &pack, QString *error);
  QString trashDir(const QString &what) const;
  void sweepLocks();

  struct Lock {
    int clientId = 0;
    QString name;
    QString purpose;
    qint64 since = 0;
    qint64 expiresAt = 0;  // 0 while the holder is connected
  };

  QThreadPool m_pool;  // exactly one thread
  mutable QMutex m_mutex;  // journal, clients, locks (read from other threads)
  std::deque<QJsonObject> m_journal;
  QHash<int, QString> m_clients;
  QHash<QString, Lock> m_locks;  // refKey -> lock
  QSet<QString> m_installing;    // packKeys placed but not added yet (mutation thread)
  QTimer m_sweep;
};
