#include "inspector.h"
#include "client/ui/scale.h"

#include <cmath>

#include <QCheckBox>
#include <QSignalBlocker>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/engine/material.h"
#include "client/engine/texture_library.h"
#include "client/net/file_fetcher.h"
#include "client/engine/scene.h"
#include "client/ui/texture_actions.h"
#include "client/ui/texture_ui.h"
#include "client/ui/action_manager.h"
#include "client/ui/face_groups_panel.h"
#include "client/ui/stairs_panel.h"
#include "client/ui/replace_texture_dialog.h"
#include "client/ui/stairs_texture_panel.h"
#include "client/engine/map_format.h"
#include "client/ui/arch_panel.h"
#include "client/ui/shape_panel.h"
#include "client/ui/bevel_panel.h"
#include "client/ui/cut_panel.h"
#include "client/ui/mirror_panel.h"
#include "client/ui/modifier_panel.h"
#include "client/ui/path_panel.h"
#include "client/ui/entity_editor.h"
#include "client/ui/io_editor.h"
#include "client/ui/io_panel.h"
#include "client/engine/entity.h"
#include "client/ui/widgets.h"
#include "client/ui/lut_window.h"
#include "client/ui/texture_browser.h"
#include "client/net/asset_client.h"
#include "client/net/texture_catalog.h"
#include "client/engine/sky_info.h"
#include "client/ui/material_props_panel.h"
#include "client/prefs/preferences.h"
#include "client/vulkan/vk_engine.h"
#include "client/vulkan/vk_shared.h"

namespace ui {

namespace {

constexpr double kSrc = vk::Engine3D::kSourceUnitsPerWorldUnit;

QToolButton *smallButton(const QString &icon, const QString &tip, QWidget *parent, const QString &text = {}) {
  auto *b = new QToolButton(parent);
  if (!icon.isEmpty())
    b->setIcon(ActionManager::icon(icon));
  if (!text.isEmpty()) {
    b->setText(text);
    b->setToolButtonStyle(icon.isEmpty() ? Qt::ToolButtonTextOnly : Qt::ToolButtonTextBesideIcon);
  }
  b->setToolTip(tip);
  b->setAutoRaise(false);
  return b;
}

QString thumbPathFor(const QString &material) {
  eng::TextureLibrary::Entry e;
  if (eng::TextureLibrary::instance().find(material, e))
    return e.albedo;
  const auto mat = vk::TextureManager::instance().materialFor(material);
  if (mat && mat->hasEntry()) {
    // Server textures: from the download cache (fetched for the next refresh).
    const QString thumb = mat->entry.thumbFile();
    if (const QString p = net::FileFetcher::instance().cachedPath(net::FileKind::Thumb, thumb); !p.isEmpty())
      return p;
    net::FileFetcher::instance().fetch(net::FileKind::Thumb, thumb, net::FetchPriority::Visible, nullptr, {});
    return {};
  }
  if (mat && !mat->libAlbedo.isEmpty() && !mat->libAlbedo.startsWith(QLatin1Char(':')))
    return mat->libAlbedo;
  return {};
}

} // namespace

Inspector::Inspector(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto *scroll = new QScrollArea(this);
  m_scroll = scroll;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_body = new QWidget(scroll);
  auto *v = new QVBoxLayout(m_body);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(2);
  scroll->setWidget(m_body);
  outer->addWidget(scroll);

  buildHeader();
  buildTransform();
  buildTexture();
  m_entity = new Section(tr("Entity"), QStringLiteral("empty_data"), m_body);
  m_entityEditor = new EntityEditor(m_engine, m_entity);
  m_entity->body()->addWidget(m_entityEditor);
  m_body->layout()->addWidget(m_entity);
  // I/O: a summary; editing is in the I/O Editor.
  m_io = new Section(tr("I/O"), QStringLiteral("decorate_linked"), m_body);
  m_ioSummary = new IOSummary(m_engine, m_io);
  m_io->body()->addWidget(m_ioSummary);
  m_body->layout()->addWidget(m_io);
  connect(m_ioSummary, &IOSummary::openEditor, this, [this](quint32 entity, int row, const QString &add) {
    IOEditor::open(m_engine, window(), entity, row, add);
  });
  m_mirror = new Section(tr("Mirror"), QStringLiteral("mod_mirror"), m_body);
  m_mirror->body()->addWidget(new MirrorPanel(m_engine, m_mirror));
  static_cast<QBoxLayout *>(m_body->layout())->insertWidget(1, m_mirror); // under the header
  m_cut = new Section(tr("Cut"), QStringLiteral("cursor_blade"), m_body);
  m_cut->body()->addWidget(new CutPanel(m_engine, m_cut));
  m_body->layout()->addWidget(m_cut);
  m_bevel = new Section(tr("Bevel"), QStringLiteral("edge_bevel"), m_body);
  m_bevel->body()->addWidget(new BevelPanel(m_engine, m_bevel));
  static_cast<QBoxLayout *>(m_body->layout())->insertWidget(1, m_bevel); // under the header
  // Placed on every refresh (arrangeSections).
  m_modifiers = new Section(tr("Modifiers"), QStringLiteral("modifier"), m_body);
  m_modifiers->body()->addWidget(new ModifierPanel(m_engine, m_modifiers));
  m_body->layout()->addWidget(m_modifiers);
  m_pathSection = new Section(tr("Path"), QStringLiteral("curve_bezcurve"), m_body);
  m_pathPanel = new PathPanel(m_engine, m_pathSection);
  m_pathSection->body()->addWidget(m_pathPanel);
  m_body->layout()->addWidget(m_pathSection);

  m_stairs = new Section(tr("Stairs"), QStringLiteral("mod_array"), m_body);
  m_stairsPanel = new StairsPanel(m_engine, m_stairs);
  m_stairs->body()->addWidget(m_stairsPanel);
  m_body->layout()->addWidget(m_stairs);
  m_arch = new Section(tr("Arch"), QStringLiteral("mod_bevel"), m_body);
  m_archPanel = new ArchPanel(m_engine, m_arch);
  m_arch->body()->addWidget(m_archPanel);
  m_body->layout()->addWidget(m_arch);
  m_shape = new Section(tr("Shape"), QStringLiteral("mesh_cylinder"), m_body);
  m_shapePanel = new ShapePanel(m_engine, m_shape);
  m_shape->body()->addWidget(m_shapePanel);
  m_body->layout()->addWidget(m_shape);
  m_faceGroups = new Section(tr("Face groups"), QStringLiteral("face_maps"), m_body);
  m_faceGroups->body()->addWidget(new FaceGroupsPanel(m_engine, m_faceGroups));
  m_body->layout()->addWidget(m_faceGroups);
  m_replaceRow = new QWidget(m_body);
  {
    auto *h = new QHBoxLayout(m_replaceRow);
    h->setContentsMargins(8, 6, 8, 6);
    auto *replace = new QPushButton(ActionManager::icon(QStringLiteral("file_refresh")), tr("Replace texture…"),
                                    m_replaceRow);
    replace->setToolTip(tr("Pick another texture for every face using this one (or only the selection), with a "
                           "live preview"));
    h->addWidget(replace, 1);
    connect(replace, &QPushButton::clicked, this, [this] { ReplaceTextureWindow::open(m_engine, window()); });
  }
  m_body->layout()->addWidget(m_replaceRow);
  buildWorld();
  v->addStretch(1);

  m_refreshTimer = new QTimer(this);
  m_refreshTimer->setSingleShot(true);
  m_refreshTimer->setInterval(40);
  connect(m_refreshTimer, &QTimer::timeout, this, &Inspector::refresh);
  connect(&prefs::get(), &prefs::Preferences::changed, this, &Inspector::scheduleRefresh);

  if (m_engine) {
    for (auto sig: {&vk::Engine3D::selectionChanged, &vk::Engine3D::sceneChanged, &vk::Engine3D::uvTransformChanged,
                    &vk::Engine3D::sunChanged, &vk::Engine3D::objectStateChanged,
                    &vk::Engine3D::textureLockChanged, &vk::Engine3D::textureScaleLockChanged,
                    &vk::Engine3D::renderModeChanged, &vk::Engine3D::undoRedoChanged, &vk::Engine3D::fillChanged,
                    &vk::Engine3D::entitiesChanged, &vk::Engine3D::lutsChanged, &vk::Engine3D::autoSelectChanged,
                    &vk::Engine3D::faceGroupsChanged, &vk::Engine3D::procObjectsChanged,
                    &vk::Engine3D::cutModeChanged, &vk::Engine3D::shapeToolChanged, &vk::Engine3D::mirrorChanged,
                    &vk::Engine3D::materialOverridesChanged, &vk::Engine3D::modHostsChanged,
                    &vk::Engine3D::editSourceChanged, &vk::Engine3D::pathsChanged, &vk::Engine3D::skyChanged,
                    &vk::Engine3D::physicsChanged, &vk::Engine3D::physicsSimChanged,
                    &vk::Engine3D::bevelChanged})
      connect(m_engine, sig, this, &Inspector::scheduleRefresh);
  }
  refresh();
}

void Inspector::arrangeSections(const bool materialMode, const bool hasModifiers) {
  // Moves only what is out of place, so a refresh does not churn the layout.
  auto *body = static_cast<QBoxLayout *>(m_body->layout());
  auto placeBefore = [body](QWidget *w, QWidget *anchor) {
    if (body->indexOf(w) + 1 == body->indexOf(anchor))
      return;
    body->removeWidget(w);
    body->insertWidget(body->indexOf(anchor), w);
  };
  auto placeAfter = [body](QWidget *w, QWidget *anchor) {
    if (body->indexOf(w) == body->indexOf(anchor) + 1)
      return;
    body->removeWidget(w);
    body->insertWidget(body->indexOf(anchor) + 1, w);
  };
  if (materialMode)
    placeAfter(m_modifiers, m_texture);
  else if (hasModifiers)
    placeBefore(m_modifiers, m_transform);
  else
    placeAfter(m_modifiers, m_transform);
  // Stairs / arch panels go first.
  QWidget *first = !materialMode && hasModifiers ? static_cast<QWidget *>(m_modifiers) : m_transform;
  placeBefore(m_stairs, first);
  placeBefore(m_arch, m_stairs);
  placeBefore(m_shape, m_arch);
}

void Inspector::scheduleRefresh() {
  if (!m_refreshTimer->isActive())
    m_refreshTimer->start();
}

void Inspector::buildHeader() {
  auto *head = new QWidget(m_body);
  auto *h = new QHBoxLayout(head);
  h->setContentsMargins(10, 8, 10, 8);
  m_icon = new QLabel(head);
  scale::bind(m_icon, [this] { m_icon->setFixedSize(scale::px(36), scale::px(36)); });
  auto *texts = new QVBoxLayout();
  texts->setSpacing(0);
  m_title = new QLabel(head);
  scale::setRole(m_title, scale::Font::Title);
  m_subtitle = new QLabel(head);
  m_subtitle->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  texts->addWidget(m_title);
  texts->addWidget(m_subtitle);
  h->addWidget(m_icon);
  h->addLayout(texts, 1);
  m_body->layout()->addWidget(head);
}

void Inspector::buildTransform() {
  m_transform = new Section(tr("Transform"), QStringLiteral("orientation_global"), m_body);
  m_pos = new AxisRow(tr("Position"), tr("u"), 1, 16.0, m_transform);
  m_pos->setToolTip(tr("Centre of the selection's bounds, in Source units"));
  m_rotation = new AxisRow(tr("Rotation"), QStringLiteral("°"), 1, 15.0, m_transform);
  m_rotation->setToolTip(tr("The brushes' rotation (Y X Z order), saved with the map"));
  m_rotation->setSliderRange(-180.0, 180.0);
  m_rotation->setReset(tr("Reset the rotation to 0"));
  m_size = new AxisRow(tr("Size"), tr("u"), 1, 16.0, m_transform);
  m_size->setToolTip(tr("Size of the selection's bounds, in Source units"));
  {
    const int row = m_transform->grid()->rowCount();
    m_transform->grid()->addWidget(m_pos, row, 0, 1, 2);
    m_transform->grid()->addWidget(m_rotation, row + 1, 0, 1, 2);
    m_transform->grid()->addWidget(m_size, row + 2, 0, 1, 2);
  }

  auto *nudge = new QWidget(m_transform);
  auto *nh = new QHBoxLayout(nudge);
  nh->setContentsMargins(0, 0, 0, 0);
  nh->setSpacing(3);
  static const char *axes[3] = {"X", "Y", "Z"};
  for (int a = 0; a < 3; ++a) {
    for (int dir: {-1, 1}) {
      auto *b = smallButton({}, tr("Nudge %1 by one grid step").arg(QLatin1String(axes[a])), nudge,
                            QStringLiteral("%1%2").arg(dir < 0 ? QStringLiteral("−") : QStringLiteral("+"),
                                                      QLatin1String(axes[a])));
      connect(b, &QToolButton::clicked, this, [this, a, dir] { m_engine->nudgeSelection(a, dir); });
      nh->addWidget(b);
    }
  }
  nh->addStretch(1);
  m_transform->addRow(tr("Nudge"), nudge);
  // Faces meeting at a shallow angle shade as one surface, here and in the Godot export.
  m_smoothShading = new QCheckBox(tr("Smooth shading"), m_transform);
  m_smoothShading->setTristate(false);
  m_smoothShading->setToolTip(tr("Shade faces that meet at under 30° as one smooth surface"));
  connect(m_smoothShading, &QCheckBox::clicked, this, [this](bool on) { m_engine->setSelectionSmoothShading(on); });
  m_transform->addRow(QString(), m_smoothShading);
  // Apply Simulated Transform.
  {
    m_physApplyRow = new QWidget(m_transform);
    auto *row = new QHBoxLayout(m_physApplyRow);
    row->setContentsMargins(0, 0, 0, 0);
    auto *apply = new QPushButton(ActionManager::icon(QStringLiteral("checkmark")), tr("Apply Simulated Transform"),
                                  m_physApplyRow);
    apply->setToolTip(tr("Keep the simulated position and rotation"));
    connect(apply, &QPushButton::clicked, this, [this] { m_engine->physicsApply(); });
    row->addWidget(apply, 1);
    const int r = m_transform->grid()->rowCount();
    m_transform->grid()->addWidget(m_physApplyRow, r, 0, 1, 2);
    m_physApplyRow->hide();
  }
  m_body->layout()->addWidget(m_transform);

  connect(m_rotation, &AxisRow::committed, this, [this](int axis, double v) {
    // Typed values wrap into (-180, 180].
    double d = std::fmod(v, 360.0);
    if (d <= -180.0)
      d += 360.0;
    else if (d > 180.0)
      d -= 360.0;
    m_engine->setSelectionRotation(axis, d);
  });
  connect(m_rotation, &AxisRow::resetRequested, this, [this] { m_engine->resetSelectionRotation(); });
  connect(m_pos, &AxisRow::committed, this, [this](int axis, double v) {
    glm::vec3 mn, mx;
    if (!m_engine->selectionBounds(mn, mx))
      return;
    const glm::vec3 c = (mn + mx) * 0.5f;
    glm::vec3 d(0.0f);
    d[axis] = float(v / kSrc) - c[axis];
    m_engine->translateSelection(d, tr("Move"));
  });
  connect(m_size, &AxisRow::committed, this, [this](int axis, double v) {
    glm::vec3 mn, mx;
    if (!m_engine->selectionBounds(mn, mx) || v <= 0.0)
      return;
    const float c = (mn[axis] + mx[axis]) * 0.5f;
    const float half = float(v / kSrc) * 0.5f;
    mn[axis] = c - half;
    mx[axis] = c + half;
    m_engine->setSelectionBounds(mn, mx);
  });
}

namespace {

// Zero-width spaces after '/', '_' and '-' so a long ref wraps.
QString breakable(const QString &ref) {
  QString out;
  out.reserve(ref.size() + 8);
  for (const QChar c: ref) {
    out.append(c);
    if (c == QLatin1Char('/') || c == QLatin1Char('_') || c == QLatin1Char('-'))
      out.append(QChar(0x200B));
  }
  return out;
}

} // namespace

void Inspector::buildTexture() {
  m_texture = new Section(tr("Positioning"), QStringLiteral("texture"), m_body);

  auto *matRow = new QWidget(m_texture);
  auto *mh = new QHBoxLayout(matRow);
  mh->setContentsMargins(0, 0, 0, 0);
  m_matThumb = new QLabel(matRow);
  scale::bind(m_matThumb, [this] { m_matThumb->setFixedSize(scale::px(64), scale::px(64)); });
  m_matThumb->setStyleSheet(QStringLiteral("background:#2a3150; border-radius:4px;"));
  m_matThumb->setAlignment(Qt::AlignCenter);
  auto *mv = new QVBoxLayout();
  m_matName = new QLabel(matRow);
  m_matName->setWordWrap(true);
  m_matName->setTextInteractionFlags(Qt::TextSelectableByMouse);
  // A long material name must not widen the dock: the label wraps in the width it gets.
  {
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    m_matName->setSizePolicy(policy);
    m_matName->setMinimumWidth(1);
  }
  auto *mb = new QHBoxLayout();
  auto *browse = smallButton(QStringLiteral("texture"), tr("Find this material in the browser"), matRow, tr("Browse"));
  auto *applyCurrent = smallButton(QStringLiteral("brush_data"), tr("Apply the browser's current material"),
                                   matRow, tr("Apply current"));
  auto *pick = smallButton(QStringLiteral("eyedropper"), tr("Make this the current material"), matRow);
  mb->addWidget(browse);
  mb->addWidget(applyCurrent);
  mb->addWidget(pick);
  connect(&ui::ThumbCache::instance(), &ui::ThumbCache::ready, this, [this](const QString &) {
    if (!m_lastThumbFor.isEmpty() && m_matThumb->pixmap().isNull()) {
      m_lastThumbFor.clear();
      scheduleRefresh();
    }
  });
  mb->addStretch(1);
  mv->addWidget(m_matName);
  mv->addLayout(mb);
  mh->addWidget(m_matThumb);
  mh->addLayout(mv, 1);
  m_texture->addRow(QString(), matRow);

  connect(browse, &QToolButton::clicked, this, [this] { emit browseMaterialRequested(m_engine->faceSummary().material); });
  connect(applyCurrent, &QToolButton::clicked, this, [this] {
    if (!m_engine->currentMaterial().isEmpty())
      m_engine->applyMaterialToSelection(m_engine->currentMaterial());
  });
  connect(pick, &QToolButton::clicked, this, [this] { m_engine->setCurrentMaterial(m_engine->faceSummary().material); });

  // "U [field] • V [field] •"
  auto pair = [this](NumberField *&a, ResetDot *&da, NumberField *&b, ResetDot *&db, double step, int dec,
                     const QString &suffix) {
    auto *w = new QWidget(m_texture);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(2);
    a = new NumberField(w);
    b = new NumberField(w);
    da = new ResetDot(w);
    db = new ResetDot(w);
    for (NumberField *f: {a, b}) {
      f->setDecimals(dec);
      f->setSingleStep(step);
      f->setSuffix(suffix);
    }
    auto *lu = new QLabel(QStringLiteral("U"), w);
    auto *lv = new QLabel(QStringLiteral("V"), w);
    h->addWidget(lu);
    h->addWidget(a, 1);
    h->addWidget(da);
    h->addSpacing(4);
    h->addWidget(lv);
    h->addWidget(b, 1);
    h->addWidget(db);
    return w;
  };
  m_texture->addRow(tr("Tile size"), pair(m_scaleU, m_dotScaleU, m_scaleV, m_dotScaleV, 16.0, 2, QString()),
                    tr("Source units covered by one repeat of the texture"));
  m_texture->addRow(tr("Offset"), pair(m_offU, m_dotOffU, m_offV, m_dotOffV, 1.0, 2, QString()),
                    tr("Shift along the texture axes, in texels of the face's texture"));
  m_scaleU->setRange(0.01, 1e6);
  m_scaleV->setRange(0.01, 1e6);
  {
    auto *w = new QWidget(m_texture);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(2);
    m_rot = new NumberField(w);
    m_rot->setDecimals(2);
    m_rot->setSingleStep(15.0);
    m_rot->setRange(-360.0, 360.0);
    m_rot->setSuffix(QStringLiteral("°"));
    m_dotRot = new ResetDot(w);
    h->addWidget(m_rot, 1);
    h->addWidget(m_dotRot);
    m_texture->addRow(tr("Rotation"), w, tr("Rotation around the face normal, relative to world alignment"));
  }

  connect(m_scaleU, &NumberField::committed, this, [this](double v) { m_engine->setFaceTextureBasis(v, 0, 0, 0, 0, 1); });
  connect(m_scaleV, &NumberField::committed, this, [this](double v) { m_engine->setFaceTextureBasis(0, v, 0, 0, 0, 2); });
  connect(m_offU, &NumberField::committed, this, [this](double v) {
    m_engine->setFaceTextureBasis(0, 0, v, 0, 0, 4 | vk::Engine3D::kOffsetsInTexels);
  });
  connect(m_offV, &NumberField::committed, this, [this](double v) {
    m_engine->setFaceTextureBasis(0, 0, 0, v, 0, 8 | vk::Engine3D::kOffsetsInTexels);
  });
  connect(m_rot, &NumberField::committed, this, [this](double v) { m_engine->setFaceTextureBasis(0, 0, 0, 0, v, 16); });
  const std::pair<ResetDot *, int> dots[] = {{m_dotScaleU, 1}, {m_dotScaleV, 2}, {m_dotOffU, 4}, {m_dotOffV, 8},
                                             {m_dotRot, 16}};
  for (const auto &[dot, bit]: dots)
    connect(dot, &QToolButton::clicked, this, [this, bit] { m_engine->resetFaceTextureFields(bit); });

  auto *tools = new QWidget(m_texture);
  auto *g = new QGridLayout(tools);
  g->setContentsMargins(0, 0, 0, 0);
  g->setSpacing(3);
  struct Btn {
    const char *icon;
    QString text, tip;
    std::function<void()> fn;
  };
  const QList<QList<Btn>> rows = {
      {{"loop_back", tr("−90°"), tr("Rotate texture 90° counter-clockwise"), [this] { m_engine->rotateFaceTextures(-90); }},
       {"loop_forwards", tr("+90°"), tr("Rotate texture 90° clockwise"), [this] { m_engine->rotateFaceTextures(90); }},
       {"arrow_leftright", tr("Flip U"), tr("Mirror horizontally"), [this] { m_engine->flipFaceTextures(true); }},
       {"mod_mirror", tr("Flip V"), tr("Mirror vertically"), [this] { m_engine->flipFaceTextures(false); }}},
      {{"world", tr("World"), tr("Align to the world axes (paraxial)"), [this] { m_engine->alignFaceTextures(0); }},
       {"facesel", tr("Face"), tr("Align to the face plane"), [this] { m_engine->alignFaceTextures(1); }},
       {"fullscreen_enter", tr("Fit"), tr("Stretch one repeat across the face"), [this] { m_engine->fitFaceTextures(1, 1); }},
       {"mod_array", tr("Fit U"), tr("Fit horizontally, keep vertical scale"), [this] { m_engine->fitFaceTextures(1, 0); }}},
      {{"", tr("½×"), tr("Halve the tile size (more repeats)"), [this] { m_engine->scaleFaceTextures(0.5, 0.5); }},
       {"", tr("2×"), tr("Double the tile size (fewer repeats)"), [this] { m_engine->scaleFaceTextures(2.0, 2.0); }},
       {"", tr("← U"), tr("Shift by 1/8 tile"), [this] { m_engine->shiftFaceTextures(-0.125, 0); }},
       {"", tr("U →"), tr("Shift by 1/8 tile"), [this] { m_engine->shiftFaceTextures(0.125, 0); }}},
      {{"", tr("↑ V"), tr("Shift up by 1/8 tile"), [this] { m_engine->shiftFaceTextures(0, -0.125); }},
       {"", tr("V ↓"), tr("Shift down by 1/8 tile"), [this] { m_engine->shiftFaceTextures(0, 0.125); }},
       {"snap_edge", tr("Edge"), tr("Run the texture along a face edge (repeat to cycle edges)"),
        [this] { m_engine->alignFaceTexturesToEdge(); }},
       {"loop_back", tr("Reset all"), tr("Every field back to its default: world-aligned, the texture's default "
                                          "scale, no offset, no rotation"),
        [this] { m_engine->resetFaceTextures(); }}},
      {{"align_left", tr("Left"), tr("Justify: texture edge on the face's left edge"), [this] { m_engine->justifyFaceTextures(0); }},
       {"align_right", tr("Right"), tr("Justify right"), [this] { m_engine->justifyFaceTextures(1); }},
       {"align_top", tr("Top"), tr("Justify top"), [this] { m_engine->justifyFaceTextures(2); }},
       {"align_bottom", tr("Bottom"), tr("Justify bottom"), [this] { m_engine->justifyFaceTextures(3); }}},
      {{"align_center", tr("Centre"), tr("Centre the texture on the face"), [this] { m_engine->justifyFaceTextures(4); }},
       {"copydown", tr("Copy"), tr("Copy the face's material and mapping (Ctrl+Alt+C)"),
        [this] { m_engine->copyFaceAttributes(); }},
       {"pastedown", tr("Paste"), tr("Paste material and mapping (Ctrl+Alt+V; Ctrl+click a face in Material mode)"),
        [this] { m_engine->pasteFaceAttributes(1); }},
       {"mod_curve", tr("Wrap"), tr("Paste wrapped around the shared edge (Ctrl+Alt+Shift+V; Ctrl+Shift+click)"),
        [this] { m_engine->pasteFaceAttributes(2); }}},
  };
  for (int r = 0; r < rows.size(); ++r)
    for (int c = 0; c < rows[r].size(); ++c) {
      const Btn &b = rows[r][c];
      auto *btn = smallButton(QString::fromLatin1(b.icon), b.tip, tools, b.text);
      btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      auto fn = b.fn;
      connect(btn, &QToolButton::clicked, this, [fn] { fn(); });
      g->addWidget(btn, r, c);
    }
  m_texture->addRow(QString(), tools);

  auto *locks = new QWidget(m_texture);
  auto *lh = new QHBoxLayout(locks);
  lh->setContentsMargins(0, 0, 0, 0);
  m_texLock = new QCheckBox(tr("Texture lock"), locks);
  m_texLock->setToolTip(tr("Textures move with brushes instead of staying fixed to the world"));
  m_scaleLock = new QCheckBox(tr("Scale lock"), locks);
  m_scaleLock->setToolTip(tr("Textures stretch with brushes when they are resized"));
  lh->addWidget(m_texLock);
  lh->addWidget(m_scaleLock);
  lh->addStretch(1);
  m_texture->addRow(QString(), locks);
  connect(m_texLock, &QCheckBox::toggled, this, [this](bool on) { m_engine->setTextureLock(on); });
  connect(m_scaleLock, &QCheckBox::toggled, this, [this](bool on) { m_engine->setTextureScaleLock(on); });

  m_body->layout()->addWidget(m_texture);

  m_stairsTex = new Section(tr("Stairs"), QStringLiteral("mod_array"), m_body);
  m_stairsTexPanel = new StairsTexturePanel(m_engine, m_stairsTex);
  m_stairsTex->body()->addWidget(m_stairsTexPanel);
  m_body->layout()->addWidget(m_stairsTex);

  m_texProps = new Section(tr("Texture"), QStringLiteral("material"), m_body);
  m_texPropsPanel = new MaterialPropsPanel(m_engine, MaterialPropsPanel::Mode::Selection, m_texProps);
  m_texProps->body()->addWidget(m_texPropsPanel);
  m_body->layout()->addWidget(m_texProps);
}

void Inspector::revealTextureProperties(const QString &ref) {
  refresh();
  if (m_texProps->isHidden())
    return;
  m_texProps->setExpanded(true);
  if (!ref.isEmpty())
    m_texPropsPanel->showMaterial(ref);
  QTimer::singleShot(0, this, [this] { m_scroll->ensureWidgetVisible(m_texProps, 0, 0); });
}

bool Inspector::showsTextureProperties() const { return !m_texProps->isHidden(); }

void Inspector::buildWorld() {
  // `curve` spends more travel near zero.
  auto num = [](QWidget *p, double lo, double hi, int dec, const QString &suffix = {}, double curve = 1.0) {
    return new SliderField(lo, hi, dec, suffix, curve, p);
  };

  // A new level's values: each row's reset dot goes back to them (one undo step each).
  const fmap::SceneState def;
  const auto colorOf = [](const glm::vec3 &c) {
    const auto ch = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return QColor(ch(c.r), ch(c.g), ch(c.b));
  };
  const auto differs = [](double v, double d) { return std::fabs(v - d) > 1e-4 * std::max(1.0, std::fabs(d)); };
  const auto dotted = [this](QWidget *editor, QWidget *parent, std::function<bool()> changed,
                             std::function<void()> reset) {
    auto *w = new QWidget(parent);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(2);
    editor->setParent(w);
    h->addWidget(editor, 1);
    auto *dot = new ResetDot(w);
    h->addWidget(dot);
    connect(dot, &QToolButton::clicked, this, std::move(reset));
    m_worldDots.push_back({dot, std::move(changed)});
    return w;
  };
  const auto numDot = [&](SliderField *f, QWidget *parent, std::function<double()> get, double d,
                          std::function<void(float)> setter) {
    return dotted(f, parent, [get, d, differs] { return differs(get(), d); }, [setter, d] { setter(float(d)); });
  };
  const auto colorDot = [&](ColorButton *b, QWidget *parent, std::function<QColor()> get, const QColor &d,
                            std::function<void(const QColor &)> setter) {
    return dotted(b, parent, [get, d] { return get().rgb() != d.rgb(); }, [setter, d] { setter(d); });
  };
  vk::Engine3D *e = m_engine;

  m_sun = new Section(tr("Sun"), QStringLiteral("light_sun"), m_body);
  m_sunOn = new QCheckBox(tr("Sun and shadows"), m_sun);
  m_sunAz = num(m_sun, -180, 180, 1, QStringLiteral("°"));
  m_sunEl = num(m_sun, 0, 90, 1, QStringLiteral("°"));
  m_sunIntensity = num(m_sun, 0, 50, 2, {}, 2.0);
  m_sunColor = new ColorButton(m_sun);
  m_sun->addRow(QString(), m_sunOn);
  m_sunRows = {
      numDot(m_sunAz, m_sun, [e] { return e->sunAzimuth(); }, def.sun.azimuth, [e](float v) { e->setSunAzimuth(v); }),
      numDot(m_sunEl, m_sun, [e] { return e->sunElevation(); }, def.sun.elevation,
             [e](float v) { e->setSunElevation(v); }),
      numDot(m_sunIntensity, m_sun, [e] { return e->sunIntensity(); }, def.sun.intensity,
             [e](float v) { e->setSunIntensity(v); }),
      colorDot(m_sunColor, m_sun, [e] { return e->sunColor(); }, colorOf(def.sun.color),
               [e](const QColor &c) { e->setSunColor(c); })};
  m_sun->addRow(tr("Azimuth"), m_sunRows[0]);
  m_sun->addRow(tr("Elevation"), m_sunRows[1]);
  m_sun->addRow(tr("Intensity"), m_sunRows[2]);
  m_sun->addRow(tr("Colour"), m_sunRows[3]);
  m_body->layout()->addWidget(m_sun);

  m_ambient = new Section(tr("Environment"), QStringLiteral("world"), m_body);
  m_renderMode = new QComboBox(m_ambient);
  m_renderMode->addItems({tr("Fullbright"), tr("Lit")});
  m_ambColor = new ColorButton(m_ambient);
  m_ambIntensity = num(m_ambient, 0, 10, 2, {}, 2.0);
  m_fogColor = new ColorButton(m_ambient);
  m_fogDensity = num(m_ambient, 0, 0.05, 4, {}, 3.0);
  m_exposure = num(m_ambient, 0.05, 8, 2, {}, 2.0);
  auto *skyRow = new QWidget(m_ambient);
  auto *skyLay = new QHBoxLayout(skyRow);
  skyLay->setContentsMargins(0, 0, 0, 0);
  m_skyName = new QLabel(skyRow);
  m_skyName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  m_skyName->setMinimumWidth(1);
  auto *skyBrowse = new QPushButton(tr("Browse…"), skyRow);
  skyBrowse->setToolTip(tr("Pick a skybox in the Texture Browser"));
  m_skyClear = new QToolButton(skyRow);
  m_skyClear->setIcon(ActionManager::icon(QStringLiteral("x")));
  m_skyClear->setAutoRaise(true);
  m_skyClear->setToolTip(tr("No skybox (the lighting stays as it is)"));
  skyLay->addWidget(m_skyName, 1);
  skyLay->addWidget(skyBrowse);
  skyLay->addWidget(m_skyClear);
  m_ambient->addRow(tr("Skybox"), skyRow);
  connect(skyBrowse, &QPushButton::clicked, this,
          [this] { TextureBrowserWindow::openSkyboxes(m_engine, window()); });
  connect(m_skyClear, &QToolButton::clicked, this, [this] { m_engine->setSkybox({}); });
  m_ambient->addRow(tr("Shading"), m_renderMode, tr("Fullbright is fast and flat; Lit uses lights and shadows"));
  m_ambient->addRow(tr("Ambient"), colorDot(m_ambColor, m_ambient, [e] { return e->ambientColor(); },
                                            colorOf(def.ambient.color), [e](const QColor &c) { e->setAmbientColor(c); }));
  m_ambient->addRow(tr("Ambient strength"),
                    numDot(m_ambIntensity, m_ambient, [e] { return e->ambientIntensity(); }, def.ambient.intensity,
                           [e](float v) { e->setAmbientIntensity(v); }));
  m_ambient->addRow(tr("Fog colour"), colorDot(m_fogColor, m_ambient, [e] { return e->fogColor(); },
                                               colorOf(def.fog.color), [e](const QColor &c) { e->setFogColor(c); }));
  m_ambient->addRow(tr("Fog density"), numDot(m_fogDensity, m_ambient, [e] { return e->fogDensity(); },
                                              def.fog.density, [e](float v) { e->setFogDensity(v); }));
  m_ambient->addRow(tr("Exposure"), numDot(m_exposure, m_ambient, [e] { return e->exposure(); }, def.render.exposure,
                                           [e](float v) { e->setExposure(v); }));

  auto *lutRow = new QWidget(m_ambient);
  auto *lutLay = new QHBoxLayout(lutRow);
  lutLay->setContentsMargins(0, 0, 0, 0);
  m_lutName = new QLabel(lutRow);
  m_lutName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  m_lutName->setMinimumWidth(1);
  auto *lutManage = new QPushButton(tr("LUTs…"), lutRow);
  lutManage->setToolTip(tr("Pick, make and edit colour LUTs"));
  m_lutShow = new QToolButton(lutRow);
  {
    QIcon eye;
    eye.addPixmap(ActionManager::icon(QStringLiteral("hide_off")).pixmap(64, 64), QIcon::Normal, QIcon::On);
    eye.addPixmap(ActionManager::icon(QStringLiteral("hide_on")).pixmap(64, 64), QIcon::Normal, QIcon::Off);
    m_lutShow->setIcon(eye);
  }
  m_lutShow->setCheckable(true);
  m_lutShow->setAutoRaise(true);
  m_lutShow->setToolTip(tr("Show the colour LUT in the 3D view"));
  m_lutClear = new QToolButton(lutRow);
  m_lutClear->setIcon(ActionManager::icon(QStringLiteral("x")));
  m_lutClear->setAutoRaise(true);
  m_lutClear->setToolTip(tr("No colour LUT"));
  lutLay->addWidget(m_lutName, 1);
  lutLay->addWidget(lutManage);
  lutLay->addWidget(m_lutShow);
  lutLay->addWidget(m_lutClear);
  connect(m_lutShow, &QToolButton::toggled, this, [this](bool on) { m_engine->setColorLutEnabled(on); });
  m_ambient->addRow(tr("Colour LUT"), lutRow);
  connect(m_lutClear, &QToolButton::clicked, this, [this] { m_engine->setColorLut({}); });
  connect(lutManage, &QPushButton::clicked, this, [this] {
    if (!m_lutWindow)
      m_lutWindow = new LutWindow(m_engine, window());
    if (!m_engine->colorLut().isEmpty())
      m_lutWindow->selectLut(m_engine->colorLut());
    m_lutWindow->show();
    m_lutWindow->raise();
    m_lutWindow->activateWindow();
  });
  m_body->layout()->addWidget(m_ambient);

  m_shadows = new Section(tr("Shadows"), QStringLiteral("light"), m_body);
  m_shStrength = num(m_shadows, 0, 1, 2);
  m_shSoftness = num(m_shadows, 0, 8, 2);
  m_shBias = num(m_shadows, 0, 0.05, 4, {}, 3.0);
  m_shNormalBias = num(m_shadows, 0, 10, 2, {}, 2.0);
  m_shadows->addRow(tr("Strength"), numDot(m_shStrength, m_shadows, [e] { return e->shadowStrength(); },
                                           def.shadows.strength, [e](float v) { e->setShadowStrength(v); }));
  m_shadows->addRow(tr("Softness"),
                    numDot(m_shSoftness, m_shadows, [e] { return e->shadowRadius(); }, def.shadows.radius,
                           [e](float v) { e->setShadowRadius(v); }),
                    tr("Filter radius in shadow-map texels"));
  m_shadows->addRow(tr("Depth bias"), numDot(m_shBias, m_shadows, [e] { return e->shadowBias(); }, def.shadows.bias,
                                             [e](float v) { e->setShadowBias(v); }));
  m_shadows->addRow(tr("Normal bias"), numDot(m_shNormalBias, m_shadows, [e] { return e->shadowNormalBias(); },
                                              def.shadows.normalBias, [e](float v) { e->setShadowNormalBias(v); }));
  m_body->layout()->addWidget(m_shadows);

  m_post = new Section(tr("Ambient occlusion"), QStringLiteral("shading_rendered"), m_body);
  m_ssaoOn = new QCheckBox(tr("Enabled"), m_post);
  m_ssaoRadius = num(m_post, 0.05, 20, 2, {}, 2.0);
  m_ssaoIntensity = num(m_post, 0, 3, 2);
  m_post->addRow(QString(), m_ssaoOn);
  m_post->addRow(tr("Radius"), numDot(m_ssaoRadius, m_post, [e] { return e->ssaoRadius(); }, def.ssao.radius,
                                      [e](float v) { e->setSsaoRadius(v); }));
  m_post->addRow(tr("Intensity"), numDot(m_ssaoIntensity, m_post, [e] { return e->ssaoIntensity(); },
                                         def.ssao.intensity, [e](float v) { e->setSsaoIntensity(v); }));
  m_body->layout()->addWidget(m_post);

  m_nothingHint = new QWidget(m_body);
  auto *hl = new QVBoxLayout(m_nothingHint);
  hl->setContentsMargins(8, 12, 8, 12);
  auto *worldBtn = new QPushButton(ActionManager::icon(QStringLiteral("world")), tr("Edit World Environment"),
                                   m_nothingHint);
  connect(worldBtn, &QPushButton::clicked, this, [this] { m_engine->selectWorldEnv(); });
  hl->addWidget(worldBtn);
  m_body->layout()->addWidget(m_nothingHint);

  connect(m_sunOn, &QCheckBox::toggled, this, [this](bool on) { m_engine->setShadowsEnabled(on); });
  connect(m_sunAz, &SliderField::committed, this, [this](double v) { m_engine->setSunAzimuth(float(v)); });
  connect(m_sunEl, &SliderField::committed, this, [this](double v) { m_engine->setSunElevation(float(v)); });
  connect(m_sunIntensity, &SliderField::committed, this, [this](double v) { m_engine->setSunIntensity(float(v)); });
  connect(m_sunColor, &ColorButton::colorPicked, this, [this](const QColor &c) { m_engine->setSunColor(c); });
  connect(m_ambColor, &ColorButton::colorPicked, this, [this](const QColor &c) { m_engine->setAmbientColor(c); });
  connect(m_ambIntensity, &SliderField::committed, this, [this](double v) { m_engine->setAmbientIntensity(float(v)); });
  connect(m_fogColor, &ColorButton::colorPicked, this, [this](const QColor &c) { m_engine->setFogColor(c); });
  connect(m_fogDensity, &SliderField::committed, this, [this](double v) { m_engine->setFogDensity(float(v)); });
  connect(m_exposure, &SliderField::committed, this, [this](double v) { m_engine->setExposure(float(v)); });
  connect(m_shStrength, &SliderField::committed, this, [this](double v) { m_engine->setShadowStrength(float(v)); });
  connect(m_shSoftness, &SliderField::committed, this, [this](double v) { m_engine->setShadowRadius(float(v)); });
  connect(m_shBias, &SliderField::committed, this, [this](double v) { m_engine->setShadowBias(float(v)); });
  connect(m_shNormalBias, &SliderField::committed, this, [this](double v) { m_engine->setShadowNormalBias(float(v)); });
  // Only the quality preference.
  connect(m_ssaoOn, &QCheckBox::toggled, this, [](bool on) { prefs::get().setValue(QStringLiteral("Render/Ssao"), on); });
  connect(m_ssaoRadius, &SliderField::committed, this, [this](double v) { m_engine->setSsaoRadius(float(v)); });
  connect(m_ssaoIntensity, &SliderField::committed, this, [this](double v) { m_engine->setSsaoIntensity(float(v)); });
  connect(m_renderMode, &QComboBox::currentIndexChanged, this, [this](int idx) { m_engine->setRenderMode(idx); });
}

void Inspector::refresh() {
  if (!m_engine)
    return;
  const size_t objects = m_engine->selectedObjects().size();
  const size_t faces = m_engine->selectedFaces().size();
  const size_t ents = m_engine->selectedEntities().size();
  const size_t hosts = m_engine->selectedMods().size();
  const size_t paths = m_engine->selectedPaths().size();
  const bool nothingSelected = objects == 0 && faces == 0 && ents == 0 && hosts == 0 && paths == 0;
  const bool archTool = m_engine->shapeToolActive() && m_engine->shapeToolKind() == 7;
  const bool shapeTool = m_engine->shapeToolActive() && vk::Engine3D::isShapeKind(m_engine->shapeToolKind());
  // The arch tool works on the selection box: it takes the panel over while open.
  // Edit Source / Material-mode output pick: Modifiers is shown with nothing selected.
  const quint32 panelHost = nothingSelected ? m_engine->panelModHost() : 0;
  const bool nothing = nothingSelected && !archTool && !shapeTool && !panelHost;
  const bool world = m_engine->worldEnvSelected();

  QString icon = QStringLiteral("world");
  QString title, subtitle;
  if (archTool && nothingSelected) {
    icon = QStringLiteral("mod_bevel");
    title = tr("Arch");
  } else if (shapeTool && nothingSelected) {
    m_shapePanel->refresh();
    icon = m_shapePanel->icon();
    title = m_shapePanel->title();
  } else if (nothing) {
    title = tr("Nothing selected");
    subtitle = QString();
  } else if (panelHost) {
    icon = QStringLiteral("modifier");
    const eng::mods::Host *h = m_engine->modHost(panelHost);
    title = h ? h->name : tr("Modified object");
    subtitle = m_engine->editSourceHost() == panelHost ? tr("Editing the source") : tr("Modified object");
  } else if (world) {
    title = tr("World Environment");
  } else if (faces > 0) {
    icon = QStringLiteral("facesel");
    title = faces == 1 ? tr("1 face") : tr("%1 faces").arg(faces);
  } else if (const eng::Entity *be = ents == 1 && objects > 0
                                         ? m_engine->entityById(*m_engine->selectedEntities().begin())
                                         : nullptr;
             be && be->cls() && be->cls()->isBrushEntity()) {
    const eng::EntityClass *c = be->cls();
    icon = c->icon;
    title = be->name.isEmpty() ? c->label : be->name;
    QString brushes = objects == 1 ? tr("1 brush") : tr("%1 brushes").arg(objects);
    if (objects == 1)
      if (const auto o = m_engine->selectedObject(*m_engine->selectedObjects().begin()); o && o->getMesh())
        brushes += QStringLiteral(" · ") + tr("%n face(s)", "", o->getMesh()->faceCount());
    subtitle = QStringLiteral("%1 · %2").arg(be->classname, brushes);
  } else if (objects > 0) {
    icon = QStringLiteral("outliner_ob_mesh");
    title = objects == 1 ? tr("1 brush") : tr("%1 brushes").arg(objects);
    if (objects == 1)
      if (const auto o = m_engine->selectedObject(*m_engine->selectedObjects().begin())) {
        subtitle = o->name().isEmpty() ? tr("Brush %1").arg(o->stableId()) : o->name();
        if (const auto mesh = o->getMesh())
          subtitle += QStringLiteral(" · ") + tr("%n face(s)", "", mesh->faceCount());
      }
  } else if (paths > 0 && hosts == 0) {
    icon = QStringLiteral("curve_bezcurve");
    if (paths == 1) {
      const eng::path::Path *p = m_engine->path(*m_engine->selectedPaths().begin());
      title = p && !p->name.isEmpty() ? p->name : tr("Path");
      subtitle = p && p->kind == eng::path::Path::Kind::Connect ? tr("Connect path") : tr("Path");
    } else {
      title = tr("%1 paths").arg(paths);
    }
  } else if (hosts > 0) {
    icon = QStringLiteral("modifier");
    if (hosts == 1) {
      const eng::mods::Host *h = m_engine->modHost(*m_engine->selectedMods().begin());
      title = h ? h->name : tr("Modified object");
      subtitle = tr("Modified object");
    } else {
      title = tr("%1 modified objects").arg(hosts);
    }
  } else if (ents > 0) {
    const eng::Entity *e = m_engine->entityById(*m_engine->selectedEntities().begin());
    const eng::EntityClass *c = e ? e->cls() : nullptr;
    icon = c ? c->icon : QStringLiteral("empty_data");
    title = ents == 1 ? (c ? c->label : tr("Entity")) : tr("%1 entities").arg(ents);
    subtitle = ents == 1 && e ? (e->name.isEmpty() ? e->classname : QStringLiteral("%1 · %2").arg(e->name, e->classname))
                              : QString();
  }
  m_icon->setPixmap(ActionManager::icon(icon).pixmap(scale::icon(scale::Icon::Large), devicePixelRatioF()));
  m_title->setText(title);
  m_subtitle->setText(subtitle);
  m_subtitle->setVisible(!subtitle.isEmpty());

  const bool matMode = m_engine->materialMode();
  m_transform->setVisible(!matMode && !nothingSelected && !world && !(ents == 1 && objects == 0 && faces == 0)
                          && (objects > 0 || hosts > 0 || ents > 0));
  m_texture->setVisible(matMode && faces > 0);
  m_texProps->setVisible(matMode && faces > 0);
  m_stairsTex->setVisible(matMode && faces > 0 && m_stairsTexPanel->refresh());
  const quint32 proc = m_engine->materialMode() ? 0 : m_engine->selectedProcObject();
  const bool stairs = proc && m_engine->procType(proc) == QLatin1String("stairs");
  m_stairs->setVisible(stairs);
  m_cut->setVisible(m_engine->cutModeActive());
  m_bevel->setVisible(m_engine->bevelMode());
  m_mirror->setVisible(m_engine->mirrorActive() || m_engine->selectedMirrorLinked() > 0);
  {
    const quint32 ph = m_engine->panelModHost();
    const eng::mods::Host *h = ph ? m_engine->modHost(ph) : nullptr;
    arrangeSections(matMode, h && !h->stack.empty());
  }
  m_modifiers->setVisible(ModifierPanel::relevant(m_engine));
  m_pathSection->setVisible(!matMode && paths == 1);
  if (paths == 1)
    m_pathPanel->refresh();
  const bool archSelected = proc && m_engine->procType(proc) == QLatin1String("arch");
  m_arch->setVisible(archTool || archSelected);
  if (archTool || archSelected)
    m_archPanel->refresh();
  const bool shapeShown = ShapePanel::relevant(m_engine);
  m_shape->setVisible(shapeShown);
  if (shapeShown) {
    m_shapePanel->refresh();
    m_shape->setTitle(m_shapePanel->title(), m_shapePanel->icon());
  }
  if (stairs)
    m_stairsPanel->refresh();
  m_faceGroups->setVisible(m_engine->materialMode() && faces > 0);
  m_replaceRow->setVisible(matMode && faces > 0);
  std::vector<quint32> entIds(m_engine->selectedEntities().begin(), m_engine->selectedEntities().end());
  bool oneClass = !entIds.empty();
  QString entClass;
  for (const quint32 id: entIds) {
    const eng::Entity *e = m_engine->entityById(id);
    if (!e || (!entClass.isEmpty() && e->classname != entClass))
      oneClass = false;
    else
      entClass = e->classname;
  }
  m_entity->setVisible(oneClass && !world);
  m_entity->setBadge(oneClass ? entClass : QString());
  if (oneClass && !world)
    m_entityEditor->setEntities(entIds);
  // Classes without I/O show nothing.
  const quint32 ioId = world || faces > 0 ? 0 : m_engine->ioFocusEntity();
  const eng::Entity *ioEnt = ioId ? m_engine->entityById(ioId) : nullptr;
  const bool showIO = ioEnt && ioEnt->cls() && ioEnt->cls()->io;
  m_io->setVisible(showIO);
  if (showIO) {
    m_ioSummary->setEntity(ioId);
    m_io->setBadge(ioEnt->outputs.isEmpty() ? QString() : QString::number(ioEnt->outputs.size()));
  }
  for (Section *s: {m_sun, m_ambient, m_shadows, m_post})
    s->setVisible(world);
  m_nothingHint->setVisible(nothing);

  if (!nothingSelected && !matMode)
    refreshTransform();
  if (matMode && faces > 0)
    refreshTexture();
  if (world)
    refreshWorld();
}

void Inspector::refreshTransform() {
  glm::vec3 mn, mx;
  if (!m_engine->selectionBounds(mn, mx))
    return;
  const glm::vec3 c = (mn + mx) * 0.5f;
  const glm::vec3 s = mx - mn;
  const double cv[3] = {c.x * kSrc, c.y * kSrc, c.z * kSrc};
  const double sv[3] = {s.x * kSrc, s.y * kSrc, s.z * kSrc};
  m_pos->setValuesQuiet(cv);
  m_size->setValuesQuiet(sv);
  const bool facesOnly = m_engine->selectedObjects().empty();
  m_size->setFieldsEnabled(!facesOnly);
  // Brushes' rotation; none for entity-only selections.
  glm::dvec3 rot(0.0);
  bool mixed[3] = {false, false, false};
  const bool brushes = m_engine->selectionRotation(rot, mixed);
  m_rotation->setVisible(brushes);
  if (brushes) {
    const double rv[3] = {rot.x, rot.y, rot.z};
    m_rotation->setValuesQuiet(rv, mixed);
  }
  m_physApplyRow->setVisible(m_engine->selectionPhysicsDirty());
  {
    const int smooth = m_engine->selectionSmoothShading();
    const QSignalBlocker block(m_smoothShading);
    m_smoothShading->setVisible(!facesOnly && (smooth >= 0 || !m_engine->selectedObjects().empty()));
    m_smoothShading->setCheckState(smooth == 1 ? Qt::Checked : smooth == 0 ? Qt::Unchecked : Qt::PartiallyChecked);
  }
}

void Inspector::refreshTexture() {
  const auto s = m_engine->faceSummary();
  m_texture->setBadge(s.count == 1 ? tr("1 face") : tr("%1 faces").arg(s.count));
  QString scope;
  if (!s.mixedMaterial && !s.material.isEmpty()) {
    const auto mat = vk::TextureManager::instance().find(s.material);
    const MaterialRef mr = MaterialRef::parse(s.material);
    const auto *ov = m_engine->scene() ? &m_engine->scene()->materialOverrides : nullptr;
    if (mat && mat->state == eng::Material::State::Error)
      scope = tr("<span style='color:#e070c0'>● missing / server unreachable</span>");
    else if (mr.isVariant())
      scope = tr("<span style='color:#b07cff'>● variant “%1”</span>").arg(mr.variant.toHtmlEscaped());
    else if (ov && ov->mapLayer(s.material))
      scope = tr("<span style='color:#f0a840'>● overridden in this map</span>");
    else if (mat && mat->hasEntry() && !mat->entry.props.isEmpty())
      scope = tr("<span style='color:#509cff'>● server values</span>");
    else if (mat && mat->kind == eng::Material::Kind::Server)
      scope = tr("<span style='color:#96a0ba'>● defaults</span>");
  }
  m_matName->setText(s.mixedMaterial ? tr("<i>multiple materials</i>")
                                     : QStringLiteral("<b>%1</b><br><span style='color:#8a9ab8'>%2 × %3 px</span>%4")
                                           .arg(breakable(s.material).toHtmlEscaped())
                                           .arg(s.texWidth)
                                           .arg(s.texHeight)
                                           .arg(scope.isEmpty() ? QString() : QStringLiteral("<br>") + scope));
  const QString thumbKey = s.mixedMaterial ? QString() : s.material;
  if (thumbKey != m_lastThumbFor) {
    m_lastThumbFor = thumbKey;
    QPixmap pm;
    const auto thumbMat = thumbKey.isEmpty() ? eng::MaterialPtr() : vk::TextureManager::instance().find(thumbKey);
    if (thumbMat && thumbMat->hasEntry()) {
      pm = ui::ThumbCache::instance().server(thumbMat->entry.thumbFile());
      if (!pm.isNull())
        pm = pm.scaled(m_matThumb->size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }
    const QString path = (thumbKey.isEmpty() || !pm.isNull()) ? QString() : thumbPathFor(thumbKey);
    if (!path.isEmpty()) {
      QImageReader r(path);
      if (r.size().isValid())
        r.setScaledSize(r.size().scaled(m_matThumb->size(), Qt::KeepAspectRatioByExpanding));
      pm = QPixmap::fromImage(r.read());
    }
    m_matThumb->setPixmap(pm);
    m_matThumb->setText(pm.isNull() ? QStringLiteral("?") : QString());
  }
  if (!m_scaleU->hasFocus())
    m_scaleU->setValueQuiet(s.tileSourceU, s.mixedScaleU);
  if (!m_scaleV->hasFocus())
    m_scaleV->setValueQuiet(s.tileSourceV, s.mixedScaleV);
  if (!m_offU->hasFocus())
    m_offU->setValueQuiet(s.offsetTexU, s.mixedOffsetTexU);
  if (!m_offV->hasFocus())
    m_offV->setValueQuiet(s.offsetTexV, s.mixedOffsetTexV);
  if (!m_rot->hasFocus())
    m_rot->setValueQuiet(s.rotation, s.mixedRotation);
  m_dotScaleU->setChanged(s.changedScaleU, tr("Not the texture's default tile size"));
  m_dotScaleV->setChanged(s.changedScaleV, tr("Not the texture's default tile size"));
  m_dotOffU->setChanged(s.changedOffsetU, tr("Offset"));
  m_dotOffV->setChanged(s.changedOffsetV, tr("Offset"));
  m_dotRot->setChanged(s.changedRotation, tr("Rotated"));
  QSignalBlocker b1(m_texLock), b2(m_scaleLock);
  m_texLock->setChecked(m_engine->textureLock());
  m_scaleLock->setChecked(m_engine->textureScaleLock());
}

void Inspector::refreshWorld() {
  QSignalBlocker b1(m_sunOn), b2(m_ssaoOn), b3(m_renderMode);
  m_sunOn->setChecked(m_engine->shadowsEnabled());
  m_renderMode->setCurrentIndex(m_engine->renderMode());
  {
    const QString lut = m_engine->colorLut();
    const TextureEntry *e = lut.isEmpty() ? nullptr : net::TextureCatalog::instance().find(lut, true);
    if (!lut.isEmpty() && !e && net::AssetClient::instance().isOnline() &&
        !net::TextureCatalog::instance().isKnownMissing(lut, true))
      net::TextureCatalog::instance().resolve({lut}, this, [this] { scheduleRefresh(); }, true);
    m_lutName->setText(lut.isEmpty() ? tr("None") : MaterialRef::parse(lut).name);
    m_lutName->setToolTip(lut);
    m_lutClear->setVisible(!lut.isEmpty());
    m_lutShow->setVisible(!lut.isEmpty());
    const QSignalBlocker block(m_lutShow);
    m_lutShow->setChecked(m_engine->colorLutEnabled());
  }
  // SliderField ignores refreshes while being dragged or typed in.
  const auto set = [](SliderField *f, double v) { f->setValueQuiet(v); };
  // A sun entity overrides the map's sun every frame: those controls go read-only.
  const bool sunEntity = m_engine->sunEntity() != nullptr;
  m_sun->setBadge(sunEntity ? tr("driven by the sun entity") : QString());
  m_sunOn->setEnabled(!sunEntity);
  for (QWidget *w: m_sunRows)
    w->setEnabled(!sunEntity);
  set(m_sunAz, m_engine->sunAzimuth());
  set(m_sunEl, m_engine->sunElevation());
  set(m_sunIntensity, m_engine->sunIntensity());
  m_sunColor->setColor(m_engine->sunColor());
  m_ambColor->setColor(m_engine->ambientColor());
  set(m_ambIntensity, m_engine->ambientIntensity());
  m_fogColor->setColor(m_engine->fogColor());
  set(m_fogDensity, m_engine->fogDensity());
  {
    const QString sky = m_engine->skybox();
    const TextureEntry *e = sky.isEmpty() ? nullptr : net::TextureCatalog::instance().find(sky);
    const QString title = e ? eng::skyInfoFromJson(e->sky).title : QString();
    m_skyName->setText(sky.isEmpty() ? tr("None") : !title.isEmpty() ? title : MaterialRef::parse(sky).name);
    m_skyName->setToolTip(sky);
    m_skyClear->setVisible(!sky.isEmpty());
  }
  set(m_exposure, m_engine->exposure());
  set(m_shStrength, m_engine->shadowStrength());
  set(m_shSoftness, m_engine->shadowRadius());
  set(m_shBias, m_engine->shadowBias());
  set(m_shNormalBias, m_engine->shadowNormalBias());
  m_ssaoOn->setChecked(prefs::b("Render/Ssao"));
  set(m_ssaoRadius, m_engine->ssaoRadius());
  set(m_ssaoIntensity, m_engine->ssaoIntensity());
  for (const WorldDot &d: m_worldDots)
    d.dot->setChanged(d.changed());
}

} // namespace ui
