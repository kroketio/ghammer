#pragma once

#include <algorithm>
#include <array>
#include <vector>

#include <QString>
#include <glm/glm.hpp>

namespace eng {

enum class LayerKind {
  Wall,
  Space,
};

class Scene;

// One face in world space with its wall frame: vertical faces (normal along X or Z) get u = the horizontal axis
// across the run and v = world up, so storey tests are rectangle arithmetic.
struct FaceInfo {
  quint32 pickId = 0;
  quint32 objId = 0;
  int face = 0;

  std::array<glm::vec3, 4> quad {};
  glm::vec3 normal {0.0f, 0.0f, 0.0f};
  glm::vec3 center {0.0f, 0.0f, 0.0f};
  double area = 0.0;

  int axis = 2;
  double plane = 0.0;

  glm::vec3 uDir {1.0f, 0.0f, 0.0f};
  glm::vec3 vDir {0.0f, 0.0f, 1.0f};
  double u0 = 0.0;
  double u1 = 0.0;
  double v0 = 0.0;
  double v1 = 0.0;

  bool vertical = false;

  [[nodiscard]] double spanU() const { return u1 - u0; }
  [[nodiscard]] double spanV() const { return v1 - v0; }
  // Footprint on the ground plane, for corner and floor tests.
  [[nodiscard]] glm::dvec3 xzMin() const;
  [[nodiscard]] glm::dvec3 xzMax() const;
};

struct FillParams {
  // Facade adds storeys and the outside-only cull; wall mode is the plain coplanar run.
  bool facade = true;
  bool wrap = true;

  // A face smaller than either is a trim, frame or vent.
  double minArea = 6.0;
  double minSpanV = 2.0;

  double planeTol = 0.05;
  // Max distance between two faces of one continuous wall.
  double gap = 0.30;

  // A floor must be big enough and close behind the wall, and the face must see open space for at least
  // this far (facade vs room partition).
  double floorMinArea = 60.0;
  double floorProbe = 64.0;
  double outsideClear = 16.0;

  // A real floor extends into the building; this keeps a wall brush's top face from inventing storeys.
  double floorMinDepth = 8.0;

  // Off: layers come from the wall's own horizontal seams (buildings with no interior).
  bool detectFloors = true;

  // A wall face must span this share of its storey; shorter ones are windows or trim bands. 0 = off.
  double storeySpanFrac = 0.7;
};

// An opening: part of the run's rectangle no wall face covers, sealed by wall on all sides (windows, doorways,
// not a gap at the wall's end).
struct GapRect {
  int axis = 0;
  double plane = 0.0;
  double u0 = 0.0, u1 = 0.0;
  double v0 = 0.0, v1 = 0.0;
};

// One wall of the building: a plane, the extent the run covers on it and its faces. Not a separate solve:
// the layer stack stays with the run. Wrap off = one facade, wrapping adds one per plane turned onto.
// Member faces keep their own rect so a band is only as wide as the wall at that height (a plinth sticking out
// widens the facade's u range).
struct FacadeFace {
  quint32 pickId = 0;
  double u0 = 0.0;
  double u1 = 0.0;
  double v0 = 0.0;
  double v1 = 0.0;
};

struct Facade {
  int axis = 2;
  double plane = 0.0;
  glm::vec3 normal {0.0f, 0.0f, 1.0f};
  glm::vec3 uDir {1.0f, 0.0f, 0.0f};
  double u0 = 0.0;
  double u1 = 0.0;
  double v0 = 0.0;
  double v1 = 0.0;
  std::vector<FacadeFace> faces;
  std::vector<quint32> objects;
  bool seed = false;

  [[nodiscard]] double spanU() const { return u1 - u0; }
  [[nodiscard]] double spanV() const { return v1 - v0; }

  // Horizontal extent at the given height band; false when nothing reaches that high.
  [[nodiscard]] bool uRangeAt(const double y0, const double y1, double &lo, double &hi) const {
    bool any = false;
    for (const auto &f: faces) {
      if (f.v1 <= y0 + 1e-3 || f.v0 >= y1 - 1e-3)
        continue;
      lo = any ? std::min(lo, f.u0) : f.u0;
      hi = any ? std::max(hi, f.u1) : f.u1;
      any = true;
    }
    return any;
  }
};

struct FillResult {
  bool ok = false;
  std::vector<quint32> faces;
  // The run split by plane, seed facade first.
  std::vector<Facade> facades;
  // Ascending floor planes found behind the run, world units.
  std::vector<double> levelYs;
  // Ascending layer bounds: size == layers + 1, bounds[0] is the bottom.
  std::vector<double> bounds;
  // size == bounds.size() - 1.
  std::vector<LayerKind> layerKinds;
  // One per bound: set where a face spans across the seam (cut not made yet; drawn dotted).
  std::vector<char> boundCutsMesh;
  // Bands came from real floor slabs, not the wall's geometry.
  bool floorsFromDetection = false;
  // Top of the lowest slab at or below the seed; its side faces are floor, never facade faces.
  double groundY = 0.0;
  quint32 groundObj = 0;
  bool groundFromSeed = false;
  // Openings found in the run, for the readout.
  std::vector<GapRect> gaps;
  QString note;
};

// Every axis-aligned face of the scene, resolved to world space.
std::vector<FaceInfo> collectFaces(const Scene &scene);

// Below the size thresholds: a trim, frame or vent.
[[nodiscard]] bool isTrimFace(const FaceInfo &f, const FillParams &p);

// A wall face must span `frac` of its storey; shorter ones are windows or trim. 0 = off.
[[nodiscard]] bool spansStorey(const FaceInfo &f, const std::vector<double> &bounds, double frac);

// The coplanar, edge-connected run the seed face belongs to.
FillResult solveWallFill(const std::vector<FaceInfo> &faces, quint32 seed, const FillParams &p);

// The run plus its storeys, wrapped around corners and limited to the outside.
FillResult solveFacadeFill(const std::vector<FaceInfo> &faces, quint32 seed, const FillParams &p);

} // namespace eng
