#include "paths_page.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "client/prefs/preferences.h"
#include "client/project/project.h"
#include "client/project/project_manager.h"
#include "client/ui/action_manager.h"
#include "client/ui/file_dialog.h"
#include "client/ui/pref_rows.h"
#include "shared/lib/config.h"
#include "shared/lib/globals.h"

namespace ui {

namespace {

const QString kAutosaveKey = QStringLiteral("Paths/Autosave");

QToolButton *toolButton(QWidget *parent, const char *icon, const QString &tip) {
  auto *b = new QToolButton(parent);
  b->setObjectName(QStringLiteral("prefsReset"));
  b->setIcon(ActionManager::icon(QString::fromLatin1(icon)));
  b->setToolTip(tip);
  return b;
}

// A read-only path with Copy and Open (a file opens its folder).
QWidget *pathField(QWidget *parent, QLineEdit *&line) {
  QWidget *field = prefValueField(parent, line, false, true);
  QToolButton *open = toolButton(field, "file_folder", QObject::tr("Open in the file manager"));
  QLineEdit *l = line;
  QObject::connect(open, &QToolButton::clicked, open, [l] {
    const QFileInfo fi(l->text());
    if (!l->text().isEmpty())
      QDesktopServices::openUrl(QUrl::fromLocalFile(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath()));
  });
  prefFieldButtons(field)->addWidget(open);
  return field;
}

void showPath(QLineEdit *line, const QString &path) {
  const QString text = path.isEmpty() ? QString() : QDir::toNativeSeparators(QDir::cleanPath(path));
  line->setText(text);
  line->setToolTip(text);
  line->setCursorPosition(0);
}

} // namespace

QString autosaveFolder() {
  const QString dir = prefs::get().stringValue(kAutosaveKey);
  return dir.isEmpty() ? QDir(gs::configDirectory).absoluteFilePath(QStringLiteral("autosave")) : dir;
}

PathsPage::PathsPage(QWidget *parent) : QWidget(parent) {
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  auto *scroll = new QScrollArea(this);
  scroll->setObjectName(QStringLiteral("prefsScroll"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  outer->addWidget(scroll);

  auto *inner = new QWidget(scroll);
  inner->setObjectName(QStringLiteral("prefsPage"));
  auto *v = new QVBoxLayout(inner);
  v->setContentsMargins(28, 22, 28, 22);
  v->setSpacing(4);
  auto *title = new QLabel(tr("Paths"), inner);
  title->setObjectName(QStringLiteral("prefsTitle"));
  v->addWidget(title);
  v->addSpacing(12);

  auto *card = new QFrame(inner);
  card->setObjectName(QStringLiteral("prefsCard"));
  auto *cl = new QVBoxLayout(card);
  cl->setContentsMargins(0, 0, 0, 0);
  cl->setSpacing(0);
  cl->addWidget(prefRow(card, tr("Settings"), tr("gHammer's settings file"), pathField(card, m_settings), true));
  cl->addWidget(prefRow(card, tr("Cache"), tr("Downloaded textures, thumbnails and skies; safe to delete"),
                        pathField(card, m_cache), false));

  QWidget *autosave = pathField(card, m_autosave);
  QToolButton *choose = toolButton(autosave, "file_folder_large", tr("Choose another folder"));
  m_autosaveReset = toolButton(autosave, "loop_back", tr("Back to the default folder"));
  prefFieldButtons(autosave)->addWidget(choose);
  prefFieldButtons(autosave)->addWidget(m_autosaveReset);
  connect(choose, &QToolButton::clicked, this, [this] {
    const QString dir = FileDialog::getExistingDirectory(this, tr("Autosave Folder"), autosaveFolder());
    if (!dir.isEmpty())
      prefs::get().setValue(kAutosaveKey, dir);
  });
  connect(m_autosaveReset, &QToolButton::clicked, this, [] { prefs::get().reset(kAutosaveKey); });
  cl->addWidget(prefRow(card, tr("Autosaves"), tr("Backups of unsaved levels"), autosave, false));

  cl->addWidget(prefRow(card, tr("Local asset server packs"),
                        tr("Texture, skybox and LUT packs of the local asset server"), pathField(card, m_serverData),
                        false));
  cl->addWidget(prefRow(card, tr("New projects"), tr("Where new projects are created"), pathField(card, m_projects),
                        false));
  cl->addWidget(prefRow(card, tr("Open project"), tr("The open project's folder"), pathField(card, m_project), false));
  cl->addWidget(prefRow(card, tr("Project settings"), tr("The open project's settings file"),
                        pathField(card, m_projectSettings), false));
  v->addWidget(card);
  v->addStretch(1);
  scroll->setWidget(inner);

  prefs::get().watch(kAutosaveKey, this, [this](const QVariant &) { refresh(); });
  connect(&proj::ProjectManager::instance(), &proj::ProjectManager::projectChanged, this, &PathsPage::refresh);
  refresh();
}

void PathsPage::refresh() {
  showPath(m_settings, config()->getFileName());
  showPath(m_cache, gs::cacheDirectory);
  showPath(m_autosave, autosaveFolder());
  m_autosaveReset->setVisible(!prefs::get().isDefault(kAutosaveKey));
  showPath(m_serverData, gs::configDirectoryAssetsTextures);
  showPath(m_projects, proj::Project::standardLocation());
  const proj::Project *p = proj::ProjectManager::instance().current();
  showPath(m_project, p ? p->root() : QString());
  showPath(m_projectSettings, p ? QDir(p->root()).absoluteFilePath(QStringLiteral(".ghammer/settings.json")) : QString());
  m_project->setPlaceholderText(tr("No project open"));
  m_projectSettings->setPlaceholderText(tr("No project open"));
}

} // namespace ui
