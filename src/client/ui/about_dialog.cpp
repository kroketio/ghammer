#include "about_dialog.h"

#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "client/ui/dialog_style.h"
#include "client/ui/scale.h"
#include "ghammer_version.h"

namespace ui {

namespace {

struct Component {
  const char *name;
  const char *usedFor;
  const char *copyright;
  const char *license;
  const char *url;
  const char *note;
  QStringList files;
};

const QVector<Component> &components() {
  static const QVector<Component> list = {
      {"Qt 6", "Application framework: widgets, Qt Quick, networking, SQL, SVG",
       "Copyright (C) The Qt Company Ltd. and other contributors", "LGPL-3.0-only", "https://www.qt.io",
       "",
       {QStringLiteral("qt-lgpl-3.0.txt"), QStringLiteral("qt-gpl-3.0.txt")}},
      {"Assimp", "Model import", "Copyright (c) 2006-2026, assimp team", "BSD-3-Clause",
       "https://github.com/assimp/assimp", "", {QStringLiteral("assimp.txt")}},
      {"Box3D", "Physics simulation and collision hulls", "Copyright (c) 2026 Erin Catto", "MIT",
       "https://github.com/erincatto/box3d", "", {QStringLiteral("box3d.txt")}},
      {"QCoro", "Coroutines for Qt", "Copyright (c) 2022 Daniel Vrátil", "MIT", "https://github.com/qcoro/qcoro", "",
       {QStringLiteral("qcoro.txt")}},
      {"GLM", "Mathematics", "Copyright (c) 2005-2015 G-Truc Creation", "MIT", "https://github.com/g-truc/glm", "",
       {QStringLiteral("glm.txt")}},
      {"tinyexr", "OpenEXR images (skyboxes)", "Copyright (c) 2014-2021, Syoyo Fujita and many contributors",
       "BSD-3-Clause", "https://github.com/syoyo/tinyexr", "", {QStringLiteral("tinyexr.txt")}},
      {"stb", "Image loading, writing and resizing", "Copyright (c) 2017 Sean Barrett", "MIT or Public Domain",
       "https://github.com/nothings/stb", "", {QStringLiteral("stb.txt")}},
      {"libpng", "PNG images", "Copyright (c) the libpng authors (see the license text)", "libpng License",
       "http://www.libpng.org/pub/png/libpng.html", "", {QStringLiteral("libpng.txt")}},
      {"zlib", "Compression", "Copyright (C) Jean-loup Gailly and Mark Adler", "zlib License", "https://zlib.net", "",
       {QStringLiteral("zlib.txt")}},
      {"SQLite", "The asset server's database (through Qt SQL)", "Public domain", "Public Domain",
       "https://sqlite.org", "", {QStringLiteral("sqlite.txt")}},
      {"RapidJSON", "JSON (asset server)", "Copyright (C) THL A29 Limited, a Tencent company, and Milo Yip", "MIT",
       "https://rapidjson.org", "", {QStringLiteral("rapidjson.txt")}},
      {"sourcepp", "Valve map (VMF) import", "Copyright (c) the sourcepp contributors", "MIT",
       "https://github.com/craftablescience/sourcepp", "", {QStringLiteral("sourcepp.txt")}},
      {"nameof", "Names of enums and types", "Copyright (c) 2016-2024 Daniil Goncharov", "MIT",
       "https://github.com/Neargye/nameof", "", {QStringLiteral("nameof.txt")}},
      {"HammerPro", "Bevel mode geometry (ported)", "Copyright (c) 2026 HammerPro contributors", "MIT", "", "",
       {QStringLiteral("hammerpro.txt")}},
      {"Godot editor icons", "Interface icons",
       "Copyright (c) 2014-present Godot Engine contributors; Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur",
       "MIT", "https://godotengine.org", "", {QStringLiteral("godot.txt")}},
      {"Blender icons", "Interface icons", "Blender Foundation and the Blender icon designers", "CC BY-SA 4.0",
       "https://www.blender.org", "", {QStringLiteral("blender-icons.txt")}},
      {"gHammer logo", "Application icon", "u/toadile on Reddit", "", "https://www.reddit.com/user/toadile", "",
       {QStringLiteral("logo.txt")}},
  };
  return list;
}

QString readLicense(const QString &file) {
  QFile f(QStringLiteral(":/licenses/") + file);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  return QString::fromUtf8(f.readAll());
}

QString row(const QString &label, const QString &value) {
  if (value.isEmpty())
    return {};
  return QStringLiteral("<tr><td style='color:#8f9bbd; padding-right:14px;'>%1</td><td>%2</td></tr>").arg(label, value);
}

} // namespace

void AboutDialog::showDialog(QWidget *parent) {
  static QPointer<AboutDialog> s_dialog;
  if (!s_dialog) {
    s_dialog = new AboutDialog(parent);
    s_dialog->setAttribute(Qt::WA_DeleteOnClose);
  }
  s_dialog->show();
  s_dialog->raise();
  s_dialog->activateWindow();
}

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("About gHammer"));
  setObjectName(QStringLiteral("dlgPage"));
  scale::setScaledStyleSheet(
      this, dialogStyle() + QStringLiteral("QTextBrowser#aboutText { background: #151a33; border: 1px solid #232A4E;"
                                           " border-radius: 8px; padding: 8px; }"));
  resize(scale::initialSize(this, {980, 640}));

  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto *body = new QWidget(this);
  auto *v = new QVBoxLayout(body);
  v->setContentsMargins(24, 20, 24, 16);
  v->setSpacing(14);

  auto *head = new QHBoxLayout;
  head->setSpacing(14);
  auto *icon = new QLabel(body);
  scale::bind(icon, [icon] {
    const int s = scale::px(56);
    icon->setPixmap(QPixmap(QStringLiteral(":/assets/icon.png")).scaled(s, s, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  });
  auto *names = new QVBoxLayout;
  names->setSpacing(2);
  auto *title = new QLabel(QStringLiteral("gHammer " GHAMMER_VERSION), body);
  title->setObjectName(QStringLiteral("dlgTitle"));
  scale::setRole(title, scale::Font::PageTitle);
  auto *byline = new QLabel(tr("by Sander Ferdinand - kroket.io"), body);
  byline->setObjectName(QStringLiteral("dlgSubtitle"));
  auto *built = new QLabel(tr("Qt %1").arg(QString::fromLatin1(qVersion())), body);
  built->setObjectName(QStringLiteral("dlgSubtitle"));
  scale::setRole(built, scale::Font::Caption);
  for (QLabel *l: {title, byline, built}) {
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    names->addWidget(l);
  }
  head->addWidget(icon, 0, Qt::AlignTop);
  head->addLayout(names, 1);
  v->addLayout(head);

  auto *section = new QLabel(tr("THIRD-PARTY SOFTWARE AND ARTWORK"), body);
  section->setObjectName(QStringLiteral("dlgSection"));
  v->addWidget(section);

  auto *split = new QHBoxLayout;
  split->setSpacing(12);
  m_list = new QListWidget(body);
  m_list->setObjectName(QStringLiteral("dlgList"));
  scale::bind(m_list, [this] { m_list->setFixedWidth(scale::tpx(240)); });
  for (const Component &c: components()) {
    auto *item = new QListWidgetItem(QString::fromUtf8(c.name), m_list);
    item->setToolTip(QString::fromUtf8(c.license));
  }
  m_text = new QTextBrowser(body);
  m_text->setObjectName(QStringLiteral("aboutText"));
  m_text->setOpenExternalLinks(true);
  split->addWidget(m_list);
  split->addWidget(m_text, 1);
  v->addLayout(split, 1);
  root->addWidget(body, 1);

  auto *footer = new QFrame(this);
  footer->setObjectName(QStringLiteral("dlgFooter"));
  auto *foot = new QHBoxLayout(footer);
  foot->setContentsMargins(16, 10, 16, 10);
  auto *aboutQt = new QPushButton(tr("About Qt"), footer);
  aboutQt->setObjectName(QStringLiteral("dlgFlat"));
  auto *close = new QPushButton(tr("Close"), footer);
  close->setObjectName(QStringLiteral("dlgPrimary"));
  close->setDefault(true);
  foot->addWidget(aboutQt);
  foot->addStretch(1);
  foot->addWidget(close);
  root->addWidget(footer);

  connect(aboutQt, &QPushButton::clicked, this, [this] { QMessageBox::aboutQt(this); });
  connect(close, &QPushButton::clicked, this, &QDialog::close);
  connect(m_list, &QListWidget::currentRowChanged, this, &AboutDialog::showComponent);
  m_list->setCurrentRow(0);
}

void AboutDialog::showComponent(const int index) {
  if (index < 0 || index >= components().size())
    return;
  const Component &c = components()[index];
  QString html = QStringLiteral("<h3>%1</h3><table>").arg(QString::fromUtf8(c.name).toHtmlEscaped());
  html += row(tr("Used for"), QString::fromUtf8(c.usedFor).toHtmlEscaped());
  html += row(tr("Copyright"), QString::fromUtf8(c.copyright).toHtmlEscaped());
  html += row(tr("License"), QString::fromUtf8(c.license).toHtmlEscaped());
  if (*c.url)
    html += row(tr("Website"), QStringLiteral("<a href='%1' style='color:#7fa6ff;'>%1</a>").arg(QString::fromLatin1(c.url)));
  html += QStringLiteral("</table>");
  if (*c.note)
    html += QStringLiteral("<p>%1</p>").arg(QString::fromUtf8(c.note).toHtmlEscaped());
  for (const QString &file: c.files) {
    const QString text = readLicense(file);
    if (!text.isEmpty())
      html += QStringLiteral("<hr><pre style='white-space:pre-wrap;'>%1</pre>").arg(text.toHtmlEscaped());
  }
  m_text->setHtml(html);
  m_text->moveCursor(QTextCursor::Start);
}

} // namespace ui
