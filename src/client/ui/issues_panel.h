#pragma once

#include <QWidget>

class QTreeWidget;
class QLabel;
class QTimer;
class QCheckBox;

namespace vk {
class Engine3D;
}

namespace ui {

// Map validation, with quick fixes where there is one.
class IssuesPanel final : public QWidget {
  Q_OBJECT

public:
  explicit IssuesPanel(vk::Engine3D *engine, QWidget *parent = nullptr);
  void runChecks();

signals:
  void countChanged(int count);

private:
  vk::Engine3D *m_engine;
  QTreeWidget *m_list;
  QLabel *m_summary;
  QCheckBox *m_auto;
  QTimer *m_timer;
};

} // namespace ui
