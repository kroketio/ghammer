#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

namespace ui {

// Preferences page for net::LocalServer; this run's credentials are never saved. Registered in main.cpp.
class LocalServerPage final : public QWidget {
  Q_OBJECT

public:
  static constexpr const char *kCategory = "Asset Server/Local Server";

  explicit LocalServerPage(QWidget *parent = nullptr);

private:
  void refresh();

  QLabel *m_status = nullptr;
  QWidget *m_card = nullptr;
  QLineEdit *m_binary = nullptr;
  QLineEdit *m_url = nullptr;
  QLineEdit *m_user = nullptr;
  QLineEdit *m_password = nullptr;
  QPushButton *m_restart = nullptr;
};

} // namespace ui
