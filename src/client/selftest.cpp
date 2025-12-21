// EDITOR_SELFTEST=1: end-to-end check of the editing operations in the running app. Prints PASS / FAIL and quits.
#include <QCoreApplication>
#include <QDir>
#include <QTimer>

#include "client/engine/brush.h"
#include <cmath>
#include <tuple>
#include "client/engine/scene.h"
#include "client/engine/color_lut.h"
#include "client/engine/entity.h"
#include "client/engine/io.h"
#include "client/engine/godot_export.h"
#include "client/engine/gltf_writer.h"
#include <QJsonArray>
#include <QFile>
#include "client/vulkan/vk_engine.h"
#include "client/prefs/preferences.h"
#include "mainwindow.h"
#include "client/ui/layout_designer.h"
#include "client/net/asset_client.h"
#include "client/net/texture_catalog.h"
#include "client/vulkan/vk_shared.h"
#include "client/project/document_manager.h"
#include "client/project/project.h"
#include "client/project/project_manager.h"
#include "client/ui/action_manager.h"
#include "client/ui/project_panel.h"
#include "client/ui/file_icons.h"
#include <QApplication>
#include <QDockWidget>
#include "ghammer_version.h"
#include <QImage>
#include <QEventLoop>
#include <QTemporaryDir>
#include <functional>

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
  qInfo().noquote() << (ok ? "[selftest] PASS " : "[selftest] FAIL ") << what;
  if (!ok)
    ++g_fail;
}

size_t brushCount(vk::Engine3D *e) { return e->scene() ? e->scene()->gameObjects.size() : 0; }

// Spins the event loop (network, rendering) until `done` or `ms` pass.
bool waitUntil(const std::function<bool()> &done, int ms) {
  QEventLoop loop;
  QTimer tick;
  tick.setInterval(20);
  QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
    if (done())
      loop.quit();
  });
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  tick.start();
  if (!done())
    loop.exec();
  return done();
}

} // namespace

void runSelfTest(MainWindow *w) {
  auto *e = g::engine3D;
  w->newScene();
  const size_t base = brushCount(e);
  {
    bool onlyWorldEnv = true;
    for (const auto &en: e->scene()->entities)
      onlyWorldEnv = onlyWorldEnv && en.classname == eng::kWorldEnvClass;
    check(onlyWorldEnv, "a new map has no light");
  }

  // Primitives
  check(e->createPrimitive(0, {0, 0, 0}, {25.6f, 12.8f, 25.6f}), "create box");
  check(brushCount(e) == base + 1, "box added");
  check(e->createPrimitive(2, {40, 0, 0}, {52.8f, 12.8f, 12.8f}), "create cylinder");
  const auto cyl = e->scene()->gameObjects.back();
  check(cyl->getMesh()->faceCount() == prefs::i("Brush/CylinderSides") + 2, "cylinder face count");
  check(e->createPrimitive(6, {0, 0, 40}, {12.8f, 12.8f, 64}), "create stairs");
  check(brushCount(e) == base + 2 + size_t(prefs::i("Brush/StairSteps")), "stairs are separate brushes");
  const size_t afterStairs = brushCount(e);
  check(e->createPrimitive(7, {80, 0, 0}, {105.6f, 12.8f, 6.4f}), "create arch");

  // Subtract: a small box through the first one
  e->setSelected({}, {});
  const quint32 bigId = e->scene()->gameObjects[base]->stableId();
  e->createPrimitive(0, {6.4f, -1, 6.4f}, {19.2f, 20, 19.2f});
  const size_t beforeSub = brushCount(e);
  const int frags = e->csgSubtract(); // selection = the new cutter
  check(frags == 4, "subtract a column through a box -> 4 fragments");
  check(e->scene()->findById(bigId) == nullptr, "minuend replaced");
  e->undo();
  check(brushCount(e) == beforeSub && e->scene()->findById(bigId) != nullptr, "undo subtract");
  e->redo();
  check(e->scene()->findById(bigId) == nullptr, "redo subtract");

  // Hollow a fresh box
  e->createPrimitive(0, {0, 30, 0}, {25.6f, 56, 25.6f});
  check(e->csgHollow(16.0) == 6, "hollow -> 6 walls");

  // Rotate a wedge 90° around Y with texture lock and check it stays valid
  e->setTextureLock(true);
  e->createPrimitive(1, {120, 0, 0}, {132.8f, 6.4f, 25.6f});
  check(e->rotateSelection(1, 90.0), "rotate wedge");
  check(e->mirrorSelection(0), "mirror wedge");
  auto wedge = e->selectedObject(*e->selectedObjects().begin());
  check(wedge && eng::ConvexBrush::fromObject(*wedge).has_value(), "rotated wedge is a valid brush");

  // Merge two touching boxes
  e->createPrimitive(0, {200, 0, 0}, {212.8f, 12.8f, 12.8f});
  const quint32 a = *e->selectedObjects().begin();
  e->createPrimitive(0, {212.8f, 0, 0}, {225.6f, 12.8f, 12.8f});
  const quint32 b = *e->selectedObjects().begin();
  e->setSelected({a, b}, {});
  check(e->csgMerge(), "merge two boxes");
  check(e->selectedObjects().size() == 1, "merge leaves one brush");

  // Clip
  e->createPrimitive(0, {300, 0, 0}, {325.6f, 25.6f, 25.6f});
  check(e->clipSelection(glm::normalize(glm::vec3(1, 1, 0)), glm::dot(glm::normalize(glm::vec3(1, 1, 0)), glm::vec3(312.8f, 12.8f, 0)), 2) == 2,
        "clip keeps both halves");

  // Copy / paste
  {
    e->createPrimitive(3, {600, 0, 0}, {612.8f, 12.8f, 12.8f});
    const size_t before = brushCount(e);
    check(e->copySelection(), "copy a cone");
    check(e->paste(false) && brushCount(e) == before + 1, "paste adds a copy");
    glm::vec3 mn, mx;
    e->selectionBounds(mn, mx);
    check(mn.x > 600.5f, "pasted copy is offset");
    e->undo();
    check(brushCount(e) == before, "undo paste");
  }

  // Free-angle cut through a box in the Top view (plane 0)
  {
    e->createPrimitive(0, {500, 0, 0}, {525.6f, 12.8f, 25.6f});
    e->enterCutMode();
    const auto bs = vk::viewBasisFor(0);
    const glm::dvec2 a(glm::dot(bs.right, glm::dvec3(500, 0, 0)), glm::dot(bs.up, glm::dvec3(500, 0, 0)));
    const glm::dvec2 b(glm::dot(bs.right, glm::dvec3(525.6, 0, 25.6)), glm::dot(bs.up, glm::dvec3(525.6, 0, 25.6)));
    e->beginCutDrag(0, a, 1.28);
    e->updateCutDrag(b);
    e->endCutDrag();
    check(e->cutLine().angled, "drew an angled cut line");
    while (e->cutMode() != vk::Engine3D::CutMode::KeepBoth)
      e->cycleCutMode();
    const size_t before = brushCount(e);
    check(e->applyCut(), "angled cut applied");
    check(brushCount(e) == before + 1, "diagonal cut splits the box in two");
    e->exitCutMode();
  }

  // Vertex editing: raise the top of a box
  {
    e->createPrimitive(0, {400, 0, 0}, {412.8f, 12.8f, 12.8f});
    const quint32 box = *e->selectedObjects().begin();
    e->setEditMode(true);
    check(e->vertexHandles().size() == 8, "box shows 8 vertex handles");
    e->m_selectedVerts.clear();
    for (const auto &h: e->vertexHandles())
      if (h.pos.y > 12.0f)
        e->m_selectedVerts.push_back(h);
    check(e->m_selectedVerts.size() == 4, "select the top 4 vertices");
    e->beginVertexDrag();
    e->updateVertexDrag({0, 6.4f, 0});
    e->endVertexDrag({0, 6.4f, 0});
    glm::vec3 mn, mx;
    e->selectionBounds(mn, mx);
    check(std::fabs((mx.y - mn.y) - 19.2f) < 1e-3f, "vertex drag made the box taller");
    check(e->scene()->findById(box) == nullptr, "vertex edit swapped the brush (undoable)");
    e->undo();
    check(e->scene()->findById(box) != nullptr, "undo vertex edit");
    e->setEditMode(false);
  }

  // Entities: add, edit, move, undo, light list, sun
  {
    const quint32 l = e->addEntity(QStringLiteral("light"), {10, 5, 10});
    const quint32 sp = e->addEntity(QStringLiteral("light_spot"), {20, 5, 10});
    const quint32 pl = e->addEntity(QStringLiteral("info_player_start"), {0, 1, 0});
    check(l && sp && pl, "add light, spot, player start");
    e->setEntityProperty(l, QStringLiteral("light_energy"), 7.5);
    check(std::fabs(e->entityById(l)->num(QStringLiteral("light_energy")) - 7.5) < 1e-6, "set entity property");
    e->undo();
    check(std::fabs(e->entityById(l)->num(QStringLiteral("light_energy")) - 2.0) < 1e-6, "undo entity property");
    e->setSelected({}, {});
    e->setSelectedEntities({l});
    e->translateSelection({1, 0, 0}, QStringLiteral("Move"));
    check(std::fabs(e->entityById(l)->origin.x - 11.0f) < 1e-5f, "move entity");
    e->undo();
    check(std::fabs(e->entityById(l)->origin.x - 10.0f) < 1e-5f, "undo entity move");
    glm::vec4 buf[32 * 4];
    check(e->fillLightUniforms(buf, 32) == 2, "two local lights reach the shader");
    const quint32 sun = e->addEntity(QStringLiteral("sun"), {0, 50, 0});
    e->setEntityTransform(sun, {0, 50, 0}, {-60.0f, 30.0f, 0.0f}, QString());
    e->applySunEntity();
    check(std::fabs(e->sunDirection().y - std::sin(glm::radians(-60.0f))) < 1e-4f, "sun entity drives the sun");
    e->setSelectedEntities({pl});
    e->deleteSelected();
    check(e->entityById(pl) == nullptr, "delete entity");
    e->undo();
    check(e->entityById(pl) != nullptr, "undo delete entity");
  }
  // Brush entities have no marker; Camera3D preview.
  {
    check(e->createPrimitive(0, {300, 0, 300}, {312.8f, 12.8f, 312.8f}), "box for a clip brush");
    const quint32 clip = e->makeBrushEntity(eng::kClipClass);
    check(clip != 0, "make a clip brush entity");
    bool clipMarker = false;
    for (const auto &m: e->entityMarkers())
      clipMarker = clipMarker || m.id == clip;
    check(!clipMarker, "a brush entity has no marker");
    e->undo();

    const quint32 cam = e->addEntity(eng::kCameraClass, {5, 6, 7});
    check(cam != 0, "add a Camera3D");
    e->setEntityTransform(cam, {5, 6, 7}, {-10.0f, 90.0f, 0.0f}, QString());
    const glm::vec3 editorPos = e->camera()->getPosition();
    const float editorFov = e->camera()->getFov();
    e->setSelected({}, {});
    e->setSelectedEntities({});
    check(!e->cameraPreviewAvailable(), "no preview without a selected camera");
    e->setSelectedEntities({cam});
    check(e->cameraPreviewAvailable(), "selected camera offers preview");
    e->setCameraPreview(true);
    const glm::vec3 dir = e->camera()->getDirection();
    check(glm::distance(e->camera()->getPosition(), glm::vec3(5, 6, 7)) < 1e-4f
              && std::fabs(e->camera()->getFov() - 75.0f) < 1e-4f
              && glm::distance(dir, e->entityById(cam)->forward()) < 1e-3f,
          "preview looks through the camera (position, direction, fov 75)");
    e->setSelectedEntities({});
    check(e->cameraPreview() && glm::distance(e->camera()->getPosition(), glm::vec3(5, 6, 7)) < 1e-4f,
          "preview stays on (and looks through the camera) when something else is selected");
    e->setCameraPreview(false);
    check(!e->cameraPreview() && glm::distance(e->camera()->getPosition(), editorPos) < 1e-4f
              && std::fabs(e->camera()->getFov() - editorFov) < 1e-4f,
          "turning preview off restores the editor camera");
    e->setSelectedEntities({cam});
    e->deleteSelected();
  }
  // Model prop (EDITOR_SELFTEST_MODEL=path/to/model)
  if (const QString model = qEnvironmentVariable("EDITOR_SELFTEST_MODEL"); !model.isEmpty()) {
    const quint32 prop = e->addEntity(QStringLiteral("prop_static"), {0, 0, 80},
                                      {{QStringLiteral("model"), model}});
    e->syncPropObjects(true);
    std::shared_ptr<eng::GameObject> render;
    for (const auto &o: e->scene()->gameObjects)
      if (o->ownerEntity() == prop)
        render = o;
    check(render != nullptr, "prop_static gets a render object");
    check(render && render->getMesh()->faceUsesVertexUV(0), "prop faces use model UVs");
    check(render && glm::length(render->getPosition() - glm::vec3(0, 0, 80)) < 1e-4f, "prop placed at entity origin");
  }
  // Entity I/O: outputs, rename, paste with unique names, validation.
  quint32 ioRelay = 0;
  {
    ioRelay = e->addEntity(QStringLiteral("logic_relay"), {30, 2, 30});
    const quint32 lamp = e->addEntity(QStringLiteral("light"), {34, 2, 30});
    e->renameEntity(ioRelay, QStringLiteral("st_relay"));
    e->renameEntity(lamp, QStringLiteral("st_lamp"));
    eng::io::Connection c;
    c.output = QStringLiteral("OnTrigger");
    c.target = QStringLiteral("st_lamp");
    c.input = QStringLiteral("Toggle");
    c.delay = 0.5f;
    e->setEntityOutputs(ioRelay, {c});
    check(e->entityById(ioRelay)->outputs.size() == 1, "add an output");
    e->undo();
    check(e->entityById(ioRelay)->outputs.isEmpty(), "undo add output");
    e->redo();
    check(e->entityById(ioRelay)->outputs.size() == 1, "redo add output");
    e->renameEntity(lamp, QStringLiteral("st_light"));
    check(e->entityById(ioRelay)->outputs[0].target == QStringLiteral("st_light"), "rename updates the connection");
    e->undo();
    check(e->entityById(ioRelay)->outputs[0].target == QStringLiteral("st_lamp")
              && e->entityById(lamp)->name == QStringLiteral("st_lamp"),
          "undo rename restores name and connection in one step");
    check(e->ioLinks().size() >= 1 || !e->showIOLines(), "the relay -> lamp link is drawn");
    // Copy the pair, paste with unique names: the copy targets its own lamp.
    e->setSelected({}, {});
    e->setSelectedEntities({ioRelay, lamp});
    e->copySelection();
    const size_t before = e->scene()->entities.size();
    e->paste(false, true);
    check(e->scene()->entities.size() == before + 2, "paste with unique names adds both");
    const eng::Entity *newRelay = nullptr;
    for (const auto &en: e->scene()->entities)
      if (en.classname == QLatin1String("logic_relay") && en.name.startsWith(QLatin1String("st_relay_")))
        newRelay = &en;
    check(newRelay && newRelay->outputs.size() == 1 && newRelay->outputs[0].target.startsWith(QLatin1String("st_lamp_")),
          "pasted relay targets the pasted lamp");
    e->undo();
    check(e->scene()->entities.size() == before, "undo paste is one step");
    // A broken target shows up in validation.
    c.target = QStringLiteral("nothing_here");
    e->setEntityOutputs(ioRelay, {c});
    bool broken = false;
    for (const auto &is: eng::io::validate(e->scene()->entities))
      broken = broken || (is.entity == ioRelay && is.text.contains(QLatin1String("matches no entity")));
    check(broken, "a broken target is reported");
    c.target = QStringLiteral("st_lamp");
    e->setEntityOutputs(ioRelay, {c});
    // The simulator: Trigger -> the lamp toggles after the delay.
    e->ioSimFire(QStringLiteral("st_relay"), QStringLiteral("Trigger"));
    check(e->ioSimRunning(), "ent_fire starts the simulation");
    e->ioSimStep(1); // Trigger
    e->ioSimStep(1); // Toggle
    check(e->ioSimState(lamp) == QStringLiteral("off"), "simulated lamp toggled off");
    e->ioSimStop();
  }
  size_t entityCount = e->scene()->entities.size();

  // Colour LUTs: template, identity sampling, grading, file round trips, undo.
  {
    const QImage tpl = eng::lut::templateStrip();
    check(tpl.width() == 1089 && tpl.height() == 33, "LUT template is 33^3 (1089x33)");
    const QColor probe(200, 90, 30);
    const QColor same = eng::lut::sample(tpl, probe);
    // Sampled like Godot (no texel-centre remap): off by up to (c - 0.5) / 32.
    check(std::abs(same.red() - 200) <= 3 && std::abs(same.green() - 90) <= 3 && std::abs(same.blue() - 30) <= 3,
          "neutral LUT maps colours (nearly) to themselves");
    check(eng::lut::sample(tpl, Qt::white) == QColor(Qt::white) && eng::lut::sample(tpl, Qt::black) == QColor(Qt::black),
          "neutral LUT keeps black and white");
    eng::LutAdjustments warm;
    warm.temperature = 1.0;
    const QColor graded = eng::lut::sample(eng::lut::applyAdjustments(tpl, warm), QColor(128, 128, 128));
    check(graded.red() > graded.blue() + 10, "warm grading pushes grey towards red");

    eng::ColorLut l{QStringLiteral("Warm"), eng::lut::applyAdjustments(tpl, warm)};
    const QString cube = QDir::tempPath() + QStringLiteral("/editor_selftest.cube");
    const QString png = QDir::tempPath() + QStringLiteral("/editor_selftest_lut.png");
    check(eng::lut::exportFile(l, cube) && eng::lut::exportFile(l, png), "export LUT as .cube and .png");
    const auto fromCube = eng::lut::importFile(cube);
    const auto fromPng = eng::lut::importFile(png);
    check(fromCube && fromCube->strip == l.strip, ".cube round trip is lossless");
    check(fromPng && fromPng->strip == l.strip, ".png round trip is lossless");
    // An 8x8 grid of 64 slices (512x512) imports as a 64^3 strip.
    QImage grid(512, 512, QImage::Format_RGB888);
    const QImage id64 = eng::lut::identityStrip(64);
    for (int s = 0; s < 64; ++s)
      for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
          grid.setPixel((s % 8) * 64 + x, (s / 8) * 64 + y, id64.pixel(s * 64 + x, y));
    const QString gridPath = QDir::tempPath() + QStringLiteral("/editor_selftest_grid.png");
    grid.save(gridPath);
    const auto fromGrid = eng::lut::importFile(gridPath);
    check(fromGrid && fromGrid->size() == 64 && fromGrid->strip == id64, "grid-layout LUT imports as a strip");
    QImage bad(100, 33, QImage::Format_RGB888);
    bad.fill(Qt::red);
    bad.save(png);
    check(!eng::lut::importFile(png).has_value(), "non-LUT image is rejected");

    // The level's LUT is a ref to a server LUT, set as an undo step.
    const QString lutRef = QStringLiteral("ghammer_luts/Siberia");
    e->setColorLut(lutRef);
    check(e->colorLut() == lutRef, "set the level's LUT");
    e->undoStack()->undo();
    check(e->colorLut().isEmpty(), "undo the level's LUT");
    e->undoStack()->redo();
    check(e->colorLut() == lutRef, "redo the level's LUT");
    // A LUT made from adjustments is always the template graded once: grades never stack.
    eng::LutAdjustments a1;
    a1.exposure = 1.0;
    check(eng::lut::graded(QStringLiteral("Sliders"), eng::LutGrade::basic(a1)).strip ==
              eng::lut::applyAdjustments(tpl, a1),
          "a LUT made from adjustments grades the template");
  }

  // Interactive stairs: live preview, step count changes, one undo step.
  {
    const size_t n0 = brushCount(e);
    vk::MarqueeBox m;
    m.plane = 1; // front view: a = X, b = Y, depth = -Z
    m.a0 = 400;
    m.a1 = 425.6;
    m.b0 = 0;
    m.b1 = 12.8;
    m.d0 = -60;
    m.d1 = -40;
    e->setMarquee(m);
    check(e->beginShapeTool(6), "stairs tool opens on a marquee");
    check(e->shapeToolActive() && e->shapeToolBrushCount() == e->stairSteps(), "stairs preview has one brush per step");
    e->setStairSteps(5);
    check(e->shapeToolBrushCount() == 5 && brushCount(e) == n0 + 5, "changing steps rebuilds the preview");
    e->setStairStyle(2);
    check(e->shapeToolBrushCount() == 6, "sloped style adds one support brush");
    e->setStairStyle(0);
    check(std::fabs(e->stairStepHeightSource() - 128.0 / 5.0) < 1e-3, "step rise read-out");
    const int depthBefore = e->undoStack()->undoDepth();
    check(e->commitShapeTool(), "commit stairs");
    check(!e->shapeToolActive() && brushCount(e) == n0 + 5 && !e->hasMarquee(), "stairs created, marquee cleared");
    check(e->undoStack()->undoDepth() == depthBefore + 1, "stairs are one undo step");
    e->undo();
    check(brushCount(e) == n0, "undo stairs");
    e->setMarquee(m);
    e->beginShapeTool(6);
    e->cancelShapeTool();
    check(brushCount(e) == n0 && !e->shapeToolActive(), "cancel removes the preview");

    // Arch: archway in a wall, then carve an opening out of a wall.
    e->setMarquee(m);
    check(e->beginShapeTool(7), "arch tool opens");
    e->setArchMode(1);
    e->setArchSegments(6);
    check(e->shapeToolBrushCount() == 8, "archway = segments + 2 jambs");
    e->commitShapeTool();
    check(brushCount(e) == n0 + 8, "archway created");
    e->createPrimitive(0, {500, 0, 40}, {540, 20, 44});
    const size_t beforeCarve = brushCount(e);
    vk::MarqueeBox wall;
    wall.plane = 1;
    wall.a0 = 510;
    wall.a1 = 530;
    wall.b0 = 0;
    wall.b1 = 14;
    wall.d0 = -45;
    wall.d1 = -39;
    e->setMarquee(wall);
    e->beginShapeTool(7);
    e->setArchMode(2);
    check(e->commitShapeTool(), "carve arch from the selected wall");
    check(brushCount(e) > beforeCarve, "carving splits the wall into pieces");
    e->undo();
    check(brushCount(e) == beforeCarve, "undo carve");
  }

  // Vertex tool: box-select, nudge, undo, edge handles, weld, scale.
  {
    e->setActiveMeshGroup(eng::kWorldspawnGroup);
    e->createPrimitive(0, {600, 0, 0}, {612.8f, 12.8f, 12.8f});
    e->setEditMode(true);
    e->setVertexSubMode(0);
    // Top view plane coords: a = x, b = -z. Box around the corners at x = 612.8.
    const int n = e->selectHandlesInBox(0, {612.0, 1.0}, {613.5, -14.0}, false);
    check(n == 4 && e->selectedVertices().size() == 4, "box-select the 4 corners on one side in the top view");
    const size_t count = brushCount(e);
    e->nudgeVertices({1.6f, 0, 0});
    const auto grown = e->scene()->findById(*e->selectedObjects().begin());
    glm::vec3 lo, hi;
    std::tie(lo, hi) = grown->getWorldAABB();
    check(brushCount(e) == count && std::fabs(hi.x - 614.4f) < 1e-3f, "nudging corners stretches the brush");
    check(e->selectedVertices().size() == 4 && std::fabs(e->selectedVertices()[0].pos.x - 614.4f) < 1e-3f,
          "corner selection follows the move");
    e->undo();
    std::tie(lo, hi) = e->scene()->findById(*e->selectedObjects().begin())->getWorldAABB();
    check(std::fabs(hi.x - 612.8f) < 1e-3f, "undo inside the vertex tool");
    check(e->selectedVertices().size() == 4, "corner selection survives undo");
    e->redo();
    e->undo();
    // Edge handle: pick it in the front view (a = x, b = y); it stands for two corners.
    e->setVertexSubMode(2);
    e->clearVertexSelection();
    check(e->pickEditHandle2D(1, {606.4, 12.8}, 0.5, false), "pick an edge handle in the front view");
    check(e->selectedVertices().size() == 4, "an edge stacked in depth picks both edges behind it (4 corners)");
    e->setVertexSubMode(0);
    e->selectAllVertices();
    check(e->beginVertexScale(), "scale opens with corners selected");
    e->setVertexScale(0.5);
    e->commitVertexScale();
    std::tie(lo, hi) = e->scene()->findById(*e->selectedObjects().begin())->getWorldAABB();
    check(std::fabs((hi.x - lo.x) - 6.4f) < 1e-3f, "scale 0.5 halves the brush");
    e->clearVertexSelection();
    // Weld the 4 corners on the +X side into one: a pyramid-ended brush.
    std::tie(lo, hi) = e->scene()->findById(*e->selectedObjects().begin())->getWorldAABB();
    e->selectHandlesInBox(0, {hi.x - 0.1, -lo.z + 0.1}, {hi.x + 0.1, -hi.z - 0.1}, false);
    check(e->selectedVertices().size() == 4 && e->weldSelectedVertices(), "weld 4 corners");
    const auto welded = eng::ConvexBrush::fromObject(*e->scene()->findById(*e->selectedObjects().begin()));
    check(welded && welded->vertices().size() == 5, "welded brush has 5 corners");
    e->setEditMode(false);
  }

  // Mesh groups: active group, undoable moves, delete sends brushes to worldspawn.
  quint32 corridorGroup = 0;
  {
    check(!e->meshGroups().empty() && e->meshGroups().front().name == QStringLiteral("worldspawn"),
          "worldspawn is the first group");
    e->clearSelection();
    const quint32 gid = e->createMeshGroup(QStringLiteral("corridors"), false, true);
    check(gid != 0 && e->activeMeshGroup() == gid, "new group becomes active");
    e->createPrimitive(0, {300, 0, 0}, {312.8f, 12.8f, 12.8f});
    const quint32 inGroup = *e->selectedObjects().begin();
    check(e->scene()->findById(inGroup)->group() == gid, "new brush lands in the active group");
    e->moveSelectionToGroup(eng::kWorldspawnGroup);
    check(e->scene()->findById(inGroup)->group() == eng::kWorldspawnGroup, "move to worldspawn");
    e->undo();
    check(e->scene()->findById(inGroup)->group() == gid, "undo group move");
    const quint32 tmp = e->createMeshGroup(QStringLiteral("temp"), true, false);
    check(e->scene()->findById(inGroup)->group() == tmp && e->activeMeshGroup() == gid,
          "group from selection, active unchanged");
    e->deleteMeshGroup(tmp, false);
    check(e->scene()->findById(inGroup)->group() == eng::kWorldspawnGroup && !e->scene()->findGroup(tmp),
          "deleting a group moves brushes to worldspawn");
    e->undo();
    check(e->scene()->findGroup(tmp) && e->scene()->findById(inGroup)->group() == tmp, "undo group delete");
    e->moveSelectionToGroup(gid);
    e->setMeshGroupHidden(gid, true);
    check(e->scene()->findById(inGroup)->hidden(), "hiding a group hides its brushes");
    e->undo();
    check(!e->scene()->findById(inGroup)->hidden(), "undo group hide");
    const size_t n = brushCount(e);
    e->deleteMeshGroup(tmp, true);
    e->undo();
    check(brushCount(e) == n && e->scene()->findGroup(tmp), "delete group + brushes undoes in one step");
    corridorGroup = gid;
    e->setActiveMeshGroup(eng::kWorldspawnGroup);
  }

  // Face extrude, merge refusal, shear.
  {
    e->createPrimitive(0, {800, 0, 0}, {812.8f, 12.8f, 12.8f});
    const quint32 cube = *e->selectedObjects().begin();
    // Pick the +X face the way Material mode does: its picking id.
    const auto cb = eng::ConvexBrush::fromObject(*e->scene()->findById(cube));
    int px = -1;
    for (int f = 0; f < int(cb->faces.size()); ++f)
      if (cb->faces[size_t(f)].normal.x > 0.9)
        px = f;
    e->setSelected({}, {eng::makePickingId(cube, uint32_t(px))});
    check(e->selectedBrushFaces().size() == 1, "a picked face resolves to its brush face");
    check(e->extrudeSelectedFaces(3.2, false) == 1, "extrude a face");
    glm::vec3 lo, hi;
    const quint32 grown = eng::stableIdFromPicking(*e->selectedFaces().begin());
    std::tie(lo, hi) = e->scene()->findById(grown)->getWorldAABB();
    check(std::fabs(hi.x - 816.0f) < 1e-3f && !e->selectedFaces().empty(), "extruded face moved out, still picked");
    const size_t n0 = brushCount(e);
    check(e->extrudeSelectedFaces(1.6, true) == 1 && brushCount(e) == n0 + 1, "extrude to a new brush");
    e->undo();
    e->undo();
    // Two separated boxes: merge refuses, force merges.
    e->createPrimitive(0, {830, 0, 0}, {832, 2, 2});
    const quint32 m1 = *e->selectedObjects().begin();
    e->createPrimitive(0, {840, 0, 0}, {842, 2, 2});
    const quint32 m2 = *e->selectedObjects().begin();
    e->setSelected({m1, m2}, {});
    check(!e->csgMerge() && e->lastMergeWouldFillGaps(), "merge refuses to fill gaps");
    check(e->csgMerge(true), "merge anyway");
    check(e->shearSelection(0, 1, 0.5), "shear");
    const auto sheared = eng::ConvexBrush::fromObject(*e->scene()->findById(*e->selectedObjects().begin()));
    check(sheared && sheared->valid(), "sheared brush is valid");
  }

  // Texture tools: justify, wrap paste across an edge, UV edit session.
  {
    e->createPrimitive(0, {900, 0, 0}, {912.8f, 12.8f, 12.8f});
    const quint32 box = *e->selectedObjects().begin();
    const auto mesh = e->scene()->findById(box)->getMesh();
    int top = -1, side = -1;
    // Use the world planes: the top face is the one whose polygon sits at y = 12.8.
    auto facesAt = [&](auto pred) {
      for (int f = 0; f < mesh->faceCount(); ++f) {
        std::vector<glm::vec3> q;
        mesh->getFacePolygon(f, q);
        bool all = true;
        for (const auto &p: q)
          all = all && pred(p);
        if (all)
          return f;
      }
      return -1;
    };
    top = facesAt([](const glm::vec3 &p) { return std::fabs(p.y - 12.8f) < 1e-3f; });
    side = facesAt([](const glm::vec3 &p) { return std::fabs(p.x - 912.8f) < 1e-3f; });
    check(top >= 0 && side >= 0, "find the top and +X faces");
    const quint32 topId = mesh->getFacePickingID(top), sideId = mesh->getFacePickingID(side);
    e->setSelected({}, {topId});
    e->rotateFaceTextures(30.0);
    e->justifyFaceTextures(0);
    {
      const auto b = mesh->faceTexBasis(top);
      std::vector<glm::vec3> q;
      mesh->getFacePolygon(top, q);
      float u0 = 1e9f;
      for (const auto &p: q)
        u0 = std::min(u0, b.uvAt(p).x);
      check(std::fabs(u0 - std::round(u0)) < 1e-3f, "justify left puts the face's left edge on a tile edge");
    }
    check(e->copyFaceAttributes(), "copy face attributes");
    e->setSelected({}, {sideId});
    e->pasteFaceAttributes(2);
    {
      const auto bt = mesh->faceTexBasis(top), bs = mesh->faceTexBasis(side);
      // A point on the shared edge (x = 912.8, y = 12.8).
      const glm::vec3 p(912.8f, 12.8f, 5.0f);
      const glm::vec2 d = bt.uvAt(p) - bs.uvAt(p);
      check(std::fabs(d.x - std::round(d.x)) < 1e-3f && std::fabs(d.y - std::round(d.y)) < 1e-3f,
            "wrap paste is seamless along the shared edge");
      const glm::vec3 q(912.8f, 12.8f, 11.0f);
      const glm::vec2 d2 = bt.uvAt(q) - bs.uvAt(q);
      check(std::fabs(d2.x - d.x) < 1e-3f && std::fabs(d2.y - d.y) < 1e-3f, "wrap keeps the texture continuous along the edge");
    }
    const int depth = e->undoStack()->undoDepth();
    e->beginFaceEdit();
    e->updateFaceEdit([](const std::vector<glm::vec3> &, const glm::vec3 &, eng::FaceTexBasis &base,
                         eng::FaceTexBasis &out) { out.offsetU = base.offsetU + 0.25f; });
    e->updateFaceEdit([](const std::vector<glm::vec3> &, const glm::vec3 &, eng::FaceTexBasis &base,
                         eng::FaceTexBasis &out) { out.offsetU = base.offsetU + 0.5f; });
    e->endFaceEdit(QStringLiteral("Move texture"));
    check(e->undoStack()->undoDepth() == depth + 1, "a UV-editor drag is one undo step");
  }

  // Brush entities: tie, pick as one, untie, undo, save / reload.
  quint32 doorEntity = 0;
  {
    e->createPrimitive(0, {700, 0, 0}, {712.8f, 25.6f, 1.6f});
    const quint32 a = *e->selectedObjects().begin();
    e->createPrimitive(0, {712.8f, 0, 0}, {725.6f, 25.6f, 1.6f});
    const quint32 b = *e->selectedObjects().begin();
    e->setSelected({a, b}, {});
    doorEntity = e->makeBrushEntity(QStringLiteral("func_door"));
    check(doorEntity != 0 && e->brushesOfEntity(doorEntity).size() == 2, "make brush entity ties both brushes");
    const eng::Entity *door = e->entityById(doorEntity);
    check(door && std::fabs(door->origin.x - 712.8f) < 1e-3f, "brush entity origin is the centre of its brushes");
    e->clearSelection();
    e->selectObject(a, false);
    check(e->selectedObjects().size() == 2 && e->selectedEntities().count(doorEntity), "clicking one brush selects the entity's brushes");
    e->setSelected({a}, {});
    check(e->moveSelectionToWorld() && e->brushesOfEntity(doorEntity).size() == 1, "move one brush back to the world");
    e->undo();
    check(e->brushesOfEntity(doorEntity).size() == 2, "undo move to world");
    e->setSelected({a, b}, {});
    e->csgHollow(0.4);
    check(e->brushesOfEntity(doorEntity).size() > 2, "CSG results stay in the brush entity");
    e->undo();
  }

  // Layout Designer: corridor + room + vent into a new mesh group, one undo step.
  int layoutId = 0;
  {
    auto *designer = ui::LayoutDesigner::open(e, w->editorCtx(), w, 0);
    ui::LayoutModel *lm = designer->model();
    lm->newLayout();
    lm->setCfg(QStringLiteral("originX"), 0);
    lm->setCfg(QStringLiteral("originZ"), 3000);
    lm->addPath(eng::layout::Corridor, {QPoint(0, 0), QPoint(320, 0), QPoint(320, 256)});
    lm->addRoomRect(QRect(224, 256, 224, 192), false, eng::layout::ContactOpen);
    lm->setTool(ui::LayoutModel::ToolVent);
    lm->addPath(eng::layout::Vent, {QPoint(448, 352), QPoint(640, 352)});
    lm->setTool(ui::LayoutModel::ToolSelect);
    lm->selectAt({160.0, 0.0}, false, false);
    check(lm->selCount() == 1 && lm->selWidth() == 128, "designer: select a corridor segment");
    lm->setSelWidth(160);
    check(lm->selWidth() == 160, "designer: per-segment width");
    lm->undo();
    check(lm->doc().paths[0].segs[0].width == 128, "designer: undo");
    lm->redo();
    lm->setSelLength(448);
    check(eng::layout::segmentLength(lm->doc().paths[0], 0) == 448, "designer: segment length edit");
    lm->undo();
    lm->setCfg(QStringLiteral("target"), 1);
    lm->setCfg(QStringLiteral("groupName"), QStringLiteral("corridors_test"));
    const size_t before = brushCount(e);
    const int depth = e->undoStack()->undoDepth();
    lm->generate();
    layoutId = lm->doc().id;
    const size_t made = brushCount(e) - before;
    check(made > 10 && made < 120, qPrintable(QStringLiteral("layout generates a sane brush count (%1)").arg(made)));
    check(e->undoStack()->undoDepth() == depth + 1, "layout generation is one undo step");
    quint32 gid = 0;
    for (const auto &g: e->meshGroups())
      if (g.name == QStringLiteral("corridors_test"))
        gid = g.id;
    check(gid != 0 && e->meshGroupCounts().value(gid) == int(made), "layout brushes land in the new mesh group");
    lm->generate(); // regenerate replaces, does not duplicate
    check(brushCount(e) - before == made, "regenerating replaces the layout's brushes");
    e->undo();
    check(brushCount(e) - before == made, "undo regeneration restores the previous generation");
    e->undo();
    check(brushCount(e) == before, "undo the first generation removes the brushes");
    e->redo();
    check(brushCount(e) - before == made && !e->layoutDocs().isEmpty(), "redo generation");
    designer->hide();
  }

  // Save / reload round trip keeps the n-gon brushes
  entityCount = e->scene()->entities.size();
  e->syncPropObjects(true);
  size_t total = 0;
  for (const auto &o: e->scene()->gameObjects)
    total += o->isDerived() ? 0 : 1;
  const QString path = QDir::tempPath() + QStringLiteral("/editor_selftest.ghmap");
  check(w->saveMapTo(path), "save");
  check(w->loadMapFrom(path), "reload");
  e->syncPropObjects(true);
  size_t reloaded = 0;
  for (const auto &o: e->scene()->gameObjects)
    reloaded += o->isDerived() ? 0 : 1;
  check(reloaded == total, "same brush count after reload (props not saved as meshes)");
  bool cylFaces = false;
  for (const auto &o: e->scene()->gameObjects)
    cylFaces = cylFaces || o->getMesh()->faceCount() == prefs::i("Brush/CylinderSides") + 2;
  check(cylFaces, "cylinder survives reload with all faces");
  check(e->scene()->entities.size() == entityCount,
        qPrintable(QStringLiteral("entities survive reload (%1 of %2)").arg(e->scene()->entities.size()).arg(entityCount)));
  check(e->colorLut() == QStringLiteral("ghammer_luts/Siberia"), "the level's LUT survives reload");
  {
    const eng::MeshGroup *g = e->scene()->findGroup(corridorGroup);
    check(g && g->name == QStringLiteral("corridors") && e->meshGroupCounts().value(corridorGroup) == 1,
          "mesh groups and membership survive reload");
    check(!e->layoutDoc(layoutId).isEmpty(), "layouts survive reload (LAYO)");
    check(e->brushesOfEntity(doorEntity).size() == 2, "brush entity links survive reload");
    const eng::Entity *relay = e->entityById(ioRelay);
    check(relay && relay->outputs.size() == 1 && relay->outputs[0].input == QStringLiteral("Toggle")
              && std::fabs(relay->outputs[0].delay - 0.5f) < 1e-6f,
          "I/O connections survive reload (IOCN)");
  }

  // World environment: one per map, survives save/reload, undeletable, picked by its billboard.
  {
    int worlds = 0;
    for (const auto &en: e->scene()->entities)
      worlds += en.classname == eng::kWorldEnvClass ? 1 : 0;
    check(worlds == 1, "exactly one world_env after reload");
    const eng::Entity *wenv = e->worldEnvEntity();
    check(wenv != nullptr, "world_env exists");
    if (wenv) {
      const quint32 wid = wenv->id;
      e->deleteEntities({wid});
      check(e->worldEnvEntity() && e->worldEnvEntity()->id == wid, "world_env cannot be deleted");
      const glm::vec3 at = e->worldEnvEntity()->origin;
      e->camera()->setPose(at + glm::vec3(0.0f, 0.0f, 30.0f), 90.0f, 0.0f);
      e->refreshViewUniforms();
      const glm::vec2 px = e->screenPosOf(at);
      const float vw = float(e->viewPixelSize().width()), vh = float(e->viewPixelSize().height());
      e->clearSelection();
      e->pick(px.x + e->entityIconHalfPx() * 0.8f, px.y, vw, vh, false, false);
      check(e->worldEnvSelected(), "clicking the world_env icon selects it");
      e->clearSelection();
      e->pick(px.x + e->entityIconHalfPx() * 3.0f, px.y, vw, vh, false, false);
      check(!e->worldEnvSelected(), "a click beside the icon does not");
    }
  }

  // The Add menu places + selects lights.
  for (const QString cls: {QStringLiteral("light"), QStringLiteral("light_spot")}) {
    const size_t before = e->scene()->entities.size();
    w->addEntityFromMenu(cls);
    const bool added = e->scene()->entities.size() == before + 1 && e->scene()->entities.back().classname == cls;
    check(added, qPrintable(QStringLiteral("Add menu creates a %1").arg(cls)));
    check(added && e->selectedEntities().count(e->scene()->entities.back().id) == 1,
          qPrintable(QStringLiteral("new %1 is selected").arg(cls)));
  }

  // Asset server: only with EDITOR_SERVER_URL.
  if (!qEnvironmentVariable("EDITOR_SERVER_URL").isEmpty()) {
    auto &client = net::AssetClient::instance();
    if (!waitUntil([&] { return client.isOnline(); }, 8000)) {
      qInfo().noquote() << "[selftest] SKIP asset server: not online";
    } else {
      using matprops::Key;
      auto &tm = vk::TextureManager::instance();
      QString ref;
      QJsonObject q;
      q.insert("limit", 1);
      q.insert("q", "asphalt");
      net::TextureCatalog::instance().query(q, qApp, [&](const net::QueryPage &p) {
        if (!p.items.isEmpty())
          ref = p.items.first().ref;
      });
      check(waitUntil([&] { return !ref.isEmpty(); }, 5000), "server: catalogue query");
      {  // start from the texture's defaults (no server overrides)
        bool done = false;
        net::TextureCatalog::instance().call("props.clear", QJsonObject{{"ref", ref}}, qApp,
                                             [&](const net::Result &) { done = true; });
        waitUntil([&] { return done; }, 5000);
        waitUntil([&] {
          const TextureEntry *te = net::TextureCatalog::instance().find(ref);
          return te && te->props.isEmpty();
        }, 3000);
      }
      e->setMaterialMode(false);
      e->createPrimitive(0, {500, 0, 0}, {525.6f, 25.6f, 25.6f});
      const quint32 boxId = *e->selectedObjects().begin();
      e->applyMaterialToSelection(ref);
      const auto mat = tm.materialFor(ref);
      check(mat->kind == eng::Material::Kind::Server && mat->hasEntry(), "server: material resolved");
      check(mat->texWidth == mat->entry.w && mat->texWidth > 0, "server: UV scale uses the source size");
      check(waitUntil([&] { return mat->state == eng::Material::State::Ready; }, 15000),
            "server: editor maps downloaded and uploaded");
      check(mat->realNormal && mat->realOrm == (mat->entry.layout() != MapLayout::None),
            "server: normal + ORM maps bound");

      // scenario 1: tag / map defaults
      const eng::ResolvedProps r0 = tm.resolvedFor(*mat);
      check(r0.source[size_t(Key::Roughness)] == eng::PropSource::Default && mat->props.roughness == 1.0f,
            "server: roughness map -> default roughness 1.0");

      // scenario 2: map override, undo, save/load
      e->setMaterialProperty(ref, Key::Metallic, 0.8);
      e->setMaterialProperty(ref, Key::Metallic, 0.7);  // a slider drag: one undo step
      check(mat->props.metallic == 0.7f && tm.resolvedFor(*mat).source[size_t(Key::Metallic)] == eng::PropSource::Map,
            "server: map override applies");
      e->undo();
      check(mat->props.metallic == r0.props.metallic, "server: undo map override (drag was one step)");
      e->redo();
      check(mat->props.metallic == 0.7f, "server: redo map override");

      // scenario 3: a variant for one face
      const auto box = e->scene()->findById(boxId);
      const quint32 facePick = box->getMesh()->getFacePickingID(0);
      e->setMaterialMode(true);
      e->setSelected({}, {facePick});
      const QString vref = e->createVariantForSelection(ref, "shiny");
      e->setMaterialProperty(vref, Key::Roughness, 0.15);
      const auto vmat = tm.materialFor(vref);
      check(box->getMesh()->faceMaterialRef(0) == vmat && box->getMesh()->faceMaterialRef(1) == mat,
            "server: variant on the selected face only");
      check(vmat->props.roughness == 0.15f && vmat->props.metallic == 0.7f && mat->props.roughness == 1.0f,
            "server: variant inherits the map layer, base unchanged");
      waitUntil([&] { return vmat->albedo == mat->albedo && mat->albedo; }, 3000);
      check(vmat->albedo == mat->albedo, "server: variant shares the base's textures");

      QTemporaryDir dir;
      const QString path = dir.filePath("server_materials.ghmap");
      check(w->saveMapTo(path), "server: save map with overrides");
      check(w->loadMapFrom(path), "server: reload map");
      const auto *ov = &e->scene()->materialOverrides;
      check(ov->mapLayer(ref) && ov->mapLayer(ref)->metallic == 0.7f && ov->variant(vref) &&
                ov->variant(vref)->props.roughness == 0.15f && !ov->idFor(ref).isEmpty(),
            "server: overrides and texture id survive save/load");
      const auto reloadedVariant = tm.materialFor(vref);
      check(reloadedVariant->props.roughness == 0.15f, "server: variant props after reload");

      // save the map layer to the server; the event updates the entry
      bool saved = false, answered = false;
      e->saveMaterialPropsToServer(ref, false, [&](bool ok, const QString &, const QJsonObject &) {
        saved = ok;
        answered = true;
      });
      check(waitUntil([&] { return answered; }, 5000) && saved, "server: save props to the server");
      check(waitUntil([&] { return tm.materialFor(ref)->entry.props.isSet(Key::Metallic); }, 5000) &&
                !e->scene()->materialOverrides.mapLayer(ref),
            "server: server layer updated, map layer cleared");
      check(tm.materialFor(ref)->props.metallic == 0.7f &&
                tm.resolvedFor(*tm.materialFor(ref)).source[size_t(Key::Metallic)] == eng::PropSource::Server,
            "server: value now comes from the server");
      // EDITOR_SELFTEST_MAPOUT=<file.ghmap>: box A with the base material, box B with a metallic variant.
      if (const QString out = qEnvironmentVariable("EDITOR_SELFTEST_MAPOUT"); !out.isEmpty()) {
        e->markClean();  // newScene() would otherwise stop at the "discard changes?" prompt
        w->newScene();
        e->setMaterialMode(false);
        e->createPrimitive(0, {0, 0, 0}, {25.6f, 25.6f, 25.6f});
        e->applyMaterialToSelection(ref);
        e->createPrimitive(0, {38.4f, 0, 0}, {64.0f, 25.6f, 25.6f});
        const quint32 bId = *e->selectedObjects().begin();
        e->applyMaterialToSelection(ref);
        std::vector<quint32> faces;
        const auto b = e->scene()->findById(bId);
        for (int f = 0; f < b->getMesh()->faceCount(); ++f)
          faces.push_back(b->getMesh()->getFacePickingID(f));
        e->setMaterialMode(true);
        e->setSelected({}, faces);
        const QString metal = e->createVariantForSelection(ref, "metal");
        e->setMaterialProperty(metal, Key::Metallic, 1.0);
        e->setMaterialProperty(metal, Key::Roughness, 0.2);
        e->setMaterialProperty(metal, Key::EmissionEnabled, true);
        e->setMaterialProperty(metal, Key::EmissionColor, QColor(40, 0, 0));
        e->setMaterialMode(false);
        check(w->saveMapTo(out), "server: comparison map written");
      }
      // leave the shared test data as it was
      bool cleared = false;
      net::TextureCatalog::instance().call("props.clear", QJsonObject{{"ref", ref}}, qApp,
                                           [&](const net::Result &) { cleared = true; });
      waitUntil([&] { return cleared; }, 5000);
      e->setMaterialMode(false);
    }
  }

  // Godot export: what leaves the level, then a real export into a temp project.
  {
    eng::Scene s;
    const auto brick = QSharedPointer<eng::Material>::create(QStringLiteral("selftest/brick"));
    const auto box = [&](glm::dvec3 mn, glm::dvec3 mx, quint32 id) {
      auto o = std::make_shared<eng::GameObject>(eng::ConvexBrush::box(mn, mx, brick, 1.0).toMesh(), id);
      s.gameObjects.push_back(o);
      return o;
    };
    box({0, 0, 0}, {10, 10, 10}, 101);
    box({10, 0, 0}, {20, 10, 10}, 102);  // touches 101: two faces nobody sees
    auto detail = box({-10, 0, 0}, {0, 10, 10}, 103);
    eng::Entity d;
    d.id = eng::kEntityIdBase + 900;
    d.classname = QStringLiteral("func_detail");
    s.entities.push_back(d);
    detail->setBrushEntity(d.id);  // touches 101 but hides nothing and loses nothing
    auto dev = box({0, 20, 0}, {10, 30, 10}, 104);
    dev->getMesh()->setFaceMaterial(0, QSharedPointer<eng::Material>::create(QStringLiteral("dev/dev_measurewall01d")));
    eng::godot::LevelOptions lo;
    lo.mapName = QStringLiteral("Self Test");
    const auto plan = eng::godot::planLevel(s, lo);
    check(plan.hiddenFaces == 2, qPrintable(QStringLiteral("godot: touching world faces removed (%1)").arg(plan.hiddenFaces)));
    check(plan.devFaces == 1, "godot: dev-textured face removed");
    check(plan.faces == 6 * 4 - 2 - 1, qPrintable(QStringLiteral("godot: face count (%1)").arg(plan.faces)));
    check(plan.glbs.size() == 2 && plan.levelRel == QStringLiteral("levels/self_test"), "godot: worldspawn + func_detail glbs");
    bool detailWhole = false;
    for (const auto &g: plan.glbs)
      detailWhole = detailWhole || (g.entity == d.id && g.faces == 6);
    check(detailWhole, "godot: func_detail keeps all its faces");

    // Collision: three floor brushes with three textures merge into one box (12 triangles).
    {
      eng::Scene floor;
      for (int k = 0; k < 3; ++k) {
        const auto mat = QSharedPointer<eng::Material>::create(QStringLiteral("selftest/floor%1").arg(k));
        floor.gameObjects.push_back(std::make_shared<eng::GameObject>(
            eng::ConvexBrush::box({10.0 * k, -1, 0}, {10.0 * (k + 1), 0, 10}, mat, 1.0).toMesh(), quint32(301 + k)));
      }
      const auto pf = eng::godot::planLevel(floor, lo);
      const int tris = pf.glbs.empty() ? -1 : pf.glbs[0].collisionTriangles;
      check(pf.hiddenFaces == 4 && tris == 12,
            qPrintable(QStringLiteral("godot: floor collision merged (%1 triangles, %2 hidden faces)").arg(tris).arg(pf.hiddenFaces)));
      QJsonObject json;
      bool colonly = false;
      if (!pf.glbs.empty() && eng::gltf::readGlb(pf.glbs[0].bytes, &json, nullptr))
        for (const auto &nv: json.value(QStringLiteral("nodes")).toArray())
          colonly = colonly || nv.toObject().value(QStringLiteral("name")).toString() == QStringLiteral("worldspawn_collision-colonly");
      check(colonly, "godot: worldspawn_collision-colonly node");
    }

    // Exported faces are CCW seen from outside, normals outward (glTF / Godot front faces).
    {
      eng::Scene one;
      one.gameObjects.push_back(std::make_shared<eng::GameObject>(
          eng::ConvexBrush::box({0, 0, 0}, {10, 10, 10}, brick, 1.0).toMesh(), 201));
      const auto p1 = eng::godot::planLevel(one, lo);
      QJsonObject json;
      QByteArray bin;
      bool outward = !p1.glbs.empty() && eng::gltf::readGlb(p1.glbs[0].bytes, &json, &bin);
      const auto accessors = json.value(QStringLiteral("accessors")).toArray();
      const auto views = json.value(QStringLiteral("bufferViews")).toArray();
      const auto data = [&](int acc) {
        const auto v = views.at(accessors.at(acc).toObject().value(QStringLiteral("bufferView")).toInt()).toObject();
        return bin.constData() + v.value(QStringLiteral("byteOffset")).toInt();
      };
      int tris = 0;
      for (const auto &mv: json.value(QStringLiteral("meshes")).toArray())
        for (const auto &pv: mv.toObject().value(QStringLiteral("primitives")).toArray()) {
          const auto pr = pv.toObject();
          const auto *pos = reinterpret_cast<const glm::vec3 *>(
              data(pr.value(QStringLiteral("attributes")).toObject().value(QStringLiteral("POSITION")).toInt()));
          const auto *nrm = reinterpret_cast<const glm::vec3 *>(
              data(pr.value(QStringLiteral("attributes")).toObject().value(QStringLiteral("NORMAL")).toInt()));
          const int ia = pr.value(QStringLiteral("indices")).toInt();
          const auto *idx = reinterpret_cast<const quint32 *>(data(ia));
          const int count = accessors.at(ia).toObject().value(QStringLiteral("count")).toInt();
          const glm::vec3 centre(5.0f * float(eng::kMetersPerWorldUnit));
          for (int t = 0; t + 2 < count; t += 3, ++tris) {
            const glm::vec3 a = pos[idx[t]], b = pos[idx[t + 1]], c = pos[idx[t + 2]];
            const glm::vec3 wind = glm::cross(b - a, c - a);
            outward = outward && glm::dot(wind, (a + b + c) / 3.0f - centre) > 0.0f && glm::dot(wind, nrm[idx[t]]) > 0.0f;
          }
        }
      check(outward && tris == 24, qPrintable(QStringLiteral("godot: faces wound outward, mesh + collision (%1 triangles)").arg(tris)));
    }

    QTemporaryDir proj;
    QFile pg(proj.filePath(QStringLiteral("project.godot")));
    pg.open(QIODevice::WriteOnly);
    pg.write("config_version=5\n\n[application]\n\nconfig/features=PackedStringArray(\"4.8\", \"Forward Plus\")\n");
    pg.close();
    check(eng::godot::checkProject(proj.path()).isEmpty(), "godot: 4.8 project accepted");
    QTemporaryDir old;
    QFile og(old.filePath(QStringLiteral("project.godot")));
    og.open(QIODevice::WriteOnly);
    og.write("config_version=5\n\n[application]\n\nconfig/features=PackedStringArray(\"4.3\")\n");
    og.close();
    check(!eng::godot::checkProject(old.path()).isEmpty(), "godot: 4.3 project refused");

    const auto runExport = [&](QString *summary) {
      bool finished = false, ok = false;
      w->exportGodotTo(proj.path(), [&](const QString &text, bool good) {
        *summary = text;
        ok = good;
        finished = true;
      });
      waitUntil([&] { return finished; }, 20000);
      return finished && ok;
    };
    // The map is all dev texture by now: give one world brush a real material.
    for (const auto &o: e->scene()->gameObjects)
      if (o && o->getMesh() && !o->hidden() && !o->brushEntity() && !o->ownerEntity() && !o->procObject()) {
        for (int f = 0; f < o->getMesh()->faceCount(); ++f)
          o->getMesh()->setFaceMaterial(f, brick);
        break;
      }
    QString first, second;
    const bool ran = runExport(&first);
    check(ran, qPrintable(QStringLiteral("godot: export ran (%1)").arg(first)));
    const QString name = w->currentMapPath().isEmpty() ? QStringLiteral("untitled")
                                                       : eng::io::snake(QFileInfo(w->currentMapPath()).completeBaseName());
    const QString tscn = proj.filePath(QStringLiteral("levels/%1/%1.tscn").arg(name));
    check(QFileInfo::exists(tscn), qPrintable(QStringLiteral("godot: %1 written").arg(tscn)));
    check(QFileInfo::exists(proj.filePath(QStringLiteral("levels/%1/worldspawn.glb").arg(name))), "godot: worldspawn.glb written");
    const bool ran2 = runExport(&second);
    check(ran2 && second.contains(QStringLiteral(": 0 written")),
          qPrintable(QStringLiteral("godot: second export writes nothing (%1)").arg(second)));
  }


  // Projects: two level tabs keep their own state; Save writes / exports only what changed; tabs are restored.
  {
    QTemporaryDir tmp;
    QString err;
    const QString root = proj::ProjectManager::createProject(tmp.path(), QStringLiteral("Selftest Game"), &err);
    check(!root.isEmpty(), qPrintable(QStringLiteral("project: created (%1)").arg(err)));
    check(w->switchToProject(root), "project: opened");
    proj::DocumentManager *docs = w->documents();
    const proj::Project *project = proj::currentProject();
    check(project && docs->count() == 0, "project: opens with no tabs");
    check(project && eng::godot::checkProject(project->gameDir()).isEmpty(), "project: game/ is a Godot project");
    {
      // Long paths lose folders from the middle.
      const QFontMetrics fm(qApp->font());
      const QString longPath = QStringLiteral("/some/long/dir/the/end/of/that/very/long/path/to/my_project");
      const int width = fm.horizontalAdvance(QStringLiteral("/some/long/dir/…/to/my_project")) + 2;
      const QString shown = ui::elidePath(longPath, fm, width);
      check(shown.startsWith(QStringLiteral("/some/")) && shown.endsWith(QStringLiteral("/my_project"))
                && shown.contains(QStringLiteral("/…/")) && fm.horizontalAdvance(shown) <= width,
            qPrintable(QStringLiteral("project list: path shortened in the middle (%1)").arg(shown)));
      check(ui::elidePath(longPath, fm, 100000) == longPath, "project list: a path that fits stays whole");
    }
    if (project) {
      // First open: project row expanded, its folders closed.
      ui::ProjectPanel *panel = w->projectPanel();
      {
        // Default dock arrangement on a first start.
        const auto dock = [w](const char *name) { return w->findChild<QDockWidget *>(QString::fromLatin1(name)); };
        QDockWidget *project = dock("dock.project"), *outliner = dock("dock.outliner"), *materials = dock("dock.materials"),
                    *console = dock("dock.console"), *inspector = dock("dock.inspector"),
                    *autosel = dock("dock.autoselect");
        if (project && outliner && materials && console && inspector && autosel) {
          check(w->dockWidgetArea(project) == Qt::LeftDockWidgetArea && w->dockWidgetArea(outliner) == Qt::LeftDockWidgetArea
                    && project->isVisible() && outliner->isVisible() && project->y() < outliner->y(),
                "layout: Project over Outliner, left of the views");
          check(std::abs(project->height() - outliner->height()) <= 12,
                qPrintable(QStringLiteral("layout: left column split in half (%1 / %2)")
                               .arg(project->height()).arg(outliner->height())));
          check(w->dockWidgetArea(materials) == Qt::BottomDockWidgetArea && materials->x() < console->x()
                    && std::abs(materials->width() - console->width()) <= 12,
                qPrintable(QStringLiteral("layout: Materials | Console at the bottom, half each (%1 / %2)")
                               .arg(materials->width()).arg(console->width())));
          check(autosel->height() < inspector->height(),
                qPrintable(QStringLiteral("layout: Auto-select under the Inspector, smaller (%1 / %2)")
                               .arg(autosel->height()).arg(inspector->height())));
        }
        check(ui::act(QStringLiteral("view.panel.project"))->shortcut() == QKeySequence(QStringLiteral("Alt+1"))
                  && ui::act(QStringLiteral("view.panel.outliner"))->shortcut() == QKeySequence(QStringLiteral("Alt+2")),
              "layout: Alt+1 / Alt+2 toggle Project / Outliner");
      }
      const auto listsFolders = [&] {
        const QStringList shown = panel->listedPaths();
        if (!shown.contains(project->root()))
          return false;
        for (const QString &f: {QStringLiteral("levels"), QStringLiteral("game"), QStringLiteral("images")})
          if (!shown.contains(project->path(f)))
            return false;
        return true;
      };
      const bool listed = waitUntil(listsFolders, 4000);
      check(listed, qPrintable(QStringLiteral("project panel: lists the project's folders (%1 rows)")
                                   .arg(panel->listedPaths().size())));
      check(panel->expandedFolders() == QStringList {QStringLiteral(".")}, "project panel: its folders stay collapsed");
    }
    if (project) {
      const QString pathA = QDir(project->levelsDir()).filePath(QStringLiteral("a.ghmap"));
      const QString pathB = QDir(project->levelsDir()).filePath(QStringLiteral("b.ghmap"));
      proj::Document *a = docs->newLevel(pathA);
      check(a && w->saveLevel(a, false) && QFileInfo::exists(pathA), "levels: A saved");
      e->createPrimitive(0, {0, 0, 0}, {25.6f, 12.8f, 25.6f});
      const quint32 boxA = *e->selectedObjects().begin();
      // A real material, so the export has faces (and collision) to write.
      if (const auto box = e->scene()->findById(boxA); box && box->getMesh()) {
        const auto brick = QSharedPointer<eng::Material>::create(QStringLiteral("selftest/brick"));
        for (int f = 0; f < box->getMesh()->faceCount(); ++f)
          box->getMesh()->setFaceMaterial(f, brick);
      }
      check(e->copySelection(), "levels: copy in A");
      e->camera()->setPose(glm::vec3(11, 22, 33), 40.0f, -10.0f);
      const size_t brushesA = brushCount(e);
      const int depthA = e->undoStack()->undoDepth();
      check(e->dirty() && docs->dirty(a), "levels: A has unsaved changes");
      check(w->windowTitle() == QStringLiteral("gHammer " GHAMMER_VERSION " - [Selftest Game/a[*]]") && w->isWindowModified(),
            qPrintable(QStringLiteral("title: version, project, level, unsaved (%1)").arg(w->windowTitle())));

      proj::Document *b = docs->newLevel(pathB);
      check(b && w->saveLevel(b, false), "levels: B saved");
      check(brushCount(e) == 0 && e->selectedObjects().empty() && !e->dirty() && e->undoStack()->undoDepth() == 0,
            "levels: B starts empty and clean, with its own history");
      check(docs->dirty(a) && !docs->dirty(b), "levels: A stays unsaved while parked");
      check(e->paste(false) && brushCount(e) == 1, "levels: paste in B what was copied in A");
      check(w->saveLevel(b, false) && !docs->dirty(b), "levels: B saved again");
      check(w->windowTitle() == QStringLiteral("gHammer " GHAMMER_VERSION " - [Selftest Game/b[*]]") && !w->isWindowModified(),
            qPrintable(QStringLiteral("title: a saved level has no * (%1)").arg(w->windowTitle())));
      const QDateTime savedB = QFileInfo(pathB).lastModified();

      check(docs->activate(docs->indexOf(a)), "levels: back to A");
      check(brushCount(e) == brushesA && e->undoStack()->undoDepth() == depthA, "levels: A's brushes and history");
      check(e->selectedObjects().size() == 1 && *e->selectedObjects().begin() == boxA, "levels: A's selection");
      check(glm::distance(e->camera()->getPosition(), glm::vec3(11, 22, 33)) < 1e-3f, "levels: A's camera");
      check(docs->dirty(a), "levels: A still unsaved");

      // A level setting: an undo step of that level.
      prefs::get().setValue(QStringLiteral("Export/Collision"), false);
      check(!prefs::b("Export/Collision") && e->levelSetting(QStringLiteral("Export/Collision")).isValid(),
            "level settings: Export/Collision off");
      docs->activate(docs->indexOf(b));
      check(prefs::b("Export/Collision"), "level settings: B has its own");
      docs->activate(docs->indexOf(a));
      check(!prefs::b("Export/Collision"), "level settings: A's comes back");
      e->undo();
      check(prefs::b("Export/Collision"), "level settings: undo");
      e->redo();
      check(!prefs::b("Export/Collision"), "level settings: redo");

      // Save writes A only, then exports it (B is unchanged on both sides).
      prefs::get().setValue(QStringLiteral("Export/AutoExport"), true);
      check(w->saveAll() && !docs->dirty(a), "save: Save writes the unsaved level");
      check(QFileInfo(pathB).lastModified() == savedB, "save: an unchanged level is not written");
      const QString tscnA = QDir(project->gameDir()).filePath(QStringLiteral("levels/a/a.tscn"));
      check(waitUntil([&] { return QFileInfo::exists(tscnA); }, 8000), "save: A exported to game/");
      check(!QFileInfo::exists(QDir(project->gameDir()).filePath(QStringLiteral("levels/b/b.tscn"))),
            "save: B not exported");
      // A parked level exports too (no collision in A: the level setting).
      bool exportedB = false;
      w->exportLevelTo(b->serial, project->gameDir(), [&](const QString &, bool ok) { exportedB = ok; });
      check(waitUntil([&] { return exportedB; }, 8000), "export: a level that is not on screen");
      QFile glbA(QDir(project->gameDir()).filePath(QStringLiteral("levels/a/worldspawn.glb")));
      QFile glbB(QDir(project->gameDir()).filePath(QStringLiteral("levels/b/worldspawn.glb")));
      check(glbA.open(QIODevice::ReadOnly) && !glbA.readAll().contains("colonly"), "export: Export/Collision off (A)");
      check(glbB.open(QIODevice::ReadOnly) && glbB.readAll().contains("colonly"), "export: collision on (B)");

      // An image tab: the level views and their commands step aside.
      const QString png = QDir(project->path(QStringLiteral("images"))).filePath(QStringLiteral("swatch.png"));
      QImage img(16, 8, QImage::Format_ARGB32);
      img.fill(Qt::red);
      img.save(png);
      const quint64 serialA = e->levelSerial();
      check(docs->open(png) && docs->active() && !docs->active()->isLevel(), "image: opens in a tab");
      check(e->levelSerial() == serialA && !ui::act(QStringLiteral("edit.undo"))->isEnabled(),
            "image: the level stays loaded, its commands are off");
      docs->activate(docs->indexOf(a));
      check(ui::act(QStringLiteral("edit.delete"))->isEnabled() || e->selectedObjects().empty(),
            "image: back to the level, its commands are on");

      // Close the project and open it again: the tabs come back, A shown.
      check(w->closeProjectDocuments() && docs->count() == 0, "session: project's tabs closed");
      w->openProjectSession();
      check(docs->count() == 3 && docs->active() && docs->active()->path == QDir::cleanPath(pathA),
            qPrintable(QStringLiteral("session: tabs restored (%1)").arg(docs->count())));
      check(brushCount(e) == brushesA, "session: A read back");
      w->closeProjectDocuments();
    }
    proj::ProjectManager::instance().close();
  }

  qInfo().noquote() << QStringLiteral("[selftest] done: %1 failure(s)").arg(g_fail);
  QTimer::singleShot(300, qApp, [] { QCoreApplication::exit(g_fail == 0 ? 0 : 1); });
}
