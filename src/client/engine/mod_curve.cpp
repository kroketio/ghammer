// Curve modifier (Blender's): Bend (vertices follow the curve, sliced as it turns) or Rigid pieces (each brush
// keeps its shape, only placed and turned).
// Input is read in the host frame: deform axis f, up u, side r = f × u, anchored at the input's bounds:
//   s = (p·f − min f)·stretch + offsetAlong
//   x = p·r − mid r + offsetSide
//   y = p·u − anchor u + offsetHeight
// Keep vertical: world = C(s) + x·R(s) + y·Y, R the horizontal side direction (walls vertical, height a shear).
// Follow: world = C(s) + x·R + y·U, U perpendicular to the tangent (pitches with the slope, no bank).
// Textures (Follow the path): each corner gets what a straight world-aligned continuation from the path start shows at
// the unrolled point q = F0 + along0·a + side0·x + up0·b (F0 the start frame). Walls (facing sideways): a = true 3D
// length along their own base curve, b = y, so lines are 128 apart along a ramp; a shear, since a texture turned with
// the climb folds on tight climbing bends. Everything else: a = s - s0, b = y (floors fan). Each face's basis is then
// solved from its corners (brush_deform), split where one flat mapping cannot hit them all.

#include "modifiers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>

#include "brush_deform.h"

namespace eng::mods {

namespace {

const glm::dvec3 kUp {0.0, 1.0, 0.0};

QString units(const double w) { return QString::number(std::lround(w * 10.0)); } // world -> Source units

struct Frame {
  glm::dvec3 pos {0.0}, along {0.0, 0.0, 1.0}, up {kUp}, side {1.0, 0.0, 0.0};
  bool ok = true;
};

// Corner tolerance for one flat mapping (world units: 0.1 Source units); beyond it the face is split.
constexpr double kTexTol = 0.01;

glm::dvec2 uvOf(const FaceTexBasis &b, const glm::dvec3 &p) {
  return {glm::dot(p, glm::dvec3(b.uAxis)) * double(b.tilesPerUnitU) + double(b.offsetU),
          glm::dot(p, glm::dvec3(b.vAxis)) * double(b.tilesPerUnitV) + double(b.offsetV)};
}

// A wall's base curve unrolled: true length from the start, sampled and read back linearly (straight past the ends).
struct WallTable {
  double s0 = 0.0, ds = 1.0;
  std::vector<double> len;
  [[nodiscard]] double at(const double s) const {
    const double t = (s - s0) / ds;
    const int i = std::clamp(int(std::floor(t)), 0, int(len.size()) - 2);
    return len[size_t(i)] + (len[size_t(i + 1)] - len[size_t(i)]) * (t - double(i));
  }
};

} // namespace

void applyCurve(Context &ctx, std::vector<Piece> &pieces) {
  if (pieces.empty())
    return;
  const CurveParams p = CurveParams::fromJson(ctx.mod.params);
  if (!p.path) {
    ctx.error(QStringLiteral("Pick a path for the curve to follow"));
    return;
  }
  const auto curve = ctx.in.curveFor ? ctx.in.curveFor(p.path) : nullptr;
  if (!curve || !curve->valid()) {
    ctx.error(QStringLiteral("The path is missing or has no length"), p.path);
    return;
  }
  const double L = curve->length();
  auto C = [&](double s) { return p.reverse ? curve->position(L - s) : curve->position(s); };
  auto T = [&](double s) { return p.reverse ? -curve->tangent(L - s) : curve->tangent(s); };
  auto pathS = [&](double s) { return p.reverse ? L - s : s; }; // path's own s, for diagnostics
  const bool vertical = p.frame == CurveParams::Frame::KeepVertical;
  const double maxSin = std::sin(glm::radians(p.maxSlopeDeg));

  auto frameAt = [&](double s) {
    Frame fr;
    fr.pos = C(s);
    const glm::dvec3 t = T(s);
    if (vertical) {
      glm::dvec3 h(t.x, 0.0, t.z);
      const double hl = glm::length(h);
      if (hl < 1e-6 || std::fabs(t.y) > maxSin) {
        fr.ok = false;
        return fr;
      }
      fr.along = h / hl;
      fr.up = kUp;
      fr.side = glm::cross(fr.along, kUp);
    } else {
      const glm::dvec3 side = glm::cross(t, kUp);
      const double sl = glm::length(side);
      if (sl < 1e-3) {
        fr.ok = false;
        return fr;
      }
      fr.along = t;
      fr.side = side / sl;
      fr.up = glm::cross(fr.side, t);
    }
    return fr;
  };

  glm::dvec3 f, u, r;
  curveAxes(ctx.in.orientation, p.axis, f, u, r);
  const glm::dvec3 axes[3] = {f, u, r};
  glm::dvec3 mn, mx;
  localBounds(pieces, axes, mn, mx);
  const double extent = mx[0] - mn[0];
  if (extent < 1e-6) {
    ctx.error(QStringLiteral("The objects have no length along the deform axis"));
    return;
  }
  const double midR = (mn[2] + mx[2]) * 0.5;
  const double anchorU = p.anchor == CurveParams::Anchor::Bottom ? mn[1]
                         : p.anchor == CurveParams::Anchor::Top  ? mx[1]
                                                                 : (mn[1] + mx[1]) * 0.5;
  double stretch = 1.0;
  if (p.stretch) {
    const double use = L - std::max(0.0, p.offsetAlong);
    if (use <= 1e-6) {
      ctx.error(QStringLiteral("Nothing of the path is left after the offset"), p.path);
      return;
    }
    stretch = use / extent;
    const double pct = (stretch - 1.0) * 100.0;
    ctx.out.summary[ctx.mod.id] = std::fabs(pct) < 0.05
                                      ? QStringLiteral("fits exactly")
                                      : QStringLiteral("stretched %1%2%").arg(pct > 0 ? "+" : "").arg(pct, 0, 'f', 1);
  }
  const double s0 = p.offsetAlong, s1 = p.offsetAlong + extent * stretch; // the span the input covers

  // The whole span must be drivable: no vertical runs (nor too steep for Keep vertical).
  {
    const int n = std::max(16, int((s1 - s0) / 0.5));
    for (int i = 0; i <= n; ++i) {
      const double s = s0 + (s1 - s0) * double(i) / n;
      if (!frameAt(s).ok) {
        ctx.error(vertical ? QStringLiteral("The path is too steep for Keep vertical at %1% (use Follow)")
                                 .arg(std::lround(100.0 * std::clamp(s / L, 0.0, 1.0)))
                           : QStringLiteral("The path runs straight up or down at %1%")
                                 .arg(std::lround(100.0 * std::clamp(s / L, 0.0, 1.0))),
                  p.path, pathS(std::max(0.0, s - 1.0)), pathS(std::min(L, s + 1.0)));
        return;
      }
    }
  }

  auto local = [&](const glm::dvec3 &v, double &s, double &x, double &y) {
    s = (glm::dot(v, f) - mn[0]) * stretch + p.offsetAlong;
    x = glm::dot(v, r) - midR + p.offsetSide;
    y = glm::dot(v, u) - anchorU + p.offsetHeight;
  };
  auto map = [&](const glm::dvec3 &v) {
    double s, x, y;
    local(v, s, x, y);
    const Frame fr = frameAt(s);
    return fr.pos + fr.side * x + fr.up * y;
  };
  // Corners are mapped many times (per face, texture split, deformation), each a walk of the path: cache per corner.
  struct CornerHash {
    size_t operator()(const std::array<long long, 3> &k) const noexcept {
      return size_t(k[0] * 73856093LL ^ k[1] * 19349663LL ^ k[2] * 83492791LL);
    }
  };
  std::unordered_map<std::array<long long, 3>, glm::dvec3, CornerHash> mapped;
  mapped.reserve(4096);
  const deform::Map mapCorner = [&](const glm::dvec3 &v) {
    const std::array<long long, 3> key {std::llround(v.x * 1e6), std::llround(v.y * 1e6), std::llround(v.z * 1e6)};
    const auto it = mapped.find(key);
    if (it != mapped.end())
      return it->second;
    const glm::dvec3 w = map(v);
    mapped.emplace(key, w);
    return w;
  };

  // Texture targets.
  deform::TexTarget texTarget;
  if (p.texture == CurveParams::Texture::Bend) {
    texTarget = [](const BrushFace &src, const glm::dvec3 &pos) { return uvOf(src.basis, pos); };
  } else if (p.texture == CurveParams::Texture::Path) {
    const Frame f0 = frameAt(s0);
    // The input placed straight at the start (rigid, no stretch): a straight corridor's world grid there.
    glm::dmat4 to(1.0), from(1.0);
    to[0] = glm::dvec4(f0.along, 0.0);
    to[1] = glm::dvec4(f0.up, 0.0);
    to[2] = glm::dvec4(f0.side, 0.0);
    to[3] = glm::dvec4(f0.pos, 1.0);
    from[0] = glm::dvec4(f, 0.0);
    from[1] = glm::dvec4(u, 0.0);
    from[2] = glm::dvec4(r, 0.0);
    from[3] = glm::dvec4(f * mn[0] + u * (anchorU - p.offsetHeight) + r * (midR - p.offsetSide), 1.0);
    const glm::dmat4 m0 = to * glm::inverse(from);
    const glm::dmat3 rot0(m0);
    auto tables = std::make_shared<std::map<long long, WallTable>>();
    auto wallTable = [=, &frameAt](const double x) -> const WallTable & {
      const long long key = std::llround(x * 1e4);
      const auto it = tables->find(key);
      if (it != tables->end())
        return it->second;
      WallTable t;
      // Every 2 Source units; the base curve is smooth, read back linearly.
      const int n = std::clamp(int((s1 - s0) / 0.2), 64, 4000);
      t.s0 = s0;
      t.ds = (s1 - s0) / n;
      t.len.resize(size_t(n) + 1);
      glm::dvec3 prev(0.0);
      for (int i = 0; i <= n; ++i) {
        const Frame fr = frameAt(s0 + t.ds * i);
        const glm::dvec3 pt = fr.pos + fr.side * x;
        t.len[size_t(i)] = i == 0 ? 0.0 : t.len[size_t(i - 1)] + glm::length(pt - prev);
        prev = pt;
      }
      return tables->emplace(key, std::move(t)).first->second;
    };
    texTarget = [=](const BrushFace &src, const glm::dvec3 &pos) {
      FaceTexBasis b0 = src.basis;
      const glm::dvec3 n0 = glm::normalize(rot0 * src.normal);
      transformFaceBasis(b0, m0, src.normal, n0, false);
      double s, x, y;
      local(pos, s, x, y);
      // Walls: true length along their own base curve; else the centre line.
      const double a = std::fabs(glm::dot(src.normal, r)) > 0.7 ? wallTable(x).at(s) : s - s0;
      return uvOf(b0, f0.pos + f0.along * a + f0.side * x + f0.up * y);
    };
  }

  std::vector<Piece> out;
  out.reserve(pieces.size());

  if (p.mode == CurveParams::Mode::Rigid) {
    // Units: each brush, or each array copy as a whole.
    std::map<int, std::vector<size_t>> units;
    for (size_t i = 0; i < pieces.size(); ++i)
      units[p.rigidUnit == CurveParams::Unit::Copy ? pieces[i].copy : int(i)].push_back(i);
    bool warned = false;
    for (const auto &[key, members]: units) {
      std::vector<Piece> group;
      for (const size_t i: members)
        group.push_back(pieces[i]);
      glm::dvec3 gmn, gmx;
      localBounds(group, axes, gmn, gmx);
      const double cAlong = (gmn[0] + gmx[0]) * 0.5;
      const glm::dvec3 origin = f * cAlong + u * anchorU + r * midR;
      const double s = (cAlong - mn[0]) * stretch + p.offsetAlong;
      const Frame fr = frameAt(s);
      const glm::dvec3 target = fr.pos + fr.side * p.offsetSide + fr.up * p.offsetHeight;
      // M = [along up side | target] · [f u r | origin]⁻¹
      glm::dmat4 to(1.0), from(1.0);
      to[0] = glm::dvec4(fr.along, 0.0);
      to[1] = glm::dvec4(fr.up, 0.0);
      to[2] = glm::dvec4(fr.side, 0.0);
      to[3] = glm::dvec4(target, 1.0);
      from[0] = glm::dvec4(f, 0.0);
      from[1] = glm::dvec4(u, 0.0);
      from[2] = glm::dvec4(r, 0.0);
      from[3] = glm::dvec4(origin, 1.0);
      const glm::dmat4 m = to * glm::inverse(from);
      const glm::dmat4 back = glm::inverse(m);
      for (Piece &pc: group) {
        const ConvexBrush before = pc.brush;
        pc.brush.transform(m, p.texture != CurveParams::Texture::World);
        if (p.texture == CurveParams::Texture::Path && pc.brush.faces.size() == before.faces.size()) {
          // Pieces stay whole: best flat fit of the path texture per face.
          for (size_t fi = 0; fi < pc.brush.faces.size() && fi < pc.brush.polygons.size(); ++fi) {
            BrushFace &face = pc.brush.faces[fi];
            if (!face.basis.valid)
              continue;
            std::vector<glm::dvec2> uv;
            for (const auto &q: pc.brush.polygons[fi])
              uv.push_back(texTarget(before.faces[fi], glm::dvec3(back * glm::dvec4(q, 1.0))));
            deform::solveBasisFromCorners(face.basis, pc.brush.polygons[fi], uv, face.normal);
          }
        }
        pc.identity = false;
        out.push_back(std::move(pc));
      }
      // Pieces stick out where the path bends under them (the sagitta).
      const double len = (gmx[0] - gmn[0]) * stretch;
      const double k = std::fabs(vertical ? curve->horizontalCurvature(pathS(s)) : curve->curvature(pathS(s)));
      if (!warned && len * len * k / 8.0 > 0.1) {
        ctx.warning(QStringLiteral("Pieces stick out at the bends: cut them shorter or use Bend"), p.path);
        warned = true;
      }
    }
    pieces = std::move(out);
    return;
  }

  // Bend: one set of cuts in s for everything, so neighbours split alike.
  std::vector<double> cutsS;
  {
    const double turnStep = glm::radians(p.segAngle);
    const double ds = std::max(0.02, std::min(0.5, (s1 - s0) / 2000.0));
    double acc = 0.0, last = s0;
    glm::dvec3 prev = T(s0);
    for (double s = s0 + ds; s < s1; s += ds) {
      const glm::dvec3 t = T(s);
      acc += std::acos(std::clamp(glm::dot(prev, t), -1.0, 1.0));
      prev = t;
      const bool byAngle = acc >= turnStep;
      const bool byLength = p.segMaxLen > 0.0 && s - last >= p.segMaxLen;
      if ((byAngle || byLength) && s - last >= p.segMinLen) {
        cutsS.push_back(s);
        last = s;
        acc = 0.0;
      }
    }
  }
  std::vector<double> cutsF; // the same cuts as planes along f
  cutsF.reserve(cutsS.size());
  for (const double s: cutsS)
    cutsF.push_back(mn[0] + (s - p.offsetAlong) / stretch);

  int triangulated = 0;
  for (const Piece &pc: pieces) {
    double lo = 1e300, hi = -1e300;
    for (const auto &poly: pc.brush.polygons)
      for (const auto &v: poly) {
        lo = std::min(lo, glm::dot(v, f));
        hi = std::max(hi, glm::dot(v, f));
      }
    std::vector<double> inside;
    for (const double d: cutsF)
      if (d > lo + 1e-4 && d < hi - 1e-4)
        inside.push_back(d);
    const std::vector<ConvexBrush> slices =
        inside.empty() ? std::vector<ConvexBrush> {pc.brush} : deform::slice(pc.brush, f, inside);
    int index = 0;
    for (const ConvexBrush &sl: slices) {
      // The bend must not squeeze the inner side to nothing (or fold it).
      for (const glm::dvec3 &v: sl.vertices()) {
        double s, x, y;
        local(v, s, x, y);
        const double e = 1e-3;
        const glm::dvec3 d = map(v + f * e) - map(v - f * e);
        const glm::dvec3 c0 = f * glm::dot(v, f) + u * (anchorU - p.offsetHeight) + r * (midR - p.offsetSide);
        const glm::dvec3 d0 = map(c0 + f * e) - map(c0 - f * e);
        const double l0 = glm::length(d0);
        if (l0 < 1e-12)
          continue;
        const double ratio = glm::dot(d, d0) / (l0 * l0);
        if (ratio < p.minJacobian) {
          const double k = std::fabs(vertical ? curve->horizontalCurvature(pathS(s)) : curve->curvature(pathS(s)));
          const double radius = k > 1e-9 ? 1.0 / k : 0.0;
          const double needed = std::fabs(x) / (1.0 - p.minJacobian);
          ctx.error(QStringLiteral("The bend is too tight at %1%: radius %2 units, these objects need %3")
                        .arg(std::lround(100.0 * std::clamp(s / L, 0.0, 1.0)))
                        .arg(units(radius))
                        .arg(units(needed)),
                    p.path, pathS(std::max(0.0, s - 2.0)), pathS(std::min(L, s + 2.0)));
          return;
        }
      }
      // Faces one flat mapping cannot texture exactly become triangles.
      const std::vector<ConvexBrush> parts =
          texTarget ? deform::splitForTexture(sl, mapCorner, texTarget, kTexTol) : std::vector<ConvexBrush> {sl};
      for (const ConvexBrush &part: parts) {
        deform::Result res = deform::deformBrush(part, mapCorner, texTarget);
        if (res.folded || !res.brush) {
          ctx.error(QStringLiteral("The bend folds a brush (cut it into shorter pieces or ease the turn)"), p.path);
          return;
        }
        if (res.brush->faces.size() > 128) {
          ctx.error(QStringLiteral("A bent brush has too many faces (cut it into shorter pieces)"), p.path);
          return;
        }
        triangulated += res.triangulated ? 1 : 0;
        Piece np = pc;
        np.brush = std::move(*res.brush);
        np.slice = index++;
        np.identity = false;
        out.push_back(std::move(np));
      }
    }
  }
  if (triangulated > 0)
    ctx.out.diagnostics.push_back({Diagnostic::Severity::Info, ctx.mod.id,
                                   QStringLiteral("%1 piece(s) twisted: faces split into triangles").arg(triangulated),
                                   p.path, -1.0, -1.0});
  QString &sum = ctx.out.summary[ctx.mod.id];
  const QString pieceText = QStringLiteral("%1 pieces").arg(out.size());
  sum = sum.isEmpty() ? pieceText : pieceText + QStringLiteral(" · ") + sum;
  pieces = std::move(out);
}

} // namespace eng::mods
