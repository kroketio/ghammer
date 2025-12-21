#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>

#include "client/vulkan/vk_engine.h"

class QWidget;

namespace proj {

// One open tab: a level (.ghmap / .vmf) or an image.
struct Document {
  enum class Kind { Level, Image };

  Kind kind = Kind::Level;
  QString path;     // absolute; empty for a level never saved
  int untitled = 0; // a level never saved: its number ("Untitled", "Untitled 2", ...)
  quint64 id = 0;   // unique for the session (tabs, signals)

  // Level: its state while another level is in the engine (null while it is the engine's, or never loaded).
  std::unique_ptr<vk::Engine3D::DocumentState> parked;
  bool loaded = false;
  quint64 serial = 0; // DocumentState::serial

  QPointer<QWidget> view; // image: the viewer page (made by the window on first show)

  [[nodiscard]] bool isLevel() const { return kind == Kind::Level; }
  // "e1m1.ghmap", "Untitled", "Untitled 2"
  [[nodiscard]] QString fileName() const;
};

// The open documents in tab order. The engine holds one level at a time (engineLevel(): shown, or shown last);
// the others are parked in their Document. Restored levels load when first shown.
class DocumentManager final : public QObject {
  Q_OBJECT

public:
  explicit DocumentManager(vk::Engine3D *engine, QObject *parent = nullptr);
  ~DocumentManager() override;

  [[nodiscard]] static bool isLevelFile(const QString &path);
  [[nodiscard]] static bool isImageFile(const QString &path);

  [[nodiscard]] int count() const { return int(m_docs.size()); }
  [[nodiscard]] Document *at(int i) const;
  [[nodiscard]] int indexOf(const Document *d) const;
  [[nodiscard]] Document *find(const QString &path) const;
  [[nodiscard]] Document *byId(quint64 id) const;
  [[nodiscard]] Document *bySerial(quint64 serial) const;
  [[nodiscard]] int activeIndex() const { return m_active; }
  [[nodiscard]] Document *active() const { return at(m_active); }
  // Shown, or shown last; null when no level is open.
  [[nodiscard]] Document *engineLevel() const { return m_engineDoc; }
  [[nodiscard]] QList<Document *> levels() const;

  // Opens or shows `path`'s tab; `activate` false adds it unshown. Null when the file is neither a level nor an
  // image, or a level fails to load (`error`).
  Document *open(const QString &path, bool activate = true, QString *error = nullptr);
  // `path` may name where Save writes it.
  Document *newLevel(const QString &path = {});
  // False when the engine cannot switch now (a gesture or undo macro open) or the level cannot be loaded.
  bool activate(int index, QString *error = nullptr);
  // Without asking about unsaved changes.
  void close(int index);
  // Every tab but `keep` (null: all), without loading any level on the way.
  void closeAll(Document *keep = nullptr);
  void move(int from, int to);
  // Tabs under `from` follow a rename / move.
  void renamed(const QString &from, const QString &to);

  [[nodiscard]] bool dirty(const Document *d) const;
  [[nodiscard]] QList<Document *> dirtyLevels() const;
  // Runs `fn` with the level in the engine, swapped in for the call if parked (no signals, no GPU work).
  // False when the level is gone or not loaded.
  bool withLevel(const Document *d, const std::function<void()> &fn);
  bool withLevel(quint64 serial, const std::function<void()> &fn);
  void markClean(Document *d);
  void setPath(Document *d, const QString &path);

signals:
  void documentsChanged();               // added, removed, moved, renamed
  void activeChanged();                  // another tab is shown
  void dirtyChanged();                   // some level's unsaved state changed
  void levelLoaded(proj::Document *doc); // a level was read from disk into the engine

private:
  bool loadInto(Document *d, QString *error);
  // Parks the current level.
  void bringIntoEngine(Document *d);
  void installLevelStore();

  vk::Engine3D *m_engine = nullptr;
  std::vector<std::unique_ptr<Document>> m_docs;
  int m_active = -1;
  Document *m_engineDoc = nullptr;
  quint64 m_nextId = 1;
};

} // namespace proj
