#pragma once

#include <QDialog>
#include <QHash>
#include <QPointer>

#include <functional>

class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QStackedWidget;
class QScrollArea;
class QLabel;

namespace prefs {

struct Def;

// Preferences window: a category tree and one generated page per category; changes apply live.
// Custom pages are registered by their owners.
class PreferencesDialog final : public QDialog {
  Q_OBJECT

public:
  explicit PreferencesDialog(QWidget *parent = nullptr);

  // Shows the singleton, raising it if already open.
  static void showDialog(QWidget *parent, const QString &category = {});

  // Called once at startup.
  static void registerPage(const QString &name, std::function<QWidget *(QWidget *parent)> factory);

  void selectCategory(const QString &category);
  [[nodiscard]] static PreferencesDialog *instance() { return s_instance; }

private:
  QWidget *buildPage(const QString &category);
  QWidget *buildEditor(const Def &def, QWidget *parent);
  void applyFilter(const QString &text);

  QTreeWidget *m_categories = nullptr;
  QHash<QString, QTreeWidgetItem *> m_categoryItems;
  QStackedWidget *m_pages = nullptr;
  QLineEdit *m_search = nullptr;
  QLabel *m_restartNote = nullptr;
  QStringList m_categoryNames;
  QHash<QString, QWidget *> m_rows;  // key -> row, for search filtering

  static QPointer<PreferencesDialog> s_instance;
};

} // namespace prefs
