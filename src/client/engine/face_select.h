#pragma once

#include <QString>

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace eng {

class FaceGraph;

namespace sel {

// Each intent decides which faces qualify and how a contiguous selection spreads from the seeds.
enum class Intent { Wall = 0, Floor, Ceiling, Facade, Group };
enum class Exposure { Any = 0, Indoor, Outdoor };
// Facade faces, measured against the main wall plane of their side.
enum class FacadeType { Wall = 0, Recessed, Reveal, Trim, Ledge, Soffit, Count };

struct Query {
  Intent intent = Intent::Wall;
  Exposure exposure = Exposure::Indoor;
  bool allSides = false;  // Facade: walk around the building's outer corners
  quint32 groupId = 0;    // Group
  bool contiguous = true;
  bool sameMaterial = false; // only faces with a material of the seed(s)
  std::vector<int> seeds;    // graph node indices
  bool hasRegion = false; // only faces with their centre in the box
  glm::dvec3 regionMin {0.0}, regionMax {0.0};
};

struct Result {
  std::vector<int> faces;
  std::vector<FacadeType> types; // parallel to faces, Facade only
  glm::dvec3 mn {0.0}, mx {0.0};
  QString note;
  [[nodiscard]] bool empty() const { return faces.empty(); }
};

// Per-face indoor / outdoor answers, computed on demand, kept with their graph.
struct ExposureCache {
  std::vector<int8_t> v; // -1 unknown, 0 indoor, 1 outdoor
};

[[nodiscard]] bool isOutdoor(const FaceGraph &g, int node, ExposureCache &cache);
Result select(const FaceGraph &g, const Query &q, ExposureCache &cache);
[[nodiscard]] QString facadeTypeName(FacadeType t);

} // namespace sel
} // namespace eng
