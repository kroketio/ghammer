#pragma once

#include <QDialog>

class QLineEdit;
class QListWidget;

namespace ui {

// Fuzzy search over every registered action (Ctrl+Shift+P / F3).
class CommandPalette final : public QDialog {
  Q_OBJECT

public:
  explicit CommandPalette(QWidget *parent = nullptr);
  static void open(QWidget *parent);

protected:
  bool eventFilter(QObject *o, QEvent *e) override;

private:
  void filter(const QString &text);
  void runCurrent();

  QLineEdit *m_edit;
  QListWidget *m_list;
};

} // namespace ui
