#include "server_status.h"

#include <QApplication>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QPainter>
#include <QPixmap>

#include "client/net/asset_client.h"
#include "client/net/local_server.h"
#include "client/prefs/preferences.h"
#include "client/prefs/preferences_dialog.h"
#include "client/ui/action_manager.h"

namespace ui {

using net::AssetClient;

ServerStatus::ServerStatus(QWidget *parent) : QToolButton(parent) {
  setAutoRaise(true);
  setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  setPopupMode(QToolButton::InstantPopup);
  setCursor(Qt::PointingHandCursor);
  setStyleSheet(QStringLiteral("QToolButton { color:#aab4cf; padding: 0 8px; border: none; }"
                               "QToolButton::menu-indicator { image: none; width: 0; }"
                               "QToolButton:hover { color:#dfe6f7; }"));

  auto *menu = new QMenu(this);
  for (const char *id: {"server.local", "server.remote", "-", "server.reconnect", "server.restartLocal", "-",
                        "server.installPacks", "server.settings"}) {
    if (qstrcmp(id, "-") == 0)
      menu->addSeparator();
    else if (QAction *a = act(QString::fromLatin1(id)))
      menu->addAction(a);
  }
  menu->addAction(tr("Copy Server Address"), this,
                  [] { QApplication::clipboard()->setText(AssetClient::instance().serverUrlText()); });
  connect(menu, &QMenu::aboutToShow, this, [] { ActionManager::instance().refresh(); });
  setMenu(menu);

  auto &c = AssetClient::instance();
  connect(&c, &AssetClient::endpointChanged, this, &ServerStatus::refresh);
  connect(&net::LocalServer::instance(), &net::LocalServer::stateChanged, this, &ServerStatus::refresh);
  prefs::get().watch(QStringLiteral("Server/Mode"), this, [this](const QVariant &) { refresh(); });
  connect(&c, &AssetClient::stateChanged, this, &ServerStatus::refresh);
  connect(&c, &AssetClient::presenceChanged, this, &ServerStatus::refresh);
  connect(&c, &AssetClient::statusChanged, this, &ServerStatus::refresh);
  connect(&c, &AssetClient::serverEvent, this, [this](const QString &ev, const QJsonObject &) {
    if (ev == QLatin1String("catalog.ready"))
      refresh();
  });
  refresh();
}

QIcon ServerStatus::dot(const QColor &c) {
  QPixmap pm(14, 14);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  QColor halo = c;
  halo.setAlpha(70);
  p.setPen(Qt::NoPen);
  p.setBrush(halo);
  p.drawEllipse(QRectF(1, 1, 12, 12));
  p.setBrush(c);
  p.drawEllipse(QRectF(3.5, 3.5, 7, 7));
  return QIcon(pm);
}

void ServerStatus::refresh() {
  const auto &c = AssetClient::instance();
  const QJsonArray clients = c.clients();
  const QJsonObject st = c.status();
  QString text;
  QColor color;
  switch (c.state()) {
    case AssetClient::State::Online:
      color = QColor(0x4c, 0xc3, 0x7a);
      text = clients.size() > 1 ? tr("Asset server · %1 online").arg(clients.size()) : tr("Asset server");
      break;
    case AssetClient::State::Loading:
      color = QColor(0xe0, 0xb0, 0x40);
      text = tr("Asset server · loading catalogue…");
      break;
    case AssetClient::State::Connecting:
      color = QColor(0xe0, 0xb0, 0x40);
      text = tr("Asset server · connecting…");
      break;
    case AssetClient::State::Offline:
      color = QColor(0xd8, 0x55, 0x55);
      text = tr("Asset server · offline");
      break;
  }
  const bool overridden = !qEnvironmentVariable("EDITOR_SERVER_URL").isEmpty();
  const QString where = overridden ? QString() : c.isLocal() ? tr("local") : tr("remote");
  if (!where.isEmpty())
    text += QStringLiteral(" · %1").arg(where);
  setIcon(dot(color));
  setText(text);

  QStringList tip;
  tip << QStringLiteral("<b>%1</b>").arg(c.serverUrlText().isEmpty() ? tr("No server").toHtmlEscaped()
                                                                      : c.serverUrlText().toHtmlEscaped());
  if (!where.isEmpty())
    tip << (c.isLocal() ? tr("Local server · this computer only") : tr("Remote server"));
  if (c.isLocal() && net::LocalServer::instance().state() == net::LocalServer::State::Failed)
    tip << tr("Local server: %1").arg(net::LocalServer::instance().lastError().toHtmlEscaped());
  if (c.state() == AssetClient::State::Offline && !c.lastError().isEmpty())
    tip << tr("Last error: %1").arg(c.lastError().toHtmlEscaped());
  if (c.state() == AssetClient::State::Online || c.state() == AssetClient::State::Loading) {
    tip << tr("%1 textures in %2 packs")
               .arg(st.value(QStringLiteral("textures")).toInt())
               .arg(st.value(QStringLiteral("packs")).toInt());
    const QJsonObject d = st.value(QStringLiteral("derivatives")).toObject();
    const qint64 queued = qint64(d.value(QStringLiteral("queued")).toDouble());
    const qint64 done = qint64(d.value(QStringLiteral("done")).toDouble());
    if (queued > done)
      tip << tr("Server is generating editor textures: %1 / %2").arg(done).arg(queued);
    QStringList names;
    for (const auto &v: clients) {
      const QJsonObject o = v.toObject();
      QString n = o.value(QStringLiteral("name")).toString().toHtmlEscaped();
      if (o.value(QStringLiteral("client")).toInt() == c.clientId())
        n += tr(" (you)");
      names << QStringLiteral("&nbsp;• %1").arg(n);
    }
    if (!names.isEmpty())
      tip << tr("Connected editors:") << names;
  }
  setToolTip(tip.join(QStringLiteral("<br>")));
}

} // namespace ui
