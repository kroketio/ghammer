#pragma once

#include <QDialog>

class QListWidget;
class QTextBrowser;

namespace ui {

// Help › About: the version, and every third-party component with its
// copyright, license and full license text (:/licenses, assets/licenses).
class AboutDialog final : public QDialog {
  Q_OBJECT

public:
  static void showDialog(QWidget *parent);

private:
  explicit AboutDialog(QWidget *parent);
  void showComponent(int row);

  QListWidget *m_list = nullptr;
  QTextBrowser *m_text = nullptr;
};

} // namespace ui
