#pragma once

// The I/O Editor window: entity list, Flow graph (ui/io_graph.*), Connections table, inspector, Test dock
// (the simulator). Every edit is an engine undo step; the selection is synced both ways with the editor.

#include <QMainWindow>
#include <QPointer>
#include <QSet>

#include <functional>

#include "client/ui/io_ui_common.h"

class QCheckBox;
class QComboBox;
class QDockWidget;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace vk {
class Engine3D;
}

namespace ui {

class ConnectionForm;
class IOGraphView;
class NumberField;
class TargetField;

class IOEditor final : public QMainWindow {
  Q_OBJECT

public:
  // Opens or raises the window. addOutput starts a new output with that name on `entity`;
  // showTest raises the Test dock with the entity filled in.
  static IOEditor *open(vk::Engine3D *engine, QWidget *owner, quint32 entity = 0, int row = -1,
                        const QString &addOutput = {}, bool showTest = false);
  static void closeIfOpen();

protected:
  void closeEvent(QCloseEvent *e) override;

private:
  IOEditor(vk::Engine3D *engine, QWidget *owner);
  void buildToolbar();
  QWidget *buildEntityList();
  QWidget *buildTable();
  QWidget *buildInspector();
  QWidget *buildEntityPage();
  QWidget *buildTestPanel();

  void scheduleRefresh();
  void refreshAll();
  void refreshEntityList();
  void refreshTable();
  void refreshEntityPage();
  void refreshTestState();
  void refreshStatus();
  [[nodiscard]] QSet<quint32> visibleEntities() const;
  [[nodiscard]] bool matchesSearch(const eng::Entity &e) const;

  void selectEntity(quint32 id, bool fromEditor = false);
  void selectConnections(const QVector<iocommon::ConnRef> &refs, bool fromTable = false);
  void syncToEditor(quint32 id);

  // Each one undo step (or macro).
  QString ensureName(quint32 id);
  void addOutput(quint32 entity, const QString &output, const QString &target = {}, const QString &input = {});
  void deleteConnections(const QVector<iocommon::ConnRef> &refs);
  void duplicateConnections(const QVector<iocommon::ConnRef> &refs);
  void moveConnection(int dir);
  void connectEntities(quint32 from, const QString &output, quint32 to, const QPointF &scenePos, const QPoint &globalPos);
  quint32 createEntity(const QString &classname, quint32 near, const QPointF *graphPos);
  void fillNewEntityMenu(QMenu *menu, const std::function<void(const QString &)> &onPick);
  void showEntityMenu(quint32 id, const QPoint &globalPos);
  void showConnectionMenu(const iocommon::ConnRef &ref, const QPoint &globalPos);
  void prepareTest(quint32 entity);
  void appendLog(double time, const QString &text, quint32 from, quint32 to, bool note);

  vk::Engine3D *m_engine;
  quint32 m_entity = 0;
  QVector<iocommon::ConnRef> m_conns;
  bool m_syncing = false;
  bool m_refreshPending = false;

  // toolbar
  QLineEdit *m_search = nullptr;
  QComboBox *m_scope = nullptr;
  QCheckBox *m_follow = nullptr;
  QAction *m_undo = nullptr, *m_redo = nullptr;
  // centre
  QTabWidget *m_tabs = nullptr;
  IOGraphView *m_graph = nullptr;
  QTreeWidget *m_table = nullptr;
  // left
  QTreeWidget *m_list = nullptr;
  QComboBox *m_listFilter = nullptr;
  // right
  QStackedWidget *m_pages = nullptr;
  ConnectionForm *m_form = nullptr;
  QLabel *m_connTitle = nullptr;
  QLabel *m_entIcon = nullptr;
  QLineEdit *m_entName = nullptr;
  QLabel *m_entClass = nullptr;
  QLabel *m_entDesc = nullptr;
  QListWidget *m_outList = nullptr;
  QListWidget *m_inList = nullptr;
  QToolButton *m_addOut = nullptr;
  QTreeWidget *m_reference = nullptr;
  QLabel *m_outHeader = nullptr;
  QLabel *m_inHeader = nullptr;
  // test
  QDockWidget *m_testDock = nullptr;
  QToolButton *m_run = nullptr, *m_pause = nullptr, *m_step = nullptr;
  QLabel *m_simTime = nullptr;
  QComboBox *m_speed = nullptr;
  TargetField *m_fireTarget = nullptr;
  QComboBox *m_fireInput = nullptr;
  QLineEdit *m_fireParam = nullptr;
  NumberField *m_fireDelay = nullptr;
  QComboBox *m_fireOutput = nullptr;
  QLineEdit *m_raiseName = nullptr;
  QListWidget *m_log = nullptr;
  QTimer *m_simTimer = nullptr;
  // status
  QLabel *m_statusCounts = nullptr;
};

} // namespace ui
