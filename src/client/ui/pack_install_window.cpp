#include "pack_install_window.h"

#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "client/net/asset_client.h"
#include "client/net/pack_installer.h"
#include "client/ui/dialog_style.h"
#include "client/ui/file_dialog.h"
#include "client/ui/scale.h"

namespace ui {

namespace {

// Published packs (GitHub releases): zips with the pack JSON at the root.
struct AvailablePack {
  QString name;
  QString description;
  QString url;
  QString author;
};

const QList<AvailablePack> &availablePacks() {
  static const QList<AvailablePack> packs = {
      {QStringLiteral("sharetextures_1k"), QStringLiteral("CC0 textures from ShareTextures"),
       QStringLiteral("https://github.com/kroketio/ghammer/releases/download/0.0.1/sharetextures_1k.zip"), QStringLiteral("kroketio")},
      {QStringLiteral("blenderkit_1k"), QStringLiteral("CC0 textures from BlenderKit"),
       QStringLiteral("https://github.com/kroketio/ghammer/releases/download/0.0.1/blenderkit_1k.zip"), QStringLiteral("kroketio")},
      {QStringLiteral("cgambient_1k"), QStringLiteral("CC0 textures from ambientCG"),
       QStringLiteral("https://github.com/kroketio/ghammer/releases/download/0.0.1/cgambient_1k.zip"), QStringLiteral("kroketio")},
      {QStringLiteral("cgbookcase_1k"), QStringLiteral("CC0 textures from cgbookcase"),
       QStringLiteral("https://github.com/kroketio/ghammer/releases/download/0.0.1/cgbookcase_1k.zip"), QStringLiteral("kroketio")},
      {QStringLiteral("polyhaven_1k"), QStringLiteral("CC0 textures from Poly Haven"),
       QStringLiteral("https://github.com/kroketio/ghammer/releases/download/0.0.1/polyhaven_1k.zip"), QStringLiteral("kroketio")},
      {QStringLiteral("sourceskyboxes"), QStringLiteral("CC BY 4.0 skyboxes from sourceskyboxes.com"),
       QStringLiteral("https://github.com/kroketio/ghammer/releases/download/0.0.1/sourceskyboxes.zip"), QStringLiteral("kroketio")},
      {QStringLiteral("ghammer_luts"), QStringLiteral("Colour LUTs"),
       QStringLiteral("https://github.com/kroketio/ghammer/releases/download/0.0.1/ghammer_luts.zip"), QStringLiteral("kroketio")},
  };
  return packs;
}

QFrame *makeCard(QWidget *parent) {
  auto *card = new QFrame(parent);
  card->setObjectName(QStringLiteral("dlgCard"));
  return card;
}

QLabel *makeLabel(const QString &text, const char *objectName, QWidget *parent) {
  auto *l = new QLabel(text, parent);
  l->setObjectName(QString::fromLatin1(objectName));
  return l;
}

QPushButton *makeButton(const QString &text, const char *objectName, QWidget *parent) {
  auto *b = new QPushButton(text, parent);
  b->setObjectName(QString::fromLatin1(objectName));
  return b;
}

QFrame *makeRow(QWidget *parent, bool first) {
  auto *row = new QFrame(parent);
  row->setObjectName(QStringLiteral("dlgRow"));
  row->setProperty("first", first);
  return row;
}

QString sizeText(qint64 bytes) {
  return QLocale().formattedDataSize(bytes, 1);
}

} // namespace

QPointer<PackInstallWindow> PackInstallWindow::s_window;

PackInstallWindow *PackInstallWindow::open(QWidget *parent, bool firstRun) {
  if (!s_window)
    s_window = new PackInstallWindow(parent);
  s_window->m_intro->setVisible(firstRun);
  s_window->show();
  s_window->raise();
  s_window->activateWindow();
  return s_window;
}

PackInstallWindow::PackInstallWindow(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Install Asset Packs"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  scale::setScaledStyleSheet(this, ui::dialogStyle());
  resize(scale::initialSize(this, {640, 480}));
  m_job = new net::PackInstaller(this);

  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  auto *page = new QWidget(this);
  page->setObjectName(QStringLiteral("dlgPage"));
  auto *root = new QVBoxLayout(page);
  root->setContentsMargins(20, 18, 20, 18);
  root->setSpacing(8);
  outer->addWidget(page, 1);

  m_intro = new QLabel(tr("gHammer comes without textures, skyboxes or colour LUTs. Install a few packs to get "
                          "started; this window is under File › Asset Server."),
                       page);
  m_intro->setWordWrap(true);
  m_intro->hide();
  root->addWidget(m_intro);
  m_server = makeLabel(QString(), "dlgSubtitle", page);
  m_server->setTextInteractionFlags(Qt::TextSelectableByMouse);
  root->addWidget(m_server);
  root->addSpacing(6);

  // Published packs.
  root->addWidget(makeLabel(tr("AVAILABLE PACKS"), "dlgSection", page));
  auto *listCard = makeCard(page);
  auto *ll = new QVBoxLayout(listCard);
  ll->setContentsMargins(0, 0, 0, 0);
  ll->setSpacing(0);
  if (availablePacks().isEmpty()) {
    auto *row = makeRow(listCard, true);
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(14, 12, 14, 12);
    rl->addWidget(makeLabel(tr("No packs listed"), "dlgDesc", row));
    ll->addWidget(row);
  }
  for (const AvailablePack &p: availablePacks()) {
    auto *row = makeRow(listCard, ll->count() == 0);
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(14, 10, 14, 10);
    rl->setSpacing(12);
    auto *text = new QVBoxLayout;
    text->setSpacing(2);
    auto *name = new QLabel(p.name, row);
    QFont f = name->font();
    f.setBold(true);
    name->setFont(f);
    text->addWidget(name);
    auto *desc = makeLabel(p.author.isEmpty() ? p.description : tr("%1 · by %2").arg(p.description, p.author), "dlgDesc",
                           row);
    desc->setWordWrap(true);
    text->addWidget(desc);
    rl->addLayout(text, 1);
    auto *install = makeButton(tr("Install"), "dlgFlat", row);
    install->setToolTip(tr("Download %1 and install it on the asset server").arg(p.name));
    connect(install, &QPushButton::clicked, this, [this, p] { startUrl(QUrl(p.url), p.name); });
    rl->addWidget(install, 0, Qt::AlignVCenter);
    m_installButtons.append(install);
    ll->addWidget(row);
  }
  root->addWidget(listCard);
  root->addSpacing(6);

  // Any other pack: a web address or a zip on disk.
  root->addWidget(makeLabel(tr("OTHER PACKS"), "dlgSection", page));
  auto *otherCard = makeCard(page);
  auto *ol = new QVBoxLayout(otherCard);
  ol->setContentsMargins(0, 0, 0, 0);
  ol->setSpacing(0);
  {
    auto *row = makeRow(otherCard, true);
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(14, 10, 14, 10);
    rl->setSpacing(8);
    m_url = new QLineEdit(row);
    m_url->setPlaceholderText(tr("https://…/pack.zip"));
    m_url->setToolTip(tr("Web address of an asset pack (.zip)"));
    rl->addWidget(m_url, 1);
    m_urlInstall = makeButton(tr("Install from URL"), "dlgFlat", row);
    m_urlInstall->setToolTip(tr("Download the pack and install it on the asset server"));
    rl->addWidget(m_urlInstall);
    m_file = makeButton(tr("Import a File…"), "dlgFlat", row);
    m_file->setToolTip(tr("Install an asset pack (.zip) from this computer"));
    connect(m_file, &QPushButton::clicked, this, &PackInstallWindow::chooseFile);
    rl->addWidget(m_file);
    connect(m_url, &QLineEdit::textChanged, this, &PackInstallWindow::refreshButtons);
    connect(m_url, &QLineEdit::returnPressed, m_urlInstall, &QPushButton::click);
    connect(m_urlInstall, &QPushButton::clicked, this, [this] {
      const QUrl url = QUrl::fromUserInput(m_url->text().trimmed());
      if (url.isValid())
        startUrl(url, QFileInfo(url.path()).fileName());
    });
    ol->addWidget(row);
  }
  root->addWidget(otherCard);
  root->addSpacing(6);

  // The install in progress.
  m_progressCard = makeCard(page);
  auto *pl = new QVBoxLayout(m_progressCard);
  pl->setContentsMargins(14, 12, 14, 12);
  pl->setSpacing(6);
  auto *head = new QHBoxLayout;
  m_what = new QLabel(m_progressCard);
  QFont wf = m_what->font();
  wf.setBold(true);
  m_what->setFont(wf);
  head->addWidget(m_what, 1);
  m_phase = makeLabel(QString(), "dlgDesc", m_progressCard);
  head->addWidget(m_phase);
  pl->addLayout(head);
  m_bar = new QProgressBar(m_progressCard);
  m_bar->setTextVisible(false);
  pl->addWidget(m_bar);
  auto *foot = new QHBoxLayout;
  m_detail = makeLabel(QString(), "dlgDesc", m_progressCard);
  foot->addWidget(m_detail, 1);
  m_cancel = makeButton(tr("Cancel"), "dlgFlat", m_progressCard);
  m_cancel->setToolTip(tr("Stop this install"));
  connect(m_cancel, &QPushButton::clicked, m_job, &net::PackInstaller::cancel);
  foot->addWidget(m_cancel);
  pl->addLayout(foot);
  m_notice = makeLabel(QString(), "dlgDesc", m_progressCard);
  m_notice->setWordWrap(true);
  m_notice->hide();
  pl->addWidget(m_notice);
  m_progressCard->hide();
  root->addWidget(m_progressCard);

  m_result = new QLabel(page);
  m_result->setWordWrap(true);
  m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_result->hide();
  root->addWidget(m_result);
  root->addStretch(1);

  auto *footer = new QFrame(this);
  footer->setObjectName(QStringLiteral("dlgFooter"));
  auto *fl = new QHBoxLayout(footer);
  fl->setContentsMargins(16, 10, 16, 10);
  fl->addStretch(1);
  auto *close = makeButton(tr("Close"), "dlgPrimary", footer);
  close->setToolTip(tr("Close; an install in progress carries on"));
  connect(close, &QPushButton::clicked, this, &QDialog::close);
  fl->addWidget(close);
  outer->addWidget(footer);

  connect(m_job, &net::PackInstaller::phaseChanged, this, &PackInstallWindow::showPhase);
  connect(m_job, &net::PackInstaller::progress, this, &PackInstallWindow::onProgress);
  connect(m_job, &net::PackInstaller::notice, this, [this](const QString &text) {
    m_notice->setText(text);
    m_notice->show();
  });
  connect(m_job, &net::PackInstaller::finished, this, &PackInstallWindow::onFinished);
  connect(&net::AssetClient::instance(), &net::AssetClient::stateChanged, this, &PackInstallWindow::refreshServer);
  refreshServer();
}

void PackInstallWindow::refreshServer() {
  const net::AssetClient &c = net::AssetClient::instance();
  if (c.isOnline())
    m_server->setText(tr("Installs on the %1 asset server · %2")
                          .arg(c.isLocal() ? tr("local") : tr("remote"), c.serverUrlText()));
  else
    m_server->setText(tr("Asset server offline"));
  refreshButtons();
}

void PackInstallWindow::refreshButtons() {
  const bool can = net::AssetClient::instance().isOnline() && !m_job->busy();
  for (QPushButton *b: std::as_const(m_installButtons))
    b->setEnabled(can);
  m_urlInstall->setEnabled(can && QUrl::fromUserInput(m_url->text().trimmed()).isValid() &&
                           !m_url->text().trimmed().isEmpty());
  m_file->setEnabled(can);
}

void PackInstallWindow::startUrl(const QUrl &url, const QString &label) {
  if (m_job->busy())
    return;
  m_what->setText(label.isEmpty() ? url.toString() : label);
  m_job->installUrl(url);
}

void PackInstallWindow::installFile(const QString &path) {
  if (m_job->busy())
    return;
  m_what->setText(QFileInfo(path).fileName());
  m_job->installFile(path);
}

void PackInstallWindow::installUrl(const QUrl &url) {
  startUrl(url, QFileInfo(url.path()).fileName());
}

void PackInstallWindow::chooseFile() {
  if (m_job->busy())
    return;
  const QString path = FileDialog::getOpenFileName(this, tr("Install Asset Pack"), {},
                                                   tr("Asset packs (*.zip);;All files (*)"), tr("Install"));
  if (!path.isEmpty())
    installFile(path);
}

void PackInstallWindow::showPhase() {
  using P = net::PackInstaller::Phase;
  const P p = m_job->phase();
  const bool busy = m_job->busy();
  m_progressCard->setVisible(busy);
  if (busy) {
    m_result->hide();
    m_notice->hide();
  }
  switch (p) {
    case P::Downloading: m_phase->setText(tr("Downloading")); break;
    case P::Checking:
      m_phase->setText(tr("Checking"));
      m_bar->setRange(0, 0);
      m_detail->clear();
      break;
    case P::Uploading:
      m_phase->setText(tr("Uploading to the asset server"));
      m_what->setText(tr("%1 · %2").arg(m_job->pack().name, packTypeLabel(m_job->pack().type)));
      m_notice->hide();
      break;
    case P::Installing:
      m_phase->setText(tr("Installing"));
      m_bar->setRange(0, 0);
      m_detail->setText((m_job->pack().assets == 1 ? tr("1 asset") : tr("%1 assets").arg(m_job->pack().assets)) +
                        QStringLiteral(" · ") + sizeText(qint64(m_job->pack().bytes)));
      break;
    default: break;
  }
  refreshButtons();
}

void PackInstallWindow::onProgress(qint64 done, qint64 total) {
  using P = net::PackInstaller::Phase;
  if (m_job->phase() != P::Downloading && m_job->phase() != P::Uploading)
    return;
  if (total > 0) {
    m_bar->setRange(0, 1000);
    m_bar->setValue(int(std::min<qint64>(1000, done * 1000 / total)));
  } else {
    m_bar->setRange(0, 0);
  }
  QString text = total > 0 ? tr("%1 of %2").arg(sizeText(done), sizeText(total)) : sizeText(done);
  if (m_job->rate() > 0)
    text += tr(" · %1/s").arg(sizeText(qint64(m_job->rate())));
  m_detail->setText(text);
  if (done > 0 && m_job->phase() == P::Downloading)
    m_notice->hide();
}

void PackInstallWindow::onFinished(bool ok, const QString &message) {
  m_progressCard->hide();
  m_result->setStyleSheet(ok ? QString() : QStringLiteral("color: #e58a8a; background: transparent;"));
  m_result->setText(message);
  m_result->show();
  if (ok)
    m_url->clear();
  refreshButtons();
}

} // namespace ui
