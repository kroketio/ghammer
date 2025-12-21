#pragma once

// Bevelling brush edges. Pure geometry.
// - Round (segments > 1) / chamfer (1): one cut plane per chord of an arc tangent to both faces; stays one brush.
// - Cove (inverse): the arc bulges in; the brush is split into convex pieces (merged back where possible).
// - Inner fill (inverse, edge face against another brush): new brushes fill the inner corner; brush unchanged.
// Kept faces keep their texture; the round's faces wrap the texture of one edge face (seam at the far tangent).

#include <optional>
#include <vector>

#include <QString>
#include <glm/glm.hpp>

#include "brush.h"

namespace eng::bevel {

constexpr int kMaxSegments = 32;

struct Edge {
  glm::dvec3 a {0.0}, b {0.0};
};

enum class Kind { Round, Cove, Fill };

enum class Issue { None, NoEdge, NotAnEdge, NoRadius, Flat, NoRoom, Failed };

struct Params {
  double radius = 16.0;
  int segments = 4;
  bool inverse = false;
  // Set: the round's faces get this material instead of the edge faces' own.
  MaterialPtr material;
  // One Source unit in the brush's units (tolerances and minimum sizes are Source sizes): 0.1 in the editor, 1 in tests.
  double sourceUnit = 1.0;
};

struct Result {
  Issue issue = Issue::None;
  // One brush for a round, the cove's pieces, or the brush unchanged for a fill.
  std::vector<ConvexBrush> pieces;
  std::vector<ConvexBrush> added;
  double radius = 0.0;  // used (clamped to the limit)
  int segments = 0;     // used (capped so no chord is shorter than half a unit)
  std::vector<Kind> kinds; // per edge
  [[nodiscard]] bool ok() const { return issue == Issue::None; }
};

QString issueText(Issue issue);
// "Round", "Chamfer" (a round of one segment), "Cove", "Inner fill".
QString kindText(Kind kind, int segments);

// The largest radius for every edge together (0 with an issue).
double limit(const ConvexBrush &brush, const std::vector<Edge> &edges, const std::vector<ConvexBrush> &neighbours,
             bool inverse, double sourceUnit = 1.0, Issue *issue = nullptr);

std::vector<Kind> kinds(const ConvexBrush &brush, const std::vector<Edge> &edges,
                        const std::vector<ConvexBrush> &neighbours, bool inverse, double sourceUnit = 1.0);

// Where one edge's bevel opens (out of the brush for a round / cove, into the corner for a fill); none if impossible.
std::optional<glm::dvec3> opening(const ConvexBrush &brush, const Edge &edge,
                                  const std::vector<ConvexBrush> &neighbours, bool inverse, double sourceUnit = 1.0);

// False when `edge` is not an edge of the brush.
bool edgeFaces(const ConvexBrush &brush, const Edge &edge, BrushFace &f0, BrushFace &f1, double sourceUnit = 1.0);

// `neighbours` are only read for inner fills.
Result bevel(const ConvexBrush &brush, const std::vector<Edge> &edges, const std::vector<ConvexBrush> &neighbours,
             const Params &params);

// `segments` capped so no chord of the round is shorter than half a Source unit. `cornerAngle` is between the
// two faces' in-face directions.
int cappedSegments(double radius, int segments, double cornerAngle, double sourceUnit = 1.0);

} // namespace eng::bevel
