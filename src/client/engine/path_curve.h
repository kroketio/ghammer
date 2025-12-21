#pragma once

#include <memory>
#include <vector>

#include <QJsonObject>
#include <QString>
#include <glm/glm.hpp>

namespace eng::path {

// Chains of cubic bezier segments, like Blender's bezier curves. World space (Y up), world units.
// Handle types (Blender's): Auto follows the neighbours (recomputed on every edit); Aligned keeps both handles on
// one line, each its own length; Free = independent; Vector points a third of the way to the neighbour.
enum class HandleType : uint8_t { Auto = 0, Aligned, Free, Vector };

struct Point {
  glm::dvec3 pos {0.0};
  glm::dvec3 in {0.0};  // handle offsets from pos (in: towards the previous point)
  glm::dvec3 out {0.0}; //                       (out: towards the next point)
  HandleType type = HandleType::Auto;
};

// A Connect path between two oriented anchor entities (eng::path::buildConnect makes the points).
struct ConnectParams {
  quint32 anchorA = 0, anchorB = 0; // entity ids
  double sharpness = 0.25;          // 0 gradual (all the room) .. 1 sharp (tightest radius)
  double position = 0.0;            // -1 turn near A .. 1 near B
  double straightEnds = 0.0;        // straight run at each anchor (world units); < 0 = auto, 0 = none
  double tensionA = 1.0, tensionB = 1.0;
  int heightMode = 1;               // 0 ease over the turn, 1 ease over the whole (default), 2 linear
  double maxSlopeDeg = 35.0;
  double minRadius = -1.0;          // world units; < 0 = auto (from the objects that follow it)
};

struct Path {
  enum class Kind : uint8_t { Drawn = 0, Connect };
  quint32 id = 0;
  QString name;
  Kind kind = Kind::Drawn;
  bool closed = false;
  bool hidden = false;
  bool locked = false;
  std::vector<Point> points;
  ConnectParams connect;
};

// Recomputes Auto and Vector handles; call after every edit, so stored points always carry concrete handles.
void resolveHandles(std::vector<Point> &points, bool closed);
// After dragging a handle: Auto becomes Aligned, Vector becomes Free (Blender); Aligned keeps the other handle
// opposite at its own length.
void dragHandle(std::vector<Point> &points, bool closed, int i, bool outHandle, const glm::dvec3 &offset,
                bool forceFree);
// Keeps the curve's shape (de Casteljau split); the new point is Aligned.
void insertPoint(std::vector<Point> &points, bool closed, int segment, double t);

QJsonObject toJson(const Path &p);
Path fromJson(const QJsonObject &o);

// Arc-length parameterised, immutable, shared by every modifier that follows it. `s` = distance from the first
// point; outside [0, length] an open path continues straight along its end tangents, a closed one wraps.
class Curve {
public:
  static std::shared_ptr<const Curve> build(const std::vector<Point> &points, bool closed);

  [[nodiscard]] bool valid() const { return m_length > 1e-6; }
  [[nodiscard]] double length() const { return m_length; }

  [[nodiscard]] glm::dvec3 position(double s) const;
  [[nodiscard]] glm::dvec3 tangent(double s) const; // unit
  // |dT/ds| (1 / radius); 0 beyond the ends of an open path.
  [[nodiscard]] double curvature(double s) const;
  // Signed turning of the horizontal direction per unit length (+ = left seen from above).
  [[nodiscard]] double horizontalCurvature(double s) const;

  // (segment, t) <-> s
  [[nodiscard]] double sAt(int segment, double t) const;
  void segmentAt(double s, int &segment, double &t) const;

  // Polyline for drawing and picking, with each vertex's s.
  [[nodiscard]] const std::vector<glm::dvec3> &polyline() const { return m_poly; }
  [[nodiscard]] const std::vector<double> &polylineS() const { return m_polyS; }
  // Nearest point seen in a 2D view (right / up axes): returns s and the 2D distance.
  double nearest2D(const glm::dvec2 &p, const glm::dvec3 &right, const glm::dvec3 &up, double &distance) const;

  [[nodiscard]] double minRadius(double &atS) const;

private:
  struct Segment {
    glm::dvec3 p0, p1, p2, p3;
    double s0 = 0.0, len = 0.0;
    std::vector<double> cum; // cumulative length at t = i / kSub
  };
  static constexpr int kSub = 32;
  [[nodiscard]] static glm::dvec3 bez(const Segment &g, double t);
  [[nodiscard]] static glm::dvec3 bezD(const Segment &g, double t);
  [[nodiscard]] static glm::dvec3 bezDD(const Segment &g, double t);
  [[nodiscard]] static double lengthBetween(const Segment &g, double t0, double t1);
  [[nodiscard]] double tAt(const Segment &g, double local) const;
  [[nodiscard]] const Segment &segmentFor(double s, double &local) const;
  [[nodiscard]] double wrap(double s) const;

  std::vector<Segment> m_segments;
  std::vector<glm::dvec3> m_poly;
  std::vector<double> m_polyS;
  double m_length = 0.0;
  bool m_closed = false;
};

// Connect paths. Both directions point into the gap (dirB is the anchor's facing: the curve arrives against
// it). Builds a corner, S or U with straight leads and a smooth height change, or reports why not.
struct ConnectEnd {
  glm::dvec3 pos {0.0};
  glm::dvec3 facing {0.0, 0.0, 1.0}; // horizontal
};
struct ConnectResult {
  bool ok = false;
  QString error;    // why not (ok == false)
  QString warning;  // built, but worth a look
  std::vector<Point> points;
  double radius = 0.0; // 0 = straight
};
// `autoStraight` / `autoRadius` stand in for the < 0 (auto) parameters.
ConnectResult buildConnect(const ConnectEnd &a, const ConnectEnd &b, const ConnectParams &p, double autoStraight,
                           double autoRadius);

} // namespace eng::path
