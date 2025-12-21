#pragma once

#include <QHash>
#include <QWidget>

#include <functional>
#include <vector>

class QGridLayout;
class QLabel;
class QLineEdit;

namespace vk {
class Engine3D;
}

namespace ui {

class AxisRow;
class ResetDot;

// Properties of the selected point entity, generated from its class. Several entities of one
// class (setEntities): property rows only, showing the first one's values; edits go to all (one undo step).
class EntityEditor final : public QWidget {
  Q_OBJECT

public:
  explicit EntityEditor(vk::Engine3D *engine, QWidget *parent = nullptr);
  void setEntity(quint32 id) { setEntities({id}); }
  void setEntities(const std::vector<quint32> &ids);
  void refresh();

private:
  void rebuild();
  void clear();
  void setProp(const QString &key, const QVariant &value);
  void resetProp(const QString &key);

  struct Row {
    QLabel *label = nullptr;
    QWidget *editor = nullptr;
    ResetDot *dot = nullptr;
    void setVisible(bool on) const;
  };

  vk::Engine3D *m_engine;
  quint32 m_id = 0;              // the first selected (shown)
  std::vector<quint32> m_ids;    // every one edited
  QString m_builtFor;
  QGridLayout *m_grid = nullptr;
  QLineEdit *m_name = nullptr;
  QLabel *m_refs = nullptr;
  QLabel *m_collision = nullptr;  // func_rigidbody only
  AxisRow *m_origin = nullptr;
  AxisRow *m_angles = nullptr;
  QHash<QString, std::function<void()>> m_refreshers;
  QHash<QString, Row> m_rows;
  // Numbered prop runs (case01..case16): prefix -> keys in order.
  QHash<QString, QStringList> m_sequences;
};

} // namespace ui
