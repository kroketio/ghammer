#pragma once

#include <array>
#include <map>
#include <optional>
#include <functional>
#include <memory>
#include <set>
#include <tuple>
#include <type_traits>
#include <vector>

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QSharedPointer>
#include <QVector>
#include <QVector3D>
#include <QJsonObject>

#include "engine/brush.h"
#include "engine/camera.h"
#include "engine/face_fill.h"
#include "engine/face_graph.h"
#include "engine/brush_select.h"
#include "engine/stair_gen.h"
#include "engine/face_select.h"
#include "engine/io_sim.h"
#include "engine/map_format.h"
#include "engine/mirror_gen.h"
#include "engine/material.h"
#include "engine/scene.h"
#include "engine/shape_gen.h"
#include "vk_common.h"
#include "vk_gpu_scene.h"
#include "vk_undo.h"

namespace prefs {
class Store;
}
namespace eng::layout {
struct Config;
}
namespace vk {
class Engine3D;
std::unique_ptr<prefs::Store> makeLevelStore(Engine3D &engine); // vk_level_settings.cpp
}

class QRhi;
class QRhiBuffer;
class QRhiTexture;
class QRhiRenderTarget;
class QRhiTextureRenderTarget;
class QRhiRenderBuffer;
class QRhiRenderPassDescriptor;
class QRhiShaderResourceBindings;
class QRhiGraphicsPipeline;
class QRhiCommandBuffer;
class QRhiResourceUpdateBatch;
class QKeyEvent;
class QTimer;

namespace eng {
class ConvexBrush;
struct Entity;
struct SkyInfo;
namespace phys {
class World;
struct Snapshot;
}
struct ArchParams;
}

namespace vk {

// A face's polygon in world space and its outward normal (vk_texture_tools.cpp).
bool worldFace(const eng::GameObject &obj, int f, std::vector<glm::vec3> &poly, glm::vec3 &n);

class TextRenderer;

// Editor-side 3D engine: camera, selection, sun and every GPU resource of one viewport.
// Geometry, face materials / UVs, transforms and the selection are versioned: only changed bytes are re-uploaded.
class Engine3D final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int renderMode READ renderMode WRITE setRenderMode NOTIFY renderModeChanged)

  // Sun / lighting: everything notifies through sunChanged.
  Q_PROPERTY(float sunAzimuth READ sunAzimuth WRITE setSunAzimuth NOTIFY sunChanged)
  Q_PROPERTY(float sunElevation READ sunElevation WRITE setSunElevation NOTIFY sunChanged)
  Q_PROPERTY(float sunIntensity READ sunIntensity WRITE setSunIntensity NOTIFY sunChanged)
  Q_PROPERTY(QColor sunColor READ sunColor WRITE setSunColor NOTIFY sunChanged)
  Q_PROPERTY(float ambientIntensity READ ambientIntensity WRITE setAmbientIntensity NOTIFY sunChanged)
  Q_PROPERTY(QColor ambientColor READ ambientColor WRITE setAmbientColor NOTIFY sunChanged)
  Q_PROPERTY(float exposure READ exposure WRITE setExposure NOTIFY sunChanged)
  Q_PROPERTY(float fogDensity READ fogDensity WRITE setFogDensity NOTIFY sunChanged)
  Q_PROPERTY(bool shadowsEnabled READ shadowsEnabled WRITE setShadowsEnabled NOTIFY sunChanged)
  Q_PROPERTY(float shadowBias READ shadowBias WRITE setShadowBias NOTIFY sunChanged)
  Q_PROPERTY(float shadowNormalBias READ shadowNormalBias WRITE setShadowNormalBias NOTIFY sunChanged)
  Q_PROPERTY(float shadowRadius READ shadowRadius WRITE setShadowRadius NOTIFY sunChanged)
  Q_PROPERTY(float shadowStrength READ shadowStrength WRITE setShadowStrength NOTIFY sunChanged)
  Q_PROPERTY(float shadowRange READ shadowRange WRITE setShadowRange NOTIFY sunChanged)
  Q_PROPERTY(int shadowMapSize READ shadowMapSize WRITE setShadowMapSize NOTIFY sunChanged)
  Q_PROPERTY(bool ssaoEnabled READ ssaoEnabled WRITE setSsaoEnabled NOTIFY sunChanged)
  Q_PROPERTY(float ssaoRadius READ ssaoRadius WRITE setSsaoRadius NOTIFY sunChanged)
  Q_PROPERTY(float ssaoIntensity READ ssaoIntensity WRITE setSsaoIntensity NOTIFY sunChanged)
  Q_PROPERTY(int msaaSamples READ msaaSamples WRITE setMsaaSamples NOTIFY msaaSamplesChanged)

  Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoRedoChanged)
  Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoRedoChanged)
  Q_PROPERTY(QString undoText READ undoText NOTIFY undoRedoChanged)
  Q_PROPERTY(QString redoText READ redoText NOTIFY undoRedoChanged)

  // The undo cursor vs. where it stood at save / load, not a flag: undoing back to it is clean again.
  Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
  // Camera orientation for the QML axis gizmo.
  Q_PROPERTY(float cameraYaw READ cameraYaw NOTIFY cameraOrientationChanged)
  Q_PROPERTY(float cameraPitch READ cameraPitch NOTIFY cameraOrientationChanged)
  Q_PROPERTY(QString gizmoReadoutText READ gizmoReadout NOTIFY gizmoReadoutChanged)
  // Camera3D entities: a selected one offers "Preview" (look through it).
  Q_PROPERTY(bool cameraPreviewAvailable READ cameraPreviewAvailable NOTIFY cameraPreviewChanged)
  Q_PROPERTY(bool cameraPreview READ cameraPreview WRITE setCameraPreview NOTIFY cameraPreviewChanged)

  // 3D draw distance in Source units: a brush whose nearest point is beyond it is not drawn.
  Q_PROPERTY(float renderDistanceSource READ renderDistanceSource WRITE
                 setRenderDistanceSource NOTIFY renderDistanceChanged)
  // Light entities beyond this are not rendered (no contribution, no shadow tiles).
  Q_PROPERTY(float lightRenderDistanceSource READ lightRenderDistanceSource WRITE
                 setLightRenderDistanceSource NOTIFY renderDistanceChanged)
  // Distance beyond which a fragment stops sampling the directional shadow map.
  Q_PROPERTY(float shadowRenderDistanceSource READ shadowRenderDistanceSource WRITE
                 setShadowRenderDistanceSource NOTIFY renderDistanceChanged)

  // Shape tools built from the 2D marquee with a live preview (ui/arch_panel.*); stairs are a generated object.
  Q_PROPERTY(bool shapeToolActive READ shapeToolActive NOTIFY shapeToolChanged)
  Q_PROPERTY(int shapeToolKind READ shapeToolKind NOTIFY shapeToolChanged)
  Q_PROPERTY(int shapeToolPlane READ shapeToolPlane NOTIFY shapeToolChanged)
  Q_PROPERTY(int shapeToolBrushCount READ shapeToolBrushCount NOTIFY shapeToolChanged)
  Q_PROPERTY(int stairSteps READ stairSteps WRITE setStairSteps NOTIFY shapeToolChanged)
  Q_PROPERTY(int stairDirection READ stairDirection WRITE setStairDirection NOTIFY shapeToolChanged)
  Q_PROPERTY(int stairStyle READ stairStyle WRITE setStairStyle NOTIFY shapeToolChanged)
  Q_PROPERTY(double stairStepHeightSource READ stairStepHeightSource NOTIFY shapeToolChanged)
  Q_PROPERTY(double stairStepDepthSource READ stairStepDepthSource NOTIFY shapeToolChanged)
  Q_PROPERTY(int archProfile READ archProfile WRITE setArchProfile NOTIFY shapeToolChanged)
  Q_PROPERTY(int archMode READ archMode WRITE setArchMode NOTIFY shapeToolChanged)
  Q_PROPERTY(int archSegments READ archSegments WRITE setArchSegments NOTIFY shapeToolChanged)
  Q_PROPERTY(double archThicknessSource READ archThicknessSource WRITE setArchThicknessSource NOTIFY shapeToolChanged)
  Q_PROPERTY(double archSpringSource READ archSpringSource WRITE setArchSpringSource NOTIFY shapeToolChanged)
  Q_PROPERTY(double archRise READ archRise WRITE setArchRise NOTIFY shapeToolChanged)
  Q_PROPERTY(double archWidthSource READ archWidthSource NOTIFY shapeToolChanged)
  Q_PROPERTY(double archHeightSource READ archHeightSource NOTIFY shapeToolChanged)

  Q_PROPERTY(bool vertexScaleActive READ vertexScaleActive NOTIFY vertexToolChanged)
  Q_PROPERTY(double vertexScale READ vertexScale WRITE setVertexScale NOTIFY vertexToolChanged)
  Q_PROPERTY(int vertexScaleAxes READ vertexScaleAxes WRITE setVertexScaleAxes NOTIFY vertexToolChanged)
  Q_PROPERTY(QVector3D vertexPivot READ vertexPivot WRITE setVertexPivot NOTIFY vertexToolChanged)
  Q_PROPERTY(int vertexEditPlane READ vertexEditPlane NOTIFY vertexToolChanged)
  Q_PROPERTY(int vertexSubMode READ vertexSubMode WRITE setVertexSubMode NOTIFY vertexToolChanged)
  Q_PROPERTY(int selectedVertexCount READ selectedVertexCount NOTIFY vertexToolChanged)

  Q_PROPERTY(QString cutModeLabel READ cutModeLabel NOTIFY cutModeChanged)
  Q_PROPERTY(bool cutModeActive READ cutModeActive NOTIFY cutModeChanged)
  Q_PROPERTY(int cutOriginPlane READ cutOriginPlane NOTIFY cutOriginChanged)
  Q_PROPERTY(int cutShape READ cutShapeInt NOTIFY cutShapeChanged)
  Q_PROPERTY(QString cutShapeLabel READ cutShapeLabel NOTIFY cutShapeChanged)
  Q_PROPERTY(double cutoutWidthSource READ cutoutWidthSource WRITE setCutoutWidthSource NOTIFY
                 cutoutSizeChanged)
  Q_PROPERTY(double cutoutHeightSource READ cutoutHeightSource WRITE setCutoutHeightSource NOTIFY
                 cutoutSizeChanged)
  Q_PROPERTY(bool cutoutPlaced READ cutoutPlaced NOTIFY cutHoleChanged)
  Q_PROPERTY(int cutoutCount READ cutoutCount NOTIFY cutHoleChanged)
  Q_PROPERTY(int cutoutTemplate READ cutoutTemplate WRITE setCutoutTemplate NOTIFY cutoutTemplateChanged)
  Q_PROPERTY(QStringList cutoutTemplateNames READ cutoutTemplateNames NOTIFY cutShapeChanged)
  Q_PROPERTY(double cutoutSillFraction READ cutoutSillFraction WRITE setCutoutSillFraction NOTIFY
                 cutoutSizeChanged)
  Q_PROPERTY(double cutoutSillSource READ cutoutSillSource NOTIFY cutoutSizeChanged)
  Q_PROPERTY(bool cutoutAutoFill READ cutoutAutoFill WRITE setCutoutAutoFill NOTIFY cutoutLayoutChanged)
  Q_PROPERTY(bool autoFillAllowed READ autoFillAllowed NOTIFY cutShapeChanged)
  Q_PROPERTY(double cutoutSpacingSource READ cutoutSpacingSource WRITE setCutoutSpacingSource NOTIFY
                 cutoutLayoutChanged)
  Q_PROPERTY(double cutoutMarginSource READ cutoutMarginSource WRITE setCutoutMarginSource NOTIFY
                 cutoutLayoutChanged)

  // Material-mode auto-select: a picked face grows into its wall run / hallway / facade; the facade's layer
  // solver drives the per-floor texturing.
  Q_PROPERTY(bool fillAvailable READ fillAvailable NOTIFY selectionChanged)
  Q_PROPERTY(int fillMode READ fillMode NOTIFY fillOptionsChanged)
  Q_PROPERTY(double fillMinAreaSource READ fillMinAreaSource WRITE setFillMinAreaSource NOTIFY
                 fillOptionsChanged)
  Q_PROPERTY(double fillMinSpanSource READ fillMinSpanSource WRITE setFillMinSpanSource NOTIFY
                 fillOptionsChanged)
  Q_PROPERTY(double fillClearSource READ fillClearSource WRITE setFillClearSource NOTIFY
                 fillOptionsChanged)
  Q_PROPERTY(QString fillNote READ fillNote NOTIFY fillChanged)
  Q_PROPERTY(int fillLevelCount READ fillLevelCount NOTIFY fillChanged)
  Q_PROPERTY(QStringList fillLevelLabels READ fillLevelLabels NOTIFY fillChanged)
  Q_PROPERTY(bool fillHasLayers READ fillHasLayers NOTIFY fillChanged)

public:
  // Preview: a scene of its own (texture preview, splash), no level settings,
  // physics, modifiers poll or overlays.
  enum class Role { Editor, Preview };
  explicit Engine3D(QObject *parent = nullptr, Role role = Role::Editor);
  ~Engine3D() override;
  // Preview scenes: replaces the point entities (world_env stays), no undo.
  void setPreviewEntities(const std::vector<eng::Entity> &entities);
  // A flat background instead of the sky and the Color/Background3D preference (previews).
  void setBackgroundOverride(const QColor &color);

  // Every 2D drag goes through beginDrag/updateDrag/endDrag (one snapping path). "Selection" = meshes and
  // entities together: grabbing any member moves all of it.
  enum class DragTarget {
    None,
    CreateMarquee,
    MoveMarquee,
    MoveSelection,
    ResizeMarquee,
    ResizeMeshes,
  };
  Q_ENUM(DragTarget)

  [[nodiscard]] const MarqueeBox &marquee() const { return m_marquee; }
  [[nodiscard]] bool hasMarquee() const { return m_marquee.valid; }
  [[nodiscard]] DragTarget dragTarget() const { return m_dragTarget; }

  Q_INVOKABLE void clearMarquee();
  // Sets the selection box directly (self-test, tools that place one).
  void setMarquee(const MarqueeBox &m) {
    m_marquee = m;
    m_marquee.valid = true;
    emit marqueeChanged();
  }
  // additive toggles this object, else replaces the selection. `stableId` is the object's id, not its index.
  Q_INVOKABLE void selectObject(quint32 stableId, bool additive = false);
  // Selects what the marquee covers and drops the marquee.
  Q_INVOKABLE void commitMarquee();

  // 2D view bases are signed world axes, so a drawn line is an axis-aligned plane `dot(p, n) = k`: a half-space clip.
  enum class CutMode {
    Off,
    Positive,  // the geometry on the positive side of the plane is removed
    Negative,  // the geometry on the negative side is removed
    KeepBoth,  // nothing is removed, the mesh is split and both halves survive
  };
  Q_ENUM(CutMode)

  struct CutLine {
    bool valid = false;
    int plane = 0;           // GridView::Perspective that drew it
    bool alongRight = false; // true: runs along basis.right, normal = basis.up
    double k = 0.0;          // world offset along that normal
    // Free-angle cut through a and b (plane coordinates of the view that drew it).
    bool angled = false;
    glm::dvec2 a {0.0}, b {0.0};
  };
  // A line dragged in a 2D view is kept: hovering the 3D view does not move it.
  bool m_cutLineFrom2D = false;
  bool m_cutTurned = false;
  bool m_cutHoverValid = false;
  glm::vec2 m_cutHoverPx {0.0f};
  bool m_cutHoverShift = false;

  // Slice = the half-space clip; the rest are rectangular openings through the selection along the view's depth.
  enum class CutShape {
    Slice,
    Door,
    Window,
    AirVent,
  };
  Q_ENUM(CutShape)

  // A prism: centre in the view plane, w along basis.right, h along basis.up, d0..d1 through the selection.
  struct CutHole {
    bool valid = false;
    bool placed = false;
    int plane = 0;
    double cx = 0.0;
    double cy = 0.0;
    double w = 0.0;
    double h = 0.0;
    double d0 = 0.0;
    double d1 = 0.0;
  };

  // Opening sizes in Source units; the first entry of each list is the default.
  struct CutTemplate {
    const char *name;
    double wSource;
    double hSource;
  };
  static const std::vector<CutTemplate> &cutTemplatesFor(CutShape shape);

  static constexpr double kDoorWidthSource = 56.0;
  static constexpr double kDoorHeightSource = 112.0;
  static constexpr double kWindowWidthSource = 64.0;
  static constexpr double kWindowHeightSource = 48.0;
  static constexpr double kAirVentSizeSource = 40.0;
  // Sill height as a fraction of the wall height.
  static constexpr double kWindowSillFraction = 0.25;
  static constexpr double kAutoFillSpacingSource = 32.0;
  static constexpr double kAutoFillMarginSource = 32.0;

  [[nodiscard]] CutMode cutMode() const { return m_cutMode; }
  [[nodiscard]] bool cutModeActive() const { return m_cutMode != CutMode::Off; }
  // The 2D view the cut was aimed from; frozen while a cut session runs.
  [[nodiscard]] int cutOriginPlane() const { return m_cutOriginPlane; }
  void noteCutOriginPlane(int plane);
  [[nodiscard]] const CutLine &cutLine() const { return m_cutLine; }
  [[nodiscard]] bool hasCutLine() const { return m_cutLine.valid; }
  [[nodiscard]] QString cutModeLabel() const;

  [[nodiscard]] int cutShapeInt() const { return static_cast<int>(m_cutShape); }
  [[nodiscard]] bool isCutout() const { return m_cutShape != CutShape::Slice; }
  [[nodiscard]] const std::vector<CutHole> &cutHoles() const { return m_cutHoles; }
  [[nodiscard]] bool hasCutHole() const { return !m_cutHoles.empty(); }
  [[nodiscard]] int cutoutCount() const { return static_cast<int>(m_cutHoles.size()); }
  [[nodiscard]] bool cutoutPlaced() const { return !m_cutHoles.empty() && m_cutHoles.front().placed; }
  [[nodiscard]] double cutoutWidthSource() const { return m_cutoutW; }
  [[nodiscard]] double cutoutHeightSource() const { return m_cutoutH; }
  [[nodiscard]] QString cutShapeLabel() const;
  [[nodiscard]] int cutoutTemplate() const { return m_cutTemplate; }
  [[nodiscard]] QStringList cutoutTemplateNames() const;
  [[nodiscard]] double cutoutSillFraction() const { return m_cutoutSill; }
  [[nodiscard]] bool cutoutAutoFill() const { return m_cutoutAutoFill; }
  [[nodiscard]] double cutoutSpacingSource() const { return m_cutoutSpacing; }
  [[nodiscard]] double cutoutMarginSource() const { return m_cutoutMargin; }
  [[nodiscard]] bool autoFillAllowed() const {
    return m_cutShape == CutShape::Window || m_cutShape == CutShape::AirVent;
  }
  // The opening's height above the selection's bottom, Source units (0 for doors).
  [[nodiscard]] double cutoutSillSource() const;

  // shape tools (vk_shape_tools.cpp)
  // kind: 6 = stairs, 7 = arch (eng::PrimitiveKind). Needs a marquee; the preview follows it.
  Q_INVOKABLE bool beginShapeTool(int kind);
  Q_INVOKABLE bool commitShapeTool();
  Q_INVOKABLE void cancelShapeTool();
  [[nodiscard]] bool shapeToolActive() const { return m_shapeTool.active; }
  [[nodiscard]] int shapeToolKind() const { return m_shapeTool.kind; }
  [[nodiscard]] int shapeToolPlane() const { return m_marquee.valid ? m_marquee.plane : -1; }
  [[nodiscard]] int shapeToolBrushCount() const { return int(m_shapePreviewIds.size()); }
  [[nodiscard]] int stairSteps() const { return m_shapeTool.steps; }
  void setStairSteps(int n);
  [[nodiscard]] int stairDirection() const { return m_shapeTool.direction; }
  void setStairDirection(int d);
  [[nodiscard]] int stairStyle() const { return m_shapeTool.style; }
  void setStairStyle(int s);
  [[nodiscard]] double stairStepHeightSource() const;
  [[nodiscard]] double stairStepDepthSource() const;
  [[nodiscard]] int archProfile() const { return m_shapeTool.archProfile; }
  void setArchProfile(int p);
  [[nodiscard]] int archMode() const { return m_shapeTool.archMode; }
  void setArchMode(int m);
  [[nodiscard]] int archSegments() const { return m_shapeTool.archSegments; }
  [[nodiscard]] bool archSmooth() const { return m_shapeTool.archSmooth; }
  // Shape tools (beginShapeTool with kShapeCylinder / kShapeCapsule / kShapePyramid): their settings.
  static constexpr int kShapeCylinder = 2, kShapePyramid = 8, kShapeCapsule = 9;
  [[nodiscard]] static bool isShapeKind(int kind) {
    return kind == kShapeCylinder || kind == kShapeCapsule || kind == kShapePyramid;
  }
  [[nodiscard]] const eng::ShapeParams &shapeToolParams() const { return m_shapeTool.shape; }
  void setShapeToolParams(const eng::ShapeParams &p);
  void setArchSmooth(bool on);
  void setArchSegments(int n);
  // Ring depth (Ring) or jamb / crown (Archway): each kind keeps its own.
  [[nodiscard]] double archThicknessSource() const {
    return m_shapeTool.archMode == 1 ? m_shapeTool.archJambSource : m_shapeTool.archThicknessSource;
  }
  void setArchThicknessSource(double t);
  [[nodiscard]] double archSpringSource() const { return m_shapeTool.archSpringSource; }
  void setArchSpringSource(double t);
  [[nodiscard]] double archRise() const { return m_shapeTool.archRise; }
  void setArchRise(double r);
  [[nodiscard]] double archWidthSource() const;
  [[nodiscard]] double archHeightSource() const;
  // True for the transient preview brushes (not saved, not selectable).
  [[nodiscard]] bool isShapePreview(quint32 stableId) const { return m_shapePreviewIds.count(stableId) != 0; }

  Q_INVOKABLE void enterCutMode();
  Q_INVOKABLE void cycleCutMode();
  // 1 = remove the positive side, 2 = the negative side, 3 = keep both.
  void setCutMode(int mode);
  Q_INVOKABLE void exitCutMode();
  Q_INVOKABLE bool applyCut();

  Q_INVOKABLE void setCutShape(int shape);
  Q_INVOKABLE void setCutoutTemplate(int index);
  Q_INVOKABLE void setCutoutWidthSource(double v);
  Q_INVOKABLE void setCutoutHeightSource(double v);
  Q_INVOKABLE void setCutoutSillFraction(double v);
  Q_INVOKABLE void setCutoutAutoFill(bool on);
  Q_INVOKABLE void setCutoutSpacingSource(double v);
  Q_INVOKABLE void setCutoutMarginSource(double v);
  Q_INVOKABLE QString cutShapeName(int shape) const;

  // Cut mode in the 3D view: hover places the cut (rotate = a quarter turn), a click applies it.
  void hoverCut3D(float px, float py, bool rotate);
  bool clickCut3D(float px, float py, bool rotate);
  // The other direction across the face.
  void turnCut3D();
  bool cutHit3D(float px, float py, glm::dvec3 &hit, int &faceAxis) const;
  void beginCutDrag(int plane, const glm::dvec2 &grabPlane, double step);
  void updateCutDrag(const glm::dvec2 &curPlane);
  void endCutDrag();

  // Wall = the coplanar run touching the seed. Facade adds the storey solver, corner wrapping and an
  // outside-only cull: the building's outside skin.
  enum class FillMode {
    Wall,
    Facade,
  };
  Q_ENUM(FillMode)

  // One storey: `y0`/`y1` are world heights; empty material = leave alone.
  struct FillLayer {
    double y0 = 0.0;
    double y1 = 0.0;
    QString material;
    eng::LayerKind kind {eng::LayerKind::Wall};
  };

  [[nodiscard]] bool fillAvailable() const { return m_fillAvailable; }
  [[nodiscard]] int fillMode() const { return static_cast<int>(m_fillMode); }
  [[nodiscard]] bool fillWrap() const { return m_fillWrap; }
  [[nodiscard]] double fillMinAreaSource() const { return m_fillMinAreaSource; }
  void setFillMinAreaSource(double v);
  [[nodiscard]] double fillMinSpanSource() const { return m_fillMinSpanSource; }
  void setFillMinSpanSource(double v);
  [[nodiscard]] double fillClearSource() const { return m_fillClearSource; }
  void setFillClearSource(double v);
  Q_PROPERTY(int fillStoreySpanPct READ fillStoreySpanPct WRITE setFillStoreySpanPct NOTIFY fillChanged)

  Q_PROPERTY(bool fillWireVisible READ fillWireVisible WRITE setFillWireVisible NOTIFY fillChanged)
  Q_PROPERTY(bool maskHidden READ maskHidden WRITE setMaskHidden NOTIFY fillChanged)
  // Hides the selected faces' red tint without dropping the selection.
  [[nodiscard]] bool maskHidden() const { return m_maskHidden; }
  void setMaskHidden(bool hide);
  // Whether the storey layer bands are drawn.
  [[nodiscard]] bool fillWireVisible() const { return m_fillWireVisible; }
  void setFillWireVisible(bool on);
  // Off: layers come from the wall's own horizontal seams instead of the floor slabs behind it.
  [[nodiscard]] bool fillDetectFloors() const { return m_fillDetectFloors; }
  // Percent of its storey a face must cover to count as wall; 0 = off.
  [[nodiscard]] int fillStoreySpanPct() const { return m_fillStoreySpanPct; }
  void setFillStoreySpanPct(int v);
  [[nodiscard]] QString fillNote() const { return m_fillNote; }
  [[nodiscard]] int fillLevelCount() const { return int(m_fillLayers.size()); }
  [[nodiscard]] bool fillHasLayers() const { return m_fillLayers.size() > 1; }
  QStringList fillLevelLabels() const;

  Q_INVOKABLE void clearFill();
  // The storey solver. replaceSelection false: the selection is kept and gets the
  // storey materials.
  bool solveStoreys(quint32 seed, const eng::FillParams &p, bool replaceSelection);

  // generated objects (stairs, ...)
  // Brushes point at their generated object (GameObject::procObject), which keeps the type and parameters.
  [[nodiscard]] std::vector<quint32> procMembers(quint32 id) const;
  // The generated object when every selected brush belongs to the same one.
  [[nodiscard]] quint32 selectedProcObject() const;
  [[nodiscard]] QString procType(quint32 id) const;
  [[nodiscard]] QString procName(quint32 id) const;
  [[nodiscard]] QJsonObject procParams(quint32 id) const;
  bool procMembersBounds(quint32 id, glm::dvec3 &mn, glm::dvec3 &mx) const;
  // The generator's box (the stairs box) for the brushes' current bounds.
  bool procBox(quint32 id, glm::dvec3 &mn, glm::dvec3 &mx) const;
  // The parameters placed at the brushes' current bounds, or at bounds they are about to get.
  [[nodiscard]] QJsonObject procCurrentParams(quint32 id) const;
  [[nodiscard]] QJsonObject procParamsForBounds(quint32 id, const glm::dvec3 &boundsMin,
                                                const glm::dvec3 &boundsMax) const;
  bool setProcParams(quint32 id, const QJsonObject &params, const QString &label);
  // Transforms whole selected generated objects through their parameters; returns those rebuilt, `handled`
  // gets the brushes replaced.
  std::vector<quint32> transformProcObjects(const glm::dmat4 &m, const QString &label, std::set<quint32> &handled);
  quint32 createStairs(const glm::dvec3 &mn, const glm::dvec3 &mx, int direction, const std::vector<quint32> &replace,
                       const QString &material);
  // Stairs from the 2D box, or replacing the selected brushes when there is no box.
  bool stairsFromMarqueeOrSelection();
  // A generated arch filling the box, its profile across `spanAxis` (0 = X, 2 = Z).
  quint32 createArch(const glm::dvec3 &mn, const glm::dvec3 &mx, int spanAxis, const eng::ArchParams &arch);
  // A generated cylinder / capsule / pyramid ("cylinder", ... ProcObject) in the box; 0 on failure.
  quint32 createShape(const glm::dvec3 &mn, const glm::dvec3 &mx, const eng::ShapeParams &shape);
  bool setProcParam(quint32 id, const QString &key, const QJsonValue &value);
  bool setProcBox(quint32 id, const glm::dvec3 &mn, const glm::dvec3 &mx, const QString &label);
  bool fitStairsDepth(quint32 id);
  bool dissolveProc(quint32 id);
  bool renameProc(quint32 id, const QString &name);

  // stairs texturing (Material mode, vk_proc_objects.cpp)
  // The generated object every selected face is on (collision ramp faces left out), else 0.
  [[nodiscard]] quint32 selectedFacesProcObject() const;
  // Bit (1 << Part) for every stair part a selected face of `id` is in.
  [[nodiscard]] int selectedStairParts(quint32 id) const;
  [[nodiscard]] eng::stairs::StepTex stairPartTexture(quint32 id, eng::stairs::Part part) const;
  // Stores a part's step mapping and applies it to its faces (one undo step); rebuilds keep applying it.
  // `force` re-applies unchanged settings (faces edited by hand).
  bool setStairPartTexture(quint32 id, eng::stairs::Part part, const eng::stairs::StepTex &t, const QString &label,
                           bool force = false);
  // Turns the step mapping on with the material most used by the part's selected faces (else the whole part).
  bool fitStairPartTexture(quint32 id, eng::stairs::Part part);
  int selectStairPart(quint32 id, eng::stairs::Part part);

  // face groups (Material mode)
  struct FaceGroupUse {
    quint32 id = 0;
    QString name;
    QColor color;
    bool builtin = false;
    int count = 0;
  };
  [[nodiscard]] const eng::FaceGroupTable *faceGroupTable() const;
  [[nodiscard]] std::vector<FaceGroupUse> faceGroupsOfSelection() const;
  [[nodiscard]] std::vector<quint32> facesInGroup(quint32 id) const;
  bool addSelectionToFaceGroup(const QString &name);
  bool removeSelectionFromFaceGroup(quint32 id);
  int selectFaceGroup(quint32 id, bool add);
  bool renameFaceGroup(quint32 id, const QString &name);
  bool deleteFaceGroup(quint32 id);

  // auto-select (the Auto-select dock)
  // Material mode: intent = eng::sel::Intent, seeds = the selected faces (inside the box, if one is drawn).
  // Object mode: intent = eng::sel::BrushIntent, seeds = the selected brushes. The previous run's seeds are
  // reused while its result is still the selection. exposure: eng::sel::Exposure. Each mode keeps its own run
  // (parked with its box across mode switches).
  // `byObject` (Material mode): only faces of the generated objects the seeds are on.
  bool runAutoSelect(int intent, int exposure, bool allSides, quint32 groupId, bool contiguous, bool add,
                     bool sameMaterial = false, bool byObject = false);
  void clearAutoSelect();
  void clearAutoSelectRegion();
  [[nodiscard]] bool autoSelectActive() const;
  [[nodiscard]] bool autoSelectIsFacade() const;
  // The running result's intent (Intent or BrushIntent, by mode) and exposure; valid while autoSelectActive().
  [[nodiscard]] int autoSelectIntent() const {
    return m_materialMode ? int(m_autoSel.query.intent) : int(m_autoSel.brushQuery.intent);
  }
  [[nodiscard]] int autoSelectExposure() const {
    return m_materialMode ? int(m_autoSel.query.exposure) : int(m_autoSel.brushQuery.exposure);
  }
  [[nodiscard]] QString autoSelectSummary() const;
  [[nodiscard]] std::array<int, 6> facadeTypeCounts() const;
  [[nodiscard]] bool facadeTypeEnabled(int t) const;
  void setFacadeTypeEnabled(int t, bool on);
  // Groups the seed face grows from: first its brush's group (not worldspawn) as kMeshGroupPick | group id,
  // then its face groups. Group with a kMeshGroupPick id selects every face of that group's visible brushes.
  static constexpr quint32 kMeshGroupPick = 0x80000000u;
  [[nodiscard]] std::vector<FaceGroupUse> seedFaceGroups() const;
  // The seed's first user face group (else its first group), contiguous.
  bool selectByFaceGroup();
  // Group's default (seedFaceGroups): the first user group (the brush group leads), else the first; 0 = none.
  [[nodiscard]] quint32 defaultSeedGroup() const;
  // Material mode: selects every face of the picked faces' brushes. False when nothing is picked.
  bool selectSameBrushFaces(bool add);
  // Material mode: selects every face in the scene with a material of the picked faces. False when nothing is
  // picked.
  bool selectSameMaterialFaces(bool add);
  // Every face (pick id) of these brushes; hidden ones are skipped.
  [[nodiscard]] std::vector<quint32> facePicksOfBrushes(const std::set<quint32> &ids) const;
  // Replaces the selection with these brushes as Object mode units; modifier output selects its host.
  void selectBrushUnits(const std::set<quint32> &ids);
  // Up to `max` material refs nearest the selected faces, nearest first; none already on every selected face.
  [[nodiscard]] QStringList nearbyMaterialRefs(int max);
  bool showStoreys();
  void hideStoreys();
  [[nodiscard]] bool storeysShown() const { return m_storeysShown; }
  // applyFillMaterials() uses each layer's material; applyFillMaterial() puts one on every layer.
  Q_INVOKABLE void setLayerMaterial(int layer, const QString &name);
  Q_INVOKABLE QString layerMaterial(int layer) const;
  Q_INVOKABLE void applyFillMaterials();
  Q_INVOKABLE void applyFillMaterial(const QString &name);
  // Splits the covered brushes at the layer bounds.
  Q_INVOKABLE bool sliceFillToLayers();
  // Bound `i` is the seam between layer i-1 and layer i.
  [[nodiscard]] int fillBoundCount() const { return int(m_fillBounds.size()); }
  [[nodiscard]] double fillBound(int i) const;
  Q_INVOKABLE void setFillBound(int i, double worldY);
  [[nodiscard]] int fillBoundHit(int plane, const glm::dvec2 &p, double tolWorld) const;
  // World AABB of the filled run.
  [[nodiscard]] bool fillExtent(glm::dvec3 &mn, glm::dvec3 &mx) const;

  void noteFillSelection();

  // Drags the grabbed layer seam in an elevation view and re-derives the layers.
  void beginFillBoundDrag(int plane, const glm::dvec2 &p, double step);
  void updateFillBoundDrag(const glm::dvec2 &p);
  void endFillBoundDrag();
  [[nodiscard]] bool fillBoundDragging() const { return m_fillDragIndex >= 0; }

  // Cutout placement: the opening follows the pointer (snapped) until a click pins it; then it is dragged.
  void trackCutHole(int plane, const glm::dvec2 &p, double step);
  void placeCutHole();
  void beginCutHoleDrag(int plane, const glm::dvec2 &p);
  void dragCutHole(const glm::dvec2 &p);
  void endCutHoleDrag();
  static void cutHoleWorld(const CutHole &hole, glm::dvec3 &mn, glm::dvec3 &mx);
  // Whether this plane point is inside a placed opening.
  [[nodiscard]] bool cutHoleContains(int plane, const glm::dvec2 &p) const;

  void beginDrag(DragTarget target, int plane, const glm::dvec2 &grabPlane, double step,
                double depth0 = 0.0, double depth1 = 0.0);
  // `handle` is a vk::SelectionHandle; resizes the marquee when one is live, else the selected meshes' box.
  void beginResize(bool marqueeTarget, int plane, int handle, const glm::dvec2 &grabPlane,
                  double step);
  // `duplicate` (Shift): once the drag snaps off its start cell, clones move and the originals go back.
  void updateDrag(const glm::dvec2 &curPlane, bool duplicate = false);
  void endDrag();

  void setScene(const QSharedPointer<eng::Scene> &scene);
  // Drops every tool, mode and gesture of the open map (setScene runs it).
  void resetEditingState();
  // Only gestures, tools and previews; the selection, selection box and auto-select runs stay (level switch).
  void endTransientState();
  [[nodiscard]] const QSharedPointer<eng::Scene> &scene() const { return m_scene; }

  // map persistence (.ghmap)
  // The whole editor state: geometry, entities, render settings and every camera.
  [[nodiscard]] fmap::Document captureMapDocument() const;
  void applyMapDocument(const fmap::Document &doc);

  // Mutations are applied first and recorded second: undo / redo run through the same setters.
  [[nodiscard]] eng::UndoStack *undoStack() const { return m_undoStack.get(); }
  Q_INVOKABLE bool undo();
  Q_INVOKABLE bool redo();
  Q_INVOKABLE void clearUndoHistory();
  [[nodiscard]] bool dirty() const {
    // Selection steps in between do not count (SelectionCommand).
    return m_undoStack && !m_undoStack->isClean();
  }
  // Call once the current state is what is on disk. A fresh scene counts.
  void markClean();
  [[nodiscard]] bool canUndo() const;
  [[nodiscard]] bool canRedo() const;
  [[nodiscard]] QString undoText() const;
  [[nodiscard]] QString redoText() const;

  // Capture / apply of objects' editable state; the undo commands wrap these.
  [[nodiscard]] ObjectState captureObjectState(quint32 stableId, bool withFaces) const;
  void applyObjectStates(const std::vector<ObjectState> &states);
  // Whole-selection restore, one notification at the end.
  void setSelected(const std::vector<quint32> &objects, const std::vector<quint32> &faces);
  // Call after changing the scene's shape from outside the render loop.
  void notifyStructureChanged();

  eng::Camera *camera() { return m_camera.get(); }
  [[nodiscard]] const eng::Camera *camera() const { return m_camera.get(); }

  int renderMode() const { return static_cast<int>(m_renderMode); }
  void setRenderMode(int mode);

  Q_INVOKABLE void clearSelection();
  // Object-mode clicks record the selection change as an undo step (eng::SelectionCommand; the map stays
  // saved). Calls nest; the outermost pair makes the step.
  struct SelectionState {
    std::set<quint32> objects, faces, entities, mods, paths;
    bool operator==(const SelectionState &) const = default;
  };
  [[nodiscard]] SelectionState selectionState() const;
  void applySelectionState(const SelectionState &s);
  void beginSelectionStep();
  void endSelectionStep();

  // Material mode parks the object selection; leaving it restores it untouched.
  Q_INVOKABLE void setMaterialMode(bool material);
  [[nodiscard]] bool materialMode() const { return m_materialMode; }

  // Deletes the selection, brushes and entities together; Object mode ids and Material mode face pick ids fold
  // into one set.
  Q_INVOKABLE void deleteSelected();
  [[nodiscard]] std::set<quint32> selectedBrushIds() const;

  // The map's world_env entity (always present once a scene is set).
  [[nodiscard]] const eng::Entity *worldEnvEntity() const;
  // Screen position (top-down pixels of the 3D view) and that view's size.
  [[nodiscard]] glm::vec2 screenPosOf(const glm::vec3 &world) const { return projectToScreen(world); }
  [[nodiscard]] QSize viewPixelSize() const { return m_lastViewSize; }
  // For the self-test: half the entity icon's edge in pixels; view refresh after a scripted camera move.
  [[nodiscard]] float entityIconHalfPx() const { return billboardHalfPx(false); }
  void refreshViewUniforms() { updateSceneUniforms(m_lastViewSize); }
  [[nodiscard]] bool worldEnvSelected() const;
  void selectWorldEnv();
  void ensureWorldEnv();
  // Both selection sets hold stable ids, never slots, so a selection survives deletes.
  [[nodiscard]] const std::set<quint32> &selectedFaces() const { return m_selectedFaces; }
  [[nodiscard]] const std::set<quint32> &selectedObjects() const { return m_selectedObjects; }
  // Resolves a selected stable id to the live object (null if it is gone).
  [[nodiscard]] std::shared_ptr<eng::GameObject> selectedObject(quint32 stableId) const {
    return m_scene ? m_scene->findById(stableId) : nullptr;
  }

  // Ray-cast pick in logical pixels: nearest face wins; the mode picks the face or its object.
  void pick(float x, float y, float viewWidth, float viewHeight, bool additive, bool materialMode);

  void setSunDirection(const glm::vec3 &dir);
  [[nodiscard]] glm::vec3 sunDirection() const { return m_sunDirection; }

  // Degrees, derived from / written into m_sunDirection. Azimuth spins around world Z; elevation above the
  // horizon (90 = overhead, 0 = horizontal).
  [[nodiscard]] float sunAzimuth() const;
  void setSunAzimuth(float degrees);
  [[nodiscard]] float sunElevation() const;
  void setSunElevation(float degrees);

  void setSunIntensity(float intensity);
  [[nodiscard]] float sunIntensity() const { return m_sunIntensity; }
  [[nodiscard]] QColor sunColor() const;
  void setSunColor(const QColor &color);
  void setAmbientIntensity(float intensity);
  [[nodiscard]] float ambientIntensity() const { return m_ambientIntensity; }
  [[nodiscard]] QColor ambientColor() const;
  void setAmbientColor(const QColor &color);
  void setFogDensity(float density);
  [[nodiscard]] float fogDensity() const { return m_fogDensity; }
  [[nodiscard]] QColor fogColor() const;
  void setFogColor(const QColor &color);
  void setExposure(float exposure);
  [[nodiscard]] float exposure() const { return m_exposure; }

  // A texture ref with a Panorama map, empty = the procedural sky. Saved in SCNE v2; undoable. Drawn in Lit
  // mode only (vk_sky.cpp: the server's 2048 x 1024 sky file, RGBA16F).
  [[nodiscard]] QString skybox() const { return m_skyRef; }
  void setSkybox(const QString &ref);

  // Level-scope settings: the scene's LCFG values (only those off their default), read / written by prefs
  // through levelStore(). Writes are undo steps; an invalid value removes the key.
  [[nodiscard]] QVariant levelSetting(const QString &key) const;
  void setLevelSetting(const QString &key, const QVariant &value);
  [[nodiscard]] prefs::Store *levelStore() const { return m_levelStore.get(); }
  // The skybox plus its sun / ambient / fog (eng::skyLighting), one undo step.
  void applySkybox(const QString &ref, const eng::SkyInfo &info);
  // physics (vk_physics.cpp)
  // The map as a Box3D world, rebuilt on one worker thread 600 ms after the last change (never during a drag
  // or an open undo macro). Solid brushes are static hulls, func_rigidbody bodies. Off with Debug/Physics.
  [[nodiscard]] const eng::phys::World *physicsWorld() const;
  [[nodiscard]] bool physicsBusy() const;
  // EDITOR_PHYSICSDUMP: after the current build, steps it `seconds` on the worker and reports each body.
  void runPhysicsCheck(double seconds, std::function<void(const QString &report)> done);

  // bevel mode (vk_bevel.cpp, geometry in engine/brush_bevel.*)
  // Rounds or chamfers the picked edges of the selected brushes (inverse: a cove or an inner-corner fill);
  // applying is one undo step.
  [[nodiscard]] bool bevelMode() const { return m_bevel.open; }
  // False + a toast when nothing to bevel is selected.
  bool enterBevelMode();
  void leaveBevelMode();
  // 3D pointer move / left press (true when bevel mode took it).
  void hoverBevel3D(float px, float py);
  bool pressBevel3D(float px, float py);
  // A 2D view press in bevel mode: picks an edge seen end-on (always taken).
  bool pickBevelEdge2D(int plane, const glm::dvec2 &p, double pxPerWorldUnit);
  bool applyBevel();
  void clearBevelEdges();
  void toggleBevelInverse();
  // Options (radius in world units).
  [[nodiscard]] double bevelRadius() const { return m_bevel.radius; }
  void setBevelRadius(double radius);
  [[nodiscard]] int bevelSegments() const { return m_bevel.segments; }
  void setBevelSegments(int segments);
  [[nodiscard]] bool bevelInverse() const { return m_bevel.inverse; }
  void setBevelInverse(bool inverse);
  [[nodiscard]] bool bevelCurrentMaterial() const { return m_bevel.currentMaterial; }
  void setBevelCurrentMaterial(bool on);
  [[nodiscard]] bool bevelSmooth() const { return m_bevel.smooth; }
  void setBevelSmooth(bool on);
  [[nodiscard]] int bevelPickedCount() const { return int(m_bevel.picked.size()); }
  // The largest radius the faces allow for the picked edges (0: none).
  [[nodiscard]] double bevelLimit();
  // "Round · R 16 · 4 segments", or why it cannot be made (`error` set).
  [[nodiscard]] QString bevelStatus(bool *error = nullptr);

  // physics simulation (vk_physics_sim.cpp)
  // Steps a Box3D world of the level at 60 Hz. While running, bodies move only through a render pose override
  // (the level is untouched, editing off); Pause writes them into the level (one undo step). A body is dirty
  // while its brushes are not its originals. Edits while paused make the next Play rebuild the world.
  enum class PhysicsSimState { None, Running, Paused };
  [[nodiscard]] PhysicsSimState physicsSimState() const;
  [[nodiscard]] bool physicsSimSelectedOnly() const;
  // Starts (false when there is nothing to simulate) or resumes, keeping the scope it started with.
  bool physicsPlay(bool selectedOnly);
  void physicsPause();
  // Dirty bodies back to their original brushes (one undo step), then ends the simulation.
  void physicsReset();
  // Ends the simulation, leaving the level as it is.
  void physicsStop();
  // The selected dirty bodies keep their current brushes (no longer dirty).
  bool physicsApply();
  [[nodiscard]] bool selectionHasRigidBody() const;
  [[nodiscard]] bool levelHasRigidBody() const;
  [[nodiscard]] bool physicsBodyDirty(quint32 entity) const;
  // A selected brush belongs to a simulated body (running or paused): it must not be untied / re-tied.
  [[nodiscard]] bool selectionInPhysicsSimulation() const;
  [[nodiscard]] bool selectionPhysicsDirty() const;
  [[nodiscard]] int physicsDirtyCount() const;
  [[nodiscard]] std::vector<quint32> physicsBodyBrushes() const;
  // Each dirty body's origin trail (world units).
  [[nodiscard]] std::vector<std::vector<glm::dvec3>> physicsTrails() const;
  [[nodiscard]] quint64 physicsPoseEpoch() const { return m_physPoseEpoch; }
  // The render override of a brush moved by the running simulation (world units), or null.
  [[nodiscard]] const glm::mat4 *physicsPose(quint32 objectId) const {
    const auto it = m_physPose.constFind(objectId);
    return it == m_physPose.constEnd() ? nullptr : &it.value();
  }

  // True once the panorama is on the GPU (false while it downloads).
  [[nodiscard]] bool skyboxLoaded() const { return m_skyTex != nullptr && m_skyTexRef == m_skyRef; }

  [[nodiscard]] bool shadowsEnabled() const { return m_shadowsEnabled; }
  void setShadowsEnabled(bool on);
  [[nodiscard]] float shadowBias() const { return m_shadowBias; }
  void setShadowBias(float bias);
  [[nodiscard]] float shadowNormalBias() const { return m_shadowNormalBias; }
  void setShadowNormalBias(float bias);
  [[nodiscard]] float shadowRadius() const { return m_shadowRadius; }
  void setShadowRadius(float radius);
  [[nodiscard]] float shadowStrength() const { return m_shadowStrength; }
  void setShadowStrength(float strength);
  [[nodiscard]] float shadowRange() const { return m_shadowFar; }
  void setShadowRange(float range);
  [[nodiscard]] int shadowMapSize() const { return m_shadowMapSize; }
  void setShadowMapSize(int size);

  // SSAO, component mode only; notifies through sunChanged.
  [[nodiscard]] bool ssaoEnabled() const { return m_ssaoEnabled; }
  void setSsaoEnabled(bool on);
  [[nodiscard]] float ssaoRadius() const { return m_ssaoRadius; }
  void setSsaoRadius(float radius);
  [[nodiscard]] float ssaoIntensity() const { return m_ssaoIntensity; }
  void setSsaoIntensity(float intensity);

  // 0 / 2 / 4; 0 = no MSAA (sample count 1 for the RHI).
  [[nodiscard]] int msaaSamples() const { return m_msaaSamples; }
  void setMsaaSamples(int samples);

  // Source units.
  static constexpr float kRenderDistanceMinSource = 1024.0f;
  static constexpr float kRenderDistanceMaxSource = 40960.0f;
  static constexpr float kRenderDistanceDefaultSource = 7168.0f;
  static constexpr float kLightRenderDistanceDefaultSource = 6144.0f;
  static constexpr float kShadowRenderDistanceDefaultSource = 10240.0f;
  static constexpr float kSourceUnitsPerWorldUnit = 10.0f;

  // A new brush's material and texture scale.
  static constexpr const char *kDefaultBrushMaterial = "dev/dev_measurewall01d";
  static constexpr float kDefaultBrushTextureScale = 0.25f;

  [[nodiscard]] float renderDistanceSource() const { return m_renderDistanceSource; }
  void setRenderDistanceSource(float sourceUnits);
  [[nodiscard]] float lightRenderDistanceSource() const { return m_lightRenderDistanceSource; }
  void setLightRenderDistanceSource(float sourceUnits);
  [[nodiscard]] float shadowRenderDistanceSource() const { return m_shadowRenderDistanceSource; }
  void setShadowRenderDistanceSource(float sourceUnits);
  [[nodiscard]] float renderDistanceWorld() const {
    return m_renderDistanceSource / kSourceUnitsPerWorldUnit;
  }
  [[nodiscard]] float lightRenderDistanceWorld() const {
    return m_lightRenderDistanceSource / kSourceUnitsPerWorldUnit;
  }
  [[nodiscard]] float shadowRenderDistanceWorld() const {
    return m_shadowRenderDistanceSource / kSourceUnitsPerWorldUnit;
  }
  // Sphere-vs-AABB: true when any part of the object is within range.
  [[nodiscard]] bool withinRenderDistance(const eng::GameObject &object) const;

  // Keeps the scene's DirectionalLight and the renderer's sun the same light.
  void syncSunToSceneLight();
  void adoptSunFromSceneLight();

  void setWireframeThickness(float px);
  [[nodiscard]] float wireframeThickness() const { return m_wireframeThicknessPx; }

  // Every entity as one flat marker list, shared by the 3D pass and the 2D views.
  [[nodiscard]] QVector<EntityMarker> entityMarkers() const;

  // One gizmo for the selected meshes or entity, kept at a constant size on screen.
  enum class GizmoHandle {
    None = 0,
    AxisX,
    AxisY,
    AxisZ,
    PlaneX, // move in the YZ plane (locked against X)
    PlaneY,
    PlaneZ,
    RingX, // rotate around X
    RingY,
    RingZ,
    ScaleX,
    ScaleY,
    ScaleZ,
    ScaleUniform,
  };
  Q_ENUM(GizmoHandle)

  enum class GizmoMode { Translate = 0, Rotate = 1, Scale = 2 };
  [[nodiscard]] GizmoMode gizmoMode() const { return m_gizmoMode; }
  void setGizmoMode(GizmoMode mode);
  // A click on the selection in a 2D view: rotate mode on / off (Translate).
  void toggleRotateMode();
  // 2D rotate mode: turns the selection about the view's depth axis through its centre (Grid/AngleSnap
  // unless Ctrl), previewed live, committed on release.
  bool beginRotate2D(int plane, const glm::dvec2 &p);
  void updateRotate2D(const glm::dvec2 &p);
  void endRotate2D();
  [[nodiscard]] bool rotating2D() const { return m_rot2D.active; }
  // The angle of the running 2D rotation, counter-clockwise on screen (degrees).
  [[nodiscard]] double rotate2DAngle() const { return m_rot2D.screenAngle; }
  // 2D entity rotate knob: turns every selected oriented point entity about its own origin around the view's
  // depth axis (snapped on screen, Ctrl free). `centre` / `dir`: the entity centre and arrow direction in the
  // view plane. One undo step on release.
  bool beginEntityRotate2D(int plane, quint32 id, const glm::dvec2 &centre, const glm::dvec2 &dir);
  void updateEntityRotate2D(const glm::dvec2 &p);
  void endEntityRotate2D();
  [[nodiscard]] bool entityRotating2D() const { return m_entRot2D.active; }
  [[nodiscard]] quint32 entityRotate2DId() const { return m_entRot2D.id; }
  [[nodiscard]] double entityRotate2DAngle() const { return m_entRot2D.angle; }
  // Oriented; not a point light / world_env / brush entity.
  [[nodiscard]] static bool entityRotatable(const eng::Entity &e);
  // Live readout while dragging ("Rotate Y 45°"), empty otherwise.
  [[nodiscard]] QString gizmoReadout() const { return m_gizmoReadout; }

  [[nodiscard]] bool gizmoVisible() const;
  [[nodiscard]] bool isGizmoDragging() const { return m_gizmoDrag != GizmoHandle::None; }
  // True when the press landed on a handle: the caller must not treat it as a pick.
  bool beginGizmoDrag(float x, float y);
  void updateGizmoDrag(float x, float y, bool duplicate = false);
  void endGizmoDrag();
  void hoverGizmo(float x, float y);

  // Grid level shared by the 2D views: a power-of-two shift on each view's zoom step; positive = coarser.
  Q_PROPERTY(int gridLevelOffset READ gridLevelOffset WRITE setGridLevelOffset NOTIFY gridLevelChanged)
  [[nodiscard]] int gridLevelOffset() const { return m_gridLevelOffset; }
  void setGridLevelOffset(int offset);

  // 1-based: level N is a 2^(N-1) Source-unit cell.
  Q_PROPERTY(int gridLevel READ gridLevel WRITE setGridLevel NOTIFY gridLevelChanged)
  [[nodiscard]] int gridLevel() const { return m_gridLevelOffset + kGridLevelBase; }
  void setGridLevel(int level) { setGridLevelOffset(level - kGridLevelBase); }

  // The offset shifts kBaseGridStepSource (16 units at offset 0): level 1 == offset -4.

  static constexpr int kGridLevelBase = 5;
  static constexpr int kGridLevelDefaultOffset = 1; // 32 Source units
  static constexpr int kGridLevelMin = 1;
  static constexpr int kGridLevelMax = 10;

  // Off by default (as Hammer). They only affect later transforms:
  //  textureLock      - moving / rotating carries the texture instead of leaving it on the world grid.
  //  textureScaleLock - resizing stretches the texture instead of adding or removing repeats.
  Q_PROPERTY(bool textureLock READ textureLock WRITE setTextureLock NOTIFY textureLockChanged)
  [[nodiscard]] bool textureLock() const { return m_textureLock; }
  void setTextureLock(bool lock);

  Q_PROPERTY(bool textureScaleLock READ textureScaleLock WRITE setTextureScaleLock NOTIFY
                 textureScaleLockChanged)
  [[nodiscard]] bool textureScaleLock() const { return m_textureScaleLock; }
  void setTextureScaleLock(bool lock);

  // material mode: UV scale and offset of the picked faces
  // Read / write the mesh's per-face texture basis; writing bumps the face version (re-upload).
  // Scale is Source units per tile (as Hammer; the inverse of the basis' tiles-per-unit); offset in tiles.
  Q_PROPERTY(int selectedFaceCount READ selectedFaceCount NOTIFY selectionChanged)
  [[nodiscard]] int selectedFaceCount() const { return int(m_selectedFaces.size()); }

  Q_PROPERTY(double uvScaleU READ uvScaleU WRITE setUvScaleU NOTIFY uvTransformChanged)
  [[nodiscard]] double uvScaleU() const;
  void setUvScaleU(double scale);

  Q_PROPERTY(double uvScaleV READ uvScaleV WRITE setUvScaleV NOTIFY uvTransformChanged)
  [[nodiscard]] double uvScaleV() const;
  void setUvScaleV(double scale);

  Q_PROPERTY(double uvOffsetU READ uvOffsetU WRITE setUvOffsetU NOTIFY uvTransformChanged)
  [[nodiscard]] double uvOffsetU() const;
  void setUvOffsetU(double offset);

  Q_PROPERTY(double uvOffsetV READ uvOffsetV WRITE setUvOffsetV NOTIFY uvTransformChanged)
  [[nodiscard]] double uvOffsetV() const;
  void setUvOffsetV(double offset);
  // >0 one level coarser, <0 one level finer.
  Q_INVOKABLE void stepGridLevel(int direction);

  // Camera keys are latched here and integrated in render() (frame-rate independent).
  void setSpeed(float speed) { m_cameraSpeed = speed; }
  [[nodiscard]] float speed() const { return m_cameraSpeed; }
  // Freelook deltas accumulate here and apply in updateCamera(), on the same tick as WASD.
  void addFreelookDelta(int dx, int dy) { m_freelookDx += dx; m_freelookDy += dy; }
  void resetFreelookDelta() { m_freelookDx = 0; m_freelookDy = 0; }

  void handleKeyPress(QKeyEvent *event);
  void handleKeyRelease(QKeyEvent *event);
  bool toggleFreeLook();

  // pick() with the last frame's view size and the current mode.
  void performPicking(float x, float y, bool additive);

  void render(QRhiCommandBuffer *cb, QRhiRenderTarget *target, const QSize &pixelSize, int sampleCount);
  void releaseResources();

  // The level's colour LUT (vk_luts.cpp): a ref to a LUT of the asset server ("pack/Name" in a "lut" pack),
  // saved in the map (SCNE), set as an undo step. Its strip is downloaded and drawn after tone mapping.
  [[nodiscard]] QString colorLut() const { return m_lutRef; }
  void setColorLut(const QString &ref);
  // The level LUT's strip once loaded (null while loading, or without one).
  [[nodiscard]] QImage colorLutStrip() const { return m_lutStripRef == m_lutRef ? m_lutStrip : QImage(); }
  // Any LUT's strip if this editor has loaded it (a parked level's, for the export); null otherwise.
  [[nodiscard]] QImage colorLutStripFor(const QString &ref) const;
  // Shows `strip` for `ref` right away (a LUT being edited) until the server's next version of it arrives.
  void previewColorLut(const QString &ref, const QImage &strip);
  // The LUT window's pick, drawn instead of the level's LUT until endColorLutPreview() (the level keeps its own).
  void previewColorLutStrip(const QImage &strip);
  void endColorLutPreview();
  // View › Colour LUT: off draws the level without its LUT (for this session, the level keeps it).
  [[nodiscard]] bool colorLutEnabled() const { return m_lutEnabled; }
  void setColorLutEnabled(bool on);

signals:
  void redrawRequested();
  void cameraOrientationChanged();
  void gizmoModeChanged();
  void gizmoReadoutChanged(const QString &text);
  // Names, visibility, locks or per-face attributes changed (no shape change).
  void objectStateChanged();
  void currentMaterialChanged(const QString &name);
  void renderModeChanged();
  void selectionChanged();
  void sceneChanged();
  // Any entity marker moved, appeared, disappeared or changed.
  void entitiesChanged();
  void cameraPreviewChanged();
  // The viewport target picker started or ended.
  void targetPickChanged(bool active);
  // The I/O simulator started, stopped, paused or stepped.
  void ioSimChanged();
  // One simulator log line: what fired / a note. from / to are entity ids (0 = the console).
  void ioSimLogged(double time, const QString &text, quint32 from, quint32 to, bool note);
  void lutsChanged();
  // Mesh group list, names, colours, flags or the active group changed.
  void meshGroupsChanged();
  void shapeToolChanged();
  void vertexToolChanged();
  void layoutsChanged();
  void marqueeChanged();
  void faceGroupsChanged();
  void procObjectsChanged();
  // The map's material overrides (map layer, variants, project pack) changed.
  void materialOverridesChanged();
  void autoSelectChanged();
  void mirrorChanged();
  // Modifier hosts table (stack, names) changed; the output changed; Edit Source.
  void modHostsChanged();
  void modOutputsChanged();
  void editSourceChanged();
  void pathsChanged();
  void toastRequested(const QString &text);
  // The pen tool turned on / off or its unfinished path changed.
  void penChanged();
  void cutModeChanged();
  void cutOriginChanged();
  void cutLineChanged();
  void cutShapeChanged();
  void cutoutSizeChanged();
  void cutoutTemplateChanged();
  void cutoutLayoutChanged();
  void cutHoleChanged();
  void fillChanged();
  void fillOptionsChanged();
  void gridLevelChanged(int offset);
  void textureLockChanged();
  void textureScaleLockChanged();
  void uvTransformChanged();
  void sunChanged();
  void skyChanged();  // skybox ref set / its panorama arrived
  void zenModeChanged(); // on / off, or another box size
  void zenChanged();     // the box moved (the 3D camera moved) or what it lets through changed
  void levelSettingsChanged();
  void physicsChanged();  // a physics build finished (collision overlay, stats)
  void bevelChanged();    // the bevel tool opened / closed, its edges, options or preview changed
  void physicsSimChanged(); // the simulation started, paused, was reset, or a body became dirty
  void msaaSamplesChanged();
  void renderDistanceChanged();
  void undoRedoChanged();
  void dirtyChanged();

private:
  // One undo step: the group list and the objects' group / hidden / locked state around `mutate`.
  void recordGroupEdit(const QString &label, const std::vector<quint32> &objects, const std::function<void()> &mutate);
  struct ShapeToolState {
    bool active = false;
    int kind = 6;
    int steps = 8;
    int direction = 2;
    int style = 0;
    int archProfile = 0;
    int archMode = 1;
    int archSegments = 8;
    double archThicknessSource = 16.0;  // Ring depth
    double archJambSource = 0.0;        // Archway jamb / crown
    double archSpringSource = 0.0;
    double archRise = 0.5;
    bool archSmooth = true;
    eng::ShapeParams shape;
  };
  ShapeToolState m_shapeTool;
  std::set<quint32> m_shapePreviewIds;
  bool m_shapeToolConnected = false;
  void rebuildShapePreview();
  void removeShapePreview();
  [[nodiscard]] std::vector<std::shared_ptr<eng::GameObject>> buildShapeToolObjects() const;
  struct VisibleFace {
    eng::Material *material = nullptr;
    QRhiShaderResourceBindings *srb = nullptr;
    quint32 objectSlot = 0;
    quint32 indexStart = 0;
    // Consecutive same-material faces with the same dynamic offsets merge into one draw.
    quint32 indexCount = 6;
    quint32 vertexBase = 0;
  };

  void ensureBuffers();
  void uploadObjectBlocks(QRhiResourceUpdateBatch *rub);
  void uploadFaceBlocks(QRhiResourceUpdateBatch *rub);
  QRhiShaderResourceBindings *materialSrb(eng::Material *material);

  // Expensive frame work is skipped when its hashed inputs did not change. Each key must cover every input its
  // consumer reads: under-invalidating is a stale image that never corrects itself.
  void computeFrameSignatures();
  void invalidateDerivedCaches();
  // Emits dirtyChanged only when the value flips.
  void refreshDirty();
  std::unique_ptr<eng::UndoStack> makeUndoStack();
  void connectSceneLight();
  void applyMaterialOverrides(const eng::MaterialOverrides &overrides);
  void pushOverridesChange(const QString &label, const eng::MaterialOverrides &before, const QString &mergeKey = {});
  void assignMaterialToFaces(const std::vector<std::pair<quint32, int>> &faces, const eng::MaterialPtr &m,
                             const QString &label);
  bool m_dirtyReported = false;
  std::unique_ptr<prefs::Store> m_levelStore;
  void applyLevelSetting(const QString &key, const QJsonValue &value);

  void ensureShadowTarget();
  void ensureOverlayTargets(const QSize &pixelSize);
  void ensureSsaoTargets(const QSize &pixelSize, QRhiResourceUpdateBatch *rub);
  void releaseSsaoTargets(bool retire);
  void setupPipelines(QRhiRenderPassDescriptor *rp, int sampleCount);
  void updateLightTransform();
  void updateSceneUniforms(const QSize &pixelSize);
  void collectVisibleFaces();
  int buildLabelDecals();

  int renderShadowPass(QRhiCommandBuffer *cb, const QSize &pixelSize, int cascade = 0);
  void renderScenePass(QRhiCommandBuffer *cb, const QSize &pixelSize);
  int renderWireOverlay(QRhiCommandBuffer *cb);
  void renderGBufferPass(QRhiCommandBuffer *cb, const QSize &pixelSize);
  void drawFullscreen(QRhiCommandBuffer *cb, QRhiGraphicsPipeline *pipeline,
                     QRhiShaderResourceBindings *srb, const QSize &size);
  void renderSsaoChain(QRhiCommandBuffer *cb, QRhiResourceUpdateBatch *rub);
  void applyAmbientOcclusion(QRhiCommandBuffer *cb);
  // FXAA (vk_fxaa.cpp): the frame renders into fxaaSceneTarget(), finishFxaa() writes the real target.
  QRhiRenderTarget *fxaaSceneTarget(QRhiRenderTarget *target, const QSize &pixelSize, int sampleCount);
  void finishFxaa(QRhiCommandBuffer *cb, QRhiRenderTarget *target, const QSize &pixelSize, int sampleCount);
  void releaseFxaa();
  // Entity icon billboards (vk_billboards.cpp).
  static int entityIconFor(int classKind);
  static int entityIconFor(const eng::EntityClass *cls);
  static int entityIconCount();
  void ensureIconAtlas(QRhiResourceUpdateBatch *rub);
  void releaseIconAtlas();
  void buildBillboardPipeline(QRhiRenderPassDescriptor *rp, int sampleCount);
  void appendBillboardDraws(const QVector<EntityMarker> &markers);
  void drawBillboards(QRhiCommandBuffer *cb);
  [[nodiscard]] float billboardHalfPx(bool selected) const;
  bool pickBillboard(float x, float y, float maxDist, quint32 &id) const;
  // The 3D LUT the scene and sky shaders sample (binding 15); a 2³ identity cube when none is active.
  // Returns the LUT size, 0 = none.
  int ensureLutTexture(QRhiResourceUpdateBatch *rub);
  // vk_sky.cpp: fetches + decodes the panorama off the GUI thread; ensureSkyTexture uploads it in the frame.
  void loadSkybox();
  void ensureSkyTexture(QRhiResourceUpdateBatch *rub);
  // vk_physics.cpp
  void setupPhysics();
  void schedulePhysics();
  void startPhysicsBuild();
  void resetPhysicsForLevel();
  struct PhysicsState;
  struct PhysicsStateDeleter {
    void operator()(PhysicsState *s) const;
  };
  std::unique_ptr<PhysicsState, PhysicsStateDeleter> m_phys;
  // The level for Box3D (GUI thread); `simulated` limits the rigid bodies to those entities (null: all).
  eng::phys::Snapshot physicsSnapshot(const std::set<quint32> *simulated);
  // vk_bevel.cpp
  struct BevelEdgeRef {
    quint32 brush = 0;
    glm::dvec3 a {0.0}, b {0.0};
  };
  struct BevelOut {
    quint32 brush = 0;
    std::vector<eng::ConvexBrush> pieces, added;
    bool changesBrush = true; // false when it only fills corners next to it
  };
  struct BevelScopeBrush {
    quint32 id = 0;
    eng::ConvexBrush brush;
    std::vector<std::pair<glm::dvec3, glm::dvec3>> edges;
  };
  struct BevelState {
    bool open = false;                // bevel mode
    std::vector<quint32> scopeIds;    // the brushes it works on (selected on entry)
    std::vector<BevelScopeBrush> scope; // their geometry, as of scopeKey
    quint64 scopeKey = ~0ull;
    std::vector<BevelEdgeRef> picked;
    std::vector<BevelEdgeRef> hover;  // the edge under the pointer and its continuation
    double radius = 0.8;              // world units (8 Source units: "Small")
    int segments = 4;
    bool inverse = false;
    bool currentMaterial = false;
    bool smooth = true; // the bevelled brushes get smooth shading
    // The preview of the picked edges.
    quint64 previewKey = ~0ull;
    std::vector<BevelOut> preview;
    QString status;
    bool statusError = false;
    double previewRadius = 0.0;
    int previewSegments = 0;
    double limit = 0.0;
    // The preview of the edge under the pointer (bevelled on its own).
    quint64 hoverKey = ~0ull;
    std::vector<BevelOut> hoverPreview;
  };
  BevelState m_bevel;
  const std::vector<BevelScopeBrush> &bevelScope();
  // The edge under the pointer, with its continuation on the other brushes.
  std::vector<BevelEdgeRef> bevelEdgeGroupAt3D(float px, float py);
  std::vector<BevelEdgeRef> bevelEdgeGroup(const BevelEdgeRef &edge);
  void toggleBevelPick(const std::vector<BevelEdgeRef> &group);
  std::vector<eng::ConvexBrush> bevelNeighbours(quint32 brush, const eng::ConvexBrush &b) const;
  void refreshBevelPreview();
  // The result per brush with the current options, or false with `status` saying why.
  bool computeBevel(const std::vector<BevelEdgeRef> &edges, std::vector<BevelOut> &out, QString &status,
                    double &limit, double &radius, int &segments);
  [[nodiscard]] quint64 bevelKey(const std::vector<BevelEdgeRef> &edges) const;
  // Bevel mode's part of the cut overlay (drawn over everything): picked edges' result filled and outlined,
  // the hovered edge's outlined.
  void appendBevelOverlay(const std::function<void(const glm::vec3 &, const glm::vec3 &)> &edge,
                          std::vector<glm::vec3> &fill);
  // vk_physics_sim.cpp
  struct PhysicsSim;
  struct PhysicsSimDeleter {
    void operator()(PhysicsSim *s) const;
  };
  std::unique_ptr<PhysicsSim, PhysicsSimDeleter> m_physSim;
  QHash<quint32, glm::mat4> m_physPose; // brush id -> simulated pose (world units, applied after its model)
  quint64 m_physPoseEpoch = 0;
  bool m_physApplying = false;
  void physicsSimTick();
  bool buildPhysicsWorld(const std::set<quint32> &entities);
  void updatePhysicsPoses();
  void checkPhysicsDocument();
  // Moved bodies: their document-pose box's corners at the current pose.
  [[nodiscard]] std::vector<std::array<glm::dvec3, 8>> physicsDirtyBoxes() const;
  // A simulated body's pose (document -> now, world units), or null.
  [[nodiscard]] const glm::dmat4 *physicsBodyPose(quint32 entity) const;
  // The world AABB of an object as drawn (through its physics pose).
  [[nodiscard]] std::pair<glm::vec3, glm::vec3> drawnAABB(const eng::GameObject &o) const;
  void releaseLutTexture();
  void releaseSkyTexture();
  void buildMarqueeGeometry();
  void drawMarquee(QRhiCommandBuffer *cb);
  void buildCutOverlayGeometry();
  void drawCutOverlay(QRhiCommandBuffer *cb);
  void buildFillOverlayGeometry();
  int buildFillLayerLabels();
  void drawFillOverlay(QRhiCommandBuffer *cb);
  void updateResize(const glm::dvec2 &curPlane);
  void applyMarqueeResize(double a0, double b0, double a1, double b1);
  void applyMeshResize(double a0, double b0, double a1, double b1);
  void updateCamera(float deltaTime);

  void ensureEntityResources(QRhiResourceUpdateBatch *rub);
  void releaseEntityResources(bool retire);
  void buildEntityDraws(QRhiResourceUpdateBatch *rub);
  void drawEntities(QRhiCommandBuffer *cb);

  // Both in the pick path's coordinate space, so gizmo hits and picks agree.
  void makeRay(float x, float y, float viewWidth, float viewHeight, glm::vec3 &origin,
              glm::vec3 &dir) const;
  [[nodiscard]] glm::vec2 projectToScreen(const glm::vec3 &point) const;
  [[nodiscard]] glm::vec3 gizmoCenter() const;
  [[nodiscard]] float gizmoScale(const glm::vec3 &center) const;
  [[nodiscard]] double gizmoSnapStep() const;
  [[nodiscard]] GizmoHandle gizmoHitTest(float x, float y) const;

  // Occlusion-aware selection overlay: one pass writes linear camera depth (R32F, nearest wins); selected
  // wireframes sample it to tell whether anything is nearer than the surface they trace.
  int renderSceneDepth(QRhiCommandBuffer *cb, const QSize &pixelSize,
                      QRhiResourceUpdateBatch *rub);
  int drawWholeObjects(QRhiCommandBuffer *cb, QRhiShaderResourceBindings *srb,
                      const std::function<bool(quint32)> &include);

  // The entity buffers exist before setupPipelines: this only checks the allocation.
  [[nodiscard]] bool entityPipelinesWanted() const {
    return m_entityVb != nullptr && m_entityUbo != nullptr;
  }

  [[nodiscard]] bool overlayUsable() const {
    return m_selDepthTarget && m_selDepthPipeline && m_wirePipeline;
  }

  [[nodiscard]] bool shadowUsable() const {
    return m_shadowTexture != nullptr && m_shadowTarget != nullptr && m_shadowPipeline != nullptr;
  }

  [[nodiscard]] bool ssaoUsable() const {
    return m_ssaoAllowed && m_gbufferTarget && m_ssaoPipeline && m_aoBlurPipeline && m_aoApplyPipeline
        && m_aoTargetB && m_quadVb && m_quadIb;
  }

  QSharedPointer<eng::Scene> m_scene;
  std::unique_ptr<eng::Camera> m_camera;
  std::unique_ptr<TextRenderer> m_text;
  std::unique_ptr<eng::UndoStack> m_undoStack;
  GpuScene m_gpu;

  RenderMode m_renderMode = RenderMode::Fullbright;

  std::set<quint32> m_selectedFaces;   // picking ids
  bool m_materialMode = false;
  bool m_hasStashedSelection = false;
  std::set<quint32> m_stashedObjects;
  std::set<quint32> m_stashedFaces;
  std::set<quint32> m_selectedObjects; // stable object ids

  MarqueeBox m_marquee;

  bool rebuildProc(quint32 id, QJsonObject params, const QString &label, bool boxFromMembers,
                   const std::vector<quint32> &alsoRemove = {});
  void pushProcTable(const QString &label, const std::vector<eng::Scene::ProcObject> &before);
  bool regenerateResizedProc();

  void editSelectionGroups(const QString &label, const std::function<void(eng::Mesh &, int)> &edit,
                           const eng::FaceGroupTable *tableBefore);
  void recordFaceGroupEdit(const QString &label, const eng::FaceGroupTable *tableBefore,
                           std::vector<ObjectState> before, std::vector<ObjectState> after);
  const eng::FaceGraph &faceGraph();
  [[nodiscard]] bool autoSelectContinuing() const;
  void runAutoSelectQuery(bool fresh);
  void setAutoSelectMarquee(bool show, const glm::dvec3 &mn, const glm::dvec3 &mx);
  void onMarqueeChangedForAutoSelect();
  // One auto-select run. Material mode: query, result, resultFaces / baseFaces, seedPicks = face pick ids.
  // Object mode: brushQuery, brushResult, resultObjects / baseObjects, seedPicks = object ids.
  struct AutoSelectState {
    bool active = false;
    bool hasRun = false;
    int lastIntent = -1;
    int lastExposure = 1;
    eng::sel::Query query;
    eng::sel::Result result;
    eng::sel::BrushQuery brushQuery;
    eng::sel::BrushResult brushResult;
    std::vector<quint32> seedPicks;
    std::set<quint32> resultFaces;
    std::set<quint32> baseFaces;
    std::set<quint32> resultObjects;
    std::set<quint32> baseObjects;
    // "By object": the generated objects of the seed faces (empty = no filter).
    std::set<quint32> byObjectProcs;
    bool boxIsOurs = false;
    bool boxEdited = false;
    std::array<bool, 6> typeOn {true, true, true, true, true, true};
    std::array<int, 6> typeCounts {};
    // The result box while the run is parked in the other mode.
    std::optional<MarqueeBox> parkedBox;
    bool parkedBoxEdited = false;
    bool storeysWereShown = false;
  };
  AutoSelectState m_autoSel;      // the current mode's run
  AutoSelectState m_autoSelOther; // the other mode's, parked
  // setMaterialMode(): park the current run with its box / restore the parked one after the selection.
  void parkAutoSelect();
  void unparkAutoSelect();
  std::unique_ptr<eng::FaceGraph> m_faceGraph;
  std::array<quint64, 6> m_faceGraphKey {};
  eng::sel::ExposureCache m_faceExposure;
  bool m_settingAutoMarquee = false;
  bool m_storeysShown = false;
  DragTarget m_dragTarget {DragTarget::None};
  int m_dragPlane = 0;
  double m_dragStep = 1.0;
  glm::dvec2 m_dragGrab {0.0};
  double m_dragBaseMinA = 0.0;
  double m_dragBaseMinB = 0.0;
  MarqueeBox m_dragBase {};
  std::vector<quint32> m_dragObjects; // stable ids
  std::vector<glm::vec3> m_dragBasePos;
  // Resize: the grabbed box (a0,b0,a1,b1 in the drag plane), the base world AABB, each mesh's scale.
  int m_dragHandle = -1;
  glm::dvec4 m_dragBox {0.0, 0.0, 0.0, 0.0};
  glm::vec3 m_resizeBaseMin {0.0f};
  glm::vec3 m_resizeBaseMax {0.0f};
  std::vector<glm::vec3> m_dragBaseScale;
  // Each face's texture basis at grab time; lock corrections are recomputed from it (no drift).
  struct DragFaceUv {
    std::vector<eng::FaceTexBasis> bases;
    std::vector<glm::dvec3> normals; // world outward normal of each face at capture
  };
  void captureFaceUv(const std::vector<quint32> &ids, std::vector<DragFaceUv> &out) const;

  // Lazy: clones spawn when the move first snaps off its start cell; the originals go back to their grab state
  // and the drag moves the clones.
  struct DuplicateSet {
    bool triggered = false;
    std::vector<std::shared_ptr<eng::GameObject>> clones;
    std::vector<quint32> sources; // parallel to clones: the brush each was copied from
    std::vector<quint32> prevObjects;
    std::vector<quint32> prevFaces;
    // Copies of generated objects (stairs) become new generated objects.
    bool procAdded = false;
    std::vector<eng::Scene::ProcObject> procBefore;
    // Their collision ramps get entities of their own.
    std::vector<eng::Entity> entitiesAdded;
    // Copies of the selected point entities: in the scene once spawned, recorded where they ended up.
    std::vector<quint32> entityClones;
    std::vector<quint32> prevEntities;
    // Modified objects selected as such: copies of their sources make new modified objects.
    std::set<quint32> wholeHosts;
  };
  void cloneProcObjects(const std::vector<quint32> &originals,
                        const std::vector<std::shared_ptr<eng::GameObject>> &clones, DuplicateSet &set);
  // Copies of all of a brush entity's brushes get a new entity of its class (props, unique name); copies of
  // some of them stay in it (vk_brush_entities.cpp).
  void cloneBrushEntities(const std::vector<quint32> &originals,
                          const std::vector<std::shared_ptr<eng::GameObject>> &clones, DuplicateSet &set);

  // Deep copy of one brush: own buffers, same materials, mapping from `baseBases` if given (else the live
  // faces), picking ids re-minted for the clone's stable id.
  [[nodiscard]] static std::shared_ptr<eng::GameObject>
  cloneBrush(const std::shared_ptr<eng::GameObject> &src, const glm::vec3 &pos,
             const std::vector<eng::FaceTexBasis> &baseBases);

  // A world-aligned box brush over [mn, mx], wound inward like VMF brushes. Picking ids are the caller's (they
  // need the object's stable id). `faceBases` / `faceMaterials` override the per-face mapping in face order
  // 0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z (cut pieces keep the source's faces).
  [[nodiscard]] static std::shared_ptr<eng::Mesh>
  buildBoxMesh(const glm::vec3 &mn, const glm::vec3 &mx,
               const QSharedPointer<eng::Material> &material,
               const eng::FaceTexBasis *faceBases = nullptr,
               const QSharedPointer<eng::Material> *faceMaterials = nullptr);

  // Unrotated, six axis-aligned faces. `faceSlots` (buildBoxMesh's order): the source face per side, or -1.
  [[nodiscard]] static bool isBoxBrush(const std::shared_ptr<eng::GameObject> &obj,
                                      std::array<int, 6> &faceSlots);
  // One half of a cut brush, in world coordinates with the object at the origin.
  [[nodiscard]] static std::shared_ptr<eng::GameObject>
  buildCutPiece(const std::shared_ptr<eng::GameObject> &src,
                const std::array<int, 6> &faceSlots, const glm::vec3 &mn, const glm::vec3 &mx);

  // Box minus prism as the frame around the opening: two side strips plus lintel and sill, up to four
  // disjoint brushes per source.
  [[nodiscard]] bool applyCutout();
  void relayoutCutHoles();
  // False when nothing is selected.
  [[nodiscard]] bool selectionWorldAABB(glm::dvec3 &mn, glm::dvec3 &mx) const;

  // Clones the dragged brushes, puts the originals back at `basePos` with their grabbed mapping and selects
  // the clones; `ids`, `basePos` and `baseUv` are rewritten to the clones in the same order.
  bool spawnDuplicates(std::vector<quint32> &ids, std::vector<glm::vec3> &basePos,
                       std::vector<DragFaceUv> &baseUv, DuplicateSet &set);
  // The same for the point entities in `entBase` (never world_env or brush entities). True when anything was
  // copied (`set.triggered`).
  bool spawnEntityDuplicates(std::map<quint32, glm::vec3> &entBase, DuplicateSet &set);
  // Records the spawned clones as one undo step and forgets them.
  void pushDuplicateUndo(DuplicateSet &set, const QString &label);
  // Plus the clones' mirror copies when mirror mode is on.
  void pushDuplicateUndoMirrored(DuplicateSet &set, const QString &label);
  DuplicateSet m_dragDuplicate;
  DuplicateSet m_gizmoDuplicate;

  CutMode m_cutMode = CutMode::Off;
  CutLine m_cutLine {};
  // The line exists only once the drag leaves its start cell; until then the existing line stays.
  struct CutDrag {
    bool active = false;
    bool alongRight = false;
    int plane = 0;
    double step = 1.0;
    glm::dvec2 start {0.0, 0.0};
  };
  CutDrag m_cutDrag {};

  CutShape m_cutShape = CutShape::Slice;
  int m_cutOriginPlane = 0;
  std::vector<CutHole> m_cutHoles {};
  double m_cutoutW = kDoorWidthSource;
  double m_cutoutH = kDoorHeightSource;
  int m_cutTemplate = 0;
  double m_cutoutSill = kWindowSillFraction;
  bool m_cutoutAutoFill = false;
  double m_cutoutSpacing = kAutoFillSpacingSource;
  double m_cutoutMargin = kAutoFillMarginSource;
  bool m_cutHoleDrag = false;
  glm::dvec2 m_cutHoleGrab {0.0, 0.0};
  std::vector<double> m_cutHoleGrabCenters {};
  // The last aim: option changes after placement re-lay the same row.
  bool m_cutHoleAimed = false;
  int m_cutHolePlane = 0;
  glm::dvec2 m_cutHoleAim {0.0, 0.0};
  double m_cutHoleStep = 1.0;

  FillMode m_fillMode {FillMode::Facade};
  bool m_fillWrap = false;
  double m_fillMinAreaSource = 600.0;
  double m_fillMinSpanSource = 20.0;
  double m_fillClearSource = 240.0;
  // Off by default: with layers from the floor slabs, wall pieces off the storey lines are normal and the test
  // drops real wall faces.
  int m_fillStoreySpanPct = 0;
  bool m_fillDetectFloors = true;
  bool m_fillWireVisible = true;
  bool m_maskHidden = false;
  bool m_fillAvailable = false;
  QString m_fillNote;
  // The faces the last fill produced, and the layer bounds it was cut into.
  std::vector<quint32> m_fillFaces;
  // The last fill's seed, for re-runs with other options.
  quint32 m_fillSeed = 0;
  std::vector<double> m_fillBounds;
  std::vector<FillLayer> m_fillLayers;
  // Every facade the run covers, seed first; the shared bands are drawn on each.
  std::vector<eng::Facade> m_fillFacades;
  // The run's planes, so a slice can rebuild the fill without the seed face.
  std::vector<std::pair<int, double>> m_fillPlanes;
  glm::dvec3 m_fillMin {0.0};
  glm::dvec3 m_fillMax {0.0};
  bool m_fillExtentValid = false;
  int m_fillDragIndex = -1;
  int m_fillDragPlane = 0;
  double m_fillDragStep = 1.0;

  // Records an already-applied property edit; skipped while the stack replays, so commands can call the
  // recording setter itself.
  template <typename T, typename Setter>
  void recordValue(const QString &key, const QString &label, T before, T after, Setter setter) {
    if (!m_undoStack || m_undoStack->isApplying() || m_preview)
      return;
    m_undoStack->push(std::make_unique<eng::ValueCommand<T>>(
        key, label, std::move(before), std::move(after),
        [this, setter](const T &value) { (this->*setter)(value); }));
  }

  // Records a finished move / resize, only what changed: nothing for a no-op drag, no face bases without a
  // texture-lock rewrite (no per-face re-upload on undo).
  void recordTransformEdit(const QString &label, const std::vector<quint32> &ids,
                          const std::vector<glm::vec3> &basePos,
                          const std::vector<glm::vec3> &baseScale,
                          const std::vector<DragFaceUv> &baseUv);
  // The sun setters' write without recording (azimuth / elevation record under their own keys).
  void applySunDirection(const glm::vec3 &dir);
  void applyTextureLockMove(const std::vector<quint32> &ids, const std::vector<DragFaceUv> &base,
                           const glm::vec3 &delta) const;
  // The captured face bases carried through `m` (eng::transformFaceBasis), for the live rotate preview.
  void applyFaceUvTransform(const std::vector<quint32> &ids, const std::vector<DragFaceUv> &base,
                            const glm::dmat4 &m) const;
  // Puts the captured face bases back (before a commit that starts from them).
  void restoreFaceUv(const std::vector<quint32> &ids, const std::vector<DragFaceUv> &base) const;
  void applyTextureScaleLock(const std::vector<quint32> &ids, const std::vector<DragFaceUv> &base,
                            int axisA, double factorA, int axisB, double factorB,
                            const glm::vec3 &anchor) const;

  // The face a picking id names, checked against the mesh.
  [[nodiscard]] static int faceIndexOfPicking(const std::shared_ptr<eng::Mesh> &mesh,
                                             quint32 pickingId);
  // False when nothing is picked.
  [[nodiscard]] bool firstPickedBasis(eng::FaceTexBasis &out) const;
  void eachPickedBasis(const std::function<void(eng::FaceTexBasis &)> &edit);
  std::vector<DragFaceUv> m_dragBaseUv;

  QRhiGraphicsPipeline *m_marqueePipeline = nullptr;
  QRhiShaderResourceBindings *m_marqueeSrb = nullptr;
  QRhiBuffer *m_marqueeVb = nullptr;
  QRhiResourceUpdateBatch *m_marqueeBatch = nullptr;
  quint32 m_marqueeVertexCount = 0;
  QVector<float> m_marqueeVerts;

  // 3D cut-out preview: the marquee's thick box, red, drawn through occluders, one per opening.
  QRhiGraphicsPipeline *m_cutWirePipeline = nullptr;
  QRhiShaderResourceBindings *m_cutWireSrb = nullptr;
  QRhiBuffer *m_cutWireVb = nullptr;
  QRhiResourceUpdateBatch *m_cutWireBatch = nullptr;
  quint32 m_cutWireVertexCount = 0;
  QVector<float> m_cutWireVerts;
  // The part a slice removes, drawn see-through red after the outline.
  QRhiGraphicsPipeline *m_cutFillPipeline = nullptr;
  quint32 m_cutFillVertexCount = 0;
  // Collision brushes: translucent faces plus brighter edges, depth tested.
  bool m_showCollision = true;
  bool m_specular = true;
  bool m_noSpecular = false;
  float m_specularStrength = 2.5f;
  float m_normalStrength = 1.0f;
  bool m_normalMaps = true;
  bool m_showTriggers = true;
  mutable std::set<quint32> m_collisionEntities;
  mutable std::set<quint32> m_triggerEntities;
  mutable quint64 m_collisionEntitiesKey = ~0ull;
  void refreshToolEntitySets() const;
  quint64 m_collisionSig = 0;
  quint64 m_collisionBuiltKey = ~0ull;
  // I/O connection lines (vk_io_lines.cpp).
  void setupIOLinePipelines(QRhiRenderPassDescriptor *rp, int sampleCount);
  void releaseIOLinePipelines();
  void buildIOLineGeometry();
  int buildIOLabels();
  void drawIOLines(QRhiCommandBuffer *cb);
  QRhiGraphicsPipeline *m_ioLineVisiblePipeline = nullptr;
  QRhiGraphicsPipeline *m_ioLineHiddenPipeline = nullptr;
  QRhiBuffer *m_ioLineVb = nullptr;
  QRhiResourceUpdateBatch *m_ioLineBatch = nullptr;
  quint32 m_ioLineVertexCount = 0;
  struct IOLabel {
    glm::vec3 pos;
    QString text;
    glm::vec4 color;
  };
  std::vector<IOLabel> m_ioLabelQueue;
  QRhiGraphicsPipeline *m_collisionFillPipeline = nullptr;
  QRhiGraphicsPipeline *m_collisionEdgePipeline = nullptr;
  QRhiBuffer *m_collisionVb = nullptr;
  QRhiResourceUpdateBatch *m_collisionBatch = nullptr;
  quint32 m_collisionFillCount = 0, m_collisionEdgeCount = 0;
  void refreshCollisionSig();
  void setupCollisionPipelines(QRhiRenderPassDescriptor *rp, int sampleCount);
  void releaseCollisionPipelines();
  void buildCollisionOverlay();
  void drawCollisionOverlay(QRhiCommandBuffer *cb);

  // 3D layer preview: one quad per band on the wall plane, through the selection's barycentric wire pipeline.
  QRhiBuffer *m_layerObjUbo = nullptr;
  QRhiBuffer *m_layerOverlayWallUbo = nullptr;
  QRhiBuffer *m_layerOverlaySpaceUbo = nullptr;
  QRhiShaderResourceBindings *m_layerSrbWall = nullptr;
  QRhiShaderResourceBindings *m_layerSrbSpace = nullptr;
  QRhiBuffer *m_fillWireVb = nullptr;
  QRhiBuffer *m_fillWireIb = nullptr;
  quint32 m_fillWireCapacity = 0;
  quint32 m_fillWireIbCapacity = 0;
  QRhiResourceUpdateBatch *m_fillWireBatch = nullptr;
  quint32 m_fillWireWallIdxCount = 0;
  quint32 m_fillWireSpaceIdxCount = 0;
  std::vector<eng::WireVertex> m_fillWireVerts;
  std::vector<uint32_t> m_fillWireIdx;

  // Azimuth 56, elevation 32.5: must match eng::DirectionalLight's default.
  glm::vec3 m_sunDirection { -0.288457f, -0.537300f, 0.792528f };
  glm::vec3 m_sunColor { 1.0f, 0.969f, 0.902f };
  float m_sunIntensity = 5.0f;
  glm::vec3 m_ambientColor { 37.0f / 255.0f, 44.0f / 255.0f, 57.0f / 255.0f };  // #252c39
  float m_ambientIntensity = 0.7f;
  float m_fogDensity = 0.0006f;
  glm::vec3 m_fogColor {0.11f, 0.13f, 0.19f};
  float m_exposure = 1.0f;  // Godot's Environment tonemap_exposure default
  float m_wireframeThicknessPx = 2.5f;
  int m_gridLevelOffset = kGridLevelDefaultOffset;
  bool m_textureLock = false;
  bool m_textureScaleLock = false;
  int m_msaaSamples = 2;
  float m_renderDistanceSource = kRenderDistanceDefaultSource;
  float m_lightRenderDistanceSource = kLightRenderDistanceDefaultSource;
  float m_shadowRenderDistanceSource = kShadowRenderDistanceDefaultSource;

  bool m_shadowsEnabled = true;
  float m_shadowBias = 0.0016f;
  float m_shadowNormalBias = 1.14f;
  float m_shadowRadius = 0.9f;
  float m_shadowStrength = 1.0f;
  float m_shadowFar = 440.0f;

  // Set while pushing into the scene light, so its change signals do not bounce back.
  bool m_pushingSunToLight = false;
  eng::DirectionalLight *m_sceneLight = nullptr;

  float m_cameraSpeed = 55.0f;
  float m_flyShiftMultiplier = 3.0f;
  float m_flySmoothing = 0.0f;
  glm::vec3 m_flyVelocity {0.0f};
  float m_gizmoScreenPx = 120.0f;
  int m_tonemapper = 0;
  float m_lastYaw = 1e9f, m_lastPitch = 1e9f;
  bool m_prevFrameAnimating = false;
  int m_shadowFilter = 1;
  int m_shadowTaps = 16;
  bool m_ssaoAllowed = true;
  bool m_skyEnabled = true;
  quint64 m_sceneBoundsKey = ~0ull;
  int m_cascadeCount = 4;
  double m_cascadeLambda = 0.75;
  bool m_cascadeDebug = false;
  QRhiBuffer *m_cascadeUbo = nullptr;
  int m_cascadeStride = 256;
  CascadeBlock m_cascades[4];
  void fitCascades(const glm::vec3 &sceneMin, const glm::vec3 &sceneMax, const glm::vec3 &sunDir,
                   const glm::vec3 &right, const glm::vec3 &lightUp, const glm::mat4 &corr);
  glm::vec3 m_sceneBoundsMin {0.0f}, m_sceneBoundsMax {0.0f};
  std::unique_ptr<QRhiGraphicsPipeline> m_skyPipeline;
  QRhiShaderResourceBindings *m_skySrb = nullptr;
  bool m_showLabels = true;
  QColor m_clearColor {20, 24, 37};
  bool m_backgroundOverride = false;
  bool m_freeLook = false;
  std::array<bool, 128> m_keysPressed {};
public:
  void releaseAllKeys();

  // edit mode: the vertex tool (vk_vertex_edit.cpp)
  // Vertex, edge and face handles plus a centre per brush. The selection is a set of corners (an edge / face
  // handle stands for its corners). Every edit is one undo step.
  struct VertexHandle {
    quint32 brush = 0;
    glm::vec3 pos {0.0f};
  };
  enum class EditHandleKind { Vertex = 0, Edge = 1, Face = 2, Centre = 3, Pivot = 4 };
  struct EditHandle {
    EditHandleKind kind = EditHandleKind::Vertex;
    quint32 brush = 0;
    glm::vec3 pos {0.0f};
    std::vector<glm::vec3> verts; // the corners this handle stands for
  };
  struct EditHandleDraw {
    glm::vec3 pos {0.0f};
    int kind = 0;
    bool selected = false;
    bool invalid = false;
  };
  // 0 = vertices + edges (default), 1 = vertices, 2 = edges, 3 = faces.
  enum VertexSubMode { SubVertsEdges = 0, SubVerts = 1, SubEdges = 2, SubFaces = 3 };
  void setEditMode(bool on);
  [[nodiscard]] bool editMode() const { return m_editMode; }
  [[nodiscard]] int vertexSubMode() const { return m_vertexSub; }
  void setVertexSubMode(int mode);
  void cycleVertexSubMode();
  [[nodiscard]] std::vector<EditHandle> editHandles() const;
  [[nodiscard]] std::vector<EditHandleDraw> editHandleDraws() const;
  [[nodiscard]] std::vector<VertexHandle> vertexHandles() const;
  [[nodiscard]] const std::vector<VertexHandle> &selectedVertices() const { return m_selectedVerts; }
  [[nodiscard]] bool vertexSelected(quint32 brush, const glm::vec3 &p) const;
  [[nodiscard]] bool handleSelected(const EditHandle &h) const;
  // 3D view click. True when a handle was hit.
  bool pickVertexHandle(float x, float y, bool additive);
  // 2D click (tol in world units); handles stacked in depth are picked together. `hit` gets the handle.
  bool pickEditHandle2D(int plane, const glm::dvec2 &p, double tol, bool additive, EditHandle *hit = nullptr);
  // 2D rubber band, ignoring depth.
  int selectHandlesInBox(int plane, const glm::dvec2 &a, const glm::dvec2 &b, bool additive);
  void selectAllVertices();
  void clearVertexSelection();
  [[nodiscard]] glm::vec3 selectedVertexCentre() const;
  [[nodiscard]] quint64 vertexEditEpoch() const { return m_vertexEpoch; }
  [[nodiscard]] int selectedVertexCount() const { return int(m_selectedVerts.size()); }
  // Gizmo drag in the 3D view.
  void beginVertexDrag();
  void updateVertexDrag(const glm::vec3 &delta);
  void endVertexDrag(const glm::vec3 &delta);
  // 2D drag. `splitFrom` (an edge / face handle): pulls a new corner out of that point instead of moving.
  void beginVertexPlaneDrag(int plane, const glm::dvec2 &p, double step, const glm::vec3 &anchor,
                            std::optional<std::pair<quint32, glm::vec3>> splitFrom = std::nullopt);
  void updateVertexPlaneDrag(const glm::dvec2 &p);
  void endVertexPlaneDrag();
  [[nodiscard]] bool vertexPlaneDragging() const { return m_vertexPlaneDrag.active; }
  void nudgeVertices(const glm::vec3 &delta);
  bool weldSelectedVertices();
  bool snapSelectedVertices();
  // Adds each selected edge's midpoint pushed out by `bulge` (0 = one grid step): on the line the hull would
  // drop it.
  bool splitSelectedEdges(double bulge);
  // Vertex scaling about a pivot (VertexScaleControls.qml).
  Q_INVOKABLE bool beginVertexScale();
  Q_INVOKABLE void commitVertexScale();
  Q_INVOKABLE void cancelVertexScale();
  Q_INVOKABLE void resetVertexPivot();
  [[nodiscard]] bool vertexScaleActive() const { return m_vertexScaleActive; }
  [[nodiscard]] double vertexScale() const { return m_vertexScale; }
  void setVertexScale(double s);
  [[nodiscard]] int vertexScaleAxes() const { return m_vertexScaleAxes; }
  void setVertexScaleAxes(int mask);
  [[nodiscard]] QVector3D vertexPivot() const { return {m_vertexPivot.x, m_vertexPivot.y, m_vertexPivot.z}; }
  void setVertexPivot(const QVector3D &p);
  void moveVertexPivotPlane(int plane, const glm::dvec2 &p, double step);
  [[nodiscard]] int vertexEditPlane() const { return m_vertexEditPlane; }
  // Corners re-attached to the brushes now in the scene (after undo / redo).
  void remapSelectedVertices();
  bool m_editMode = false;
  bool m_vertexDragging = false;
  glm::vec3 m_vertexDragDelta {0.0f};
  std::vector<VertexHandle> m_selectedVerts;
  std::vector<VertexHandle> m_vertexDragBase;
  std::map<quint32, eng::ConvexBrush> m_vertexDragOriginal;
  struct SavedShape {
    std::shared_ptr<eng::Mesh> mesh;
    glm::vec3 pos, rot, scale;
  };
  std::map<quint32, SavedShape> m_vertexDragMeshes;

private:
  using VertexMapper = std::function<glm::dvec3(quint32 brush, const glm::dvec3 &p)>;
  void beginVertexEdit();
  void previewVertexEdit(const VertexMapper &mapper, const std::map<quint32, std::vector<glm::dvec3>> &extra = {});
  bool finishVertexEdit(bool commit, const QString &label);
  bool buildEditedBrush(quint32 brushId, eng::ConvexBrush &out, int *absorbed = nullptr) const;
  bool applyVertexEditNow(const VertexMapper &mapper, const QString &label,
                          const std::map<quint32, std::vector<glm::dvec3>> &extra = {});
  void touchVertexEdit();
  int m_vertexSub = SubVertsEdges;
  quint64 m_vertexEpoch = 1;
  VertexMapper m_vertexMapper;
  std::map<quint32, std::vector<glm::dvec3>> m_vertexExtra;
  bool m_vertexEditOpen = false;
  int m_vertexAbsorbed = 0;
  bool m_vertexScaleActive = false;
  double m_vertexScale = 1.0;
  int m_vertexScaleAxes = 7;
  glm::vec3 m_vertexPivot {0.0f};
  int m_vertexEditPlane = 0;
  struct VertexPlaneDrag {
    bool active = false;
    int plane = 0;
    glm::dvec2 start {0.0};
    glm::vec3 anchor {0.0f};
    double step = 1.0;
    bool split = false;
    quint32 splitBrush = 0;
    glm::vec3 splitFrom {0.0f};
    glm::vec3 delta {0.0f};
  } m_vertexPlaneDrag;

public:
  // point entities (vk_entities.cpp)
  [[nodiscard]] const eng::Entity *entityById(quint32 id) const;
  [[nodiscard]] std::vector<eng::Entity> *sceneEntities() const;
  quint32 addEntity(const QString &classname, const glm::vec3 &origin, const QVariantMap &props = {});
  // Fills the (new, empty) level with File › New › Playground Level's room, crate and light (vk_playground.cpp).
  void buildPlayground();
  // brush entities (vk_brush_entities.cpp)
  // Ties the selected brushes to a new entity of `classname`; one undo step. Returns the entity id.
  quint32 makeBrushEntity(const QString &classname);
  // Unties the selected brushes; entities left without brushes are removed.
  bool moveSelectionToWorld();
  // Of the selected brushes (groups and brush entities included): 1 all smooth, 0 none, -1 mixed / no brush.
  [[nodiscard]] int selectionSmoothShading() const;
  void setSelectionSmoothShading(bool on);
  // Unties the selected brushes from any brush entity and puts them in worldspawn, one undo step.
  void moveSelectionToWorldspawn();
  [[nodiscard]] std::vector<quint32> brushesOfEntity(quint32 entityId) const;
  [[nodiscard]] bool selectionHasBrushEntity() const;
  // A picked brush brings the rest of its unit: brush entity, generated object, group (not worldspawn).
  void expandSelectionToBrushEntities();
  // Takes a brush's whole unit out of the selection.
  void dropSelectionUnitOf(quint32 id);
  // Brush entity origins = the centre of their brushes' bounds.
  void syncBrushEntityOrigins();
  void deleteEntities(const std::vector<quint32> &ids);
  // collision brushes (vk_collision.cpp)
  // Tied to a func_clip: drawn see-through, never in the lit scene or the shadows.
  [[nodiscard]] bool isCollisionBrush(const eng::GameObject &o) const;
  // Tied to a trigger_*: drawn see-through, never lit.
  [[nodiscard]] bool isTriggerBrush(const eng::GameObject &o) const;
  // Left out of the lit scene, shadows and SSAO.
  [[nodiscard]] bool isToolBrush(const eng::GameObject &o) const { return isCollisionBrush(o) || isTriggerBrush(o); }
  // Hidden by the View menu (not drawn, not pickable).
  [[nodiscard]] bool toolBrushHidden(const eng::GameObject &o) const;
  [[nodiscard]] bool showCollision() const { return m_showCollision; }
  void setShowCollision(bool on);
  // Pref Render/Specular: highlights and ambient reflection (off: diffuse only).
  [[nodiscard]] bool specularEnabled() const { return m_specular; }
  void setSpecularEnabled(bool on);
  // This engine never draws specular, whatever the preference (the project splash).
  void setNoSpecular(bool on) {
    m_noSpecular = on;
    ++m_viewEpoch;
    requestRedraw();
  }
  [[nodiscard]] bool normalMapsEnabled() const { return m_normalMaps; }
  void setNormalMapsEnabled(bool on);
  [[nodiscard]] bool showTriggers() const { return m_showTriggers; }
  void setShowTriggers(bool on);
  [[nodiscard]] eng::MaterialPtr triggerMaterial() const;
  [[nodiscard]] eng::MaterialPtr collisionMaterial() const;
  // Selection-box bounds: generated objects' collision ramps (which reach past the steps) are left out unless
  // nothing else is listed.
  bool objectsBounds(const std::vector<quint32> &ids, glm::dvec3 &mn, glm::dvec3 &mx) const;
  [[nodiscard]] bool isProcRamp(const eng::GameObject &o) const { return o.procObject() && isCollisionBrush(o); }
  // A new func_clip entity outside the undo stack (the caller records it).
  quint32 addCollisionEntityRaw(const glm::vec3 &origin);
  void setEntityProperty(quint32 id, const QString &key, const QVariant &value);
  // Back to the class default (the key is dropped from Entity::props); undoable.
  void resetEntityProperty(quint32 id, const QString &key);
  // Several entities at once: one undo step; repeated sets merge.
  void setEntitiesProperty(const std::vector<quint32> &ids, const QString &key, const QVariant &value);
  void resetEntitiesProperty(const std::vector<quint32> &ids, const QString &key);
  void setEntityTransform(quint32 id, const glm::vec3 &origin, const glm::vec3 &angles, const QString &name,
                          bool record = true);
  void recordEntityMoves(const std::map<quint32, glm::vec3> &before, const QString &label);
  [[nodiscard]] std::map<quint32, glm::vec3> selectedEntityOrigins() const;
  void moveSelectedEntities(const std::map<quint32, glm::vec3> &base, const glm::vec3 &delta);
  [[nodiscard]] bool markerSelected(quint32 id) const;
  void selectMarker(quint32 id, bool additive);
  void setSelectedEntities(const std::vector<quint32> &ids);

  // Camera3D preview (vk_entities.cpp): looks through the selected camera (position, pitch / yaw, FOV); the
  // editor camera is restored afterwards. Flying is off while previewing.
  [[nodiscard]] bool cameraPreviewAvailable() const { return selectedCameraEntity() != 0; }
  [[nodiscard]] bool cameraPreview() const { return m_previewCamera != 0; }
  void setCameraPreview(bool on);
  [[nodiscard]] const std::set<quint32> &selectedEntities() const { return m_selectedEntities; }
  void appendEntityMarkers(QVector<EntityMarker> &markers) const;
  int fillLightUniforms(glm::vec4 *out, int maxLights, std::vector<quint32> *ids = nullptr) const;

  // entity light shadows (vk_light_shadows.cpp)
  struct LightShadowJob {
    quint32 entity = 0;
    int firstTile = 0;
    int tileCount = 0;
    glm::vec3 position {0.0f};
    float range = 1.0f;
    float tanHalf = 1.0f;
    LightShadowFace faces[6];
  };
  void ensureLightAtlas();
  void prepareLightShadows();
  void renderLightShadowAtlas(QRhiCommandBuffer *cb, QRhiResourceUpdateBatch *rub, bool &rubSubmitted);
  void fillLightShadowUniforms();
  void assignLightShadowSlot(quint32 entityId, int lightIndex);
  std::vector<LightShadowJob> m_lightJobs;
  std::map<quint32, int> m_lightShadowTile;
  QRhiTexture *m_lightAtlasTex = nullptr;
  QRhiRenderBuffer *m_lightAtlasDepth = nullptr;
  QRhiTextureRenderTarget *m_lightAtlasRt = nullptr;
  QRhiRenderPassDescriptor *m_lightAtlasRp = nullptr;
  QRhiBuffer *m_lightFaceUbo = nullptr;
  int m_lightFaceStride = 256;
  std::unique_ptr<QRhiGraphicsPipeline> m_lightAtlasPipeline;
  QRhiShaderResourceBindings *m_lightAtlasSrb = nullptr;
  int m_lightAtlasSize = 0;
  quint64 m_lightAtlasKey = ~0ull;
  void captureEntities(std::vector<fmap::EntityRecord> &out) const;
  bool applyEntity(const fmap::EntityRecord &record);
  [[nodiscard]] bool noLocalLights() const { return m_noLocalLights; }
  [[nodiscard]] const eng::Entity *sunEntity() const;
  // One render object per prop_static entity. `blocking` loads missing models synchronously (tests, export).
  void syncPropObjects(bool blocking = false);
  struct PropSlot {
    QString path;
    quint32 stableId = 0;
  };
  std::map<quint32, PropSlot> m_propObjects;
  quint64 m_propEpoch = ~0ull;
  void applySunEntity();
  // Nearest brush hit along a ray (world space), any side.
  bool raycastScene(const glm::vec3 &origin, const glm::vec3 &dir, float &t, glm::vec3 &normal,
                    quint32 *object = nullptr, int *face = nullptr) const;
  // The surface point under view pixel (px,py), lifted along the normal; negative = the view centre.
  [[nodiscard]] glm::vec3 placementPoint(float px, float py, float lift) const;
  Q_INVOKABLE quint32 placeEntityAt(const QString &classname, float px = -1, float py = -1);
  bool applyMaterialAt(const QString &material, float px, float py);
  std::set<quint32> m_selectedEntities;
  std::map<quint32, glm::vec3> m_gizmoEntBase;
  std::map<quint32, glm::vec3> m_dragEntBase;
  quint64 m_entityEpoch = 0;
  bool m_noLocalLights = false;

  // clipboard (vk_brush_ops.cpp)
  bool copySelection();
  bool cutSelection();
  // uniqueNames: pasted entities get fresh targetnames and the connections inside the pasted set follow them.
  bool paste(bool inPlace, bool uniqueNames = false);
  [[nodiscard]] bool canPaste() const { return !m_clipBrushes.empty() || !m_clipEntities.empty(); }
  std::vector<eng::ConvexBrush> m_clipBrushes;
  std::vector<eng::Entity> m_clipEntities;
  std::vector<quint32> m_clipBrushOwner; // brush entity of each m_clipBrushes entry (0 = world)
  // Parallel to m_clipBrushes: orientation and mirror link / side / set (relinkCopies).
  struct ClipBrushMeta {
    glm::dquat orientation {1.0, 0.0, 0.0, 0.0};
    bool linked = false;
    eng::Scene::MirrorLink link;
    int side = 0;
    eng::Scene::MirrorSet set;
    quint32 host = 0; // a source of this copied modified object (m_clipHosts)
    // A source brush copied on its own: the paste joins that modified object if it still exists by that name.
    quint32 joinHost = 0;
    QString joinHostName;
  };
  std::vector<ClipBrushMeta> m_clipMeta;
  std::map<quint32, eng::mods::Host> m_clipHosts;
  int m_pasteCount = 0;

  // entity I/O (vk_io.cpp)
  // One undo step; `text` names it in the history.
  void setEntityOutputs(quint32 id, const eng::io::ConnectionList &outputs, const QString &text = {});
  // Renames; with pref IO/RenameUpdatesReferences also every target / Target prop with the old name. One step.
  void renameEntity(quint32 id, const QString &name);
  // Adds a copy of `tmpl` (name, angles, props, outputs) at `origin`; undoable.
  quint32 addEntityFrom(const eng::Entity &tmpl, const glm::vec3 &origin);
  // "door" -> the first free "door_N" (names compared case-insensitively).
  [[nodiscard]] QString uniqueEntityName(const QString &base, const QSet<QString> &reserved = {}) const;
  // The single selected entity, or the brush entity whose brushes are selected; 0 = none.
  [[nodiscard]] quint32 ioFocusEntity() const;
  // Selected entities plus the brush entities of selected brushes.
  [[nodiscard]] std::vector<quint32> ioSelectionEntities() const;
  void selectEntitiesWithBrushes(const std::vector<quint32> &ids);
  int selectIOTargets();
  int selectIOSources();
  int selectSameName();
  [[nodiscard]] const std::vector<IOLink> &ioLinks() const;
  [[nodiscard]] bool showIOLines() const { return m_showIOLines; }
  void setShowIOLines(bool on);
  [[nodiscard]] bool ioLinesAll() const { return m_ioLinesAll; }
  void setIOLinesAll(bool on);
  // The next entity click in any view calls `onPicked` instead of selecting.
  void beginTargetPick(std::function<void(quint32)> onPicked);
  void cancelTargetPick();
  [[nodiscard]] bool targetPickActive() const { return bool(m_targetPick); }
  bool deliverTargetPick(quint32 entityId);
  // A new entity next to `near` (or at the view centre); undoable, selected.
  quint32 addEntityNear(const QString &classname, quint32 near);
  // I/O Editor node positions (IOGL chunk; view state, no undo).
  [[nodiscard]] bool ioGraphPos(quint32 id, glm::vec2 *pos) const;
  void setIOGraphPos(quint32 id, const glm::vec2 &pos);
  std::map<quint32, glm::vec2> m_ioGraphLayout;
  void captureConnections(std::vector<fmap::EntityConnectionsRecord> &out) const;
  void applyConnections(const std::vector<fmap::EntityConnectionsRecord> &records);
  // I/O simulator (vk_io_sim.cpp)
  // Runs the map's logic on a copy of the entities; the document is untouched.
  [[nodiscard]] bool ioSimRunning() const;
  void ioSimStart();
  void ioSimStop();
  [[nodiscard]] bool ioSimPaused() const { return m_ioSimPaused; }
  void setIOSimPaused(bool paused);
  int ioSimStep(int n = 1); // pauses, then delivers n events
  // ent_fire / ent_fire_output / IO.raise; each starts the simulation if needed.
  int ioSimFire(const QString &target, const QString &input, const QString &param = {}, double delay = 0.0);
  bool ioSimFireOutput(quint32 entity, const QString &output, const QString &param = {});
  int ioSimRaise(const QString &eventName, const QString &param = {});
  [[nodiscard]] QStringList ioSimQueue() const;
  [[nodiscard]] double ioSimTime() const;
  [[nodiscard]] QString ioSimState(quint32 id) const;
  [[nodiscard]] bool ioSimKilled(quint32 id) const;
  [[nodiscard]] bool ioSimFlashing(quint32 id) const;
  [[nodiscard]] std::vector<IOPulse> ioPulses() const;
  // The same pulses by entity (for the I/O graph's edges).
  struct IOPulseLink {
    quint32 from = 0, to = 0;
    float t = 0.0f;
  };
  [[nodiscard]] std::vector<IOPulseLink> ioPulseLinks() const;
  void ioSimTick();
  void drainIOSimLog();
  struct ActivePulse {
    quint32 from = 0, to = 0;
    double t0 = 0.0, t1 = 0.0;
  };
  std::unique_ptr<eng::io::Simulator> m_ioSim;
  std::unique_ptr<QElapsedTimer> m_ioSimClock;
  QTimer *m_ioSimTimer = nullptr;
  bool m_ioSimPaused = false;
  std::vector<ActivePulse> m_ioPulses;
  std::map<quint32, double> m_ioFlash;

  bool m_showIOLines = true;
  bool m_ioLinesAll = true;
  bool m_ioLabels = true;
  mutable std::vector<IOLink> m_ioLinks;
  mutable quint64 m_ioLinksKey = ~0ull;
  std::function<void(quint32)> m_targetPick;

  // brush geometry (vk_brush_ops.cpp)
  // eng::PrimitiveKind: 0 box, 1 wedge, 2 cylinder, 3 cone, 4 spike, 5 sphere, 6 stairs, 7 arch, 8 pyramid.
  bool createPrimitive(int kind, const glm::vec3 &mn, const glm::vec3 &mx);
  Q_INVOKABLE bool createPrimitiveFromMarquee(int kind);
  Q_INVOKABLE int csgSubtract();
  Q_INVOKABLE int csgHollow(double thicknessSource = 0.0);
  Q_INVOKABLE bool csgMerge(bool force = false);
  // True when the last csgMerge refused because the hull would fill gaps.
  [[nodiscard]] bool lastMergeWouldFillGaps() const { return m_mergeWouldFillGaps; }
  bool m_mergeWouldFillGaps = false;
  // face tools (vk_face_tools.cpp)
  // Face indices per brush: Material-mode picks, or the vertex tool's fully selected face handles.
  [[nodiscard]] std::map<quint32, std::vector<int>> selectedBrushFaces() const;
  // Moves the selected faces' planes out (+) / in (-) by `distance` world units; newBrush: the swept volume
  // becomes a new brush. Returns the brushes changed / made.
  int extrudeSelectedFaces(double distance, bool newBrush);
  // `axis` moves in proportion to `byAxis`, about the selection's minimum corner.
  bool shearSelection(int axis, int byAxis, double factor);
  // World units (the gizmo / nudge snap step).
  [[nodiscard]] double gridStepWorld() const { return gizmoSnapStep(); }
  Q_INVOKABLE bool csgIntersect();
  // keep: 0 = behind the plane, 1 = in front, 2 = both pieces.
  int clipSelection(const glm::vec3 &normal, float distance, int keep);
  // A brush with no volume (ConvexBrush cannot hold it) cut by n·p = d through its face polygons. `sides`:
  // 1 behind, 2 in front, 3 both; `pieces`: the kept parts as new brushes, only when it is cut through.
  struct FlatSplit {
    int sides = 0;
    std::vector<std::shared_ptr<eng::GameObject>> pieces;
  };
  [[nodiscard]] FlatSplit splitFlatObject(const eng::GameObject &obj, const glm::dvec3 &n, double d, bool keepBack,
                                          bool keepFront) const;
  Q_INVOKABLE bool rotateSelection(int axis, double degrees);
  Q_INVOKABLE bool mirrorSelection(int axis);
  Q_INVOKABLE bool snapSelectionToGrid();
  // Moves without reshaping: the brushes as one block onto grid lines (per axis the nearer corner), point
  // entities to their cell's middle. One undo step; false when nothing moved.
  bool snapSelectionToGridCell();
  // `unmirrorMask` < 0: textures follow the texture lock; otherwise locked and,
  // for the reflected axes in the mask, flipped back (eng::unmirrorBasis).
  bool transformSelectionBrushes(const glm::dmat4 &m, const QString &label, int unmirrorMask = -1);
  // Orientation after `m`: rotations turn it, reflections mirror it (R' = M R M), scale / shear leave it.
  [[nodiscard]] static glm::dquat carryOrientation(const glm::dquat &q, const glm::dmat4 &m);
  // Brush rotation as Euler angles in degrees (Godot's YXZ order: R = Ry Rx Rz).
  [[nodiscard]] static glm::dvec3 eulerFromOrientation(const glm::dquat &q);
  [[nodiscard]] static glm::dquat orientationFromEuler(const glm::dvec3 &degrees);
  // False without brushes; `mixed` per axis when they disagree.
  bool selectionRotation(glm::dvec3 &degrees, bool mixed[3]) const;
  // Turns the selected brushes by the difference: about the selection centre when they share a rotation,
  // else each about its own centre. One undo step.
  void setSelectionRotation(int axis, double degrees);
  void resetSelectionRotation();

  // Modifiers (vk_modifiers.cpp)
  // A modified object (Scene::modHosts) owns source brushes (GameObject::modHost) and a stack (eng::mods).
  // Its output (derived modOutput GameObjects) is re-evaluated from sources, stack, paths and anchors: never
  // saved, never in an undo step (undo restores the inputs). Sources are hidden while the host has output,
  // except in Edit Source. Clicking output selects the host (m_selectedMods), so brush operations never
  // touch derived geometry.
  // Blocking ignores the time budget (load, edits); Required also runs while Show generated is off (exports).
  enum class ModPump { Frame, Poll, Blocking, Required };
  // Re-evaluates what changed. With Show generated off only Required evaluates; the output catches up later.
  void pumpModifiers(ModPump mode);
  [[nodiscard]] const std::set<quint32> &selectedMods() const { return m_selectedMods; }
  // Selected brushes plus the output of selected hosts.
  [[nodiscard]] const std::set<quint32> &drawSelection() const { return m_drawSelection; }
  void selectModHost(quint32 host, bool additive);
  void setSelectedMods(const std::vector<quint32> &hosts);
  // The Modifiers panel's host: the single selected host; in Edit Source the edited host while nothing else
  // is selected; a selection of one host's sources only; Material mode: the host whose output was clicked last.
  [[nodiscard]] quint32 panelModHost() const;
  [[nodiscard]] const eng::mods::Host *modHost(quint32 id) const;
  [[nodiscard]] std::vector<quint32> modSources(quint32 host) const;
  [[nodiscard]] std::vector<quint32> modOutputs(quint32 host) const;
  struct ModStatus {
    bool active = false, error = false;
    int outputs = 0;
    std::vector<eng::mods::Diagnostic> diagnostics;
    std::map<quint32, QString> summary;
  };
  [[nodiscard]] ModStatus modStatus(quint32 host) const;
  // Adds to the panel's host, or to a new host of the selected brushes. Returns the host (0 = refused, with a
  // toast). Arrays / curves run along the axis every source spans end to end, else `runHint` (a world
  // direction), else the longest side.
  quint32 addModifier(const QString &kind, const glm::dvec3 *runHint = nullptr);
  void removeModifier(quint32 host, quint32 mod);
  void moveModifier(quint32 host, quint32 mod, int delta);
  void setModifierEnabled(quint32 host, quint32 mod, bool on);
  void setModifierShowInEditSource(quint32 host, quint32 mod, bool on);
  void setModifierExpanded(quint32 host, quint32 mod, bool on); // panel state, not undone
  // Replaces a modifier's parameters; `mergeKey` merges a drag into one step.
  void setModifierParams(quint32 host, quint32 mod, const QJsonObject &params, const QString &mergeKey = {});
  void renameModHost(quint32 host, const QString &name);
  // Bakes the output (all modifiers or the first) into brushes; the sources go, the host too when its stack
  // is empty.
  bool applyModifiers(quint32 host, bool firstOnly);
  // Removes the host; its sources become ordinary brushes.
  void dissolveModHost(quint32 host);
  // Edit Source: the host's sources shown and editable, the output follows (0 = off).
  [[nodiscard]] quint32 editSourceHost() const { return m_editSourceHost; }
  // Object mode: entering selects the host's sources.
  void setEditSourceHost(quint32 host);
  void toggleEditSource();
  // Off: modifier output hidden everywhere, the sources shown instead.
  Q_PROPERTY(bool showModOutput READ showModOutput WRITE setShowModOutput NOTIFY editSourceChanged)
  [[nodiscard]] bool showModOutput() const { return m_showModOutput; }

  // zen mode (vk_zen.cpp)
  // The 2D views show only the selection and what touches a box around the 3D camera. Size in Source units as
  // Hammer reads them: x × y across, z up (pref Viewport/ZenBox).
  [[nodiscard]] bool zenMode() const { return m_zenOn; }
  void setZenMode(bool on);
  void setZenSizeSource(const glm::vec3 &size);
  // The box in world units; false while zen mode is off.
  bool zenBox(glm::vec3 &mn, glm::vec3 &mx) const;
  // Kept out of the 2D views (drawing, picking, fitting).
  [[nodiscard]] bool zenHides(const eng::GameObject &o) const;
  [[nodiscard]] bool zenHidesPoint(const glm::vec3 &p) const;
  // Bumped whenever what zen mode lets through may have changed; the 2D views compare it.
  [[nodiscard]] quint64 zenEpoch() const { return m_zenEpoch; }
  void updateZenBox(bool force = false);
  void setShowModOutput(bool show);
  // The box around each modified object's sources (the 3D view's green outline).
  [[nodiscard]] std::vector<std::pair<glm::dvec3, glm::dvec3>> modSourceBoxes() const;
  // Edit Source: adds the selected brushes to / takes them out of the host.
  void addSelectionToModHost();
  void removeSelectionFromModHost();
  // Plus their mirror twins: both sides of a link are sources of the same modified object.
  [[nodiscard]] std::set<quint32> withMirrorTwins(const std::set<quint32> &ids) const;
  // Removes the hosts left without sources (in the same undo step as the edit that emptied them).
  void dropEmptyModHosts(const std::set<quint32> &hosts, const QString &label);
  // After its first modifier ("Curve 2"), prefixed with the brushes' shared group ("test · Curve"); never a
  // mirror collection's name or one in use.
  [[nodiscard]] QString newModHostName(const QString &kind, quint32 group) const;
  // The selected brushes plus the sources of selected hosts not bound to a path.
  [[nodiscard]] std::vector<quint32> moveTargets() const;
  // Why a selected host cannot move (it follows a path), or empty.
  [[nodiscard]] QString modMoveBlocked() const;

  // Paths (vk_paths.cpp)
  [[nodiscard]] const std::set<quint32> &selectedPaths() const { return m_selectedPaths; }
  // Connect paths are built from their anchors; nullptr when missing or impossible.
  [[nodiscard]] std::shared_ptr<const eng::path::Curve> pathCurve(quint32 id);
  // Why a Connect path cannot be built (empty when it can).
  [[nodiscard]] QString pathError(quint32 id);
  // Hosts whose Curve follows the path.
  [[nodiscard]] std::vector<quint32> pathUsers(quint32 path) const;
  [[nodiscard]] const eng::path::Path *path(quint32 id) const;
  void selectPath(quint32 id, bool additive);
  void setPathHidden(quint32 id, bool hidden);
  void setPathLocked(quint32 id, bool locked);
  void renamePath(quint32 id, const QString &name);
  void deletePaths(const std::vector<quint32> &ids);
  // One path edited as a whole; `mergeKey` merges a field drag into one step.
  void editPath(quint32 id, const std::function<void(eng::path::Path &)> &fn, const QString &label,
                const QString &mergeKey = {});
  // Points of the path being edited (the single selected path).
  [[nodiscard]] quint32 pointsPath() const;
  [[nodiscard]] const std::set<int> &selectedPathPoints() const { return m_selectedPathPoints; }
  void setPathPointType(eng::path::HandleType type); // selected points
  void cyclePathPointType();
  bool deleteSelectedPathPoints();
  // A Connect path between the two selected anchor entities (true when made).
  bool connectSelectedAnchors();
  // A Connect path becomes a drawn one with its current shape.
  bool convertPathToDrawn(quint32 id);
  // The selected brushes (or modified object) follow a Connect path between the two selected anchors (Array
  // fit curve with World textures + Curve).
  bool brushesBetweenAnchors();
  // 2D: with only modified objects selected, one following a Connect path, the box's resize handles become
  // one handle per anchor (on the box side facing away from the gap); dragging one moves that anchor.
  [[nodiscard]] bool anchorHandleMode() const;
  struct AnchorHandle {
    quint32 anchor = 0;
    glm::dvec2 pos {0.0}; // in the view plane
  };
  // `box` is the selection box in that view's plane (a0, b0, a1, b1).
  [[nodiscard]] std::vector<AnchorHandle> anchorHandles2D(int plane, const glm::dvec4 &box) const;
  bool beginAnchorDrag(int plane, quint32 anchor, const glm::dvec2 &p, double step);
  void updateAnchorDrag(const glm::dvec2 &p);
  void endAnchorDrag();

  // The pen tool (2D views): a click adds a point, a drag pulls its handles.
  [[nodiscard]] bool penActive() const { return m_pen.active; }
  void setPenActive(bool on);
  bool penPress(int plane, const glm::dvec2 &p, double step, double grab);
  void penDrag(const glm::dvec2 &p);
  void penRelease();
  void penHover(int plane, const glm::dvec2 &p, double step);
  void penLeave();
  bool penFinish();      // the path becomes real
  void penCancel();
  bool penBackspace();
  [[nodiscard]] bool penDragging() const { return m_pen.dragging; }
  struct PathHandleHit {
    quint32 path = 0;
    int point = -1;
    int handle = 0; // 0 the point, -1 the in handle, +1 the out handle
  };
  bool pickPathHandle2D(int plane, const glm::dvec2 &p, double tol, PathHandleHit &hit) const;
  // The path under a 2D point (within tol of the curve) and where on it.
  bool pickPathCurve2D(int plane, const glm::dvec2 &p, double tol, quint32 &path, double &s) const;
  bool beginPathDrag(int plane, const glm::dvec2 &p, double step, const PathHandleHit &hit, bool additive);
  void updatePathDrag(const glm::dvec2 &p);
  void endPathDrag();
  [[nodiscard]] bool pathDragging() const { return m_pathDrag.active; }
  bool insertPathPointAt(quint32 path, double s);
  struct PathDraw {
    quint32 id = 0;
    bool selected = false, editing = false, error = false, connect = false;
    std::vector<glm::dvec3> polyline;
    struct Pt {
      glm::dvec3 pos, in, out;
      bool selected = false;
      int type = 0;
    };
    std::vector<Pt> points;
    std::vector<std::pair<double, double>> errorSpans; // polyline s ranges shown red
    std::vector<double> polylineS;
  };
  [[nodiscard]] std::vector<PathDraw> pathDraws();
  // The pen's rubber band (last point to the pointer) and its unfinished points.
  [[nodiscard]] bool penPreview(std::vector<glm::dvec3> &polyline, std::vector<glm::dvec3> &points) const;

  // Mirror mode (vk_mirror.cpp)
  // The mirror line is the plane p[axis] = d (Scene::mirrorLine). While on, what a command creates is also made
  // reflected and linked; linked brushes stay reflections through every later edit, mode on or off
  // (syncMirrors, run by the undo stack's commit hook).
  [[nodiscard]] bool mirrorActive() const { return m_mirrorOn; }
  [[nodiscard]] bool mirrorLineValid() const;
  [[nodiscard]] eng::mirror::Plane mirrorLine() const;
  // On without a line: the next 2D drag / 3D click places it.
  [[nodiscard]] bool mirrorPlacing() const { return m_mirrorOn && !mirrorLineValid(); }
  // On with a line: creating makes linked copies.
  [[nodiscard]] bool mirrorApplies() const { return m_mirrorOn && mirrorLineValid(); }
  void setMirrorActive(bool on);
  void setMirrorLine(const eng::mirror::Plane &p);
  // Removes the line (links keep their own planes); placing again while on.
  void resetMirrorLine();
  [[nodiscard]] bool mirrorTextures() const { return m_mirrorTextures; }
  void setMirrorTextures(bool on);
  // 2D: placing, a drag draws the line; else a press within `grab` (world units) moves it. True when taken.
  bool beginMirrorDrag(int plane, const glm::dvec2 &p, double step, double grab);
  void updateMirrorDrag(const glm::dvec2 &p);
  void endMirrorDrag();
  [[nodiscard]] bool mirrorDragging() const { return m_mirrorDrag.plane >= 0; }
  // Near the line in this 2D view (for the cursor).
  [[nodiscard]] bool mirrorLineNear(int plane, const glm::dvec2 &p, double grab) const;
  // 3D placing: the plane under the pointer, turned by turnMirror3D, placed by a click.
  void hoverMirror3D(float px, float py);
  bool clickMirror3D(float px, float py);
  void turnMirror3D();
  void leaveMirror3D();
  // Mode on: the line (or the 3D hover preview while placing); else the planes of the selection's links.
  struct MirrorShownPlane {
    eng::mirror::Plane plane;
    bool preview = false;
  };
  [[nodiscard]] std::vector<MirrorShownPlane> mirrorShownPlanes() const;
  // The brush across the link whose box is this box reflected (a cut leaves several brushes per side).
  [[nodiscard]] std::shared_ptr<eng::GameObject> mirrorTwinOf(const eng::GameObject &s) const;
  // The face of `twin` whose plane is face `f` of `s` reflected across `p`, or -1.
  [[nodiscard]] static int mirrorTwinFace(const eng::GameObject &s, int f, const eng::GameObject &twin,
                                          const eng::mirror::Plane &p);
  // Twins of the selected brushes (Object mode) / faces (Material mode: twin object + face index).
  [[nodiscard]] std::vector<std::shared_ptr<eng::GameObject>> mirrorTwinObjects() const;
  [[nodiscard]] std::vector<std::pair<std::shared_ptr<eng::GameObject>, int>> mirrorTwinFaces() const;
  // 2D outlines: twin boxes (Object mode), twin face polygons in world space (Material mode).
  [[nodiscard]] std::vector<std::pair<glm::dvec3, glm::dvec3>> mirrorTwinBoxes() const;
  [[nodiscard]] std::vector<std::vector<glm::dvec3>> mirrorTwinFacePolygons() const;
  [[nodiscard]] int selectedMirrorLinked() const;
  [[nodiscard]] QStringList selectedMirrorSetNames() const;
  // `add`: the twins join the selection instead of replacing it.
  Q_INVOKABLE void selectMirrorTwins(bool add = false);
  Q_INVOKABLE void detachMirror();
  // Mirror sets (Scene::mirrorSets).
  [[nodiscard]] std::vector<quint32> mirrorSetMembers(quint32 set) const;
  // The sets of the selected brushes (or of the selected faces' brushes).
  [[nodiscard]] std::set<quint32> selectedMirrorSets() const;
  // Hides / shows the mirrored copies (side 1) of the sets; undoable, saved.
  void setMirrorSetsHidden(const std::set<quint32> &sets, bool hide);
  void renameMirrorSet(quint32 set, const QString &name);
  // Unlinks every brush of the set (both sides stay as plain brushes).
  void dissolveMirrorSet(quint32 set);
  // Every brush (Material mode: face) of the selection's mirror sets; `add` keeps the current selection.
  bool selectSameMirror(bool add);
  // Puts GameObject::mirrorHidden in line with the sets' hideMirrored.
  void refreshMirrorVisibility();
  // Runs `fn` (a create command leaving what it made selected) plus its linked mirror copy, one undo step.
  template <class F> bool withMirror(const QString &label, F &&fn) {
    if (!mirrorApplies())
      return fn();
    const bool record = m_undoStack && !m_undoStack->isApplying();
    if (record)
      m_undoStack->beginMacro(label);
    const bool ok = fn();
    if (ok)
      mirrorCreated({m_selectedObjects.begin(), m_selectedObjects.end()}, label);
    if (record)
      m_undoStack->endMacro();
    return ok;
  }
  // Adds the linked copy of `created` (only `created` stays selected).
  void mirrorCreated(const std::vector<quint32> &created, const QString &label);
  // The commit hook: keeps the other side of every link touched by `cmd`.
  void syncMirrors(eng::UndoCommand &cmd);
  // Live twins during a drag: each twin's mesh is swapped for its source's reflection; end restores them
  // before the drag commits.
  void beginMirrorPreview(const std::vector<quint32> &dragIds);
  void updateMirrorPreview();
  void endMirrorPreview();
  // Linked copies across the line; an existing exact mirror image is linked (same materials) or replaced.
  // Returns a toast summary.
  QString mirrorSelectionAcrossLine();
  // Brushes of one mirror set selected with brushes in none: the latter join that set on its plane.
  // mirrorSetToAddTo() is that set, or 0. Returns a toast message.
  [[nodiscard]] quint32 mirrorSetToAddTo() const;
  QString addSelectionToMirrorSet();

private:
  bool m_mirrorOn = false;
  bool m_mirrorTextures = false;
  struct MirrorDrag {
    int plane = -1; // -1: no drag
    bool move = false; // moving the existing line (else drawing it)
    bool active = false;
    bool alongRight = false;
    double step = 1.0;
    glm::dvec2 start {0.0};
  } m_mirrorDrag;
  bool m_mirrorHoverValid = false;
  eng::mirror::Plane m_mirrorHover;
  glm::vec2 m_mirrorHoverPx {0.0f};
  bool m_mirrorTurned = false;
  bool m_mirrorSyncing = false;
  // Links made in the step being recorded: both sides are new, nothing to sync.
  std::set<quint32> m_mirrorFresh;
  struct MirrorPreviewPair {
    std::shared_ptr<eng::GameObject> source, twin;
    eng::mirror::Plane plane;
    bool mirrorTextures = false;
    std::shared_ptr<eng::Mesh> twinMesh;
    glm::vec3 pos {0.0f}, rot {0.0f}, scale {1.0f};
  };
  std::vector<MirrorPreviewPair> m_mirrorPreview;
  // The linked-brush overlay is rebuilt only when this changes.
  quint64 m_mirrorOverlayKey = ~0ull;
  // modifier runtime (vk_modifiers.cpp)
  struct ModRuntime {
    quint64 signature = 0;
    bool evaluated = false;
    // Output objects by provenance (source id, array copy, bend slice).
    std::map<std::tuple<quint32, int, int>, std::shared_ptr<eng::GameObject>> outputs;
    std::map<quint32, quint64> contentHash; // output id -> hash of what its mesh shows
    std::vector<std::shared_ptr<eng::GameObject>> spare; // removed outputs, reused before minting ids
    std::vector<eng::mods::Diagnostic> diagnostics;
    std::map<quint32, QString> summary;
    bool error = false, active = false;
    double lastMs = 0.0;
    qint64 lastAt = 0;
  };
  std::map<quint32, ModRuntime> m_modRuntime;
  std::set<quint32> m_selectedMods;
  std::set<quint32> m_selectedPaths;
  int m_selStepDepth = 0;
  SelectionState m_selStepBefore;
  std::set<quint32> m_drawSelection;
  quint32 m_editSourceHost = 0;
  bool m_showModOutput = true;
  // Material mode: the host of the output clicked last (Edit Source target).
  quint32 m_materialPickHost = 0;
  quint64 m_modKey = 0;
  bool m_modPumping = false;
  QTimer *m_modPoll = nullptr;
  struct CurveCacheEntry {
    quint64 key = 0;
    std::shared_ptr<const eng::path::Curve> curve;
    QString error;
    std::vector<eng::path::Point> points; // the points a Connect path was built into
  };
  std::map<quint32, CurveCacheEntry> m_curveCache;
  // path editing (vk_paths.cpp)
  std::set<int> m_selectedPathPoints;
  struct PenState {
    bool active = false;
    bool dragging = false;     // the press of the newest point is still held (pulling handles)
    int plane = -1;
    std::vector<eng::path::Point> points;
    glm::dvec3 hover {0.0};
    bool hoverValid = false;
    double depth = 0.0;        // along the view's depth axis, from the first point
    bool depthKnown = false;
  } m_pen;
  struct AnchorDragState {
    bool active = false;
    int plane = -1;
    quint32 anchor = 0;
    glm::dvec2 grab {0.0};
    double step = 1.0;
    glm::vec3 base {0.0f};
  } m_anchorDrag;
  // The anchors of the Connect paths the selected modified objects follow.
  [[nodiscard]] std::set<quint32> selectedHostAnchors() const;
  struct PathDragState {
    bool active = false;
    int plane = -1;
    quint32 path = 0;
    int point = -1;
    int handle = 0;
    glm::dvec2 start {0.0};
    double step = 1.0;
    std::vector<eng::path::Path> before;
    bool moved = false;
  } m_pathDrag;
  // The world point a 2D view position stands for, at depth `depth`.
  [[nodiscard]] glm::dvec3 planeToWorld(int plane, const glm::dvec2 &p, double depth) const;
  void pushModTable(const std::vector<eng::mods::Host> &before, const QString &label, const QString &mergeKey = {});
  void pushPathTable(const std::vector<eng::path::Path> &before, const QString &label, const QString &mergeKey = {});
  void refreshModVisibility();
  void refreshDrawSelection();
  void evaluateHost(const eng::mods::Host &host, const std::vector<std::shared_ptr<eng::GameObject>> &sources,
                    ModRuntime &rt);
  void clearHostOutputs(ModRuntime &rt, std::vector<quint32> &removeIds);
  quint64 hostSignature(const eng::mods::Host &host, const std::vector<std::shared_ptr<eng::GameObject>> &sources);
  void resetModRuntime();
  // The planes in the 3D view (collision overlay pipelines, own buffer).
  QRhiBuffer *m_mirrorVb = nullptr;
  QRhiResourceUpdateBatch *m_mirrorBatch = nullptr;
  quint32 m_mirrorFillCount = 0, m_mirrorEdgeCount = 0;
  void mirrorLineChanged();
  // Each unit of `sel` (a brush, or a whole generated object) gets a link across `plane` in `intoSet` (0: the
  // set new links join) and its twin.
  QString mirrorUnits(const std::set<quint32> &sel, const eng::mirror::Plane &plane, quint32 intoSet,
                      bool mirrorTextures, const QString &label);
  // Duplicates `ids` in place, reflects the copies across `link`'s plane and tags them (link, side).
  // Returns the copies' ids, left selected.
  std::vector<quint32> reflectInto(const std::vector<quint32> &ids, const eng::Scene::MirrorLink &link, int side,
                                   const QString &label);
  // The plane across which `side0` and `side1` are mirror images (bounds and brush by brush), `preferAxis`
  // first; none = they are not.
  [[nodiscard]] std::optional<eng::mirror::Plane> fitMirrorPlane(const std::vector<quint32> &side0,
                                                                 const std::vector<quint32> &side1,
                                                                 int preferAxis) const;
  // The links, sets and active set as one undoable table.
  struct MirrorTable {
    std::vector<eng::Scene::MirrorLink> links;
    std::vector<eng::Scene::MirrorSet> sets;
    quint32 activeSet = 0;
  };
  [[nodiscard]] MirrorTable mirrorTable() const;
  void restoreMirrorTable(const MirrorTable &t);
  void pushMirrorLinkTable(const MirrorTable &before, const QString &label);
  // The set a new link on the current line joins (minted on first use).
  quint32 mirrorSetForNewLink();
  // Unlinks both sides of every link in `links` (one undo step).
  void detachLinks(const std::set<quint32> &links, const QString &label);
  // Duplicated linked brushes get new links and sets, the plane moved with the copy, missing twins created.
  void relinkDuplicates(const DuplicateSet &set, const QString &label);
  // A copied linked brush: its link, side and set (which may be gone by paste time) and how far it moved.
  struct MirrorCopy {
    std::shared_ptr<eng::GameObject> clone;
    eng::Scene::MirrorLink link;
    int side = 0;
    eng::Scene::MirrorSet set;
    glm::dvec3 offset {0.0};
  };
  void relinkCopies(const std::vector<MirrorCopy> &copies, const QString &label);
  // Duplicated sources (outside Edit Source) make new hosts with the same stack. Returns the new hosts.
  std::vector<quint32> relinkHostDuplicates(const DuplicateSet &set, const QString &label);
  void buildMirrorOverlay();
  // Paths in the 3D view (collision line pipeline, own buffer).
  QRhiBuffer *m_pathVb = nullptr;
  QRhiResourceUpdateBatch *m_pathBatch = nullptr;
  quint32 m_pathEdgeCount = 0;
  void buildPathOverlay();
  void drawPathOverlay(QRhiCommandBuffer *cb);
  void drawMirrorOverlay(QRhiCommandBuffer *cb);

public:
  [[nodiscard]] glm::dvec3 selectionPivot() const;
  // Swap scene objects in one undo step; the added objects become the selection.
  void replaceObjects(const std::vector<quint32> &removeIds,
                      const std::vector<std::shared_ptr<eng::GameObject>> &add, const QString &label);
  [[nodiscard]] std::shared_ptr<eng::GameObject> objectFromBrush(const eng::ConvexBrush &brush,
                                                                 const eng::GameObject *metaFrom = nullptr) const;
  [[nodiscard]] std::vector<std::pair<quint32, eng::ConvexBrush>> selectedBrushes() const;
  [[nodiscard]] eng::MaterialPtr newBrushMaterial(double &tilesPerUnit) const;

  // editor operations (vk_editor_ops.cpp)
  [[nodiscard]] QString entityDisplayName(quint32 entityId) const;
  [[nodiscard]] bool objectHidden(quint32 id) const;
  [[nodiscard]] bool objectLocked(quint32 id) const;
  void setObjectsHidden(const std::vector<quint32> &ids, bool hidden);
  void setObjectsLocked(const std::vector<quint32> &ids, bool locked);
  Q_INVOKABLE void hideSelected();
  Q_INVOKABLE void isolateSelected();
  Q_INVOKABLE void showAll();
  Q_INVOKABLE void lockSelected();
  Q_INVOKABLE void unlockAll();
  [[nodiscard]] int hiddenCount() const;
  Q_INVOKABLE void selectAll();
  Q_INVOKABLE void invertSelection();
  Q_INVOKABLE void selectByMaterial();
  Q_INVOKABLE void selectFacesWithMaterial(const QString &name);
  // World AABB of everything selected (brushes, faces' brushes, the entity).
  bool selectionBounds(glm::vec3 &mn, glm::vec3 &mx) const;
  // World AABB of the brushes selected last (now, or before the selection was cleared); false before any.
  bool lastSelectionBox(glm::dvec3 &mn, glm::dvec3 &mx) const {
    mn = m_lastSelectionMin;
    mx = m_lastSelectionMax;
    return m_hasLastSelection;
  }
  Q_INVOKABLE void focusSelection();
  [[nodiscard]] float cameraYaw() const { return m_camera->getYaw(); }
  [[nodiscard]] float cameraPitch() const { return m_camera->getPitch(); }
  // Looks along a world axis (0 X, 1 Y, 2 Z; sign = direction), keeping the target in view.
  Q_INVOKABLE void lookAlongAxis(int axis, int sign);
  // Camera bookmarks (session): slot 0..3.
  void storeBookmark(int slot) {
    if (slot < 0 || slot > 3)
      return;
    m_bookmarks[slot] = {true, m_camera->getPosition(), m_camera->getYaw(), m_camera->getPitch()};
  }
  bool recallBookmark(int slot) {
    if (slot < 0 || slot > 3 || !m_bookmarks[slot].valid)
      return false;
    m_camera->setPose(m_bookmarks[slot].pos, m_bookmarks[slot].yaw, m_bookmarks[slot].pitch);
    requestRedraw();
    return true;
  }
  struct Bookmark {
    bool valid = false;
    glm::vec3 pos {0.0f};
    float yaw = 0.0f, pitch = 0.0f;
  };
  Bookmark m_bookmarks[4];
  // mesh groups (vk_groups.cpp)
  // Every brush is in one group (0 = worldspawn); new brushes go into the active group. Edits are undoable.
  [[nodiscard]] const std::vector<eng::MeshGroup> &meshGroups() const;
  [[nodiscard]] quint32 activeMeshGroup() const;
  void setActiveMeshGroup(quint32 id);
  // Returns the new id (0 on failure). moveSelection puts the selected brushes in it.
  quint32 createMeshGroup(const QString &name, bool moveSelection, bool makeActive = true);
  bool renameMeshGroup(quint32 id, const QString &name);
  void setMeshGroupColor(quint32 id, const QColor &color);
  void setMeshGroupHidden(quint32 id, bool hidden);
  void setMeshGroupLocked(quint32 id, bool locked);
  // Members move to worldspawn, or are deleted with the group.
  bool deleteMeshGroup(quint32 id, bool deleteBrushes);
  void moveSelectionToGroup(quint32 id);
  void selectMeshGroup(quint32 id, bool additive);
  // Hides every group except this one (or shows all when it is the only visible one).
  void soloMeshGroup(quint32 id);
  [[nodiscard]] QHash<quint32, int> meshGroupCounts() const;
  [[nodiscard]] QHash<quint32, int> meshGroupSelectedCounts() const;
  [[nodiscard]] QString uniqueGroupName(const QString &base) const;
  // Layout Designer documents saved with the map (LAYO chunk, JSON).
  QByteArray m_layoutBlob;
  // layouts (vk_layouts.cpp)
  [[nodiscard]] QList<QJsonObject> layoutDocs() const;
  [[nodiscard]] QJsonObject layoutDoc(int id) const;
  [[nodiscard]] int mintLayoutId() const;
  // (Re)builds the layout's brushes in its mesh group and stores the layout: one undo step. Returns the
  // brush count.
  int generateLayout(const QJsonObject &doc);
  // A one-room layout document whose inside is the selection box (walls, floor, ceiling around it); one undo
  // step. Returns the brush count.
  int createRoomFromMarquee();
  // The open project's Layout/* settings.
  [[nodiscard]] static eng::layout::Config layoutDefaults();
  // Removes a layout's brushes (and optionally the layout itself).
  void removeLayoutBrushes(int id);
  // The layout id a brush was generated from (0 = none).
  [[nodiscard]] int layoutOfObject(quint32 stableId) const;
  // Alt+LMB orbit, MMB pan, Alt+wheel dolly (QQQuickWidget drives these).
  void beginOrbit(float px, float py);
  void orbit(float dx, float dy);
  void pan(float dx, float dy);
  void dolly(float steps);
  void translateSelection(const glm::vec3 &delta, const QString &label);
  Q_INVOKABLE void nudgeSelection(int axis, int direction);
  void setSelectionBounds(const glm::vec3 &newMin, const glm::vec3 &newMax);
  Q_INVOKABLE void duplicateSelection();

  // Material mode: the picked faces; Object mode: every face of the selected brushes.
  [[nodiscard]] std::vector<std::pair<quint32, int>> selectedFaceRefs() const;
  struct FaceSummary {
    int count = 0;
    QString material;
    int texWidth = 0, texHeight = 0;
    double tileSourceU = 0, tileSourceV = 0, offsetU = 0, offsetV = 0, rotation = 0;
    // The offsets in texels of each face's own texture, wrapped to [0, size).
    double offsetTexU = 0, offsetTexV = 0;
    bool mixedMaterial = false, mixedScaleU = false, mixedScaleV = false;
    bool mixedOffsetU = false, mixedOffsetV = false, mixedRotation = false;
    bool mixedOffsetTexU = false, mixedOffsetTexV = false;
    // Some selected face differs from its default (resetFaceTextureFields).
    bool changedScaleU = false, changedScaleV = false, changedOffsetU = false, changedOffsetV = false,
         changedRotation = false;
  };
  [[nodiscard]] FaceSummary faceSummary() const;
  void rotateFaceTextures(double degrees);
  void flipFaceTextures(bool horizontal);
  void alignFaceTextures(int mode);
  void fitFaceTextures(int repeatU, int repeatV);
  void shiftFaceTextures(double du, double dv);
  void scaleFaceTextures(double fu, double fv);
  // `which`: 1 scaleU, 2 scaleV, 4 offU, 8 offV, 16 rotation; add kOffsetsInTexels
  // to give the offsets in texels of each face's own texture.
  static constexpr int kOffsetsInTexels = 32;
  void setFaceTextureBasis(double tileSourceU, double tileSourceV, double offU, double offV, double rotationDeg,
                           int which);
  // Resets the fields in `which` (same bits): tile = texture size x Brush/DefaultTextureScale, offset 0,
  // rotation 0 (all fields: the paraxial eng::defaultTexAxes).
  void resetFaceTextureFields(int which);
  [[nodiscard]] static float faceTextureRotationOf(const glm::vec3 &n, const eng::FaceTexBasis &b);
  // texture tools (vk_texture_tools.cpp)
  // editSelectedFaces with the face in world space (polygon, outward normal).
  void editSelectedFacesWorld(const QString &label,
                              const std::function<void(const std::vector<glm::vec3> &poly, const glm::vec3 &n,
                                                       eng::FaceTexBasis &)> &edit);
  // 0 left, 1 right, 2 top, 3 bottom, 4 centre: snap the texture to the face's edge.
  void justifyFaceTextures(int mode);
  // Rotates U onto the next edge of each face (repeat to cycle the edges).
  void alignFaceTexturesToEdge();
  void resetFaceTextures();
  // Shift by Source units along U / V.
  void shiftFaceTexturesSource(double du, double dv);
  // Copies from the first selected face. Paste mode: 0 = material, 1 = material + mapping, 2 = wrap (mapping
  // rotated about the shared edge, seamless).
  bool copyFaceAttributes();
  [[nodiscard]] bool hasFaceClipboard() const { return m_faceClip.valid; }
  void pasteFaceAttributes(int mode);
  // Pastes onto the face under the cursor.
  bool pasteFaceAttributesAt(float x, float y, int mode);
  [[nodiscard]] quint32 facePickAt(float x, float y) const;
  // The first selected face, for the UV editor.
  struct FaceView {
    bool valid = false;
    quint32 object = 0;
    int face = -1;
    std::vector<glm::vec3> poly; // world
    glm::vec3 normal {0, 1, 0};
    eng::FaceTexBasis basis;
    eng::MaterialPtr material;
    std::vector<std::vector<glm::vec3>> others; // other selected faces, world
  };
  [[nodiscard]] FaceView primaryFace() const;
  // Live edits from the UV editor: one undo step from begin to end.
  void beginFaceEdit();
  void updateFaceEdit(const std::function<void(const std::vector<glm::vec3> &poly, const glm::vec3 &n,
                                               eng::FaceTexBasis &base, eng::FaceTexBasis &out)> &edit);
  void endFaceEdit(const QString &label);
  struct FaceClip {
    bool valid = false;
    QString material;
    eng::FaceTexBasis basis;
    glm::vec3 normal {0, 1, 0};
    glm::vec3 point {0.0f};
  } m_faceClip;
  struct FaceEditSession {
    bool active = false;
    std::vector<ObjectState> before;
    std::vector<std::pair<quint32, int>> refs;
    std::vector<eng::FaceTexBasis> base;
  } m_faceEdit;
  int m_alignEdgeCycle = 0;
  Q_INVOKABLE void applyMaterialToSelection(const QString &materialName);
  [[nodiscard]] QString pickMaterialFromSelection() const;

  // material properties of the open map (vk_material_props.cpp)
  // Material refs on the selected faces (or selected objects), most used first.
  [[nodiscard]] QStringList selectionMaterialRefs() const;
  [[nodiscard]] int facesUsingMaterial(const QString &ref, bool selectionOnly) const;
  [[nodiscard]] std::vector<std::pair<quint32, int>> facesWithMaterial(const QString &ref, bool selectionOnly) const;
  // Edits `ref`'s layer: the variant for "pack/Name#v", else the map layer of "pack/Name". Drags merge.
  void setMaterialProperty(const QString &ref, matprops::Key key, const QVariant &value);
  void resetMaterialProperty(const QString &ref, matprops::Key key);
  void revertMaterialOverrides(const QString &ref);
  // A new variant of `ref`'s base for the selected faces that use it; returns its ref.
  QString createVariantForSelection(const QString &ref, const QString &name);
  void deleteVariant(const QString &variantRef);
  void mergeVariantIntoMap(const QString &variantRef);
  // Every face using `fromRef` (or only the selected ones) gets `toRef`.
  void replaceMaterial(const QString &fromRef, const QString &toRef, bool selectionOnly);
  void setProjectPack(const QString &pack);
  [[nodiscard]] QString projectPack() const;
  // Server layer := server + map layer of `ref`'s base, then the map layer is cleared. `force` skips the
  // revision check. done(ok, message|"conflict", current entry on conflict).
  void saveMaterialPropsToServer(const QString &ref, bool force,
                                 std::function<void(bool, const QString &, const QJsonObject &)> done);
  [[nodiscard]] QStringList materialsInUse() const;
  [[nodiscard]] QString currentMaterial() const { return m_currentMaterial; }
  void setCurrentMaterial(const QString &m) {
    if (m_currentMaterial == m)
      return;
    m_currentMaterial = m;
    emit currentMaterialChanged(m);
  }
  // The view redraws on demand: call after changing what is on screen from outside the render loop.
  void requestRedraw() { emit redrawRequested(); }
  [[nodiscard]] bool wantsContinuousFrames() const;
  // CPU time of the last render() call.
  [[nodiscard]] float lastFrameMs() const { return m_lastRenderMs; }
  float m_lastRenderMs = 0.0f;
  // Hash of everything outside the meshes that changes the 3D view; polled by the on-demand redraw.
  [[nodiscard]] quint64 viewSignature() const;
  quint64 m_viewEpoch = 0;
  bool m_cameraMovedThisFrame = false;
private:
  void bindPreferences();
  void updateClipPlanes();
  void editSelectedFaces(const QString &label,
                         const std::function<void(eng::Mesh &, int, eng::FaceTexBasis &)> &edit);
  glm::vec3 m_orbitPivot {0.0f};
  QString m_currentMaterial;
  int m_freelookDx = 0;
  int m_freelookDy = 0;
  std::unique_ptr<QElapsedTimer> m_frameTimer;
  QSize m_lastViewSize {1, 1};
  bool m_shadowDirty = true;

  int m_shadowMapSize = 4096;
  int m_builtShadowMapSize = 0;
  float m_shadowDepthScale = 1.0f / 600.0f;
  float m_shadowWorldTexel = 0.25f;
  float m_shadowNearPlane = 1.0f;
  float m_shadowFarPlane = 500.0f;
  QRhiTexture *m_shadowTexture = nullptr;
  QRhiRenderBuffer *m_shadowDepthBuffer = nullptr;
  QRhiRenderTarget *m_shadowTarget = nullptr;
  QRhiRenderPassDescriptor *m_shadowRenderPass = nullptr;

  // SSAO: full-res view-space position / normal g-buffer, half-res raw AO, and its blurred result.
  bool m_ssaoEnabled = false;  float m_ssaoRadius = 2.0f;
  float m_ssaoIntensity = 1.0f;
  int m_ssaoSamples = 16;
  bool m_fxaaEnabled = true;
  quint32 m_autoWorldEnvId = 0; // the world_env ensureWorldEnv() created
  QRhiTexture *m_iconAtlas = nullptr;
  QRhiShaderResourceBindings *m_billboardSrb = nullptr;
  std::unique_ptr<QRhiGraphicsPipeline> m_billboardPipeline;
  float m_entityIconPx = 60.0f; // physical pixels (preference × device pixel ratio)
  QString m_skyRef;          // the map's skybox (empty = procedural sky)
  QString m_skyTexRef;       // the ref m_skyTex shows
  QImage m_skyImage;         // decoded RGBA16F panorama (kept for a re-upload after a device loss)
  QString m_skyImageRef;
  quint64 m_skyLoadGen = 0;  // drops decodes for a ref no longer wanted
  QString m_lutRef;          // the level's colour LUT (empty = none)
  QImage m_lutStrip;         // its strip (RGB888, N*N x N) once loaded
  QString m_lutStripRef;     // the ref m_lutStrip belongs to
  quint64 m_lutLoadGen = 0;
  quint64 m_lutEpoch = 1;    // bumps with every strip change (texture key)
  bool m_lutEnabled = true;
  QHash<quint64, quint64> m_smoothUnits;  // eng::refreshSmoothUnits state
  glm::dvec3 m_lastSelectionMin {0.0}, m_lastSelectionMax {0.0};
  bool m_hasLastSelection = false;
  bool m_lutPreviewOn = false;
  QImage m_lutPreview;
  void loadColorLut();
  void setLutStrip(const QString &ref, const QImage &strip);
  void watchLutCatalog();
  QRhiTexture *m_skyTex = nullptr;
  QRhiTexture *m_lutTex = nullptr;
  quint64 m_lutTexKey = 0;
  int m_lutTexSize = 0;
  int m_lutActiveSize = 0;
  QRhiTexture *m_fxaaTex = nullptr;
  QRhiRenderBuffer *m_fxaaMsaa = nullptr;
  QRhiRenderBuffer *m_fxaaDepth = nullptr;
  QRhiTextureRenderTarget *m_fxaaTarget = nullptr;
  QRhiRenderPassDescriptor *m_fxaaRp = nullptr;
  QRhiShaderResourceBindings *m_fxaaSrb = nullptr;
  std::unique_ptr<QRhiGraphicsPipeline> m_fxaaPipeline;
  QRhiRenderPassDescriptor *m_fxaaPipelineRp = nullptr;
  int m_fxaaPipelineSamples = 0;
  QSize m_fxaaSize;
  int m_fxaaSamples = 0;
  bool m_ssaoHalfRes = true;
  QSize m_ssaoSize { 0, 0 };
  QSize m_aoSize { 0, 0 };
  QSize m_ssaoFailedSize {};
  int m_ssaoRetry = 0;
  QRhiTexture *m_gposTex = nullptr;
  QRhiTexture *m_gnormTex = nullptr;
  QRhiRenderBuffer *m_gbufferDepth = nullptr;
  QRhiTextureRenderTarget *m_gbufferTarget = nullptr;
  QRhiRenderPassDescriptor *m_gbufferRp = nullptr;
  QRhiShaderResourceBindings *m_gbufferSrb = nullptr;
  QRhiTexture *m_aoTexA = nullptr;
  QRhiTexture *m_aoTexB = nullptr;
  QRhiTextureRenderTarget *m_aoTargetA = nullptr;
  QRhiTextureRenderTarget *m_aoTargetB = nullptr;
  QRhiRenderPassDescriptor *m_aoRp = nullptr;
  QRhiShaderResourceBindings *m_ssaoSrb = nullptr;
  QRhiShaderResourceBindings *m_aoBlurSrb = nullptr;
  QRhiShaderResourceBindings *m_aoApplySrb = nullptr;
  QRhiBuffer *m_ssaoUbo = nullptr;
  QRhiBuffer *m_quadVb = nullptr;
  QRhiBuffer *m_quadIb = nullptr;
  std::unique_ptr<QRhiGraphicsPipeline> m_gbufferPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> m_ssaoPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> m_aoBlurPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> m_aoApplyPipeline;
  SsaoUniforms m_ssaoUniforms {};

  // Selection overlay depth: linear distance / far plane, nearest wins, 0 = no geometry.
  QRhiTexture *m_selDepthTex = nullptr;
  QRhiRenderBuffer *m_selDepthDs = nullptr;
  QRhiTextureRenderTarget *m_selDepthTarget = nullptr;
  QRhiRenderPassDescriptor *m_selDepthRp = nullptr;
  QRhiShaderResourceBindings *m_selDepthSrb = nullptr;
  std::unique_ptr<QRhiGraphicsPipeline> m_selDepthPipeline;
  QSize m_overlaySize { 0, 0 };
  QSize m_overlayFailedSize {};
  int m_overlayRetry = 0;
  std::vector<quint32> m_selList; // selected objects resolved to slots, per frame

  // Entity markers: one vertex pool of every marker shape, plus EntityUniforms per drawn part (dynamic offset).
  struct EntityGeom {
    quint32 offset = 0; // first vertex in m_entityVb
    quint32 count = 0;
  };
  // The pipeline a marker part is drawn with: the box is occluded like level geometry, the gizmo never is
  // (Godot's depth_test_disabled handles), so two triangle pipelines.
  enum class EntityPass : quint8 {
    TriDepth,  // marker box: depth tested and writes depth
    TriOverlay, // gizmo: always visible, never writes depth
    Line,      // range sphere: always visible
    Billboard, // entity icon: depth tested, no depth write (vk_billboards.cpp)
  };
  struct EntityDraw {
    quint32 vbOffset = 0;
    quint32 vbCount = 0;
    EntityPass pass = EntityPass::TriOverlay;
    glm::mat4 model { 1.0f };
    glm::vec4 color { 1.0f };
    glm::vec4 info { 0.0f };
  };

  QRhiBuffer *m_entityVb = nullptr;
  QRhiBuffer *m_entityUbo = nullptr;
  QRhiShaderResourceBindings *m_entitySrb = nullptr;
  std::unique_ptr<QRhiGraphicsPipeline> m_entityTriDepthPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> m_entityTriOverlayPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> m_entityLinePipeline;
  quint32 m_entityStride = 0;
  quint32 m_entityCapacity = 0;
  bool m_entityGeomBuilt = false;
  std::vector<uint8_t> m_entityScratch;
  EntityGeom m_geoMarkerBox {};   // triangles, 1 unit half-extent, scaled per marker
  EntityGeom m_geoRangeSphere {}; // lines, unit radius, scaled by a light's range
  EntityGeom m_geoSpotCone {};    // lines: apex at the origin, unit rim circle at z = 1 (scaled per spot light)
  EntityGeom m_geoFrustum {};     // lines, apex at 0, far rectangle (±1, ±1, -1), "up" triangle
  // Camera preview state (vk_entities.cpp).
  quint32 m_previewCamera = 0;
  glm::vec3 m_previewSavedPos {0.0f};
  float m_previewSavedYaw = 0.0f, m_previewSavedPitch = 0.0f, m_previewSavedFov = 70.0f;
  [[nodiscard]] quint32 selectedCameraEntity() const;
  void applyCameraPreview();
  void syncCameraPreview();
  void addCameraFrustumDraws(const std::function<void(const EntityGeom &, const glm::mat4 &, const glm::vec4 &)> &add);
  EntityGeom m_geoArrow[3] {};   // triangles, along world axis i
  EntityGeom m_geoPlane[3] {};   // triangles, in the plane normal to axis i
  std::vector<EntityDraw> m_entityDraws;

  // Gizmo drag state, captured at grab time (the drag plane stays fixed if the camera turns mid-drag).
  GizmoHandle m_gizmoDrag {GizmoHandle::None};
  GizmoMode m_gizmoMode {GizmoMode::Translate};
  struct Rotate2D {
    bool active = false;
    int axis = 1;      // world axis turned about
    double sign = 1.0; // +1 when that axis points out of the view (CCW on screen = +angle)
    glm::dvec2 centre {0.0};
    glm::dvec2 startVec {1.0, 0.0};
    double screenAngle = 0.0;
  } m_rot2D;
  QString m_gizmoReadout;
  EntityGeom m_geoRing[3] {};
  // Rotate / scale drag state (vk_gizmo_modes.cpp).
  std::vector<glm::vec3> m_gizmoBaseRot;
  std::vector<glm::vec3> m_gizmoBaseScale;
  glm::vec3 m_gizmoBoundsMin {0.0f}, m_gizmoBoundsMax {0.0f};
  glm::vec3 m_gizmoClickVec {0.0f};
  double m_gizmoAngle = 0.0;
  glm::vec3 m_gizmoScaleFactor {1.0f};
  GizmoHandle gizmoHitTestModes(float x, float y) const;
  bool beginGizmoModeDrag(GizmoHandle handle, float x, float y);
  void captureGizmoModeDrag(GizmoHandle handle);
  void updateGizmoModeDrag(float x, float y);
  void previewGizmoRotation(int axis, double degrees);
  [[nodiscard]] double snapRotationAngle(double degrees) const;
  [[nodiscard]] static glm::dmat4 rotationAbout(int axis, double degrees, const glm::dvec3 &centre);
  void endGizmoModeDrag();
  void addGizmoModeDraws(const std::function<void(const EntityGeom &, int, const glm::mat4 &, const glm::vec4 &)> &add);
  GizmoHandle m_gizmoHover {GizmoHandle::None};
  glm::vec3 m_gizmoDragCenter { 0.0f };
  glm::vec3 m_gizmoBaseMin { 0.0f };
  glm::vec3 m_gizmoClickOrigin { 0.0f };
  glm::vec3 m_gizmoClickDir { 0.0f };
  std::vector<quint32> m_gizmoMeshIds;
  std::vector<glm::vec3> m_gizmoMeshBasePos;
  std::vector<DragFaceUv> m_gizmoBaseUv;
  // Selected point entities at the start of a rotate (brush entities turn as brushes).
  struct EntityPose {
    quint32 id = 0;
    glm::vec3 origin {0.0f};
    glm::vec3 angles {0.0f};
  };
  std::vector<EntityPose> m_gizmoEntities;
  struct EntityRotate2D {
    bool active = false;
    quint32 id = 0;           // the entity whose knob is dragged
    glm::dvec3 axis {0.0};    // out of the view (CCW on screen = +angle)
    glm::dvec2 centre {0.0};  // its centre in the view plane
    double startAngle = 0.0;  // its arrow's direction on screen (degrees)
    double angle = 0.0;       // the turn so far
    std::vector<EntityPose> poses;
  } m_entRot2D;
  [[nodiscard]] std::vector<EntityPose> selectedPointEntityPoses() const;
  // Origins orbit `pivot`; angles turn in the entity's own convention (eng::entityRotation). `record` =
  // undoable (false for the drag preview).
  void rotateEntityPoses(const std::vector<EntityPose> &base, const glm::mat3 &r, const glm::vec3 &pivot,
                         bool record);

  QRhiBuffer *m_sceneUbo = nullptr;
  QRhiBuffer *m_objectUbo = nullptr;
  QRhiBuffer *m_drawUbo = nullptr;
  QRhiBuffer *m_overlayUbo = nullptr;
  quint32 m_objectStride = 0;
  quint32 m_drawStride = 0;
  quint32 m_objectCapacity = 0;
  quint32 m_drawCapacity = 0;

  QRhiShaderResourceBindings *m_sceneTemplateSrb = nullptr;
  QRhiShaderResourceBindings *m_shadowSrb = nullptr;
  QRhiShaderResourceBindings *m_wireSrb = nullptr;
  quint64 m_bufferGen = 0;
  std::unique_ptr<QRhiGraphicsPipeline> m_scenePipeline;
  std::unique_ptr<QRhiGraphicsPipeline> m_shadowPipeline;
  std::unique_ptr<QRhiGraphicsPipeline> m_wirePipeline;

  QRhi *m_rhi = nullptr;
  bool m_preview = false;
  QRhiRenderPassDescriptor *m_builtRenderPass = nullptr;
  QSize m_builtOverlaySize { 0, 0 };
  QSize m_builtSsaoSize { 0, 0 };
  int m_builtSampleCount = 0;
  quint64 m_builtBufferGen = ~0ull;

  SceneUniforms m_sceneUniforms {};
  OverlayUniforms m_overlayUniforms {};

  glm::mat4 m_lightView { 1.0f };
  glm::mat4 m_lightClip { 1.0f };

  // A permanent object UBO slot per stable id (removal moves nothing else). Version 0 = never uploaded
  // (transformVersion starts at 1).
  RangeAllocator m_objectSlotAlloc;
  QHash<quint32, quint32> m_objectSlots;
  QHash<quint32, quint64> m_objectUploadedVersion;
  std::vector<uint8_t> m_drawScratch;
  quint64 m_uploadedStructureVersion = ~0ull;

  // ~0 when the object is unknown.
  [[nodiscard]] quint32 objectSlotOf(const quint32 stableId) const {
    const auto it = m_objectSlots.constFind(stableId);
    return it == m_objectSlots.constEnd() ? ~0u : it.value();
  }
  void syncObjectSlots();
  void clearObjectSlotState();

  quint64 m_sigGeometry = ~0ull; // objects + transforms + mesh geometry

  // Signature of everything an object's DrawBlocks read, by stable id (survives structural changes).
  QHash<quint32, quint64> m_faceSigById;
  quint64 m_faceStructVersion = ~0ull;
  quint64 m_uploadedTransformEpoch = ~0ull;
  quint64 m_shadowMapKey = ~0ull;
  quint64 m_visibleKey = ~0ull;

  QVector<VisibleFace> m_visible;

  // Visible faces bucketed by material (fewer state changes). A counting sort: std::stable_sort cost ~3.7 ms
  // per drag frame.
  struct MatBucket {
    eng::Material *mat = nullptr;
    quint32 count = 0;
    quint32 offset = 0;
  };
  void sortByMaterial();
  std::vector<MatBucket> m_visBuckets;
  std::vector<quint32> m_visBucketOf;
  std::vector<quint32> m_visCursor;
  QVector<VisibleFace> m_visibleTmp;

  // VK_PERF=1 prints one line per second: fps, CPU ms per render() stage, and counters of costs that should
  // be one-time.
  struct PerfCounters {
    qint64 frames = 0;
    qint64 pipelineRebuilds = 0; // setupPipelines() falling through its guard
    qint64 srbRebuilds = 0;      // materialSrb() creating a new SRB
    qint64 hoverCalls = 0;       // hoverGizmo() invocations
    qint64 entityDraws = 0;      // draws issued by drawEntities()
    double msTotal = 0.0;
    double msPre = 0.0;    // buffers, pipelines, uploads, visibility, labels
    double msGpuSync = 0.0;
    double msEnsure = 0.0;
    double msUpload = 0.0;
    double msLight = 0.0;
    double msEntityBuild = 0.0;
    double msVis = 0.0;
    double msSort = 0.0;
    double msLabels = 0.0;
    qint64 sceneDraws = 0;
    qint64 shadowDraws = 0;
    qint64 visibleFaces = 0;
    qint64 distanceCulled = 0;
    qint64 shadowSkips = 0;
    qint64 uploadSkips = 0;
    // Frame period vs time inside render(): a period far above the buckets' sum means the stall is outside
    // the renderer (event loop, QML sync, another viewport).
    double msGap = 0.0;
    qint64 dragCalls = 0;
    double msDrag = 0.0;
    double msOverlay = 0.0; // scene-depth prepass for the selection overlay
    qint64 selObjects = 0;
    qint64 sceneDepthDraws = 0;
    qint64 wireOverlayDraws = 0;
    qint64 labelDecals = 0;
    qint64 syncWireCalls = 0;
    qint64 lastSelCount = -1;
    double msShadow = 0.0;
    double msLightShadow = 0.0; // entity light shadow atlas
    double msSsao = 0.0;
    double msMain = 0.0;   // main pass: scene, AO apply, wire, entities, text, marquee
  };
  PerfCounters m_perf;
  QElapsedTimer m_perfClock;
  qint64 m_perfWindowMs = 0;
  QElapsedTimer m_gapTimer;

  // open levels (vk_documents.cpp)
  // The level's camera, 2D views and mode while parked (the live values are the camera and the GridViews).
  fmap::PerspectiveCameraState m_levelCamera;
  bool m_levelCameraValid = false;
  std::vector<fmap::OrthoViewState> m_levelViews;
  int m_levelMode = 0; // g::EditorMode
  quint64 m_levelSerial = 0;
  quint64 m_nextLevelSerial = 1;
  bool m_capturingParked = false; // withDocument: the camera / views are the level's own
  bool m_zenOn = false;
  glm::vec3 m_zenSizeSource {768.0f};
  glm::vec3 m_zenCenter {0.0f};
  quint64 m_zenEpoch = 1;
  int m_switchFramesToLog = 0;    // logs the cost of the first frames after a switch

public:
  // Every member that belongs to a level (scene, history, selection, scene settings, derived objects, camera,
  // views); switchDocument() swaps the whole list with a parked DocumentState. A level member missing from
  // this list leaks from one level into the next: add it. Renderer state, preferences, view toggles, the
  // clipboard and tools stay with the engine.
#define VK_ENGINE_LEVEL_STATE(X)                                                                             \
  X(m_scene) X(m_undoStack)                                                                                  \
  X(m_selectedFaces) X(m_materialMode) X(m_hasStashedSelection) X(m_stashedObjects) X(m_stashedFaces)        \
  X(m_selectedObjects) X(m_marquee) X(m_selectedEntities) X(m_selectedVerts) X(m_editMode)                    \
  X(m_autoSel) X(m_autoSelOther) X(m_faceGraph) X(m_faceGraphKey) X(m_faceExposure) X(m_storeysShown)         \
  X(m_sunDirection) X(m_sunColor) X(m_sunIntensity) X(m_ambientColor) X(m_ambientIntensity)                   \
  X(m_fogDensity) X(m_fogColor) X(m_exposure) X(m_gridLevelOffset)                                           \
  X(m_shadowsEnabled) X(m_shadowBias) X(m_shadowNormalBias) X(m_shadowRadius) X(m_shadowStrength)            \
  X(m_shadowFar) X(m_sceneLight) X(m_cameraSpeed)                                                            \
  X(m_ssaoEnabled) X(m_ssaoRadius) X(m_ssaoIntensity) X(m_skyRef) X(m_lutRef) X(m_autoWorldEnvId)                        \
  X(m_propObjects) X(m_ioGraphLayout) X(m_mirrorOn) X(m_mirrorTextures)                                      \
  X(m_modRuntime) X(m_selectedMods) X(m_selectedPaths) X(m_selectedPathPoints) X(m_drawSelection)            \
  X(m_editSourceHost) X(m_materialPickHost) X(m_modKey) X(m_curveCache)                                      \
  X(m_bookmarks) X(m_layoutBlob) X(m_orbitPivot) X(m_maskHidden)                                             \
  X(m_levelCamera) X(m_levelCameraValid) X(m_levelViews) X(m_levelMode) X(m_levelSerial)

  // One parked level.
  struct DocumentState {
#define VK_ENGINE_LEVEL_DECL(m) std::remove_cvref_t<decltype(Engine3D::m)> m {};
    VK_ENGINE_LEVEL_STATE(VK_ENGINE_LEVEL_DECL)
#undef VK_ENGINE_LEVEL_DECL
    [[nodiscard]] bool dirty() const { return m_undoStack && !m_undoStack->isClean(); }
    [[nodiscard]] quint64 serial() const { return m_levelSerial; }
    [[nodiscard]] const QSharedPointer<eng::Scene> &scene() const { return m_scene; }
  };

  // An empty scene, a fresh history, every scene setting at the engine's constructed default.
  [[nodiscard]] std::unique_ptr<DocumentState> newDocumentState();
  // False while a gesture or an undo macro is open.
  [[nodiscard]] bool canSwitchDocument() const;
  // Swaps the active level with the one parked in `state`: tools end, camera / views / mode go with the
  // level, documentChanged plus the usual signals are emitted.
  void switchDocument(DocumentState &state);
  // Runs `fn` with a parked level swapped in, no signals or GPU work (save / export a parked level).
  void withDocument(DocumentState &state, const std::function<void()> &fn);
  // The active level's id (DocumentState::serial()).
  [[nodiscard]] quint64 levelSerial() const { return m_levelSerial; }
  // The editor mode the active level was left in (g::EditorMode).
  [[nodiscard]] int levelMode() const { return m_levelMode; }

signals:
  void documentChanged();

private:
  void swapLevelState(DocumentState &state);
  void captureLevelDefaults();
  void captureLevelView();
  void restoreLevelView();
  std::unique_ptr<DocumentState> m_levelDefaults;
};

} // namespace vk
