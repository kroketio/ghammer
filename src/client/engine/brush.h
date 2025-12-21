#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <QSharedPointer>
#include <QString>
#include <glm/glm.hpp>

#include "material.h"
#include "mesh.h"

namespace eng {

class GameObject;

// One bounding plane n·x = d (n out of the solid) and its paint. The texture basis is world-projected, so it
// survives edits that keep the plane and transforms cleanly under texture lock.
struct BrushFace {
  glm::dvec3 normal {0.0, 1.0, 0.0};
  double distance = 0.0;
  MaterialPtr material;
  FaceTexBasis basis;
  FaceGroupSet groups;
  // Made by a cut inside the Curve modifier: inside the solid, never split again for its texture.
  bool cut = false;

  [[nodiscard]] double signedDistance(const glm::dvec3 &p) const { return glm::dot(normal, p) - distance; }
};

// A convex polyhedron: planes plus each plane's polygon (from build()). Mesh polygons are the stored form;
// fromObject / toMesh convert. World space, double precision.
class ConvexBrush {
public:
  std::vector<BrushFace> faces;
  std::vector<std::vector<glm::dvec3>> polygons; // parallel to faces after build()

  static constexpr double kEpsilon = 1e-4;        // world units (0.001 Source)

  // Clips a huge quad per plane against every other plane; empty faces are removed. False unless a closed solid
  // (≥ 4 faces, volume).
  bool build();

  [[nodiscard]] bool valid() const { return polygons.size() >= 4 && volume() > 1e-6; }
  [[nodiscard]] double volume() const;
  [[nodiscard]] glm::dvec3 centroid() const;
  void bounds(glm::dvec3 &mn, glm::dvec3 &mx) const;
  [[nodiscard]] std::vector<glm::dvec3> vertices() const;

  static std::optional<ConvexBrush> fromObject(const GameObject &object);
  // From planes only (VMF sides); the caller fills attributes.
  static std::optional<ConvexBrush> fromPlanes(std::vector<BrushFace> planes);
  // Convex hull; faces get `material` and a world-aligned basis.
  static std::optional<ConvexBrush> fromPoints(const std::vector<glm::dvec3> &points, const MaterialPtr &material,
                                               double tilesPerUnit);
  static ConvexBrush box(const glm::dvec3 &mn, const glm::dvec3 &mx, const MaterialPtr &material, double tilesPerUnit);

  // World-space mesh; picking ids are minted for `stableId` when non-zero.
  [[nodiscard]] std::shared_ptr<Mesh> toMesh() const;

  // Keeps the part behind the plane (n·x <= d); false when nothing is left. The cap copies `plane`'s attributes.
  bool clip(const BrushFace &plane);
  // Either piece may be empty; caps copy the most parallel face.
  void split(const glm::dvec3 &normal, double distance, std::optional<ConvexBrush> &front,
             std::optional<ConvexBrush> &back) const;
  // this − cutter as convex fragments: {*this} when apart, {} when swallowed.
  [[nodiscard]] std::vector<ConvexBrush> subtract(const ConvexBrush &cutter) const;
  [[nodiscard]] std::optional<ConvexBrush> intersect(const ConvexBrush &other) const;
  [[nodiscard]] bool overlaps(const ConvexBrush &other) const;
  // Walls of thickness `t` around the interior.
  [[nodiscard]] std::vector<ConvexBrush> hollow(double thickness) const;

  // With `textureLock` the bases are re-expressed so every surface point keeps its texture coordinate.
  void transform(const glm::dmat4 &m, bool textureLock);

  // For inheriting attributes.
  [[nodiscard]] const BrushFace *mostParallelFace(const glm::dvec3 &n) const;
};

// Default texture axes for outward normal `n`: the one rule for new brushes and Align to World.
// Paraxial (the normal's dominant axis picks the projection plane), oriented so seen from outside U runs right and
// V down (v = 0 is the image's top in QRhi, glTF and Godot), +Y up for walls, -Z for floors / ceilings:
// cross(u, v) == -n, nothing mirrored. +X (-Z, -Y), -X (+Z, -Y), +Z (+X, -Y), -Z (-X, -Y), +Y (+X, +Z), -Y (-X, +Z).
void defaultTexAxes(const glm::dvec3 &n, glm::vec3 &u, glm::vec3 &v);

// defaultTexAxes() with a repeat rate.
FaceTexBasis paraxialBasis(const glm::dvec3 &normal, double tilesPerUnit);

// Fit: the texture along the face's edges, stretched to `repeatU` x `repeatV` tiles (0 keeps that axis).
// U goes along the edge closest to the current U (tie: the longer edge), V at right angles on the same side
// (cross(u, v) keeps its sign against `n`), so a rotated brush fits like an unrotated one. `poly` is world space.
void fitBasisToFace(FaceTexBasis &b, const std::vector<glm::vec3> &poly, const glm::vec3 &n, int repeatU,
                    int repeatV);

// Carries a face basis through `m`, which took its normal from `oldNormal` to `newNormal`:
// - `textureLock`: every surface point keeps its texture coordinate (scale into repeat, translation into offset);
// - otherwise it stays on the world grid: axes turn from defaultTexAxes(oldNormal) to defaultTexAxes(newNormal),
//   repeat and offset kept, so a default basis stays default and a hand-rotated one keeps its rotation.
void transformFaceBasis(FaceTexBasis &b, const glm::dmat4 &m, const glm::dvec3 &oldNormal,
                        const glm::dvec3 &newNormal, bool textureLock);

// A texture-locked reflection reads mirror-imaged: flips the texture axis closest to the reflected one back
// (U on a tie). `axisMask`: reflected axes (bit i = axis i); an even count is a rotation and is left alone.
void unmirrorBasis(FaceTexBasis &b, int axisMask);

// Primitive shapes fitted to an axis-aligned box. `up`: 0 = X, 1 = Y, 2 = Z. Stairs / arch give several brushes.
struct PrimitiveParams {
  int sides = 16;        // cylinder / cone / sphere segments
  int steps = 8;         // stairs
  // Climb direction: 0 = +X, 1 = -X, 2 = +Z, 3 = -Z (world when up = Y, else the matching local axes).
  int stairDirection = 2;
  // 0 = solid blocks down to the floor, 1 = floating treads, 2 = treads on one sloped support (n + 1 brushes).
  int stairStyle = 0;
  int segments = 8;      // arch
  double archThickness = 0.25; // fraction of the box depth
  double archDegrees = 180.0;
  int up = 1;
};
enum class PrimitiveKind { Box, Wedge, Cylinder, Cone, Spike, Sphere, Stairs, Arch, Pyramid };
std::vector<ConvexBrush> buildPrimitive(PrimitiveKind kind, const glm::dvec3 &mn, const glm::dvec3 &mx,
                                        const PrimitiveParams &params, const MaterialPtr &material,
                                        double tilesPerUnit);

} // namespace eng
