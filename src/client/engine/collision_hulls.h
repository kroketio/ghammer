#pragma once

#include <vector>

#include <glm/glm.hpp>

// Collision hulls for a group of brushes (func_rigidbody), shared by the editor's Box3D bodies and the Godot export.
// Greedy merging: the pair whose merged hull wastes the least volume, vol(hull(A ∪ B)) − (vol(A) + vol(B)),
// merges while the waste stays under `tolerance` of the total, and always while above `maxHulls`.
// Final hulls are cut to `maxVertices`. Uses Box3D's quickhull; units in = units out.
namespace eng::hulls {

struct Options {
  int maxHulls = 4;
  double tolerance = 0.08;  // wasted volume, as a share of the total
  int maxVertices = 32;
};

// `brushes`: each brush's points (any order, duplicates fine). Flat / degenerate ones are counted in `skipped`.
std::vector<std::vector<glm::vec3>> collisionHulls(const std::vector<std::vector<glm::vec3>> &brushes,
                                                   const Options &opt = {}, int *skipped = nullptr);

} // namespace eng::hulls
