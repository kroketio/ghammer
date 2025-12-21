#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;

namespace vk {
class Engine3D;
}

namespace ui {

// Inspector section for mirror mode and mirror links.
class MirrorPanel final : public QWidget {
  Q_OBJECT

public:
  explicit MirrorPanel(vk::Engine3D *engine, QWidget *parent = nullptr);

private:
  void refresh();

  vk::Engine3D *m_engine;
  QTimer *m_timer;
  QWidget *m_modeRow;
  QPushButton *m_mirrorSel, *m_reset, *m_off;
  QCheckBox *m_textures;
  QWidget *m_linkRow;
  QLabel *m_linked;
  QPushButton *m_addTo;
  QCheckBox *m_hideCopies;
};

} // namespace ui
