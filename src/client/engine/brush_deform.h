#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "brush.h"

namespace eng::deform {

// Brush slicing and non-linear deformation (the Curve modifier's Bend).

// Cuts with the parallel planes n·x = d (any order); pieces from low d to high. Caps copy the most parallel face.
std::vector<ConvexBrush> slice(const ConvexBrush &brush, const glm::dvec3 &normal, std::vector<double> distances);

using Map = std::function<glm::dvec3(const glm::dvec3 &)>;

// Texture coordinate (tiles) for a corner, from its source face and its position before the deformation.
using TexTarget = std::function<glm::dvec2(const BrushFace &src, const glm::dvec3 &srcPos)>;

// Fits `b` so b.uvAt(pts[i]) ≈ uv[i] (least-squares affine in the plane of `n`, exact for three corners);
// offset wrapped into [0, 1). Returns the largest miss in world units, or -1 when degenerate (b untouched).
double solveBasisFromCorners(FaceTexBasis &b, const std::vector<glm::dvec3> &pts, const std::vector<glm::dvec2> &uv,
                             const glm::dvec3 &n);

struct Result {
  std::optional<ConvexBrush> brush;
  bool folded = false;    // the map turned the brush inside out / concave (no brush)
  bool triangulated = false; // a face went non-planar and was split into triangles
};

// Moves every vertex through `map` and rebuilds a convex brush. Bent faces become a triangle fan (the first that
// keeps it convex); new faces keep material and groups. With `target` each face's basis is solved from its corners,
// else the texture stays world-projected. `map` must preserve orientation (determinant > 0).
Result deformBrush(const ConvexBrush &brush, const Map &map, const TexTarget &target);

// Splits `brush` (pre-deformation) until every planar face fits `target` with one flat mapping within `tol`:
// such a face is cut along its shorter diagonal, so it and the opposite face become triangles. Cut faces are never split.
std::vector<ConvexBrush> splitForTexture(const ConvexBrush &brush, const Map &map, const TexTarget &target,
                                         double tol);

} // namespace eng::deform
