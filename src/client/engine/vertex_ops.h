#pragma once

#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "brush.h"

namespace eng {

// Edit-mode vertex tool. A brush is rebuilt as the hull of its moved vertices; new faces take material and basis
// from the old face sharing the most vertices, else the same plane, else the most parallel. Texture lock re-solves
// the basis from three reference vertices per face (unmoved first).
struct VertexEditResult {
  std::optional<ConvexBrush> brush;
  // Moved / added points the hull swallowed (the move would make the brush concave).
  int absorbed = 0;
  // Faces were merged or re-split (a face went non-planar).
  bool retopologized = false;
};

// `newPositions` is parallel to orig.vertices(); `extra` are added points (edge / face splits).
VertexEditResult moveBrushVertices(const ConvexBrush &orig, const std::vector<glm::dvec3> &newPositions,
                                   const std::vector<glm::dvec3> &extra, bool textureLock);

// Re-expresses the basis so p keeps its UV after moving to A * p + t.
void lockBasisAffine(FaceTexBasis &basis, const glm::dmat3 &A, const glm::dvec3 &t);

std::vector<std::pair<glm::dvec3, glm::dvec3>> brushEdges(const ConvexBrush &brush);

} // namespace eng
