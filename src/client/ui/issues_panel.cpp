#include "issues_panel.h"

#include <QCheckBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cfloat>
#include <map>

#include "client/engine/brush.h"
#include "client/engine/entity.h"
#include "client/engine/io.h"
#include "client/engine/scene.h"
#include "client/ui/action_manager.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

enum Severity { Error = 0, Warning = 1, Info = 2 };
enum Role { BrushRole = Qt::UserRole + 1, EntityRole, FixRole, ConnRole };

} // namespace

IssuesPanel::IssuesPanel(vk::Engine3D *engine, QWidget *parent) : QWidget(parent), m_engine(engine) {
  auto *v = new QVBoxLayout(this);
  v->setContentsMargins(4, 4, 4, 4);
  auto *bar = new QHBoxLayout();
  auto *run = new QPushButton(ActionManager::icon(QStringLiteral("checkmark")), tr("Check map"), this);
  m_auto = new QCheckBox(tr("Check automatically"), this);
  m_auto->setChecked(true);
  m_summary = new QLabel(this);
  bar->addWidget(run);
  bar->addWidget(m_auto);
  bar->addStretch(1);
  bar->addWidget(m_summary);
  v->addLayout(bar);
  m_list = new QTreeWidget(this);
  m_list->setColumnCount(3);
  m_list->setHeaderLabels({tr("Issue"), tr("Object"), tr("Fix")});
  m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_list->setRootIsDecorated(false);
  m_list->setContextMenuPolicy(Qt::CustomContextMenu);
  v->addWidget(m_list, 1);

  m_timer = new QTimer(this);
  m_timer->setSingleShot(true);
  m_timer->setInterval(1500);
  connect(m_timer, &QTimer::timeout, this, [this] {
    if (m_auto->isChecked() && isVisible())
      runChecks();
  });
  connect(m_engine, &vk::Engine3D::sceneChanged, m_timer, qOverload<>(&QTimer::start));
  connect(m_engine, &vk::Engine3D::entitiesChanged, m_timer, qOverload<>(&QTimer::start));
  connect(run, &QPushButton::clicked, this, &IssuesPanel::runChecks);

  auto select = [this](QTreeWidgetItem *it) {
    if (!it)
      return;
    const quint32 brush = it->data(0, BrushRole).toUInt();
    const quint32 ent = it->data(0, EntityRole).toUInt();
    if (brush) {
      m_engine->setSelected({brush}, {});
      m_engine->setSelectedEntities({});
    } else if (ent) {
      m_engine->setSelected({}, {});
      m_engine->selectEntitiesWithBrushes({ent});
    }
    m_engine->focusSelection();
  };
  connect(m_list, &QTreeWidget::itemDoubleClicked, this, select);
  connect(m_list, &QTreeWidget::customContextMenuRequested, this, [this, select](const QPoint &pos) {
    QTreeWidgetItem *it = m_list->itemAt(pos);
    if (!it)
      return;
    QMenu menu(this);
    menu.addAction(tr("Select"), this, [it, select] { select(it); });
    const QString fix = it->data(0, FixRole).toString();
    if (fix == QLatin1String("delete")) {
      menu.addAction(ActionManager::icon(QStringLiteral("trash")), tr("Delete it"), this, [this, it, select] {
        select(it);
        m_engine->deleteSelected();
        runChecks();
      });
    } else if (fix == QLatin1String("io.delete")) {
      menu.addAction(ActionManager::icon(QStringLiteral("trash")), tr("Delete this output"), this, [this, it] {
        const quint32 ent = it->data(0, EntityRole).toUInt();
        const int conn = it->data(0, ConnRole).toInt();
        if (const eng::Entity *e = m_engine->entityById(ent); e && conn >= 0 && conn < e->outputs.size()) {
          auto list = e->outputs;
          list.removeAt(conn);
          m_engine->setEntityOutputs(ent, list, tr("Delete output"));
        }
        runChecks();
      });
    } else if (fix == QLatin1String("snap")) {
      menu.addAction(ActionManager::icon(QStringLiteral("snap_grid")), tr("Snap vertices to grid"), this,
                     [this, it, select] {
                       select(it);
                       m_engine->snapSelectionToGrid();
                       runChecks();
                     });
    }
    menu.exec(m_list->viewport()->mapToGlobal(pos));
  });
}

void IssuesPanel::runChecks() {
  QElapsedTimer t;
  t.start();
  m_list->clear();
  auto *scene = m_engine->scene().data();
  int counts[3] = {0, 0, 0};
  auto add = [this, &counts](Severity sev, const QString &text, const QString &object, quint32 brush, quint32 ent,
                             const QString &fix = {}, int conn = -1) {
    auto *it = new QTreeWidgetItem(m_list, {text, object, fix == QLatin1String("delete")      ? tr("delete")
                                                          : fix == QLatin1String("snap")      ? tr("snap to grid")
                                                          : fix == QLatin1String("io.delete") ? tr("delete output")
                                                                                              : QString()});
    it->setData(0, ConnRole, conn);
    it->setIcon(0, ActionManager::icon(sev == Error ? QStringLiteral("error")
                                       : sev == Warning ? QStringLiteral("info") : QStringLiteral("question")));
    it->setForeground(0, sev == Error ? QColor(255, 120, 120) : sev == Warning ? QColor(240, 190, 100) : QColor(170, 180, 210));
    it->setData(0, BrushRole, brush);
    it->setData(0, EntityRole, ent);
    it->setData(0, FixRole, fix);
    ++counts[sev];
  };
  if (!scene) {
    m_summary->clear();
    return;
  }
  glm::vec3 mapMin(FLT_MAX), mapMax(-FLT_MAX);
  std::map<std::tuple<int, int, int, int, int, int>, quint32> boxes;
  int checked = 0;
  for (const auto &obj: scene->gameObjects) {
    if (obj->isDerived())
      continue;
    const QString label = obj->name().isEmpty() ? tr("Brush %1").arg(obj->stableId()) : obj->name();
    const auto [lo, hi] = obj->getWorldAABB();
    mapMin = glm::min(mapMin, lo);
    mapMax = glm::max(mapMax, hi);
    // Duplicates: same bounds to 1/100 unit.
    const auto key = std::make_tuple(int(std::lround(lo.x * 100)), int(std::lround(lo.y * 100)),
                                     int(std::lround(lo.z * 100)), int(std::lround(hi.x * 100)),
                                     int(std::lround(hi.y * 100)), int(std::lround(hi.z * 100)));
    if (auto it = boxes.find(key); it != boxes.end()) {
      add(Warning, tr("Duplicate brush (same bounds as brush %1)").arg(it->second), label, obj->stableId(), 0,
          QStringLiteral("delete"));
    } else {
      boxes.emplace(key, obj->stableId());
    }
    if (++checked > 20000)
      continue; // the expensive per-brush checks are capped on giant maps
    const auto mesh = obj->getMesh();
    bool missing = false;
    for (int f = 0; mesh && f < mesh->faceCount() && !missing; ++f) {
      const auto m = mesh->faceMaterialRef(f);
      missing = !m || m->state == eng::Material::State::Error;
    }
    if (missing)
      add(Warning, tr("Uses a texture that is missing or unreachable (shows the checkerboard)"), label,
          obj->stableId(), 0);
    if (mesh && mesh->faceCount() > 6 || (mesh && mesh->hasFaceRanges())) {
      const auto b = eng::ConvexBrush::fromObject(*obj);
      if (!b || !b->valid())
        add(Error, tr("Not a valid convex brush"), label, obj->stableId(), 0, QStringLiteral("delete"));
    }
    // Off-grid vertices (Source wants integer coordinates).
    bool offGrid = false;
    if (mesh) {
      const glm::mat4 model = obj->getModelMatrix();
      for (const auto &vx: mesh->vertexData()) {
        const glm::vec3 w = glm::vec3(model * glm::vec4(vx.position, 1.0f)) * float(vk::Engine3D::kSourceUnitsPerWorldUnit);
        if (glm::length(w - glm::round(w)) > 0.01f) {
          offGrid = true;
          break;
        }
      }
    }
    if (offGrid)
      add(Info, tr("Vertices are off the 1-unit grid"), label, obj->stableId(), 0, QStringLiteral("snap"));
  }

  int starts = 0, suns = 0;
  for (const eng::Entity &e: scene->entities) {
    const eng::EntityClass *c = e.cls();
    const QString label = e.name.isEmpty() ? (c ? c->label : e.classname) : e.name;
    if (!c) {
      add(Warning, tr("Unknown entity class '%1'").arg(e.classname), label, 0, e.id, QStringLiteral("delete"));
      continue;
    }
    if (c->kind == eng::EntityClass::Kind::PlayerStart)
      ++starts;
    if (c->kind == eng::EntityClass::Kind::Sun)
      ++suns;
    if (c->kind == eng::EntityClass::Kind::Prop) {
      const QString model = e.value(QStringLiteral("model")).toString();
      if (model.isEmpty())
        add(Warning, tr("Prop has no model"), label, 0, e.id);
      else if (!QFileInfo::exists(model))
        add(Error, tr("Model file not found: %1").arg(model), label, 0, e.id);
    }
    if ((c->kind == eng::EntityClass::Kind::PointLight || c->kind == eng::EntityClass::Kind::SpotLight)
        && (e.num(QStringLiteral("light_energy")) <= 0.0 || eng::lightRangeMeters(e) <= 0.0))
      add(Warning, tr("Light has no effect (energy or range is 0)"), label, 0, e.id);
    if (mapMin.x <= mapMax.x) {
      const glm::vec3 pad(64.0f);
      if (glm::any(glm::lessThan(e.origin, mapMin - pad)) || glm::any(glm::greaterThan(e.origin, mapMax + pad)))
        add(Warning, tr("Entity is far outside the map"), label, 0, e.id);
    }
  }
  // Entity I/O (engine/io.cpp validate()).
  for (const auto &is: eng::io::validate(scene->entities)) {
    const eng::Entity *e = scene->findEntity(is.entity);
    if (!e)
      continue;
    const eng::EntityClass *c = e->cls();
    const QString label = e->name.isEmpty() ? (c ? c->label : e->classname) : e->name;
    QString text = is.text;
    if (is.connection >= 0 && is.connection < e->outputs.size()) {
      const auto &cn = e->outputs[is.connection];
      text = QStringLiteral("%1 → %2 → %3: %4").arg(cn.output, cn.target, cn.input, is.text);
    }
    const Severity sev = is.severity == eng::io::Issue::Severity::Error     ? Error
                         : is.severity == eng::io::Issue::Severity::Warning ? Warning
                                                                            : Info;
    add(sev, text, label, 0, e->id, is.connection >= 0 ? QStringLiteral("io.delete") : QString(), is.connection);
  }

  if (starts == 0 && !scene->gameObjects.empty())
    add(Info, tr("No info_player_start in the map"), QString(), 0, 0);
  if (suns > 1)
    add(Warning, tr("%1 sun entities: only the first one is used").arg(suns), QString(), 0, 0);

  const int total = counts[0] + counts[1] + counts[2];
  m_summary->setText(total == 0 ? tr("No issues (%1 ms)").arg(t.elapsed())
                                : QStringLiteral("<span style='color:#ff7878'>%1 errors</span> · "
                                                 "<span style='color:#f0be64'>%2 warnings</span> · %3 notes")
                                      .arg(counts[0])
                                      .arg(counts[1])
                                      .arg(counts[2]));
  emit countChanged(total);
}

} // namespace ui
