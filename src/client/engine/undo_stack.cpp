#include "undo_stack.h"

#include <algorithm>

#include <QLoggingCategory>

namespace eng {

static QLoggingCategory undoLog("editor.undo", QtInfoMsg);

CompositeCommand::CompositeCommand(QString label) : m_label(std::move(label)) {}

void CompositeCommand::add(std::unique_ptr<UndoCommand> child) {
  if (child)
    m_children.push_back(std::move(child));
}

void CompositeCommand::undo() {
  for (auto it = m_children.rbegin(); it != m_children.rend(); ++it)
    (*it)->undo();
}

void CompositeCommand::redo() {
  for (const auto &child : m_children)
    child->redo();
}

std::unique_ptr<UndoCommand> CompositeCommand::takeOnly() {
  if (m_children.size() != 1)
    return nullptr;
  std::unique_ptr<UndoCommand> c = std::move(m_children.front());
  m_children.clear();
  return c;
}

void CompositeCommand::touched(Touched &t) const {
  for (const auto &child : m_children)
    child->touched(t);
}

namespace {

// A throwing command must not leave the stack replaying, or every later edit stops recording.
struct ApplyingGuard {
  explicit ApplyingGuard(bool &flag) : m_flag(flag) { m_flag = true; }
  ~ApplyingGuard() { m_flag = false; }
  bool &m_flag;
};

} // namespace

UndoStack::UndoStack(QObject *parent) : QObject(parent) {}

QString UndoStack::undoText() const {
  return m_top > 0 ? m_cmds[size_t(m_top - 1)]->text() : QString();
}

QString UndoStack::redoText() const {
  return canRedo() ? m_cmds[size_t(m_top)]->text() : QString();
}

void UndoStack::setMaxSteps(const int maxSteps) {
  const int next = std::max(1, maxSteps);
  if (next == m_maxSteps)
    return;
  m_maxSteps = next;
  while (static_cast<int>(m_cmds.size()) > m_maxSteps)
    dropOldest();
  emit maxStepsChanged();
  emit canUndoChanged();
  emit undoTextChanged();
}

void UndoStack::push(std::unique_ptr<UndoCommand> command) {
  if (!command)
    return;
  if (m_applying) {
    qCDebug(undoLog).noquote()
        << QStringLiteral("[undo] ignored '%1' recorded while replaying").arg(command->text());
    return;
  }

  if (m_macro) {
    m_macro->add(std::move(command));
    return;
  }
  if (m_hook && !m_inHook) {
    // Wrapped, so what the hook pushes joins this step.
    m_macro = std::make_unique<CompositeCommand>(command->text());
    m_macroDepth = 1;
    m_macro->add(std::move(command));
    finishMacroWithHook();
    return;
  }
  record(std::move(command));
}

void UndoStack::finishMacroWithHook() {
  {
    m_inHook = true;
    struct Reset {
      bool &f;
      ~Reset() { f = false; }
    } reset {m_inHook};
    m_hook(*m_macro);
  }
  m_macroDepth = 0;
  std::unique_ptr<CompositeCommand> macro = std::move(m_macro);
  if (!macro || macro->empty())
    return;
  if (macro->childCount() == 1)
    record(macro->takeOnly()); // nothing added: record the command itself (merges normally)
  else
    record(std::move(macro));
}

void UndoStack::record(std::unique_ptr<UndoCommand> command) {
  if (!command || m_applying)
    return;
  const bool hadRedo = canRedo();
  if (hadRedo) {
    // The saved state was ahead of the cursor: still reachable only when the dropped steps did not change the
    // document (selection steps).
    if (m_clean > m_top)
      m_clean = changesBetween(m_top, m_clean) ? -1 : m_top;
    m_cmds.resize(static_cast<size_t>(m_top));
  }

  const QString key = command->mergeKey();
  if (!key.isEmpty() && m_top > 0) {
    UndoCommand &top = *m_cmds[size_t(m_top - 1)];
    if (top.mergeKey() == key && top.mergeWith(command.get())) {
      if (top.changesDocument())
        ++m_docRevision;
      // The saved step itself changed: nothing reachable matches the file now.
      if (m_clean == m_top && top.changesDocument())
        m_clean = -1;
      qCDebug(undoLog).noquote()
          << QStringLiteral("[undo] merged '%1' into '%2' (depth %3)").arg(command->text(), top.text())
                 .arg(m_top);
      if (hadRedo) {
        emit canRedoChanged();
        emit redoTextChanged();
      }
      emit undoTextChanged();
      return;
    }
  }

  const int dropped = std::max(0, static_cast<int>(m_cmds.size()) + 1 - m_maxSteps);
  if (command->changesDocument())
    ++m_docRevision;
  m_cmds.push_back(std::move(command));
  ++m_top;
  while (static_cast<int>(m_cmds.size()) > m_maxSteps)
    dropOldest();
  if (dropped > 0)
    qCDebug(undoLog).noquote()
        << QStringLiteral("[undo] dropped %1 oldest step(s) at the %2 limit").arg(dropped).arg(m_maxSteps);

  qCInfo(undoLog).noquote() << QStringLiteral("[undo] recorded '%1' (depth %2 of %3)")
                                  .arg(m_cmds[size_t(m_top - 1)]->text())
                                  .arg(m_top)
                                  .arg(m_maxSteps);

  emit canUndoChanged();
  emit canRedoChanged();
  emit undoTextChanged();
  emit redoTextChanged();
}

void UndoStack::beginMacro(const QString &label) {
  if (m_macroDepth++ == 0)
    m_macro = std::make_unique<CompositeCommand>(label);
}

void UndoStack::endMacro() {
  if (m_macroDepth <= 0)
    return;
  if (m_macroDepth > 1) {
    --m_macroDepth;
    return;
  }
  if (m_hook && !m_inHook && m_macro && !m_macro->empty() && !m_applying) {
    finishMacroWithHook(); // depth stays 1 while the hook runs
    return;
  }
  m_macroDepth = 0;
  std::unique_ptr<CompositeCommand> macro = std::move(m_macro);
  if (!macro || macro->empty())
    return;
  record(std::move(macro));
}

bool UndoStack::undo() {
  if (m_top <= 0) {
    qCInfo(undoLog) << "[undo] nothing to undo";
    return false;
  }

  UndoCommand &cmd = *m_cmds[size_t(m_top - 1)];
  const QString text = cmd.text();
  {
    const ApplyingGuard guard(m_applying);
    cmd.undo();
  }
  --m_top;
  if (cmd.changesDocument())
    ++m_docRevision;

  qCInfo(undoLog).noquote() << QStringLiteral("[undo] undone '%1' (%2 left, %3 redoable)")
                                  .arg(text)
                                  .arg(m_top)
                                  .arg(static_cast<int>(m_cmds.size()) - m_top);

  emit canUndoChanged();
  emit canRedoChanged();
  emit undoTextChanged();
  emit redoTextChanged();
  return true;
}

bool UndoStack::redo() {
  if (!canRedo()) {
    qCInfo(undoLog) << "[undo] nothing to redo";
    return false;
  }

  UndoCommand &cmd = *m_cmds[size_t(m_top)];
  const QString text = cmd.text();
  {
    const ApplyingGuard guard(m_applying);
    cmd.redo();
  }
  ++m_top;
  if (cmd.changesDocument())
    ++m_docRevision;

  qCInfo(undoLog).noquote() << QStringLiteral("[undo] redid '%1' (%2 left, %3 redoable)")
                                  .arg(text)
                                  .arg(m_top)
                                  .arg(static_cast<int>(m_cmds.size()) - m_top);

  emit canUndoChanged();
  emit canRedoChanged();
  emit undoTextChanged();
  emit redoTextChanged();
  return true;
}

void UndoStack::dropOldest() {
  m_cmds.pop_front();
  m_top = std::max(0, m_top - 1);
  // Saved state at or before the dropped step: unreachable when at depth 0, else one step closer.
  if (m_clean >= 0)
    --m_clean;
}

void UndoStack::markClean() {
  m_clean = m_top;
}

void UndoStack::clear() {
  const bool hadAnything = !m_cmds.empty();
  ++m_docRevision;
  m_cmds.clear();
  m_top = 0;
  m_clean = 0;
  if (hadAnything) {
    qCInfo(undoLog) << "[undo] history cleared";
    emit canUndoChanged();
    emit canRedoChanged();
    emit undoTextChanged();
    emit redoTextChanged();
  }
}

} // namespace eng
