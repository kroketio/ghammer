#include "project_dialog.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QQuickItem>
#include <QPainter>
#include <QQuickWidget>
#include <QStyledItemDelegate>
#include <QStackedWidget>
#include <QRandomGenerator>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "client/project/project.h"
#include "client/project/project_manager.h"
#include "client/net/local_server.h"
#include "client/prefs/preferences_dialog.h"
#include "client/ui/action_manager.h"
#include "client/ui/file_dialog.h"
#include "client/prefs/preferences.h"
#include "client/ui/file_icons.h"
#include "client/ui/scale.h"
#include <glm/gtc/matrix_transform.hpp>

#include "client/ui/scene_preview.h"
#include "client/vulkan/vk_engine.h"
#include "client/vulkan/vk_shared.h"
#include "ghammer_version.h"

namespace ui {

namespace {

enum Page { PageRecent = 0, PageNew = 1 };
constexpr const char *kSplashBackground = "#0c1022";
constexpr int kSplashDropMs = 7000;
constexpr int kRootRole = Qt::UserRole + 1;
constexpr int kExistsRole = Qt::UserRole + 2;
constexpr int kRecentRole = Qt::UserRole + 3; // in the recents (else found in ~/GodotProjects)

const char *kStyle = R"(
#projSide { background: #0c1022; border-right: 1px solid #1f2747; }
#projTitle { font-weight: 600; }
#projCaption { color: #8a95b5; }
#projProblem { color: #f0b45a; }
#projSection { color: #8f9bbd; font-size: $font_heading; font-weight: 600; }
#projScale, #projServer { border-top: 1px solid #1f2747; }
QToolButton#projSegment { background: #0f1328; border: 1px solid #2a3360; border-left: none; padding: 4px 12px; }
QToolButton#projSegment[pos="first"] { border-left: 1px solid #2a3360; border-top-left-radius: 5px;
  border-bottom-left-radius: 5px; }
QToolButton#projSegment[pos="last"] { border-top-right-radius: 5px; border-bottom-right-radius: 5px; }
QToolButton#projSegment:hover { background: #171C38; }
QToolButton#projSegment:checked { background: #4866C5; border-color: #6A84D9; color: white; }
QToolButton#projStep { background: #0f1328; border: 1px solid #2a3360; border-radius: 5px; padding: 3px; }
QToolButton#projStep:hover { background: #171C38; border-color: #4866C5; }
QToolButton#projStep:disabled { background: transparent; border-color: #1f2747; }
QLineEdit { background: #171C38; border: 1px solid #2a3360; border-radius: 5px; padding: 5px 8px; }
QLineEdit:focus { border-color: #4866C5; }
QListWidget#projRecent { background: transparent; border: none; outline: 0; }
QListWidget#projRecent::item { border: none; border-radius: 6px; margin: 1px 0px; }
QListWidget#projRecent::item:hover { background: #171C38; }
QListWidget#projRecent::item:selected { background: #26305e; }
QPushButton#projPrimary { background: #4866C5; color: white; border: none; border-radius: 5px; padding: 7px 22px; }
QPushButton#projPrimary:hover { background: #6A84D9; }
QPushButton#projPrimary:disabled { background: #232A4E; color: #6c7699; }
QPushButton#projFlat { background: transparent; border: 1px solid #346077; border-radius: 5px; padding: 7px 14px; }
QPushButton#projFlat:hover { background: #171C38; }
)";

QString shortPath(const QString &path) {
  return homeShortPath(path);
}

// Name, and the path under it; a missing project is greyed out.
class ProjectRowDelegate final : public QStyledItemDelegate {
public:
  using QStyledItemDelegate::QStyledItemDelegate;

  QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override {
    const QFontMetrics name(option.font);
    const QFontMetrics path(scale::font(scale::Font::Caption, option.font));
    return {option.rect.width(), name.height() + path.height() + scale::px(2) + 2 * scale::px(5)};
  }

  void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);
    const QString name = opt.text;
    opt.text.clear();
    const QWidget *w = opt.widget;
    (w ? w->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);

    const bool exists = index.data(kExistsRole).toBool();
    const QRect r = option.rect.adjusted(scale::px(10), scale::px(5), -scale::px(10), -scale::px(5));
    const QFont pathFont = scale::font(scale::Font::Caption, option.font);
    const QFontMetrics nameFm(option.font), pathFm(pathFont);
    p->save();
    p->setFont(option.font);
    p->setPen(exists ? option.palette.color(QPalette::Text) : QColor(QStringLiteral("#6c7699")));
    p->drawText(QRect(r.left(), r.top(), r.width(), nameFm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                nameFm.elidedText(name, Qt::ElideRight, r.width()));
    p->setFont(pathFont);
    QString where = shortPath(index.data(kRootRole).toString());
    if (!exists)
      where = QObject::tr("missing · %1").arg(where);
    const bool selected = option.state & QStyle::State_Selected;
    p->setPen(!exists ? QColor(QStringLiteral("#f0b45a"))
                      : selected ? QColor(255, 255, 255, 205) : QColor(QStringLiteral("#8a95b5")));
    p->drawText(QRect(r.left(), r.top() + nameFm.height() + scale::px(2), r.width(), pathFm.height()),
                Qt::AlignLeft | Qt::AlignVCenter, elidePath(where, pathFm, r.width()));
    p->restore();
  }
};

} // namespace

QString ProjectDialog::run(QWidget *parent, const Mode mode) {
  ProjectDialog d(parent, mode);
  d.exec();
  return d.m_result;
}

namespace {

// The splash scene. Centre and size in Source units; every face fitted to whole 128-unit tiles.
void buildSplashLevel(vk::Engine3D &engine) {
  double tiles = 0.0;
  (void) engine.newBrushMaterial(tiles);
  const eng::MaterialPtr mat = vk::TextureManager::instance().privateMaterial(prefs::s("Brush/DefaultMaterial"));
  constexpr double kTileWorld = 12.8;
  std::vector<eng::ConvexBrush> brushes;
  const auto add = [&](glm::dvec3 c, glm::dvec3 size, glm::dvec3 axis = {0, 1, 0}, double turn = 0.0) {
    c *= 0.1;
    size *= 0.1;
    eng::ConvexBrush b = eng::ConvexBrush::box(c - size * 0.5, c + size * 0.5, mat, tiles);
    if (turn != 0.0) {
      const glm::dmat4 m = glm::translate(glm::dmat4(1.0), c) *
                           glm::rotate(glm::dmat4(1.0), glm::radians(turn), glm::normalize(axis)) *
                           glm::translate(glm::dmat4(1.0), -c);
      b.transform(m, false);
    }
    b.build();
    for (size_t i = 0; i < b.faces.size() && i < b.polygons.size(); ++i) {
      std::vector<glm::vec3> poly;
      for (const auto &p: b.polygons[i])
        poly.emplace_back(p);
      const glm::vec3 n(b.faces[i].normal);
      eng::FaceTexBasis &basis = b.faces[i].basis;
      eng::fitBasisToFace(basis, poly, n, 1, 1);
      const int ru = std::max(1, int(std::lround(1.0 / (std::abs(basis.tilesPerUnitU) * kTileWorld))));
      const int rv = std::max(1, int(std::lround(1.0 / (std::abs(basis.tilesPerUnitV) * kTileWorld))));
      eng::fitBasisToFace(basis, poly, n, ru, rv);
    }
    brushes.push_back(std::move(b));
  };

  add({0, -16, 0}, {1024, 32, 1024});
  add({-96, 32, -64}, {256, 64, 256});
  add({190, 40, -64}, {336, 24, 192}, {0, 0, 1}, -12.0);
  add({-288, 64, 224}, {24, 128, 24});

  struct Crate {
    glm::dvec3 centre;
    glm::dvec3 axis;
    double turn;
  };
  const glm::dvec3 drops[] = {{-112, 420, -72}, {150, 520, -40}, {40, 660, 64},
                              {-40, 580, -150}, {230, 720, -90}, {-170, 800, 30}};
  QRandomGenerator *rng = QRandomGenerator::global();
  const auto uniform = [rng](double lo, double hi) { return lo + rng->generateDouble() * (hi - lo); };
  std::vector<Crate> crates;
  for (const glm::dvec3 &at: drops) {
    glm::dvec3 axis(uniform(-1.0, 1.0), uniform(-1.0, 1.0), uniform(-1.0, 1.0));
    if (glm::length(axis) < 1e-3)
      axis = {0, 1, 0};
    crates.push_back({at + glm::dvec3(uniform(-24.0, 24.0), 0.0, uniform(-24.0, 24.0)), axis, uniform(0.0, 60.0)});
  }
  for (const Crate &c: crates)
    add(c.centre, {48, 48, 48}, c.axis, c.turn);

  std::vector<eng::Entity> entities;
  eng::Entity lantern;
  lantern.classname = QStringLiteral("light");
  lantern.origin = {-28.8f, 15.0f, 22.4f};
  lantern.props = {{QStringLiteral("light_color"), QStringLiteral("#ffb067")},
                   {QStringLiteral("light_energy"), 2.0},
                   {QStringLiteral("omni_range"), 7.0}};
  entities.push_back(lantern);
  eng::Entity spot;
  spot.classname = QStringLiteral("light_spot");
  spot.origin = {6.0f, 60.0f, -2.0f};
  spot.angles = {-78.0f, 20.0f, 0.0f};
  spot.props = {{QStringLiteral("light_color"), QStringLiteral("#9cc2ff")},
                {QStringLiteral("light_energy"), 8.0},
                {QStringLiteral("spot_range"), 16.0},
                {QStringLiteral("spot_angle"), 28.0}};
  entities.push_back(spot);
  // A key light from the side, aimed at the plateau where the crates land.
  {
    eng::Entity key;
    key.classname = QStringLiteral("light_spot");
    key.origin = {-46.0f, 42.0f, 24.0f};
    const glm::vec3 target(-9.6f, 6.4f, -6.4f);  // the plateau's top, centre
    const glm::vec3 d = glm::normalize(target - key.origin);
    key.angles = {glm::degrees(std::asin(d.y)), glm::degrees(std::atan2(d.x, d.z)), 0.0f};
    key.props = {{QStringLiteral("light_color"), QStringLiteral("#fff1d6")},
                 {QStringLiteral("light_energy"), 16.0},
                 {QStringLiteral("spot_range"), 16.0},
                 {QStringLiteral("spot_angle"), 30.0}};
    entities.push_back(key);
  }
  for (const Crate &c: crates) {
    eng::Entity body;
    body.classname = eng::kRigidBodyClass;
    body.origin = glm::vec3(c.centre * 0.1);
    body.props = {{QStringLiteral("mass"), 20.0}};
    entities.push_back(body);
  }

  preview::setScene(engine, brushes, entities);
  if (const auto &scene = engine.scene()) {
    std::vector<quint32> bodies;
    for (const auto &e: scene->entities)
      if (e.classname == eng::kRigidBodyClass)
        bodies.push_back(e.id);
    const size_t n = scene->gameObjects.size();
    for (size_t i = 0; i < bodies.size() && i < crates.size() && n >= crates.size(); ++i)
      scene->gameObjects[n - crates.size() + i]->setBrushEntity(bodies[i]);
  }

  const QColor background(QString::fromLatin1(kSplashBackground));
  engine.setBackgroundOverride(background);
  engine.setFogColor(background);
  engine.setSunElevation(45.0f);
  engine.setSunAzimuth(300.0f);
  engine.setSunColor(QColor(QStringLiteral("#8ea2e8")));
  engine.setSunIntensity(0.35f);
  engine.setAmbientColor(QColor(QStringLiteral("#2a3566")));
  engine.setAmbientIntensity(0.35f);
}

} // namespace

ProjectDialog::ProjectDialog(QWidget *parent, const Mode mode) : QDialog(parent), m_mode(mode) {
  setWindowTitle(mode == Mode::Create ? tr("New Project") : tr("Open Project"));
  scale::setScaledStyleSheet(this, QString::fromLatin1(kStyle));
  resize(scale::initialSize(this, {1100, 640}));

  auto *root = new QHBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // left: widgets
  auto *side = new QFrame(this);
  side->setObjectName(QStringLiteral("projSide"));
  scale::bind(side, [side] { side->setFixedWidth(scale::tpx(420)); });
  auto *v = new QVBoxLayout(side);
  v->setContentsMargins(20, 20, 20, 16);
  v->setSpacing(10);

  auto *head = new QHBoxLayout;
  head->setSpacing(10);
  auto *icon = new QLabel(side);
  scale::bind(icon, [icon] {
    const int s = scale::px(40);
    icon->setPixmap(QPixmap(QStringLiteral(":/assets/icon.png")).scaled(s, s, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  });
  auto *names = new QVBoxLayout;
  names->setSpacing(0);
  auto *title = new QLabel(QStringLiteral("gHammer " GHAMMER_VERSION), side);
  title->setObjectName(QStringLiteral("projTitle"));
  scale::setRole(title, scale::Font::PageTitle);
  auto *byline = new QLabel(tr("by Sander Ferdinand - kroket.io"), side);
  byline->setObjectName(QStringLiteral("projCaption"));
  scale::setRole(byline, scale::Font::Caption);
  names->addWidget(title);
  names->addWidget(byline);
  head->addWidget(icon, 0, Qt::AlignVCenter);
  head->addLayout(names, 1);
  v->addLayout(head);
  v->addSpacing(12);

  m_pages = new QStackedWidget(side);
  m_pages->addWidget(buildRecentPage());
  m_pages->addWidget(buildNewPage());
  v->addWidget(m_pages, 1);
  v->addWidget(buildServerControls(side));
  v->addWidget(buildScaleControls(side));
  root->addWidget(side);

  // right: the scene
  m_scene = new QQuickWidget(this);
  m_scene->setResizeMode(QQuickWidget::SizeRootObjectToView);
  m_scene->setClearColor(QColor(QString::fromLatin1(kSplashBackground)));
  m_scene->setSource(QUrl(QStringLiteral("qrc:/Main/qml/ProjectSplash.qml")));
  if (QQuickItem *item = m_scene->rootObject()) {
    scale::bind(item, [item] { item->setProperty("textPx", scale::tpx(13)); });
    m_splash = preview::newEngine(this);
    m_splash->camera()->setFOV(42.0f);
    m_splash->setNoSpecular(true);
    buildSplashLevel(*m_splash);
    preview::attach(item, "view", m_splash);
    if (prefs::b("Debug/Physics")) {
      const auto drop = [this] {
        m_splash->physicsStop();
        buildSplashLevel(*m_splash);
        m_splash->physicsPlay(false);
      };
      auto *loop = new QTimer(this);
      loop->setInterval(kSplashDropMs);
      connect(loop, &QTimer::timeout, this, drop);
      QTimer::singleShot(600, this, [drop, loop] {
        drop();
        loop->start();
      });
    }
    connect(item, SIGNAL(viewChanged()), this, SLOT(updateSplashCamera()));
    updateSplashCamera();
  } else {
    qWarning() << "[project] splash scene:" << m_scene->errors();
  }
  root->addWidget(m_scene, 1);

  rebuildRecent();
  showPage(mode == Mode::Create || m_recent->count() == 0 ? PageNew : PageRecent);
}

void ProjectDialog::updateSplashCamera() {
  if (m_splash && m_scene->rootObject())
    preview::orbit(*m_splash, glm::vec3(0.0f, 4.0f, 0.0f), m_scene->rootObject()->property("angle").toFloat(), 24.0f,
                   115.0f);
}

QWidget *ProjectDialog::buildRecentPage() {
  auto *page = new QWidget(this);
  auto *v = new QVBoxLayout(page);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(8);
  auto *heading = new QLabel(tr("PROJECTS"), page);
  heading->setObjectName(QStringLiteral("projSection"));
  v->addWidget(heading);

  m_recent = new QListWidget(page);
  m_recent->setObjectName(QStringLiteral("projRecent"));
  m_recent->setSelectionMode(QAbstractItemView::SingleSelection);
  m_recent->setContextMenuPolicy(Qt::CustomContextMenu);
  m_recent->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_recent->setItemDelegate(new ProjectRowDelegate(m_recent));
  v->addWidget(m_recent, 1);
  m_recentEmpty = new QLabel(tr("No projects"), page);
  m_recentEmpty->setObjectName(QStringLiteral("projCaption"));
  m_recentEmpty->setAlignment(Qt::AlignCenter);
  v->addWidget(m_recentEmpty, 1);
  m_openProblem = new QLabel(page);
  m_openProblem->setObjectName(QStringLiteral("projProblem"));
  m_openProblem->setWordWrap(true);
  m_openProblem->hide();
  v->addWidget(m_openProblem);

  auto *buttons = new QHBoxLayout;
  auto *create = new QPushButton(ActionManager::icon(QStringLiteral("add")), tr("New Project…"), page);
  create->setObjectName(QStringLiteral("projFlat"));
  auto *other = new QPushButton(tr("Open Other…"), page);
  other->setObjectName(QStringLiteral("projFlat"));
  m_open = new QPushButton(tr("Open"), page);
  m_open->setObjectName(QStringLiteral("projPrimary"));
  buttons->addWidget(create);
  buttons->addWidget(other);
  buttons->addStretch(1);
  buttons->addWidget(m_open);
  v->addLayout(buttons);
  connect(create, &QPushButton::clicked, this, [this] { showPage(PageNew); });

  // A missing project does nothing.
  connect(m_recent, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
    m_recent->setCurrentItem(it);
    openSelected();
  });
  connect(m_recent, &QListWidget::itemActivated, this, [this] { openSelected(); });
  connect(m_recent, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *cur) {
    m_open->setEnabled(cur && cur->data(kExistsRole).toBool());
  });
  connect(m_recent, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
    QListWidgetItem *it = m_recent->itemAt(pos);
    // Only recents can be removed; the others are what is in ~/GodotProjects.
    if (!it || !it->data(kRecentRole).toBool())
      return;
    QMenu menu(this);
    menu.addAction(tr("Remove from List"), this, [this, it] {
      proj::ProjectManager::instance().forget(it->data(kRootRole).toString());
      rebuildRecent();
    });
    menu.exec(m_recent->viewport()->mapToGlobal(pos));
  });
  connect(m_open, &QPushButton::clicked, this, &ProjectDialog::openSelected);
  connect(other, &QPushButton::clicked, this, &ProjectDialog::openOther);
  auto *forget = new QAction(m_recent);
  forget->setShortcut(QKeySequence::Delete);
  forget->setShortcutContext(Qt::WidgetShortcut);
  m_recent->addAction(forget);
  connect(forget, &QAction::triggered, this, [this] {
    if (QListWidgetItem *it = m_recent->currentItem(); it && it->data(kRecentRole).toBool()) {
      proj::ProjectManager::instance().forget(it->data(kRootRole).toString());
      rebuildRecent();
    }
  });
  return page;
}

QWidget *ProjectDialog::buildNewPage() {
  auto *page = new QWidget(this);
  auto *v = new QVBoxLayout(page);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(6);
  auto *heading = new QLabel(tr("NEW PROJECT"), page);
  heading->setObjectName(QStringLiteral("projSection"));
  v->addWidget(heading);
  v->addSpacing(4);

  auto *nameLabel = new QLabel(tr("Name"), page);
  m_name = new QLineEdit(page);
  m_name->setPlaceholderText(tr("my_game"));
  v->addWidget(nameLabel);
  v->addWidget(m_name);
  v->addSpacing(6);

  auto *locLabel = new QLabel(tr("Location"), page);
  auto *locRow = new QHBoxLayout;
  m_location = new QLineEdit(proj::Project::standardLocation(), page);
  auto *browse = new QPushButton(tr("Browse…"), page);
  browse->setObjectName(QStringLiteral("projFlat"));
  locRow->addWidget(m_location, 1);
  locRow->addWidget(browse);
  v->addWidget(locLabel);
  v->addLayout(locRow);
  v->addSpacing(6);

  m_resultPath = new QLabel(page);
  m_resultPath->setObjectName(QStringLiteral("projCaption"));
  m_resultPath->setWordWrap(true);
  m_resultPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(m_resultPath);

  auto *problemRow = new QHBoxLayout;
  m_problem = new QLabel(page);
  m_problem->setObjectName(QStringLiteral("projProblem"));
  m_problem->setWordWrap(true);
  m_openExisting = new QPushButton(tr("Open It"), page);
  m_openExisting->setObjectName(QStringLiteral("projFlat"));
  problemRow->addWidget(m_problem, 1);
  problemRow->addWidget(m_openExisting);
  v->addLayout(problemRow);
  v->addStretch(1);

  auto *buttons = new QHBoxLayout;
  auto *back = new QPushButton(tr("Back"), page);
  back->setObjectName(QStringLiteral("projFlat"));
  buttons->addWidget(back);
  buttons->addStretch(1);
  m_create = new QPushButton(tr("Create"), page);
  m_create->setObjectName(QStringLiteral("projPrimary"));
  buttons->addWidget(m_create);
  v->addLayout(buttons);
  connect(back, &QPushButton::clicked, this, [this] { showPage(PageRecent); });

  connect(m_name, &QLineEdit::textChanged, this, &ProjectDialog::validateNew);
  connect(m_location, &QLineEdit::textChanged, this, &ProjectDialog::validateNew);
  connect(m_name, &QLineEdit::returnPressed, this, [this] {
    if (m_create->isEnabled())
      createProject();
  });
  connect(browse, &QPushButton::clicked, this, [this] {
    const QString dir = FileDialog::getExistingDirectory(this, tr("Project Location"), m_location->text(),
                                                         tr("Select Folder"));
    if (!dir.isEmpty())
      m_location->setText(QDir::toNativeSeparators(dir));
  });
  connect(m_openExisting, &QPushButton::clicked, this, [this] {
    accept(QDir(QDir::fromNativeSeparators(m_location->text().trimmed())).filePath(m_name->text()));
  });
  connect(m_create, &QPushButton::clicked, this, &ProjectDialog::createProject);
  validateNew();
  return page;
}

QWidget *ProjectDialog::buildScaleControls(QWidget *parent) {
  // General/UiScale and FontScale; − / + step through the preference's values.
  auto *box = new QFrame(parent);
  box->setObjectName(QStringLiteral("projScale"));
  auto *grid = new QGridLayout(box);
  grid->setContentsMargins(0, 10, 0, 0);
  grid->setHorizontalSpacing(8);
  grid->setVerticalSpacing(6);
  int row = 0;
  for (const char *k: {"General/UiScale", "General/FontScale"}) {
    const QString key = QString::fromLatin1(k);
    const prefs::Def *def = prefs::get().def(key);
    if (!def || def->choiceValues.isEmpty())
      continue;
    auto *label = new QLabel(def->label, box);
    label->setToolTip(def->tooltip);
    auto *down = new QToolButton(box);
    down->setObjectName(QStringLiteral("projStep"));
    down->setIcon(ActionManager::icon(QStringLiteral("remove")));
    down->setToolTip(tr("Smaller"));
    auto *value = new QLabel(box);
    value->setAlignment(Qt::AlignCenter);
    value->setToolTip(def->tooltip);
    auto *up = new QToolButton(box);
    up->setObjectName(QStringLiteral("projStep"));
    up->setIcon(ActionManager::icon(QStringLiteral("add")));
    up->setToolTip(tr("Larger"));
    // Copies: the registry's list can grow (and move) once the window registers its actions.
    const QVariantList steps = def->choiceValues;
    const QStringList names = def->choices;
    scale::bind(value, [value, names] {
      // Wide enough for the widest step.
      int w = 0;
      const QFontMetrics fm(value->font());
      for (const QString &c: names)
        w = std::max(w, fm.horizontalAdvance(c));
      value->setFixedWidth(w + scale::tpx(12));
    });
    const auto indexOf = [steps](const QVariant &v) {
      int best = 0;
      for (int i = 0; i < steps.size(); ++i)
        if (std::abs(steps[i].toDouble() - v.toDouble()) < std::abs(steps[best].toDouble() - v.toDouble()))
          best = i;
      return best;
    };
    const auto show = [value, down, up, names, steps, indexOf](const QVariant &v) {
      const int i = indexOf(v);
      value->setText(names.value(i));
      down->setEnabled(i > 0);
      up->setEnabled(i + 1 < steps.size());
    };
    const auto step = [key, steps, indexOf](int dir) {
      const int i = std::clamp(indexOf(prefs::get().value(key)) + dir, 0, int(steps.size()) - 1);
      prefs::get().setValue(key, steps[i]);
    };
    connect(down, &QToolButton::clicked, this, [step] { step(-1); });
    connect(up, &QToolButton::clicked, this, [step] { step(+1); });
    prefs::get().watch(key, value, show);

    grid->addWidget(label, row, 0);
    grid->addWidget(down, row, 1);
    grid->addWidget(value, row, 2);
    grid->addWidget(up, row, 3);
    ++row;
  }
  grid->setColumnStretch(0, 1);
  return box;
}

QWidget *ProjectDialog::buildServerControls(QWidget *parent) {
  // Server/Mode: Local or Remote.
  auto *box = new QFrame(parent);
  box->setObjectName(QStringLiteral("projServer"));
  auto *grid = new QGridLayout(box);
  grid->setContentsMargins(0, 10, 0, 0);
  grid->setHorizontalSpacing(8);
  grid->setVerticalSpacing(4);
  auto *label = new QLabel(tr("Asset server"), box);
  auto *local = new QToolButton(box);
  local->setObjectName(QStringLiteral("projSegment"));
  local->setProperty("pos", "first");
  local->setText(tr("Local"));
  local->setToolTip(tr("Run an asset server on this computer"));
  local->setCheckable(true);
  auto *remote = new QToolButton(box);
  remote->setObjectName(QStringLiteral("projSegment"));
  remote->setProperty("pos", "last");
  remote->setText(tr("Remote"));
  remote->setToolTip(tr("Connect to an asset server on another computer"));
  remote->setCheckable(true);
  auto *group = new QButtonGroup(box);
  group->addButton(local);
  group->addButton(remote);
  auto *caption = new QLabel(box);
  caption->setObjectName(QStringLiteral("projCaption"));
  scale::setRole(caption, scale::Font::Caption);
  caption->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto *settings = new QToolButton(box);
  settings->setObjectName(QStringLiteral("projStep"));
  settings->setIcon(ActionManager::icon(QStringLiteral("preferences")));
  settings->setToolTip(tr("Asset server settings"));
  auto *segments = new QHBoxLayout;
  segments->setSpacing(0);
  segments->addWidget(local);
  segments->addWidget(remote);
  grid->addWidget(label, 0, 0);
  grid->addLayout(segments, 0, 1);
  grid->addWidget(settings, 0, 2);
  grid->addWidget(caption, 1, 0, 1, 3);
  grid->setColumnStretch(0, 1);

  const auto refresh = [local, remote, caption] {
    const bool isRemote = prefs::s("Server/Mode") == QLatin1String("remote");
    local->setChecked(!isRemote);
    remote->setChecked(isRemote);
    const auto &s = net::LocalServer::instance();
    QString text;
    if (!qEnvironmentVariable("EDITOR_SERVER_URL").isEmpty())
      text = qEnvironmentVariable("EDITOR_SERVER_URL");
    else if (isRemote)
      text = prefs::s("Server/Url");
    else if (s.state() == net::LocalServer::State::Running)
      text = tr("Running · this computer only");
    else if (s.state() == net::LocalServer::State::Starting)
      text = s.lastError().isEmpty() ? tr("Starting…") : s.lastError();
    else
      text = s.lastError();
    caption->setText(text);
    caption->setVisible(!text.isEmpty());
  };
  connect(local, &QToolButton::clicked, this, [] {
    prefs::get().setValue(QStringLiteral("Server/Mode"), QStringLiteral("local"));
    // The main window's AssetClient takes over later; start loading now.
    if (qEnvironmentVariableIsEmpty("EDITOR_SERVER_URL"))
      net::LocalServer::instance().start();
  });
  connect(remote, &QToolButton::clicked, this, [this] {
    prefs::get().setValue(QStringLiteral("Server/Mode"), QStringLiteral("remote"));
    if (prefs::s("Server/Url").trimmed().isEmpty())
      prefs::PreferencesDialog::showDialog(this, QStringLiteral("Asset Server"));
  });
  connect(settings, &QToolButton::clicked, this,
          [this] { prefs::PreferencesDialog::showDialog(this, QStringLiteral("Asset Server")); });
  for (const char *key: {"Server/Mode", "Server/Url"})
    prefs::get().watch(QString::fromLatin1(key), caption, [refresh](const QVariant &) { refresh(); });
  connect(&net::LocalServer::instance(), &net::LocalServer::stateChanged, caption, refresh);
  connect(&net::LocalServer::instance(), &net::LocalServer::ready, caption, refresh);
  return box;
}

void ProjectDialog::showPage(const int page) {
  m_pages->setCurrentIndex(page);
  if (page == PageNew) {
    m_name->setFocus();
  } else {
    m_recent->setFocus();
  }
}

void ProjectDialog::rebuildRecent() {
  m_recent->clear();
  for (const auto &r: proj::ProjectManager::instance().projectList()) {
    auto *it = new QListWidgetItem(m_recent);
    it->setData(Qt::DisplayRole, r.name);
    it->setData(kRootRole, r.root);
    it->setData(kExistsRole, r.exists);
    it->setData(kRecentRole, r.recent);
    it->setToolTip(r.exists ? QDir::toNativeSeparators(r.root) : tr("Missing: %1").arg(QDir::toNativeSeparators(r.root)));
  }
  const bool empty = m_recent->count() == 0;
  m_recent->setVisible(!empty);
  m_recentEmpty->setVisible(empty);
  for (int i = 0; i < m_recent->count(); ++i)
    if (m_recent->item(i)->data(kExistsRole).toBool()) {
      m_recent->setCurrentRow(i);
      break;
    }
  m_open->setEnabled(m_recent->currentItem() && m_recent->currentItem()->data(kExistsRole).toBool());
}

void ProjectDialog::openSelected() {
  QListWidgetItem *it = m_recent->currentItem();
  if (!it || !it->data(kExistsRole).toBool())
    return;
  accept(it->data(kRootRole).toString());
}

void ProjectDialog::openOther() {
  QString start = proj::Project::standardLocation();
  if (!QFileInfo(start).isDir())
    start = QDir::homePath();
  const QString dir = FileDialog::getExistingDirectory(this, tr("Open Project"), start, tr("Open Project"));
  if (dir.isEmpty())
    return;
  const QString root = proj::Project::findEnclosing(dir);
  if (root.isEmpty()) {
    m_openProblem->setText(tr("%1 is not a project").arg(shortPath(dir)));
    m_openProblem->show();
    return;
  }
  accept(root);
}

void ProjectDialog::validateNew() {
  const QString location = QDir::fromNativeSeparators(m_location->text().trimmed());
  const QString name = m_name->text();
  bool existing = false;
  const QString why = proj::Project::validateName(location, name, &existing);
  const bool typed = !name.isEmpty();
  m_resultPath->setText(typed ? QDir::toNativeSeparators(QDir(location).filePath(name.trimmed())) : QString());
  // No warning for an empty name.
  m_problem->setText(typed ? why : QString());
  m_problem->setVisible(typed && !why.isEmpty());
  m_openExisting->setVisible(typed && existing);
  m_create->setEnabled(why.isEmpty());
}

void ProjectDialog::createProject() {
  const QString location = QDir::fromNativeSeparators(m_location->text().trimmed());
  const QString name = m_name->text();
  QString error;
  if (!QDir().mkpath(location)) {
    m_problem->setText(tr("Cannot create %1").arg(QDir::toNativeSeparators(location)));
    m_problem->show();
    return;
  }
  const QString root = proj::ProjectManager::createProject(location, name, &error);
  if (root.isEmpty()) {
    m_problem->setText(error);
    m_problem->show();
    return;
  }
  accept(root);
}

void ProjectDialog::accept(const QString &root) {
  m_result = QDir::cleanPath(root);
  QDialog::accept();
}

} // namespace ui
