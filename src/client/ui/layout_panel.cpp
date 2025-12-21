#include "layout_panel.h"
#include "client/ui/scale.h"

#include <algorithm>
#include <climits>
#include <cmath>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "client/ui/layout_designer.h"
#include "client/ui/widgets.h"

namespace ui {

using namespace eng::layout;

namespace {

void setRowVisible(Section *s, int row, bool on) {
  for (int c = 0; c < 2; ++c)
    if (QLayoutItem *it = s->grid()->itemAtPosition(row, c))
      if (QWidget *w = it->widget())
        w->setVisible(on);
}

// A width picker: the preset list, plus the current value when it is not one of them.
void fillWidths(QComboBox *cb, const std::vector<int> &presets, int value) {
  if (cb->view()->isVisible())
    return;
  QSignalBlocker b(cb);
  std::vector<int> items = presets;
  if (value > 0 && std::find(items.begin(), items.end(), value) == items.end()) {
    items.push_back(value);
    std::sort(items.begin(), items.end());
  }
  bool same = cb->count() == int(items.size());
  for (int i = 0; same && i < cb->count(); ++i)
    same = cb->itemData(i).toInt() == items[size_t(i)];
  if (!same) {
    cb->clear();
    for (const int w: items)
      cb->addItem(QStringLiteral("%1 u").arg(w), w);
  }
  cb->setCurrentIndex(value > 0 ? cb->findData(value) : -1);
}

void setNumber(NumberField *f, int v) {
  if (f->hasFocus())
    return;
  f->setValueQuiet(v == INT_MIN || v < -1000000 ? 0 : v, v == INT_MIN);
}

void setText(QLineEdit *e, const QString &t) {
  if (e->hasFocus() || e->text() == t)
    return;
  QSignalBlocker b(e);
  e->setText(t);
}

QComboBox *contactCombo(QWidget *parent) {
  auto *cb = new QComboBox(parent);
  cb->addItem(QObject::tr("Open"), int(ContactOpen));
  cb->setItemData(0, QObject::tr("The room cuts into the corridor: the corridor opens fully where they overlap"),
                  Qt::ToolTipRole);
  cb->addItem(QObject::tr("Doorway"), int(ContactDoorway));
  cb->setItemData(1, QObject::tr("A wall with a door-sized hole where a corridor meets the room"), Qt::ToolTipRole);
  cb->addItem(QObject::tr("Shared wall"), int(ContactSharedWall));
  cb->setItemData(2,
                  QObject::tr("The corridor keeps its wall; the room shrinks to sit against it and shares that wall"),
                  Qt::ToolTipRole);
  cb->setPlaceholderText(QObject::tr("mixed"));
  return cb;
}

} // namespace

LayoutPanel::LayoutPanel(LayoutModel *model, QWidget *parent) : QWidget(parent), m_model(model) {
  scale::bind(this, [this] { setMinimumWidth(scale::tpx(300)); });
  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_body = new QWidget(scroll);
  auto *v = new QVBoxLayout(m_body);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(2);
  scroll->setWidget(m_body);
  outer->addWidget(scroll);

  build();
  v->addStretch(1);

  m_refreshTimer = new QTimer(this);
  m_refreshTimer->setSingleShot(true);
  m_refreshTimer->setInterval(30);
  connect(m_refreshTimer, &QTimer::timeout, this, &LayoutPanel::refresh);
  connect(m_model, &LayoutModel::changed, this, &LayoutPanel::scheduleRefresh);
  connect(m_model, &LayoutModel::toolChanged, this, &LayoutPanel::scheduleRefresh);
  connect(m_model, &LayoutModel::gridChanged, this, &LayoutPanel::scheduleRefresh);
  connect(m_model, &LayoutModel::layoutsChanged, this, &LayoutPanel::scheduleRefresh);
  refresh();
}

void LayoutPanel::scheduleRefresh() { m_refreshTimer->start(); }

NumberField *LayoutPanel::number(int lo, int hi, int step) {
  auto *f = new NumberField(m_body);
  f->setDecimals(0);
  f->setRange(lo, hi);
  f->setSingleStep(step);
  f->setSuffix(QStringLiteral(" u"));
  return f;
}

void LayoutPanel::setCfgInt(const char *key, int v) { m_model->setCfg(QString::fromLatin1(key), v); }

void LayoutPanel::build() {
  auto *body = static_cast<QVBoxLayout *>(m_body->layout());
  auto connectNum = [this](NumberField *f, auto fn) {
    connect(f, &NumberField::committed, this, [fn](double v) { fn(int(std::lround(v))); });
  };
  auto cfgNum = [this, connectNum](const char *key, int lo, int hi, int step) {
    NumberField *f = number(lo, hi, step);
    connectNum(f, [this, key](int v) { setCfgInt(key, v); });
    return f;
  };

  auto *lay = new Section(tr("Layout"), QStringLiteral("mesh_grid"), m_body);
  {
    auto *row = new QWidget(lay);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    m_layouts = new QComboBox(row);
    m_layouts->setPlaceholderText(tr("(new, not in the map yet)"));
    m_layouts->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *add = new QToolButton(row);
    add->setText(tr("New"));
    add->setToolTip(tr("Start a new, empty layout (keeps the current settings)"));
    h->addWidget(m_layouts, 1);
    h->addWidget(add);
    lay->addRow(QString(), row);
    connect(m_layouts, &QComboBox::activated, this, [this](int i) { m_model->loadLayout(i); });
    connect(add, &QToolButton::clicked, this, [this] { m_model->newLayout(); });
    m_name = new QLineEdit(lay);
    lay->addRow(tr("Name"), m_name);
    connect(m_name, &QLineEdit::editingFinished, this, [this] { m_model->setCfg(QStringLiteral("name"), m_name->text()); });
  }
  body->addWidget(lay);

  m_sel = new Section(tr("Selection"), QStringLiteral("restrict_select_off"), m_body);
  {
    m_selWidth = new QComboBox(m_sel);
    m_selWidth->setPlaceholderText(tr("mixed"));
    m_rowSelWidth = m_sel->addRow(tr("Width"), m_selWidth);
    connect(m_selWidth, &QComboBox::activated, this, [this] { m_model->setSelWidth(m_selWidth->currentData().toInt()); });
    m_selLength = number(8, 65536);
    m_rowSelLength = m_sel->addRow(tr("Length"), m_selLength,
                                   tr("Floor length along the segment (end caps included). The far end moves; the "
                                      "rest of the path follows."));
    connectNum(m_selLength, [this](int v) { m_model->setSelLength(v); });
    m_roomW = number(8, 65536);
    m_rowRoomW = m_sel->addRow(tr("Width"), m_roomW);
    m_roomD = number(8, 65536);
    m_rowRoomD = m_sel->addRow(tr("Depth"), m_roomD);
    connectNum(m_roomW, [this](int v) { m_model->setSelRoomSize(v, m_model->selRoomSize().height()); });
    connectNum(m_roomD, [this](int v) { m_model->setSelRoomSize(m_model->selRoomSize().width(), v); });
    m_selHeight = number(8, 65536);
    m_rowSelHeight = m_sel->addRow(tr("Height"), m_selHeight);
    connectNum(m_selHeight, [this](int v) { m_model->setSelHeight(v); });
    m_selElev = number(-65536, 65536);
    m_rowSelElev = m_sel->addRow(tr("Floor elevation"), m_selElev);
    connectNum(m_selElev, [this](int v) { m_model->setSelElevation(v); });
    m_selContact = contactCombo(m_sel);
    m_rowContact = m_sel->addRow(tr("Corridor contact"), m_selContact);
    connect(m_selContact, &QComboBox::activated, this,
            [this] { m_model->setSelContact(m_selContact->currentData().toInt()); });
    auto *del = new QPushButton(tr("Delete selection"), m_sel);
    del->setToolTip(tr("Delete / Backspace"));
    m_sel->addRow(QString(), del);
    connect(del, &QPushButton::clicked, this, [this] { m_model->deleteSelection(); });
  }
  body->addWidget(m_sel);

  auto *corr = new Section(tr("Corridors"), QStringLiteral("ipo_linear"), m_body);
  {
    m_corrWidth = new QComboBox(corr);
    corr->addRow(tr("Width"), m_corrWidth, tr("Width of new corridors"));
    connect(m_corrWidth, &QComboBox::activated, this,
            [this] { setCfgInt("corridorWidth", m_corrWidth->currentData().toInt()); });
    m_corrHeight = cfgNum("corridorHeight", 8, 65536, 16);
    corr->addRow(tr("Height"), m_corrHeight);
  }
  body->addWidget(corr);

  m_rooms = new Section(tr("Rooms"), QStringLiteral("mesh_plane"), m_body);
  {
    m_roomHeight = cfgNum("roomHeight", 8, 65536, 16);
    m_rooms->addRow(tr("Height"), m_roomHeight);
    m_roomContact = contactCombo(m_rooms);
    m_rooms->addRow(tr("Corridor contact"), m_roomContact,
                    tr("How new rooms meet corridors. Hold Alt while drawing a room to toggle Shared wall."));
    connect(m_roomContact, &QComboBox::activated, this,
            [this] { setCfgInt("roomContact", m_roomContact->currentData().toInt()); });
    m_doorW = cfgNum("doorWidth", 8, 4096, 8);
    m_rowDoorW = m_rooms->addRow(tr("Door width"), m_doorW);
    m_doorH = cfgNum("doorHeight", 8, 4096, 8);
    m_rowDoorH = m_rooms->addRow(tr("Door height"), m_doorH);
  }
  body->addWidget(m_rooms);

  auto *vents = new Section(tr("Vents"), QStringLiteral("force_wind"), m_body);
  {
    m_ventWidth = new QComboBox(vents);
    vents->addRow(tr("Width"), m_ventWidth, tr("Width of new vents"));
    connect(m_ventWidth, &QComboBox::activated, this,
            [this] { setCfgInt("ventWidth", m_ventWidth->currentData().toInt()); });
    m_ventHeight = cfgNum("ventHeight", 8, 4096, 8);
    vents->addRow(tr("Height"), m_ventHeight);
    m_ventElev = cfgNum("ventElevation", -4096, 4096, 8);
    vents->addRow(tr("Height above floor"), m_ventElev);
    m_ventWall = cfgNum("ventWall", 1, 256, 2);
    vents->addRow(tr("Duct wall"), m_ventWall);
  }
  body->addWidget(vents);

  auto *walls = new Section(tr("Walls"), QStringLiteral("mod_solidify"), m_body);
  {
    m_wall = cfgNum("wall", 1, 512, 2);
    walls->addRow(tr("Wall thickness"), m_wall);
    m_floor = cfgNum("floor", 1, 512, 2);
    walls->addRow(tr("Floor thickness"), m_floor);
    m_ceiling = cfgNum("ceiling", 1, 512, 2);
    walls->addRow(tr("Ceiling thickness"), m_ceiling);
  }
  body->addWidget(walls);

  auto *view = new Section(tr("Grid && View"), QStringLiteral("snap_grid"), m_body);
  {
    m_grid = new QComboBox(view);
    for (const int g: LayoutModel::gridSizes())
      m_grid->addItem(QStringLiteral("%1 u").arg(g), g);
    view->addRow(tr("Grid"), m_grid, tr("Snap grid for drawing  ( [ smaller, ] bigger )"));
    connect(m_grid, &QComboBox::activated, this, [this] { m_model->setGridSize(m_grid->currentData().toInt()); });
    m_dims = new QCheckBox(tr("Show dimensions"), view);
    view->addRow(QString(), m_dims);
    connect(m_dims, &QCheckBox::toggled, this, [this](bool on) { m_model->setShowDimensions(on); });
    auto *fit = new QPushButton(tr("Fit view"), view);
    fit->setToolTip(tr("F"));
    view->addRow(QString(), fit);
    connect(fit, &QPushButton::clicked, this, &LayoutPanel::fitRequested);
  }
  body->addWidget(view);

  auto *mats = new Section(tr("Materials"), QStringLiteral("material"), m_body);
  {
    static const char *keys[4] = {"matFloor", "matWall", "matCeiling", "matVent"};
    const QString labels[4] = {tr("Floor"), tr("Walls"), tr("Ceiling"), tr("Vents")};
    for (int i = 0; i < 4; ++i) {
      auto *row = new QWidget(mats);
      auto *h = new QHBoxLayout(row);
      h->setContentsMargins(0, 0, 0, 0);
      h->setSpacing(6);
      m_mat[i] = new QLineEdit(row);
      m_mat[i]->setPlaceholderText(tr("current material"));
      auto *use = new QToolButton(row);
      use->setText(tr("Use current"));
      use->setToolTip(tr("Take the material browser's current material"));
      h->addWidget(m_mat[i], 1);
      h->addWidget(use);
      mats->addRow(labels[i], row);
      const QString key = QString::fromLatin1(keys[i]);
      QLineEdit *edit = m_mat[i];
      connect(edit, &QLineEdit::editingFinished, this, [this, key, edit] { m_model->setCfg(key, edit->text()); });
      connect(use, &QToolButton::clicked, this, [this, key] { m_model->useCurrentMaterial(key); });
    }
  }
  body->addWidget(mats);
}

void LayoutPanel::refresh() {
  const Doc &doc = m_model->doc();
  const eng::layout::Config &c = doc.config;

  {
    QSignalBlocker b(m_layouts);
    const QStringList names = m_model->layoutNames();
    QStringList cur;
    for (int i = 0; i < m_layouts->count(); ++i)
      cur << m_layouts->itemText(i);
    if (cur != names) {
      m_layouts->clear();
      m_layouts->addItems(names);
    }
    m_layouts->setCurrentIndex(m_model->layoutIndex());
  }
  setText(m_name, doc.name);

  // Selection.
  const int kind = m_model->selKind();
  const bool any = m_model->selCount() > 0;
  m_sel->setVisible(any);
  if (any) {
    const bool segs = kind == 0 || kind == 2;
    m_sel->setBadge(segs ? tr("%n segment(s)", "", int(m_model->selectedSegs().size()))
                         : tr("%n room(s)", "", int(m_model->selectedRooms().size())));
    const bool rooms = !m_model->selectedRooms().empty();
    const QSize rs = m_model->selRoomSize();
    setRowVisible(m_sel, m_rowSelWidth, segs);
    setRowVisible(m_sel, m_rowSelLength, segs);
    setRowVisible(m_sel, m_rowRoomW, rs.width() > 0);
    setRowVisible(m_sel, m_rowRoomD, rs.width() > 0);
    setRowVisible(m_sel, m_rowContact, rooms);
    if (segs) {
      fillWidths(m_selWidth, kind == 2 ? LayoutModel::ventWidths() : LayoutModel::corridorWidths(), m_model->selWidth());
      setNumber(m_selLength, m_model->selLength());
    }
    if (rs.width() > 0) {
      setNumber(m_roomW, rs.width());
      setNumber(m_roomD, rs.height());
    }
    setNumber(m_selHeight, m_model->selHeight());
    setNumber(m_selElev, m_model->selElevation());
    if (rooms) {
      QSignalBlocker b(m_selContact);
      const int ct = m_model->selContact();
      m_selContact->setCurrentIndex(ct >= 0 ? m_selContact->findData(ct) : -1);
    }
  }

  fillWidths(m_corrWidth, LayoutModel::corridorWidths(), c.corridorWidth);
  setNumber(m_corrHeight, c.corridorHeight);

  setNumber(m_roomHeight, c.roomHeight);
  {
    QSignalBlocker b(m_roomContact);
    m_roomContact->setCurrentIndex(m_roomContact->findData(c.roomContact));
  }
  bool doorway = c.roomContact == ContactDoorway;
  for (const RoomShape &r: doc.rooms)
    doorway = doorway || r.contact == ContactDoorway;
  setRowVisible(m_rooms, m_rowDoorW, doorway);
  setRowVisible(m_rooms, m_rowDoorH, doorway);
  setNumber(m_doorW, c.doorWidth);
  setNumber(m_doorH, c.doorHeight);

  fillWidths(m_ventWidth, LayoutModel::ventWidths(), c.ventWidth);
  setNumber(m_ventHeight, c.ventHeight);
  setNumber(m_ventElev, c.ventElevation);
  setNumber(m_ventWall, c.ventWall);

  setNumber(m_wall, c.wall);
  setNumber(m_floor, c.floor);
  setNumber(m_ceiling, c.ceiling);

  {
    QSignalBlocker b(m_grid);
    m_grid->setCurrentIndex(m_grid->findData(m_model->gridSize()));
  }
  {
    QSignalBlocker b(m_dims);
    m_dims->setChecked(m_model->showDimensions());
  }

  const QString mats[4] = {c.matFloor, c.matWall, c.matCeiling, c.matVent};
  for (int i = 0; i < 4; ++i)
    setText(m_mat[i], mats[i]);
}

} // namespace ui
