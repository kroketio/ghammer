#pragma once

#include <QWidget>

class QLabel;
class QStackedWidget;

namespace ui {

class ImageViewer;

// One picture to work from, in an ImageViewer.
class ReferencePanel final : public QWidget {
  Q_OBJECT

public:
  explicit ReferencePanel(QWidget *parent = nullptr);
  // Empty clears it.
  void setImage(const QString &path);
  [[nodiscard]] QString image() const { return m_path; }

private:
  QString m_path;
  QStackedWidget *m_stack = nullptr;
  QLabel *m_empty = nullptr;
  ImageViewer *m_viewer = nullptr;
};

} // namespace ui
