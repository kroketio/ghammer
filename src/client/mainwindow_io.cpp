// Map file I/O, debug hooks, the self-tests and the Godot export driver.
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkReply>
#include <QComboBox>
#include <QPointer>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QToolBar>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QtConcurrent/QtConcurrentRun>

#include "client/ctx.h"
#include "client/engine/brush.h"
#include "client/engine/godot_export.h"
#include "client/engine/sky_info.h"
#include "client/engine/godot_io_export.h"
#include "client/engine/godot_text.h"
#include "client/net/asset_client.h"
#include "client/engine/map_format.h"
#include "client/engine/map_loader.h"
#include "client/engine/scene.h"
#include "client/prefs/preferences.h"
#include "client/prefs/preferences_dialog.h"
#include "client/engine/color_lut.h"
#include "client/ui/action_manager.h"
#include "client/ui/lut_window.h"
#include "client/ui/pack_install_window.h"
#include "client/net/pack_installer.h"
#include "client/ui/lut_levels.h"
#include "client/ui/scale.h"
#include "client/vulkan/vk_engine.h"
#include "client/vulkan/vk_gridview.h"
#include "shared/lib/config.h"
#include "mainwindow.h"
#include "client/net/texture_catalog.h"
#include "client/ui/copy_texture_dialog.h"
#include "client/ui/material_browser.h"
#include "client/ui/inspector.h"
#include "client/ui/texture_browser.h"
#include "client/ui/texture_packs.h"
#include "client/ui/file_dialog.h"
#include "client/project/document_manager.h"
#include "client/project/godot_project.h"
#include "client/project/project.h"
#include "client/project/project_manager.h"
#include <QJsonObject>
#include <QStandardItemModel>
#include <QLabel>
#include "ghammer_version.h"
#include <optional>
#include <QApplication>
#include <QDockWidget>

void runSelfTest(MainWindow *w);

void MainWindow::maybeReopenLastMap() {
  // EDITOR_LUTDUMP=<dir>: render the LUT window to PNGs (EDITOR_LUTDUMP_REF selects a LUT) and quit. Needs an
  // asset server with LUTs.
  if (const QString dir = qEnvironmentVariable("EDITOR_LUTDUMP"); !dir.isEmpty()) {
    QTimer::singleShot(800, this, [this, dir] {
      auto *win = new ui::LutWindow(g::engine3D, this);
      win->show();
      const QString ref = qEnvironmentVariable("EDITOR_LUTDUMP_REF");
      QTimer::singleShot(2000, this, [win, dir, ref] {
        if (!ref.isEmpty())
          win->selectLut(ref);
        QTimer::singleShot(1500, win, [win, dir] {
          win->grab().save(dir + QStringLiteral("/lutwin.png"));
          const eng::LutGrade *g = win->grade(win->selectedRef());
          QWidget *curves = nullptr;
          if (g)
            for (const auto &st: g->steps)
              if (st.kind == eng::GradeStep::Kind::Curves && !curves)
                curves = win->openStepWindow(win->selectedRef(), st.id);
          QTimer::singleShot(400, win, [curves, dir] {
            if (curves)
              curves->grab().save(dir + QStringLiteral("/lutcurves.png"));
            QCoreApplication::quit();
          });
        });
      });
    });
  }
  // EDITOR_PACKDUMP=<dir>: render Install Asset Packs to PNGs and quit; with EDITOR_PACKDUMP_FILE (a zip) or
  // EDITOR_PACKDUMP_URL it installs that pack, dumps while it runs and once it is done.
  if (const QString dir = qEnvironmentVariable("EDITOR_PACKDUMP"); !dir.isEmpty()) {
    QDir().mkpath(dir);
    QTimer::singleShot(1500, this, [this, dir] {
      auto *win = ui::PackInstallWindow::open(this);
      const QString file = qEnvironmentVariable("EDITOR_PACKDUMP_FILE");
      const QString url = qEnvironmentVariable("EDITOR_PACKDUMP_URL");
      if (file.isEmpty() && url.isEmpty()) {
        QTimer::singleShot(500, win, [win, dir] {
          win->grab().save(dir + QStringLiteral("/packs.png"));
          QCoreApplication::quit();
        });
        return;
      }
      connect(win->job(), &net::PackInstaller::finished, win, [win, dir](bool ok, const QString &message) {
        qInfo().noquote() << QStringLiteral("[packs] %1: %2").arg(ok ? "ok" : "failed", message);
        QTimer::singleShot(300, win, [win, dir] {
          win->grab().save(dir + QStringLiteral("/packs_done.png"));
          QCoreApplication::quit();
        });
      });
      connect(win->job(), &net::PackInstaller::notice, win,
              [](const QString &text) { qInfo().noquote() << QStringLiteral("[packs] %1").arg(text); });
      connect(win->job(), &net::PackInstaller::phaseChanged, win, [win, dir](net::PackInstaller::Phase p) {
        qInfo().noquote() << "[packs] phase" << QVariant::fromValue(p).toString();
        if (p == net::PackInstaller::Phase::Uploading || p == net::PackInstaller::Phase::Downloading)
          QTimer::singleShot(700, win, [win, dir] { win->grab().save(dir + QStringLiteral("/packs_busy.png")); });
      });
      if (!file.isEmpty())
        win->installFile(file);
      else
        win->installUrl(QUrl(url));
    });
  }
  // EDITOR_TEXDUMP=<dir>: render the texture windows and docks to PNGs and quit. Needs an asset server.
  if (const QString dir = qEnvironmentVariable("EDITOR_TEXDUMP"); !dir.isEmpty()) {
    QDir().mkpath(dir);
    QTimer::singleShot(2500, this, [this, dir] {
      auto *e = g::engine3D;
      // A box with the first server texture, one face selected.
      e->createPrimitive(0, {0, 0, 0}, {25.6f, 25.6f, 25.6f});
      QJsonObject q;
      q.insert("limit", 1);
      net::TextureCatalog::instance().query(q, this, [this, dir, e](const net::QueryPage &p) {
        // EDITOR_TEXDUMP_REF=pack/Name picks the texture instead of the first one.
        const QString wanted = qEnvironmentVariable("EDITOR_TEXDUMP_REF");
        const QString ref = !wanted.isEmpty() ? wanted : p.items.isEmpty() ? QString() : p.items.first().ref;
        if (!ref.isEmpty())
          e->applyMaterialToSelection(ref);
        auto *browser = ui::TextureBrowserWindow::open(e, this, ref);
        browser->resize(1580, 960);
        // The 3D preview: Cube + Sun, then Sphere + Omni, then back to 2D.
        auto clickSwitch = [browser](const QString &text) {
          for (auto *b: browser->findChildren<QToolButton *>(QStringLiteral("tbSeg")))
            if (b->text() == text)
              b->click();
        };
        QTimer::singleShot(1500, this, [=] { clickSwitch(QStringLiteral("Cube")), clickSwitch(QStringLiteral("Sun")); });
        QTimer::singleShot(4000, this, [=] {
          browser->grab().save(dir + QStringLiteral("/texture_browser_cube.png"));
          clickSwitch(QStringLiteral("Sphere")), clickSwitch(QStringLiteral("Omni"));
        });
        QTimer::singleShot(5500, this, [=] {
          browser->grab().save(dir + QStringLiteral("/texture_browser_sphere.png"));
          clickSwitch(QStringLiteral("Plane")), clickSwitch(QStringLiteral("Sun"));
        });
        QTimer::singleShot(7000, this, [=] {
          browser->grab().save(dir + QStringLiteral("/texture_browser_plane.png"));
          clickSwitch(QStringLiteral("2D"));
        });
        QTimer::singleShot(7500, this, [this, dir, e, ref, browser] {
          browser->grab().save(dir + QStringLiteral("/texture_browser.png"));
          auto *packs = ui::TexturePacksWindow::open(e, this, ref);
          packs->resize(1500, 900);
          if (const auto obj = e->scene()->gameObjects.back(); obj && obj->getMesh()) {
            e->setMaterialMode(true);
            e->setSelected({}, {obj->getMesh()->getFacePickingID(0)});
          }
          m_dockInspector->show();
          m_dockInspector->raise();
          m_inspector->revealTextureProperties();
          QTimer::singleShot(3000, this, [this, dir, packs, ref] {
            packs->grab().save(dir + QStringLiteral("/texture_packs.png"));
            m_inspector->resize(m_inspector->width(), 1800);
            m_inspector->grab().save(dir + QStringLiteral("/inspector.png"));
            m_materials->grab().save(dir + QStringLiteral("/materials_dock.png"));
            ui::CopyTextureDialog::run(g::engine3D, ref, this);
            QTimer::singleShot(1200, this, [dir] {
              for (QWidget *w: QApplication::topLevelWidgets())
                if (auto *dlg = qobject_cast<ui::CopyTextureDialog *>(w); dlg && dlg->isVisible())
                  dlg->grab().save(dir + QStringLiteral("/copy_dialog.png"));
              QCoreApplication::quit();
            });
          });
        });
      });
    });
  }
  // EDITOR_PREFSDUMP=<dir>: render a few Preferences pages to PNGs and quit.
  if (const QString dir = qEnvironmentVariable("EDITOR_PREFSDUMP"); !dir.isEmpty()) {
    QTimer::singleShot(800, this, [this, dir] {
      prefs::PreferencesDialog::showDialog(this, QString());
      QTimer::singleShot(300, this, [dir] {
        auto *dlg = prefs::PreferencesDialog::instance();
        if (!dlg)
          return;
        for (const QString &cat: {QStringLiteral("General"), QStringLiteral("Rendering"),
                                  QStringLiteral("Rendering/Shadows"), QStringLiteral("Grid & Snap"),
                                  QStringLiteral("Keyboard")}) {
          dlg->selectCategory(cat);
          QCoreApplication::processEvents();
          QString name = cat;
          name.replace(QLatin1Char('/'), QLatin1Char('_')).replace(QLatin1Char(' '), QLatin1Char('_'))
              .replace(QLatin1Char('&'), QLatin1Char('n'));
          dlg->grab().save(dir + QStringLiteral("/prefs_") + name + QStringLiteral(".png"));
        }
        QCoreApplication::quit();
      });
    });
  }
  // EDITOR_SKYBOX=<ref>: set the skybox (with its lighting) once the map is loaded, before EDITOR_GODOT_EXPORT.
  static bool skyboxHook = false;
  if (const QString sky = qEnvironmentVariable("EDITOR_SKYBOX"); !sky.isEmpty() && !skyboxHook) {
    skyboxHook = true;
    QTimer::singleShot(1500, this, [sky] {
      net::TextureCatalog::instance().resolve({sky}, g::engine3D, [sky] {
        const TextureEntry *e = net::TextureCatalog::instance().find(sky);
        g::engine3D->applySkybox(sky, eng::skyInfoFromJson(e ? e->sky : QJsonObject()));
        qInfo().noquote() << QStringLiteral("[sky] %1 (%2)").arg(sky, e && e->isSky() ? QStringLiteral("a skybox")
                                                                                    : QStringLiteral("unknown"));
      });
    });
  }
  // EDITOR_PHYSICSDUMP=<seconds>: once the physics world is built, step it, print each rigid body's start / end
  // position, quit.
  static bool physicsHook = false;
  if (const QString secs = qEnvironmentVariable("EDITOR_PHYSICSDUMP"); !secs.isEmpty() && !physicsHook) {
    physicsHook = true;
    QTimer::singleShot(1000, this, [secs] {
      g::engine3D->runPhysicsCheck(std::max(0.0, secs.toDouble()), [](const QString &report) {
        qInfo().noquote() << report;
        QCoreApplication::exit(0);
      });
    });
  }
  // EDITOR_GODOT_EXPORT=<project root>: export once the map is loaded (and the server had time to connect), quit.
  static bool godotExportHook = false;
  if (const QString root = qEnvironmentVariable("EDITOR_GODOT_EXPORT"); !root.isEmpty() && !godotExportHook) {
    godotExportHook = true;
    QTimer::singleShot(3000, this, [this, root] {
      if (const QString err = eng::godot::checkProject(root); !err.isEmpty()) {
        qWarning().noquote() << QStringLiteral("[godot] %1").arg(err);
        QCoreApplication::exit(1);
        return;
      }
      exportGodotTo(root, [](const QString &summary, bool ok) {
        qInfo().noquote() << QStringLiteral("[godot] %1").arg(summary);
        QCoreApplication::exit(ok ? 0 : 1);
      });
    });
  }
  // EDITOR_DUMPLIGHTS=1: print the lighting setup and brush bounds after load.
  if (!qEnvironmentVariableIsEmpty("EDITOR_DUMPLIGHTS")) {
    QTimer::singleShot(500, this, [] {
      auto *e = g::engine3D;
      // EDITOR_DUMPLIGHTS=nosun / none: switch the sun off to measure each contribution (with EDITOR_LUTDEBUG).
      const QString what = qEnvironmentVariable("EDITOR_DUMPLIGHTS");
      // EDITOR_CAMLOOK=ex,ey,ez,tx,ty,tz: camera at e looking at t.
      if (const QStringList c = qEnvironmentVariable("EDITOR_CAMLOOK").split(QLatin1Char(',')); c.size() == 6) {
        const glm::vec3 eye(c[0].toFloat(), c[1].toFloat(), c[2].toFloat());
        const glm::vec3 d = glm::normalize(glm::vec3(c[3].toFloat(), c[4].toFloat(), c[5].toFloat()) - eye);
        e->camera()->setPose(eye, glm::degrees(std::atan2(-d.z, d.x)), glm::degrees(std::asin(d.y)));
        e->requestRedraw();
      }
      if (what == QStringLiteral("nosun") || what == QStringLiteral("none"))
        e->setSunIntensity(0.0f);
      if (qEnvironmentVariableIsSet("EDITOR_DUMP_STRENGTH1"))
        e->setShadowStrength(1.0f);
      auto *cam = e->camera();
      qInfo("[dump] camera (%.2f %.2f %.2f) yaw %.1f pitch %.1f", cam->getPosition().x, cam->getPosition().y,
            cam->getPosition().z, cam->getYaw(), cam->getPitch());
      const glm::vec3 sd = e->sunDirection();
      e->refreshViewUniforms();
      for (const auto &m: e->entityMarkers()) {
        const glm::vec2 sp = e->screenPosOf(m.position);
        qInfo("[dump] marker %u icon %d screen (%.1f, %.1f) of %dx%d half %.1f", m.id, m.icon, sp.x, sp.y,
              e->viewPixelSize().width(), e->viewPixelSize().height(), e->entityIconHalfPx());
      }
      qInfo("[dump] sun dir (%.3f %.3f %.3f) shadows %d ambient %s x%.2f", sd.x, sd.y, sd.z, int(e->shadowsEnabled()),
            qPrintable(e->ambientColor().name()), e->ambientIntensity());
      for (const auto &en: e->scene()->entities) {
        QStringList props;
        for (auto it = en.props.cbegin(); it != en.props.cend(); ++it)
          props << it.key() + QLatin1Char('=') + it.value().toString();
        qInfo("[dump] entity %u %s at (%.2f %.2f %.2f) %s", en.id, qPrintable(en.classname), en.origin.x, en.origin.y,
              en.origin.z, qPrintable(props.join(QLatin1Char(' '))));
      }
      for (const auto &o: e->scene()->gameObjects) {
        const auto [mn, mx] = o->getWorldAABB();
        qInfo("[dump] brush %u faces %d (%.2f %.2f %.2f)..(%.2f %.2f %.2f)", o->stableId(),
              o->getMesh()->faceCount(), mn.x, mn.y, mn.z, mx.x, mx.y, mx.z);
      }
    });
  }
  // EDITOR_MOVETEST=1: strafe the camera every 16 ms for 3 s (the engine logs each frame's draw counts).
  if (!qEnvironmentVariableIsEmpty("EDITOR_MOVETEST")) {
    auto *t = new QTimer(this);
    connect(t, &QTimer::timeout, this, [t] {
      static int n = 0;
      auto *cam = g::engine3D->camera();
      cam->setPose(cam->getPosition() + cam->getRight() * 0.6f, cam->getYaw(), cam->getPitch());
      g::engine3D->requestRedraw();
      if (++n > 180) {
        t->stop();
        QCoreApplication::quit();
      }
    });
    QTimer::singleShot(1500, t, [t] { t->start(16); });
  }
  if (!qEnvironmentVariableIsEmpty("EDITOR_SELFTEST")) {
    QTimer::singleShot(1500, this, [this] { runSelfTest(this); });
    return;
  }
  // An explicit map (first non-option argument, or EDITOR_OPEN_MAP) wins; else the project's tabs as left.
  if (const QString map = commandLineMap(); !map.isEmpty()) {
    loadMapFrom(map);
    // EDITOR_DOCBENCH=<second map>: open it in a second tab and switch between the two ([docs] lines).
    if (const QString second = qEnvironmentVariable("EDITOR_DOCBENCH"); !second.isEmpty()) {
      QTimer::singleShot(1500, this, [this, second] {
        m_docs->open(second);
        auto *flips = new QTimer(this);
        auto n = std::make_shared<int>(0);
        connect(flips, &QTimer::timeout, this, [this, flips, n] {
          if (++*n > 6) {
            flips->stop();
            QCoreApplication::quit();
            return;
          }
          m_docs->activate(m_docs->activeIndex() == 0 ? 1 : 0);
        });
        flips->start(1000);
      });
    }
    return;
  }
  // Without a project (scripted runs): an untitled level.
  if (!proj::currentProject()) {
    newScene();
    return;
  }
  openProjectSession();
}

// "gHammer 0.1.0 - [my_game/e1m1*]": version, project and the tab shown, * while unsaved.
void MainWindow::updateWindowTitle() {
  const proj::Project *p = proj::currentProject();
  if (m_titleLabel)
    m_titleLabel->setText(p ? p->name() : QStringLiteral("gHammer"));
  const proj::Document *d = m_docs ? m_docs->active() : nullptr;
  QStringList where;
  if (p)
    where << p->name();
  if (d)
    where << (d->isLevel() && !d->path.isEmpty() ? QFileInfo(d->path).completeBaseName() : d->fileName());
  QString title = QStringLiteral("gHammer " GHAMMER_VERSION);
  if (!where.isEmpty())
    // [*] is Qt's placeholder: "*" while the window is marked modified.
    title += QStringLiteral(" - [%1%2]").arg(where.join(QLatin1Char('/')), d ? QStringLiteral("[*]") : QString());
  setWindowTitle(title);
  setWindowModified(d && m_docs->dirty(d));
}

// Godot projects (config, free-form keys): Godot/RecentProjects newest first; Godot/ExportOnSave the toolbar
// toggle; Godot/ExportOnSaveProject its project; Godot/MapProjects map file -> last project (the combo follows it).

namespace {

const QString kGodotRecent = QStringLiteral("Godot/RecentProjects");
const QString kGodotOnSave = QStringLiteral("Godot/ExportOnSave");
const QString kGodotOnSaveProject = QStringLiteral("Godot/ExportOnSaveProject");
const QString kGodotMapProjects = QStringLiteral("Godot/MapProjects");
constexpr int kGodotRecentMax = 10;

// Hidden faces of the last export per level (serial): reused while the world brushes are the same.
QHash<quint64, std::shared_ptr<const eng::godot::HiddenFaces>> s_godotHidden;

// "My Game · ~/Documents/my_game": project.godot's config/name and where it is.
QString godotProjectLabel(const QString &root) {
  QString name;
  QFile f(QDir(root).filePath(QStringLiteral("project.godot")));
  if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
    static const QRegularExpression re(QStringLiteral(R"re(^config/name="(.*)"\s*$)re"),
                                       QRegularExpression::MultilineOption);
    name = re.match(QString::fromUtf8(f.read(64 * 1024))).captured(1);
  }
  QString where = QDir::toNativeSeparators(root);
  const QString home = QDir::homePath();
  if (where.startsWith(home + QDir::separator()))
    where = QStringLiteral("~") + where.mid(home.size());
  return name.isEmpty() ? where : QStringLiteral("%1 · %2").arg(name, where);
}

} // namespace

QStringList MainWindow::recentGodotProjects() const {
  QStringList out;
  for (const QString &p: config()->getRaw(kGodotRecent).toStringList())
    if (!out.contains(p) && QFileInfo::exists(QDir(p).filePath(QStringLiteral("project.godot"))))
      out << p;
  return out;
}

void MainWindow::rememberGodotProject(const QString &root) {
  QStringList list = config()->getRaw(kGodotRecent).toStringList();
  list.removeAll(root);
  list.prepend(root);
  while (list.size() > kGodotRecentMax)
    list.removeLast();
  config()->setRaw(kGodotRecent, list);
  // Without a project each map remembers its Godot project; a project's own target (Export/Target) is not changed
  // by a manual export elsewhere.
  const QString map = currentMapPath();
  if (!proj::currentProject() && !map.isEmpty()) {
    QVariantMap maps = config()->getRaw(kGodotMapProjects).toMap();
    maps.insert(QFileInfo(map).absoluteFilePath(), root);
    config()->setRaw(kGodotMapProjects, maps);
  }
  rebuildGodotTargets();
}

QString MainWindow::pickGodotProject() {
  QString start = prefs::get().value(QStringLiteral("Godot/ExportDir")).toString();
  if (start.isEmpty()) {
    const QString map = currentMapPath();
    start = map.isEmpty() ? QDir::homePath() : QFileInfo(map).absolutePath();
  }
  const QString dir = ui::FileDialog::getExistingDirectory(this, tr("Export to Godot: the Godot project folder"), start,
                                                                tr("Select Project"));
  if (dir.isEmpty())
    return {};
  QString root = dir;
  if (!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("project.godot")))) {
    const QString up = eng::godot::findProjectRoot(dir);
    if (up.isEmpty()) {
      QMessageBox::warning(this, tr("Export to Godot"), tr("%1 is not a Godot project (no project.godot).").arg(dir));
      return {};
    }
    if (QMessageBox::question(this, tr("Export to Godot"), tr("Export into the project at %1?").arg(up))
        != QMessageBox::Yes)
      return {};
    root = up;
  }
  if (const QString err = eng::godot::checkProject(root); !err.isEmpty()) {
    QMessageBox::warning(this, tr("Export to Godot"), err);
    return {};
  }
  prefs::get().setValue(QStringLiteral("Godot/ExportDir"), root);
  return root;
}

void MainWindow::exportGodot() {
  vk::Engine3D *e = g::engine3D;
  if (!e || !e->scene())
    return;
  if (const QString root = pickGodotProject(); !root.isEmpty())
    exportGodotToProject(root, true);
}

void MainWindow::exportGodotToProject(const QString &root, const bool interactive) {
  if (const QString err = eng::godot::checkProject(root); !err.isEmpty()) {
    if (interactive)
      QMessageBox::warning(this, tr("Export to Godot"), err);
    else
      showToast(tr("Not exported: %1").arg(err));
    return;
  }
  QPointer<MainWindow> self(this);
  exportGodotTo(root, [self, interactive](const QString &summary, bool ok) {
    if (!self)
      return;
    if (ok || !interactive)
      self->showToast(summary);
    else
      QMessageBox::warning(self, tr("Export to Godot"), summary);
  });
}

void MainWindow::populateGodotExportMenu(QMenu *menu) {
  menu->clear();
  menu->setToolTipsVisible(true);
  QStringList projects = recentGodotProjects();
  if (const proj::Project *project = proj::currentProject()) {
    const QString game = QDir::cleanPath(project->gameDir());
    projects.removeAll(game);
    if (eng::godot::checkProject(game).isEmpty())
      projects.prepend(game);
  }
  for (const QString &p: projects) {
    QAction *a = menu->addAction(ui::ActionManager::icon(QStringLiteral("export")), godotProjectLabel(p), this,
                                 [this, p] { exportGodotToProject(p, true); });
    a->setToolTip(QDir::toNativeSeparators(p));
  }
  if (projects.isEmpty())
    menu->addAction(tr("(no recent projects)"))->setEnabled(false);
  menu->addSeparator();
  menu->addAction(ui::ActionManager::icon(QStringLiteral("file_folder")), tr("Other Project…"), this,
                  [this] { exportGodot(); });
}

QString MainWindow::godotSaveTarget() const {
  return m_godotTarget ? m_godotTarget->currentData().toString() : QString();
}

namespace {
// "game" (its own Godot project), a path relative to the project, or an absolute one.
QString projectExportTarget(const proj::Project &p) {
  const QString t = prefs::s("Export/Target").trimmed();
  if (t.isEmpty())
    return p.gameDir();
  return QDir::cleanPath(QDir::isAbsolutePath(t) ? t : p.path(t));
}
} // namespace

// The Godot project the toolbar's target names (or the open project's own), when it is one.
QString MainWindow::godotStartDir() const {
  QString dir = godotSaveTarget();
  if (dir.isEmpty())
    if (const proj::Project *p = proj::currentProject())
      dir = projectExportTarget(*p);
  return !dir.isEmpty() && QFileInfo::exists(QDir(dir).filePath(QStringLiteral("project.godot"))) ? dir : QString();
}

void MainWindow::rebuildGodotTargets() {
  if (!m_godotTarget)
    return;
  const proj::Project *project = proj::currentProject();
  QStringList projects = recentGodotProjects();
  QString want;
  QSignalBlocker block(m_godotTarget);
  m_godotTarget->clear();
  if (project) {
    // The project's own game/ first (even before it is a Godot project), then the other Godot projects exported to.
    const QString game = QDir::cleanPath(project->gameDir());
    projects.removeAll(game);
    const QString why = eng::godot::checkProject(game);
    const QString name = proj::godotProjectName(game);
    m_godotTarget->addItem(why.isEmpty() ? QStringLiteral("%1 · game").arg(name.isEmpty() ? project->name() : name)
                                         : tr("game (no Godot project)"),
                           game);
    m_godotTarget->setItemData(0, why.isEmpty() ? QDir::toNativeSeparators(game) : why, Qt::ToolTipRole);
    if (!why.isEmpty())
      if (auto *model = qobject_cast<QStandardItemModel *>(m_godotTarget->model()))
        model->item(0)->setEnabled(false);
    want = projectExportTarget(*project);
  } else {
    // The open map's own project first, else the last choice, else the newest.
    if (const QString map = currentMapPath(); !map.isEmpty())
      want = config()->getRaw(kGodotMapProjects).toMap().value(QFileInfo(map).absoluteFilePath()).toString();
    if (!projects.contains(want))
      want = config()->getRaw(kGodotOnSaveProject).toString();
    if (!projects.contains(want))
      want = projects.value(0);
  }
  for (const QString &p: projects) {
    m_godotTarget->addItem(godotProjectLabel(p), p);
    m_godotTarget->setItemData(m_godotTarget->count() - 1, QDir::toNativeSeparators(p), Qt::ToolTipRole);
  }
  if (project && m_godotTarget->findData(want) < 0 && !want.isEmpty()) {
    m_godotTarget->addItem(godotProjectLabel(want), want); // a target no longer in the recents
    m_godotTarget->setItemData(m_godotTarget->count() - 1, QDir::toNativeSeparators(want), Qt::ToolTipRole);
  }
  m_godotTarget->addItem(tr("Choose…"));  // no data: opens the folder dialog
  m_godotTarget->setCurrentIndex(std::max(0, m_godotTarget->findData(want)));
  m_godotTarget->setEnabled(exportOnSave());
}

void MainWindow::setupGodotSaveTarget(QToolBar *bar) {
  m_godotTarget = new QComboBox(bar);
  m_godotTarget->setToolTip(tr("Godot project to auto-export to"));
  m_godotTarget->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  m_godotTarget->setMaximumWidth(ui::scale::tpx(320));
  bar->addWidget(m_godotTarget);
  connect(m_godotTarget, &QComboBox::activated, this, [this](int row) {
    QString root = m_godotTarget->itemData(row).toString();
    if (root.isEmpty())
      root = pickGodotProject();
    if (root.isEmpty()) {
      rebuildGodotTargets();  // back to the previous project
      return;
    }
    if (const proj::Project *p = proj::currentProject()) {
      // Project setting: "game" for its own, else relative when inside it.
      const QString clean = QDir::cleanPath(root);
      prefs::get().setValue(QStringLiteral("Export/Target"),
                            clean == QDir::cleanPath(p->gameDir()) ? QStringLiteral("game") : p->relative(clean));
      if (clean != QDir::cleanPath(p->gameDir()))
        rememberGodotProject(clean);
      rebuildGodotTargets();
      return;
    }
    config()->setRaw(kGodotOnSaveProject, root);
    rememberGodotProject(root);  // newest, and this map's project from now on
  });
  rebuildGodotTargets();
}

bool MainWindow::exportOnSave() const {
  return proj::currentProject() ? prefs::b("Export/AutoExport") : config()->getRaw(kGodotOnSave).toBool();
}

void MainWindow::setExportOnSave(const bool on) {
  if (proj::currentProject())
    prefs::get().setValue(QStringLiteral("Export/AutoExport"), on);
  else
    config()->setRaw(kGodotOnSave, on);
  if (m_godotTarget)
    m_godotTarget->setEnabled(on);
  ui::ActionManager::instance().refresh();
}

bool MainWindow::withLevelSerial(const quint64 serial, const std::function<void()> &fn) {
  if (g::engine3D && serial == g::engine3D->levelSerial()) {
    fn();
    return true;
  }
  return m_docs && m_docs->withLevel(serial, fn);
}

void MainWindow::exportLevels(QList<quint64> serials, const QString &root) {
  if (serials.isEmpty())
    return;
  const quint64 serial = serials.takeFirst();
  QPointer<MainWindow> self(this);
  exportLevelTo(serial, root, [self, serials, root](const QString &summary, bool ok) {
    if (!self)
      return;
    self->showToast(summary);
    if (!ok)
      qWarning().noquote() << QStringLiteral("[godot] %1").arg(summary);
    self->exportLevels(serials, root);
  });
}

void MainWindow::exportGodotTo(const QString &root, std::function<void(const QString &, bool)> done) {
  if (!g::engine3D || !g::engine3D->scene()) {
    done(tr("No map"), false);
    return;
  }
  exportLevelTo(g::engine3D->levelSerial(), root, std::move(done));
}

void MainWindow::exportLevelTo(const quint64 serial, const QString &root,
                               std::function<void(const QString &, bool)> done) {
  vk::Engine3D *e = g::engine3D;
  rememberGodotProject(QDir(root).absolutePath());
  // Catalogue entries of every server texture the level uses, read with the level in the engine (shown or not).
  QSet<QString> bases;
  const bool found = e && withLevelSerial(serial, [e, &bases] {
    if (!e->scene())
      return;
    e->syncPropObjects(true);
    e->pumpModifiers(vk::Engine3D::ModPump::Required); // modifier output is exported, not its sources
    for (const auto &o: e->scene()->gameObjects)
      if (const auto mesh = o ? o->getMesh() : nullptr)
        for (int f = 0; f < mesh->faceCount(); ++f)
          if (const auto &m = mesh->faceMaterialRef(f))
            if (const MaterialRef r = MaterialRef::parse(m->ref); !r.isLegacy())
              bases.insert(r.base());
    if (const MaterialRef sky = MaterialRef::parse(e->skybox()); !sky.isEmpty() && !sky.isLegacy())
      bases.insert(sky.base());
    if (const MaterialRef lut = MaterialRef::parse(e->colorLut()); !lut.isEmpty() && !lut.isLegacy())
      bases.insert(lut.base());
  });
  if (!found) {
    done(tr("No map"), false);
    return;
  }
  auto started = QSharedPointer<bool>::create(false);
  QPointer<MainWindow> self(this);
  auto proceed = [self, serial, root, done, started] {
    if (!self || *started)
      return;
    *started = true;
    self->exportGodotWithEntries(serial, root, done);
  };
  if (bases.isEmpty() || !net::AssetClient::instance().isOnline()) {
    proceed();
    return;
  }
  net::TextureCatalog::instance().resolve(QStringList(bases.begin(), bases.end()), this, proceed);
  QTimer::singleShot(15000, this, proceed);  // a server that never answers
}

void MainWindow::exportGodotWithEntries(const quint64 serial, const QString &root,
                                        std::function<void(const QString &, bool)> done) {
  vk::Engine3D *e = g::engine3D;
  QString mapPath;
  if (const proj::Document *d = m_docs ? m_docs->bySerial(serial) : nullptr)
    mapPath = d->path;
  eng::godot::LevelOptions lo;
  std::shared_ptr<std::vector<eng::SolidInput>> inputs;
  const bool found = e && withLevelSerial(serial, [&] {
    if (!e->scene())
      return;
    lo.mapName = mapPath.isEmpty() ? QStringLiteral("untitled") : QFileInfo(mapPath).completeBaseName();
    lo.devMaterialPref = prefs::s("Brush/DefaultMaterial");
    lo.entryFor = [](const QString &base) { return net::TextureCatalog::instance().find(base); };
    lo.skyRef = e->skybox();
    lo.lutRef = e->colorLut();
    lo.lutStrip = e->colorLutStripFor(lo.lutRef);
    // Read from this level (the registry caches the one on screen).
    const QVariant collision = e->levelSetting(QStringLiteral("Export/Collision"));
    lo.exportCollision = collision.isValid() ? collision.toBool() : true;
    const QVariant dev = e->levelSetting(QStringLiteral("Export/DevMaterials"));
    lo.exportDevMaterials = dev.isValid() && dev.toBool();
    inputs = std::make_shared<std::vector<eng::SolidInput>>(eng::godot::hiddenFaceInputs(*e->scene()));
  });
  if (!found || !inputs) {
    done(tr("No map"), false);
    return;
  }

  // Hidden faces (the slow part): cached per level while its world brushes stay the same, else computed on a
  // worker; the brushes are compared again when it returns (they may have changed).
  const auto cached = s_godotHidden.value(serial);
  if (!cached || cached->inputKey != eng::godot::hiddenFaceKey(*inputs)) {
    using Result = std::shared_ptr<const eng::godot::HiddenFaces>;
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, serial, root, done] {
      s_godotHidden.insert(serial, watcher->result());
      watcher->deleteLater();
      exportGodotWithEntries(serial, root, done);
    });
    watcher->setFuture(QtConcurrent::run([inputs]() -> Result {
      return std::make_shared<const eng::godot::HiddenFaces>(eng::godot::computeHiddenFaces(*inputs));
    }));
    return;
  }
  const auto hidden = cached;
  QSharedPointer<eng::godot::LevelPlan> plan;
  withLevelSerial(serial, [&] { plan = QSharedPointer<eng::godot::LevelPlan>::create(eng::godot::planLevel(*e->scene(), lo, hidden.get())); });
  if (!plan) {
    done(tr("No map"), false);
    return;
  }

  // Only maps that are missing or changed on the server.
  struct Downloads {
    QJsonObject manifest;
    QVector<int> queue;
    int active = 0, total = 0, finished = 0, fetched = 0;
    QStringList failed;
    bool canceled = false;
    QPointer<QProgressDialog> dialog;
  };
  auto dl = QSharedPointer<Downloads>::create();
  dl->manifest = eng::godot::readTextureManifest(root);
  const bool online = net::AssetClient::instance().isOnline();
  int offlineMissing = 0;
  for (int i = 0; i < int(plan->textures.size()); ++i) {
    const auto &t = plan->textures[size_t(i)];
    if (!t.localSource.isEmpty() || eng::godot::textureUpToDate(root, t, dl->manifest))
      continue;
    if (!online) {
      ++offlineMissing;
      continue;
    }
    dl->queue.append(i);
  }
  dl->total = int(dl->queue.size());

  QPointer<MainWindow> self(this);
  auto finish = [self, serial, mapPath, root, plan, dl, done, offlineMissing] {
    if (!self)
      return;
    if (dl->dialog)
      dl->dialog->deleteLater();
    eng::godot::writeTextureManifest(root, dl->manifest);
    vk::Engine3D *eng3d = g::engine3D;
    eng::godot::WriteOptions wo;
    wo.root = root;
    wo.sourcePath = mapPath;
    wo.writeAddon = prefs::b("Export/WriteAddon");
    // The level may have been closed while the textures came in.
    std::optional<eng::godot::WriteResult> written;
    self->withLevelSerial(serial, [&] {
      if (!eng3d || !eng3d->scene())
        return;
      // The sun (Export/Sun): ours is intensity * colour / PI (Lambert); Godot's energy * colour has its PI cancel the
      // BRDF's, so energy = intensity / PI. Our colour is linear, Godot's light_color is sRGB.
      const QVariant exportSun = eng3d->levelSetting(QStringLiteral("Export/Sun"));
      wo.sun.present = (exportSun.isValid() ? exportSun.toBool() : true) && eng3d->sunIntensity() > 0.0f;
      wo.sun.direction = eng3d->sunDirection();
      const QColor c = eng3d->sunColor(); // channels are the shader's linear values
      const glm::vec3 lin(float(c.redF()), float(c.greenF()), float(c.blueF()));
      auto toSrgb = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
      };
      wo.sun.color = QColor::fromRgbF(toSrgb(lin.r), toSrgb(lin.g), toSrgb(lin.b));
      wo.sun.energy = eng3d->sunIntensity() / 3.14159265f;
      wo.sun.shadows = eng3d->shadowsEnabled();
      written = eng::godot::writeLevel(*eng3d->scene(), *plan, wo);
    });
    if (!written) {
      done(tr("Not exported: the level was closed"), false);
      return;
    }
    const auto &r = *written;
    for (const auto &m: plan->materials)
      qInfo().noquote() << QStringLiteral("[godot] material %1 -> %2 (%3)")
                               .arg(m.ref, m.tresRel, m.isShared ? QStringLiteral("shared") : QStringLiteral("this map"));
    for (const QString &w: plan->warnings + r.warnings)
      qWarning().noquote() << QStringLiteral("[godot] %1").arg(w);
    for (const QString &f: dl->failed)
      qWarning().noquote() << QStringLiteral("[godot] download failed: %1").arg(f);
    for (const QString &err: r.errors)
      qWarning().noquote() << QStringLiteral("[godot] %1").arg(err);
    if (offlineMissing)
      qWarning().noquote() << QStringLiteral("[godot] server offline: %1 texture map(s) not downloaded").arg(offlineMissing);
    if (!r.skyTres.isEmpty())
      qInfo().noquote() << QStringLiteral("[godot] skybox %1 -> res://%2 (PanoramaSkyMaterial): set it as a Sky's "
                                          "sky_material in your WorldEnvironment")
                               .arg(plan->sky->ref, r.skyTres);
    for (const eng::godot::LutJob &l: plan->luts)
      qInfo().noquote() << QStringLiteral("[godot] LUT %1 -> %2 (Texture3D, %3 slices): set it as Environment › "
                                          "Adjustments › Color Correction (Adjustments on)")
                               .arg(l.name, l.rel)
                               .arg(l.strip.height());
    qInfo().noquote() << QStringLiteral("[godot] exported %1: %2 written, %3 unchanged, %4 removed, %5 downloaded; "
                                        "%6 faces, %7 hidden and %8 untextured faces left out")
                             .arg(r.scenePath)
                             .arg(r.written)
                             .arg(r.unchanged)
                             .arg(r.removed)
                             .arg(dl->fetched)
                             .arg(plan->faces)
                             .arg(plan->hiddenFaces)
                             .arg(plan->devFaces);
    if (!r.errors.isEmpty()) {
      done(r.errors.join(QLatin1Char('\n')), false);
      return;
    }
    const int problems = int(dl->failed.size()) + offlineMissing + int(plan->warnings.size() + r.warnings.size());
    done(tr("Exported to %1: %2 written, %3 unchanged, %4 downloaded · %5 hidden, %6 untextured faces removed%7%8")
             .arg(QDir(root).relativeFilePath(r.scenePath))
             .arg(r.written)
             .arg(r.unchanged)
             .arg(dl->fetched)
             .arg(plan->hiddenFaces)
             .arg(plan->devFaces)
             .arg(r.luts ? tr(" · %n LUT(s) in textures/luts (Texture3D)", nullptr, r.luts) : QString())
             .arg(problems ? tr(" · %1 warning(s), see Console").arg(problems) : QString()),
         true);
  };
  if (dl->queue.isEmpty()) {
    finish();
    return;
  }
  if (!qEnvironmentVariableIsSet("EDITOR_SELFTEST")) {
    dl->dialog = new QProgressDialog(tr("Downloading textures for Godot…"), tr("Cancel"), 0, dl->total, this);
    dl->dialog->setWindowModality(Qt::WindowModal);
    dl->dialog->setMinimumDuration(400);
    connect(dl->dialog, &QProgressDialog::canceled, this, [dl] { dl->canceled = true; });
  }
  auto pump = QSharedPointer<std::function<void()>>::create();
  *pump = [self, root, plan, dl, finish, pump] {
    if (!self)
      return;
    while (dl->active < 4 && !dl->queue.isEmpty() && !dl->canceled) {
      const int idx = dl->queue.takeFirst();
      const eng::godot::TextureJob t = plan->textures[size_t(idx)];
      ++dl->active;
      net::AssetClient::instance().httpGet(
          QStringLiteral("/api/2/source/%1/%2/%3").arg(t.pack, t.name, t.mapKey), self,
          [root, t, dl, finish, pump](QNetworkReply *reply) {
            --dl->active;
            ++dl->finished;
            if (reply->error() == QNetworkReply::NoError) {
              QString err;
              if (eng::godot::writeIfChanged(QDir(root).filePath(t.rel), reply->readAll(), &err) >= 0) {
                const QByteArray sum = reply->rawHeader("X-Checksum");
                dl->manifest[t.rel] = sum.isEmpty() ? t.checksum : QString::fromLatin1(sum);
                ++dl->fetched;
              } else {
                dl->failed << err;
              }
            } else {
              dl->failed << QStringLiteral("%1: %2").arg(t.rel, reply->errorString());
            }
            if (dl->dialog)
              dl->dialog->setValue(dl->finished);
            if (dl->active == 0 && (dl->queue.isEmpty() || dl->canceled))
              finish();
            else
              (*pump)();
          });
    }
  };
  (*pump)();
}
