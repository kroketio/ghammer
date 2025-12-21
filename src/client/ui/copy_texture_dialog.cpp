#include "copy_texture_dialog.h"
#include "client/ui/scale.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/mainwindow.h"
#include "client/net/texture_catalog.h"
#include "client/net/texture_edit_sessions.h"
#include "client/ui/action_manager.h"
#include "client/ui/dialog_style.h"
#include "client/ui/texture_ui.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

void CopyTextureDialog::run(vk::Engine3D *engine, const QString &ref, QWidget *parent) {
  auto *dlg = new CopyTextureDialog(engine, ref, parent);
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->open();
}

CopyTextureDialog::CopyTextureDialog(vk::Engine3D *engine, const QString &ref, QWidget *parent)
    : QDialog(parent), m_engine(engine), m_ref(MaterialRef::parse(ref).base()) {
  setWindowTitle(tr("Copy into pack"));
  setObjectName(QStringLiteral("dlgPage"));
  scale::setScaledStyleSheet(this, dialogStyle());
  scale::bind(this, [this] { setMinimumWidth(scale::tpx(560)); });
  const TextureEntry *e = net::TextureCatalog::instance().find(m_ref);

  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(20, 18, 20, 14);
  v->setSpacing(12);

  // source
  auto *head = new QHBoxLayout();
  head->setSpacing(14);
  m_thumb = new QLabel(this);
  m_thumb->setFixedSize(scale::px(84), scale::px(84));
  head->addWidget(m_thumb);
  auto *names = new QVBoxLayout();
  auto *title = new QLabel(tr("Copy %1").arg(e ? e->name : m_ref), this);
  title->setObjectName(QStringLiteral("dlgTitle"));
  auto *sub = new QLabel(tr("from %1").arg(e ? e->pack : QString()), this);
  sub->setObjectName(QStringLiteral("dlgSubtitle"));
  sub->setWordWrap(true);
  names->addWidget(title);
  names->addWidget(sub);
  names->addStretch(1);
  head->addLayout(names, 1);
  v->addLayout(head);
  auto paintThumb = [this, e] {
    if (!e)
      return;
    const QPixmap pm = ThumbCache::instance().server(e->thumbFile());
    const int edge = scale::px(84);
    QPixmap shown(edge, edge);
    shown.fill(Qt::transparent);
    QPainter p(&shown);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, edge, edge), scale::px(7), scale::px(7));
    p.setClipPath(clip);
    if (!pm.isNull())
      p.drawPixmap(0, 0, pm.scaled(edge, edge, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
    else
      TextureCardDelegate::paintPlaceholder(&p, QRect(0, 0, edge, edge));
    p.end();
    m_thumb->setPixmap(shown);
  };
  paintThumb();
  connect(&ThumbCache::instance(), &ThumbCache::ready, this, paintThumb);

  // target
  auto *card = new QWidget(this);
  card->setObjectName(QStringLiteral("dlgCard"));
  auto *form = new QFormLayout(card);
  form->setContentsMargins(16, 14, 16, 14);
  form->setHorizontalSpacing(14);
  form->setVerticalSpacing(10);
  auto *packRow = new QHBoxLayout();
  m_pack = new QComboBox(card);
  scale::bind(m_pack, [=, this] { m_pack->setMinimumWidth(scale::tpx(240)); });
  auto *newPack = new QToolButton(card);
  newPack->setObjectName(QStringLiteral("dlgTool"));
  newPack->setText(tr("New pack…"));
  packRow->addWidget(m_pack, 1);
  packRow->addWidget(newPack);
  form->addRow(tr("Into pack"), packRow);
  m_newPackRow = new QWidget(card);
  auto *np = new QHBoxLayout(m_newPackRow);
  np->setContentsMargins(0, 0, 0, 0);
  m_newPackName = new QLineEdit(m_newPackRow);
  m_newPackName->setPlaceholderText(tr("pack name (letters, digits, _ and -)"));
  auto *createBtn = new QPushButton(tr("Create"), m_newPackRow);
  createBtn->setObjectName(QStringLiteral("dlgFlat"));
  np->addWidget(m_newPackName, 1);
  np->addWidget(createBtn);
  m_newPackRow->hide();
  form->addRow(QString(), m_newPackRow);
  connect(newPack, &QToolButton::clicked, this, [this] {
    m_newPackRow->setVisible(!m_newPackRow->isVisible());
    if (m_newPackRow->isVisible()) {
      // suggest the map's file name
      QString suggestion;
      if (auto *mw = qobject_cast<MainWindow *>(window()->parentWidget() ? window()->parentWidget()->window() : nullptr))
        suggestion = QFileInfo(mw->currentMapPath()).completeBaseName();
      suggestion.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
      m_newPackName->setText(suggestion.isEmpty() ? QStringLiteral("my_map") : suggestion);
      m_newPackName->setFocus();
      m_newPackName->selectAll();
    }
  });
  connect(createBtn, &QPushButton::clicked, this, &CopyTextureDialog::createPack);
  connect(m_newPackName, &QLineEdit::returnPressed, this, &CopyTextureDialog::createPack);

  m_name = new QLineEdit(card);
  m_name->setPlaceholderText(tr("%1 (made unique if taken)").arg(e ? e->name : QString()));
  form->addRow(tr("Name"), m_name);
  m_copyProps = new QCheckBox(tr("Copy its server material properties"), card);
  m_copyProps->setChecked(true);
  m_copyProps->setEnabled(e && !e->props.isEmpty());
  form->addRow(QString(), m_copyProps);
  m_makeProject = new QCheckBox(card);
  form->addRow(QString(), m_makeProject);
  v->addWidget(card);

  // what happens afterwards
  auto *after = new QLabel(tr("AFTERWARDS"), this);
  after->setObjectName(QStringLiteral("dlgSection"));
  v->addWidget(after);
  auto *card2 = new QWidget(this);
  card2->setObjectName(QStringLiteral("dlgCard"));
  auto *c2 = new QVBoxLayout(card2);
  c2->setContentsMargins(16, 12, 16, 12);
  c2->setSpacing(8);
  const int selFaces = m_engine ? m_engine->facesUsingMaterial(m_ref, true) : 0;
  const int mapFaces = m_engine ? m_engine->facesUsingMaterial(m_ref, false) : 0;
  m_replaceSelection = new QRadioButton(tr("Use the copy on the selected faces (%1)").arg(selFaces), card2);
  m_replaceAll = new QRadioButton(tr("Use the copy on every face in this map that uses the original (%1)").arg(mapFaces),
                                  card2);
  m_replaceNone = new QRadioButton(tr("Don't change any faces"), card2);
  m_replaceSelection->setEnabled(selFaces > 0);
  m_replaceAll->setEnabled(mapFaces > 0);
  (selFaces > 0 ? m_replaceSelection : (mapFaces > 0 ? m_replaceAll : m_replaceNone))->setChecked(true);
  auto *grp = new QButtonGroup(this);
  grp->addButton(m_replaceSelection);
  grp->addButton(m_replaceAll);
  grp->addButton(m_replaceNone);
  c2->addWidget(m_replaceSelection);
  c2->addWidget(m_replaceAll);
  c2->addWidget(m_replaceNone);
  m_edit = new QCheckBox(tr("Open the color map in the image editor"), card2);
  c2->addWidget(m_edit);
  v->addWidget(card2);

  m_status = new QLabel(this);
  m_status->setObjectName(QStringLiteral("dlgDesc"));
  m_status->setWordWrap(true);
  v->addWidget(m_status);

  auto *buttons = new QHBoxLayout();
  buttons->addStretch(1);
  auto *cancel = new QPushButton(tr("Cancel"), this);
  cancel->setObjectName(QStringLiteral("dlgFlat"));
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  m_copy = new QPushButton(tr("Copy"), this);
  m_copy->setObjectName(QStringLiteral("dlgPrimary"));
  m_copy->setDefault(true);
  connect(m_copy, &QPushButton::clicked, this, &CopyTextureDialog::doCopy);
  buttons->addWidget(cancel);
  buttons->addWidget(m_copy);
  v->addLayout(buttons);

  connect(&net::TextureCatalog::instance(), &net::TextureCatalog::packsChanged, this,
          [this] { rebuildPacks(m_pack->currentData().toString()); });
  connect(m_pack, &QComboBox::currentIndexChanged, this, [this] {
    const QString pack = m_pack->currentData().toString();
    const QString project = m_engine ? m_engine->projectPack() : QString();
    m_makeProject->setText(tr("Make %1 this map's project pack").arg(pack.isEmpty() ? tr("it") : pack));
    m_makeProject->setVisible(!pack.isEmpty() && pack.compare(project, Qt::CaseInsensitive) != 0);
    m_makeProject->setChecked(project.isEmpty());
    m_copy->setEnabled(!pack.isEmpty());
  });
  rebuildPacks(m_engine ? m_engine->projectPack() : QString());
  if (m_pack->count() == 0) {
    m_status->setText(tr("There is no editable pack yet: create one for this map."));
    newPack->click();
  }
}

void CopyTextureDialog::rebuildPacks(const QString &select) {
  QSignalBlocker b(m_pack);
  m_pack->clear();
  const QString project = m_engine ? m_engine->projectPack() : QString();
  for (const auto &p: net::TextureCatalog::instance().packs()) {
    if (p.base)
      continue;
    const bool isProject = p.name.compare(project, Qt::CaseInsensitive) == 0;
    m_pack->addItem(ActionManager::icon(isProject ? QStringLiteral("pinned") : QStringLiteral("unlocked")),
                    isProject ? tr("%1  (project pack)").arg(p.name) : p.name, p.name);
  }
  int idx = m_pack->findData(select);
  if (idx < 0)
    idx = 0;
  m_pack->setCurrentIndex(idx);
  b.unblock();
  emit m_pack->currentIndexChanged(idx);
}

void CopyTextureDialog::createPack() {
  const QString name = m_newPackName->text().trimmed();
  if (!isValidAssetName(name)) {
    m_status->setText(tr("Pack names use letters, digits, '_' and '-'."));
    return;
  }
  QPointer<CopyTextureDialog> self(this);
  QJsonObject args;
  args.insert(QStringLiteral("name"), name);
  args.insert(QStringLiteral("description"), tr("Textures made for %1").arg(name));
  m_status->setText(tr("Creating %1…").arg(name));
  net::TextureCatalog::instance().call(QStringLiteral("packs.create"), args, this, [self, name](const net::Result &r) {
    if (!self)
      return;
    if (!r.ok) {
      self->m_status->setText(tr("Could not create the pack: %1").arg(r.message));
      return;
    }
    self->m_status->setText(tr("Created %1.").arg(name));
    self->m_newPackRow->hide();
    // packsChanged follows from the event; select it once it is listed
    net::TextureCatalog::instance().refreshPacks();
    QTimer::singleShot(300, self, [self, name] {
      if (self)
        self->rebuildPacks(name);
    });
  });
}

void CopyTextureDialog::doCopy() {
  const QString pack = m_pack->currentData().toString();
  if (pack.isEmpty())
    return;
  m_copy->setEnabled(false);
  m_status->setText(tr("Copying on the server…"));
  QJsonObject args;
  args.insert(QStringLiteral("from"), m_ref);
  args.insert(QStringLiteral("pack"), pack);
  args.insert(QStringLiteral("name"), m_name->text().trimmed());
  args.insert(QStringLiteral("copy_props"), m_copyProps->isChecked());
  QPointer<CopyTextureDialog> self(this);
  net::TextureCatalog::instance().call(QStringLiteral("textures.copy"), args, this, [self, pack](const net::Result &r) {
    if (!self)
      return;
    if (!r.ok) {
      self->m_status->setText(tr("Could not copy: %1").arg(r.message));
      self->m_copy->setEnabled(true);
      return;
    }
    const TextureEntry copy = TextureEntry::fromJson(r.data.value(QStringLiteral("entry")).toObject());
    if (self->m_engine) {
      if (self->m_makeProject->isVisible() && self->m_makeProject->isChecked())
        self->m_engine->setProjectPack(pack);
      if (self->m_replaceSelection->isChecked())
        self->m_engine->replaceMaterial(self->m_ref, copy.ref, true);
      else if (self->m_replaceAll->isChecked())
        self->m_engine->replaceMaterial(self->m_ref, copy.ref, false);
    }
    if (self->m_edit->isChecked())
      net::TextureEditSessions::instance().start(copy.ref, QStringLiteral("color"), self->parentWidget());
    self->accept();
  });
}

} // namespace ui
