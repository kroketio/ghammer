#pragma once

#include <QWidget>

#include <functional>
#include <vector>

#include "client/engine/color_grade.h"

class QFrame;
class QVBoxLayout;

namespace ui {

[[nodiscard]] QString gradeKindName(eng::GradeStep::Kind kind);

// One adjustment step's controls. Changes go through `edit` (key: a slider name, so a drag is one undo step).
class StepEditor : public QWidget {
public:
  using EditFn = std::function<void(const QString &key, const std::function<void(eng::GradeStep &)> &fn)>;

  StepEditor(EditFn edit, QWidget *parent);
  void load(const eng::GradeStep &s);

protected:
  using Get = std::function<double(const eng::GradeStep &)>;
  using Set = std::function<void(eng::GradeStep &, double)>;

  QVBoxLayout *card(const QString &heading);
  QFrame *slider(QVBoxLayout *card, const QString &label, double lo, double hi, int decimals, const QString &suffix,
                 Get get, Set set, double neutral, const QString &key);
  void colour(QVBoxLayout *card, const QString &label, std::function<QColor(const eng::GradeStep &)> get,
              std::function<void(eng::GradeStep &, const QColor &)> set, const QColor &neutral, const QString &key);
  QFrame *row(QVBoxLayout *card, const QString &label, QWidget *editor, QWidget *trailing = nullptr);
  void onLoad(std::function<void(const eng::GradeStep &)> f) { m_binders.push_back(std::move(f)); }
  void addResetAll(std::function<void(eng::GradeStep &)> reset);

  QVBoxLayout *m_v;
  EditFn m_edit;

private:
  std::vector<std::function<void(const eng::GradeStep &)>> m_binders;
};

// Histogram of the preview picture through the steps before the edited one.
using InputHistogramFn = std::function<eng::grade::Histogram(eng::GradeChannel)>;

// Curves' Edit… button calls `openWindow`; Levels is edited in place over `histogram`.
[[nodiscard]] StepEditor *makeStepEditor(eng::GradeStep::Kind kind, StepEditor::EditFn edit,
                                         std::function<void()> openWindow, InputHistogramFn histogram,
                                         QWidget *parent);

} // namespace ui
