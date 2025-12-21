// Open documents: tabs, central pages, open / save / close, and the project session (.ghammer/workspace.json).
#include <QDesktopServices>
#include <QDockWidget>
#include <QQuickItem>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QSaveFile>
#include <QStackedWidget>
#include <QTimer>
#include <QToolBar>
#include <QUrl>

#include "client/ctx.h"
#include "client/engine/godot_export.h"
#include "client/engine/map_format.h"
#include "client/engine/map_loader.h"
#include "client/prefs/preferences.h"
#include "client/project/document_manager.h"
#include "client/project/godot_project.h"
#include "client/project/project.h"
#include "client/project/project_manager.h"
#include "client/ui/action_manager.h"
#include "client/ui/document_tabs.h"
#include "client/ui/external_apps.h"
#include "client/ui/file_dialog.h"
#include "client/ui/image_viewer.h"
#include "client/ui/project_panel.h"
#include "client/ui/reference_panel.h"
#include "client/ui/scale.h"
#include "client/ui/tool_shelf.h"
#include "client/vulkan/vk_engine.h"
#include "mainwindow.h"

namespace {

bool worksWithoutLevel(const QString &id, const QString &category) {
  static const QSet<QString> categories {QStringLiteral("File"), QStringLiteral("Panels"), QStringLiteral("Help"),
                                         QStringLiteral("Documents"), QStringLiteral("Server")};
  if (categories.contains(category))
    return true;
  return id == QLatin1String("view.resetLayout") || id == QLatin1String("view.palette")
      || id == QLatin1String("view.uiLarger") || id == QLatin1String("view.uiSmaller");
}

// Viewing, selecting, files and the simulation itself; nothing that edits the level.
bool worksWhileSimulating(const QString &id, const QString &category) {
  static const QSet<QString> categories {QStringLiteral("File"),    QStringLiteral("Panels"),  QStringLiteral("Help"),
                                         QStringLiteral("Documents"), QStringLiteral("View"), QStringLiteral("Physics"),
                                         QStringLiteral("Mode"),    QStringLiteral("Select"),  QStringLiteral("Grid"),
                                         QStringLiteral("Server")};
  static const QSet<QString> edits {QStringLiteral("view.hide"), QStringLiteral("view.isolate"),
                                    QStringLiteral("view.showAll")};
  return categories.contains(category) && !edits.contains(id);
}

// The path, or empty when cancelled.
QString askLevelPath(QWidget *parent, const QString &dir, const QString &title) {
  QString name;
  for (;;) {
    bool ok = false;
    name = QInputDialog::getText(parent, title, QObject::tr("Name"), QLineEdit::Normal, name, &ok).trimmed();
    if (!ok || name.isEmpty())
      return {};
    if (name.endsWith(QLatin1String(".ghmap"), Qt::CaseInsensitive))
      name.chop(6);
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
      QMessageBox::warning(parent, title, QObject::tr("A level name cannot contain / or \\"));
      continue;
    }
    if (QFileInfo::exists(QDir(dir).filePath(name + QStringLiteral(".ghmap")))) {
      QMessageBox::warning(parent, title, QObject::tr("A level named ‘%1’ already exists").arg(name));
      continue;
    }
    return QDir(dir).filePath(name + QStringLiteral(".ghmap"));
  }
}

} // namespace

QString MainWindow::currentMapPath() const {
  const proj::Document *d = m_docs ? m_docs->engineLevel() : nullptr;
  return d ? d->path : QString();
}

void MainWindow::setupDocuments() {
  connect(m_docs, &proj::DocumentManager::activeChanged, this, &MainWindow::showActiveDocument);
  connect(m_docs, &proj::DocumentManager::documentsChanged, this, [this] {
    updateWindowTitle();
    scheduleWorkspaceWrite();
    for (int i = m_central->count() - 1; i >= 0; --i) {
      QWidget *w = m_central->widget(i);
      if (w == quickContent_ || w == m_emptyPage)
        continue;
      bool used = false;
      for (int d = 0; d < m_docs->count() && !used; ++d)
        used = m_docs->at(d)->view == w;
      if (!used) {
        m_central->removeWidget(w);
        w->deleteLater();
      } else if (auto *viewer = qobject_cast<ui::ImageViewer *>(w)) {
        for (int d = 0; d < m_docs->count(); ++d)
          if (m_docs->at(d)->view == w && m_docs->at(d)->path != viewer->path())
            viewer->setPath(m_docs->at(d)->path); // renamed
      }
    }
  });
  connect(m_docs, &proj::DocumentManager::dirtyChanged, this, &MainWindow::updateWindowTitle);
  // Another level in the engine: the window follows its mode.
  connect(g::engine3D, &vk::Engine3D::documentChanged, this,
          [this] { ctx_->syncMode(g::EditorMode(g::engine3D->levelMode())); });
  ui::ActionManager::instance().setGate([this](const QString &id, const QString &category) {
    const bool simulating = g::engine3D->physicsSimState() == vk::Engine3D::PhysicsSimState::Running;
    return (m_levelShown || worksWithoutLevel(id, category)) && (!simulating || worksWhileSimulating(id, category));
  });
  connect(g::engine3D, &vk::Engine3D::physicsSimChanged, this, &MainWindow::updateEditingEnabled);
  m_workspaceTimer = new QTimer(this);
  m_workspaceTimer->setSingleShot(true);
  m_workspaceTimer->setInterval(1000);
  connect(m_workspaceTimer, &QTimer::timeout, this, &MainWindow::writeWorkspace);
  connect(&proj::ProjectManager::instance(), &proj::ProjectManager::projectChanged, this,
          [this] { m_projectPanel->setProject(proj::currentProject()); });
  m_projectPanel->setProject(proj::currentProject());
  showActiveDocument();
}

void MainWindow::createDocumentBar() {
  // A row of its own under the main toolbars.
  addToolBarBreak(Qt::TopToolBarArea);
  auto *bar = addToolBar(tr("Documents"));
  bar->setObjectName(QStringLiteral("toolbar.documents"));
  bar->setMovable(false);
  bar->setFloatable(false);
  bar->setProperty("topBar", true);
  bar->setProperty("documentBar", true);
  m_tabs = new ui::DocumentTabBar(m_docs, bar);
  bar->addWidget(m_tabs);
  connect(m_tabs, &ui::DocumentTabBar::closeRequested, this, [this](int i) { closeDocument(i); });
  connect(m_tabs, &ui::DocumentTabBar::closeOthersRequested, this, [this](int i) { closeAllDocuments(i); });
  connect(m_tabs, &ui::DocumentTabBar::closeAllRequested, this, [this] { closeAllDocuments(); });
  connect(m_tabs, &ui::DocumentTabBar::revealRequested, this, [this](int i) {
    if (const proj::Document *d = m_docs->at(i)) {
      m_dockProject->show();
      m_dockProject->raise();
      m_projectPanel->reveal(d->path);
    }
  });
}

void MainWindow::showActiveDocument() {
  proj::Document *d = m_docs->active();
  QWidget *page = m_emptyPage;
  if (d && d->isLevel()) {
    page = quickContent_;
  } else if (d) {
    if (!d->view) {
      auto *viewer = new ui::ImageViewer(d->path, m_central);
      viewer->asReference = [this](const QString &path) { showReference(path); };
      d->view = viewer;
      m_central->addWidget(viewer);
    }
    page = d->view;
  }
  m_levelShown = page == quickContent_;
  if (m_central->currentWidget() != page)
    m_central->setCurrentWidget(page);
  for (QToolBar *bar: findChildren<QToolBar *>())
    if (bar->property("topBar").toBool() && !bar->property("documentBar").toBool()
        && bar->objectName() != QLatin1String("toolbar.title"))
      bar->setEnabled(m_levelShown);
  updateEditingEnabled();
  ui::ActionManager::instance().refresh();
  updateWindowTitle();
  rebuildGodotTargets();
  scheduleWorkspaceWrite();
}

void MainWindow::updateEditingEnabled() {
  const bool simulating = g::engine3D && g::engine3D->physicsSimState() == vk::Engine3D::PhysicsSimState::Running;
  if (m_toolShelf)
    m_toolShelf->setEnabled(m_levelShown && !simulating);
  for (QDockWidget *dock: {m_dockInspector, m_dockMaterials, m_dockUv})
    if (dock && dock->widget())
      dock->widget()->setEnabled(!simulating);
}

void MainWindow::openPath(const QString &path) {
  if (path.isEmpty())
    return;
  if (!proj::DocumentManager::isLevelFile(path) && !proj::DocumentManager::isImageFile(path)) {
    openExternally(path);
    return;
  }
  QString error;
  if (!m_docs->open(path, true, &error) && !error.isEmpty())
    showToast(error);
}

void MainWindow::openExternally(const QString &path) {
  if (proj::DocumentManager::isImageFile(path)) {
    ui::external::openImage(path, this);
    return;
  }
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
    showToast(tr("No program to open %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::newLevelDialog(const QString &folder) {
  if (folder.isEmpty()) {
    // No folder: an untitled level, named on first save.
    newScene();
    return;
  }
  const QString path = askLevelPath(this, folder, tr("New Level"));
  if (path.isEmpty())
    return;
  QDir().mkpath(folder);
  proj::Document *d = m_docs->newLevel(path);
  if (!d) {
    showToast(tr("Finish the current edit first"));
    return;
  }
  // On disk at once: the Project panel lists it and Save needs no dialog.
  saveLevel(d, false);
}

void MainWindow::newPlayground() {
  newScene();
  if (g::engine3D && m_docs->engineLevel())
    g::engine3D->buildPlayground();
}

void MainWindow::openLevelDialog() {
  QString start = QDir::homePath();
  if (const proj::Project *p = proj::currentProject())
    start = p->levelsDir();
  if (const QString cur = currentMapPath(); !cur.isEmpty())
    start = QFileInfo(cur).absolutePath();
  const QString path = ui::FileDialog::getOpenFileName(this, tr("Open Level"), start, eng::MapLoader::loadNameFilter());
  if (!path.isEmpty())
    openPath(path);
}

void MainWindow::newScene() {
  if (!m_docs->newLevel()) {
    showToast(tr("Finish the current edit first"));
    return;
  }
  // A new level starts in Object mode with the quad view.
  if (QObject *root = quickContent_ ? quickContent_->rootObject() : nullptr)
    QMetaObject::invokeMethod(root, "resetModeLayouts");
  setViewportLayout(1);
  qInfo() << "[map] new empty level started";
}

bool MainWindow::loadMapFrom(const QString &path) {
  if (path.isEmpty())
    return false;
  // Read from disk even when open (scripts reload what they saved).
  if (proj::Document *open = m_docs->find(path))
    m_docs->close(m_docs->indexOf(open));
  QString error;
  proj::Document *d = m_docs->open(path, true, &error);
  if (!d) {
    if (!error.isEmpty())
      showToast(error);
    return false;
  }
  return true;
}

bool MainWindow::saveMapTo(const QString &path) {
  proj::Document *d = m_docs->engineLevel();
  if (!d)
    d = m_docs->newLevel();
  if (!d)
    return false;
  const QString before = d->path;
  m_docs->setPath(d, fmap::withFileSuffix(path));
  if (!saveLevel(d, false)) {
    m_docs->setPath(d, before);
    return false;
  }
  return true;
}

bool MainWindow::saveLevel(proj::Document *d, const bool askPath) {
  if (!d || !d->isLevel())
    return false;
  QString target = d->path;
  const proj::Project *project = proj::currentProject();
  if (target.isEmpty() && project) {
    if (!askPath)
      return false;
    const QString dir = project->levelsDir();
    target = askLevelPath(this, dir, tr("Save %1").arg(d->fileName()));
    if (target.isEmpty())
      return false;
    QDir().mkpath(dir);
  } else if (target.isEmpty() || !fmap::isWritablePath(target)) {
    // Never saved without a project, or a .vmf (read-only): ask where its .ghmap goes (next to it, else home).
    if (!askPath)
      return false;
    const QString name = (target.isEmpty() ? QStringLiteral("untitled") : QFileInfo(target).completeBaseName())
                       + QStringLiteral(".ghmap");
    const QString start = target.isEmpty() ? QDir(QDir::homePath()).filePath(name)
                                           : QDir(QFileInfo(target).absolutePath()).filePath(name);
    target = ui::FileDialog::getSaveFileName(this, tr("Save %1").arg(d->fileName()), start,
                                             QLatin1String(fmap::kFileFilter));
    if (target.isEmpty())
      return false;
    target = fmap::withFileSuffix(target);
  }
  if (!fmap::isWritablePath(target)) {
    qWarning().noquote() << QStringLiteral("[map] refusing to save '%1': only .ghmap can be written").arg(target);
    return false;
  }
  fmap::Document doc;
  if (!m_docs->withLevel(d, [&doc] { doc = g::engine3D->captureMapDocument(); }))
    return false;
  doc.meta.mapName = QFileInfo(target).completeBaseName();
  QString error;
  if (!fmap::saveMap(target, doc, &error)) {
    qWarning().noquote() << QStringLiteral("[map] failed to save '%1': %2").arg(target, error);
    showToast(tr("Could not save %1: %2").arg(QFileInfo(target).fileName(), error));
    return false;
  }
  if (QDir::cleanPath(QFileInfo(target).absoluteFilePath()) != d->path)
    m_docs->setPath(d, target);
  m_docs->markClean(d);
  qInfo().noquote() << QStringLiteral("[map] saved '%1'").arg(target);
  return true;
}

bool MainWindow::saveLevelAs() {
  proj::Document *d = m_docs->engineLevel();
  if (!d)
    return false;
  QString start = d->path;
  if (start.isEmpty())
    start = proj::currentProject() ? proj::currentProject()->levelsDir() : QDir::homePath();
  else if (!fmap::isWritablePath(start)) // a .vmf: offer its .ghmap
    start = QDir(QFileInfo(start).absolutePath()).filePath(QFileInfo(start).completeBaseName() + QStringLiteral(".ghmap"));
  QString target = ui::FileDialog::getSaveFileName(this, tr("Save Level As"), start, QLatin1String(fmap::kFileFilter));
  if (target.isEmpty())
    return false;
  target = fmap::withFileSuffix(target);
  if (proj::Document *other = m_docs->find(target); other && other != d) {
    QMessageBox::warning(this, tr("Save Level As"), tr("%1 is open in another tab.").arg(QFileInfo(target).fileName()));
    return false;
  }
  const QString before = d->path;
  m_docs->setPath(d, target);
  if (!saveLevel(d, false)) {
    m_docs->setPath(d, before);
    return false;
  }
  showToast(tr("Saved %1").arg(QFileInfo(target).fileName()));
  return true;
}

bool MainWindow::saveAll() {
  // In tab order; a never-saved level asks where.
  QList<quint64> written;
  QStringList names;
  for (proj::Document *d: m_docs->levels()) {
    if (!m_docs->dirty(d) && !(d == m_docs->engineLevel() && d->path.isEmpty()))
      continue;
    if (!saveLevel(d, true))
      return false;
    written << d->serial;
    names << d->fileName();
  }
  if (written.isEmpty()) {
    showToast(tr("Nothing to save"));
    return true;
  }
  const QString saved = tr("Saved %1").arg(names.join(QStringLiteral(", ")));
  if (!exportOnSave()) {
    showToast(saved);
    return true;
  }
  const QString root = godotSaveTarget();
  const QString err = root.isEmpty() ? tr("no Godot project to export to") : eng::godot::checkProject(root);
  if (!err.isEmpty()) {
    showToast(tr("%1 · not exported: %2").arg(saved, err));
    return true;
  }
  showToast(saved);
  exportLevels(written, root);
  return true;
}

bool MainWindow::askAboutUnsaved(const QList<proj::Document *> &levels, const QString &question) {
  if (levels.isEmpty() || scriptedRun())
    return true;
  QStringList names;
  for (const proj::Document *d: levels)
    names << d->fileName();
  const auto choice = QMessageBox::question(
      this, tr("Unsaved changes"),
      tr("%1 %2 unsaved changes.\n%3").arg(names.join(QStringLiteral(", ")),
                                         levels.size() == 1 ? tr("has") : tr("have"), question),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
  if (choice == QMessageBox::Cancel)
    return false;
  if (choice == QMessageBox::Save)
    for (proj::Document *d: levels)
      if (!saveLevel(d, true))
        return false;
  return true;
}

bool MainWindow::confirmDiscardChanges() {
  return askAboutUnsaved(m_docs->dirtyLevels(), tr("Save before closing?"));
}

bool MainWindow::closeDocument(const int index) {
  proj::Document *d = m_docs->at(index);
  if (!d)
    return false;
  if (m_docs->dirty(d) && !askAboutUnsaved({d}, tr("Save before closing it?")))
    return false;
  if (d == m_docs->engineLevel() && !g::engine3D->canSwitchDocument()) {
    showToast(tr("Finish the current edit first"));
    return false;
  }
  m_docs->close(index);
  return true;
}

bool MainWindow::closeAllDocuments(const int except) {
  QList<proj::Document *> dirty;
  for (proj::Document *d: m_docs->dirtyLevels())
    if (m_docs->indexOf(d) != except)
      dirty << d;
  if (!askAboutUnsaved(dirty, tr("Save before closing?")))
    return false;
  if (!g::engine3D->canSwitchDocument()) {
    showToast(tr("Finish the current edit first"));
    return false;
  }
  m_docs->closeAll(m_docs->at(except));
  return true;
}

bool MainWindow::closeDocumentsUnder(const QString &path) {
  const QString clean = QDir::cleanPath(path);
  QList<proj::Document *> under;
  for (int i = 0; i < m_docs->count(); ++i) {
    proj::Document *d = m_docs->at(i);
    if (d->path == clean || d->path.startsWith(clean + QLatin1Char('/')))
      under << d;
  }
  // The file is deleted: nothing to ask about unsaved changes.
  for (const proj::Document *d: under)
    m_docs->close(m_docs->indexOf(d));
  return true;
}

bool MainWindow::closeProjectDocuments() {
  if (!confirmDiscardChanges())
    return false;
  writeWorkspace();
  if (!g::engine3D->canSwitchDocument()) {
    showToast(tr("Finish the current edit first"));
    return false;
  }
  m_docs->closeAll();
  return true;
}

void MainWindow::scheduleWorkspaceWrite() {
  if (m_workspaceTimer && proj::currentProject())
    m_workspaceTimer->start();
}

void MainWindow::writeWorkspace() {
  const proj::Project *p = proj::currentProject();
  if (!p)
    return;
  QJsonArray tabs;
  for (int i = 0; i < m_docs->count(); ++i) {
    const proj::Document *d = m_docs->at(i);
    if (d->path.isEmpty())
      continue;
    tabs.append(p->relative(d->path));
  }
  QJsonObject root;
  root.insert(QStringLiteral("tabs"), tabs);
  if (const proj::Document *a = m_docs->active(); a && !a->path.isEmpty())
    root.insert(QStringLiteral("active"), p->relative(a->path));
  root.insert(QStringLiteral("expanded"), QJsonArray::fromStringList(m_projectPanel->expandedFolders()));
  if (!m_reference->image().isEmpty())
    root.insert(QStringLiteral("reference"), p->relative(m_reference->image()));
  QDir().mkpath(p->metaDir());
  QSaveFile f(p->workspaceFile());
  if (f.open(QIODevice::WriteOnly)) {
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.commit();
  }
}

void MainWindow::openProjectSession() {
  const proj::Project *p = proj::currentProject();
  if (!p)
    return;
  QFile f(p->workspaceFile());
  QJsonObject root;
  if (f.open(QIODevice::ReadOnly))
    root = QJsonDocument::fromJson(f.readAll()).object();
  // The project row always opens; inner folders as left.
  QStringList expanded = root.value(QStringLiteral("expanded")).toVariant().toStringList();
  if (!expanded.contains(QStringLiteral(".")))
    expanded.prepend(QStringLiteral("."));
  m_projectPanel->setExpandedFolders(expanded);
  // The Reference Image panel's picture; whether the panel shows is the window layout's business.
  const QString reference = root.value(QStringLiteral("reference")).toString();
  m_reference->setImage(reference.isEmpty() || !QFileInfo::exists(p->path(reference)) ? QString() : p->path(reference));
  // Every tab comes back; only the shown one is read now, the others on first show.
  QString active;
  for (const QJsonValue &v: root.value(QStringLiteral("tabs")).toArray()) {
    const QString path = p->path(v.toString());
    if (QFileInfo::exists(path))
      m_docs->open(path, false);
  }
  if (const QString a = root.value(QStringLiteral("active")).toString(); !a.isEmpty())
    active = p->path(a);
  proj::Document *show = m_docs->find(active);
  if (!show && m_docs->count() > 0)
    show = m_docs->at(0);
  if (show) {
    QString error;
    if (!m_docs->activate(m_docs->indexOf(show), &error) && !error.isEmpty())
      showToast(error);
  }
  showActiveDocument();
}

void MainWindow::showReference(const QString &path) {
  m_reference->setImage(path);
  m_dockReference->show();
  m_dockReference->raise();
  scheduleWorkspaceWrite();
}

void MainWindow::setupProjectGodot(const QString &dir) {
  const proj::Project *p = proj::currentProject();
  QString error;
  if (!proj::writeNewGodotProject(dir, p ? p->name() : QFileInfo(dir).fileName(), &error, proj::newGodotSettings())) {
    QMessageBox::warning(this, tr("Create Godot Project"), error);
    return;
  }
  showToast(tr("Godot project created in %1").arg(QFileInfo(dir).fileName()));
  rebuildGodotTargets();
}
