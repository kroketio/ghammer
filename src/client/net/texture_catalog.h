#pragma once

// The editor's cache of the server catalogue: only the entries it touched, never all ~20k.
// Single sink for server events: own and other editors' changes both arrive as events and update the same cache.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <functional>
#include <vector>

#include "asset_client.h"
#include "shared/models/texture_entry.h"

namespace net {

struct PackInfo {
  QString name;
  QString description;
  bool base = true;
  QString type = QStringLiteral("texture");  // "texture" or "lut" (colour LUTs)
  int count = 0;
  qint64 rev = 0;
};

struct LockInfo {
  QString ref;
  int clientId = 0;
  QString name;
  QString purpose;
};

struct QueryPage {
  bool ok = false;
  QString error;
  int total = 0;
  int offset = 0;
  QVector<TextureEntry> items;
  QVector<QPair<QString, int>> facets;
  int distinctTags = 0;
  QVector<QPair<QString, int>> licenseFacets; // license key ("cc0", "rf", …) -> textures
};

class TextureCatalog final : public QObject {
  Q_OBJECT

public:
  static TextureCatalog &instance();

  // Cached entry for `ref` (follows renames), or nullptr. Bare names resolve after a resolve() answer. A texture and a
  // colour LUT may share a ref (packs are unique by name + type): `lut` picks the LUT.
  [[nodiscard]] const TextureEntry *find(const QString &ref, bool lut = false) const;
  [[nodiscard]] TextureEntry entry(const QString &ref, bool lut = false) const;
  [[nodiscard]] bool isKnownMissing(const QString &ref, bool lut = false) const;

  // Asks for uncached refs (batched ~30 ms); `done` runs once all are answered (found or missing).
  void resolve(const QStringList &refs, QObject *context = nullptr, std::function<void()> done = {}, bool lut = false);

  // One page of the catalogue; the entries are cached as a side effect.
  void query(const QJsonObject &args, QObject *context, std::function<void(const QueryPage &)> fn);
  void tagSearch(const QString &text, const QJsonObject &filter, int limit, QObject *context,
                 std::function<void(const QVector<QPair<QString, int>> &)> fn);
  void popularTags(const QJsonObject &filter, QObject *context,
                   std::function<void(const QVector<QPair<QString, int>> &)> fn);

  // Texture packs; LUT packs only through lutPacks().
  [[nodiscard]] QVector<PackInfo> packs() const { return packsOfType(QStringLiteral("texture")); }
  [[nodiscard]] QVector<PackInfo> lutPacks() const { return packsOfType(QStringLiteral("lut")); }
  [[nodiscard]] QVector<PackInfo> packsOfType(const QString &type) const {
    QVector<PackInfo> out;
    for (const PackInfo &p: m_packs)
      if (p.type == type)
        out.append(p);
    return out;
  }
  [[nodiscard]] const PackInfo *pack(const QString &name, const QString &type = QStringLiteral("texture")) const;
  void refreshPacks();

  [[nodiscard]] const LockInfo *lockFor(const QString &ref) const;

  // Mutations: their effect on the cache arrives through the resulting server event.
  void call(const QString &op, const QJsonObject &args, QObject *context, ResultFn fn);

signals:
  void entryChanged(const QString &ref);           // new / updated entry (also props)
  void entryRenamed(const QString &oldRef, const QString &newRef);
  void entryRemoved(const QString &ref);
  void propsChanged(const QString &ref);
  void packsChanged();
  void lockChanged(const QString &ref);
  void resolved(const QStringList &refs);          // a resolve batch came back
  // Something a query result might depend on changed (create / delete / rename / retag).
  void catalogChanged(int pendingChanges);
  void serverChanged();

private:
  TextureCatalog();
  void onServerEvent(const QString &ev, const QJsonObject &msg);
  void onResync();
  void store(const TextureEntry &e);
  void flushResolve();
  void sendResolve(const QStringList &refs, bool lut);
  QString canonicalKey(const QString &ref, bool lut = false) const;

  QHash<QString, TextureEntry> m_entries;  // assetKey(base ref) -> entry
  QHash<QString, QString> m_aliases;       // old / legacy assetKey -> assetKey
  QSet<QString> m_missing;                 // assetKeys the server does not know
  QVector<PackInfo> m_packs;
  QHash<QString, LockInfo> m_locks;        // refKey -> lock

  struct ResolveWaiter {
    QSet<QString> keys;
    QPointer<QObject> context;
    bool hasContext = false;
    std::function<void()> done;
  };
  QSet<QString> m_resolveQueue;  // refs (original spelling; LUTs prefixed "lut:") to ask for
  QSet<QString> m_resolving;     // keys in a request now
  std::vector<ResolveWaiter> m_resolveWaiters;
  QTimer m_resolveTimer;
  // Re-reads packs.list after adds / removes / pack moves (coalesced).
  QTimer m_packRefresh;
  int m_pendingChanges = 0;
};

} // namespace net
