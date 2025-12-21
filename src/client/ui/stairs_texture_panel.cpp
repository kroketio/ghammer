#include "stairs_texture_panel.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "client/prefs/preferences.h"
#include "client/ui/widgets.h"
#include "client/vulkan/vk_engine.h"
#include "client/vulkan/vk_shared.h"

namespace ui {

using eng::stairs::Part;
using eng::stairs::StepTex;

namespace {

QWidget *fieldWithDot(QWidget *parent, NumberField *&field, ResetDot *&dot) {
  auto *w = new QWidget(parent);
  auto *h = new QHBoxLayout(w);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(2);
  field = new NumberField(w);
  dot = new ResetDot(w);
  h->addWidget(field, 1);
  h->addWidget(dot);
  return w;
}

} // namespace

StairsTexturePanel::StairsTexturePanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(4);
  m_title = new QLabel(this);
  m_title->setWordWrap(true);
  m_title->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  v->addWidget(m_title);
  m_blocks = new QVBoxLayout();
  m_blocks->setContentsMargins(0, 0, 0, 0);
  m_blocks->setSpacing(2);
  v->addLayout(m_blocks);
  buildBlock(Part::Flat, tr("Treads (flat)"));
  buildBlock(Part::Wall, tr("Risers (wall)"));
  buildBlock(Part::Stringer, tr("Side brushes"));
}

void StairsTexturePanel::buildBlock(const Part part, const QString &title) {
  Block &b = m_block[size_t(part)];
  b.section = new Section(title, QStringLiteral("texture"), this);
  b.section->setSubsection();
  m_blocks->addWidget(b.section);

  b.status = new QLabel(b.section);
  b.status->setWordWrap(true);
  b.status->setStyleSheet(QStringLiteral("color:#8a9ab8;"));
  b.section->addRow(QString(), b.status);

  auto *buttons = new QWidget(b.section);
  auto *g = new QGridLayout(buttons);
  g->setContentsMargins(0, 0, 0, 0);
  g->setSpacing(3);
  b.fit = new QPushButton(tr("Fit to steps"), buttons);
  b.fit->setToolTip(tr("Give every face of this part the same placement, each centred on its own face, and the "
                       "texture most used by the selected faces"));
  b.rotate = new QPushButton(tr("Rotate 90°"), buttons);
  b.rotate->setToolTip(tr("Turn the texture a quarter on every face of this part"));
  b.select = new QPushButton(tr("Select"), buttons);
  b.select->setToolTip(tr("Select every face of this part of these stairs"));
  b.release = new QPushButton(tr("Release"), buttons);
  b.release->setToolTip(tr("Stop applying the step placement (the faces keep their textures and can be edited "
                           "one by one)"));
  g->addWidget(b.fit, 0, 0);
  g->addWidget(b.rotate, 0, 1);
  g->addWidget(b.select, 1, 0);
  g->addWidget(b.release, 1, 1);
  b.section->addRow(QString(), buttons);

  b.fields = new QWidget(b.section);
  auto *fg = new QGridLayout(b.fields);
  fg->setContentsMargins(0, 0, 0, 0);
  fg->setHorizontalSpacing(4);
  fg->setVerticalSpacing(4);
  auto *offRow = new QWidget(b.fields);
  auto *oh = new QHBoxLayout(offRow);
  oh->setContentsMargins(0, 0, 0, 0);
  oh->setSpacing(2);
  oh->addWidget(new QLabel(QStringLiteral("U"), offRow));
  oh->addWidget(fieldWithDot(offRow, b.offU, b.dotOffU), 1);
  oh->addSpacing(4);
  oh->addWidget(new QLabel(QStringLiteral("V"), offRow));
  oh->addWidget(fieldWithDot(offRow, b.offV, b.dotOffV), 1);
  auto *offLabel = new QLabel(tr("Offset"), b.fields);
  offLabel->setToolTip(tr("Which texel of the texture sits on the centre of every face, from the texture's centre "
                          "(move the plank you want onto the step)"));
  fg->addWidget(offLabel, 0, 0);
  fg->addWidget(offRow, 0, 1);
  auto *tileLabel = new QLabel(tr("Tile size"), b.fields);
  tileLabel->setToolTip(tr("Source units one texture width covers on every face (V follows the texture's "
                           "shape); it grows and shrinks around the face centre"));
  fg->addWidget(tileLabel, 1, 0);
  fg->addWidget(fieldWithDot(b.fields, b.tile, b.dotTile), 1, 1);
  fg->setColumnStretch(1, 1);
  b.section->addRow(QString(), b.fields);

  for (NumberField *f: {b.offU, b.offV}) {
    f->setDecimals(2);
    f->setSingleStep(1.0);
    f->setSuffix(QStringLiteral(" px"));
  }
  b.tile->setDecimals(2);
  b.tile->setSingleStep(1.0);
  b.tile->setRange(0.01, 1e6);
  b.tile->setSuffix(QStringLiteral(" u"));

  connect(b.fit, &QPushButton::clicked, this, [this, part] {
    if (m_id)
      m_engine->fitStairPartTexture(m_id, part);
  });
  connect(b.rotate, &QPushButton::clicked, this,
          [this, part] { edit(part, [](StepTex &t) { t.rotation = (t.rotation + 1) % 4; }); });
  connect(b.select, &QPushButton::clicked, this, [this, part] {
    if (m_id)
      m_engine->selectStairPart(m_id, part);
  });
  connect(b.release, &QPushButton::clicked, this, [this, part] { edit(part, [](StepTex &t) { t.on = false; }); });
  connect(b.offU, &NumberField::committed, this, [this, part](double v) { edit(part, [v](StepTex &t) { t.offsetU = v; }); });
  connect(b.offV, &NumberField::committed, this, [this, part](double v) { edit(part, [v](StepTex &t) { t.offsetV = v; }); });
  connect(b.tile, &NumberField::committed, this, [this, part](double v) { edit(part, [v](StepTex &t) { t.tile = v; }); });
  connect(b.dotOffU, &QToolButton::clicked, this, [this, part] { edit(part, [](StepTex &t) { t.offsetU = 0.0; }); });
  connect(b.dotOffV, &QToolButton::clicked, this, [this, part] { edit(part, [](StepTex &t) { t.offsetV = 0.0; }); });
  connect(b.dotTile, &QToolButton::clicked, this,
          [this, part] { edit(part, [this](StepTex &t) { t.tile = defaultTile(t); }); });
}

double StairsTexturePanel::defaultTile(const StepTex &t) const {
  const auto m = t.material.isEmpty() ? eng::MaterialPtr() : vk::TextureManager::instance().find(t.material);
  const double w = m && m->texWidth > 0 ? m->texWidth : 512.0;
  return w * prefs::d("Brush/DefaultTextureScale");
}

void StairsTexturePanel::edit(const Part part, const std::function<void(StepTex &)> &fn) {
  if (!m_id)
    return;
  StepTex t = m_engine->stairPartTexture(m_id, part);
  if (!t.on)
    return;
  fn(t);
  m_engine->setStairPartTexture(m_id, part, t, tr("Stairs texture"));
}

bool StairsTexturePanel::refresh() {
  m_id = m_engine->selectedFacesProcObject();
  if (!m_id || m_engine->procType(m_id) != QLatin1String("stairs"))
    return false;
  const QJsonObject params = m_engine->procParams(m_id);
  const eng::stairs::Params sp = eng::stairs::fromJson(params);
  const int parts = m_engine->selectedStairParts(m_id);
  m_title->setText(m_engine->procName(m_id).toHtmlEscaped());

  // Another stairs or part picked: open only the picked parts.
  const bool reopen = m_id != m_lastId || parts != m_lastParts;
  m_lastId = m_id;
  m_lastParts = parts;

  for (int pi = 0; pi < int(Part::Count); ++pi) {
    const Part part = Part(pi);
    Block &b = m_block[size_t(pi)];
    const bool exists = part != Part::Stringer || sp.sides;
    b.section->setVisible(exists);
    if (!exists)
      continue;
    if (reopen)
      b.section->setExpanded(parts == 0 ? part == Part::Flat : (parts & (1 << pi)) != 0);
    const StepTex t = eng::stairs::stepTexOf(params, part);
    b.section->setBadge(t.on ? tr("on") : QString());
    b.status->setText(t.on ? tr("Every face: %1").arg(t.material.isEmpty() ? tr("its own texture") : t.material)
                           : tr("Off"));
    b.rotate->setEnabled(t.on);
    b.release->setEnabled(t.on);
    b.fields->setEnabled(t.on);
    if (!b.offU->hasFocus())
      b.offU->setValueQuiet(t.offsetU);
    if (!b.offV->hasFocus())
      b.offV->setValueQuiet(t.offsetV);
    if (!b.tile->hasFocus())
      b.tile->setValueQuiet(t.on ? t.tile : defaultTile(t));
    b.dotOffU->setChanged(t.on && std::fabs(t.offsetU) > 1e-6, tr("Offset"));
    b.dotOffV->setChanged(t.on && std::fabs(t.offsetV) > 1e-6, tr("Offset"));
    b.dotTile->setChanged(t.on && std::fabs(t.tile - defaultTile(t)) > 1e-3, tr("Not the texture's default tile size"));
  }
  return true;
}

} // namespace ui
