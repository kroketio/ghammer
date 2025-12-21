#pragma once

#include <QString>
#include <QtGlobal>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "face_groups.h"

namespace eng {

class ConvexBrush;
class Scene;

// One solid's faces as world polygons.
struct SolidInput {
  quint32 objId = 0;
  bool occluder = true; // false for sky / trigger / clip style tool brushes
  struct Face {
    quint32 pickId = 0;
    int face = 0;
    std::vector<glm::dvec3> poly;
    FaceGroupSet groups;
    QString material;
  };
  std::vector<Face> faces;
};

enum class LinkKind : uint8_t { Flat, Convex, Concave };

struct FaceLink {
  int other = -1;
  LinkKind kind = LinkKind::Flat;
  glm::dvec3 a {0.0}, b {0.0}; // the shared stretch of edge
};

struct FaceNode {
  quint32 objId = 0;
  quint32 pickId = 0;
  int face = 0;
  int solid = -1;
  std::vector<glm::dvec3> poly;
  glm::dvec3 normal {0.0, 1.0, 0.0}; // outward
  double dist = 0.0;                  // normal . x on the plane
  double area = 0.0;
  glm::dvec3 centroid {0.0};
  glm::dvec3 mn {0.0}, mx {0.0};
  FaceGroupSet groups;
  QString material;
  bool hidden = false; // covered all over by touching / overlapping solids (FaceGraph::faceCovered)
  bool selectable = true;
};

// Every brush face with adjacency: faces sharing a stretch of edge are linked (within and across brushes), each
// link convex, concave or flat. Also ray casts against the solids for indoor / outdoor tests. World units.
class FaceGraph {
public:
  // withLinks = false leaves `links` empty (most of the cost on a large map); hidden flags and solid queries remain.
  static FaceGraph build(const std::vector<SolidInput> &solids, bool withLinks = true);
  static FaceGraph fromScene(const Scene &scene);
  static std::vector<SolidInput> solidsFromBrushes(const std::vector<std::pair<quint32, ConvexBrush>> &brushes);

  std::vector<FaceNode> nodes;
  std::vector<std::vector<FaceLink>> links;
  glm::dvec3 mn {0.0}, mx {0.0};

  [[nodiscard]] int indexOfPick(quint32 pickId) const;
  // Nearest occluding hit within maxT; false when the ray leaves the map.
  bool raycast(const glm::dvec3 &origin, const glm::dvec3 &dir, double maxT, double *t = nullptr) const;
  [[nodiscard]] bool insideOtherSolid(const glm::dvec3 &p, int exceptSolid) const { return solidContaining(p, exceptSolid) >= 0; }
  // The occluding solid (other than exceptSolid) p is inside, or -1.
  [[nodiscard]] int solidContaining(const glm::dvec3 &p, int exceptSolid) const;
  // Depth of a solid along a direction (max - min of its vertices).
  [[nodiscard]] double solidDepth(int solid, const glm::dvec3 &dir) const;

  // One solid per brush.
  [[nodiscard]] int solidCount() const { return int(m_solids.size()); }
  [[nodiscard]] quint32 solidObjId(int s) const { return m_solids[size_t(s)].objId; }
  [[nodiscard]] bool solidOccluder(int s) const { return m_solids[size_t(s)].occluder; }
  [[nodiscard]] glm::dvec3 solidMin(int s) const { return m_solids[size_t(s)].mn; }
  [[nodiscard]] glm::dvec3 solidMax(int s) const { return m_solids[size_t(s)].mx; }
  // No face plane of either keeps the other more than kEps away: exact for axis-aligned brushes; solids meeting
  // edge to edge at an angle may count as touching.
  [[nodiscard]] bool solidsTouch(int a, int b) const;
  [[nodiscard]] std::vector<int> touchingSolids(int s) const;

  static constexpr double kEps = 0.01; // 0.1 Source units

private:
  struct Solid {
    quint32 objId = 0;
    bool occluder = true;
    std::vector<glm::dvec3> normals;
    std::vector<double> dists;
    std::vector<glm::dvec3> verts;
    glm::dvec3 mn {0.0}, mx {0.0};
  };
  std::vector<Solid> m_solids;
  std::unordered_map<quint32, int> m_byPick;
  double m_cell = 16.0;
  glm::ivec3 m_dims {1, 1, 1};
  std::unordered_map<quint64, std::vector<int>> m_solidGrid;
  mutable std::vector<quint32> m_rayStamp;
  mutable quint32 m_rayEpoch = 0;
  bool hitSolid(const Solid &s, const glm::dvec3 &origin, const glm::dvec3 &dir, double &best) const;
  [[nodiscard]] quint64 cellKey(int x, int y, int z) const;
  void computeHidden();
  // Each other solid's part of the face is cut away exactly; true when nothing is left.
  [[nodiscard]] bool faceCovered(const FaceNode &n) const;
  void computeLinks();
};

} // namespace eng
