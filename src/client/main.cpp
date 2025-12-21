#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QPixmap>
#include <QPixmapCache>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtPlugin>

#if defined(Q_OS_LINUX) && defined(STATIC)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin)
#endif

#include "client/lib/globals_colors.h"
#include "client/lib/icon_dir.h"
#include "client/ctx.h"
#include "client/mainwindow.h"
#include "ghammer_version.h"
#include "client/project/project.h"
#include "client/project/project_manager.h"
#include "client/net/local_server.h"
#include "client/prefs/preferences.h"
#include "client/prefs/preferences_dialog.h"
#include "client/ui/local_server_page.h"
#include "client/ui/paths_page.h"
#include "client/ui/project_dialog.h"
#include "client/ui/console_panel.h"
#include "client/ui/scale.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_gridview.h"
#include "client/vulkan/vk_viewport3d.h"

int main(int argc, char **argv) {
  // First: everything logged from here on also reaches the console panel.
  ui::LogSink::install();
  // In the static `shared` library: kept only when its init function is referenced.
  Q_INIT_RESOURCE(shared_assets);
  icons::installIconDir();
  QPixmapCache::setCacheLimit(2000 * 1024);

  QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);

  QApplication::setApplicationName("ghammer");
  QApplication::setApplicationDisplayName(QStringLiteral("gHammer"));
  QApplication::setApplicationVersion(QStringLiteral(GHAMMER_VERSION));
  QApplication::setOrganizationDomain("kroket.io");
  QApplication::setOrganizationName("Kroket Ltd.");
  QGuiApplication::setDesktopFileName(QStringLiteral("io.kroket.ghammer"));
  QApplication app(argc, argv);
  ui::installWheelGuard(&app);

  // The source icon is 1024 px; X11 copies every size into each window, so give it a few downscaled ones.
  {
    const QPixmap src(QStringLiteral(":/assets/icon.png"));
    QIcon icon;
    for (const int px: {16, 24, 32, 48, 64, 128, 256})
      icon.addPixmap(src.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    QApplication::setWindowIcon(icon);
  }

  qmlRegisterType<vk::Viewport3D>("RhiView", 1, 0, "Viewport3D");
  qmlRegisterType<vk::GridView>("RhiView", 1, 0, "GridView");

  // EDITOR_QSS: a stylesheet file on disk for live theme work.
  const QString path_css = qEnvironmentVariableIsSet("EDITOR_QSS")
                               ? qEnvironmentVariable("EDITOR_QSS")
                               : QStringLiteral(":/theme/DarkBlue.qss");
  QString body;
  QFile theme_file(path_css);
  theme_file.open(QFile::ReadOnly);
  if (theme_file.isOpen()) {
    body = QString(theme_file.readAll());
    body = colors::injectStyleSheetColors(body);
    qDebug().noquote() << QStringLiteral("[theme] %1 chars from %2").arg(body.size()).arg(path_css);
  } else {
    qWarning().noquote() << QStringLiteral("[theme] could not open %1, widgets are unstyled").arg(path_css);
  }
  ui::scale::install(&app, body);

  // Owned here so teardown is ordered (window first, while the QRhi and QML engines still exist).
  auto ctx = std::make_unique<Ctx>();

  // Local mode starts ghammer_server now so its catalogue loads while the splash is up. Scripted runs never start one.
  prefs::PreferencesDialog::registerPage(QString::fromLatin1(ui::PathsPage::kCategory),
                                         [](QWidget *parent) { return new ui::PathsPage(parent); });
  prefs::PreferencesDialog::registerPage(QString::fromLatin1(ui::LocalServerPage::kCategory),
                                         [](QWidget *parent) { return new ui::LocalServerPage(parent); });
  net::LocalServer::instance().setAllowed(!MainWindow::scriptedRun());
  if (qEnvironmentVariableIsEmpty("EDITOR_SERVER_URL") && prefs::s("Server/Mode") != QLatin1String("remote"))
    net::LocalServer::instance().start();

  // Map argument / scripted run: no splash, open the map's project if any. Else EDITOR_PROJECT, the last project,
  // or the chooser (closing it quits). Test runs and EDITOR_PROJECT leave the recent projects alone.
  if (MainWindow::scriptedRun() || qEnvironmentVariableIsSet("EDITOR_PROJECT"))
    proj::ProjectManager::instance().setRemember(false);
  QString project = qEnvironmentVariable("EDITOR_PROJECT");
  if (const QString create = qEnvironmentVariable("EDITOR_CREATE_PROJECT"); !create.isEmpty()) {
    QString error;
    project = proj::ProjectManager::createProject(QFileInfo(create).absolutePath(), QFileInfo(create).fileName(), &error);
    if (project.isEmpty()) {
      qCritical().noquote() << QStringLiteral("[project] %1").arg(error);
      return 1;
    }
  }
  if (project.isEmpty()) {
    const QString map = MainWindow::commandLineMap();
    if (!map.isEmpty() || MainWindow::scriptedRun()) {
      project = proj::Project::findEnclosing(map);
    } else {
      project = proj::ProjectManager::instance().lastProject();
      if (project.isEmpty()) {
        project = ui::ProjectDialog::run(nullptr, ui::ProjectDialog::Mode::Startup);
        if (project.isEmpty())
          return 0;
      }
    }
  }

  auto mainWindow = std::make_unique<MainWindow>(ctx.get(), project);

  const int rc = QApplication::exec();
  mainWindow.reset();
  ctx.reset();
  return rc;
}
