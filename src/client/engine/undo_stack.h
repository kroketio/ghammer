#pragma once

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QString>

namespace eng {

class GameObject;

// What a command changed, for derived-state sync (mirror links): objects added / removed (may be out of the
// scene), live objects moved, live objects with face-attribute changes only.
struct Touched {
  std::vector<std::shared_ptr<GameObject>> objects;
  std::vector<quint32> moved;
  std::vector<quint32> faces;
};

// The caller applies the change, then pushes the command: redo() re-applies, undo() reverses.
class UndoCommand {
public:
  virtual ~UndoCommand() = default;

  virtual void undo() = 0;
  virtual void redo() = 0;

  [[nodiscard]] virtual QString text() const = 0;

  // Adjacent commands with the same non-empty key are one gesture (a slider drag) and merge into one step.
  [[nodiscard]] virtual QString mergeKey() const { return {}; }

  // Takes the newer command's end state, keeping this one's start. False keeps both.
  virtual bool mergeWith(const UndoCommand *) { return false; }

  virtual void touched(Touched &) const {}

  // False for selection-only steps: undoable, but the map does not become unsaved.
  [[nodiscard]] virtual bool changesDocument() const { return true; }
};

// A click that changed the selection (Object mode). Never marks the map unsaved.
class SelectionCommand final : public UndoCommand {
public:
  SelectionCommand(QString text, std::function<void()> undoFn, std::function<void()> redoFn)
      : m_text(std::move(text)), m_undo(std::move(undoFn)), m_redo(std::move(redoFn)) {}
  void undo() override { if (m_undo) m_undo(); }
  void redo() override { if (m_redo) m_redo(); }
  [[nodiscard]] QString text() const override { return m_text; }
  [[nodiscard]] bool changesDocument() const override { return false; }

private:
  QString m_text;
  std::function<void()> m_undo;
  std::function<void()> m_redo;
};

// Before / after captured as closures at the call site.
class LambdaCommand final : public UndoCommand {
public:
  LambdaCommand(QString text, std::function<void()> undoFn, std::function<void()> redoFn, QString key = {})
      : m_text(std::move(text)), m_undo(std::move(undoFn)), m_redo(std::move(redoFn)), m_key(std::move(key)) {}
  void undo() override { if (m_undo) m_undo(); }
  void redo() override { if (m_redo) m_redo(); }
  [[nodiscard]] QString text() const override { return m_text; }
  [[nodiscard]] QString mergeKey() const override { return m_key; }
  bool mergeWith(const UndoCommand *other) override {
    const auto *o = dynamic_cast<const LambdaCommand *>(other);
    if (!o || m_key.isEmpty() || o->m_key != m_key)
      return false;
    m_redo = o->m_redo; // keep our undo (start state), take their end state
    return true;
  }

private:
  QString m_text;
  std::function<void()> m_undo;
  std::function<void()> m_redo;
  QString m_key;
};

// One value plus the setter to push it through (scene scalars and the like).
template <typename T>
class ValueCommand final : public UndoCommand {
public:
  using Apply = std::function<void(const T &)>;

  ValueCommand(QString key, QString label, T before, T after, Apply apply)
      : m_key(std::move(key)), m_label(std::move(label)), m_before(std::move(before)),
        m_after(std::move(after)), m_apply(std::move(apply)) {}

  void undo() override { m_apply(m_before); }
  void redo() override { m_apply(m_after); }

  [[nodiscard]] QString text() const override { return m_label; }
  [[nodiscard]] QString mergeKey() const override { return m_key; }

  bool mergeWith(const UndoCommand *other) override {
    const auto *same = dynamic_cast<const ValueCommand<T> *>(other);
    if (same == nullptr || same->m_key != m_key)
      return false;
    m_after = same->m_after;
    return true;
  }

private:
  QString m_key;
  QString m_label;
  T m_before;
  T m_after;
  Apply m_apply;
};

// Several commands as one step, undone back to front.
class CompositeCommand final : public UndoCommand {
public:
  explicit CompositeCommand(QString label);

  void add(std::unique_ptr<UndoCommand> child);
  [[nodiscard]] bool empty() const { return m_children.empty(); }
  [[nodiscard]] size_t childCount() const { return m_children.size(); }
  // The only child, taken out (a wrapper that turned out to hold one).
  [[nodiscard]] std::unique_ptr<UndoCommand> takeOnly();

  void undo() override;
  void redo() override;
  void touched(Touched &t) const override;
  [[nodiscard]] bool changesDocument() const override {
    for (const auto &c: m_children)
      if (c->changesDocument())
        return true;
    return false;
  }

  [[nodiscard]] QString text() const override { return m_label; }

private:
  QString m_label;
  std::vector<std::unique_ptr<UndoCommand>> m_children;
};

// Bounded history: `push()` only records an already applied change; the oldest step drops when full.
class UndoStack final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool canUndo READ canUndo NOTIFY canUndoChanged)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY canRedoChanged)
  Q_PROPERTY(QString undoText READ undoText NOTIFY undoTextChanged)
  Q_PROPERTY(QString redoText READ redoText NOTIFY redoTextChanged)
  Q_PROPERTY(int maxSteps READ maxSteps WRITE setMaxSteps NOTIFY maxStepsChanged)

public:
  static constexpr int kDefaultMaxSteps = 250;

  explicit UndoStack(QObject *parent = nullptr);

  void push(std::unique_ptr<UndoCommand> command);
  // Nested calls join the outer macro. An empty macro records nothing.
  void beginMacro(const QString &label);
  void endMacro();
  [[nodiscard]] bool inMacro() const { return m_macroDepth > 0; }
  bool undo();
  bool redo();
  void clear();

  [[nodiscard]] bool canUndo() const { return m_top > 0; }
  [[nodiscard]] bool canRedo() const { return m_top < static_cast<int>(m_cmds.size()); }
  [[nodiscard]] int undoDepth() const { return m_top; }
  // A depth outside the history counts as a change.
  [[nodiscard]] bool changesBetween(int a, int b) const {
    const int lo = std::min(a, b), hi = std::max(a, b);
    if (lo < 0 || hi > static_cast<int>(m_cmds.size()))
      return lo != hi;
    for (int i = lo; i < hi; ++i)
      if (m_cmds[size_t(i)]->changesDocument())
        return true;
    return false;
  }
  [[nodiscard]] int stepCount() const { return static_cast<int>(m_cmds.size()); }
  // Moves when a document-changing step is recorded, merged, undone or redone, or the history is cleared.
  [[nodiscard]] quint64 documentRevision() const { return m_docRevision; }

  // markClean() after a load or save; isClean() while the cursor is on a state with the same document (selection
  // steps aside). Survives trims, dropped redo and merges into the saved step; -1 once unreachable (dirty until saved).
  void markClean();
  [[nodiscard]] bool isClean() const { return !changesBetween(m_clean, m_top); }
  [[nodiscard]] int cleanDepth() const { return m_clean; }
  [[nodiscard]] QString undoText() const;
  [[nodiscard]] QString redoText() const;
  // Label of step i (0 = oldest).
  [[nodiscard]] QString textAt(int i) const {
    return i >= 0 && i < static_cast<int>(m_cmds.size()) ? m_cmds[size_t(i)]->text() : QString();
  }

  // True while replaying; recording code checks it so a replay cannot record itself.
  [[nodiscard]] bool isApplying() const { return m_applying; }

  // Runs once per step about to be recorded (top-level push or outermost endMacro); what it pushes joins that
  // step. Not run while replaying, nor for its own pushes.
  void setCommitHook(std::function<void(UndoCommand &)> hook) { m_hook = std::move(hook); }

  void setMaxSteps(int maxSteps);
  [[nodiscard]] int maxSteps() const { return m_maxSteps; }

signals:
  void canUndoChanged();
  void canRedoChanged();
  void undoTextChanged();
  void redoTextChanged();
  void maxStepsChanged();

private:
  std::deque<std::unique_ptr<UndoCommand>> m_cmds;
  int m_top = 0;
  int m_clean = 0;
  quint64 m_docRevision = 0;
  int m_maxSteps = kDefaultMaxSteps;
  bool m_applying = false;
  int m_macroDepth = 0;
  std::unique_ptr<CompositeCommand> m_macro;
  std::function<void(UndoCommand &)> m_hook;
  bool m_inHook = false;

  void record(std::unique_ptr<UndoCommand> command);
  void dropOldest();
  // Runs the hook on the open macro (depth 1), then closes and records it.
  void finishMacroWithHook();
};

} // namespace eng
