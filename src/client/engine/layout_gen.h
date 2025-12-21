#pragma once

#include <array>
#include <vector>

#include <QHash>
#include <QJsonObject>
#include <QPoint>
#include <QRect>
#include <QRectF>
#include <QString>

#include <glm/glm.hpp>

namespace eng::layout {

// The Layout Designer's document: corridors and vents as centre-line polylines (per-segment width / height /
// floor elevation), rooms as unions of rectangles, sealed squares. Source units on the plan (x right, y down =
// world +Z). generate() is exact: every shape edge and its wall-thickness offsets become lines of a non-uniform
// grid. Joined corridors / rooms open by the shared cross-section; a vent opens a hole of its own size.
enum Kind { Corridor = 0, Room = 1, Vent = 2 };

// Open cuts the room into the corridor, Doorway leaves a wall with a door hole, SharedWall keeps the
// corridor's wall and shrinks the room against it.
enum Contact { ContactOpen = 0, ContactDoorway = 1, ContactSharedWall = 2 };

struct Segment {
  int width = 128;
  int height = 128;
  int elevation = 0; // floor height
};

struct Path {
  int id = 0;
  Kind kind = Corridor;
  std::vector<QPoint> nodes; // Source units
  std::vector<Segment> segs; // nodes.size() - 1
};

struct RoomShape {
  int id = 0;
  std::vector<QRect> rects; // Source units; x()/y() is the corner, width()/height() the size
  int height = 128;
  int elevation = 0;
  int contact = ContactOpen;
};

struct Config {
  int wall = 10;
  int floor = 16;
  int ceiling = 16;
  int ventWall = 8;
  int doorWidth = 64;
  int doorHeight = 112;
  int corridorWidth = 128;
  int corridorHeight = 128;
  int roomHeight = 128;
  int roomContact = ContactOpen;
  int ventWidth = 48;
  int ventHeight = 48;
  int ventElevation = 0;
  QString matFloor, matWall, matCeiling, matVent;
  // Output: 0 = worldspawn, 1 = new mesh group (groupName), 2 = existing group (groupId).
  int target = 0;
  QString groupName = QStringLiteral("layout");
  int groupId = 0;
};

struct Doc {
  int id = 0;
  QString name = QStringLiteral("Layout");
  std::vector<Path> paths;
  std::vector<RoomShape> rooms;
  std::vector<QRect> sealed; // Source units
  Config config;
  int nextShapeId = 1;
  // World position of plan (0, 0), world units.
  glm::vec3 origin {0.0f};
};

// A shared-wall room side this close to a corridor wall is extended to meet it.
constexpr int kAttachDistance = 64;

[[nodiscard]] inline quint64 cellKey(int x, int y) {
  return (quint64(quint32(x)) << 32) | quint64(quint32(y));
}
[[nodiscard]] inline QPoint keyCell(quint64 k) { return {int(qint32(k >> 32)), int(qint32(k & 0xffffffffu))}; }

QJsonObject toJson(const Doc &doc);
Doc fromJson(const QJsonObject &o);

// Floor extent of a segment along its axis: node distance plus half the width at each end.
[[nodiscard]] int segmentLength(const Path &path, int seg);

// What a generated face looks onto: nothing (outside or buried) or the space it bounds.
enum FaceCategory { FaceNone = 0, FaceFloor, FaceWall, FaceCeiling, FaceVent };

// One output brush in Source units: x / y are the plan (y = world +Z), z is up.
// faces[] is indexed +x, -x, +y, -y, +z (top), -z (bottom).
struct Box {
  glm::ivec3 mn, mx;
  bool vent = false;
  std::array<FaceCategory, 6> faces {FaceNone, FaceNone, FaceNone, FaceNone, FaceNone, FaceNone};
};

// One grid cell, for the designer's drawing and tests.
struct CellInfo {
  int kind = -1; // -1 empty, Corridor, Room
  int shape = 0;
  int seg = -1;
  int elevation = 0;
  int height = 0;
  int lateral = 0; // distance from the corridor centre-line, Source units
  bool sealed = false;
  bool doorZone = false; // corridor cell within a wall of a doorway room
  bool vent = false;
  int ventElevation = 0, ventHeight = 0;
};

// Non-uniform grid: cell (i, j) spans xs[i]..xs[i+1] by ys[j]..ys[j+1]; only covered cells are stored.
struct Raster {
  std::vector<int> xs, ys;
  QHash<quint64, CellInfo> cells;
  [[nodiscard]] QRect cellRect(quint64 key) const;
};

Raster rasterize(const Doc &doc);
std::vector<Box> generate(const Doc &doc, int *columns = nullptr);

// Solid z-intervals at plan point (x, y) in Source units, for tests.
std::vector<std::pair<int, int>> solidColumn(const Doc &doc, int x, int y);

} // namespace eng::layout
