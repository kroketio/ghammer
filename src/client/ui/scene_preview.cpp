#include "scene_preview.h"

#include <cmath>

#include <QQuickItem>

#include <glm/gtc/constants.hpp>

#include "client/engine/brush_deform.h"
#include "client/engine/gameobject.h"
#include "client/engine/scene.h"
#include "client/vulkan/vk_common.h"
#include "client/vulkan/vk_engine.h"
#include "client/vulkan/vk_viewport3d.h"

namespace ui::preview {

vk::Engine3D *newEngine(QObject *parent) {
  auto *e = new vk::Engine3D(parent, vk::Engine3D::Role::Preview);
  e->setRenderMode(int(vk::RenderMode::Component));
  setScene(*e, {});
  return e;
}

void setScene(vk::Engine3D &engine, const std::vector<eng::ConvexBrush> &brushes,
              const std::vector<eng::Entity> &entities, const bool smooth) {
  auto scene = QSharedPointer<eng::Scene>(new eng::Scene());
  for (const auto &b: brushes)
    if (auto obj = engine.objectFromBrush(b)) {
      if (smooth)
        obj->setSmoothShading(true);
      scene->addObject(obj);
    }
  engine.setScene(scene);
  engine.setPreviewEntities(entities);
}

glm::vec3 orbitDir(const float yaw, const float pitch) {
  const float y = glm::radians(yaw), p = glm::radians(pitch);
  return {std::cos(p) * std::sin(y), std::sin(p), std::cos(p) * std::cos(y)};
}

void orbit(vk::Engine3D &engine, const glm::vec3 &target, const float yaw, const float pitch, const float distance) {
  const glm::vec3 eye = target + orbitDir(yaw, pitch) * distance;
  const glm::vec3 d = glm::normalize(target - eye);
  // eng::Camera: dir = (cos y cos p, sin p, -sin y cos p).
  engine.camera()->setPose(eye, glm::degrees(std::atan2(-d.z, d.x)), glm::degrees(std::asin(d.y)));
  engine.requestRedraw();
}

bool attach(QQuickItem *root, const char *objectName, vk::Engine3D *engine) {
  auto *view = root ? root->findChild<vk::Viewport3D *>(QString::fromLatin1(objectName)) : nullptr;
  if (view)
    view->setEngine(engine);
  return view != nullptr;
}

eng::ConvexBrush fittedBox(const glm::dvec3 &mn, const glm::dvec3 &mx, const eng::MaterialPtr &material,
                           const int repeat) {
  eng::ConvexBrush b = eng::ConvexBrush::box(mn, mx, material, 0.1);
  b.build();
  for (size_t i = 0; i < b.faces.size() && i < b.polygons.size(); ++i) {
    std::vector<glm::vec3> poly;
    for (const auto &p: b.polygons[i])
      poly.emplace_back(p);
    eng::fitBasisToFace(b.faces[i].basis, poly, glm::vec3(b.faces[i].normal), repeat, repeat);
  }
  return b;
}

eng::ConvexBrush sphere(const double radius, const int segments, const int rings, const eng::MaterialPtr &material,
                        const double repeatU, const double repeatV) {
  const double pi = glm::pi<double>();
  std::vector<glm::dvec3> pts;
  pts.emplace_back(0.0, radius, 0.0);
  pts.emplace_back(0.0, -radius, 0.0);
  for (int r = 1; r < rings; ++r) {
    const double th = pi * r / rings;
    for (int s = 0; s < segments; ++s) {
      const double ph = 2.0 * pi * s / segments;
      pts.emplace_back(radius * std::sin(th) * std::sin(ph), radius * std::cos(th), radius * std::sin(th) * std::cos(ph));
    }
  }
  std::optional<eng::ConvexBrush> b = eng::ConvexBrush::fromPoints(pts, material, 0.1);
  if (!b)
    return fittedBox(glm::dvec3(-radius), glm::dvec3(radius), material);

  // Spherical UVs at the corners; a face across the seam is unwrapped, a pole
  // corner takes the face's mean u.
  for (size_t i = 0; i < b->faces.size() && i < b->polygons.size(); ++i) {
    const auto &poly = b->polygons[i];
    std::vector<glm::dvec2> uv;
    std::vector<bool> pole;
    for (const auto &p: poly) {
      const glm::dvec3 n = glm::normalize(p);
      const bool atPole = std::abs(n.y) > 1.0 - 1e-6;
      pole.push_back(atPole);
      const double u = atPole ? 0.0 : (std::atan2(n.x, n.z) / (2.0 * pi) + 0.5);
      uv.emplace_back(u, std::acos(std::clamp(n.y, -1.0, 1.0)) / pi);
    }
    double lo = 1.0, hi = 0.0;
    for (size_t k = 0; k < uv.size(); ++k)
      if (!pole[k]) {
        lo = std::min(lo, uv[k].x);
        hi = std::max(hi, uv[k].x);
      }
    double sum = 0.0;
    int count = 0;
    for (size_t k = 0; k < uv.size(); ++k)
      if (!pole[k]) {
        if (hi - lo > 0.5 && uv[k].x < 0.5)
          uv[k].x += 1.0;
        sum += uv[k].x;
        ++count;
      }
    for (size_t k = 0; k < uv.size(); ++k) {
      if (pole[k])
        uv[k].x = count ? sum / count : 0.0;
      uv[k] *= glm::dvec2(repeatU, repeatV);
    }
    eng::deform::solveBasisFromCorners(b->faces[i].basis, poly, uv, b->faces[i].normal);
  }
  return *b;
}

} // namespace ui::preview
