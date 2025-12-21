#include "local_server_page.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/net/asset_client.h"
#include "client/net/local_server.h"
#include "client/prefs/preferences.h"
#include "client/ui/action_manager.h"
#include "client/ui/pref_rows.h"
#include "client/ui/scale.h"

namespace ui {

LocalServerPage::LocalServerPage(QWidget *parent) : QWidget(parent) {
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
  auto *title = new QLabel(tr("Asset Server  ›  Local Server"), inner);
  title->setObjectName(QStringLiteral("prefsTitle"));
  v->addWidget(title);
  m_status = new QLabel(inner);
  m_status->setObjectName(QStringLiteral("prefsSubtitle"));
  m_status->setWordWrap(true);
  m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(m_status);
  v->addSpacing(12);

  auto *card = new QFrame(inner);
  card->setObjectName(QStringLiteral("prefsCard"));
  m_card = card;
  auto *cl = new QVBoxLayout(card);
  cl->setContentsMargins(0, 0, 0, 0);
  cl->setSpacing(0);
  cl->addWidget(prefRow(card, tr("Address"), tr("The local server's URL"),
                    prefValueField(card, m_url, false, true), true));
  cl->addWidget(prefRow(card, tr("Username"), tr("Made up each time gHammer starts; not saved"),
                    prefValueField(card, m_user, false, true), false));
  cl->addWidget(prefRow(card, tr("Password"), tr("Made up each time gHammer starts; not saved"),
                    prefValueField(card, m_password, true, true), false));
  cl->addWidget(prefRow(card, tr("Executable"), tr("ghammer_server next to the editor"),
                    prefValueField(card, m_binary, false, false), false));
  v->addWidget(card);
  v->addSpacing(12);

  auto *buttons = new QHBoxLayout;
  m_restart = new QPushButton(ActionManager::icon(QStringLiteral("file_refresh")), tr("Restart Local Server"), inner);
  m_restart->setObjectName(QStringLiteral("prefsFlat"));
  buttons->addWidget(m_restart);
  buttons->addStretch(1);
  v->addLayout(buttons);
  v->addStretch(1);
  scroll->setWidget(inner);

  connect(m_restart, &QPushButton::clicked, this, [] { net::LocalServer::instance().restart(); });
  connect(&net::LocalServer::instance(), &net::LocalServer::stateChanged, this, &LocalServerPage::refresh);
  connect(&net::LocalServer::instance(), &net::LocalServer::ready, this, &LocalServerPage::refresh);
  connect(&net::AssetClient::instance(), &net::AssetClient::endpointChanged, this, &LocalServerPage::refresh);
  prefs::get().watch(QStringLiteral("Server/Mode"), this, [this](const QVariant &) { refresh(); });
}

void LocalServerPage::refresh() {
  const auto &s = net::LocalServer::instance();
  const bool remote = prefs::s("Server/Mode") == QLatin1String("remote");
  const bool overridden = !qEnvironmentVariable("EDITOR_SERVER_URL").isEmpty();

  QString status;
  if (overridden)
    status = tr("Not used · EDITOR_SERVER_URL");
  else if (remote)
    status = tr("Not used · remote server");
  else if (!s.allowed())
    status = s.lastError();
  else {
    switch (s.state()) {
      case net::LocalServer::State::Running: status = tr("Running · this computer only"); break;
      case net::LocalServer::State::Starting:
        status = s.lastError().isEmpty() ? tr("Starting…") : s.lastError();
        break;
      case net::LocalServer::State::Failed: status = s.lastError(); break;
      case net::LocalServer::State::Stopped:
        status = s.lastError().isEmpty() ? tr("Stopped") : s.lastError();
        break;
    }
  }
  m_status->setText(status);
  m_url->setText(s.isActive() ? s.url().toString() : QString());
  m_user->setText(s.user());
  m_password->setText(s.password());
  m_binary->setText(QDir::toNativeSeparators(net::LocalServer::binaryPath()));
  m_binary->setToolTip(m_binary->text());
  const bool used = !remote && !overridden && s.allowed();
  m_card->setEnabled(used);
  m_restart->setEnabled(used);
}

} // namespace ui
