#include "material_props_panel.h"
#include "client/ui/scale.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QMenu>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/engine/scene.h"
#include "client/net/asset_client.h"
#include "client/net/texture_catalog.h"
#include "client/ui/action_manager.h"
#include "client/ui/texture_actions.h"
#include "client/ui/texture_ui.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"
#include "client/vulkan/vk_shared.h"

namespace ui {

using matprops::Key;
using PropSection = matprops::Section;

namespace {

QColor sourceColor(eng::PropSource s) {
  switch (s) {
    case eng::PropSource::Godot: return QColor(96, 104, 130);
    case eng::PropSource::Default: return QColor(150, 160, 186);
    case eng::PropSource::Server: return QColor(80, 156, 255);
    case eng::PropSource::Map: return QColor(240, 168, 64);
    case eng::PropSource::Variant: return QColor(176, 124, 255);
  }
  return QColor(150, 160, 186);
}

QString sourceText(eng::PropSource s) {
  switch (s) {
    case eng::PropSource::Godot: return QObject::tr("Godot's default");
    case eng::PropSource::Default: return QObject::tr("default for this texture (tags / maps)");
    case eng::PropSource::Server: return QObject::tr("saved on the server (all maps)");
    case eng::PropSource::Map: return QObject::tr("overridden in this map");
    case eng::PropSource::Variant: return QObject::tr("set on this variant");
  }
  return {};
}

const char *kSectionIcon[] = {"image_alpha", "shading_rendered", "material", "brush_data",
                              "light",       "normals_face",     "shading_solid", "mod_displace"};

} // namespace

MaterialPropsPanel::MaterialPropsPanel(vk::Engine3D *engine, Mode mode, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_mode(mode) {
  auto *content = this;
  auto *v = new QVBoxLayout(content);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(6);

  // header
  auto *head = new QHBoxLayout();
  head->setSpacing(8);
  m_title = new QLabel(content);
  m_title->setStyleSheet(QStringLiteral("font-weight: 600;"));
  m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
  m_title->setVisible(mode == Mode::Selection);  // the browser shows its own name
  m_scope = new QLabel(content);
  head->addWidget(m_title, 1);
  head->addWidget(m_scope, 0, Qt::AlignRight);
  if (mode == Mode::Pinned)
    head->insertStretch(0, 1);
  v->addLayout(head);

  m_picker = new QComboBox(content);
  m_picker->setToolTip(tr("The selection uses several materials: pick the one to edit"));
  connect(m_picker, &QComboBox::activated, this, [this] {
    m_pinnedRef = m_picker->currentData().toString();
    refresh();
  });
  v->addWidget(m_picker);

  m_empty = new QLabel(mode == Mode::Selection
                           ? tr("Nothing selected")
                           : tr("No texture selected"),
                       content);
  m_empty->setWordWrap(true);
  m_empty->setAlignment(Qt::AlignCenter);
  m_empty->setStyleSheet(QStringLiteral("color:#8a96b4; padding: 8px 6px;"));
  v->addWidget(m_empty);

  m_body = new QWidget(content);
  auto *bv = new QVBoxLayout(m_body);
  bv->setContentsMargins(0, 0, 0, 0);
  bv->setSpacing(4);
  for (int s = 0; s < matprops::kSectionCount; ++s) {
    auto *sec = new ui::Section(QLatin1String(matprops::sectionName(PropSection(s))), QLatin1String(kSectionIcon[s]), m_body);
    sec->setSubsection();
    m_sections.insert(s, sec);
    bv->addWidget(sec);
  }
  for (const auto &d: matprops::defs())
    buildRow(m_sections.value(int(d.section)), d.key);

  // legend
  auto *legend = new QLabel(m_body);
  legend->setTextFormat(Qt::RichText);
  legend->setWordWrap(true);
  legend->setStyleSheet(QStringLiteral("color:#7d88a6; padding: 4px 2px;"));
  scale::setRole(legend, scale::Font::Caption);
  legend->setText(tr("<span style='color:#96a0ba'>●</span> default &nbsp; <span style='color:#509cff'>●</span> server "
                     "&nbsp; <span style='color:#f0a840'>●</span> this map &nbsp; <span style='color:#b07cff'>●</span> "
                     "variant"));
  bv->addWidget(legend);

  // actions
  auto *actions = new QGridLayout();
  actions->setSpacing(6);
  m_save = new QPushButton(ActionManager::icon(QStringLiteral("internet")), tr("Save to server"), m_body);
  m_save->setToolTip(tr("Make this map's values the server's values for this texture: every map that uses it "
                        "(and every connected editor) gets them"));
  connect(m_save, &QPushButton::clicked, this, [this] { saveToServer(false); });
  m_revert = new QPushButton(ActionManager::icon(QStringLiteral("loop_back")), tr("Revert"), m_body);
  m_revert->setToolTip(tr("Drop this map's overrides (or the variant's values)"));
  connect(m_revert, &QPushButton::clicked, this, [this] {
    if (m_engine)
      m_engine->revertMaterialOverrides(m_ref);
  });
  m_variant = new QPushButton(ActionManager::icon(QStringLiteral("duplicate")), tr("Variant for selection…"), m_body);
  m_variant->setToolTip(tr("Give the selected faces their own copy of this material to tune (e.g. two shinier "
                           "walls). The texture stays the same; only the properties differ."));
  connect(m_variant, &QPushButton::clicked, this, &MaterialPropsPanel::startVariant);
  m_merge = new QPushButton(tr("Merge into map"), m_body);
  m_merge->setToolTip(tr("Apply this variant's values to every face of the base material in this map, then remove "
                         "the variant"));
  connect(m_merge, &QPushButton::clicked, this, [this] {
    if (m_engine)
      m_engine->mergeVariantIntoMap(m_ref);
  });
  m_deleteVariant = new QPushButton(ActionManager::icon(QStringLiteral("x")), tr("Delete variant"), m_body);
  m_deleteVariant->setToolTip(tr("Its faces go back to the base material"));
  connect(m_deleteVariant, &QPushButton::clicked, this, [this] {
    if (m_engine)
      m_engine->deleteVariant(m_ref);
  });
  actions->addWidget(m_save, 0, 0);
  actions->addWidget(m_revert, 0, 1);
  actions->addWidget(m_merge, 1, 0);
  actions->addWidget(m_deleteVariant, 1, 1);
  // Copy into pack (base packs) / edit in place (user packs): inspector only.
  m_copy = new QPushButton(ActionManager::icon(QStringLiteral("duplicate")), tr("Copy into pack…"), m_body);
  m_copy->setToolTip(tr("Base textures are read-only: copy this one into one of your packs to edit its maps"));
  connect(m_copy, &QPushButton::clicked, this, [this] {
    emit TextureHub::instance().copyRequested(MaterialRef::parse(m_ref).base());
  });
  m_editMap = new QPushButton(ActionManager::icon(QStringLiteral("greasepencil")), tr("Edit map"), m_body);
  m_editMap->setToolTip(tr("Open one of this texture's maps in the image editor; saving uploads it"));
  auto *editMenu = new QMenu(m_editMap);
  connect(editMenu, &QMenu::aboutToShow, this, [this, editMenu] {
    editMenu->clear();
    if (const TextureEntry *e = net::TextureCatalog::instance().find(MaterialRef::parse(m_ref).base()))
      addEditMapActions(editMenu, *e, this);
  });
  m_editMap->setMenu(editMenu);
  auto *ownRow = new QHBoxLayout();
  ownRow->setContentsMargins(0, 0, 0, 0);
  ownRow->addWidget(m_copy);
  ownRow->addWidget(m_editMap);
  actions->addWidget(m_variant, 2, 0);
  actions->addLayout(ownRow, 2, 1);
  bv->addLayout(actions);

  m_variantForm = new QWidget(m_body);
  auto *vf = new QHBoxLayout(m_variantForm);
  vf->setContentsMargins(0, 0, 0, 0);
  m_variantName = new QLineEdit(m_variantForm);
  m_variantName->setPlaceholderText(tr("variant name, e.g. shiny"));
  auto *ok = new QPushButton(tr("Create"), m_variantForm);
  auto *cancel = new QToolButton(m_variantForm);
  cancel->setIcon(ActionManager::icon(QStringLiteral("x")));
  vf->addWidget(m_variantName, 1);
  vf->addWidget(ok);
  vf->addWidget(cancel);
  m_variantForm->hide();
  auto create = [this] {
    if (!m_engine)
      return;
    const QString name = m_variantName->text().trimmed().isEmpty() ? QStringLiteral("variant") : m_variantName->text();
    const QString vref = m_engine->createVariantForSelection(m_ref, name);
    m_variantForm->hide();
    m_pinnedRef = vref;
    scheduleRefresh();
  };
  connect(ok, &QPushButton::clicked, this, create);
  connect(m_variantName, &QLineEdit::returnPressed, this, create);
  connect(cancel, &QToolButton::clicked, m_variantForm, &QWidget::hide);
  bv->addWidget(m_variantForm);

  m_status = new QLabel(m_body);
  m_status->setWordWrap(true);
  m_status->setStyleSheet(QStringLiteral("color:#8a96b4;"));
  scale::setRole(m_status, scale::Font::Caption);
  bv->addWidget(m_status);
  v->addWidget(m_body);

  m_refreshTimer = new QTimer(this);
  m_refreshTimer->setSingleShot(true);
  m_refreshTimer->setInterval(40);
  connect(m_refreshTimer, &QTimer::timeout, this, &MaterialPropsPanel::refresh);
  if (m_engine) {
    connect(m_engine, &vk::Engine3D::selectionChanged, this, [this] {
      if (m_mode == Mode::Selection)
        m_pinnedRef.clear();
      scheduleRefresh();
    });
    connect(m_engine, &vk::Engine3D::objectStateChanged, this, &MaterialPropsPanel::scheduleRefresh);
    connect(m_engine, &vk::Engine3D::materialOverridesChanged, this, &MaterialPropsPanel::scheduleRefresh);
    connect(m_engine, &vk::Engine3D::currentMaterialChanged, this, &MaterialPropsPanel::scheduleRefresh);
  }
  auto &cat = net::TextureCatalog::instance();
  connect(&cat, &net::TextureCatalog::entryChanged, this, &MaterialPropsPanel::scheduleRefresh);
  connect(&net::AssetClient::instance(), &net::AssetClient::stateChanged, this, &MaterialPropsPanel::scheduleRefresh);
  refresh();
}

void MaterialPropsPanel::buildRow(ui::Section *section, Key key) {
  const matprops::PropDef &d = matprops::def(key);
  Row row;
  row.key = key;
  auto *holder = new QWidget(section);
  auto *h = new QHBoxLayout(holder);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(4);
  row.dot = new ResetDot(holder);
  connect(row.dot, &QToolButton::clicked, this, [this, key] {
    if (m_engine)
      m_engine->resetMaterialProperty(m_ref, key);
  });
  switch (d.type) {
    case matprops::Type::Float: {
      row.slider = new SliderField(d.min, d.max, 2, QString(), 1.0, holder);
      connect(row.slider, &SliderField::committed, this, [this, key](double v) { commit(key, v); });
      row.editor = row.slider;
      break;
    }
    case matprops::Type::Bool: {
      row.check = new QCheckBox(holder);
      connect(row.check, &QCheckBox::clicked, this, [this, key](bool on) { commit(key, on); });
      row.editor = row.check;
      break;
    }
    case matprops::Type::Channel: {
      row.combo = new QComboBox(holder);
      for (const char *n: {"red", "green", "blue", "alpha", "grey"})
        row.combo->addItem(QString::fromLatin1(n).replace(0, 1, QString::fromLatin1(n).left(1).toUpper()),
                           QString::fromLatin1(n));
      connect(row.combo, &QComboBox::activated, this,
              [this, key, combo = row.combo] { commit(key, combo->currentData().toString()); });
      row.editor = row.combo;
      break;
    }
    case matprops::Type::Shading: {
      row.combo = new QComboBox(holder);
      row.combo->addItem(tr("Per pixel"), QStringLiteral("per_pixel"));
      row.combo->addItem(tr("Unshaded"), QStringLiteral("unshaded"));
      connect(row.combo, &QComboBox::activated, this,
              [this, key, combo = row.combo] { commit(key, combo->currentData().toString()); });
      row.editor = row.combo;
      break;
    }
    case matprops::Type::Color: {
      row.color = new ColorButton(holder);
      connect(row.color, &ColorButton::colorPicked, this, [this, key](const QColor &c) { commit(key, c); });
      row.editor = row.color;
      break;
    }
  }
  h->addWidget(row.editor, 1);
  h->addWidget(row.dot);
  section->addRow(QLatin1String(d.label), holder, QLatin1String(d.tip));
  m_rows.insert(int(key), row);
}

void MaterialPropsPanel::showMaterial(const QString &ref) {
  m_pinnedRef = ref;
  refresh();
}

void MaterialPropsPanel::scheduleRefresh() { m_refreshTimer->start(); }

QString MaterialPropsPanel::activeRef() const {
  if (!m_engine)
    return {};
  if (m_mode == Mode::Pinned)
    return m_pinnedRef;
  const QStringList refs = m_engine->selectionMaterialRefs();
  if (!m_pinnedRef.isEmpty()) {
    // A pinned ref stays while it is part of the selection (or nothing is selected).
    for (const auto &r: refs)
      if (r.compare(m_pinnedRef, Qt::CaseInsensitive) == 0)
        return r;
    if (refs.isEmpty())
      return m_pinnedRef;
  }
  if (!refs.isEmpty())
    return refs.first();
  return m_engine->currentMaterial();
}

void MaterialPropsPanel::refresh() {
  m_ref = activeRef();
  // Pinned: only look it up, so browsing does not download every texture clicked (the catalogue entry suffices).
  auto &tm = vk::TextureManager::instance();
  eng::MaterialPtr material;
  const TextureEntry *entry = nullptr;
  if (m_engine && !m_ref.isEmpty() && m_engine->scene()) {
    material = m_mode == Mode::Selection ? tm.materialFor(m_ref) : tm.find(m_ref);
    if (material && material->hasEntry())
      entry = &material->entry;
    else if (!material)
      entry = net::TextureCatalog::instance().find(MaterialRef::parse(m_ref).base());
  }
  const bool has = material || entry;
  m_empty->setVisible(!has);
  m_body->setVisible(has);
  // picker: the selection uses several materials
  const QStringList refs = m_engine && m_mode == Mode::Selection ? m_engine->selectionMaterialRefs() : QStringList();
  {
    QSignalBlocker b(m_picker);
    m_picker->clear();
    for (const auto &r: refs)
      m_picker->addItem(r, r);
    m_picker->setCurrentIndex(std::max(0, m_picker->findData(m_ref)));
  }
  m_picker->setVisible(refs.size() > 1);
  refreshHeader();
  if (!has)
    return;

  const eng::MaterialOverrides &ov = m_engine->scene()->materialOverrides;
  const eng::ResolvedProps rp = material ? tm.resolvedFor(*material) : ov.resolve(m_ref, entry);
  const bool isVariant = MaterialRef::parse(m_ref).isVariant();
  const eng::PropSource ownLayer = isVariant ? eng::PropSource::Variant : eng::PropSource::Map;

  for (auto it = m_rows.begin(); it != m_rows.end(); ++it) {
    Row &row = it.value();
    const QVariant v = rp.props.value(row.key);
    if (row.slider)
      row.slider->setValueQuiet(v.toDouble());
    if (row.check) {
      QSignalBlocker b(row.check);
      row.check->setChecked(v.toBool());
    }
    if (row.combo) {
      QSignalBlocker b(row.combo);
      row.combo->setCurrentIndex(std::max(0, row.combo->findData(v.toString())));
    }
    if (row.color)
      row.color->setColor(v.value<QColor>());
    const eng::PropSource src = rp.source[size_t(row.key)];
    const bool resettable = src == ownLayer;
    row.dot->setState(sourceColor(src), resettable,
                      resettable ? tr("%1 — click to reset to the inherited value").arg(sourceText(src))
                                 : sourceText(src),
                      !resettable && src == eng::PropSource::Godot);
  }
  // A new material opens only the sub-sections with non-default values.
  if (m_ref.compare(m_expandedFor, Qt::CaseInsensitive) != 0) {
    m_expandedFor = m_ref;
    for (auto it = m_sections.begin(); it != m_sections.end(); ++it) {
      bool set = false;
      for (const auto &d: matprops::defs())
        if (int(d.section) == it.key() && rp.source[size_t(d.key)] != eng::PropSource::Godot)
          set = true;
      it.value()->setExpanded(set);
    }
  }
  // Height only makes sense with a height map; emission colour/energy only when enabled.
  const bool server = material ? material->kind == eng::Material::Kind::Server : entry != nullptr;
  const bool height = entry && entry->hasHeight();
  const bool ormMap = material ? material->hasMetallicRoughnessMap() : entry && entry->layout() != MapLayout::None;
  m_sections.value(int(PropSection::Height))->setVisible(height);
  m_rows[int(Key::EmissionEnergy)].editor->setEnabled(rp.props.emissionEnabled);
  m_rows[int(Key::EmissionColor)].editor->setEnabled(rp.props.emissionEnabled);
  for (Key k: {Key::MetallicChannel, Key::RoughnessChannel, Key::AoChannel})
    m_rows[int(k)].editor->setEnabled(ormMap || !server);

  // actions
  const bool mapLayer = ov.mapLayer(m_ref) && !ov.mapLayer(m_ref)->isEmpty();
  const bool variantLayer = isVariant && ov.variant(m_ref) && !ov.variant(m_ref)->props.isEmpty();
  const bool online = net::AssetClient::instance().isOnline();
  m_save->setVisible(!isVariant);
  m_save->setEnabled(!isVariant && mapLayer && online && entry);
  m_save->setToolTip(!entry    ? tr("Only server textures can be saved to the server")
                     : !online ? tr("The asset server is offline")
                     : !mapLayer
                         ? tr("Nothing to save: this map has no overrides for this texture")
                         : tr("Make this map's values the server's values for this texture: every map that uses it "
                              "(and every connected editor) gets them"));
  m_revert->setEnabled(isVariant ? variantLayer : mapLayer);
  m_merge->setVisible(isVariant);
  m_deleteVariant->setVisible(isVariant);
  // Server textures only.
  const bool own = m_mode == Mode::Selection && entry;
  m_copy->setVisible(own && entry->base);
  m_editMap->setVisible(own && !entry->base);
  m_editMap->setEnabled(own && online && !entry->maps.isEmpty());
  const int selFaces = m_engine->facesUsingMaterial(m_ref, true);
  m_variant->setEnabled(selFaces > 0 && server);
  m_variant->setToolTip(selFaces > 0 ? tr("Give the selected faces their own copy of this material's properties")
                                     : tr("No faces selected"));

  QStringList status;
  const int mapFaces = m_engine->facesUsingMaterial(m_ref, false);
  status << (mapFaces ? tr("%n face(s) in this map", nullptr, mapFaces) : tr("not used in this map"));
  if (selFaces)
    status << tr("%n selected", nullptr, selFaces);
  if (material && material->state == eng::Material::State::Error)
    status << tr("<span style='color:#e070c0'>texture missing or server unreachable</span>");
  if (isVariant)
    status << tr("variant of %1").arg(MaterialRef::parse(m_ref).base().toHtmlEscaped());
  m_status->setText(status.join(QStringLiteral(" · ")));
}

void MaterialPropsPanel::refreshHeader() {
  if (m_ref.isEmpty()) {
    m_title->clear();
    m_scope->clear();
    return;
  }
  const auto material = vk::TextureManager::instance().find(m_ref);
  const TextureEntry *entry = material && material->hasEntry()
                                  ? &material->entry
                                  : net::TextureCatalog::instance().find(MaterialRef::parse(m_ref).base());
  const MaterialRef r = MaterialRef::parse(m_ref);
  m_title->setText(r.isVariant() ? QStringLiteral("%1 · %2").arg(r.name, r.variant) : r.name);
  m_title->setToolTip(r.isLegacy() ? tr("local / built-in texture") : m_ref);

  // scope pill
  QString scope;
  QColor color;
  if (r.isVariant()) {
    scope = tr("Variant “%1”").arg(r.variant);
    color = QColor(176, 124, 255);
  } else if (m_engine && m_engine->scene() && m_engine->scene()->materialOverrides.mapLayer(m_ref)) {
    scope = tr("Overridden in this map");
    color = QColor(240, 168, 64);
  } else if (entry && !entry->props.isEmpty()) {
    scope = tr("Server values");
    color = QColor(80, 156, 255);
  } else {
    scope = tr("Defaults");
    color = QColor(150, 160, 186);
  }
  m_scope->setText(scope);
  m_scope->setStyleSheet(QStringLiteral("color: %1; border: 1px solid %1; border-radius: 8px; padding: 1px 8px;")
                             .arg(color.name()));
}

void MaterialPropsPanel::commit(Key key, const QVariant &value) {
  if (m_engine && !m_ref.isEmpty())
    m_engine->setMaterialProperty(m_ref, key, value);
}

void MaterialPropsPanel::saveToServer(bool force) {
  if (!m_engine)
    return;
  m_save->setEnabled(false);
  m_status->setText(tr("Saving to the server…"));
  QPointer<MaterialPropsPanel> self(this);
  const QString ref = m_ref;
  m_engine->saveMaterialPropsToServer(ref, force, [self, ref](bool ok, const QString &message, const QJsonObject &current) {
    if (!self)
      return;
    if (ok) {
      self->m_status->setText(tr("Saved to the server"));
      self->scheduleRefresh();
      return;
    }
    if (message != QLatin1String("conflict")) {
      QMessageBox::warning(self, tr("Save to server"), tr("Could not save: %1").arg(message));
      self->scheduleRefresh();
      return;
    }
    // Save conflict: someone else saved meanwhile; show both.
    const TextureEntry theirs = TextureEntry::fromJson(current);
    const auto material = vk::TextureManager::instance().materialFor(ref);
    MaterialProps mine = material->entry.props;
    if (const MaterialProps *layer = self->m_engine->scene()->materialOverrides.mapLayer(ref))
      mine.overlay(*layer);
    QStringList diff;
    for (const auto &d: matprops::defs()) {
      const QVariant a = theirs.props.isSet(d.key) ? theirs.props.value(d.key) : QVariant();
      const QVariant b = mine.isSet(d.key) ? mine.value(d.key) : QVariant();
      if (a != b)
        diff << QStringLiteral("%1: theirs %2, yours %3")
                    .arg(QLatin1String(d.label), a.isValid() ? a.toString() : QStringLiteral("—"),
                         b.isValid() ? b.toString() : QStringLiteral("—"));
    }
    QMessageBox box(QMessageBox::Question, tr("Changed by someone else"),
                    tr("Another editor saved properties for %1 after you loaded them.").arg(ref), QMessageBox::NoButton,
                    self);
    box.setDetailedText(diff.join(QLatin1Char('\n')));
    box.setInformativeText(tr("Keep theirs (your overrides stay in this map), or save yours on top of theirs "
                              "(their values, with the keys you changed replaced)?"));
    QPushButton *keep = box.addButton(tr("Keep theirs"), QMessageBox::RejectRole);
    QPushButton *merge = box.addButton(tr("Save mine on top"), QMessageBox::AcceptRole);
    box.setDefaultButton(merge);
    box.exec();
    if (box.clickedButton() != merge) {
      Q_UNUSED(keep);
      self->scheduleRefresh();
      return;
    }
    // Their values are in the catalogue (the conflict reply carried them): save theirs + this map's layer.
    self->saveToServer(true);
  });
}

void MaterialPropsPanel::startVariant() {
  m_variantName->setText(QStringLiteral("variant"));
  m_variantForm->show();
  m_variantName->setFocus();
  m_variantName->selectAll();
}

} // namespace ui
