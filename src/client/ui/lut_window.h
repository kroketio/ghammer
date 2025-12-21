#pragma once

#include <QDialog>
#include <QImage>
#include <QPointer>

#include <QHash>
#include <QSet>
#include <QVector>

#include <functional>
#include <optional>

#include "client/engine/color_lut.h"
#include "shared/models/texture_entry.h"
#include "client/ui/lut_levels.h"

class QAbstractButton;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class QLineEdit;
class QListWidget;
class QTreeWidget;
class QStackedWidget;
class QWidget;

namespace vk {
class Engine3D;
}

namespace ui {

class LutStepWindow;
class StepEditor;

// A test chart (hue sweep, skin tones, grey ramp): the step windows' histogram input.
[[nodiscard]] const QImage &lutPreviewPicture();

// The asset server's colour LUTs, by pack (base packs locked). A LUT is made from adjustments (the neutral template
// graded; edits upload as you go) or imported (a strip used as is). The level uses one of them (Use in Level).
// Godot layout: N slices of N×N side by side (33³ = 1089×33).
class LutWindow final : public QDialog {
  Q_OBJECT

public:
  explicit LutWindow(vk::Engine3D *engine, QWidget *parent = nullptr);
  void selectLut(const QString &ref);
  [[nodiscard]] QString selectedRef() const { return m_ref; }

  // The selected LUT's adjustments and strip (step windows), by ref; nullptr / null for any other.
  [[nodiscard]] const eng::LutGrade *grade(const QString &ref) const;
  [[nodiscard]] const eng::GradeStep *gradeStep(const QString &ref, int stepId) const;
  [[nodiscard]] QImage strip(const QString &ref) const;
  void editStep(const QString &ref, int stepId, const QString &key, const std::function<void(eng::GradeStep &)> &fn);
  // One window per step.
  LutStepWindow *openStepWindow(const QString &ref, int stepId);

protected:
  void showEvent(QShowEvent *e) override;
  void hideEvent(QHideEvent *e) override;

private:
  void refresh();
  void rebuildList();
  void rebuildPackCombo();
  void select(const QString &ref);
  void showSelected();
  void fetchStrip();
  void rebuildSteps();
  void loadStepEditor();
  void syncStepWindows();
  [[nodiscard]] int selectedStepId() const;
  [[nodiscard]] bool editable() const;
  void setGrade(const eng::LutGrade &g, int selectStepId);
  void gradeChanged();
  void upload();
  void put(const QString &pack, const QString &name, const QImage &strip, const std::optional<eng::LutGrade> &grade,
           const std::function<void(const QString &ref)> &done);
  void addStep(eng::GradeStep::Kind kind);
  void removeStep();
  void moveStep(int delta);
  [[nodiscard]] QString targetPack();
  [[nodiscard]] QString uniqueName(const QString &pack, const QString &wanted) const;

  void newLut();
  void importLut();
  void copyToPack();
  void deleteLut();
  void renameLut();
  void exportLut();
  void saveTemplate();

  vk::Engine3D *m_engine;
  QVector<TextureEntry> m_entries; // the server's LUTs
  QString m_ref;                   // selected
  TextureEntry m_entry;
  std::optional<eng::LutGrade> m_grade; // the selected LUT's adjustments (made here)
  QImage m_strip;                       // the selected LUT's strip (null while downloading)
  QHash<QString, QImage> m_strips;      // downloaded strips by checksum
  bool m_dirty = false;                 // adjustments not uploaded yet
  QString m_pendingSelect;              // select once the list has it
  QTimer *m_upload = nullptr;
  QTimer *m_refresh = nullptr;
  bool m_editing = false;

  QTreeWidget *m_list = nullptr;
  QSet<QString> m_collapsedPacks;  // lower-case pack names
  QLabel *m_listNote = nullptr;
  QComboBox *m_pack = nullptr;
  QStackedWidget *m_stack = nullptr; // 0: nothing selected, 1: a LUT
  QLineEdit *m_name = nullptr;
  QLabel *m_kind = nullptr;
  QStackedWidget *m_kindStack = nullptr; // 0: adjustments, 1: imported
  QWidget *m_gradePane = nullptr;
  QListWidget *m_steps = nullptr;
  QStackedWidget *m_stepEditors = nullptr; // one editor per kind
  std::vector<StepEditor *> m_editors;
  QList<QAbstractButton *> m_needsStep;
  int m_stepId = 0;     // the selected step, kept across rebuilds
  QList<QPointer<LutStepWindow>> m_stepWindows;
  GradeInputCache m_input; // histograms of the selected step's input
  QLabel *m_stripLabel = nullptr;
  QPushButton *m_use = nullptr;
  QPushButton *m_copy = nullptr;
  QPushButton *m_delete = nullptr;
  QPushButton *m_export = nullptr;
};

} // namespace ui
