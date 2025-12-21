#pragma once

#include <QWidget>

class QLineEdit;
class QToolButton;

namespace ui {

// Preferences › Paths: where gHammer keeps its files (settings, cache, autosaves, the local asset server's packs,
// projects). Read-only, except the autosave folder. Registered in main.cpp.
class PathsPage final : public QWidget {
  Q_OBJECT

public:
  static constexpr const char *kCategory = "Paths";

  explicit PathsPage(QWidget *parent = nullptr);

private:
  void refresh();

  QLineEdit *m_settings = nullptr;
  QLineEdit *m_cache = nullptr;
  QLineEdit *m_autosave = nullptr;
  QToolButton *m_autosaveReset = nullptr;
  QLineEdit *m_serverData = nullptr;
  QLineEdit *m_projects = nullptr;
  QLineEdit *m_project = nullptr;
  QLineEdit *m_projectSettings = nullptr;
};

// The folder unsaved levels are backed up in (Paths/Autosave, or <config>/autosave).
QString autosaveFolder();

} // namespace ui
