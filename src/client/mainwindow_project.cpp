// Projects: the one open project, the project chooser, recent projects.
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>

#include "client/prefs/preferences.h"
#include "client/project/project.h"
#include "client/project/project_manager.h"
#include "client/ui/action_manager.h"
#include "client/ui/project_dialog.h"
#include "mainwindow.h"

QString MainWindow::commandLineMap() {
  QString map = qEnvironmentVariable("EDITOR_OPEN_MAP");
  const QStringList args = QCoreApplication::arguments();
  for (int i = 1; i < args.size() && map.isEmpty(); ++i)
    if (!args[i].startsWith(QLatin1Char('-')))
      map = args[i];
  return map;
}

bool MainWindow::scriptedRun() {
  static const char *kHooks[] = {
      "EDITOR_SELFTEST",  "EDITOR_FRAMEDUMP",    "EDITOR_TEXDUMP",  "EDITOR_PREFSDUMP", "EDITOR_LUTDUMP",
      "EDITOR_LUTDEBUG",  "EDITOR_GODOT_EXPORT", "EDITOR_PHYSICSDUMP", "EDITOR_MOVETEST", "EDITOR_DUMPLIGHTS",
      "EDITOR_AODEBUG",   "EDITOR_CREATE_PROJECT", "EDITOR_PACKDUMP",
  };
  for (const char *k: kHooks)
    if (!qEnvironmentVariableIsEmpty(k))
      return true;
  return false;
}

void MainWindow::setupProjects(const QString &project) {
  auto &pm = proj::ProjectManager::instance();
  connect(&pm, &proj::ProjectManager::projectChanged, this, [this] {
    updateWindowTitle();
    rebuildGodotTargets();
    rebuildRecentProjectsMenu();
    ui::ActionManager::instance().refresh();
  });
  for (const char *key: {"Export/AutoExport", "Export/Target"})
    prefs::get().watch(QString::fromLatin1(key), this, [this](const QVariant &) {
      rebuildGodotTargets();
      ui::ActionManager::instance().refresh();
    });
  connect(qApp, &QCoreApplication::aboutToQuit, this, [] {
    if (proj::Project *p = proj::currentProject())
      p->settings().sync();
  });
  if (project.isEmpty())
    return;
  QString error;
  if (!pm.open(project, &error)) {
    qWarning().noquote() << QStringLiteral("[project] %1").arg(error);
    QTimer::singleShot(0, this, [this, error] { showToast(error); });
  }
}

void MainWindow::openProjectDialog(const bool create) {
  const QString root = ui::ProjectDialog::run(this, create ? ui::ProjectDialog::Mode::Create
                                                          : ui::ProjectDialog::Mode::Open);
  if (!root.isEmpty())
    switchToProject(root);
}

bool MainWindow::switchToProject(const QString &root) {
  auto &pm = proj::ProjectManager::instance();
  if (pm.current() && QDir::cleanPath(pm.current()->root()) == QDir::cleanPath(root))
    return true;
  if (!closeProjectDocuments())
    return false;
  QString error;
  if (!pm.open(root, &error)) {
    QMessageBox::warning(this, tr("Open Project"), error);
    return false;
  }
  openProjectSession();
  return true;
}

void MainWindow::rebuildRecentProjectsMenu() {
  if (!m_recentMenu)
    return;
  m_recentMenu->clear();
  m_recentMenu->setToolTipsVisible(true);
  const auto recent = proj::ProjectManager::instance().recent();
  int shown = 0;
  for (const auto &r: recent) {
    if (!r.exists)
      continue;
    QAction *a = m_recentMenu->addAction(r.name, this, [this, root = r.root] { switchToProject(root); });
    a->setToolTip(QDir::toNativeSeparators(r.root));
    ++shown;
  }
  if (shown == 0)
    m_recentMenu->addAction(tr("(no recent projects)"))->setEnabled(false);
}
