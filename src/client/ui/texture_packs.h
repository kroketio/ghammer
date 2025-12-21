#pragma once

#include <QMainWindow>
#include <QPointer>

class QLabel;
class QLineEdit;
class QListView;
class QListWidget;
class QComboBox;
class QPushButton;
class QToolButton;
class QVBoxLayout;
class QGridLayout;
class QSplitter;

namespace vk {
class Engine3D;
}

namespace ui {

class ServerTextureModel;
class TextureCardDelegate;
class FlowLayout;

// Texture pack management (top-level window). Base packs are read-only; deleted user packs go to the
// server's trash, never deleted. The map's project pack (where copies go) is chosen here.
class TexturePacksWindow final : public QMainWindow {
  Q_OBJECT

public:
  static TexturePacksWindow *open(vk::Engine3D *engine, QWidget *owner, const QString &selectRef = {});
  static void closeIfOpen();

protected:
  void closeEvent(QCloseEvent *e) override;

private:
  TexturePacksWindow(vk::Engine3D *engine, QWidget *owner);
  QWidget *buildPacks();
  QWidget *buildTextures();
  QWidget *buildEditor();
  void rebuildPacks();
  void rebuildProjectCombo();
  void rebuildSessions();
  void selectPack(const QString &pack);
  void showTexture(const QString &ref);
  void refreshEditor();
  void newPack();
  void renamePack();
  void deletePack();
  void renameTexture();
  void deleteTexture();
  void saveTags(const QStringList &tags);
  void report(const QString &text, bool error = false);
  void saveWindowState();

  QPointer<vk::Engine3D> m_engine;
  QString m_pack;
  QString m_ref;
  QString m_pendingSelect;

  QListWidget *m_packs = nullptr;
  QPushButton *m_renamePack = nullptr;
  QPushButton *m_deletePack = nullptr;
  QComboBox *m_project = nullptr;
  QLabel *m_packInfo = nullptr;

  QLineEdit *m_search = nullptr;
  QListView *m_grid = nullptr;
  ServerTextureModel *m_model = nullptr;
  TextureCardDelegate *m_delegate = nullptr;
  QLabel *m_gridInfo = nullptr;

  QWidget *m_editor = nullptr;
  QLabel *m_editorEmpty = nullptr;
  QLabel *m_title = nullptr;
  QLabel *m_subtitle = nullptr;
  QLabel *m_lockNote = nullptr;
  QLineEdit *m_nameEdit = nullptr;
  QPushButton *m_renameButton = nullptr;
  QWidget *m_tagBox = nullptr;
  FlowLayout *m_tagFlow = nullptr;
  QLineEdit *m_tagAdd = nullptr;
  QGridLayout *m_mapGrid = nullptr;
  QWidget *m_mapHolder = nullptr;
  QPushButton *m_copyButton = nullptr;
  QPushButton *m_deleteButton = nullptr;
  QLabel *m_derived = nullptr;

  QListWidget *m_sessions = nullptr;
  QLabel *m_status = nullptr;
  QSplitter *m_split = nullptr;
};

} // namespace ui
