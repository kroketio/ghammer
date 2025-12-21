#pragma once

#include <optional>
#include <QPoint>

#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWidget>

#include "vulkan/vk_engine.h"

#include "lib/QQQuickWidget.h"
#include "lib/globals.h"

class Ctx;
class QMenu;
class QLabel;
class QComboBox;
class QDockWidget;
class QActionGroup;
class QToolBar;
class QTimer;
class QStackedWidget;

namespace proj {
class DocumentManager;
struct Document;
}

namespace ui {
class Outliner;
class Inspector;
class MaterialBrowser;
class ConsolePanel;
class HistoryPanel;
class CommandPalette;
class ToolShelf;
class ViewportNotices;
class PieMenu;
class QuickMaterialModel;
class DocumentTabBar;
class ProjectPanel;
class ReferencePanel;
}

// The editor window. Panels are QDockWidgets; the viewports stay one QML scene so overlays can sit on the 3D view.
class MainWindow final : public QMainWindow {
  Q_OBJECT

public:
  // `project`: the project folder to open (empty: none).
  explicit MainWindow(Ctx *ctx, const QString &project = {}, QWidget *parent = nullptr);
  // First non-option argument or EDITOR_OPEN_MAP, or empty.
  [[nodiscard]] static QString commandLineMap();
  // Self-test, frame / dialog dumps, exports: no prompts, no splash.
  [[nodiscard]] static bool scriptedRun();
  [[nodiscard]] Ctx *editorCtx() const { return ctx_; }
  static MainWindow *getInstance();
  ~MainWindow() override;

  // Open documents (mainwindow_documents.cpp).
  [[nodiscard]] proj::DocumentManager *documents() const { return m_docs; }
  [[nodiscard]] ui::ProjectPanel *projectPanel() const { return m_projectPanel; }
  // Empty when the engine's level was never saved.
  Q_INVOKABLE [[nodiscard]] QString currentMapPath() const;
  // A level or an image in a tab; anything else in its own program.
  void openPath(const QString &path);
  void openExternally(const QString &path);
  // Asks for a name and makes <folder or levels>/<name>.ghmap (an untitled tab without a project).
  Q_INVOKABLE void newLevelDialog(const QString &folder = {});
  // The tool shelf and level docks: off with no level shown and while the physics simulation runs.
  void updateEditingEnabled();
  Q_INVOKABLE void openLevelDialog();
  // Every unsaved level, then the ones written are exported (Export/AutoExport).
  Q_INVOKABLE bool saveAll();
  Q_INVOKABLE bool saveLevelAs();
  // Writes one level (the engine's or a parked one). `askPath`: a level without a .ghmap yet asks where,
  // otherwise it is skipped.
  bool saveLevel(proj::Document *doc, bool askPath);
  // False = cancelled.
  bool closeDocument(int index);
  bool closeAllDocuments(int except = -1);
  // For the self-test and scripts.
  Q_INVOKABLE void newScene();
  void newPlayground();
  bool loadMapFrom(const QString &path);
  bool saveMapTo(const QString &path);
  Q_INVOKABLE bool saveMap() { return saveAll(); }

  // Named menus built from the ActionManager, popped up under the button (x/y in widget coordinates).
  Q_INVOKABLE void showViewportMenu(const QString &name, qreal x, qreal y);
  // Object mode only. x, y in the QML scene's coordinates.
  Q_INVOKABLE void showGridContextMenu(qreal x, qreal y);
  Q_INVOKABLE void setViewportLayout(int mode);
  [[nodiscard]] int viewportLayout() const;
  void showAddMenuAtCursor();
  void addEntityFromMenu(const QString &classname);
  // Asks for the project folder (mainwindow_io.cpp).
  void exportGodot();
  // `interactive`: failures in a message box, else (export on save) a viewport message.
  void exportGodotToProject(const QString &root, bool interactive);
  // Into a Godot 4.8+ project root; `done` gets a summary (or the error) once everything is written.
  void exportGodotTo(const QString &root, std::function<void(const QString &, bool)> done);
  // The same for one open level (DocumentState serial), shown or not.
  void exportLevelTo(quint64 serial, const QString &root, std::function<void(const QString &, bool)> done);
  // Runs `fn` with that level in the engine (its own, or swapped in).
  bool withLevelSerial(quint64 serial, const std::function<void()> &fn);
  Q_INVOKABLE void showToast(const QString &text);
  // From the Entities / Materials panels.
  Q_INVOKABLE bool dropOnViewport(const QString &kind, const QString &name, qreal x, qreal y);

  void maybeReopenLastMap();
  void maybeShowFirstRun();
  [[nodiscard]] QString godotStartDir() const;
  // Save All / Discard / Cancel for every unsaved level. False = cancelled.
  [[nodiscard]] bool confirmDiscardChanges();
  void updateWindowTitle();

  // Projects (mainwindow_project.cpp).
  void openProjectDialog(bool create);
  // Asks about unsaved levels first.
  bool switchToProject(const QString &root);
  // False when the user cancelled.
  bool closeProjectDocuments();
  // The tabs as left (workspace.json).
  void openProjectSession();

protected:
  bool focusNextPrevChild(bool) override { return false; } // else Tab gets eaten
  bool eventFilter(QObject *watched, QEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void keyReleaseEvent(QKeyEvent *event) override;
  void closeEvent(QCloseEvent *event) override;

private slots:
  void onEditorModeChanged(g::EditorMode mode);

private:
  // Construction (mainwindow.cpp).
  void createQmlContent();
  // mainwindow_notices.cpp
  void setupViewportNotices();
  void createDocks();
  void createToolBars();
  void createStatusBar();
  void restoreWindowLayout();
  void applyDefaultDockSizes();
  void restoreSessionView();
  void saveWindowLayout();
  void resetWindowLayout();
  void refreshStatus();
  void startAutosave();
  void runAutosave();
  void setupProjects(const QString &project);
  void rebuildRecentProjectsMenu();

  // mainwindow_actions.cpp
  void registerActions();
  void buildMenuBar();
  QMenu *viewportMenu(const QString &name);
  QMenu *buildMenu(const QString &title, const QStringList &ids, QWidget *parent);

  // mainwindow_keys.cpp
  bool handleEditorKey(QKeyEvent *event);
  [[nodiscard]] bool keyboardBelongsToViewport() const;
  [[nodiscard]] bool isTextCopy(const QKeyEvent *ke) const;
  void copySelectedText();
  [[nodiscard]] bool flyKeysActive() const;

  static MainWindow *pMainWindow;

  QQQuickWidget *quickContent_ = nullptr;
  Ctx *ctx_ = nullptr;
  const QList<Qt::Key> keys_wsad_;

  ui::Outliner *m_outliner = nullptr;
  ui::Inspector *m_inspector = nullptr;
  ui::ViewportNotices *m_notices = nullptr;  // 3D view, lower left
  ui::PieMenu *m_pie = nullptr;              // Shift+S over the viewports
  ui::QuickMaterialModel *m_quickMaterials = nullptr;
  ui::MaterialBrowser *m_materials = nullptr;
  ui::ConsolePanel *m_console = nullptr;
  ui::HistoryPanel *m_history = nullptr;
  QDockWidget *m_dockEntities = nullptr;
  QDockWidget *m_dockModels = nullptr;
  QDockWidget *m_dockIssues = nullptr;
  QDockWidget *m_dockOutliner = nullptr;
  QDockWidget *m_dockHistory = nullptr;
  QDockWidget *m_dockInspector = nullptr;
  QDockWidget *m_dockMaterials = nullptr;
  QDockWidget *m_dockConsole = nullptr;
  QDockWidget *m_dockUv = nullptr;
  QDockWidget *m_dockAutoSelect = nullptr;
  ui::ToolShelf *m_toolShelf = nullptr;
  void rebuildToolShelf();
  void beginShapeTool(int kind);
  QByteArray m_defaultState;
  bool m_sessionRestored = false;
  bool m_layoutRestored = false; // a saved arrangement was applied (else default proportions)
  static constexpr int kLayoutVersion = 12; // bump when the default layout changes

  QComboBox *m_gridCombo = nullptr;
  QLabel *m_statusSelection = nullptr;
  QLabel *m_statusCamera = nullptr;
  QLabel *m_statusFrame = nullptr;
  QLabel *m_statusLog = nullptr;
  QTimer *m_statusTimer = nullptr;
  QTimer *m_autosaveTimer = nullptr;

  QMap<QString, QMenu *> m_viewportMenus;
  // Where Shift+A opened the Add menu (global), to place the entity there.
  std::optional<QPoint> m_addAt;
  QMenu *m_recentMenu = nullptr;

  // Qt folds both shifts into one modifier bit; cut mode binds to the left one, so scan codes are tracked.
  bool m_leftShiftHeld = false;
  QPointer<QWidget> m_textCopyOwner;

  proj::DocumentManager *m_docs = nullptr;
  QStackedWidget *m_central = nullptr;
  QWidget *m_emptyPage = nullptr;
  ui::DocumentTabBar *m_tabs = nullptr;
  ui::ProjectPanel *m_projectPanel = nullptr;
  QDockWidget *m_dockProject = nullptr;
  ui::ReferencePanel *m_reference = nullptr;
  QDockWidget *m_dockReference = nullptr;
  void showReference(const QString &path);
  QTimer *m_workspaceTimer = nullptr;
  bool m_levelShown = false; // the 3D / 2D views are the page shown
  void setupDocuments();
  void createDocumentBar();
  void showActiveDocument();
  // False = cancelled.
  bool askAboutUnsaved(const QList<proj::Document *> &levels, const QString &question);
  void scheduleWorkspaceWrite();
  void writeWorkspace();
  bool closeDocumentsUnder(const QString &path);
  // An empty game/.
  void setupProjectGodot(const QString &dir);
  // One after another, by DocumentState serial.
  void exportLevels(QList<quint64> serials, const QString &root);
  void exportGodotWithEntries(quint64 serial, const QString &root, std::function<void(const QString &, bool)> done);

  // Godot export targets (mainwindow_io.cpp; config): recent projects and export on save (each map its own).
  [[nodiscard]] QStringList recentGodotProjects() const;
  void rememberGodotProject(const QString &root);
  QString pickGodotProject();  // empty = cancelled / not a project
  void populateGodotExportMenu(QMenu *menu);
  void setupGodotSaveTarget(QToolBar *bar);
  void rebuildGodotTargets();
  [[nodiscard]] QString godotSaveTarget() const;
  [[nodiscard]] bool exportOnSave() const;
  void setExportOnSave(bool on);
  QComboBox *m_godotTarget = nullptr;
  QLabel *m_titleLabel = nullptr; // the open project's name
};
