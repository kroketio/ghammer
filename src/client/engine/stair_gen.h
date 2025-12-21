#pragma once

#include <QJsonObject>

#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "brush.h"

namespace eng::stairs {

// StepSize: count = box height / step height, tread depth stretches to fill; Count: given directly.
enum class Sizing { StepSize = 0, Count };
// Solid down to the floor, step-high planks (zigzag), or a sloped soffit.
enum class Underside { Solid = 0, Planks, Slope };

// Lengths in Source units, the box in world units.
struct Params {
  glm::dvec3 mn {0.0}, mx {0.0};
  int direction = 2; // climbs toward 0 = +X, 1 = -X, 2 = +Z, 3 = -Z
  Sizing sizing = Sizing::StepSize;
  double stepHeight = 8.0;
  double stepDepth = 8.0; // used by "fit box depth"
  int steps = 8;
  Underside underside = Underside::Solid;
  double thickness = 16.0; // slope: vertical depth of the soffit below the step line
  // Planks only: real planks of plankThickness instead of step-high blocks.
  bool thinPlanks = false;
  double plankThickness = 4.0;
  // Nosing: an extra brush on each step's front top edge, nosingDepth out, nosingHeight tall.
  bool nosing = false;
  double nosingDepth = 6.0;
  double nosingHeight = 2.0;
  // A walkable ramp brush over the steps (func_clip in the editor).
  bool collision = true;
  // Stringers along both ends inside the box (steps narrow by `sideWidth` each side); top `sideHeight` above the
  // nosing line, bottom follows the underside.
  bool sides = false;
  double sideWidth = 4.0;
  double sideHeight = 8.0;
  // Placement for turns the direction cannot express (any angle, tilts), applied after building in the box.
  glm::dmat4 xform {1.0};
};

[[nodiscard]] bool isIdentity(const glm::dmat4 &m);
// Folds `m` (applied after the current placement) into the parameters: quarter turns about Y and mirrors become a
// new box and direction, anything else goes into xform.
void applyTransform(Params &p, const glm::dmat4 &m);

[[nodiscard]] int stepCount(const Params &p);
// Box extent along the climb, across it, and up (world units).
[[nodiscard]] double climbLength(const Params &p);
[[nodiscard]] int climbAxis(int direction);

// One convex brush per step plus side brushes; face groups: tops / bottoms Flat, fronts / backs Wall, step ends
// Side, slanted undersides Under, side brushes Stringer.
std::vector<ConvexBrush> build(const Params &p, const MaterialPtr &material, double tilesPerUnit);

// Collision ramp: solid stairs fill under the nosing line (top corner cut by the box); planks / slope get a slab
// parallel to it just thick enough to hold the steps. Runs down to the floor past the box. Empty when off.
std::optional<ConvexBrush> buildCollision(const Params &p, const MaterialPtr &material, double tilesPerUnit);

QJsonObject toJson(const Params &p);
Params fromJson(const QJsonObject &o);

// Step texturing: one mapping per stair part, each face in its own frame with the texture centred on the face
// centre, so every step shows the same part of the texture; scaling grows from the centre.

enum class Part { Flat = 0, Wall, Stringer, Count };
[[nodiscard]] const char *partKey(Part part);  // "flat", "wall", "stringer"
// Count when the group is none of them.
[[nodiscard]] Part partOfGroup(int groupId);
[[nodiscard]] int groupOfPart(Part part);

struct StepTex {
  bool on = false;
  int rotation = 0;      // quarter turns, 0..3
  double offsetU = 0.0;  // texels of the texture on the face centre, from the texture centre
  double offsetV = 0.0;
  double tile = 64.0;    // Source units one texture width covers (V follows the aspect)
  QString material;      // ref; empty keeps each face's material

  bool operator==(const StepTex &) const = default;
};

[[nodiscard]] QJsonObject stepTexToJson(const StepTex &t);
[[nodiscard]] StepTex stepTexFromJson(const QJsonObject &o);
// The "tex" object of the stairs params: {"flat": {...}, "wall": {...}, ...}.
[[nodiscard]] StepTex stepTexOf(const QJsonObject &procParams, Part part);
void setStepTex(QJsonObject &procParams, Part part, const StepTex &t);

// `normal` outward, `polygon` world corners. U right, V down seen from outside (cross(u, v) == -n); "up" is the
// climb direction on treads, world up elsewhere.
[[nodiscard]] FaceTexBasis stepTexBasis(const Params &p, const glm::dvec3 &normal,
                                        const std::vector<glm::dvec3> &polygon, const StepTex &t, int texW,
                                        int texH);

} // namespace eng::stairs
