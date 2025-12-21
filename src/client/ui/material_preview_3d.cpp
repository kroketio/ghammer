#include "material_preview_3d.h"

#include <algorithm>

#include <QQuickItem>
#include <QTimer>

#include "client/ui/scale.h"
#include "client/ui/scene_preview.h"
#include "client/vulkan/vk_engine.h"
#include "client/vulkan/vk_shared.h"

namespace ui {

namespace {

// World units (10 Source units): a 128-unit cube, a sphere as wide, a 128 x 128 plate.
constexpr double kHalf = 6.4;

float baseDistance(MaterialPreview3D::Shape s) {
  switch (s) {
    case MaterialPreview3D::Shape::Sphere: return 30.0f;
    case MaterialPreview3D::Shape::Plane: return 28.0f;
    case MaterialPreview3D::Shape::Cube: break;
  }
  return 46.0f;
}

} // namespace

MaterialPreview3D::MaterialPreview3D(QWidget *parent) : QQuickWidget(parent) {
  setResizeMode(QQuickWidget::SizeRootObjectToView);
  setClearColor(QColor(0x0f, 0x13, 0x26));
  setToolTip(tr("Drag to turn · right-drag or Shift+drag to move the light · wheel to zoom · "
                "double-click to reset"));
  m_pendingPoll = new QTimer(this);
  m_pendingPoll->setInterval(250);
  connect(m_pendingPoll, &QTimer::timeout, this, &MaterialPreview3D::updatePending);
}

void MaterialPreview3D::showEvent(QShowEvent *e) {
  ensureLoaded();
  QQuickWidget::showEvent(e);
}

void MaterialPreview3D::ensureLoaded() {
  if (m_loaded)
    return;
  m_loaded = true;
  setSource(QUrl(QStringLiteral("qrc:/Main/qml/MaterialPreview3D.qml")));
  QQuickItem *root = rootObject();
  if (!root) {
    qWarning() << "[preview3d] failed to load the scene:" << errors();
    return;
  }
  scale::bind(this, [this] {
    if (QQuickItem *r = rootObject())
      r->setProperty("textPx", scale::fontPx(scale::Font::Caption));
  });
  m_engine = preview::newEngine(this);
  m_engine->camera()->setFOV(35.0f);
  m_sunIntensity = m_engine->sunIntensity();
  preview::attach(root, "view", m_engine);
  connect(root, SIGNAL(viewChanged()), this, SLOT(updateView()));
  if (m_entry.isValid())
    m_material = vk::TextureManager::instance().privateMaterial(m_entry, m_props);
  rebuildScene();
}

void MaterialPreview3D::setShape(Shape s) {
  if (m_shape == s)
    return;
  m_shape = s;
  rebuildScene();
}

void MaterialPreview3D::setLighting(Lighting l) {
  if (m_lighting == l)
    return;
  m_lighting = l;
  updateView();
}

void MaterialPreview3D::showTexture(const TextureEntry &entry, const MaterialProps &props) {
  if (entry == m_entry && props == m_props)
    return;
  const bool newTexture = !(entry == m_entry) || !m_material;
  m_entry = entry;
  m_props = props;
  if (!m_engine)
    return;
  auto &tm = vk::TextureManager::instance();
  if (newTexture) {
    m_material = tm.privateMaterial(entry, props);
    rebuildScene();
  } else {
    tm.setPrivateProps(*m_material, props);
    m_engine->requestRedraw();
  }
}

void MaterialPreview3D::clear() {
  m_entry = {};
  m_props = {};
  m_material.reset();
  rebuildScene();
}

void MaterialPreview3D::rebuildScene() {
  if (!m_engine)
    return;
  std::vector<eng::ConvexBrush> brushes;
  if (m_material) {
    switch (m_shape) {
      case Shape::Cube:
        brushes.push_back(preview::fittedBox(glm::dvec3(-kHalf), glm::dvec3(kHalf), m_material));
        break;
      case Shape::Plane:
        brushes.push_back(
            preview::fittedBox(glm::dvec3(-kHalf, -kHalf, -0.2), glm::dvec3(kHalf, kHalf, 0.2), m_material));
        break;
      case Shape::Sphere:
        brushes.push_back(preview::sphere(kHalf, 32, 16, m_material, 3.0, 1.5));
        break;
    }
  }
  preview::setScene(*m_engine, brushes, {}, m_shape == Shape::Sphere);
  updateView();
  updatePending();
}

void MaterialPreview3D::updateView() {
  QQuickItem *root = rootObject();
  if (!m_engine || !root)
    return;
  const float yaw = root->property("yaw").toFloat();
  const float pitch = root->property("pitch").toFloat();
  const float distance = baseDistance(m_shape) * root->property("zoom").toFloat();
  preview::orbit(*m_engine, glm::vec3(0.0f), yaw, pitch, distance);

  // The light is fixed relative to the camera.
  const float lightYaw = yaw + root->property("lightYaw").toFloat();
  const float lightElevation = std::clamp(-root->property("lightPitch").toFloat(), -60.0f, 89.0f);
  const glm::vec3 toward = preview::orbitDir(lightYaw, lightElevation);
  if (m_lighting == Lighting::Sun) {
    m_engine->setSunDirection(-toward);
    m_engine->setSunIntensity(m_sunIntensity);
    m_engine->setPreviewEntities({});
  } else {
    m_engine->setSunIntensity(0.0f);
    eng::Entity light;
    light.classname = QStringLiteral("light");
    light.origin = toward * (distance * 0.6f);
    light.props = {{QStringLiteral("light_color"), QStringLiteral("#fff0dc")},
                   {QStringLiteral("light_energy"), 2.0},
                   {QStringLiteral("omni_range"), 12.0}};
    m_engine->setPreviewEntities({light});
  }
}

void MaterialPreview3D::updatePending() {
  const bool pending = m_material && m_material->state == eng::Material::State::Pending;
  if (QQuickItem *root = rootObject())
    root->setProperty("pending", pending ? 1 : 0);
  if (pending)
    m_pendingPoll->start();
  else
    m_pendingPoll->stop();
}

} // namespace ui
