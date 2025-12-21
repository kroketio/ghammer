#pragma once

// Object-mode auto-select on the FaceGraph. Wall / Floor / Ceiling / Facade run the face_select.h rules from
// the picked brushes' faces and lift the result to their brushes; Same size / material / Touching are brush-only.

#include <QString>

#include <array>
#include <vector>

#include <glm/glm.hpp>

#include "face_select.h"

namespace eng {

class FaceGraph;

namespace sel {

enum class BrushIntent { Wall = 0, Floor, Ceiling, Facade, SameSize, SameMaterial, Touching, Count };

struct BrushQuery {
  BrushIntent intent = BrushIntent::Wall;
  Exposure exposure = Exposure::Indoor; // Wall / Floor
  bool allSides = false;                // Facade
  // Grow over touching brushes; off = every match (inside the region, if any).
  bool contiguous = true;
  bool sameMaterial = false;  // only brushes with a material of the picked one(s)
  std::vector<quint32> seeds; // object ids
  bool hasRegion = false;     // only brushes with their centre in the box
  glm::dvec3 regionMin {0.0}, regionMax {0.0};
  // Facade: kinds to keep (indexed by FacadeType).
  std::array<bool, 6> facadeTypes {true, true, true, true, true, true};
};

struct BrushResult {
  std::vector<quint32> objects; // sorted
  glm::dvec3 mn {0.0}, mx {0.0};
  // Facade: brushes per kind before the facadeTypes filter, each under the kind of most of its facade area.
  std::array<int, 6> typeCounts {};
  QString note;
  [[nodiscard]] bool empty() const { return objects.empty(); }
};

BrushResult selectBrushes(const FaceGraph &g, const BrushQuery &q, ExposureCache &cache);

} // namespace sel
} // namespace eng
