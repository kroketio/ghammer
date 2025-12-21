// Array modifier: copies of the input along an offset (Blender's Array).

#include "modifiers.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace eng::mods {

namespace {

// The Curve an Array fits to: an explicit path, else the next enabled Curve after it. False when none.
bool fitCurveTarget(const Context &ctx, const ArrayParams &p, CurveParams &curve) {
  const auto &stack = ctx.in.stack;
  for (size_t j = size_t(ctx.index) + 1; j < stack.size(); ++j) {
    const Modifier &m = stack[j];
    if (!m.enabled || m.kind != QLatin1String("curve") || (ctx.in.editSource && !m.showInEditSource))
      continue;
    curve = CurveParams::fromJson(m.params);
    if (p.curvePath)
      curve.path = p.curvePath;
    return true;
  }
  if (p.curvePath) {
    curve = CurveParams();
    curve.path = p.curvePath;
    return true;
  }
  return false;
}

} // namespace

void applyArray(Context &ctx, std::vector<Piece> &pieces) {
  if (pieces.empty())
    return;
  const ArrayParams p = ArrayParams::fromJson(ctx.mod.params);
  const glm::dquat q = ctx.in.orientation;
  const glm::dvec3 axes[3] = {q * glm::dvec3(1, 0, 0), q * glm::dvec3(0, 1, 0), q * glm::dvec3(0, 0, 1)};
  glm::dvec3 mn, mx;
  localBounds(pieces, axes, mn, mx);
  const glm::dvec3 size = mx - mn;
  glm::dvec3 local(0.0);
  if (p.useRelative)
    local += p.relative * size;
  if (p.useConstant)
    local += p.constant;
  const glm::dvec3 offset = axes[0] * local.x + axes[1] * local.y + axes[2] * local.z;
  const double step = glm::length(offset);

  int count = p.count;
  if (p.fit != ArrayParams::Fit::Count) {
    double length = p.length;
    bool stretch = false;
    if (p.fit == ArrayParams::Fit::Curve) {
      CurveParams curve;
      if (!fitCurveTarget(ctx, p, curve)) {
        ctx.error(QStringLiteral("Fit curve: no Curve modifier after this one (or pick a path)"));
        return;
      }
      const auto c = ctx.in.curveFor ? ctx.in.curveFor(curve.path) : nullptr;
      if (!c || !c->valid()) {
        ctx.error(QStringLiteral("Fit curve: the path is missing or empty"), curve.path);
        return;
      }
      length = c->length() - std::max(0.0, curve.offsetAlong);
      stretch = curve.stretch;
    }
    if (step < 1e-6) {
      ctx.warning(QStringLiteral("The offset is zero: one copy"));
      count = 1;
    } else {
      // Extent of one copy along the offset: copies fit end to end.
      glm::dvec3 dir = offset / step;
      double lo = 1e300, hi = -1e300;
      for (const Piece &pc: pieces)
        for (const auto &poly: pc.brush.polygons)
          for (const auto &v: poly) {
            lo = std::min(lo, glm::dot(v, dir));
            hi = std::max(hi, glm::dot(v, dir));
          }
      const double extent = std::max(0.0, hi - lo);
      const double room = (length - extent) / step;
      count = int(stretch ? std::lround(room) : std::floor(room + 1e-9)) + 1;
      if (length < extent * 0.5)
        ctx.warning(QStringLiteral("The length is shorter than one copy"));
    }
  }
  count = std::clamp(count, 1, 1000);

  std::vector<Piece> out;
  out.reserve(pieces.size() * size_t(count));
  for (int i = 0; i < count; ++i)
    for (const Piece &src: pieces) {
      Piece c = src;
      if (i > 0) {
        c.brush.transform(glm::translate(glm::dmat4(1.0), offset * double(i)), p.textureFollow);
        c.identity = false;
      }
      c.copy = src.copy * 1000 + i;
      out.push_back(std::move(c));
    }
  pieces = std::move(out);
  ctx.out.summary[ctx.mod.id] = count == 1 ? QStringLiteral("1 copy") : QStringLiteral("%1 copies").arg(count);
}

} // namespace eng::mods
