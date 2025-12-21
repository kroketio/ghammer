#pragma once

// Entity I/O widgets.

#include <QWidget>

#include <functional>

#include "client/engine/io_types.h"
#include "client/ui/io_ui_common.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

namespace vk {
class Engine3D;
}

namespace ui {

class NumberField;

// Entity-name box with an eyedropper; an unnamed entity picked that way is named <classname>_N first.
class TargetField final : public QWidget {
  Q_OBJECT

public:
  explicit TargetField(vk::Engine3D *engine, QWidget *parent = nullptr);
  void setText(const QString &text);
  [[nodiscard]] QString text() const;
  // Refills the drop-down and recolours; `self` resolves !self.
  void refresh(quint32 self);

signals:
  void committed(const QString &text);

private:
  void recolor();
  void commit();
  void picked(quint32 id); // the viewport picker answered

  vk::Engine3D *m_engine;
  QComboBox *m_combo;
  QToolButton *m_pick;
  quint32 m_self = 0;
  QString m_last;
};

// Edits a set of connections at once; a change goes to every one as one undo step.
class ConnectionForm final : public QWidget {
  Q_OBJECT

public:
  explicit ConnectionForm(vk::Engine3D *engine, QWidget *parent = nullptr);
  void setConnections(const QVector<iocommon::ConnRef> &refs);
  void refresh();
  void focusTarget();

signals:
  void showEntity(quint32 id);

private:
  void edit(const QString &text, const std::function<void(eng::io::Connection &)> &fn);
  [[nodiscard]] const eng::io::Connection *first() const;

  vk::Engine3D *m_engine;
  QVector<iocommon::ConnRef> m_refs;
  QLabel *m_sentence;
  QComboBox *m_output;
  TargetField *m_target;
  QComboBox *m_input;
  QLineEdit *m_param;
  QLabel *m_paramHint;
  NumberField *m_delay;
  QComboBox *m_timesMode;
  QSpinBox *m_times;
  QCheckBox *m_disabled;
  QLabel *m_issues;
  QToolButton *m_toTarget;
  QToolButton *m_toOwner;
  bool m_loading = false;
};

// The inspector's I/O section.
class IOSummary final : public QWidget {
  Q_OBJECT

public:
  explicit IOSummary(vk::Engine3D *engine, QWidget *parent = nullptr);
  void setEntity(quint32 id);
  void refresh();

signals:
  // row -1 = no output; addOutput starts a new output with that name.
  void openEditor(quint32 entity, int row, const QString &addOutput);

private:
  vk::Engine3D *m_engine;
  quint32 m_id = 0;
  QLabel *m_counts;
  QVBoxLayout *m_rows;
  QToolButton *m_add;
};

} // namespace ui
