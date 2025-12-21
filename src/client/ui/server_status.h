#pragma once

#include <QToolButton>

namespace ui {

// Status bar indicator and menu for the asset server connection.
class ServerStatus final : public QToolButton {
  Q_OBJECT

public:
  explicit ServerStatus(QWidget *parent = nullptr);

private:
  void refresh();
  static QIcon dot(const QColor &c);
};

} // namespace ui
