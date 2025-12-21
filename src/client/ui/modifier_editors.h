#pragma once

#include <QJsonObject>
#include <QWidget>

namespace vk {
class Engine3D;
}

namespace ui {

// A modifier card's parameters. load() leaves fields being edited alone; changes emit the whole set with
// a merge key, so a drag is one undo step.
class ModifierEditor : public QWidget {
  Q_OBJECT

public:
  using QWidget::QWidget;
  virtual void load(const QJsonObject &params) = 0;

signals:
  void changed(const QJsonObject &params, const QString &mergeKey);
};

// nullptr for a kind without an editor.
ModifierEditor *makeModifierEditor(const QString &kind, vk::Engine3D *engine, QWidget *parent);

} // namespace ui
