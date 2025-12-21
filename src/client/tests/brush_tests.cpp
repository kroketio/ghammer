// Geometry kernel tests. Run: cmake-build-debug/src/client/brush_tests
#include <cmath>
#include <set>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <glm/gtc/matrix_transform.hpp>

#include "client/engine/arch_gen.h"
#include "client/engine/brush.h"
#include "client/engine/face_graph.h"
#include "client/engine/brush_select.h"
#include "client/engine/face_select.h"
#include "client/engine/map_format.h"
#include "client/engine/color_lut.h"
#include "client/engine/layout_gen.h"
#include "client/engine/vertex_ops.h"
#include "client/engine/gameobject.h"
#include "client/engine/scene.h"
#include "client/engine/stair_gen.h"
#include "client/engine/entity.h"
#include "client/engine/godot_export.h"
#include "client/engine/godot_io_export.h"
#include "client/engine/godot_text.h"
#include "client/engine/gltf_writer.h"
#include "client/engine/io.h"
#include "client/engine/brush_bevel.h"
#include "client/engine/io_sim.h"
#include "client/engine/material_overrides.h"
#include "client/engine/orm_compose.h"
#include "client/engine/undo_stack.h"
#include "client/project/godot_project.h"
#include "client/project/project.h"
#include "shared/models/material_defaults.h"
#include "shared/models/material_props.h"
#include "shared/models/texture_entry.h"

using namespace eng;

class BrushTests : public QObject {
  Q_OBJECT

private:
  static ConvexBrush unitBox(double s = 1.0) {
    return ConvexBrush::box(glm::dvec3(0), glm::dvec3(s), MaterialPtr(), 1.0);
  }

  static io::Connection conn(const char *output, const char *target, const char *input, const char *param = "",
                             float delay = 0.0f, int times = -1) {
    io::Connection c;
    c.output = QString::fromLatin1(output);
    c.target = QString::fromLatin1(target);
    c.input = QString::fromLatin1(input);
    c.param = QString::fromLatin1(param);
    c.delay = delay;
    c.times = times;
    return c;
  }
  static Entity ent(quint32 id, const char *cls, const char *name, io::ConnectionList outs = {},
                    QVariantMap props = {}) {
    Entity e;
    e.id = kEntityIdBase + id;
    e.classname = QString::fromLatin1(cls);
    e.name = QString::fromLatin1(name);
    e.outputs = std::move(outs);
    e.props = std::move(props);
    return e;
  }
  static double simValue(const io::Simulator &s, quint32 id) { return s.numericState(kEntityIdBase + id); }
  static bool hasIssue(const std::vector<io::Issue> &v, const char *needle) {
    for (const auto &i: v)
      if (i.text.contains(QLatin1String(needle), Qt::CaseInsensitive))
        return true;
    return false;
  }

private slots:
  void boxFromPlanes() {
    const ConvexBrush b = unitBox(2.0);
    QVERIFY(b.valid());
    QCOMPARE(int(b.faces.size()), 6);
    QVERIFY(std::fabs(b.volume() - 8.0) < 1e-6);
    for (const auto &p: b.polygons)
      QCOMPARE(int(p.size()), 4);
  }

  void unboundedRejected() {
    std::vector<BrushFace> planes(3);
    planes[0].normal = {1, 0, 0};
    planes[1].normal = {0, 1, 0};
    planes[2].normal = {0, 0, 1};
    QVERIFY(!ConvexBrush::fromPlanes(planes).has_value());
  }

  void splitConservesVolume() {
    const ConvexBrush b = unitBox(4.0);
    std::optional<ConvexBrush> f, k;
    b.split(glm::normalize(glm::dvec3(1, 1, 0)), 2.0, f, k);
    QVERIFY(f && k);
    QVERIFY(std::fabs(f->volume() + k->volume() - 64.0) < 1e-6);
  }

  void subtractCubeFromCube() {
    const ConvexBrush big = unitBox(3.0);
    const ConvexBrush hole = ConvexBrush::box(glm::dvec3(1), glm::dvec3(2), MaterialPtr(), 1.0);
    const auto pieces = big.subtract(hole);
    QCOMPARE(int(pieces.size()), 6);
    double v = 0;
    for (const auto &p: pieces) {
      QVERIFY(p.valid());
      v += p.volume();
    }
    QVERIFY(std::fabs(v - 26.0) < 1e-6);
  }

  void subtractDisjointKeepsOriginal() {
    const ConvexBrush a = unitBox(1.0);
    const ConvexBrush b = ConvexBrush::box(glm::dvec3(5), glm::dvec3(6), MaterialPtr(), 1.0);
    QCOMPARE(int(a.subtract(b).size()), 1);
  }

  void hollowMakesWalls() {
    const ConvexBrush b = unitBox(10.0);
    const auto walls = b.hollow(1.0);
    QCOMPARE(int(walls.size()), 6);
    double v = 0;
    for (const auto &w: walls)
      v += w.volume();
    QVERIFY(std::fabs(v - (1000.0 - 512.0)) < 1e-5);
  }

  void hullOfCubeCorners() {
    std::vector<glm::dvec3> pts;
    for (int i = 0; i < 8; ++i)
      pts.push_back({double(i & 1), double((i >> 1) & 1), double((i >> 2) & 1)});
    pts.push_back({0.5, 0.5, 0.5}); // interior point must be ignored
    const auto h = ConvexBrush::fromPoints(pts, MaterialPtr(), 1.0);
    QVERIFY(h && h->valid());
    QCOMPARE(int(h->faces.size()), 6);
  }

  void cylinderHasNSides() {
    PrimitiveParams p;
    p.sides = 12;
    const auto c = buildPrimitive(PrimitiveKind::Cylinder, glm::dvec3(0), glm::dvec3(10), p, MaterialPtr(), 1.0);
    QCOMPARE(int(c.size()), 1);
    QCOMPARE(int(c[0].faces.size()), 14);
    const double r = 5.0, area = 0.5 * 12 * r * r * std::sin(2 * M_PI / 12);
    QVERIFY(std::fabs(c[0].volume() - area * 10.0) < area * 10.0 * 1e-5);
  }

  void stairsAndArch() {
    PrimitiveParams p;
    p.steps = 5;
    QCOMPARE(int(buildPrimitive(PrimitiveKind::Stairs, glm::dvec3(0), glm::dvec3(10), p, MaterialPtr(), 1.0).size()), 5);
    p.segments = 6;
    const auto arch = buildPrimitive(PrimitiveKind::Arch, glm::dvec3(0), glm::dvec3(10), p, MaterialPtr(), 1.0);
    QCOMPARE(int(arch.size()), 6);
    for (const auto &a: arch)
      QVERIFY(a.valid());
  }

  // Stairs are n non-overlapping boxes whose volume is the staircase, in every direction and style.
  void stairsAreOptimal() {
    const double size = 8.0;
    for (int dir = 0; dir < 4; ++dir) {
      for (int style = 0; style < 3; ++style) {
        PrimitiveParams p;
        p.steps = 8;
        p.stairDirection = dir;
        p.stairStyle = style;
        const auto steps = buildPrimitive(PrimitiveKind::Stairs, glm::dvec3(0), glm::dvec3(size), p, MaterialPtr(), 1.0);
        QCOMPARE(int(steps.size()), style == 2 ? 9 : 8);
        double v = 0;
        for (const auto &b: steps) {
          QVERIFY(b.valid());
          v += b.volume();
        }
        const double n = 8, slice = size * size * size / n; // footprint strip x full height
        const double solid = slice * (n + 1) / 2.0;          // sum of (s+1)/n heights
        const double treads = size * size * size / (n * n) * n;
        const double support = 0.5 * (size * (n - 1) / n) * (size * (n - 1) / n) * size;
        const double expect = style == 0 ? solid : style == 1 ? treads : treads + support;
        QVERIFY2(std::fabs(v - expect) < 1e-4 * expect, qPrintable(QStringLiteral("dir %1 style %2: %3 vs %4")
                                                                          .arg(dir).arg(style).arg(v).arg(expect)));
        // No two pieces share volume: the sum of pairwise intersections is zero.
        for (size_t i = 0; i < steps.size(); ++i)
          for (size_t j = i + 1; j < steps.size(); ++j)
            if (const auto x = steps[i].intersect(steps[j]))
              QVERIFY(x->volume() < 1e-6);
        // The top step is at the climb end.
        const auto &last = steps[size_t(p.steps - 1)];
        glm::dvec3 c(0);
        for (const auto &poly: last.polygons)
          for (const auto &q: poly)
            c += q / double(poly.size() * last.polygons.size());
        const double climb = dir == 0 ? c.x : dir == 1 ? size - c.x : dir == 2 ? c.z : size - c.z;
        QVERIFY(climb > size * 0.8);
      }
    }
  }

  // Arch pieces are valid and disjoint; archway + opening fill the box exactly.
  void archesAreClean() {
    const glm::dvec3 mn(0, 0, 0), mx(12, 16, 2);
    for (int prof = 0; prof < 5; ++prof) {
      ArchParams p;
      p.profile = ArchProfile(prof);
      p.segments = 8;
      p.thickness = 1.5;
      p.spring = 3.0;
      double volumes[3] = {0, 0, 0};
      for (int mode = 0; mode < 3; ++mode) {
        p.mode = ArchMode(mode);
        // The opening for the archway is the box minus jambs and crown.
        const glm::dvec3 omn = mode == 2 ? glm::dvec3(1.5, 0, 0) : mn;
        const glm::dvec3 omx = mode == 2 ? glm::dvec3(10.5, 14.5, 2) : mx;
        const auto pieces = buildArch(omn, omx, 0, p, MaterialPtr(), 1.0);
        QVERIFY2(!pieces.empty(), qPrintable(QStringLiteral("profile %1 mode %2").arg(prof).arg(mode)));
        for (size_t i = 0; i < pieces.size(); ++i) {
          QVERIFY(pieces[i].valid());
          volumes[mode] += pieces[i].volume();
          for (size_t j = i + 1; j < pieces.size(); ++j)
            if (const auto x = pieces[i].intersect(pieces[j]))
              QVERIFY2(x->volume() < 1e-6, qPrintable(QStringLiteral("profile %1 mode %2 overlap %3/%4")
                                                          .arg(prof).arg(mode).arg(i).arg(j)));
        }
        if (mode == 1)
          QCOMPARE(int(pieces.size()), 8 + 2);
      }
      // Archway + carved opening = the whole box.
      const double box = 12.0 * 16.0 * 2.0;
      QVERIFY2(std::fabs(volumes[1] + volumes[2] - box) < 1e-3 * box,
               qPrintable(QStringLiteral("profile %1: %2 + %3 vs %4").arg(prof).arg(volumes[1]).arg(volumes[2]).arg(box)));
    }
  }

  // Vertex tool kernel.
  void vertexEdits() {
    ConvexBrush b = unitBox(4.0);
    for (auto &f: b.faces)
      f.basis = paraxialBasis(f.normal, 0.25);
    const auto v = b.vertices();
    QCOMPARE(int(v.size()), 8);
    // Edge move: the edge along x at y = 4, z = 4.
    std::vector<glm::dvec3> moved = v;
    int n = 0;
    for (auto &p: moved)
      if (p.y > 3.9 && p.z > 3.9) {
        p.y += 2.0;
        ++n;
      }
    QCOMPARE(n, 2);
    auto r = moveBrushVertices(b, moved, {}, true);
    QVERIFY(r.brush && r.brush->valid());
    QCOMPARE(r.absorbed, 0);
    // A wedge-topped box: 4*4*4 + 0.5*4*2*4 = 80
    QVERIFY(std::fabs(r.brush->volume() - 80.0) < 1e-6);
    // UV lock: a moved corner keeps its texture coordinate on the front face.
    {
      const glm::dvec3 corner(4, 4, 4), after(4, 6, 4);
      const BrushFace *of = b.mostParallelFace({0, 0, 1});
      const BrushFace *nf = r.brush->mostParallelFace({0, 0, 1});
      QVERIFY(of && nf);
      const glm::vec2 uvBefore = of->basis.uvAt(glm::vec3(corner));
      const glm::vec2 uvAfter = nf->basis.uvAt(glm::vec3(after));
      const glm::vec2 d = uvBefore - uvAfter;
      // Offsets are wrapped to [0,1): compare modulo whole tiles.
      QVERIFY(std::fabs(d.x - std::round(d.x)) < 1e-4 && std::fabs(d.y - std::round(d.y)) < 1e-4);
    }
    // Pushing a corner into the brush is reported as absorbed.
    moved = v;
    for (auto &p: moved)
      if (p.x > 3.9 && p.y > 3.9 && p.z > 3.9)
        p = glm::dvec3(2, 2, 2);
    r = moveBrushVertices(b, moved, {}, false);
    QVERIFY(r.brush);
    QCOMPARE(r.absorbed, 1);
    // Welding the four top corners into one: a pyramid-topped box, 5 corners.
    moved = v;
    for (auto &p: moved)
      if (p.y > 3.9)
        p = glm::dvec3(2, 4, 2);
    r = moveBrushVertices(b, moved, {}, false);
    QVERIFY(r.brush && r.brush->valid());
    QCOMPARE(int(r.brush->vertices().size()), 5);
    // Scaling about a pivot.
    moved = v;
    const glm::dvec3 pivot(2, 2, 2);
    for (auto &p: moved)
      p = pivot + (p - pivot) * 0.5;
    r = moveBrushVertices(b, moved, {}, false);
    QVERIFY(r.brush && std::fabs(r.brush->volume() - 8.0) < 1e-6);
    // Splitting: an extra corner above the top face.
    r = moveBrushVertices(b, v, {glm::dvec3(2, 5, 2)}, false);
    QVERIFY(r.brush && r.brush->vertices().size() == 9 && r.absorbed == 0);
    QCOMPARE(int(brushEdges(b).size()), 12);
  }

  // Layout Designer generator.
  static bool boxesDisjoint(const std::vector<layout::Box> &boxes) {
    for (size_t i = 0; i < boxes.size(); ++i)
      for (size_t j = i + 1; j < boxes.size(); ++j) {
        const auto &a = boxes[i], &b = boxes[j];
        bool sep = false;
        for (int k = 0; k < 3; ++k)
          sep = sep || a.mx[k] <= b.mn[k] || b.mx[k] <= a.mn[k];
        if (!sep)
          return false;
      }
    return true;
  }

  void layoutCorridor() {
    layout::Doc d;
    d.config.wall = 16;
    layout::Path p;
    p.id = 1;
    p.kind = layout::Corridor;
    p.nodes = {QPoint(0, 0), QPoint(320, 0)};
    p.segs = {layout::Segment {128, 128, 0}};
    d.paths.push_back(p);
    const auto boxes = layout::generate(d);
    // floor + ceiling + two side walls + two end caps
    QCOMPARE(int(boxes.size()), 6);
    QVERIFY(boxesDisjoint(boxes));
    // Inside the corridor: floor below 0 and ceiling above 128, open in between.
    const auto col = layout::solidColumn(d, 80, 4);
    QCOMPARE(int(col.size()), 2);
    QCOMPARE(col[0], std::make_pair(-16, 0));
    QCOMPARE(col[1], std::make_pair(128, 144));
    // Interior width is exactly 128: walls start at y = +-64.
    QVERIFY(layout::solidColumn(d, 80, 60).size() == 2);
    QVERIFY(layout::solidColumn(d, 80, 68).size() == 1);
  }

  void layoutLAndWidths() {
    layout::Doc d;
    d.config.wall = 16;
    layout::Path p;
    p.id = 1;
    p.nodes = {QPoint(0, 0), QPoint(320, 0), QPoint(320, 320)};
    p.segs = {layout::Segment {128, 128, 0}, layout::Segment {192, 128, 0}};
    d.paths.push_back(p);
    const auto boxes = layout::generate(d);
    QVERIFY(boxesDisjoint(boxes));
    QVERIFY(boxes.size() < 20);
    // The 192-wide leg: x = 320 +- 96 is open at y = 240.
    QCOMPARE(int(layout::solidColumn(d, 320 - 92, 240).size()), 2);
    QCOMPARE(int(layout::solidColumn(d, 320 + 100, 240).size()), 1);
    QCOMPARE(layout::segmentLength(d.paths[0], 0), 320 + 128);
  }

  void layoutRoomAndVent() {
    layout::Doc d;
    d.config.wall = 16;
    layout::Path c;
    c.id = 1;
    c.nodes = {QPoint(0, 0), QPoint(320, 0)};
    c.segs = {layout::Segment {128, 128, 0}};
    d.paths.push_back(c);
    layout::RoomShape r;
    r.id = 2;
    r.rects = {QRect(320, -160, 320, 320)};
    r.height = 192;
    d.rooms.push_back(r);
    const auto boxes = layout::generate(d);
    QVERIFY(boxesDisjoint(boxes));
    // Where the corridor meets the room (x in [304,320)): open for the corridor's height, header above.
    auto col = layout::solidColumn(d, 316, 4);
    QCOMPARE(col.front(), std::make_pair(-16, 0));
    QCOMPARE(col.back(), std::make_pair(128, 208));
    // The room's own wall away from the corridor is solid to the full height.
    col = layout::solidColumn(d, 316, 124);
    QCOMPARE(int(col.size()), 1);
    QCOMPARE(col.front(), std::make_pair(-16, 208));

    // A vent from the outside into the room's far wall at height 16..64.
    layout::Path v;
    v.id = 3;
    v.kind = layout::Vent;
    v.nodes = {QPoint(640, 64), QPoint(960, 64)};
    v.segs = {layout::Segment {48, 48, 16}};
    d.paths.push_back(v);
    // The room wall column on the vent's line (x in [640,656)) has a hole 16..64.
    col = layout::solidColumn(d, 644, 68);
    QCOMPARE(int(col.size()), 2);
    QCOMPARE(col[0], std::make_pair(-16, 16));
    QCOMPARE(col[1], std::make_pair(64, 208));
    QVERIFY(boxesDisjoint(layout::generate(d)));

    // Doorway contact: a 64 x 112 doorway instead of the full cross-section.
    d.rooms[0].contact = layout::ContactDoorway;
    col = layout::solidColumn(d, 316, 4);
    QCOMPARE(col.back(), std::make_pair(112, 208));
    col = layout::solidColumn(d, 316, 44); // lateral 44 > 32: sealed
    QCOMPARE(int(col.size()), 1);
    QVERIFY(boxesDisjoint(layout::generate(d)));

    // JSON round trip.
    const auto back = layout::fromJson(layout::toJson(d));
    QCOMPARE(int(back.paths.size()), 2);
    QCOMPARE(back.paths[1].segs[0].width, 48);
    QCOMPARE(back.paths[1].nodes[1], QPoint(960, 64));
    QCOMPARE(back.rooms[0].rects[0], QRect(320, -160, 320, 320));
    QCOMPARE(back.rooms[0].contact, int(layout::ContactDoorway));
    QCOMPARE(back.config.wall, 16);
  }

  void layoutExactWall() {
    // The default 10-unit wall is exact, not rounded to a raster.
    layout::Doc d;
    QCOMPARE(d.config.wall, 10);
    layout::Path p;
    p.id = 1;
    p.nodes = {QPoint(0, 0), QPoint(320, 0)};
    p.segs = {layout::Segment {80, 128, 0}};
    d.paths.push_back(p);
    QVERIFY(boxesDisjoint(layout::generate(d)));
    QCOMPARE(int(layout::solidColumn(d, 80, 38).size()), 2); // inside: half width 40
    QCOMPARE(int(layout::solidColumn(d, 80, 41).size()), 1); // wall
    QCOMPARE(int(layout::solidColumn(d, 80, 49).size()), 1); // still wall (40..50)
    QCOMPARE(int(layout::solidColumn(d, 80, 51).size()), 0); // outside
  }

  void layoutSharedWall() {
    layout::Doc d;
    layout::Path p;
    p.id = 1;
    p.nodes = {QPoint(0, 0), QPoint(640, 0)};
    p.segs = {layout::Segment {128, 128, 0}};
    d.paths.push_back(p);
    layout::RoomShape r;
    r.id = 2;
    r.rects = {QRect(128, 0, 256, 256)}; // overlaps the corridor's south half
    r.contact = layout::ContactSharedWall;
    d.rooms.push_back(r);
    QVERIFY(boxesDisjoint(layout::generate(d)));
    // The corridor stays whole and keeps its wall; the room starts behind it.
    QCOMPARE(int(layout::solidColumn(d, 256, 4).size()), 2);
    QCOMPARE(int(layout::solidColumn(d, 256, 69).size()), 1);
    QCOMPARE(int(layout::solidColumn(d, 256, 76).size()), 2);
    const auto ras = layout::rasterize(d);
    int roomCells = 0;
    for (auto it = ras.cells.constBegin(); it != ras.cells.constEnd(); ++it)
      if (it->kind == layout::Room) {
        ++roomCells;
        QVERIFY(ras.cellRect(it.key()).y() >= 74);
      }
    QVERIFY(roomCells > 0);

    // Open contact: the room cuts into the corridor instead.
    d.rooms[0].contact = layout::ContactOpen;
    QCOMPARE(int(layout::solidColumn(d, 256, 69).size()), 2);

    // A shared-wall room drawn 14 units short of the corridor wall is extended to meet it.
    d.rooms[0].contact = layout::ContactSharedWall;
    d.rooms[0].rects = {QRect(128, 88, 256, 168)};
    QVERIFY(boxesDisjoint(layout::generate(d)));
    QCOMPARE(int(layout::solidColumn(d, 256, 76).size()), 2);
    QCOMPARE(int(layout::solidColumn(d, 256, 69).size()), 1);
  }

  void layoutVentAtFloor() {
    layout::Doc d;
    QCOMPARE(d.config.ventElevation, 0);
    QCOMPARE(d.config.roomHeight, 128);
    layout::Path v;
    v.id = 1;
    v.kind = layout::Vent;
    v.nodes = {QPoint(0, 0), QPoint(320, 0)};
    v.segs = {layout::Segment {48, 48, 0}};
    d.paths.push_back(v);
    QVERIFY(boxesDisjoint(layout::generate(d)));
    const auto col = layout::solidColumn(d, 160, 0);
    QCOMPARE(int(col.size()), 2);
    QCOMPARE(col[0], std::make_pair(-8, 0));
    QCOMPARE(col[1], std::make_pair(48, 56));
  }

  void layoutLegacyJson() {
    // Version 1 documents stored shapes in 16-unit grid cells.
    const QJsonObject o = QJsonDocument::fromJson(R"({
      "id": 3, "config": {"cell": 16, "doorMode": 1},
      "paths": [{"id": 1, "kind": 0, "nodes": [[0, 0], [20, 0]], "segs": [[128, 128, 0]]}],
      "rooms": [{"id": 2, "rects": [[20, -10, 20, 20]]}],
      "sealed": [[3, 4]]
    })").object();
    const layout::Doc d = layout::fromJson(o);
    QCOMPARE(d.paths[0].nodes[1], QPoint(320, 0));
    QCOMPARE(d.rooms[0].rects[0], QRect(320, -160, 320, 320));
    QCOMPARE(d.rooms[0].height, 192);
    QCOMPARE(d.rooms[0].contact, int(layout::ContactDoorway));
    QCOMPARE(int(d.sealed.size()), 1);
    QCOMPARE(d.sealed[0], QRect(48, 64, 16, 16));
    QCOMPARE(d.config.wall, 16);
    QVERIFY(boxesDisjoint(layout::generate(d)));
  }

  void textureLockUnderRotation() {
    ConvexBrush b = unitBox(4.0);
    BrushFace &top = b.faces[2]; // +Y
    QVERIFY(top.normal.y > 0.5);
    top.basis = paraxialBasis(top.normal, 0.25);
    top.basis.offsetU = 0.3f;
    const glm::dvec3 p(1.0, 4.0, 3.0); // on the top face
    const glm::dvec2 before(glm::dot(p, glm::dvec3(top.basis.uAxis)) * top.basis.tilesPerUnitU + top.basis.offsetU,
                            glm::dot(p, glm::dvec3(top.basis.vAxis)) * top.basis.tilesPerUnitV + top.basis.offsetV);
    const glm::dmat4 m = glm::translate(glm::dmat4(1.0), glm::dvec3(7, 0, -2))
                         * glm::rotate(glm::dmat4(1.0), glm::radians(90.0), glm::dvec3(0, 1, 0));
    b.transform(m, true);
    const glm::dvec3 q(m * glm::dvec4(p, 1.0));
    const BrushFace *nt = b.mostParallelFace({0, 1, 0});
    const glm::dvec2 after(glm::dot(q, glm::dvec3(nt->basis.uAxis)) * nt->basis.tilesPerUnitU + nt->basis.offsetU,
                           glm::dot(q, glm::dvec3(nt->basis.vAxis)) * nt->basis.tilesPerUnitV + nt->basis.offsetV);
    QVERIFY(glm::length(before - after) < 1e-4);
  }

  // bevel (engine/brush_bevel.*)

  // The top-front edge of a 4-unit box (along X at y = 4, z = 4).
  static bevel::Edge topFront() { return {glm::dvec3(0, 4, 4), glm::dvec3(4, 4, 4)}; }

  static bevel::Params bevelParams(double radius, int segments, bool inverse = false) {
    bevel::Params p;
    p.radius = radius;
    p.segments = segments;
    p.inverse = inverse;
    return p;
  }

  void bevelChamfer() {
    const ConvexBrush b = unitBox(4.0);
    const bevel::Result r = bevel::bevel(b, {topFront()}, {}, bevelParams(1.0, 1));
    QVERIFY(r.ok());
    QCOMPARE(int(r.pieces.size()), 1);
    QCOMPARE(int(r.pieces[0].faces.size()), 7);
    QVERIFY(std::fabs(r.pieces[0].volume() - (64.0 - 2.0)) < 1e-6);
    QCOMPARE(bevel::kindText(r.kinds.front(), r.segments), QStringLiteral("Chamfer"));
  }

  void bevelRound() {
    const ConvexBrush b = unitBox(4.0);
    const bevel::Result r = bevel::bevel(b, {topFront()}, {}, bevelParams(2.0, 4));
    QVERIFY(r.ok());
    QCOMPARE(r.segments, 4);
    const ConvexBrush &p = r.pieces.front();
    QCOMPARE(int(p.faces.size()), 10);
    // The round's corners lie on the circle of radius 2 about the line y = z = 2.
    for (const glm::dvec3 &v: p.vertices())
      if (v.y > 2.0 + 1e-9 && v.z > 2.0 + 1e-9)
        QVERIFY2(std::fabs(glm::length(glm::dvec2(v.y - 2.0, v.z - 2.0)) - 2.0) < 1e-4,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(v.x).arg(v.y).arg(v.z)));
    // Less than the chamfer takes, more than the true round would.
    QVERIFY(p.volume() > 64.0 - 8.0 && p.volume() < 64.0 - (4.0 - 3.14159265) * 4.0 + 1e-9);
    // A radius of 1 is too small for 4 chords of half a unit: 3.
    QCOMPARE(bevel::bevel(b, {topFront()}, {}, bevelParams(1.0, 4)).segments, 3);
  }

  void bevelAllEdgesAndLimits() {
    const ConvexBrush b = unitBox(4.0);
    std::vector<bevel::Edge> all;
    for (const auto &[a, e]: brushEdges(b))
      all.push_back({a, e});
    QCOMPARE(int(all.size()), 12);
    const bevel::Result r = bevel::bevel(b, all, {}, bevelParams(1.0, 2));
    QVERIFY(r.ok());
    QVERIFY(r.pieces.front().valid());
    // The radius is limited by the faces: a 4-unit face takes a radius of 4.
    QVERIFY(std::fabs(bevel::limit(b, {topFront()}, {}, false) - 4.0) < 1e-6);
    const bevel::Result big = bevel::bevel(b, {topFront()}, {}, bevelParams(10.0, 1));
    QVERIFY(big.ok());
    QVERIFY(std::fabs(big.radius - 4.0) < 1e-6);
    // Two parallel edges on one face share it.
    const bevel::Edge topBack {glm::dvec3(0, 4, 0), glm::dvec3(4, 4, 0)};
    QVERIFY(std::fabs(bevel::limit(b, {topFront(), topBack}, {}, false) - 2.0) < 1e-6);
    // A thin plate has no room; a diagonal is no edge; nothing picked.
    const ConvexBrush thin = boxAt(glm::dvec3(0), glm::dvec3(4, 0.2, 4));
    QVERIFY(bevel::bevel(thin, {{glm::dvec3(0, 0.2, 4), glm::dvec3(4, 0.2, 4)}}, {}, bevelParams(1.0, 4)).issue
            == bevel::Issue::NoRoom);
    QVERIFY(bevel::bevel(b, {{glm::dvec3(0, 4, 0), glm::dvec3(4, 4, 4)}}, {}, bevelParams(1.0, 4)).issue
            == bevel::Issue::NotAnEdge);
    QVERIFY(bevel::bevel(b, {}, {}, bevelParams(1.0, 4)).issue == bevel::Issue::NoEdge);
    // Short chords are merged: a tiny radius cannot have 32 segments.
    QVERIFY(bevel::cappedSegments(1.0, 32, 3.14159265 / 2.0) < 32);
  }

  void bevelTextureWraps() {
    ConvexBrush b = unitBox(4.0);
    for (BrushFace &f: b.faces)
      f.basis = paraxialBasis(f.normal, 0.25);
    const bevel::Result r = bevel::bevel(b, {topFront()}, {}, bevelParams(2.0, 4));
    QVERIFY(r.ok());
    const ConvexBrush &p = r.pieces.front();
    auto axisAligned = [](const glm::dvec3 &n) {
      return std::fabs(n.x) > 0.999 || std::fabs(n.y) > 0.999 || std::fabs(n.z) > 0.999;
    };
    // Neighbouring faces of the round show the same texel along the line they share.
    int joins = 0;
    for (size_t i = 0; i < p.faces.size(); ++i)
      for (size_t j = i + 1; j < p.faces.size(); ++j) {
        if (axisAligned(p.faces[i].normal) || axisAligned(p.faces[j].normal))
          continue;
        std::vector<glm::dvec3> shared;
        for (const glm::dvec3 &a: p.polygons[i])
          for (const glm::dvec3 &c: p.polygons[j])
            if (glm::length(a - c) < 1e-6)
              shared.push_back(a);
        if (shared.size() < 2)
          continue;
        ++joins;
        for (const glm::dvec3 &s: shared)
          QVERIFY(glm::length(p.faces[i].basis.uvAt(glm::vec3(s)) - p.faces[j].basis.uvAt(glm::vec3(s))) < 1e-3f);
      }
    QCOMPARE(joins, 3);
  }

  void bevelCove() {
    const ConvexBrush b = unitBox(4.0);
    const bevel::Result r = bevel::bevel(b, {topFront()}, {}, bevelParams(1.0, 4, true));
    QVERIFY(r.ok());
    QCOMPARE(r.kinds.front(), bevel::Kind::Cove);
    QVERIFY(!r.pieces.empty() && r.pieces.size() <= 4 + 3);
    double vol = 0.0;
    for (const ConvexBrush &p: r.pieces) {
      QVERIFY(p.valid());
      vol += p.volume();
    }
    // The cove takes less than the 1 x 1 corner square along the edge.
    QVERIFY(vol < 64.0 && vol > 64.0 - 4.0);
  }

  // The editor's scale: a 128-unit box through its mesh, radius 16, 4 segments, every edge.
  void bevelWorldScaleEveryEdge() {
    const ConvexBrush box = boxAt(glm::dvec3(0), glm::dvec3(12.8));
    GameObject obj(box.toMesh());
    const auto b = ConvexBrush::fromObject(obj);
    QVERIFY(b.has_value());
    bevel::Params p = bevelParams(1.6, 4);
    p.sourceUnit = 0.1;
    for (const auto &[a, e]: brushEdges(*b)) {
      const bevel::Result r = bevel::bevel(*b, {{a, e}}, {}, p);
      QVERIFY2(r.ok(), qPrintable(QStringLiteral("edge (%1 %2 %3)-(%4 %5 %6): %7")
                                      .arg(a.x).arg(a.y).arg(a.z).arg(e.x).arg(e.y).arg(e.z)
                                      .arg(bevel::issueText(r.issue))));
      QCOMPARE(r.segments, 4);
      QCOMPARE(int(r.pieces.front().faces.size()), 10);
    }
  }

  // A corner with a sliver chamfer: the bevel steps over the sliver and rounds between the big faces.
  void bevelOverSliver() {
    ConvexBrush b = unitBox(4.0);
    BrushFace cut;
    cut.normal = glm::normalize(glm::dvec3(0, 1, 1));
    cut.distance = glm::dot(cut.normal, glm::dvec3(0, 4, 3.7)); // 0.3 off the top-front edge
    QVERIFY(b.clip(cut));
    QCOMPARE(int(b.faces.size()), 7);
    // Either edge of the sliver: the corner (0..4, 4, 4) with its full room.
    for (const bevel::Edge &e: {bevel::Edge {glm::dvec3(0, 4, 3.7), glm::dvec3(4, 4, 3.7)},
                                bevel::Edge {glm::dvec3(0, 3.7, 4), glm::dvec3(4, 3.7, 4)}}) {
      QVERIFY(std::fabs(bevel::limit(b, {e}, {}, false) - 4.0) < 1e-6);
      const bevel::Result r = bevel::bevel(b, {e}, {}, bevelParams(2.0, 4));
      QVERIFY(r.ok());
      QCOMPARE(r.segments, 4);
      // The sliver is gone: the box's 6 faces + 4 chords.
      QCOMPARE(int(r.pieces.front().faces.size()), 10);
    }
  }

  // Smooth shading: the round's inner corners average their two segments, 90° corners stay sharp,
  // big faces stay (nearly) flat.
  void smoothShadingNormals() {
    const ConvexBrush box = boxAt(glm::dvec3(0), glm::dvec3(12.8));
    bevel::Params p = bevelParams(1.6, 4);
    p.sourceUnit = 0.1;
    const bevel::Result r = bevel::bevel(box, {{glm::dvec3(0, 12.8, 12.8), glm::dvec3(12.8, 12.8, 12.8)}}, {}, p);
    QVERIFY(r.ok());
    const ConvexBrush &b = r.pieces.front();
    std::vector<glm::dvec3> normals;
    for (const BrushFace &f: b.faces)
      normals.push_back(f.normal);
    const auto n = smoothPolygonNormals(b.polygons, normals);
    for (size_t f = 0; f < b.faces.size(); ++f) {
      const bool chord = std::fabs(b.faces[f].normal.y) > 1e-6 && std::fabs(b.faces[f].normal.z) > 1e-6;
      for (size_t k = 0; k < b.polygons[f].size(); ++k) {
        const glm::dvec3 &v = b.polygons[f][k];
        const double dev = std::acos(std::clamp(glm::dot(glm::dvec3(n[f][k]), b.faces[f].normal), -1.0, 1.0));
        if (!chord) {
          // The box's own faces stay within a degree of flat.
          QVERIFY2(dev < 0.02, qPrintable(QStringLiteral("face %1 corner %2").arg(f).arg(k)));
        } else if (v.y < 12.8 - 1e-6 && v.z < 12.8 - 1e-6 && v.y > 11.2 + 1e-6 && v.z > 11.2 + 1e-6) {
          // An inner corner of the round: halfway to the next segment (11.25°).
          QVERIFY(std::fabs(dev - glm::radians(11.25)) < 1e-3);
        }
      }
    }
  }

  void bevelInnerFill() {
    // A slab against a block: the slab's top edge at x = 4 meets the block's side.
    const ConvexBrush block = boxAt(glm::dvec3(0), glm::dvec3(4, 4, 4));
    const ConvexBrush slab = boxAt(glm::dvec3(4, 0, 0), glm::dvec3(8, 1, 4));
    const bevel::Edge inner {glm::dvec3(4, 1, 0), glm::dvec3(4, 1, 4)};
    const bevel::Result r = bevel::bevel(slab, {inner}, {block}, bevelParams(1.0, 4, true));
    QVERIFY(r.ok());
    QCOMPARE(r.kinds.front(), bevel::Kind::Fill);
    QCOMPARE(int(r.pieces.size()), 1);
    QVERIFY(std::fabs(r.pieces.front().volume() - slab.volume()) < 1e-9);
    QVERIFY(!r.added.empty());
    for (const ConvexBrush &a: r.added) {
      QVERIFY(a.valid());
      glm::dvec3 mn, mx;
      a.bounds(mn, mx);
      QVERIFY(mn.x > 4.0 - 1e-6 && mx.x < 5.0 + 1e-6 && mn.y > 1.0 - 1e-6 && mx.y < 2.0 + 1e-6);
      QVERIFY(mn.z > -1e-6 && mx.z < 4.0 + 1e-6);
    }
  }

  // Fit on a turned box (texture lock off): every corner lands on a tile corner.
  void fitOnRotatedBrush() {
    const glm::dmat4 turns[] = {
        glm::dmat4(1.0),
        glm::rotate(glm::dmat4(1.0), glm::radians(30.0), glm::dvec3(0, 1, 0)),
        glm::rotate(glm::dmat4(1.0), glm::radians(30.0), glm::dvec3(1, 0, 0)),
        glm::rotate(glm::dmat4(1.0), glm::radians(25.0), glm::dvec3(0, 1, 0))
            * glm::rotate(glm::dmat4(1.0), glm::radians(40.0), glm::dvec3(1, 0, 0))
            * glm::rotate(glm::dmat4(1.0), glm::radians(-15.0), glm::dvec3(0, 0, 1)),
    };
    for (int k = 0; k < 4; ++k) {
      ConvexBrush b = unitBox(4.0);
      for (BrushFace &f: b.faces)
        f.basis = paraxialBasis(f.normal, 0.25);
      b.transform(turns[k], false);
      QVERIFY(b.valid());
      for (size_t i = 0; i < b.faces.size(); ++i) {
        std::vector<glm::vec3> poly;
        for (const glm::dvec3 &p: b.polygons[i])
          poly.emplace_back(p);
        const glm::vec3 n(b.faces[i].normal);
        FaceTexBasis fit = b.faces[i].basis;
        const FaceTexBasis before = fit;
        fitBasisToFace(fit, poly, n, 1, 1);
        QVERIFY(std::fabs(glm::dot(glm::cross(fit.uAxis, fit.vAxis), n) + 1.0f) < 1e-4f);
        std::set<int> corners;
        for (const glm::vec3 &p: poly) {
          const glm::vec2 uv = fit.uvAt(p);
          const float cu = std::round(uv.x), cv = std::round(uv.y);
          QVERIFY2(std::fabs(uv.x - cu) < 1e-4f && std::fabs(uv.y - cv) < 1e-4f,
                   qPrintable(QStringLiteral("turn %1 face %2: uv %3 %4").arg(k).arg(i).arg(uv.x).arg(uv.y)));
          QVERIFY(cu >= 0.0f && cu <= 1.0f && cv >= 0.0f && cv <= 1.0f);
          corners.insert(int(cu) * 2 + int(cv));
        }
        QCOMPARE(int(corners.size()), 4);
        if (k == 0) {
          // An unrotated box keeps its axes.
          QVERIFY(glm::length(fit.uAxis - before.uAxis) < 1e-5f);
          QVERIFY(glm::length(fit.vAxis - before.vAxis) < 1e-5f);
        }
      }
    }
  }

  void meshRoundTrip() {
    PrimitiveParams p;
    p.sides = 8;
    const auto c = buildPrimitive(PrimitiveKind::Cylinder, glm::dvec3(0), glm::dvec3(2), p, MaterialPtr(), 1.0);
    auto mesh = c[0].toMesh();
    QCOMPARE(mesh->faceCount(), 10);
    GameObject obj(mesh);
    const auto back = ConvexBrush::fromObject(obj);
    QVERIFY(back.has_value());
    QCOMPARE(int(back->faces.size()), 10);
    QVERIFY(std::fabs(back->volume() - c[0].volume()) < 1e-4);
  }

  // The light-atlas lookup in scene.frag must agree with the matrices vk_light_shadows.cpp renders with.
  void lightAtlasProjectionMatches() {
    static const glm::vec3 F[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 U[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    // Vulkan clip-space correction (QRhi::clipSpaceCorrMatrix): flip Y, depth to 0..1.
    glm::mat4 corr(1.0f);
    corr[1][1] = -1.0f;
    corr[2][2] = 0.5f;
    corr[3][2] = 0.5f;
    const glm::vec3 light(3, 4, 5);
    const glm::vec3 pts[] = {{9, 6, 4}, {-2, 5, 6}, {4, 12, 3}, {2, -3, 7}, {5, 6, 13}, {1, 2, -4}};
    for (const glm::vec3 &w: pts) {
      const glm::vec3 d = w - light;
      const glm::vec3 a = glm::abs(d);
      const int face = (a.x >= a.y && a.x >= a.z) ? (d.x > 0 ? 0 : 1) : (a.y >= a.z) ? (d.y > 0 ? 2 : 3) : (d.z > 0 ? 4 : 5);
      const glm::mat4 view = glm::lookAt(light, light + F[face], U[face]);
      const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.05f, 50.0f);
      const glm::vec4 clip = corr * proj * view * glm::vec4(w, 1.0f);
      const glm::vec2 viaMatrix = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
      const glm::vec3 R = glm::normalize(glm::cross(F[face], U[face]));
      const glm::vec3 Up = glm::cross(R, F[face]);
      glm::vec2 ndc = glm::vec2(glm::dot(d, R), glm::dot(d, Up)) / glm::dot(d, F[face]);
      ndc.y *= -1.0f; // Vulkan: NDC y down
      const glm::vec2 viaShader = ndc * 0.5f + 0.5f;
      QVERIFY2(glm::length(viaMatrix - viaShader) < 1e-4f, qPrintable(QStringLiteral("face %1").arg(face)));
    }
  }

  static ConvexBrush boxAt(const glm::dvec3 &mn, const glm::dvec3 &mx) {
    return ConvexBrush::box(mn, mx, MaterialPtr(), 1.0);
  }

  void faceGroupsSurviveBrushOps() {
    ConvexBrush b = unitBox(4.0);
    for (size_t i = 0; i < b.faces.size(); ++i)
      b.faces[i].groups = {facegroup::kFirstUser + quint32(i)};
    const FaceGroupSet top = b.mostParallelFace({0, 1, 0})->groups;
    std::optional<ConvexBrush> front, back;
    b.split({1, 0, 0}, 2.0, front, back);
    QVERIFY(front && back);
    QCOMPARE(front->mostParallelFace({0, 1, 0})->groups, top);
    QCOMPARE(back->mostParallelFace({0, 1, 0})->groups, top);
    QCOMPARE(back->mostParallelFace({1, 0, 0})->groups, b.mostParallelFace({1, 0, 0})->groups);

    auto mesh = b.toMesh();
    GameObject obj(mesh);
    const auto round = ConvexBrush::fromObject(obj);
    QVERIFY(round.has_value());
    for (const auto &f: round->faces)
      QCOMPARE(f.groups, b.mostParallelFace(f.normal)->groups);

    // A hole cut through the box: the new sides inside take the cutter's groups.
    ConvexBrush cutter = boxAt({1, -1, 1}, {3, 5, 3});
    for (auto &f: cutter.faces)
      f.groups = {99};
    int holeSides = 0;
    for (const auto &piece: b.subtract(cutter))
      for (const auto &f: piece.faces)
        holeSides += f.groups == FaceGroupSet {99} ? 1 : 0;
    QCOMPARE(holeSides, 4);

    FaceGroupSet set;
    facegroup::insert(set, 7);
    facegroup::insert(set, 3);
    facegroup::insert(set, 7);
    QCOMPARE(set, (FaceGroupSet {3, 7}));
    facegroup::erase(set, 3);
    QCOMPARE(set, (FaceGroupSet {7}));
  }

  void faceGroupTableRules() {
    FaceGroupTable t;
    QCOMPARE(t.name(facegroup::kFloor), QStringLiteral("Floor"));
    const quint32 brick = t.ensure(QStringLiteral("brick"));
    QVERIFY(brick >= facegroup::kFirstUser);
    QCOMPARE(t.ensure(QStringLiteral("BRICK")), brick);
    QVERIFY(!t.rename(facegroup::kWall, QStringLiteral("x")));
    QVERIFY(!t.remove(facegroup::kWall));
    QVERIFY(!t.rename(brick, QStringLiteral("floor")));
    QVERIFY(t.rename(brick, QStringLiteral("stone")));
    QVERIFY(t.remove(brick));
    QVERIFY(t.find(brick) == nullptr);
  }

  void fmapFaceGroupsRoundTrip() {
    fmap::MeshRecord m;
    m.id = 1;
    m.vertices.resize(3);
    m.indices = {0, 1, 2};
    fmap::FaceRecord f;
    f.indexOffset = 0;
    f.indexCount = 3;
    f.material = QStringLiteral("dev");
    f.groups = {facegroup::kWall, 17};
    m.faces.push_back(f);
    fmap::Writer w;
    fmap::writeMeshRecord(w, m);
    fmap::Reader r(w.data());
    const auto back = fmap::readMeshRecord(r, 2);
    QVERIFY(r.ok());
    QCOMPARE(int(back.faces.size()), 1);
    QCOMPARE(back.faces[0].groups, f.groups);

    fmap::Document doc;
    doc.meshes.push_back(m);
    doc.faceGroups.push_back({17, QStringLiteral("brick"), QColor(10, 20, 30), 0});
    const QString path = QDir::temp().filePath(QStringLiteral("facegroups_test.ghmap"));
    QString error;
    QVERIFY2(fmap::saveMap(path, doc, &error), qPrintable(error));
    fmap::Document loaded;
    QVERIFY2(fmap::loadMap(path, loaded, &error), qPrintable(error));
    QFile::remove(path);
    QCOMPARE(int(loaded.faceGroups.size()), 1);
    QCOMPARE(loaded.faceGroups[0].name, QStringLiteral("brick"));
    QCOMPARE(loaded.meshes[0].faces[0].groups, f.groups);
  }

  void layoutFaceCategories() {
    layout::Doc d;
    layout::RoomShape r;
    r.id = 1;
    r.rects = {QRect(0, 0, 256, 256)};
    r.height = 128;
    d.rooms.push_back(r);
    const auto boxes = layout::generate(d);
    QVERIFY(boxesDisjoint(boxes));
    long long floorArea = 0, ceilArea = 0;
    int walls = 0;
    for (const auto &b: boxes) {
      const glm::ivec3 e = b.mx - b.mn;
      for (int s = 0; s < 6; ++s) {
        const auto c = b.faces[size_t(s)];
        if (c == layout::FaceFloor) {
          QCOMPARE(s, 4);
          floorArea += (long long)e.x * e.y;
        } else if (c == layout::FaceCeiling) {
          QCOMPARE(s, 5);
          ceilArea += (long long)e.x * e.y;
        } else if (c == layout::FaceWall) {
          QVERIFY(s < 4);
          ++walls;
          // Walls stand on the floor slab and stop under the ceiling slab.
          QCOMPARE(b.mn.z, 0);
          QCOMPARE(b.mx.z, 128);
        }
      }
    }
    // One floor and one ceiling slab, running under and over the walls.
    const long long outer = (256LL + 2 * d.config.wall) * (256LL + 2 * d.config.wall);
    QCOMPARE(floorArea, outer);
    QCOMPARE(ceilArea, outer);
    QVERIFY(walls >= 4);
    QCOMPARE(int(boxes.size()), 6);
  }

  // A 10x4x10 room, walls 1 thick, doorway (x 4..6, height 3) in the front wall (z = 0).
  static std::vector<std::pair<quint32, ConvexBrush>> roomWithDoor() {
    return {
      {1, boxAt({-1, -1, -1}, {11, 0, 11})}, // floor
      {2, boxAt({-1, 4, -1}, {11, 5, 11})},  // ceiling
      {3, boxAt({-1, 0, 10}, {11, 4, 11})},  // back
      {4, boxAt({-1, 0, 0}, {0, 4, 10})},    // left
      {5, boxAt({10, 0, 0}, {11, 4, 10})},   // right
      {6, boxAt({-1, 0, -1}, {4, 4, 0})},    // front, left of the door
      {7, boxAt({6, 0, -1}, {11, 4, 0})},    // front, right of the door
      {8, boxAt({4, 3, -1}, {6, 4, 0})},     // lintel
    };
  }

  static int nodeAt(const FaceGraph &g, quint32 obj, const glm::dvec3 &n) {
    for (int i = 0; i < int(g.nodes.size()); ++i)
      if (g.nodes[size_t(i)].objId == obj && glm::dot(g.nodes[size_t(i)].normal, n) > 0.99)
        return i;
    return -1;
  }

  void faceGraphHiddenAndLinks() {
    const auto g = FaceGraph::build(FaceGraph::solidsFromBrushes({{1, boxAt({0, 0, 0}, {2, 2, 2})},
                                                                  {2, boxAt({2, 0, 0}, {4, 2, 2})}}));
    QVERIFY(g.nodes[size_t(nodeAt(g, 1, {1, 0, 0}))].hidden);
    QVERIFY(g.nodes[size_t(nodeAt(g, 2, {-1, 0, 0}))].hidden);
    QVERIFY(!g.nodes[size_t(nodeAt(g, 1, {0, 1, 0}))].hidden);
    const int topA = nodeAt(g, 1, {0, 1, 0}), topB = nodeAt(g, 2, {0, 1, 0});
    bool flat = false;
    for (const auto &l: g.links[size_t(topA)])
      flat |= l.other == topB && l.kind == LinkKind::Flat;
    QVERIFY(flat);
    const int sideA = nodeAt(g, 1, {0, 0, 1});
    bool convex = false;
    for (const auto &l: g.links[size_t(topA)])
      convex |= l.other == sideA && l.kind == LinkKind::Convex;
    QVERIFY(convex);
  }

  // Without links (the Godot export): the same hidden faces, no adjacency.
  void faceGraphHiddenWithoutLinks() {
    const auto solids = FaceGraph::solidsFromBrushes(roomWithDoor());
    const auto full = FaceGraph::build(solids);
    const auto bare = FaceGraph::build(solids, false);
    QVERIFY(bare.links.empty());
    QCOMPARE(bare.nodes.size(), full.nodes.size());
    int hidden = 0;
    for (size_t i = 0; i < full.nodes.size(); ++i) {
      QCOMPARE(bare.nodes[i].hidden, full.nodes[i].hidden);
      hidden += full.nodes[i].hidden ? 1 : 0;
    }
    QVERIFY(hidden > 0);
  }

  void autoSelectIndoorWallsStopAtDoor() {
    const auto g = FaceGraph::build(FaceGraph::solidsFromBrushes(roomWithDoor()));
    sel::ExposureCache cache;
    sel::Query q;
    q.intent = sel::Intent::Wall;
    q.exposure = sel::Exposure::Indoor;
    q.seeds = {nodeAt(g, 3, {0, 0, -1})}; // inside of the back wall
    QVERIFY(q.seeds[0] >= 0);
    const auto r = sel::select(g, q, cache);
    std::set<int> got(r.faces.begin(), r.faces.end());
    const std::set<int> want {nodeAt(g, 3, {0, 0, -1}), nodeAt(g, 4, {1, 0, 0}),  nodeAt(g, 5, {-1, 0, 0}),
                              nodeAt(g, 6, {0, 0, 1}),  nodeAt(g, 7, {0, 0, 1}),  nodeAt(g, 8, {0, 0, 1})};
    QCOMPARE(got, want);

    // Fenced to the left half, the right wall drops out.
    q.hasRegion = true;
    q.regionMin = {-2, -2, -2};
    q.regionMax = {5, 6, 12};
    const auto fenced = sel::select(g, q, cache);
    QVERIFY(std::find(fenced.faces.begin(), fenced.faces.end(), nodeAt(g, 5, {-1, 0, 0})) == fenced.faces.end());
    QVERIFY(std::find(fenced.faces.begin(), fenced.faces.end(), nodeAt(g, 4, {1, 0, 0})) != fenced.faces.end());
  }

  void autoSelectOutdoorWallsStayOutside() {
    const auto g = FaceGraph::build(FaceGraph::solidsFromBrushes(roomWithDoor()));
    sel::ExposureCache cache;
    sel::Query q;
    q.intent = sel::Intent::Wall;
    q.exposure = sel::Exposure::Outdoor;
    q.seeds = {nodeAt(g, 3, {0, 0, 1})}; // outside of the back wall
    const auto r = sel::select(g, q, cache);
    QVERIFY(!r.faces.empty());
    for (const int i: r.faces) {
      const auto &n = g.nodes[size_t(i)];
      // Nothing inside the room's footprint.
      const bool inside = n.centroid.x > 0.0 && n.centroid.x < 10.0 && n.centroid.z > 0.0 && n.centroid.z < 10.0;
      QVERIFY2(!inside, qPrintable(QStringLiteral("face of %1 inside").arg(n.objId)));
    }
    QVERIFY(std::find(r.faces.begin(), r.faces.end(), nodeAt(g, 4, {-1, 0, 0})) != r.faces.end());
    QVERIFY(std::find(r.faces.begin(), r.faces.end(), nodeAt(g, 8, {0, 0, -1})) != r.faces.end());
  }

  void autoSelectFloorStopsAtStep() {
    const auto g = FaceGraph::build(FaceGraph::solidsFromBrushes({{1, boxAt({0, -1, 0}, {4, 0, 4})},
                                                                  {2, boxAt({4, -1, 0}, {8, 0, 4})},
                                                                  {3, boxAt({8, -1, 0}, {12, 0.5, 4})}}));
    sel::ExposureCache cache;
    sel::Query q;
    q.intent = sel::Intent::Floor;
    q.exposure = sel::Exposure::Outdoor;
    q.seeds = {nodeAt(g, 1, {0, 1, 0})};
    const auto r = sel::select(g, q, cache);
    const std::set<int> got(r.faces.begin(), r.faces.end());
    QCOMPARE(got, (std::set<int> {nodeAt(g, 1, {0, 1, 0}), nodeAt(g, 2, {0, 1, 0})}));
  }

  void autoSelectSameMaterial() {
    auto solids = FaceGraph::solidsFromBrushes({{1, boxAt({0, -1, 0}, {4, 0, 4})},
                                                {2, boxAt({4, -1, 0}, {8, 0, 4})},
                                                {3, boxAt({8, -1, 0}, {12, 0, 4})}});
    for (auto &s: solids)
      for (auto &f: s.faces)
        f.material = s.objId == 2 ? QStringLiteral("tile") : QStringLiteral("concrete");
    const auto g = FaceGraph::build(solids);
    sel::ExposureCache cache;
    sel::Query q;
    q.intent = sel::Intent::Floor;
    q.exposure = sel::Exposure::Outdoor;
    q.seeds = {nodeAt(g, 1, {0, 1, 0})};
    QCOMPARE(int(sel::select(g, q, cache).faces.size()), 3);
    q.sameMaterial = true;
    QCOMPARE(int(sel::select(g, q, cache).faces.size()), 1);
    q.contiguous = false;
    QCOMPARE(int(sel::select(g, q, cache).faces.size()), 2);
  }

  void autoSelectByGroup() {
    auto brushes = roomWithDoor();
    for (auto &[id, b]: brushes)
      for (auto &f: b.faces)
        if (f.normal.y > 0.5 && id == 1)
          f.groups = {facegroup::kFloor};
    const auto g = FaceGraph::build(FaceGraph::solidsFromBrushes(brushes));
    sel::ExposureCache cache;
    sel::Query q;
    q.intent = sel::Intent::Group;
    q.groupId = facegroup::kFloor;
    q.seeds = {nodeAt(g, 1, {0, 1, 0})};
    const auto r = sel::select(g, q, cache);
    QCOMPARE(int(r.faces.size()), 1);
  }

  // brush auto-select (Object mode)

  static std::set<quint32> brushSet(const sel::BrushResult &r) { return {r.objects.begin(), r.objects.end()}; }

  void brushSelectFloorNotWalls() {
    const auto g = FaceGraph::build(FaceGraph::solidsFromBrushes(roomWithDoor()));
    sel::ExposureCache cache;
    sel::BrushQuery q;
    q.intent = sel::BrushIntent::Floor;
    q.exposure = sel::Exposure::Indoor;
    q.seeds = {1};
    QCOMPARE(brushSet(sel::selectBrushes(g, q, cache)), (std::set<quint32> {1}));
    // A wall brush picked for a floor finds nothing: its top is not a main face.
    q.seeds = {3};
    QVERIFY(sel::selectBrushes(g, q, cache).empty());
  }

  void brushSelectWallsIndoorOutdoor() {
    const auto g = FaceGraph::build(FaceGraph::solidsFromBrushes(roomWithDoor()));
    sel::ExposureCache cache;
    sel::BrushQuery q;
    q.intent = sel::BrushIntent::Wall;
    q.exposure = sel::Exposure::Indoor;
    q.seeds = {3};
    const std::set<quint32> walls {3, 4, 5, 6, 7, 8};
    QCOMPARE(brushSet(sel::selectBrushes(g, q, cache)), walls);
    // From outside: the same walls, never the floor / ceiling slabs (their outer edges are not main faces).
    q.exposure = sel::Exposure::Outdoor;
    QCOMPARE(brushSet(sel::selectBrushes(g, q, cache)), walls);
  }

  void brushSelectFacadeSide() {
    const auto g = FaceGraph::build(FaceGraph::solidsFromBrushes(roomWithDoor()));
    sel::ExposureCache cache;
    sel::BrushQuery q;
    q.intent = sel::BrushIntent::Facade;
    q.seeds = {3};
    const auto got = brushSet(sel::selectBrushes(g, q, cache));
    QVERIFY(got.count(3));
    QVERIFY(!got.count(5)); // the right wall is another side
  }

  void brushSelectSameSize() {
    const auto g = FaceGraph::build(FaceGraph::solidsFromBrushes({{1, boxAt({0, 0, 0}, {2, 1, 4})},
                                                                  {2, boxAt({10, 0, 10}, {14, 1, 12})}, // turned
                                                                  {3, boxAt({20, 0, 0}, {22, 2, 4})},   // taller
                                                                  {4, boxAt({2, 0, 0}, {5, 1, 4})}}));  // touches 1, wider
    sel::ExposureCache cache;
    sel::BrushQuery q;
    q.intent = sel::BrushIntent::SameSize;
    q.seeds = {1};
    q.contiguous = false;
    QCOMPARE(brushSet(sel::selectBrushes(g, q, cache)), (std::set<quint32> {1, 2}));
    q.contiguous = true;
    QCOMPARE(brushSet(sel::selectBrushes(g, q, cache)), (std::set<quint32> {1}));
  }

  void brushSelectSameMaterial() {
    auto solids = FaceGraph::solidsFromBrushes({{1, boxAt({0, 0, 0}, {4, 1, 4})},
                                                {2, boxAt({4, 0, 0}, {8, 1, 4})},
                                                {3, boxAt({8, 0, 0}, {12, 1, 4})}});
    for (auto &s: solids)
      for (size_t i = 0; i < s.faces.size(); ++i)
        // Brush 2 is mostly tile with one concrete face: tile is its material.
        s.faces[i].material = s.objId == 2 && i != 0 ? QStringLiteral("tile") : QStringLiteral("concrete");
    const auto g = FaceGraph::build(solids);
    sel::ExposureCache cache;
    sel::BrushQuery q;
    q.intent = sel::BrushIntent::SameMaterial;
    q.seeds = {1};
    QCOMPARE(brushSet(sel::selectBrushes(g, q, cache)), (std::set<quint32> {1}));
    q.contiguous = false;
    QCOMPARE(brushSet(sel::selectBrushes(g, q, cache)), (std::set<quint32> {1, 3}));
  }

  void brushSelectTouchingStopsAtGap() {
    const auto g = FaceGraph::build(FaceGraph::solidsFromBrushes({{1, boxAt({0, 0, 0}, {2, 2, 2})},
                                                                  {2, boxAt({2, 0, 0}, {4, 2, 2})},
                                                                  {3, boxAt({4, 0, 0}, {6, 2, 2})},
                                                                  {4, boxAt({7, 0, 0}, {9, 2, 2})}}));
    QVERIFY(g.solidsTouch(0, 1));
    QVERIFY(!g.solidsTouch(2, 3));
    sel::ExposureCache cache;
    sel::BrushQuery q;
    q.intent = sel::BrushIntent::Touching;
    q.seeds = {1};
    QCOMPARE(brushSet(sel::selectBrushes(g, q, cache)), (std::set<quint32> {1, 2, 3}));
  }

  static stairs::Params stairBox(double hSu, double dSu, double wSu = 64) {
    stairs::Params p;
    p.mn = glm::dvec3(0.0);
    p.mx = glm::dvec3(wSu / 10.0, hSu / 10.0, dSu / 10.0); // climbs +Z
    p.direction = 2;
    return p;
  }

  void stairsCountFromStepSize() {
    stairs::Params p = stairBox(16, 16);
    p.stepHeight = 8;
    QCOMPARE(stairs::stepCount(p), 2);
    p = stairBox(128, 300);
    p.stepHeight = 16;
    QCOMPARE(stairs::stepCount(p), 8);
    p.sizing = stairs::Sizing::Count;
    p.steps = 5;
    QCOMPARE(stairs::stepCount(p), 5);
  }

  void stairsUndersides() {
    stairs::Params p = stairBox(64, 64);
    p.stepHeight = 16;
    const double box = 6.4 * 6.4 * 6.4;
    // Solid: a full staircase, half the box plus half a step row.
    p.underside = stairs::Underside::Solid;
    auto solid = stairs::build(p, MaterialPtr(), 1.0);
    QCOMPARE(int(solid.size()), 4);
    double v = 0;
    for (const auto &b: solid)
      v += b.volume();
    QVERIFY(std::fabs(v - box * (1 + 2 + 3 + 4) / 16.0) < 1e-6);
    // Planks: one step-high slab per step.
    p.underside = stairs::Underside::Planks;
    auto planks = stairs::build(p, MaterialPtr(), 1.0);
    QCOMPARE(int(planks.size()), 4);
    for (const auto &b: planks)
      QVERIFY(std::fabs(b.volume() - box / 16.0) < 1e-6);
    // Slope: each step has a sloped bottom face, all on one plane.
    p.underside = stairs::Underside::Slope;
    p.thickness = 16;
    auto slope = stairs::build(p, MaterialPtr(), 1.0);
    QCOMPARE(int(slope.size()), 4);
    std::vector<std::pair<glm::dvec3, double>> sloped;
    for (const auto &b: slope)
      for (const auto &f: b.faces)
        if (f.normal.y < -0.1 && f.normal.y > -0.99)
          sloped.emplace_back(f.normal, f.distance);
    QVERIFY(sloped.size() >= 3);
    for (const auto &[n, d]: sloped) {
      QVERIFY(glm::length(n - sloped.front().first) < 1e-6);
      QVERIFY(std::fabs(d - sloped.front().second) < 1e-6);
    }
  }

  void stairsFaceGroups() {
    stairs::Params p = stairBox(32, 32);
    p.stepHeight = 16;
    for (const auto &b: stairs::build(p, MaterialPtr(), 1.0))
      for (const auto &f: b.faces) {
        QCOMPARE(int(f.groups.size()), 1);
        const quint32 g = f.groups.front();
        // Tops and bottoms are flat, fronts and backs are walls.
        if (std::fabs(f.normal.y) > 0.7)
          QCOMPARE(g, facegroup::kStairFlat);
        else if (std::fabs(f.normal.z) > 0.7)
          QCOMPARE(g, facegroup::kStairWall);
        else if (std::fabs(f.normal.x) > 0.7)
          QCOMPARE(g, facegroup::kStairSide);
        else
          QCOMPARE(g, facegroup::kStairUnder);
      }
    // Planks have bottoms and backs: flat and wall too.
    p.underside = stairs::Underside::Planks;
    bool bottomFlat = false, backWall = false;
    for (const auto &b: stairs::build(p, MaterialPtr(), 1.0))
      for (const auto &f: b.faces) {
        bottomFlat |= f.normal.y < -0.7 && f.groups.front() == facegroup::kStairFlat;
        backWall |= f.normal.z > 0.7 && f.groups.front() == facegroup::kStairWall;
      }
    QVERIFY(bottomFlat);
    QVERIFY(backWall);
    const auto back = stairs::fromJson(stairs::toJson(p));
    QCOMPARE(back.stepHeight, p.stepHeight);
    QVERIFY(glm::length(back.mx - p.mx) < 1e-9);
  }

  void stairsSides() {
    stairs::Params p = stairBox(64, 64);  // climbs +Z, width along X
    p.stepHeight = 16;
    p.sides = true;
    p.sideWidth = 8;   // 0.8 world units
    p.sideHeight = 8;
    for (const auto u: {stairs::Underside::Solid, stairs::Underside::Planks, stairs::Underside::Slope}) {
      p.underside = u;
      const auto brushes = stairs::build(p, MaterialPtr(), 1.0);
      int sides = 0;
      for (const auto &b: brushes) {
        glm::dvec3 mn, mx;
        b.bounds(mn, mx);
        const bool side = b.faces.front().groups.front() == facegroup::kStairStringer;
        for (const auto &f: b.faces)
          QCOMPARE(f.groups.front() == facegroup::kStairStringer, side);
        if (side) {
          ++sides;
          QVERIFY(std::fabs((mx.x - mn.x) - 0.8) < 1e-6);
          QVERIFY(mx.y > 6.4 - 1e-6);  // rises above the top step
          QVERIFY(b.valid());
        } else {
          // Steps stay between the sides.
          QVERIFY(mn.x > p.mn.x + 0.8 - 1e-6);
          QVERIFY(mx.x < p.mx.x - 0.8 + 1e-6);
        }
      }
      QCOMPARE(sides, 2);
    }
    const auto back = stairs::fromJson(stairs::toJson(p));
    QVERIFY(back.sides);
    QCOMPARE(back.sideWidth, 8.0);
  }

  void stairsStepTexture() {
    stairs::Params p = stairBox(64, 64);
    p.stepHeight = 16;
    stairs::StepTex t;
    t.on = true;
    t.tile = 64;  // 6.4 world units per texture width
    // A tread top 1 x 1.6 at height 3.2, centred at (1, 3.2, 2).
    const std::vector<glm::dvec3> poly {{0.5, 3.2, 1.2}, {1.5, 3.2, 1.2}, {1.5, 3.2, 2.8}, {0.5, 3.2, 2.8}};
    const glm::dvec3 n(0, 1, 0), c(1, 3.2, 2);
    auto b = stairs::stepTexBasis(p, n, poly, t, 512, 256);
    // The face centre shows the texture centre.
    QVERIFY(glm::length(b.uvAt(glm::vec3(c)) - glm::vec2(0.5f)) < 1e-4f);
    // Seen from outside: cross(u, v) == -n.
    QVERIFY(glm::length(glm::cross(glm::dvec3(b.uAxis), glm::dvec3(b.vAxis)) + n) < 1e-5);
    // V follows the aspect: a 512 x 256 texture covers half as much along V.
    QVERIFY(std::fabs(b.tilesPerUnitV - 2.0f * b.tilesPerUnitU) < 1e-5f);
    // Another tile size keeps the centre where it is (grows from the centre).
    t.tile = 32;
    b = stairs::stepTexBasis(p, n, poly, t, 512, 256);
    QVERIFY(glm::length(b.uvAt(glm::vec3(c)) - glm::vec2(0.5f)) < 1e-4f);
    // An offset of 128 texels puts texel 256 + 128 on the centre.
    t.offsetU = 128;
    b = stairs::stepTexBasis(p, n, poly, t, 512, 256);
    QVERIFY(std::fabs(b.uvAt(glm::vec3(c)).x - 0.75f) < 1e-4f);
    // A quarter turn swaps the axes.
    t.offsetU = 0;
    const auto b0 = stairs::stepTexBasis(p, n, poly, t, 512, 256);
    t.rotation = 1;
    const auto b1 = stairs::stepTexBasis(p, n, poly, t, 512, 256);
    QVERIFY(std::fabs(glm::dot(b0.uAxis, b1.uAxis)) < 1e-5f);
    QVERIFY(std::fabs(std::fabs(glm::dot(b0.uAxis, b1.vAxis)) - 1.0f) < 1e-5f);
    // Stored with the stairs.
    QJsonObject params;
    stairs::setStepTex(params, stairs::Part::Wall, t);
    QVERIFY(stairs::stepTexOf(params, stairs::Part::Wall) == t);
    QVERIFY(!stairs::stepTexOf(params, stairs::Part::Flat).on);
  }

  void stairsOvershootAndThinPlanks() {
    stairs::Params p = stairBox(32, 32);
    p.stepHeight = 8; // 4 steps of 8 x 8
    p.underside = stairs::Underside::Planks;
    p.nosing = true;
    p.nosingDepth = 2;
    p.nosingHeight = 1;
    const auto withNosing = stairs::build(p, MaterialPtr(), 1.0);
    QCOMPARE(int(withNosing.size()), 8);
    int nosings = 0;
    for (const auto &b: withNosing) {
      glm::dvec3 mn, mx;
      b.bounds(mn, mx);
      if (std::fabs((mx.z - mn.z) - 0.2) < 1e-6) {
        ++nosings;
        QVERIFY(std::fabs((mx.y - mn.y) - 0.1) < 1e-6);
      }
    }
    QCOMPARE(nosings, 4);

    p.nosing = false;
    p.thinPlanks = true;
    p.plankThickness = 2;
    for (const auto &b: stairs::build(p, MaterialPtr(), 1.0)) {
      glm::dvec3 mn, mx;
      b.bounds(mn, mx);
      QVERIFY(std::fabs((mx.y - mn.y) - 0.2) < 1e-6);
    }
    const auto back = stairs::fromJson(stairs::toJson(p));
    QVERIFY(back.thinPlanks);
    QCOMPARE(back.plankThickness, 2.0);
  }

  void stairsCollisionRamp() {
    auto inside = [](const ConvexBrush &b, const glm::dvec3 &q) {
      for (const auto &f: b.faces)
        if (f.signedDistance(q) > 1e-6)
          return false;
      return true;
    };
    stairs::Params p = stairBox(32, 32); // 4 steps of 8 x 8, climbs +Z
    p.stepHeight = 8;
    for (const auto u: {stairs::Underside::Solid, stairs::Underside::Planks, stairs::Underside::Slope}) {
      p.underside = u;
      const auto ramp = stairs::buildCollision(p, MaterialPtr(), 1.0);
      QVERIFY(ramp.has_value());
      glm::dvec3 mn, mx;
      ramp->bounds(mn, mx);
      QVERIFY(mn.y > -1e-6 && mx.y < 3.2 + 1e-6 && mx.z < 3.2 + 1e-6);
      // It meets the floor one step in front of the box.
      QVERIFY(std::fabs(mn.z + 0.8) < 1e-6);
      QVERIFY(inside(*ramp, glm::dvec3(3.2, 0.01, -0.75)));
      // Every step corner is under (or on) the ramp's top.
      for (const auto &b: stairs::build(p, MaterialPtr(), 1.0))
        for (const auto &poly: b.polygons)
          for (const auto &q: poly)
            QVERIFY(inside(*ramp, q));
      // The front of the first step is the ramp's lowest top edge.
      QVERIFY(inside(*ramp, glm::dvec3(3.2, 0.8, 0.0)));
      QVERIFY(!inside(*ramp, glm::dvec3(3.2, 0.9, 0.0)));
      // Solid stairs ramp all the way down; open undersides keep the high end free.
      QCOMPARE(inside(*ramp, glm::dvec3(3.2, 0.1, 3.1)), u == stairs::Underside::Solid);
    }
    p.collision = false;
    QVERIFY(!stairs::buildCollision(p, MaterialPtr(), 1.0).has_value());
    QVERIFY(!stairs::fromJson(stairs::toJson(p)).collision);
    QVERIFY(stairs::fromJson(QJsonObject {}).collision);
  }

  void stairsTransforms() {
    // A quarter turn about up bakes into a new box and direction.
    stairs::Params p = stairBox(64, 128, 32); // 3.2 wide (x), 6.4 high, 12.8 long (z), climbs +Z
    const glm::dmat4 quarter = glm::rotate(glm::dmat4(1.0), glm::radians(90.0), glm::dvec3(0, 1, 0));
    glm::dmat4 q = quarter;
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        q[c][r] = std::round(q[c][r]);
    stairs::applyTransform(p, q);
    QVERIFY(stairs::isIdentity(p.xform));
    QCOMPARE(p.direction, 0); // +Z turned 90 degrees about +Y climbs +X
    QVERIFY(std::fabs((p.mx.x - p.mn.x) - 12.8) < 1e-9);
    QVERIFY(std::fabs((p.mx.z - p.mn.z) - 3.2) < 1e-9);
    // A mirror along the climb flips the direction.
    glm::dmat4 mirror(1.0);
    mirror[0][0] = -1.0;
    stairs::applyTransform(p, mirror);
    QCOMPARE(p.direction, 1);
    // Any other angle is kept as a placement and survives JSON.
    stairs::applyTransform(p, glm::rotate(glm::dmat4(1.0), glm::radians(30.0), glm::dvec3(0, 1, 0)));
    QVERIFY(!stairs::isIdentity(p.xform));
    const auto back = stairs::fromJson(stairs::toJson(p));
    QVERIFY(!stairs::isIdentity(back.xform));
    QVERIFY(std::fabs(back.xform[0][2] - p.xform[0][2]) < 1e-12);
    const auto brushes = stairs::build(p, MaterialPtr(), 1.0);
    QVERIFY(!brushes.empty());
    for (const auto &b: brushes)
      QVERIFY(b.valid());
  }

  void fmapProcObjectsRoundTrip() {
    fmap::Document doc;
    fmap::NodeRecord n;
    n.id = 5;
    n.procObject = 3;
    doc.nodes.push_back(n);
    doc.procObjects.push_back({3, QStringLiteral("stairs"), QStringLiteral("Stairs 1"), QByteArray("{\"steps\":4}")});
    const QString path = QDir::temp().filePath(QStringLiteral("procobjects_test.ghmap"));
    QString error;
    QVERIFY2(fmap::saveMap(path, doc, &error), qPrintable(error));
    fmap::Document loaded;
    QVERIFY2(fmap::loadMap(path, loaded, &error), qPrintable(error));
    QFile::remove(path);
    QCOMPARE(int(loaded.procObjects.size()), 1);
    QCOMPARE(loaded.procObjects[0].type, QStringLiteral("stairs"));
    QCOMPARE(loaded.procObjects[0].params, QByteArray("{\"steps\":4}"));
    QCOMPARE(loaded.nodes[0].procObject, 3u);
  }

  void ioSnakeCase() {
    QCOMPARE(io::snake("OnTrigger"), QStringLiteral("on_trigger"));
    QCOMPARE(io::snake("FireUser1"), QStringLiteral("fire_user1"));
    QCOMPARE(io::snake("startDisabled"), QStringLiteral("start_disabled"));
    QCOMPARE(io::snake("OnCase01"), QStringLiteral("on_case01"));
    QCOMPARE(io::snake("NearestEntityDistance"), QStringLiteral("nearest_entity_distance"));
    QCOMPARE(io::snake("useLOS"), QStringLiteral("use_los"));
    QCOMPARE(io::snake("LOSValue"), QStringLiteral("los_value"));
    QCOMPARE(io::godotMethod("SetValueTest"), QStringLiteral("input_set_value_test"));
    QCOMPARE(io::godotGroup("Door_A"), QStringLiteral("io_door_a"));
    QCOMPARE(io::godotGroup("@hud"), QStringLiteral("hud"));
  }

  void ioParseParams() {
    QVariant v;
    QVERIFY(io::parseParam(io::ParamType::Bool, "yes", &v) && v.toBool());
    QVERIFY(io::parseParam(io::ParamType::Bool, "0", &v) && !v.toBool());
    QVERIFY(!io::parseParam(io::ParamType::Bool, "maybe"));
    QVERIFY(io::parseParam(io::ParamType::Int, "1.000", &v) && v.toInt() == 1);
    QVERIFY(!io::parseParam(io::ParamType::Float, "abc"));
    QVERIFY(io::parseParam(io::ParamType::Vec3, "1 2,3", &v) && v.toList().size() == 3);
    QVERIFY(!io::parseParam(io::ParamType::Vec3, "1 2"));
    QVERIFY(io::parseParam(io::ParamType::Color, "255 0 0", &v) && v.value<QColor>() == QColor(255, 0, 0));
    QVERIFY(io::parseParam(io::ParamType::Color, "#00ff00", &v) && v.value<QColor>() == QColor(0, 255, 0));
    QVERIFY(!io::parseParam(io::ParamType::Color, "300 0 0"));
    QVERIFY(io::parseParam(io::ParamType::Float, "", &v)); // <none> is always fine
  }

  void ioResolveTargets() {
    const std::vector<Entity> ents = {ent(1, "logic_relay", "Relay_Door"), ent(2, "func_door", "door_a"),
                                      ent(3, "func_door", "door_b"), ent(4, "func_detail", "detail"),
                                      ent(5, "light", "")};
    using K = io::TargetKind;
    QCOMPARE(io::resolve(ents, "RELAY_DOOR", 0).ids.size(), size_t(1)); // case-insensitive
    QCOMPARE(io::resolve(ents, "door_*", 0).ids.size(), size_t(2));
    QCOMPARE(io::resolve(ents, "func_door", 0).ids.size(), size_t(2)); // classname
    QCOMPARE(io::resolve(ents, "light", 0).ids.size(), size_t(1));
    QCOMPARE(io::resolve(ents, "detail", 0).kind, K::Unresolved); // func_detail has no I/O
    QCOMPARE(io::resolve(ents, "nothing", 0).kind, K::Unresolved);
    const auto self = io::resolve(ents, "!self", kEntityIdBase + 2);
    QVERIFY(self.kind == K::Resolved && self.ids.size() == 1 && self.ids[0] == kEntityIdBase + 2);
    QCOMPARE(io::resolve(ents, "!activator", 0).kind, K::Runtime);
    QCOMPARE(io::resolve(ents, "!player", 0).kind, K::Runtime);
    QCOMPARE(io::resolve(ents, "@hud", 0).kind, K::External);
    // Inputs offered for a target: the union over the classes it resolves to.
    const auto defs = io::inputsForTarget(ents, "door_*", 0);
    QVERIFY(std::any_of(defs.begin(), defs.end(), [](const io::IODef &d) { return d.name == "Open"; }));
    QVERIFY(std::none_of(defs.begin(), defs.end(), [](const io::IODef &d) { return d.name == "Trigger"; }));
  }

  void ioValidateConnections() {
    std::vector<Entity> ents = {
        ent(1, "logic_relay", "relay",
            {conn("OnTrigger", "door", "Open"), conn("OnFoo", "door", "Open"), conn("OnTrigger", "missing", "Open"),
             conn("OnTrigger", "door", "Explode"), conn("OnTrigger", "door", "SetSpeed", "fast"),
             conn("OnTrigger", "door", "Open", "x"), conn("OnTrigger", "door", "Open", "", -1.0f),
             conn("OnTrigger", "door", "Open", "", 0.0f, 0), conn("OnTrigger", "@hud", "Anything")}),
        ent(2, "func_door", "door")};
    QVERIFY(io::validateConnection(ents, ents[0], 0).empty());
    QVERIFY(hasIssue(io::validateConnection(ents, ents[0], 1), "no output"));
    QVERIFY(hasIssue(io::validateConnection(ents, ents[0], 2), "matches no entity"));
    QVERIFY(hasIssue(io::validateConnection(ents, ents[0], 3), "has an input"));
    QVERIFY(hasIssue(io::validateConnection(ents, ents[0], 4), "not a valid float"));
    QVERIFY(hasIssue(io::validateConnection(ents, ents[0], 5), "takes no parameter"));
    QVERIFY(hasIssue(io::validateConnection(ents, ents[0], 6), "negative delay"));
    QVERIFY(hasIssue(io::validateConnection(ents, ents[0], 7), "never fires"));
    QVERIFY(io::validateConnection(ents, ents[0], 8).empty()); // external: not checked
    // A disabled connection is not validated.
    ents[0].outputs[1].disabled = true;
    QVERIFY(io::validateConnection(ents, ents[0], 1).empty());
  }

  void ioZeroDelayLoops() {
    // a.OnTrigger -> b.Trigger, b.OnTrigger -> a.Trigger: a loop.
    std::vector<Entity> loop = {ent(1, "logic_relay", "a", {conn("OnTrigger", "b", "Trigger")}),
                                ent(2, "logic_relay", "b", {conn("OnTrigger", "a", "Trigger")})};
    QVERIFY(hasIssue(io::validate(loop), "zero-delay loop"));
    // Any delay breaks it.
    loop[1].outputs[0].delay = 0.5f;
    QVERIFY(!hasIssue(io::validate(loop), "zero-delay loop"));
    // An entity configuring itself is not a loop (SetEnergy fires nothing).
    std::vector<Entity> self = {ent(1, "light", "lamp", {conn("OnUser1", "!self", "SetEnergy", "2")})};
    QVERIFY(!hasIssue(io::validate(self), "zero-delay loop"));
    // FireUser1 -> OnUser1 -> FireUser1 on itself is.
    std::vector<Entity> user = {ent(1, "light", "lamp", {conn("OnUser1", "!self", "FireUser1")})};
    QVERIFY(hasIssue(io::validate(user), "zero-delay loop"));
  }

  void ioRenameAndReferences() {
    std::vector<Entity> ents = {ent(1, "logic_relay", "relay", {conn("OnTrigger", "Door", "Open"), conn("OnTrigger", "door*", "Close")}),
                                ent(2, "func_door", "door"),
                                ent(3, "path_node", "n1", {}, {{"next", "door"}})};
    QCOMPARE(io::countNameReferences(ents, "door"), 2); // exact: connection + Target prop, not the wildcard
    QCOMPARE(int(io::incoming(ents, kEntityIdBase + 2).size()), 2); // both connections reach it
    QVERIFY(io::renameReferences(ents[0], "door", "gate"));
    QCOMPARE(ents[0].outputs[0].target, QStringLiteral("gate"));
    QCOMPARE(ents[0].outputs[1].target, QStringLiteral("door*"));
    QVERIFY(io::renameReferences(ents[2], "door", "gate"));
    QCOMPARE(ents[2].props.value("next").toString(), QStringLiteral("gate"));
  }

  void ioAddOutputSyntax() {
    io::Connection c;
    QVERIFY(io::parseAddOutput("OnTrigger door:Open:fast:1.5:2", &c));
    QCOMPARE(c.output, QStringLiteral("OnTrigger"));
    QCOMPARE(c.target, QStringLiteral("door"));
    QCOMPARE(c.input, QStringLiteral("Open"));
    QCOMPARE(c.param, QStringLiteral("fast"));
    QCOMPARE(c.delay, 1.5f);
    QCOMPARE(c.times, 2);
    QVERIFY(io::parseAddOutput("OnTrigger door", &c));
    QCOMPARE(c.input, QStringLiteral("Use"));
    QCOMPARE(c.times, -1);
    QVERIFY(io::parseAddOutput("OnTrigger door:Open::0:0", &c) && c.times == -1); // 0 = infinite
    QVERIFY(!io::parseAddOutput("nonsense", &c));
  }

  void fmapConnectionsRoundTrip() {
    fmap::Document doc;
    fmap::EntityConnectionsRecord rec;
    rec.entityId = kEntityIdBase + 7;
    fmap::ConnectionRecord c;
    c.output = "OnTrigger";
    c.target = "door_*";
    c.input = "Open";
    c.param = "a \"quoted\" param";
    c.delay = 2.5f;
    c.times = 3;
    c.flags = fmap::kConnectionDisabled;
    rec.connections = {c, c};
    rec.connections[1].times = -1;
    rec.connections[1].flags = 0;
    doc.connections.push_back(rec);
    const QString path = QDir::temp().filePath(QStringLiteral("connections_test.ghmap"));
    QString error;
    QVERIFY2(fmap::saveMap(path, doc, &error), qPrintable(error));
    fmap::Document loaded;
    QVERIFY2(fmap::loadMap(path, loaded, &error), qPrintable(error));
    QFile::remove(path);
    QCOMPARE(int(loaded.connections.size()), 1);
    QCOMPARE(loaded.connections[0].entityId, kEntityIdBase + 7);
    QCOMPARE(int(loaded.connections[0].connections.size()), 2);
    const auto &l = loaded.connections[0].connections[0];
    QCOMPARE(l.target, QStringLiteral("door_*"));
    QCOMPARE(l.param, QStringLiteral("a \"quoted\" param"));
    QCOMPARE(l.delay, 2.5f);
    QCOMPARE(l.times, 3);
    QCOMPARE(l.flags, fmap::kConnectionDisabled);
    QCOMPARE(loaded.connections[0].connections[1].times, -1);
  }

  void ioSimAutoRelayDelay() {
    // logic_auto -> relay.Trigger; relay -> counter.Add(1) after 1 s.
    io::Simulator sim({ent(1, "logic_auto", "auto", {conn("OnMapSpawn", "relay", "Trigger")}),
                       ent(2, "logic_relay", "relay", {conn("OnTrigger", "counter", "Add", "1", 1.0f)}),
                       ent(3, "math_counter", "counter")});
    sim.start();
    sim.advance(0.5);
    QCOMPARE(simValue(sim, 3), 0.0);
    sim.advance(0.6);
    QCOMPARE(simValue(sim, 3), 1.0);
  }

  void ioSimRelayRefireLock() {
    auto make = [&](bool fast) {
      return std::vector<Entity> {
          ent(1, "logic_relay", "relay", {conn("OnTrigger", "counter", "Add", "1", 1.0f)}, {{"fastRetrigger", fast}}),
          ent(2, "math_counter", "counter")};
    };
    io::Simulator locked(make(false));
    locked.start();
    locked.fireInput("relay", "Trigger");
    locked.advance(0.1);
    locked.fireInput("relay", "Trigger"); // its delayed output is still pending: ignored
    locked.advance(2.0);
    QCOMPARE(simValue(locked, 2), 1.0);
    io::Simulator fast(make(true));
    fast.start();
    fast.fireInput("relay", "Trigger");
    fast.advance(0.1);
    fast.fireInput("relay", "Trigger");
    fast.advance(2.0);
    QCOMPARE(simValue(fast, 2), 2.0);
  }

  void ioSimTimerOscillator() {
    io::Simulator sim({ent(1, "logic_timer", "t", {conn("OnTimerLow", "low", "Add", "1"), conn("OnTimerHigh", "high", "Add", "1")},
                           {{"oscillator", true}, {"refireTime", 1.0}}),
                       ent(2, "math_counter", "low"), ent(3, "math_counter", "high")});
    sim.start();
    sim.advance(3.05);
    QCOMPARE(simValue(sim, 2), 2.0); // Low first: t=1, t=3
    QCOMPARE(simValue(sim, 3), 1.0); // t=2
  }

  void ioSimCounterLimitsAndTimes() {
    io::Simulator sim({ent(1, "math_counter", "c", {conn("OnHitMax", "b", "SetValue", "1"), conn("OutValue", "seen", "Add", "1", 0.0f, 1)},
                           {{"max", 3.0}}),
                       ent(2, "logic_branch", "b"), ent(3, "math_counter", "seen")});
    sim.start();
    sim.fireInput("c", "Add", "2");
    sim.fireInput("c", "Add", "5");
    sim.advance(0.1);
    QCOMPARE(simValue(sim, 1), 3.0); // clamped
    QCOMPARE(simValue(sim, 2), 1.0); // OnHitMax reached the branch
    QCOMPARE(simValue(sim, 3), 1.0); // OutValue -> seen fired only once (times = 1)
  }

  void ioSimCancelPending() {
    io::Simulator sim({ent(1, "logic_relay", "relay", {conn("OnTrigger", "c", "Add", "1", 1.0f)}), ent(2, "math_counter", "c")});
    sim.start();
    sim.fireInput("relay", "Trigger");
    sim.advance(0.2);
    sim.fireInput("relay", "CancelPending");
    sim.advance(2.0);
    QCOMPARE(simValue(sim, 2), 0.0);
  }

  void ioSimCaseShuffle() {
    io::Simulator sim({ent(1, "logic_case", "pick",
                           {conn("OnCase01", "a", "Add", "1"), conn("OnCase02", "b", "Add", "1"), conn("OnCase03", "c", "Add", "1")}),
                       ent(2, "math_counter", "a"), ent(3, "math_counter", "b"), ent(4, "math_counter", "c")});
    sim.start();
    for (int i = 0; i < 3; ++i)
      sim.fireInput("pick", "PickRandomShuffle");
    sim.advance(0.1);
    QCOMPARE(simValue(sim, 2) + simValue(sim, 3) + simValue(sim, 4), 3.0);
    QCOMPARE(simValue(sim, 2), 1.0); // no repeats inside one round
    QCOMPARE(simValue(sim, 3), 1.0);
    QCOMPARE(simValue(sim, 4), 1.0);
  }

  void ioSimUserKillAddOutput() {
    io::Simulator sim({ent(1, "info_target", "t", {conn("OnUser1", "c", "Add", "1")}), ent(2, "math_counter", "c")});
    sim.start();
    sim.fireInput("t", "FireUser1");
    sim.fireInput("t", "AddOutput", "OnUser2 c:Add:10");
    sim.advance(0.1);
    sim.fireInput("t", "FireUser2");
    sim.advance(0.1);
    QCOMPARE(simValue(sim, 2), 11.0);
    sim.fireInput("t", "Kill");
    sim.advance(0.1);
    QVERIFY(sim.killed(kEntityIdBase + 1));
    sim.fireInput("t", "FireUser1");
    sim.advance(0.1);
    QCOMPARE(simValue(sim, 2), 11.0); // killed entities ignore inputs
  }

  void ioSimZeroDelayLoopIsCut() {
    io::Simulator sim({ent(1, "logic_relay", "a", {conn("OnTrigger", "b", "Trigger")}, {{"fastRetrigger", true}}),
                       ent(2, "logic_relay", "b", {conn("OnTrigger", "a", "Trigger")}, {{"fastRetrigger", true}})});
    sim.start();
    sim.fireInput("a", "Trigger");
    sim.advance(0.1, 500); // must return, not hang
    bool noted = false;
    for (const auto &l: sim.takeLog())
      noted = noted || l.text.contains(QLatin1String("zero-delay"));
    QVERIFY(noted);
  }

  void godotExportScript() {
    std::vector<Entity> ents = {ent(1, "logic_relay", "relay", {conn("OnTrigger", "door", "Open", "", 1.0f)}),
                                ent(2, "func_door", "door"), ent(3, "func_detail", "skip"),
                                ent(4, "logic_event", "ev", {}, {{"eventName", "boss_intro"}})};
    ents[0].outputs.push_back(conn("OnTrigger", "door", "Close"));
    ents[0].outputs.back().disabled = true;
    ents[1].origin = glm::vec3(10.0f, 0.0f, 0.0f);
    godot::HullMap hulls;
    hulls[ents[1].id] = {{glm::vec3(9, 0, 0), glm::vec3(11, 0, 0), glm::vec3(10, 1, 0), glm::vec3(10, 0, 1)}};
    godot::ExportOptions opt;
    opt.mapName = "test";
    int e = 0, c = 0;
    const QString gd = godot::buildScript(ents, hulls, opt, &e, &c);
    QCOMPARE(e, 3); // func_detail has no I/O: left out
    QCOMPARE(c, 1); // the disabled connection is left out
    QVERIFY(gd.contains(QStringLiteral("\"class\": \"logic_relay\"")));
    QVERIFY(gd.contains(QStringLiteral("[\"OnTrigger\", \"door\", \"Open\", \"\", 1.0, -1]")));
    QVERIFY(gd.contains(QStringLiteral("\"only_once\": false")));
    QVERIFY(gd.contains(QStringLiteral("\"origin\": Vector3(2.03125")));
    QVERIFY(gd.contains(QStringLiteral("Vector3(-0.203125, 0.0, 0.0)"))); // hull point relative to the origin, metres
    QVERIFY(gd.contains(QStringLiteral("const TARGETNAMES = [\"door\", \"ev\", \"relay\"]")));
    QVERIFY(gd.contains(QStringLiteral("const EVENTS = [\"boss_intro\"]")));
    QVERIFY(!gd.contains(QStringLiteral("skip")));
  }

  void ioClassDefinitions() {
    const EntityRegistry &r = EntityRegistry::instance();
    for (const char *n: {"logic_auto", "logic_relay", "logic_timer", "logic_branch", "logic_case", "logic_compare",
                         "logic_multicompare", "math_counter", "logic_event", "logic_playerproxy", "trigger_once",
                         "trigger_multiple", "trigger_look", "trigger_proximity"})
      QVERIFY2(r.find(QString::fromLatin1(n)), n);
    const EntityClass *relay = r.find("logic_relay");
    QVERIFY(relay->input("trigger") && relay->output("OnTrigger") && relay->input("FireUser3") && relay->output("OnKilled"));
    QVERIFY(!r.find("func_detail")->io);
    QVERIFY(r.find("trigger_once")->isBrushEntity());
    QVERIFY(!r.find("trigger_multiple")->prop("target")); // replaced by outputs
    QCOMPARE(r.find("path_node")->prop("next")->type, PropType::Target);
  }

  // texture workflow: shared material model

  void matPropsDefaultsAreGodot() {
    const MaterialProps d = MaterialProps::godotDefaults();
    QCOMPARE(d.setMask, matprops::kAllKeys);
    QCOMPARE(d.metallic, 0.0f);
    QCOMPARE(d.roughness, 1.0f);
    QCOMPARE(d.metallicSpecular, 0.5f);
    QCOMPARE(d.normalScale, 1.0f);
    QCOMPARE(d.aoLightAffect, 0.0f);
    QCOMPARE(d.emissionEnergy, 1.0f);
    QVERIFY(MaterialProps().isEmpty());
  }

  void matPropsJsonRoundTrip() {
    MaterialProps p;
    p.setValue(matprops::Key::Metallic, 0.731234);
    p.setValue(matprops::Key::RoughnessChannel, QStringLiteral("green"));
    p.setValue(matprops::Key::ShadingMode, QStringLiteral("unshaded"));
    p.setValue(matprops::Key::EmissionColor, QStringLiteral("#ff8000"));
    p.setValue(matprops::Key::Transparency, true);
    const QJsonObject j = p.toJson();
    QCOMPARE(j.size(), 5);
    QCOMPARE(j.value("metallic").toDouble(), 0.7312);
    QCOMPARE(j.value("roughness_texture_channel").toString(), QStringLiteral("green"));
    QCOMPARE(j.value("emission_color").toString(), QStringLiteral("#ff8000"));
    QStringList errors;
    const MaterialProps back = MaterialProps::fromJson(j, &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(", ")));
    QCOMPARE(back, p);
    // a second round trip is stable
    QCOMPARE(MaterialProps::fromJson(back.toJson()).toJson(), j);
  }

  void matPropsValidation() {
    QStringList errors;
    QJsonObject bad;
    bad.insert("metallic", 7.0);                 // clamped
    bad.insert("roughness", QStringLiteral("x")); // wrong type
    bad.insert("shininess", 1.0);                // unknown key
    bad.insert("ao_texture_channel", QStringLiteral("purple"));
    const MaterialProps p = MaterialProps::fromJson(bad, &errors);
    QCOMPARE(errors.size(), 3);
    QVERIFY(p.isSet(matprops::Key::Metallic));
    QCOMPARE(p.metallic, 1.0f);
    QVERIFY(!p.isSet(matprops::Key::Roughness));
    QVERIFY(MaterialProps::fromJson(bad, nullptr, true).isEmpty());
    MaterialProps q;
    QVERIFY(q.setValue(matprops::Key::AoChannel, QStringLiteral("gray")));
    QCOMPARE(q.aoChannel, matprops::Channel::Grey);
  }

  void matPropsOverlayAndDiff() {
    MaterialProps base = MaterialProps::godotDefaults();
    MaterialProps server;
    server.setValue(matprops::Key::Roughness, 0.4);
    MaterialProps map;
    map.setValue(matprops::Key::Metallic, 0.8);
    map.setValue(matprops::Key::Roughness, 0.2);
    MaterialProps eff = base;
    eff.overlay(server);
    eff.overlay(map);
    QCOMPARE(eff.roughness, 0.2f);
    QCOMPARE(eff.metallic, 0.8f);
    QCOMPARE(eff.normalScale, 1.0f);
    const MaterialProps d = eff.diff(base);
    QCOMPARE(d.setMask, matprops::bit(matprops::Key::Roughness) | matprops::bit(matprops::Key::Metallic));
    QCOMPARE(base.diff(base).setMask, 0u);
  }

  void mapFileNames() {
    ParsedMapFile p = parseMapFileName("AerialAsphalt001_1K_Color.png");
    QVERIFY(p.ok);
    QCOMPARE(p.textureName, QStringLiteral("AerialAsphalt001"));
    QCOMPARE(p.sizeTag, QStringLiteral("1K"));
    QCOMPARE(p.type, MapType::Color);
    p = parseMapFileName("Wood_Floor_01_2k_NormalDX_v2.JPG");
    QVERIFY(p.ok);
    QCOMPARE(p.textureName, QStringLiteral("Wood_Floor_01"));
    QCOMPARE(p.type, MapType::Normal);
    QVERIFY(p.directX);
    QCOMPARE(p.variant, QStringLiteral("v2"));
    QCOMPARE(p.ext, QStringLiteral("jpg"));
    QCOMPARE(parseMapFileName("Foo_1K_IdMask.png").type, MapType::IdMask);
    QCOMPARE(parseMapFileName("Foo_1K_ARM.png").type, MapType::Arm);
    QCOMPARE(parseMapFileName("Foo_1K_AmbientOcclusion.png").type, MapType::Ao);
    QVERIFY(!parseMapFileName("list.txt").ok);
    QVERIFY(!parseMapFileName("Foo_1K_Bogus.png").ok);
    QCOMPARE(makeMapFileName("Tiles", "1K", MapType::Ao, false, "png"), QStringLiteral("Tiles_1K_AmbientOcclusion.png"));
    QCOMPARE(makeMapFileName("Tiles", "1K", MapType::Normal, true, "jpg"), QStringLiteral("Tiles_1K_NormalDX.jpg"));
    QVERIFY(mapTypeRendered(MapType::Opacity));
    QVERIFY(!mapTypeRendered(MapType::Displacement));
    QVERIFY(!mapTypeRendered(MapType::Scattering));
  }

  void materialRefs() {
    MaterialRef r = MaterialRef::parse("cgambient_1k/Tiles012#shiny");
    QCOMPARE(r.pack, QStringLiteral("cgambient_1k"));
    QCOMPARE(r.name, QStringLiteral("Tiles012"));
    QCOMPARE(r.variant, QStringLiteral("shiny"));
    QCOMPARE(r.base(), QStringLiteral("cgambient_1k/Tiles012"));
    QCOMPARE(r.full(), QStringLiteral("cgambient_1k/Tiles012#shiny"));
    r = MaterialRef::parse("devtexture01");
    QVERIFY(r.isLegacy());
    QVERIFY(!r.isVariant());
    QCOMPARE(r.full(), QStringLiteral("devtexture01"));
    QCOMPARE(refKey(" Pack/Name "), QStringLiteral("pack/name"));
    QVERIFY(isValidAssetName("my_pack-01"));
    QVERIFY(!isValidAssetName("../etc"));
    QVERIFY(!isValidAssetName(""));
  }

  void textureEntryJson() {
    TextureEntry e;
    e.ref = "cgambient_1k/Tiles012";
    e.id = "1234";
    e.pack = "cgambient_1k";
    e.name = "Tiles012";
    e.base = true;
    e.w = 1024;
    e.h = 512;
    e.tags = {"tiles", "floor"};
    MapFile c;
    c.file = "Tiles012_1K_Color.png";
    c.w = 1024;
    c.h = 512;
    c.checksum = "abcd";
    c.thumb = "abcd.jpg";
    c.editor = "abcd.jpg";
    e.maps.insert(MapType::Color, c);
    MapFile r;
    r.file = "Tiles012_1K_Roughness.png";
    r.checksum = "ef01";
    r.editor = "ef01.jpg";
    e.maps.insert(MapType::Roughness, r);
    MapFile d;
    d.file = "Tiles012_1K_Displacement.png";
    d.checksum = "9999";
    e.maps.insert(MapType::Displacement, d);
    e.props.setValue(matprops::Key::NormalScale, 0.5);
    e.rev = 42;
    QCOMPARE(e.layout(), MapLayout::Separate);
    QVERIFY(e.hasHeight());
    const QJsonObject j = e.toJson();
    QCOMPARE(j.value("layout").toString(), QStringLiteral("separate"));
    QCOMPARE(j.value("thumb").toString(), QStringLiteral("abcd.jpg"));
    QVERIFY(j.value("maps").toObject().value("displacement").toObject().value("editor").isNull());
    const TextureEntry back = TextureEntry::fromJson(j);
    QVERIFY(back == e);
    e.maps.insert(MapType::Arm, r);
    QCOMPARE(e.layout(), MapLayout::Arm);
  }

  void materialDefaultsFromTags() {
    QJsonObject doc = QJsonDocument::fromJson(R"({"rules":[
        {"name":"metal","match":["metal","steel"],"props":{"metallic":1.0,"roughness":0.45}},
        {"name":"stone","match":["stone","rock"],"props":{"metallic":0.0,"roughness":0.9,"normal_scale":0.4}}],
        "fallback":{"props":{"normal_scale":0.6}}})").object();
    QStringList errors;
    const MaterialDefaults md = MaterialDefaults::fromJson(doc, &errors);
    QVERIFY(errors.isEmpty());

    TextureEntry rock;
    rock.name = "Rocks012";
    rock.tags = {"grey", "cliff"};
    MaterialProps p = md.compute(rock);  // "rocks" folds to "rock" through the name
    QCOMPARE(p.roughness, 0.9f);
    QCOMPARE(p.metallic, 0.0f);
    QCOMPARE(p.normalScale, 0.4f);
    QCOMPARE(md.matchedRule(rock), QStringLiteral("stone"));

    // metal name + ARM layout: the rule's values win over the maps' 1.0, channels follow ARM.
    TextureEntry plate;
    plate.name = "MetalPlate004";
    plate.maps.insert(MapType::Arm, MapFile{});
    p = md.compute(plate);
    QCOMPARE(p.metallic, 1.0f);
    QCOMPARE(p.roughness, 0.45f);
    QCOMPARE(p.roughnessChannel, matprops::Channel::Green);
    QCOMPARE(p.metallicChannel, matprops::Channel::Blue);
    QCOMPARE(p.aoChannel, matprops::Channel::Red);
    QCOMPARE(p.normalScale, 0.6f);  // fallback

    // no rule: the ARM maps drive roughness and metal (both 1.0)
    TextureEntry blank;
    blank.name = "Thing001";
    blank.maps.insert(MapType::Arm, MapFile{});
    p = md.compute(blank);
    QCOMPARE(p.metallic, 1.0f);
    QCOMPARE(p.roughness, 1.0f);

    // tags before name words: tagged rock, named metal -> stone values
    TextureEntry tagged;
    tagged.name = "MetalRock";
    tagged.tags = {"rock"};
    p = md.compute(tagged);
    QCOMPARE(p.roughness, 0.9f);
    QCOMPARE(p.metallic, 0.0f);
    QCOMPARE(md.matchedRule(tagged), QStringLiteral("stone"));

    // separate maps: red channels; the tag value wins over the roughness map
    TextureEntry wall;
    wall.name = "StoneWall";
    wall.alpha = true;
    wall.maps.insert(MapType::Roughness, MapFile{});
    wall.maps.insert(MapType::Emission, MapFile{});
    p = md.compute(wall);
    QCOMPARE(p.roughness, 0.9f);
    QVERIFY(!p.isSet(matprops::Key::Metallic) || p.metallic == 0.0f);
    QCOMPARE(p.roughnessChannel, matprops::Channel::Red);
    QVERIFY(p.emissionEnabled);
    QVERIFY(p.transparency);

    // the bundled rules load and know the common words
    const MaterialDefaults &bundled = MaterialDefaults::instance();
    QVERIFY(bundled.rules().size() >= 10);
    TextureEntry wood;
    wood.name = "WoodFloor041";
    QCOMPARE(bundled.matchedRule(wood), QStringLiteral("wood"));
    // fab_1k/ConcretePlaster: tags concrete + smooth, ARM maps
    TextureEntry cp;
    cp.name = "ConcretePlaster";
    cp.tags = {"concrete", "smooth"};
    cp.maps.insert(MapType::Arm, MapFile{});
    p = bundled.compute(cp);
    QCOMPARE(bundled.matchedRule(cp), QStringLiteral("concrete"));
    QCOMPARE(p.roughness, 0.65f);
    QCOMPARE(p.metallic, 0.2f);
    QCOMPARE(p.metallicSpecular, 0.3f);
  }

  void overridesResolveStack() {
    TextureEntry e;
    e.ref = "cgambient_1k/Tiles012";
    e.pack = "cgambient_1k";
    e.name = "Tiles012";
    e.tags = {"tile", "floor"};
    e.maps.insert(MapType::Color, MapFile{});
    e.maps.insert(MapType::Roughness, MapFile{});
    e.props.setValue(matprops::Key::NormalScale, 0.3);  // server layer

    eng::MaterialOverrides ov;
    MaterialProps map;
    map.setValue(matprops::Key::Metallic, 0.5);
    ov.setMapLayer("CGAmbient_1k/tiles012", map);  // case-insensitive
    eng::MaterialVariant v;
    v.ref = "cgambient_1k/Tiles012#shiny";
    v.props.setValue(matprops::Key::Roughness, 0.2);
    ov.setVariant(v);

    const eng::ResolvedProps base = ov.resolve("cgambient_1k/Tiles012", &e);
    QCOMPARE(base.props.metallic, 0.5f);
    QCOMPARE(base.source[size_t(matprops::Key::Metallic)], eng::PropSource::Map);
    QCOMPARE(base.props.normalScale, 0.3f);
    QCOMPARE(base.source[size_t(matprops::Key::NormalScale)], eng::PropSource::Server);
    QCOMPARE(base.props.roughness, 0.4f);  // the tiles tag wins over the roughness map
    QCOMPARE(base.source[size_t(matprops::Key::Roughness)], eng::PropSource::Default);
    QCOMPARE(base.source[size_t(matprops::Key::EmissionEnergy)], eng::PropSource::Godot);

    const eng::ResolvedProps var = ov.resolve("cgambient_1k/Tiles012#shiny", &e);
    QCOMPARE(var.props.roughness, 0.2f);
    QCOMPARE(var.source[size_t(matprops::Key::Roughness)], eng::PropSource::Variant);
    QCOMPARE(var.props.metallic, 0.5f);  // the variant inherits the map layer

    QCOMPARE(ov.uniqueVariantName("cgambient_1k/Tiles012", "shiny"), QStringLiteral("shiny_2"));
    QCOMPARE(ov.uniqueVariantName("cgambient_1k/Tiles012", "matte wall"), QStringLiteral("matte_wall"));
    QCOMPARE(ov.variantsOf("cgambient_1k/tiles012").size(), 1);

    ov.renameTexture("cgambient_1k/Tiles012", "mymap/TilesShiny");
    QVERIFY(ov.mapLayer("mymap/TilesShiny"));
    QVERIFY(!ov.mapLayer("cgambient_1k/Tiles012"));
    QVERIFY(ov.variant("mymap/TilesShiny#shiny"));
  }

  void overridesJsonRoundTrip() {
    eng::MaterialOverrides ov;
    ov.projectPack = "my_map";
    MaterialProps map;
    map.setValue(matprops::Key::Metallic, 0.25);
    ov.setMapLayer("pack/A", map);
    eng::MaterialVariant v;
    v.ref = "pack/A#wet";
    v.props.setValue(matprops::Key::Roughness, 0.1);
    ov.setVariant(v);
    ov.rememberId("pack/A", "uuid-a");
    QHash<QString, MaterialProps> snap;
    snap.insert("pack/A", MaterialProps::godotDefaults());
    const QJsonObject j = ov.toJson(snap);
    QHash<QString, MaterialProps> snapBack;
    const eng::MaterialOverrides back = eng::MaterialOverrides::fromJson(j, &snapBack);
    QVERIFY(back == ov);
    QCOMPARE(back.idFor("PACK/a"), QStringLiteral("uuid-a"));
    QCOMPARE(snapBack.value("pack/A"), MaterialProps::godotDefaults());
    QCOMPARE(back.variant("pack/A#wet")->baseRef, QStringLiteral("pack/A"));
  }

  void fmapMaterialsRoundTrip() {
    fmap::Document doc;
    eng::MaterialOverrides ov;
    ov.projectPack = "p";
    MaterialProps m;
    m.setValue(matprops::Key::AoLightAffect, 0.7);
    ov.setMapLayer("x/Y", m);
    doc.materials = QJsonDocument(ov.toJson()).toJson(QJsonDocument::Compact);
    QTemporaryDir dir;
    const QString path = dir.filePath("materials.ghmap");
    QString err;
    QVERIFY2(fmap::saveMap(path, doc, &err), qPrintable(err));
    fmap::Document back;
    QVERIFY2(fmap::loadMap(path, back, &err), qPrintable(err));
    QCOMPARE(back.materials, doc.materials);
    QVERIFY(eng::MaterialOverrides::fromJson(QJsonDocument::fromJson(back.materials).object()) == ov);
  }

  void ormComposeLayouts() {
    using matprops::Channel;
    QImage arm(4, 4, QImage::Format_RGBA8888);
    arm.fill(QColor(10, 20, 30, 255));
    // ARM: R=AO, G=rough, B=metal
    QImage orm = eng::composeOrm(arm, Channel::Red, arm, Channel::Green, arm, Channel::Blue);
    QCOMPARE(orm.pixelColor(1, 1), QColor(10, 20, 30, 255));
    // channel overrides
    orm = eng::composeOrm(arm, Channel::Blue, arm, Channel::Red, arm, Channel::Grey);
    QCOMPARE(orm.pixelColor(0, 0), QColor(30, 10, 20, 255));
    // separate maps, missing ones are white (Godot's default)
    QImage rough(8, 8, QImage::Format_Grayscale8);
    rough.fill(100);
    orm = eng::composeOrm(QImage(), Channel::Red, rough, Channel::Red, QImage(), Channel::Red);
    QCOMPARE(orm.size(), QSize(8, 8));
    QCOMPARE(orm.pixelColor(3, 3), QColor(255, 100, 255, 255));
    // nothing at all
    QVERIFY(eng::composeOrm(QImage(), Channel::Red, QImage(), Channel::Red, QImage(), Channel::Red).isNull());
    // opacity merge
    QImage albedo(4, 4, QImage::Format_RGBA8888);
    albedo.fill(QColor(200, 100, 50, 255));
    QImage opacity(2, 2, QImage::Format_Grayscale8);
    opacity.fill(0);
    QCOMPARE(eng::mergeOpacity(albedo, opacity).pixelColor(1, 1).alpha(), 0);
    QCOMPARE(eng::errorChecker().size(), QSize(64, 64));
  }

  void pickingIdsHaveRoom() {
    QCOMPARE(stableIdFromPicking(makePickingId(12345, 100)), 12345u);
    QCOMPARE(faceFromPicking(makePickingId(12345, 100)), 100u);
  }

  // Entity orientation: angles -> matrix -> angles round trip; turning by R turns the facing by R.
  void entityRotationRoundTrip() {
    for (const char *cls: {"light_spot", "prop_static"}) {
      eng::Entity e;
      e.classname = QString::fromLatin1(cls);
      for (const glm::vec3 a: {glm::vec3(-90, 0, 0), glm::vec3(30, 45, 10), glm::vec3(-20, 170, -35), glm::vec3(0, -90, 0)}) {
        e.angles = a;
        const glm::mat3 m = eng::entityRotation(e);
        eng::Entity back = e;
        back.angles = eng::entityAnglesFromRotation(e, m);
        const glm::mat3 m2 = eng::entityRotation(back);
        for (int c = 0; c < 3; ++c)
          QVERIFY2(glm::length(m[c] - m2[c]) < 1e-4f, qPrintable(QStringLiteral("%1 %2,%3,%4").arg(cls).arg(a.x).arg(a.y).arg(a.z)));
      }
    }
    eng::Entity spot;
    spot.classname = QStringLiteral("light_spot");
    spot.angles = glm::vec3(-90, 0, 0);  // facing down
    QVERIFY(glm::length(spot.forward() - glm::vec3(0, -1, 0)) < 1e-5f);
    const glm::mat3 r(glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1, 0, 0)));
    const glm::vec3 want = r * spot.forward();
    spot.angles = eng::entityAnglesFromRotation(spot, r * eng::entityRotation(spot));
    QVERIFY(glm::length(spot.forward() - want) < 1e-4f);
  }

  // Default texture axes: seen from outside, U right and V down (cross(u, v) == -n).
  void defaultTexAxesOriented() {
    const glm::dvec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto &n: axes) {
      glm::vec3 u, v;
      defaultTexAxes(n, u, v);
      QVERIFY(glm::length(glm::cross(u, v) + glm::vec3(n)) < 1e-6f);
      if (std::fabs(n.y) < 0.5)
        QCOMPARE(v, glm::vec3(0, -1, 0));
    }
    // Sloped faces keep the rule's handedness too.
    for (int i = 0; i < 200; ++i) {
      const glm::dvec3 n = glm::normalize(glm::dvec3(std::sin(i * 1.3), std::cos(i * 0.7) * 0.9, std::sin(i * 2.1 + 1.0)));
      glm::vec3 u, v;
      defaultTexAxes(n, u, v);
      QVERIFY(glm::dot(glm::cross(u, v), glm::vec3(n)) < 0.0f);
    }
    // A new box: every face, through the mesh (and getFacePlane points out).
    const auto mesh = boxAt({0, 0, 0}, {4, 4, 4}).toMesh();
    for (int f = 0; f < mesh->faceCount(); ++f) {
      glm::vec3 n;
      float d;
      QVERIFY(mesh->getFacePlane(f, n, d));
      std::vector<glm::vec3> poly;
      mesh->getFacePolygon(f, poly);
      glm::vec3 c(0.0f);
      for (const auto &p: poly)
        c += p;
      QVERIFY(glm::dot(n, c / float(poly.size()) - glm::vec3(2.0f)) > 0.0f);  // outward
      const FaceTexBasis &b = mesh->faceTexBasis(f);
      QVERIFY(glm::dot(glm::cross(b.uAxis, b.vAxis), n) < -0.99f);
    }
  }

  static godot::MaterialJob armJob() {
    godot::MaterialJob m;
    m.ref = QStringLiteral("pk/Brick");
    m.props = MaterialProps::godotDefaults();
    m.props.roughnessChannel = matprops::Channel::Green;
    m.props.metallicChannel = matprops::Channel::Blue;
    m.textures[size_t(MaterialSlot::Albedo)] = godot::SlotTexture {QStringLiteral("textures/pk/Brick_Color.png"), 0};
    m.textures[size_t(MaterialSlot::Normal)] = godot::SlotTexture {QStringLiteral("textures/pk/Brick_NormalGL.png"), 0};
    for (const auto s: {MaterialSlot::Roughness, MaterialSlot::Metallic, MaterialSlot::Ao})
      m.textures[size_t(s)] = godot::SlotTexture {QStringLiteral("textures/pk/Brick_ARM.png"),
                                                  s == MaterialSlot::Roughness ? 1 : s == MaterialSlot::Metallic ? 2 : 0};
    return m;
  }
  static int countOf(const QString &text, const QString &needle) { return int(text.count(needle)); }

  void godotValues() {
    using godot::GdValue;
    const auto t = GdValue::parse(QStringLiteral("Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, -3.78044, 0, -5.54585)"));
    QCOMPARE(t.kind, GdValue::Kind::Call);
    QCOMPARE(t.items.size(), 12);
    QVERIFY(GdValue::parse(t.toText()).equals(t));
    QVERIFY(GdValue::parse(QStringLiteral("0.25")).equals(GdValue::parse(QStringLiteral("0.250000"))));
    QVERIFY(GdValue::parse(QStringLiteral("1")).equals(GdValue::number(1.0)));
    const auto d = GdValue::parse(QStringLiteral("{\n\"nodes\": {\n\"PATH:a\": {\n\"generate/physics\": true\n}\n}\n}"));
    QCOMPARE(d.kind, GdValue::Kind::Dict);
    QVERIFY(d.find(QStringLiteral("nodes")) && d.find(QStringLiteral("nodes"))->find(QStringLiteral("PATH:a")));
    QCOMPARE(GdValue::parse(QStringLiteral("Array[int]([1, 2])")).kind, GdValue::Kind::Raw);
    QCOMPARE(GdValue::parse(QStringLiteral("&\"name\"")).kind, GdValue::Kind::StringName);

    // An untouched file comes back byte for byte.
    const QString text = QStringLiteral("[gd_scene format=3 uid=\"uid://x\"]\n\n[node name=\"A\" type=\"Node3D\"]\n"
                                        "transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0)\n");
    auto file = godot::GdTextFile::parse(text);
    QCOMPARE(file.toText(), text);
    QVERIFY(!file.setProp(file.rootNode(), QStringLiteral("transform"),
                          GdValue::transform({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0})));
    QVERIFY(!file.isDirty());

    glm::vec3 x, y, z;
    godot::basisLookingAlong({1, 0, 0}, x, y, z);
    QVERIFY(glm::length(z - glm::vec3(-1, 0, 0)) < 1e-5f);
    QVERIFY(glm::length(glm::cross(x, y) - z) < 1e-5f);
    QVERIFY(std::fabs(y.y - 1.0f) < 1e-5f);
  }

  void godotMaterialTres() {
    // ARM: one ext_resource for the packed file, used by three slots.
    const QString arm = godot::mergeMaterialTres({}, armJob());
    QVERIFY(arm.startsWith(QStringLiteral("[gd_resource type=\"StandardMaterial3D\" format=3]")));
    QCOMPARE(countOf(arm, QStringLiteral("[ext_resource")), 3);
    QVERIFY(arm.contains(QStringLiteral("roughness_texture_channel = 1")));
    QVERIFY(arm.contains(QStringLiteral("metallic_texture_channel = 2")));
    QVERIFY(!arm.contains(QStringLiteral("ao_texture_channel")));  // red = default
    QVERIFY(arm.contains(QStringLiteral("ao_enabled = true")));
    QVERIFY(arm.contains(QStringLiteral("normal_enabled = true")));
    QVERIFY(!arm.contains(QStringLiteral("load_steps")));
    QVERIFY(!arm.contains(QStringLiteral("roughness = ")));  // 1.0 = default

    // Only a separate AO map: AO keys only, no roughness / metallic textures.
    godot::MaterialJob ao;
    ao.props = MaterialProps::godotDefaults();
    ao.textures[size_t(MaterialSlot::Albedo)] = godot::SlotTexture {QStringLiteral("textures/pk/T_Color.png"), 0};
    ao.textures[size_t(MaterialSlot::Ao)] = godot::SlotTexture {QStringLiteral("textures/pk/T_AmbientOcclusion.png"), 0};
    const QString aoText = godot::mergeMaterialTres({}, ao);
    QVERIFY(aoText.contains(QStringLiteral("ao_texture = ")));
    QVERIFY(!aoText.contains(QStringLiteral("roughness_texture")));
    QVERIFY(!aoText.contains(QStringLiteral("metallic_texture")));
    QVERIFY(!aoText.contains(QStringLiteral("normal_")));

    // A file Godot saved, with a key the user added: only normal_scale changes.
    QString saved = arm;
    saved.replace(QStringLiteral("[resource]\n"), QStringLiteral("[resource]\ncull_mode = 2\n"));
    godot::MaterialJob tweaked = armJob();
    tweaked.props.normalScale = 0.5f;
    const QString merged = godot::mergeMaterialTres(saved, tweaked);
    QVERIFY(merged.contains(QStringLiteral("cull_mode = 2")));
    QVERIFY(merged.contains(QStringLiteral("normal_scale = 0.5")));
    const QStringList a = saved.split('\n'), b2 = merged.split('\n');
    QCOMPARE(a.size() + 1, b2.size());  // one line added, nothing else touched
    // Same values spelled differently: no change at all.
    QString respelled = merged;
    respelled.replace(QStringLiteral("normal_scale = 0.5"), QStringLiteral("normal_scale = 0.500000"));
    QCOMPARE(godot::mergeMaterialTres(respelled, tweaked), respelled);

    // The AO map goes away: its keys and its unused file go too...
    godot::MaterialJob noAo;
    noAo.props = MaterialProps::godotDefaults();
    noAo.textures[size_t(MaterialSlot::Albedo)] = ao.textures[size_t(MaterialSlot::Albedo)];
    const QString dropped = godot::mergeMaterialTres(aoText, noAo);
    QVERIFY(!dropped.contains(QStringLiteral("ao_")));
    QVERIFY(!dropped.contains(QStringLiteral("AmbientOcclusion")));
    QCOMPARE(countOf(dropped, QStringLiteral("[ext_resource")), 1);
    // ...and another material type the user picked is left alone.
    const QString orm = QStringLiteral("[gd_resource type=\"ORMMaterial3D\" format=3]\n\n[resource]\n");
    QCOMPARE(godot::mergeMaterialTres(orm, armJob()), orm);
  }

  void godotGlbImport() {
    const QString existing = QStringLiteral(
        "[remap]\n\nimporter=\"scene\"\nimporter_version=1\ntype=\"PackedScene\"\nuid=\"uid://abc\"\n\n[params]\n\n"
        "nodes/root_type=\"\"\n_subresources={\n\"nodes\": {\n\"PATH:worldspawn\": {\n\"generate/physics\": true\n}\n}\n}\n");
    const QMap<QString, QString> mats {{QStringLiteral("pk__Brick"), QStringLiteral("textures/pk/brick.tres")}};
    const QString merged = godot::mergeGlbImport(existing, mats);
    QVERIFY(merged.contains(QStringLiteral("uid=\"uid://abc\"")));
    QVERIFY(merged.contains(QStringLiteral("\"PATH:worldspawn\"")));
    QVERIFY(merged.contains(QStringLiteral("\"use_external/path\": \"res://textures/pk/brick.tres\"")));
    QVERIFY(merged.contains(QStringLiteral("nodes/use_name_suffixes=true")));
    QCOMPARE(godot::mergeGlbImport(merged, mats), merged);  // nothing to do the second time
  }

  void godotLutImport() {
    const QString fresh = godot::mergeLutImport(QString(), 33);
    QVERIFY(fresh.contains(QStringLiteral("importer=\"3d_texture\"")));
    QVERIFY(fresh.contains(QStringLiteral("type=\"CompressedTexture3D\"")));
    QVERIFY(fresh.contains(QStringLiteral("slices/horizontal=33")));
    QVERIFY(fresh.contains(QStringLiteral("slices/vertical=1")));
    QVERIFY(fresh.contains(QStringLiteral("compress/mode=0")));
    QVERIFY(fresh.contains(QStringLiteral("mipmaps/generate=false")));
    QCOMPARE(godot::mergeLutImport(fresh, 33), fresh);
    // Godot filled it in: the uid and other keys stay, the slice count follows the LUT.
    const QString godotWrote = QStringLiteral(
        "[remap]\n\nimporter=\"3d_texture\"\ntype=\"CompressedTexture3D\"\nuid=\"uid://lut1\"\n"
        "path=\"res://.godot/imported/a.png-x.ctex3d\"\n\n[deps]\n\nsource_file=\"res://textures/luts/a.png\"\n\n"
        "[params]\n\ncompress/mode=0\ncompress/lossy_quality=0.7\nslices/horizontal=8\nslices/vertical=8\n");
    const QString merged = godot::mergeLutImport(godotWrote, 64);
    QVERIFY(merged.contains(QStringLiteral("uid=\"uid://lut1\"")));
    QVERIFY(merged.contains(QStringLiteral("compress/lossy_quality=0.7")));
    QVERIFY(merged.contains(QStringLiteral("slices/horizontal=64")));
    QVERIFY(merged.contains(QStringLiteral("slices/vertical=1")));
    // Imported as a 2D texture before: replaced, uid kept.
    const QString as2d = QStringLiteral(
        "[remap]\n\nimporter=\"texture\"\ntype=\"CompressedTexture2D\"\nuid=\"uid://lut2\"\n\n[params]\n\n"
        "compress/mode=2\nroughness/mode=1\n");
    const QString replaced = godot::mergeLutImport(as2d, 33);
    QVERIFY(replaced.contains(QStringLiteral("importer=\"3d_texture\"")));
    QVERIFY(replaced.contains(QStringLiteral("uid=\"uid://lut2\"")));
    QVERIFY(!replaced.contains(QStringLiteral("roughness/mode")));
  }

  void godotPlanLuts() {
    eng::ColorLut a;
    a.name = QStringLiteral("Warm sunset");
    a.strip = eng::lut::identityStrip(17);
    std::vector<eng::ColorLut> luts {a, a, eng::ColorLut {}};
    luts[1].strip = eng::lut::identityStrip(33);
    const std::vector<godot::LutJob> jobs = godot::planLuts(luts);
    QCOMPARE(int(jobs.size()), 2);  // the empty one is skipped
    QCOMPARE(jobs[0].rel, QStringLiteral("textures/luts/Warm_sunset.png"));
    QCOMPARE(jobs[1].rel, QStringLiteral("textures/luts/Warm_sunset_2.png"));
    QCOMPARE(jobs[1].strip.height(), 33);
  }

  void colorLutGrade() {
    eng::LutAdjustments g;
    g.exposure = 0.75;
    g.hue = -30.0;
    g.shadows = QColor(40, 60, 200);
    QCOMPARE(eng::LutAdjustments::fromJson(g.toJson()), g);
    QCOMPARE(eng::LutAdjustments::fromJson({}), eng::LutAdjustments());
    const eng::ColorLut l = eng::lut::graded(QStringLiteral("G"), eng::LutGrade::basic(g));
    QVERIFY(l.isGraded() && l.isValid());
    QCOMPARE(l.strip, eng::lut::applyAdjustments(eng::lut::templateStrip(), g));
    QCOMPARE(eng::lut::graded(QStringLiteral("N"), {}).strip, eng::lut::templateStrip());
    // Sampled like Godot: black and white stay, the middle is off a little.
    const QImage id = eng::lut::identityStrip(33);
    QCOMPARE(eng::lut::sample(id, Qt::white), QColor(Qt::white));
    QCOMPARE(eng::lut::sample(id, Qt::black), QColor(Qt::black));
    QVERIFY(std::abs(eng::lut::sample(id, QColor(128, 128, 128)).red() - 128) <= 1);
  }


  void projectCreate() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString loc = tmp.path();
    bool existing = true;
    QVERIFY(!proj::Project::validateName(loc, QString(), &existing).isEmpty());
    QVERIFY(!existing);
    QVERIFY(!proj::Project::validateName(loc, QStringLiteral("a/b")).isEmpty());
    QVERIFY(!proj::Project::validateName(loc, QStringLiteral(".hidden")).isEmpty());
    QVERIFY(!proj::Project::validateName(loc, QStringLiteral(" padded")).isEmpty());
    QVERIFY(!proj::Project::validateName(loc, QStringLiteral("a \"b\"")).isEmpty()); // not on Windows
    const QString name = QStringLiteral("Kroket's Game");
    QVERIFY(proj::Project::validateName(loc, name).isEmpty());

    QString err;
    auto p = proj::Project::create(loc, name, &err);
    QVERIFY2(p, qPrintable(err));
    for (const QString &f: proj::Project::standardFolders())
      QVERIFY2(QFileInfo(QDir(p->root()).filePath(f)).isDir(), qPrintable(f));
    QVERIFY(QFileInfo(p->path(QStringLiteral(".ghammer/settings.json"))).isFile());
    QVERIFY(proj::Project::isProjectDir(p->root()));
    // game/ is a Godot project the export accepts, named after ours.
    QCOMPARE(godot::checkProject(p->gameDir()), QString());
    QCOMPARE(proj::godotProjectName(p->gameDir()), name);
    QVERIFY(QFileInfo(QDir(p->gameDir()).filePath(QStringLiteral("icon.svg"))).isFile());
    QVERIFY(QFileInfo(QDir(p->gameDir()).filePath(QStringLiteral(".editorconfig"))).isFile());
    QVERIFY(!QFileInfo(QDir(p->gameDir()).filePath(QStringLiteral(".godot"))).exists());
    // Taken now: the same name is refused and recognised as a project.
    QVERIFY(!proj::Project::validateName(loc, name, &existing).isEmpty());
    QVERIFY(existing);
    QVERIFY(!proj::Project::create(loc, name));
    // config/name is a Godot string: quotes and backslashes survive.
    const QString odd = QDir(loc).filePath(QStringLiteral("odd"));
    QVERIFY(proj::writeNewGodotProject(odd, QStringLiteral("A \"B\" \\ C")));
    QCOMPARE(proj::godotProjectName(odd), QStringLiteral("A \"B\" \\ C"));
    QVERIFY(!proj::writeNewGodotProject(odd, QStringLiteral("again"))); // never over an existing one
    // Window size and frame rate cap in Godot's keys; none given: nothing written.
    proj::NewGodotSettings gs;
    gs.viewportWidth = 1920;
    gs.viewportHeight = 1080;
    gs.maxFps = 120;
    const QString text = proj::newProjectGodotText(QStringLiteral("x"), gs);
    QVERIFY(text.contains(QStringLiteral("[application]")) && text.contains(QStringLiteral("run/max_fps=120\n")));
    QVERIFY(text.contains(QStringLiteral("[display]\n\nwindow/size/viewport_width=1920\nwindow/size/viewport_height=1080\n")));
    QVERIFY(!proj::newProjectGodotText(QStringLiteral("x")).contains(QStringLiteral("max_fps")));
    // A plain folder of that name is not a project.
    QDir(loc).mkdir(QStringLiteral("plain"));
    // The projects in a folder (the chooser scans ~/GodotProjects): only real ones.
    const QStringList found = proj::Project::findProjects(loc);
    QVERIFY(found.contains(p->root()));
    QVERIFY(!found.contains(QDir::cleanPath(QDir(loc).filePath(QStringLiteral("plain")))));
    QVERIFY(!proj::Project::validateName(loc, QStringLiteral("plain"), &existing).isEmpty());
    QVERIFY(!existing);

    // Open it again; files inside it find it.
    auto again = proj::Project::open(p->root(), &err);
    QVERIFY2(again, qPrintable(err));
    QCOMPARE(again->name(), name);
    const QString level = QDir(again->levelsDir()).filePath(QStringLiteral("sub/e1m1.ghmap"));
    QCOMPARE(proj::Project::findEnclosing(level), again->root());
    QCOMPARE(again->relative(level), QStringLiteral("levels/sub/e1m1.ghmap"));
    QCOMPARE(proj::Project::findEnclosing(loc), QString());
    QVERIFY(!proj::Project::open(loc));
  }

  void projectConfigRoundTrip() {
    QTemporaryDir tmp;
    const QString file = QDir(tmp.path()).filePath(QStringLiteral(".ghammer/settings.json"));
    {
      proj::ProjectConfig c(file);
      c.setName(QStringLiteral("game"));
      c.setRaw(QStringLiteral("Layout/Wall"), 8);
      c.setRaw(QStringLiteral("Export/Target"), QStringLiteral("game"));
      c.removeRaw(QStringLiteral("Export/Target"));
      QVERIFY(c.sync());
    }
    // Unknown top-level keys survive a rewrite.
    {
      QFile f(file);
      QVERIFY(f.open(QIODevice::ReadOnly));
      QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
      f.close();
      QCOMPARE(root.value(QStringLiteral("format")).toInt(), proj::ProjectConfig::kFormat);
      root.insert(QStringLiteral("future"), QStringLiteral("kept"));
      QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
      f.write(QJsonDocument(root).toJson());
    }
    proj::ProjectConfig c(file);
    QVERIFY(c.load());
    QCOMPARE(c.name(), QStringLiteral("game"));
    QCOMPARE(c.getRaw(QStringLiteral("Layout/Wall")).toInt(), 8);
    QVERIFY(!c.getRaw(QStringLiteral("Export/Target")).isValid());
    QCOMPARE(c.getRaw(QStringLiteral("Missing"), 3).toInt(), 3);
    c.setRaw(QStringLiteral("Layout/Wall"), 12);
    QVERIFY(c.sync());
    QFile f(file);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    QCOMPARE(root.value(QStringLiteral("future")).toString(), QStringLiteral("kept"));
    QCOMPARE(root.value(QStringLiteral("settings")).toObject().value(QStringLiteral("Layout/Wall")).toInt(), 12);
  }

  void levelSettingsChunk() {
    fmap::Document doc;
    doc.levelSettings = QByteArray(R"({"Export/Collision":false})");
    const QString path = QDir::temp().filePath(QStringLiteral("brush_tests_lcfg.ghmap"));
    QVERIFY(fmap::saveMap(path, doc));
    fmap::Document back;
    QVERIFY(fmap::loadMap(path, back));
    QFile::remove(path);
    const QJsonObject o = QJsonDocument::fromJson(back.levelSettings).object();
    QCOMPARE(o.value(QStringLiteral("Export/Collision")).toBool(true), false);
  }

  void undoCleanMarker() {
    struct Step final : UndoCommand {
      bool doc = true;
      QString key;
      void undo() override {}
      void redo() override {}
      QString text() const override { return QStringLiteral("step"); }
      QString mergeKey() const override { return key; }
      bool mergeWith(const UndoCommand *o) override { return !key.isEmpty() && o->mergeKey() == key; }
      bool changesDocument() const override { return doc; }
    };
    const auto step = [](bool doc = true, QString key = {}) {
      auto s = std::make_unique<Step>();
      s->doc = doc;
      s->key = std::move(key);
      return s;
    };
    UndoStack u;
    QVERIFY(u.isClean());
    u.push(step());
    QVERIFY(!u.isClean());
    u.undo();
    QVERIFY(u.isClean());
    // Saved, then the oldest steps fall off the end of a full history.
    u.setMaxSteps(3);
    u.clear();
    u.push(step());
    u.markClean();
    for (int i = 0; i < 3; ++i)
      u.push(step());
    QCOMPARE(u.undoDepth(), 3); // one step dropped: the saved state is now depth 0
    QVERIFY(!u.isClean());
    for (int i = 0; i < 3; ++i)
      u.undo();
    QVERIFY(u.isClean());
    for (int i = 0; i < 3; ++i)
      u.redo();
    u.push(step()); // a second step dropped: the saved state is gone
    for (int i = 0; i < 3; ++i)
      u.undo();
    QVERIFY(!u.isClean());
    // A new edit after undoing past the saved state throws it away.
    u.setMaxSteps(50);
    u.clear();
    u.push(step());
    u.push(step());
    u.markClean();
    u.undo();
    u.push(step());
    QVERIFY(!u.isClean());
    QCOMPARE(u.undoDepth(), 2);
    // ... unless the steps thrown away only changed the selection.
    u.clear();
    u.push(step());
    u.push(step(false));
    u.markClean();
    u.undo();
    u.push(step(false));
    QVERIFY(u.isClean());
    // A merge into the saved step changes it.
    u.clear();
    u.push(step(true, QStringLiteral("drag")));
    u.markClean();
    u.push(step(true, QStringLiteral("drag")));
    QCOMPARE(u.undoDepth(), 1);
    QVERIFY(!u.isClean());
  }

  void lutRefRoundTrip() {
    fmap::Document doc;
    doc.scene.lut = QStringLiteral("ghammer_luts/Desert");
    doc.scene.sky = QStringLiteral("sourceskyboxes/SkyMorning002");
    const QString path = QDir::temp().filePath(QStringLiteral("brush_tests_lut.ghmap"));
    QVERIFY(fmap::saveMap(path, doc));
    fmap::Document back;
    QVERIFY(fmap::loadMap(path, back));
    QFile::remove(path);
    QCOMPARE(back.scene.lut, doc.scene.lut);
    QCOMPARE(back.scene.sky, doc.scene.sky);
  }

  void godotGlb() {
    gltf::Document doc;
    gltf::Mesh m;
    m.name = QStringLiteral("worldspawn");
    gltf::Primitive p;
    p.positions = {{0, 0, 0}, {1, 0, 0}, {0, 2, 0}};
    p.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    p.uvs = {{0, 0}, {1, 0}, {0, 1}};
    p.indices = {0, 1, 2};
    m.primitives.push_back(p);
    doc.meshes.push_back(m);
    doc.nodes.push_back({QStringLiteral("worldspawn-col"), 0, {}, glm::vec3(0.0f), {}});
    doc.roots = {0};
    const QByteArray glb = gltf::toGlb(doc);
    QCOMPARE(glb, gltf::toGlb(doc));  // deterministic
    QJsonObject json;
    QByteArray bin;
    QVERIFY(gltf::readGlb(glb, &json, &bin));
    QCOMPARE(json.value(QStringLiteral("nodes")).toArray().at(0).toObject().value(QStringLiteral("name")).toString(),
             QStringLiteral("worldspawn-col"));
    const auto acc = json.value(QStringLiteral("accessors")).toArray().at(0).toObject();
    QCOMPARE(acc.value(QStringLiteral("max")).toArray().at(1).toDouble(), 2.0);
    QCOMPARE(bin.size() % 4, 0);
    QCOMPARE(qsizetype(json.value(QStringLiteral("buffers")).toArray().at(0).toObject().value(QStringLiteral("byteLength")).toInt()),
             bin.size());
  }

  // The level's sun as a DirectionalLight3D: its -Z is the sun's direction.
  void godotSceneSun() {
    godot::LevelPlan plan;
    plan.mapName = QStringLiteral("box");
    godot::SceneSun sun;
    sun.present = true;
    sun.direction = glm::normalize(glm::vec3(-0.3f, -0.6f, 0.74f));
    sun.color = QColor::fromRgbF(1.0, 0.9, 0.8);
    sun.energy = 1.25f;
    sun.shadows = true;
    const auto m = godot::mergeLevelScene({}, {}, plan, sun, {}, kMetersPerWorldUnit);
    QVERIFY(m.text.contains(QStringLiteral("type=\"DirectionalLight3D\"")));
    QVERIFY(m.text.contains(QStringLiteral("light_energy = 1.25")));
    QVERIFY(m.text.contains(QStringLiteral("shadow_enabled = true")));
    QVERIFY(m.text.contains(QStringLiteral("light_color = Color(")));
    const int at = int(m.text.indexOf(QStringLiteral("Transform3D(")));
    QVERIFY(at >= 0);
    const QString inner = m.text.mid(at + 12, m.text.indexOf(QLatin1Char(')'), at) - at - 12);
    const QStringList v = inner.split(QLatin1Char(','));
    QCOMPARE(v.size(), 12);
    // Row by row: column 2 (z) is (v[2], v[5], v[8]); the light shines along -z.
    const glm::vec3 minusZ(-v[2].toFloat(), -v[5].toFloat(), -v[8].toFloat());
    QVERIFY(glm::length(minusZ - sun.direction) < 1e-4f);
    // Off: no sun node, and an earlier one goes.
    godot::SceneSun none;
    const auto off = godot::mergeLevelScene(m.text, {}, plan, none, {}, kMetersPerWorldUnit);
    QVERIFY(!off.text.contains(QStringLiteral("DirectionalLight3D")));
  }

  void godotSceneMerge() {
    godot::LevelPlan plan;
    plan.mapName = QStringLiteral("box");
    plan.levelRel = QStringLiteral("levels/box");
    godot::GlbJob g;
    g.key = QStringLiteral("worldspawn");
    g.rel = QStringLiteral("levels/box/worldspawn.glb");
    plan.glbs.push_back(g);
    Entity light = ent(1, "light", "lamp");
    light.origin = glm::vec3(10.0f, 4.0f, 0.0f);
    light.props.insert(QStringLiteral("light_energy"), 2.0);
    const godot::SceneSun sun;  // not present
    const auto first = godot::mergeLevelScene({}, {light}, plan, sun, {}, kMetersPerWorldUnit);
    QVERIFY(first.text.startsWith(QStringLiteral("[gd_scene format=3]")));
    QVERIFY(first.text.contains(QStringLiteral("instance=ExtResource(")));
    QVERIFY(first.text.contains(QStringLiteral("type=\"OmniLight3D\"")));
    QVERIFY(first.text.contains(QStringLiteral("light_energy = 2.0")));
    QCOMPARE(first.entityNodes.value(light.id), QStringLiteral("lights/lamp"));
    // Unchanged map: unchanged file.
    QCOMPARE(godot::mergeLevelScene(first.text, {light}, plan, sun, {}, kMetersPerWorldUnit).text, first.text);

    // The user renames the light, tweaks it and adds a node of their own.
    QString edited = first.text;
    edited.replace(QStringLiteral("[node name=\"lamp\""), QStringLiteral("[node name=\"Renamed\""));
    // shadow_opacity: a property the editor does not write (shadow_bias it does).
    edited.replace(QStringLiteral("light_energy = 2.0\n"), QStringLiteral("light_energy = 2.0\nshadow_opacity = 0.5\n"));
    edited += QStringLiteral("\n[node name=\"Mine\" type=\"Node3D\" parent=\".\"]\n");
    light.origin = glm::vec3(20.0f, 4.0f, 0.0f);
    const auto second = godot::mergeLevelScene(edited, {light}, plan, sun, {}, kMetersPerWorldUnit);
    QVERIFY(second.text.contains(QStringLiteral("shadow_opacity = 0.5")));
    QVERIFY(second.text.contains(QStringLiteral("[node name=\"Mine\"")));
    QVERIFY(second.text.contains(QStringLiteral("[node name=\"Renamed\"")));
    QVERIFY(second.text.contains(QStringLiteral("4.0625")));  // 20 world units * 0.203125
    QCOMPARE(second.entityNodes.value(light.id), QStringLiteral("lights/Renamed"));

    // The light is deleted in the editor: its node (and the empty Lights) go.
    const auto third = godot::mergeLevelScene(second.text, {}, plan, sun, {}, kMetersPerWorldUnit);
    QVERIFY(!third.text.contains(QStringLiteral("OmniLight3D")));
    QVERIFY(!third.text.contains(QStringLiteral("\"lights\"")));
    QVERIFY(third.text.contains(QStringLiteral("[node name=\"Mine\"")));

    // Renamed in the editor: the node follows, unless Godot renamed it first.
    Entity lamp2 = ent(2, "light", "a");
    const auto r1 = godot::mergeLevelScene({}, {lamp2}, plan, sun, {}, kMetersPerWorldUnit);
    lamp2.name = QStringLiteral("b");
    const auto r2 = godot::mergeLevelScene(r1.text, {lamp2}, plan, sun, {}, kMetersPerWorldUnit);
    QCOMPARE(r2.entityNodes.value(lamp2.id), QStringLiteral("lights/b"));
    QString godotRenamed = r2.text;
    godotRenamed.replace(QStringLiteral("[node name=\"b\""), QStringLiteral("[node name=\"Mine2\""));
    lamp2.name = QStringLiteral("c");
    const auto r3 = godot::mergeLevelScene(godotRenamed, {lamp2}, plan, sun, {}, kMetersPerWorldUnit);
    QCOMPARE(r3.entityNodes.value(lamp2.id), QStringLiteral("lights/Mine2"));
  }
};

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  // shared_assets.qrc is in the static `shared` library: this links it in.
  Q_INIT_RESOURCE(shared_assets);
  BrushTests tc;
  QTEST_SET_MAIN_SOURCE_PATH
  return QTest::qExec(&tc, argc, argv);
}
#include "brush_tests.moc"
