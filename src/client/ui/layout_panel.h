#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

namespace ui {

class LayoutModel;
class NumberField;
class Section;

// The Layout Designer's settings panel.
class LayoutPanel final : public QWidget {
  Q_OBJECT

public:
  explicit LayoutPanel(LayoutModel *model, QWidget *parent = nullptr);

signals:
  void fitRequested();

private:
  void build();
  void refresh();
  void scheduleRefresh();
  NumberField *number(int lo, int hi, int step = 16);
  void setCfgInt(const char *key, int v);

  LayoutModel *m_model;
  QWidget *m_body = nullptr;
  QTimer *m_refreshTimer = nullptr;

  QComboBox *m_layouts = nullptr;
  QLineEdit *m_name = nullptr;

  Section *m_sel = nullptr;
  int m_rowSelWidth = -1, m_rowSelLength = -1, m_rowSelHeight = -1, m_rowSelElev = -1, m_rowRoomW = -1,
      m_rowRoomD = -1, m_rowContact = -1;
  QComboBox *m_selWidth = nullptr;
  NumberField *m_selLength = nullptr, *m_selHeight = nullptr, *m_selElev = nullptr, *m_roomW = nullptr,
              *m_roomD = nullptr;
  QComboBox *m_selContact = nullptr;

  QComboBox *m_corrWidth = nullptr;
  NumberField *m_corrHeight = nullptr;

  Section *m_rooms = nullptr;
  int m_rowDoorW = -1, m_rowDoorH = -1;
  NumberField *m_roomHeight = nullptr, *m_doorW = nullptr, *m_doorH = nullptr;
  QComboBox *m_roomContact = nullptr;

  QComboBox *m_ventWidth = nullptr;
  NumberField *m_ventHeight = nullptr, *m_ventElev = nullptr, *m_ventWall = nullptr;

  NumberField *m_wall = nullptr, *m_floor = nullptr, *m_ceiling = nullptr;

  QComboBox *m_grid = nullptr;
  QCheckBox *m_dims = nullptr;

  QLineEdit *m_mat[4] = {};
};

} // namespace ui
