// Open levels: the active one lives in the engine's members, the others are
// parked in DocumentStates (VK_ENGINE_LEVEL_STATE lists what moves).

#include "vk_engine.h"

#include <QElapsedTimer>

#include "vk_gridview.h"
#include "vk_shared.h"
#include "client/lib/globals.h"
#include "client/prefs/preferences.h"

namespace vk {

namespace {

// Copies a level's defaults (DocumentState from a pristine engine).
template <class T>
void copyDefault(T &dst, const T &src) {
  dst = src;
}
template <class T, class D>
void copyDefault(std::unique_ptr<T, D> &dst, const std::unique_ptr<T, D> &) {
  dst.reset(); // owned per level: made fresh (the undo stack) or rebuilt (caches)
}
template <class T, std::size_t N>
void copyDefault(T (&dst)[N], const T (&src)[N]) {
  std::copy(src, src + N, dst);
}

} // namespace

std::unique_ptr<Engine3D::DocumentState> Engine3D::newDocumentState() {
  auto st = std::make_unique<DocumentState>();
  if (m_levelDefaults) {
#define VK_LEVEL_COPY(m) copyDefault(st->m, m_levelDefaults->m);
    VK_ENGINE_LEVEL_STATE(VK_LEVEL_COPY)
#undef VK_LEVEL_COPY
  }
  st->m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  st->m_undoStack = makeUndoStack();
  st->m_levelSerial = m_nextLevelSerial++;
  st->m_levelCameraValid = false;
  st->m_levelViews.clear();
  st->m_levelMode = 0;
  return st;
}

void Engine3D::captureLevelDefaults() {
  // What a new level starts with.
  m_levelDefaults = std::make_unique<DocumentState>();
#define VK_LEVEL_COPY(m) copyDefault(m_levelDefaults->m, m);
  VK_ENGINE_LEVEL_STATE(VK_LEVEL_COPY)
#undef VK_LEVEL_COPY
  m_levelDefaults->m_scene.reset();
  m_levelDefaults->m_sceneLight = nullptr;
  if (m_levelSerial == 0)
    m_levelSerial = m_nextLevelSerial++;
}

bool Engine3D::canSwitchDocument() const {
  if (m_undoStack && m_undoStack->inMacro())
    return false;
  return m_selStepDepth == 0;
}

void Engine3D::swapLevelState(DocumentState &state) {
  using std::swap;
#define VK_LEVEL_SWAP(m) swap(m, state.m);
  VK_ENGINE_LEVEL_STATE(VK_LEVEL_SWAP)
#undef VK_LEVEL_SWAP
}

void Engine3D::captureLevelView() {
  // The 3D camera (the editor camera while a Camera3D is previewed).
  m_levelCamera.position = m_previewCamera ? m_previewSavedPos : m_camera->getPosition();
  m_levelCamera.yaw = m_previewCamera ? m_previewSavedYaw : m_camera->getYaw();
  m_levelCamera.pitch = m_previewCamera ? m_previewSavedPitch : m_camera->getPitch();
  m_levelCamera.speed = m_cameraSpeed;
  m_levelCameraValid = true;
  m_levelViews.clear();
  for (const GridView *view: GridView::instances()) {
    fmap::OrthoViewState v;
    v.perspective = static_cast<int32_t>(view->perspective());
    v.centerA = view->viewCenterA();
    v.centerB = view->viewCenterB();
    v.pxPerWorldUnit = view->pixelsPerWorldUnit();
    v.gridLevel = view->gridLevel();
    v.showGrid = view->showGrid();
    m_levelViews.push_back(v);
  }
  m_levelMode = int(g::editorMode);
}

void Engine3D::restoreLevelView() {
  if (m_levelCameraValid && m_camera)
    m_camera->setPose(m_levelCamera.position, m_levelCamera.yaw, m_levelCamera.pitch);
  emit cameraOrientationChanged();
  for (GridView *view: GridView::instances()) {
    bool restored = false;
    for (const auto &v: m_levelViews) {
      if (int(view->perspective()) != int(v.perspective))
        continue;
      view->restoreView(v.centerA, v.centerB, v.pxPerWorldUnit, v.showGrid);
      restored = true;
      break;
    }
    if (!restored)
      view->resetView(); // a level never shown: fit its contents
  }
}

void Engine3D::switchDocument(DocumentState &state) {
  QElapsedTimer t;
  t.start();
  const quint64 from = m_levelSerial;
  const auto typeOn = m_autoSel.typeOn; // a preference of the panel, not of a level

  // Outgoing level: tools end, camera / views / mode are kept with it.
  endTransientState();
  captureLevelView();
  if (m_sceneLight)
    disconnect(m_sceneLight, nullptr, this, nullptr);

  swapLevelState(state);

  if (!m_scene)
    m_scene = QSharedPointer<eng::Scene>(new eng::Scene());
  if (!m_undoStack)
    m_undoStack = makeUndoStack();
  m_undoStack->setMaxSteps(prefs::i("General/UndoLimit"));
  m_autoSel.typeOn = typeOn;
  m_autoSelOther.typeOn = typeOn;
  connectSceneLight();
  TextureManager::instance().setOverrides(&m_scene->materialOverrides);

  // GPU pools follow the scene pointer (GpuScene::sync); slots, face blocks and signatures are rebuilt.
  clearObjectSlotState();
  m_faceSigById.clear();
  m_faceStructVersion = ~0ull;
  m_visible.clear();
  invalidateDerivedCaches();
  m_sigGeometry = ~0ull;
  m_collisionEntitiesKey = ~0ull;
  m_collisionBuiltKey = ~0ull;
  m_sceneBoundsKey = ~0ull;
  m_lightAtlasKey = ~0ull;
  m_ioLinksKey = ~0ull;
  m_mirrorOverlayKey = ~0ull;
  m_lutTexKey = 0;
  m_shadowDirty = true;
  ++m_entityEpoch;
  m_propEpoch = ~0ull;
  m_shapePreviewIds.clear();
  resetPhysicsForLevel();
  loadSkybox();
  loadColorLut();
  restoreLevelView();
  noteFillSelection(); // the Storeys panel's "one face picked" for this level

  // Every panel refreshes as after a map load, plus the dirty flag and history.
  m_dirtyReported = dirty();
  emit documentChanged();
  emit levelSettingsChanged();
  emit sceneChanged();
  emit selectionChanged();
  emit undoRedoChanged();
  emit dirtyChanged();
  emit sunChanged();
  emit skyChanged();
  emit lutsChanged();
  emit meshGroupsChanged();
  emit faceGroupsChanged();
  emit procObjectsChanged();
  emit mirrorChanged();
  emit modHostsChanged();
  emit modOutputsChanged();
  emit editSourceChanged();
  emit pathsChanged();
  emit materialOverridesChanged();
  emit layoutsChanged();
  emit entitiesChanged();
  emit autoSelectChanged();
  emit marqueeChanged();
  emit vertexToolChanged();
  emit gridLevelChanged(m_gridLevelOffset);
  emit cameraPreviewChanged();
  emit fillChanged(); // V (selection tint) and the storeys state
  requestRedraw();
  m_switchFramesToLog = 3;
  qInfo().noquote() << QStringLiteral("[docs] switched level %1 -> %2 in %3 ms")
                           .arg(from).arg(m_levelSerial).arg(double(t.nsecsElapsed()) * 1e-6, 0, 'f', 1);
}

void Engine3D::withDocument(DocumentState &state, const std::function<void()> &fn) {
  if (state.m_levelSerial == m_levelSerial) {
    fn(); // it is the active one
    return;
  }
  // No signals, no GPU work: only the members change hands for the call.
  const QSignalBlocker block(this);
  swapLevelState(state);
  TextureManager::instance().setOverrides(m_scene ? &m_scene->materialOverrides : nullptr);
  m_capturingParked = true;
  fn();
  m_capturingParked = false;
  swapLevelState(state);
  TextureManager::instance().setOverrides(m_scene ? &m_scene->materialOverrides : nullptr);
}

} // namespace vk
