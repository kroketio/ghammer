#pragma once

#include <QDialog>
#include <QPointer>

class QComboBox;
class QLineEdit;
class QCheckBox;
class QRadioButton;
class QLabel;
class QPushButton;
class QWidget;

namespace vk {
class Engine3D;
}

namespace ui {

// "Copy into pack": an editable copy of a texture (every map) in a user pack, optionally put on the original's
// faces and opened for editing. Base textures are never modified.
class CopyTextureDialog final : public QDialog {
  Q_OBJECT

public:
  static void run(vk::Engine3D *engine, const QString &ref, QWidget *parent);

private:
  CopyTextureDialog(vk::Engine3D *engine, const QString &ref, QWidget *parent);
  void rebuildPacks(const QString &select = {});
  void createPack();
  void doCopy();

  QPointer<vk::Engine3D> m_engine;
  QString m_ref;
  QLabel *m_thumb = nullptr;
  QComboBox *m_pack = nullptr;
  QWidget *m_newPackRow = nullptr;
  QLineEdit *m_newPackName = nullptr;
  QLineEdit *m_name = nullptr;
  QCheckBox *m_copyProps = nullptr;
  QRadioButton *m_replaceSelection = nullptr;
  QRadioButton *m_replaceAll = nullptr;
  QRadioButton *m_replaceNone = nullptr;
  QCheckBox *m_edit = nullptr;
  QCheckBox *m_makeProject = nullptr;
  QLabel *m_status = nullptr;
  QPushButton *m_copy = nullptr;
};

} // namespace ui
