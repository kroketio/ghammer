#pragma once

#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <QJsonObject>
#include <QString>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "brush.h"
#include "path_curve.h"

namespace eng::mods {

// Non-destructive modifiers (Blender's stack): a host owns source brushes (GameObject::modHost) and a stack whose
// evaluation gives the output brushes. Pure: Engine3D feeds snapshots (vk_modifiers.cpp) and makes derived objects.

struct Modifier {
  quint32 id = 0;
  QString kind;  // "array", "curve" (unknown kinds are kept and skipped)
  QString name;
  bool enabled = true;
  bool showInEditSource = true; // evaluated while the host's sources are edited
  bool expanded = true;         // panel state
  QJsonObject params;           // per kind (ArrayParams / CurveParams), "v" = version
};

struct Host {
  quint32 id = 0;
  QString name;
  // Local axes: array offsets and the curve's deform axis are read in them (Blender's object space).
  glm::dquat orientation {1.0, 0.0, 0.0, 0.0};
  std::vector<Modifier> stack;
  quint32 nextModifierId = 1;
};

QJsonObject toJson(const Host &h);
Host hostFromJson(const QJsonObject &o);

enum class Category { Geometry, Texture };
struct KindInfo {
  QString id, label, icon;
  Category category = Category::Geometry;
  QJsonObject (*defaults)() = nullptr;
};
const std::vector<KindInfo> &kinds();
const KindInfo *kindInfo(const QString &id);

struct ArrayParams {
  enum class Fit { Count, Length, Curve } fit = Fit::Count;
  int count = 2;
  double length = 50.0;      // world units (Fit length)
  quint32 curvePath = 0;     // Fit curve: 0 = the path of the next Curve modifier
  bool useRelative = true;
  glm::dvec3 relative {1.0, 0.0, 0.0}; // × the input's size
  bool useConstant = false;
  glm::dvec3 constant {0.0};           // world units
  bool textureFollow = true;           // textures move with the copies
  static ArrayParams fromJson(const QJsonObject &o);
  [[nodiscard]] QJsonObject toJson() const;
};

// Curve: Blender's Curve modifier, plus Rigid pieces.
struct CurveParams {
  quint32 path = 0;
  int axis = 0;              // deform axis in the host frame: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
  enum class Mode { Bend, Rigid } mode = Mode::Bend;
  enum class Unit { Brush, Copy } rigidUnit = Unit::Brush;
  enum class Frame { KeepVertical, Follow } frame = Frame::KeepVertical;
  enum class Anchor { Bottom, Center, Top } anchor = Anchor::Bottom;
  double offsetAlong = 0.0, offsetSide = 0.0, offsetHeight = 0.0; // world units
  double segAngle = 3.0;     // Bend: a cut every this many degrees of turning
  double segMaxLen = 0.0;    // Bend: and at least every this length (0 = off)
  double segMinLen = 0.4;    // Bend: never closer than this
  bool stretch = false;      // scale along the axis so the input spans the path exactly
  bool reverse = false;
  // Path: a straight world-aligned texture from the start, unrolled (walls along each wall, following a ramp;
  // floors / ceilings fan with the bend). Bend: carried by the bending brushes. World: projected. Path and Bend are
  // exact at every corner (faces that cannot hold one mapping are split).
  enum class Texture { Path, Bend, World } texture = Texture::Path;
  double maxSlopeDeg = 80.0; // Keep vertical: steeper than this is an error
  double minJacobian = 0.15; // Bend: tighter than this squeeze (inner side) is an error
  static CurveParams fromJson(const QJsonObject &o);
  [[nodiscard]] QJsonObject toJson() const;
};

struct Piece {
  ConvexBrush brush;
  quint32 sourceId = 0;
  int copy = 0;           // array copy index
  int slice = 0;          // bend slice index
  bool identity = true;   // still exactly the source (array copy 0, nothing after)
};

struct Diagnostic {
  enum class Severity { Info, Warning, Error } severity = Severity::Info;
  quint32 modifierId = 0;
  QString text;
  quint32 pathId = 0;
  double s0 = -1.0, s1 = -1.0; // the span of the path concerned (world units), < 0 none
};

struct EvalInput {
  glm::dquat orientation {1.0, 0.0, 0.0, 0.0};
  std::vector<Piece> sources;
  std::vector<Modifier> stack;
  bool editSource = false; // skip modifiers hidden in Edit Source, drop identity pieces
  std::function<std::shared_ptr<const path::Curve>(quint32)> curveFor;
  int maxPieces = 20000;
};

struct EvalOutput {
  std::vector<Piece> pieces;
  std::vector<Diagnostic> diagnostics;
  std::map<quint32, QString> summary; // per modifier: "12 copies · stretched +2.3%"
  bool error = false;                 // an Error: no output at all
  bool active = false;                // at least one modifier ran
};

EvalOutput evaluate(const EvalInput &in);

// Shared by the kinds (mod_array.cpp, mod_curve.cpp).
struct Context {
  const EvalInput &in;
  EvalOutput &out;
  const Modifier &mod;
  int index = 0; // position in the stack
  void error(const QString &text, quint32 pathId = 0, double s0 = -1.0, double s1 = -1.0);
  void warning(const QString &text, quint32 pathId = 0, double s0 = -1.0, double s1 = -1.0);
};
void applyArray(Context &ctx, std::vector<Piece> &pieces);
void applyCurve(Context &ctx, std::vector<Piece> &pieces);

// Bounds of pieces in the frame (axes f, u, r): min / max of p·axis.
void localBounds(const std::vector<Piece> &pieces, const glm::dvec3 axes[3], glm::dvec3 &mn, glm::dvec3 &mx);
// The deform axes of a curve modifier in a host frame: f (along), u (up), r = f × u.
void curveAxes(const glm::dquat &q, int axis, glm::dvec3 &f, glm::dvec3 &u, glm::dvec3 &r);

} // namespace eng::mods
