#pragma once

#include <QWidget>

class QTreeView;
class QFileSystemModel;
class QLineEdit;
class QLabel;

namespace vk {
class Engine3D;
}

namespace ui {

// Model files in the model library folder; double-click or drag places a prop_static.
class ModelBrowser final : public QWidget {
  Q_OBJECT

public:
  explicit ModelBrowser(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void setRoot(const QString &dir);

  vk::Engine3D *m_engine;
  QFileSystemModel *m_fs;
  QTreeView *m_view;
  QLineEdit *m_filter;
  QLabel *m_info;
};

} // namespace ui
