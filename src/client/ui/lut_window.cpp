#include "lut_window.h"

#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QInputDialog>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QDir>
#include <QGuiApplication>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

#include "client/net/asset_client.h"
#include "client/net/texture_catalog.h"
#include "client/ui/action_manager.h"
#include "client/ui/texture_ui.h"
#include "client/ui/dialog_style.h"
#include "client/ui/lut_curves.h"
#include "client/ui/lut_step_editors.h"
#include "client/ui/scale.h"
#include "client/vulkan/vk_engine.h"
#include "client/ui/file_dialog.h"
#include "client/ui/widgets.h"
#include <QTreeWidget>

namespace ui {

namespace {

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

// Hue sweep, skin tones, grey ramp.
QImage chart(const QSize &size) {
  QImage img(size, QImage::Format_RGB888);
  const int w = size.width(), h = size.height();
  const int hueRows = h * 55 / 100, skinRows = h * 25 / 100;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double u = double(x) / std::max(1, w - 1);
      QColor c;
      if (y < hueRows) {
        const double v = double(y) / std::max(1, hueRows - 1);
        c = QColor::fromHsvF(float(u), float(1.0 - v * 0.75), float(1.0 - v * 0.15));
      } else if (y < hueRows + skinRows) {
        c = QColor::fromHsvF(float(0.05 + 0.03 * std::sin(u * 6.0)), 0.45f, float(0.2 + 0.75 * u));
      } else {
        const int g = int(std::lround(u * 255.0));
        c = QColor(g, g, g);
      }
      img.setPixelColor(x, y, c);
    }
  return img;
}

std::optional<eng::LutGrade> gradeOf(const TextureEntry &e) {
  const QJsonObject g = e.lut.value(QStringLiteral("grade")).toObject();
  if (g.isEmpty())
    return std::nullopt;
  return eng::LutGrade::fromJson(g);
}

QString sanitizedName(const QString &wanted) {
  QString out;
  for (const QChar c: wanted)
    out += (c.isLetterOrNumber() && c.unicode() < 128) || c == QLatin1Char('-') ? c : QLatin1Char('_');
  while (out.contains(QStringLiteral("__")))
    out.replace(QStringLiteral("__"), QStringLiteral("_"));
  out = out.left(56);
  return out.isEmpty() || out == QStringLiteral("_") ? QStringLiteral("LUT") : out;
}

} // namespace

const QImage &lutPreviewPicture() {
  static const QImage picture = chart(QSize(720, 480));
  return picture;
}


LutWindow::LutWindow(vk::Engine3D *engine, QWidget *parent) : QDialog(parent), m_engine(engine) {
  setWindowTitle(tr("Colour LUTs"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  scale::setScaledStyleSheet(this, ui::dialogStyle());
  resize(scale::initialSize(this, {860, 820}));
  m_upload = new QTimer(this);
  m_upload->setSingleShot(true);
  m_upload->setInterval(600);
  connect(m_upload, &QTimer::timeout, this, &LutWindow::upload);
  m_refresh = new QTimer(this);
  m_refresh->setSingleShot(true);
  m_refresh->setInterval(150);
  connect(m_refresh, &QTimer::timeout, this, &LutWindow::refresh);

  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  auto *page = new QWidget(this);
  page->setObjectName(QStringLiteral("dlgPage"));
  auto *root = new QHBoxLayout(page);
  root->setContentsMargins(20, 18, 20, 18);
  root->setSpacing(18);
  outer->addWidget(page, 1);

  // Left: the server's LUTs by pack.
  auto *left = new QVBoxLayout;
  left->setSpacing(8);
  left->addWidget(makeLabel(tr("LUTS"), "dlgSection", page));
  m_list = new QTreeWidget(page);
  m_list->setObjectName(QStringLiteral("dlgTree"));
  m_list->setHeaderHidden(true);
  m_list->setRootIsDecorated(false);
  m_list->setFocusPolicy(Qt::NoFocus);
  scale::bind(m_list, [this] {
    m_list->setFixedWidth(scale::tpx(220));
    m_list->setIndentation(scale::px(14));
    m_list->setIconSize(scale::icon(scale::Icon::Small));
    rebuildList();
  });
  left->addWidget(m_list, 1);
  m_listNote = makeLabel(QString(), "dlgDesc", page);
  m_listNote->setWordWrap(true);
  m_listNote->hide();
  left->addWidget(m_listNote);
  auto *packRow = new QHBoxLayout;
  packRow->setSpacing(6);
  packRow->addWidget(makeLabel(tr("Pack"), "dlgDesc", page));
  m_pack = new QComboBox(page);
  m_pack->setToolTip(tr("Where new, imported and copied LUTs go"));
  packRow->addWidget(m_pack, 1);
  left->addLayout(packRow);
  auto *tools = new QHBoxLayout;
  tools->setSpacing(6);
  const auto addTool = [&](const QString &text, const QString &icon, const QString &tip, void (LutWindow::*fn)()) {
    auto *b = new QToolButton(page);
    b->setObjectName(QStringLiteral("dlgTool"));
    b->setText(text);
    b->setIcon(ui::ActionManager::icon(icon));
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setToolTip(tip);
    connect(b, &QToolButton::clicked, this, fn);
    tools->addWidget(b);
  };
  addTool(tr("New"), QStringLiteral("add"), tr("A LUT made from adjustments"), &LutWindow::newLut);
  addTool(tr("Import…"), QStringLiteral("import"), tr(".png strip / grid of N×N slices, or .cube"),
          &LutWindow::importLut);
  left->addLayout(tools);
  root->addLayout(left, 0);

  // Right: nothing selected, or the selected LUT.
  m_stack = new QStackedWidget(page);
  root->addWidget(m_stack, 1);
  auto *empty = new QWidget(m_stack);
  auto *ev = new QVBoxLayout(empty);
  ev->addStretch(1);
  auto *emptyText = makeLabel(tr("No LUT selected"), "dlgDesc", empty);
  emptyText->setAlignment(Qt::AlignCenter);
  ev->addWidget(emptyText);
  ev->addStretch(1);
  m_stack->addWidget(empty);

  auto *sel = new QWidget(m_stack);
  auto *sv = new QVBoxLayout(sel);
  sv->setContentsMargins(0, 0, 0, 0);
  sv->setSpacing(8);
  m_name = new QLineEdit(sel);
  m_name->setPlaceholderText(tr("Name"));
  m_name->setToolTip(tr("Letters, digits, '_' and '-'"));
  scale::setRole(m_name, scale::Font::Heading);
  sv->addWidget(m_name);
  m_kind = makeLabel(QString(), "dlgSubtitle", sel);
  m_kind->setWordWrap(true);
  sv->addWidget(m_kind);
  auto *actions = new QHBoxLayout;
  actions->setSpacing(6);
  m_use = makeButton(tr("Use in Level"), "dlgPrimary", sel);
  m_use->setToolTip(tr("Make this the level's colour LUT (World Environment)"));
  connect(m_use, &QPushButton::clicked, this, [this] {
    if (m_engine && !m_ref.isEmpty())
      m_engine->setColorLut(m_ref);
  });
  m_copy = makeButton(tr("Copy to Pack"), "dlgFlat", sel);
  m_copy->setToolTip(tr("An editable copy in the pack chosen on the left"));
  connect(m_copy, &QPushButton::clicked, this, &LutWindow::copyToPack);
  m_delete = makeButton(tr("Delete"), "dlgFlat", sel);
  m_delete->setToolTip(tr("Delete this LUT from its pack"));
  connect(m_delete, &QPushButton::clicked, this, &LutWindow::deleteLut);
  actions->addWidget(m_use);
  actions->addWidget(m_copy);
  actions->addWidget(m_delete);
  actions->addStretch(1);
  sv->addLayout(actions);
  sv->addSpacing(6);
  m_kindStack = new QStackedWidget(sel);
  sv->addWidget(m_kindStack, 1);

  // Adjustments (a LUT made here).
  m_gradePane = new QWidget(m_kindStack);
  auto *gp = new QVBoxLayout(m_gradePane);
  gp->setContentsMargins(0, 0, 0, 0);
  gp->setSpacing(6);
  gp->addWidget(makeLabel(tr("ADJUSTMENTS"), "dlgSection", m_gradePane));
  m_steps = new QListWidget(m_gradePane);
  m_steps->setObjectName(QStringLiteral("dlgList"));
  scale::bind(m_steps, [this] { m_steps->setFixedHeight(scale::tpx(150)); });
  gp->addWidget(m_steps);
  auto *stepTools = new QHBoxLayout;
  stepTools->setSpacing(4);
  auto *add = new QToolButton(m_gradePane);
  add->setObjectName(QStringLiteral("dlgTool"));
  add->setText(tr("Add"));
  add->setIcon(ui::ActionManager::icon(QStringLiteral("add")));
  add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  add->setPopupMode(QToolButton::InstantPopup);
  add->setToolTip(tr("Add an adjustment after the selected one"));
  auto *addMenu = new QMenu(add);
  for (const auto kind: {eng::GradeStep::Kind::Levels, eng::GradeStep::Kind::Curves, eng::GradeStep::Kind::ColorBalance,
                         eng::GradeStep::Kind::Hsv, eng::GradeStep::Kind::Cdl, eng::GradeStep::Kind::Basic})
    addMenu->addAction(gradeKindName(kind), this, [this, kind] { addStep(kind); });
  add->setMenu(addMenu);
  stepTools->addWidget(add);
  const auto stepTool = [&](const QString &text, const QString &icon, std::function<void()> fn) {
    auto *b = new QToolButton(m_gradePane);
    b->setObjectName(QStringLiteral("dlgTool"));
    b->setText(text);
    b->setIcon(ui::ActionManager::icon(icon));
    b->setToolButtonStyle(icon.isEmpty() ? Qt::ToolButtonTextOnly : Qt::ToolButtonTextBesideIcon);
    connect(b, &QToolButton::clicked, this, std::move(fn));
    stepTools->addWidget(b);
    m_needsStep << b;
  };
  stepTool(tr("Remove"), QStringLiteral("trash"), [this] { removeStep(); });
  stepTool(QStringLiteral("▲"), {}, [this] { moveStep(-1); });
  stepTool(QStringLiteral("▼"), {}, [this] { moveStep(1); });
  m_needsStep[0]->setToolTip(tr("Remove the selected adjustment"));
  m_needsStep[1]->setToolTip(tr("Move the selected adjustment up (applied earlier)"));
  m_needsStep[2]->setToolTip(tr("Move the selected adjustment down (applied later)"));
  stepTools->addStretch(1);
  gp->addLayout(stepTools);

  auto *editorScroll = new QScrollArea(m_gradePane);
  editorScroll->setWidgetResizable(true);
  editorScroll->setFrameShape(QFrame::NoFrame);
  m_stepEditors = new QStackedWidget(editorScroll);
  editorScroll->setWidget(m_stepEditors);
  for (int k = 0; k < int(eng::GradeStep::Kind::Count); ++k) {
    StepEditor *ed = makeStepEditor(
        eng::GradeStep::Kind(k),
        [this](const QString &key, const std::function<void(eng::GradeStep &)> &fn) {
          editStep(m_ref, selectedStepId(), key, fn);
        },
        [this] { openStepWindow(m_ref, selectedStepId()); },
        [this](eng::GradeChannel ch) {
          return m_grade ? m_input.histogram(*m_grade, selectedStepId(), ch) : eng::grade::Histogram {};
        },
        m_stepEditors);
    m_editors.push_back(ed);
    m_stepEditors->addWidget(ed);
  }
  m_stepEditors->addWidget(new QWidget(m_stepEditors)); // no step selected
  gp->addWidget(editorScroll, 1);
  m_kindStack->addWidget(m_gradePane);

  connect(m_steps, &QListWidget::currentRowChanged, this, [this] {
    m_stepId = selectedStepId();
    loadStepEditor();
  });
  connect(m_steps, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
    const bool on = it->checkState() == Qt::Checked;
    editStep(m_ref, it->data(Qt::UserRole).toInt(), QStringLiteral("enabled"),
             [on](eng::GradeStep &st) { st.enabled = on; });
  });
  connect(m_steps, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem *it) { openStepWindow(m_ref, it->data(Qt::UserRole).toInt()); });

  // Imported: used as is.
  auto *imported = new QWidget(m_kindStack);
  auto *iv = new QVBoxLayout(imported);
  iv->setContentsMargins(0, 0, 0, 0);
  iv->setSpacing(8);
  iv->addWidget(makeLabel(tr("LUT IMAGE"), "dlgSection", imported));
  auto *icard = makeCard(imported);
  auto *icl = new QVBoxLayout(icard);
  icl->setContentsMargins(14, 12, 14, 12);
  m_stripLabel = new QLabel(icard);
  m_stripLabel->setAlignment(Qt::AlignCenter);
  icl->addWidget(m_stripLabel);
  iv->addWidget(icard);
  iv->addStretch(1);
  m_kindStack->addWidget(imported);
  m_stack->addWidget(sel);

  auto *footer = new QFrame(this);
  footer->setObjectName(QStringLiteral("dlgFooter"));
  auto *fv = new QVBoxLayout(footer);
  fv->setContentsMargins(16, 10, 16, 10);
  fv->setSpacing(8);
  auto *godotNote = makeLabel(tr("Godot export writes the level's LUT to textures/luts/ as a Texture3D, but does not "
                                 "apply it: in Godot, add a WorldEnvironment, turn on Adjustments and pick the LUT "
                                 "as its Color Correction."),
                              "dlgDesc", footer);
  godotNote->setWordWrap(true);
  fv->addWidget(godotNote);
  auto *fl = new QHBoxLayout;
  fv->addLayout(fl);
  auto *tpl = makeButton(tr("Save neutral template…"), "dlgFlat", footer);
  tpl->setToolTip(tr("Save the neutral 33³ LUT strip as a .png, to grade in an image editor and import"));
  connect(tpl, &QPushButton::clicked, this, &LutWindow::saveTemplate);
  auto *copyTpl = makeButton(tr("Copy base LUT to clipboard"), "dlgFlat", footer);
  copyTpl->setToolTip(tr("Copy the neutral 33³ LUT strip to the clipboard, to paste into an image editor"));
  connect(copyTpl, &QPushButton::clicked, this,
          [] { QGuiApplication::clipboard()->setImage(eng::lut::templateStrip().convertToFormat(QImage::Format_RGB32)); });
  m_export = makeButton(tr("Export LUT…"), "dlgFlat", footer);
  m_export->setToolTip(tr("Save the selected LUT as a .png strip or a .cube file"));
  connect(m_export, &QPushButton::clicked, this, &LutWindow::exportLut);
  auto *close = makeButton(tr("Close"), "dlgPrimary", footer);
  close->setToolTip(tr("Close; the 3D view shows the level's own LUT again"));
  connect(close, &QPushButton::clicked, this, &QDialog::close);
  fl->addWidget(tpl);
  fl->addWidget(copyTpl);
  fl->addWidget(m_export);
  fl->addStretch(1);
  fl->addWidget(close);
  outer->addWidget(footer);

  connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
    const QString ref = it ? it->data(0, Qt::UserRole).toString() : QString();
    if (!ref.isEmpty())
      select(ref);
  });
  connect(m_list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it) {
    if (it->parent() || it->childCount() == 0)
      return;
    it->setExpanded(!it->isExpanded());
    const QString pack = it->text(0).toLower();
    if (it->isExpanded())
      m_collapsedPacks.remove(pack);
    else
      m_collapsedPacks.insert(pack);
  });
  connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *it) {
    if (const QString ref = it->data(0, Qt::UserRole).toString(); m_engine && !ref.isEmpty())
      m_engine->setColorLut(ref);
  });
  connect(m_name, &QLineEdit::editingFinished, this, &LutWindow::renameLut);
  connect(m_pack, &QComboBox::activated, this, [this](int row) {
    if (m_pack->itemData(row).toString().isEmpty() && row == m_pack->count() - 1)
      (void) targetPack();
  });

  auto &cat = net::TextureCatalog::instance();
  const auto later = [this] { m_refresh->start(); };
  connect(&cat, &net::TextureCatalog::catalogChanged, this, later);
  connect(&cat, &net::TextureCatalog::serverChanged, this, later);
  connect(&cat, &net::TextureCatalog::packsChanged, this, [this, later] {
    rebuildPackCombo();
    later();
  });
  connect(&cat, &net::TextureCatalog::entryRemoved, this, later);
  connect(&cat, &net::TextureCatalog::entryChanged, this, [this, later](const QString &ref) {
    const TextureEntry *e = net::TextureCatalog::instance().find(ref, true);
    if (e)
      later();
  });
  connect(&net::AssetClient::instance(), &net::AssetClient::stateChanged, this, later);
  if (m_engine)
    connect(m_engine, &vk::Engine3D::lutsChanged, this, [this] {
      if (!m_editing)
        rebuildList();
      showSelected();
    });
  rebuildPackCombo();
  refresh();
  m_stack->setCurrentIndex(0);
}

void LutWindow::showEvent(QShowEvent *e) {
  QDialog::showEvent(e);
  if (m_engine && !m_ref.isEmpty())
    m_engine->previewColorLutStrip(m_strip);
}

// The level's own LUT again; the pick here was only a preview.
void LutWindow::hideEvent(QHideEvent *e) {
  upload();
  if (m_engine)
    m_engine->endColorLutPreview();
  QDialog::hideEvent(e);
}

void LutWindow::selectLut(const QString &ref) {
  for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
    QTreeWidgetItem *pack = m_list->topLevelItem(i);
    for (int j = 0; j < pack->childCount(); ++j)
      if (refKey(pack->child(j)->data(0, Qt::UserRole).toString()) == refKey(ref)) {
        pack->setExpanded(true);
        m_collapsedPacks.remove(pack->text(0).toLower());
        m_list->setCurrentItem(pack->child(j));
        return;
      }
  }
  m_pendingSelect = ref;
}

void LutWindow::refresh() {
  if (!net::AssetClient::instance().isOnline()) {
    m_entries.clear();
    rebuildList();
    m_listNote->setText(tr("Asset server offline"));
    m_listNote->show();
    return;
  }
  QJsonObject args;
  args.insert(QStringLiteral("lut"), true);
  args.insert(QStringLiteral("sort"), QStringLiteral("pack"));
  args.insert(QStringLiteral("limit"), 240);
  net::TextureCatalog::instance().query(args, this, [this](const net::QueryPage &page) {
    if (!page.ok) {
      m_listNote->setText(tr("Could not load LUTs: %1").arg(page.error));
      m_listNote->show();
      return;
    }
    m_entries = page.items;
    m_listNote->hide();
    // The selected LUT as the server has it now; adjustments being edited here stay.
    if (!m_ref.isEmpty()) {
      const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                                   [this](const TextureEntry &e) { return refKey(e.ref) == refKey(m_ref); });
      if (it == m_entries.end()) {
        m_ref.clear();
        m_entry = {};
        m_grade.reset();
        m_strip = {};
      } else {
        const bool newData = it->rev != m_entry.rev;
        m_entry = *it;
        if (newData && !m_dirty) {
          m_grade = gradeOf(m_entry);
          m_strip = m_grade ? eng::grade::apply(eng::lut::templateStrip(), *m_grade) : QImage();
          if (!m_grade)
            fetchStrip();
        }
      }
    }
    rebuildList();
    if (!m_pendingSelect.isEmpty()) {
      const QString want = m_pendingSelect;
      m_pendingSelect.clear();
      selectLut(want);
    }
    showSelected();
  });
}

void LutWindow::rebuildPackCombo() {
  const QString keep = m_pack->currentData().toString();
  QSignalBlocker block(m_pack);
  m_pack->clear();
  for (const net::PackInfo &p: net::TextureCatalog::instance().lutPacks())
    if (!p.base)
      m_pack->addItem(ui::ActionManager::icon(QStringLiteral("unlocked")), p.name, p.name);
  m_pack->addItem(tr("New pack…"));
  const int idx = m_pack->findData(keep);
  m_pack->setCurrentIndex(idx >= 0 ? idx : 0);
}

void LutWindow::rebuildList() {
  QSignalBlocker block(m_list);
  m_list->clear();
  const QString inLevel = m_engine ? m_engine->colorLut() : QString();
  const QIcon glyph = subItemGlyph(scale::icon(scale::Icon::Small).height());
  QTreeWidgetItem *current = nullptr;
  for (const net::PackInfo &p: net::TextureCatalog::instance().lutPacks()) {
    auto *head = new QTreeWidgetItem(m_list, {p.name});
    head->setIcon(0, ui::ActionManager::icon(p.base ? QStringLiteral("locked") : QStringLiteral("unlocked")));
    head->setFlags(Qt::ItemIsEnabled);
    head->setToolTip(0, p.base ? tr("Base pack (read-only)") : QString());
    for (const TextureEntry &e: m_entries) {
      if (e.pack.compare(p.name, Qt::CaseInsensitive) != 0)
        continue;
      const bool used = !inLevel.isEmpty() && refKey(e.ref) == refKey(inLevel);
      auto *it = new QTreeWidgetItem(head, {used ? tr("%1 · in level").arg(e.name) : e.name});
      it->setIcon(0, glyph);
      it->setToolTip(0, gradeOf(e) ? tr("Made from adjustments") : tr("Imported strip"));
      it->setData(0, Qt::UserRole, e.ref);
      if (used) {
        QFont f = it->font(0);
        f.setBold(true);
        it->setFont(0, f);
      }
      if (!m_ref.isEmpty() && refKey(e.ref) == refKey(m_ref))
        current = it;
    }
    head->setExpanded(!m_collapsedPacks.contains(p.name.toLower()) || (current && current->parent() == head));
  }
  if (current)
    m_list->setCurrentItem(current);
}

void LutWindow::select(const QString &ref) {
  if (refKey(ref) == refKey(m_ref))
    return;
  upload();
  const auto it = std::find_if(m_entries.begin(), m_entries.end(),
                               [&](const TextureEntry &e) { return refKey(e.ref) == refKey(ref); });
  if (it == m_entries.end())
    return;
  m_ref = it->ref;
  m_entry = *it;
  m_grade = gradeOf(m_entry);
  m_strip = m_grade ? eng::grade::apply(eng::lut::templateStrip(), *m_grade) : QImage();
  m_stepId = 0;
  if (!m_grade)
    fetchStrip();
  showSelected();
  syncStepWindows();
}

void LutWindow::fetchStrip() {
  const MapFile *f = m_entry.map(MapType::Lut);
  if (!f)
    return;
  if (auto it = m_strips.constFind(f->checksum); it != m_strips.constEnd()) {
    m_strip = *it;
    return;
  }
  const QString ref = m_ref, checksum = f->checksum;
  const MaterialRef r = MaterialRef::parse(m_entry.ref);
  net::AssetClient::instance().httpGet(QStringLiteral("/api/2/source/%1/%2/lut").arg(r.pack, r.name), this,
                                       [this, ref, checksum](QNetworkReply *reply) {
                                         QImage img = QImage::fromData(reply->readAll(), "PNG");
                                         if (img.isNull())
                                           return;
                                         img = img.convertToFormat(QImage::Format_RGB888);
                                         m_strips.insert(checksum, img);
                                         if (refKey(ref) == refKey(m_ref) && !m_grade) {
                                           m_strip = img;
                                           showSelected();
                                         }
                                       });
}

bool LutWindow::editable() const {
  return m_entry.isValid() && !m_entry.base;
}

void LutWindow::showSelected() {
  if (m_ref.isEmpty() || !m_entry.isValid()) {
    m_stack->setCurrentIndex(0);
    m_export->setEnabled(false);
    return;
  }
  m_stack->setCurrentIndex(1);
  if (!m_name->hasFocus())
    m_name->setText(m_entry.name);
  m_name->setReadOnly(!editable());
  const bool used = m_engine && refKey(m_engine->colorLut()) == refKey(m_ref);
  const int n = m_strip.height();
  QStringList parts{m_entry.pack, m_grade ? tr("Adjustments") : tr("Imported")};
  if (n >= 2)
    parts << QStringLiteral("%1³").arg(n);
  if (m_entry.base)
    parts << tr("base pack (read-only)");
  if (used)
    parts << tr("in level");
  m_kind->setText(parts.join(QStringLiteral(" · ")));
  m_use->setEnabled(!used);
  m_use->setText(used ? tr("In Level") : tr("Use in Level"));
  m_delete->setEnabled(editable());
  m_export->setEnabled(!m_strip.isNull());
  if (m_engine && isVisible())
    m_engine->previewColorLutStrip(m_strip);
  if (m_grade) {
    m_kindStack->setCurrentIndex(0);
    m_gradePane->setEnabled(editable());
    rebuildSteps();
    loadStepEditor();
  } else {
    m_kindStack->setCurrentIndex(1);
    m_stripLabel->setPixmap(m_strip.isNull() ? QPixmap()
                                             : QPixmap::fromImage(m_strip.scaled(scale::tpx(400) - scale::px(40),
                                                                                 scale::px(40), Qt::KeepAspectRatio,
                                                                                 Qt::FastTransformation)));
  }
}

const eng::LutGrade *LutWindow::grade(const QString &ref) const {
  return m_grade && !m_ref.isEmpty() && refKey(ref) == refKey(m_ref) ? &*m_grade : nullptr;
}

const eng::GradeStep *LutWindow::gradeStep(const QString &ref, int stepId) const {
  const eng::LutGrade *g = grade(ref);
  if (!g)
    return nullptr;
  const int idx = g->indexOf(stepId);
  return idx >= 0 ? &g->steps[size_t(idx)] : nullptr;
}

QImage LutWindow::strip(const QString &ref) const {
  return !m_ref.isEmpty() && refKey(ref) == refKey(m_ref) ? m_strip : QImage();
}

void LutWindow::editStep(const QString &ref, int stepId, const QString &key,
                         const std::function<void(eng::GradeStep &)> &fn) {
  Q_UNUSED(key)
  if (!editable() || !gradeStep(ref, stepId))
    return;
  fn(m_grade->steps[size_t(m_grade->indexOf(stepId))]);
  gradeChanged();
  if (m_steps->currentItem()) {
    const QSignalBlocker block(m_steps);
    for (int i = 0; i < m_steps->count(); ++i) {
      QListWidgetItem *row = m_steps->item(i);
      if (const eng::GradeStep *st = gradeStep(m_ref, row->data(Qt::UserRole).toInt()))
        row->setCheckState(st->enabled ? Qt::Checked : Qt::Unchecked);
    }
  }
  loadStepEditor();
}

// The neutral template through the adjustments: shown here and in the level at once, uploaded a moment later.
void LutWindow::gradeChanged() {
  if (!m_grade)
    return;
  m_strip = eng::grade::apply(eng::lut::templateStrip(), *m_grade);
  m_dirty = true;
  m_upload->start();
  if (m_engine) {
    m_editing = true;
    m_engine->previewColorLut(m_ref, m_strip);
    m_engine->previewColorLutStrip(m_strip);
    m_editing = false;
  }
  syncStepWindows();
}

void LutWindow::upload() {
  m_upload->stop();
  if (!m_dirty || !editable() || !m_grade)
    return;
  m_dirty = false;
  put(m_entry.pack, m_entry.name, m_strip, m_grade, {});
}

void LutWindow::put(const QString &pack, const QString &name, const QImage &strip,
                    const std::optional<eng::LutGrade> &grade, const std::function<void(const QString &ref)> &done) {
  QByteArray png;
  QBuffer buf(&png);
  buf.open(QIODevice::WriteOnly);
  strip.convertToFormat(QImage::Format_RGB888).save(&buf, "PNG");
  QHash<QByteArray, QByteArray> headers;
  headers.insert("Content-Type", "image/png");
  if (grade)
    headers.insert("X-Lut-Grade", QJsonDocument(grade->toJson()).toJson(QJsonDocument::Compact).toBase64());
  QPointer<LutWindow> self(this);
  net::AssetClient::instance().httpPut(
      QStringLiteral("/api/2/lut/%1/%2").arg(pack, name), png, headers, this, [self, done](QNetworkReply *reply) {
        const net::Result r = net::AssetClient::resultFromReply(reply);
        if (!self)
          return;
        if (!r.ok) {
          QMessageBox::warning(self, tr("Colour LUT"), r.message.isEmpty() ? tr("The asset server refused it") : r.message);
          return;
        }
        const QString ref = r.data.value(QStringLiteral("entry")).toObject().value(QStringLiteral("ref")).toString();
        if (done)
          done(ref);
      });
}

LutStepWindow *LutWindow::openStepWindow(const QString &ref, int stepId) {
  const eng::GradeStep *st = gradeStep(ref, stepId);
  if (!st || st->kind != eng::GradeStep::Kind::Curves || !editable())
    return nullptr;
  for (const QPointer<LutStepWindow> &w: m_stepWindows)
    if (w && refKey(w->lut()) == refKey(ref) && w->stepId() == stepId) {
      w->show();
      w->raise();
      w->activateWindow();
      return w;
    }
  const QString title = QStringLiteral("%1 — %2").arg(gradeKindName(st->kind), m_entry.name);
  LutStepWindow *w = new CurvesWindow(this, m_engine, m_ref, stepId, title);
  m_stepWindows.push_back(w);
  w->show();
  return w;
}

void LutWindow::syncStepWindows() {
  m_stepWindows.removeAll(nullptr);
  const auto windows = m_stepWindows;
  for (const QPointer<LutStepWindow> &w: windows)
    if (w)
      w->sync();
}

int LutWindow::selectedStepId() const {
  const QListWidgetItem *it = m_steps->currentItem();
  return it ? it->data(Qt::UserRole).toInt() : 0;
}

void LutWindow::rebuildSteps() {
  const QSignalBlocker block(m_steps);
  m_steps->clear();
  if (!m_grade)
    return;
  int row = -1;
  for (int i = 0; i < int(m_grade->steps.size()); ++i) {
    const eng::GradeStep &st = m_grade->steps[size_t(i)];
    auto *it = new QListWidgetItem(gradeKindName(st.kind), m_steps);
    it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
    it->setCheckState(st.enabled ? Qt::Checked : Qt::Unchecked);
    it->setData(Qt::UserRole, st.id);
    if (st.id == m_stepId)
      row = i;
  }
  if (row < 0 && !m_grade->steps.empty())
    row = 0;
  m_steps->setCurrentRow(row);
  m_stepId = selectedStepId();
}

void LutWindow::loadStepEditor() {
  const eng::GradeStep *st = gradeStep(m_ref, selectedStepId());
  for (QAbstractButton *b: m_needsStep)
    b->setEnabled(st != nullptr);
  if (!st) {
    m_stepEditors->setCurrentIndex(m_stepEditors->count() - 1);
    return;
  }
  StepEditor *ed = m_editors[size_t(st->kind)];
  m_stepEditors->setCurrentWidget(ed);
  ed->load(*st);
}

void LutWindow::setGrade(const eng::LutGrade &g, int selectStepId) {
  if (!editable())
    return;
  m_grade = g;
  m_stepId = selectStepId;
  gradeChanged();
  rebuildSteps();
  loadStepEditor();
}

void LutWindow::addStep(eng::GradeStep::Kind kind) {
  if (!m_grade || !editable())
    return;
  eng::LutGrade g = *m_grade;
  eng::GradeStep st = eng::GradeStep::make(kind);
  st.id = g.nextId();
  const int at = g.indexOf(selectedStepId());
  g.steps.insert(at >= 0 ? g.steps.begin() + at + 1 : g.steps.end(), st);
  setGrade(g, st.id);
  if (kind == eng::GradeStep::Kind::Curves)
    openStepWindow(m_ref, st.id);
}

void LutWindow::removeStep() {
  if (!gradeStep(m_ref, selectedStepId()))
    return;
  eng::LutGrade g = *m_grade;
  const int at = g.indexOf(selectedStepId());
  g.steps.erase(g.steps.begin() + at);
  const int next = g.steps.empty() ? 0 : g.steps[size_t(std::min(at, int(g.steps.size()) - 1))].id;
  setGrade(g, next);
}

void LutWindow::moveStep(int delta) {
  if (!gradeStep(m_ref, selectedStepId()))
    return;
  eng::LutGrade g = *m_grade;
  const int at = g.indexOf(selectedStepId()), to = at + delta;
  if (to < 0 || to >= int(g.steps.size()))
    return;
  std::swap(g.steps[size_t(at)], g.steps[size_t(to)]);
  setGrade(g, selectedStepId());
}

// The user LUT pack new LUTs go into; asks for a new one when there is none (or "New pack…" is picked).
QString LutWindow::targetPack() {
  const QString chosen = m_pack->currentData().toString();
  if (!chosen.isEmpty())
    return chosen;
  bool ok = false;
  const QString name = QInputDialog::getText(this, tr("New LUT pack"), tr("Pack name"), QLineEdit::Normal,
                                             QStringLiteral("my_luts"), &ok)
                           .trimmed();
  if (!ok || name.isEmpty())
    return {};
  if (!isValidAssetName(name)) {
    QMessageBox::warning(this, tr("New LUT pack"), tr("Pack names use letters, digits, '_' and '-'."));
    return {};
  }
  QJsonObject args;
  args.insert(QStringLiteral("name"), name);
  args.insert(QStringLiteral("type"), QStringLiteral("lut"));
  QPointer<LutWindow> self(this);
  net::TextureCatalog::instance().call(QStringLiteral("packs.create"), args, this, [self, name](const net::Result &r) {
    if (!self)
      return;
    if (!r.ok) {
      QMessageBox::warning(self, tr("New LUT pack"), r.message);
      return;
    }
    net::TextureCatalog::instance().refreshPacks();
    QTimer::singleShot(300, self, [self, name] {
      if (self)
        self->m_pack->setCurrentIndex(std::max(0, self->m_pack->findData(name)));
    });
  });
  return {};
}

QString LutWindow::uniqueName(const QString &pack, const QString &wanted) const {
  const QString base = sanitizedName(wanted);
  const auto taken = [&](const QString &n) {
    return std::any_of(m_entries.begin(), m_entries.end(), [&](const TextureEntry &e) {
      return e.pack.compare(pack, Qt::CaseInsensitive) == 0 && e.name.compare(n, Qt::CaseInsensitive) == 0;
    });
  };
  if (!taken(base))
    return base;
  for (int i = 2;; ++i)
    if (const QString n = QStringLiteral("%1_%2").arg(base).arg(i); !taken(n))
      return n;
}

void LutWindow::newLut() {
  const QString pack = targetPack();
  if (pack.isEmpty())
    return;
  const eng::LutGrade g = eng::LutGrade::basic();
  const QString name = uniqueName(pack, QStringLiteral("Graded"));
  QPointer<LutWindow> self(this);
  put(pack, name, eng::grade::apply(eng::lut::templateStrip(), g), g, [self](const QString &ref) {
    if (!self)
      return;
    self->selectLut(ref);
    self->m_name->setFocus();
    self->m_name->selectAll();
  });
}

void LutWindow::importLut() {
  const QString pack = targetPack();
  if (pack.isEmpty())
    return;
  const QString filters = tr("LUT image (*.png);;Other images (*.webp *.jpg *.jpeg *.tga *.bmp);;"
                             "Adobe / Resolve cube (*.cube);;All LUT files (*.png *.webp *.jpg *.jpeg *.tga *.bmp "
                             "*.cube)");
  const QStringList files = FileDialog::getOpenFileNames(this, tr("Import a LUT"), {}, filters, tr("Import"));
  QPointer<LutWindow> self(this);
  for (const QString &f: files) {
    QString error;
    const auto l = eng::lut::importFile(f, &error);
    if (!l) {
      QMessageBox::warning(this, tr("Import a LUT"), error);
      continue;
    }
    put(pack, uniqueName(pack, QFileInfo(f).completeBaseName()), l->strip, std::nullopt, [self](const QString &ref) {
      if (self)
        self->selectLut(ref);
    });
  }
}

void LutWindow::copyToPack() {
  if (m_ref.isEmpty())
    return;
  const QString pack = targetPack();
  if (pack.isEmpty())
    return;
  upload();
  QJsonObject args;
  args.insert(QStringLiteral("from"), m_ref);
  args.insert(QStringLiteral("pack"), pack);
  args.insert(QStringLiteral("type"), QStringLiteral("lut"));
  args.insert(QStringLiteral("name"), uniqueName(pack, m_entry.name));
  QPointer<LutWindow> self(this);
  net::TextureCatalog::instance().call(QStringLiteral("textures.copy"), args, this, [self](const net::Result &r) {
    if (!self)
      return;
    if (!r.ok) {
      QMessageBox::warning(self, tr("Copy to Pack"), r.message);
      return;
    }
    self->selectLut(r.data.value(QStringLiteral("entry")).toObject().value(QStringLiteral("ref")).toString());
  });
}

void LutWindow::deleteLut() {
  if (!editable())
    return;
  if (QMessageBox::question(this, tr("Delete LUT"), tr("Delete %1 from %2?").arg(m_entry.name, m_entry.pack)) !=
      QMessageBox::Yes)
    return;
  m_dirty = false;
  m_upload->stop();
  QPointer<LutWindow> self(this);
  net::TextureCatalog::instance().call(QStringLiteral("textures.delete"),
                                       {{QStringLiteral("ref"), m_ref}, {QStringLiteral("type"), QStringLiteral("lut")}}, this,
                                       [self](const net::Result &r) {
                                         if (self && !r.ok)
                                           QMessageBox::warning(self, tr("Delete LUT"), r.message);
                                       });
}

void LutWindow::renameLut() {
  const QString name = m_name->text().trimmed();
  if (!editable() || name == m_entry.name)
    return;
  if (!isValidAssetName(name)) {
    m_name->setText(m_entry.name);
    return;
  }
  upload();
  QJsonObject args;
  args.insert(QStringLiteral("ref"), m_ref);
  args.insert(QStringLiteral("name"), name);
  args.insert(QStringLiteral("type"), QStringLiteral("lut"));
  QPointer<LutWindow> self(this);
  net::TextureCatalog::instance().call(QStringLiteral("textures.update"), args, this, [self](const net::Result &r) {
    if (!self)
      return;
    if (!r.ok) {
      QMessageBox::warning(self, tr("Rename LUT"), r.message);
      self->m_name->setText(self->m_entry.name);
      return;
    }
    const QString ref = r.data.value(QStringLiteral("entry")).toObject().value(QStringLiteral("ref")).toString();
    self->m_ref = ref;
    self->m_pendingSelect = ref;
    self->m_refresh->start();
  });
}

void LutWindow::exportLut() {
  if (m_strip.isNull())
    return;
  const QString path = FileDialog::getSaveFileName(this, tr("Export LUT"), m_entry.name + QStringLiteral(".png"),
                                                   tr("LUT strip (*.png);;Adobe / Resolve cube (*.cube)"), tr("Export"));
  if (path.isEmpty())
    return;
  eng::ColorLut l;
  l.name = m_entry.name;
  l.strip = m_strip;
  QString error;
  if (!eng::lut::exportFile(l, path, &error))
    QMessageBox::warning(this, tr("Export LUT"), error);
}

void LutWindow::saveTemplate() {
  const QString path = FileDialog::getSaveFileName(this, tr("Save neutral LUT template"),
                                                    QStringLiteral("lut_template_33.png"), tr("PNG image (*.png)"));
  if (path.isEmpty())
    return;
  eng::ColorLut l;
  l.name = QStringLiteral("Neutral");
  l.strip = eng::lut::templateStrip();
  QString error;
  if (!eng::lut::exportFile(l, path, &error))
    QMessageBox::warning(this, tr("Save template"), error);
}

} // namespace ui
