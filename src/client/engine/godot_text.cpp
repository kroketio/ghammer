#include "godot_text.h"

#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace eng::godot {

QString gdNumber(double v, bool forceDecimal) {
  if (std::isnan(v))
    return QStringLiteral("nan");
  if (std::isinf(v))
    return v > 0 ? QStringLiteral("inf") : QStringLiteral("inf_neg");
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    const QString s = QString::number(qint64(v));
    return forceDecimal ? s + QStringLiteral(".0") : s;
  }
  QString s = QString::number(v, 'g', 7);
  if (forceDecimal && !s.contains(QLatin1Char('.')) && !s.contains(QLatin1Char('e')))
    s += QStringLiteral(".0");
  return s;
}

QString gdString(const QString &s) {
  QString out;
  out.reserve(s.size() + 2);
  out += QLatin1Char('"');
  for (const QChar c: s) {
    switch (c.unicode()) {
      case '\\': out += QStringLiteral("\\\\"); break;
      case '"': out += QStringLiteral("\\\""); break;
      case '\n': out += QStringLiteral("\\n"); break;
      case '\r': out += QStringLiteral("\\r"); break;
      case '\t': out += QStringLiteral("\\t"); break;
      default: out += c;
    }
  }
  out += QLatin1Char('"');
  return out;
}

QString validNodeName(const QString &name) {
  QString n = name;
  static const QString bad = QStringLiteral(".:@/\"%");
  for (QChar &c: n)
    if (bad.contains(c) || c.unicode() < 32)
      c = QLatin1Char('_');
  n = n.trimmed();
  return n.isEmpty() ? QStringLiteral("Node") : n;
}

GdValue GdValue::boolean(bool v) {
  GdValue r;
  r.kind = Kind::Bool;
  r.b = v;
  return r;
}
GdValue GdValue::integer(qint64 v) {
  GdValue r;
  r.kind = Kind::Int;
  r.i = v;
  return r;
}
GdValue GdValue::number(double v) {
  GdValue r;
  r.kind = Kind::Float;
  r.f = v;
  return r;
}
GdValue GdValue::string(const QString &v) {
  GdValue r;
  r.kind = Kind::String;
  r.s = v;
  return r;
}
GdValue GdValue::call(const QString &name, const QVector<GdValue> &args) {
  GdValue r;
  r.kind = Kind::Call;
  r.s = name;
  r.items = args;
  return r;
}
GdValue GdValue::raw(const QString &text) {
  GdValue r;
  r.kind = Kind::Raw;
  r.s = text;
  return r;
}
GdValue GdValue::color(const QColor &c) {
  return call(QStringLiteral("Color"), {number(c.redF()), number(c.greenF()), number(c.blueF()), number(c.alphaF())});
}
GdValue GdValue::transform(const glm::vec3 &x, const glm::vec3 &y, const glm::vec3 &z, const glm::vec3 &o) {
  // Transform3D(...) lists the basis row by row (rows[i][j]); column j is axis j.
  const auto n = [](float v) { return number(std::fabs(v) < 1e-7f ? 0.0 : double(v)); };
  return call(QStringLiteral("Transform3D"),
              {n(x.x), n(y.x), n(z.x), n(x.y), n(y.y), n(z.y), n(x.z), n(y.z), n(z.z), n(o.x), n(o.y), n(o.z)});
}
GdValue GdValue::extResource(const QString &id) { return call(QStringLiteral("ExtResource"), {string(id)}); }

double GdValue::toNumber(double fallback) const {
  switch (kind) {
    case Kind::Int: return double(i);
    case Kind::Float: return f;
    case Kind::Bool: return b ? 1.0 : 0.0;
    default: return fallback;
  }
}

const GdValue *GdValue::find(const QString &key) const {
  for (const auto &e: dict)
    if ((e.first.kind == Kind::String || e.first.kind == Kind::StringName) && e.first.s == key)
      return &e.second;
  return nullptr;
}

void GdValue::set(const QString &key, const GdValue &v) {
  for (auto &e: dict)
    if ((e.first.kind == Kind::String || e.first.kind == Kind::StringName) && e.first.s == key) {
      e.second = v;
      return;
    }
  dict.append({string(key), v});
}

namespace {

struct Parser {
  const QString &t;
  int p = 0;
  bool ok = true;

  void ws() {
    while (p < t.size() && t[p].isSpace())
      ++p;
  }
  bool peek(QChar c) {
    ws();
    return p < t.size() && t[p] == c;
  }
  QString str() {
    // t[p] == '"'
    ++p;
    QString out;
    while (p < t.size() && t[p] != QLatin1Char('"')) {
      QChar c = t[p++];
      if (c == QLatin1Char('\\') && p < t.size()) {
        const QChar e = t[p++];
        switch (e.unicode()) {
          case 'n': out += QLatin1Char('\n'); break;
          case 't': out += QLatin1Char('\t'); break;
          case 'r': out += QLatin1Char('\r'); break;
          case 'u':
            if (p + 4 <= t.size()) {
              out += QChar(t.mid(p, 4).toUShort(nullptr, 16));
              p += 4;
            }
            break;
          default: out += e;
        }
      } else {
        out += c;
      }
    }
    if (p >= t.size()) {
      ok = false;
      return out;
    }
    ++p;
    return out;
  }
  GdValue value() {
    ws();
    if (p >= t.size()) {
      ok = false;
      return {};
    }
    const QChar c = t[p];
    if (c == QLatin1Char('"'))
      return GdValue::string(str());
    if ((c == QLatin1Char('&') || c == QLatin1Char('^')) && p + 1 < t.size() && t[p + 1] == QLatin1Char('"')) {
      ++p;
      GdValue v;
      v.kind = c == QLatin1Char('&') ? GdValue::Kind::StringName : GdValue::Kind::NodePath;
      v.s = str();
      return v;
    }
    if (c == QLatin1Char('[')) {
      ++p;
      GdValue v;
      v.kind = GdValue::Kind::Array;
      if (peek(QLatin1Char(']'))) {
        ++p;
        return v;
      }
      while (ok) {
        v.items.append(value());
        if (peek(QLatin1Char(','))) {
          ++p;
          if (peek(QLatin1Char(']'))) {
            ++p;
            break;
          }
          continue;
        }
        if (peek(QLatin1Char(']'))) {
          ++p;
          break;
        }
        ok = false;
      }
      return v;
    }
    if (c == QLatin1Char('{')) {
      ++p;
      GdValue v;
      v.kind = GdValue::Kind::Dict;
      if (peek(QLatin1Char('}'))) {
        ++p;
        return v;
      }
      while (ok) {
        GdValue k = value();
        if (!peek(QLatin1Char(':'))) {
          ok = false;
          break;
        }
        ++p;
        GdValue val = value();
        v.dict.append({k, val});
        if (peek(QLatin1Char(','))) {
          ++p;
          if (peek(QLatin1Char('}'))) {
            ++p;
            break;
          }
          continue;
        }
        if (peek(QLatin1Char('}'))) {
          ++p;
          break;
        }
        ok = false;
      }
      return v;
    }
    if (c.isDigit() || c == QLatin1Char('-') || c == QLatin1Char('+') || c == QLatin1Char('.')) {
      const int s = p;
      if (t[p] == QLatin1Char('-') || t[p] == QLatin1Char('+'))
        ++p;
      bool isFloat = false;
      while (p < t.size()) {
        const QChar d = t[p];
        if (d.isDigit()) {
          ++p;
        } else if (d == QLatin1Char('.')) {
          isFloat = true;
          ++p;
        } else if (d == QLatin1Char('e') || d == QLatin1Char('E')) {
          isFloat = true;
          ++p;
          if (p < t.size() && (t[p] == QLatin1Char('-') || t[p] == QLatin1Char('+')))
            ++p;
        } else {
          break;
        }
      }
      const QString num = t.mid(s, p - s);
      bool good = false;
      if (isFloat) {
        const double d = num.toDouble(&good);
        ok = ok && good;
        return GdValue::number(d);
      }
      const qint64 n = num.toLongLong(&good);
      if (!good) {
        const double d = num.toDouble(&good);
        ok = ok && good;
        return GdValue::number(d);
      }
      return GdValue::integer(n);
    }
    if (c.isLetter() || c == QLatin1Char('_')) {
      const int s = p;
      while (p < t.size() && (t[p].isLetterOrNumber() || t[p] == QLatin1Char('_')))
        ++p;
      const QString id = t.mid(s, p - s);
      if (id == QLatin1String("true"))
        return GdValue::boolean(true);
      if (id == QLatin1String("false"))
        return GdValue::boolean(false);
      if (id == QLatin1String("null"))
        return {};
      if (id == QLatin1String("inf"))
        return GdValue::number(INFINITY);
      if (id == QLatin1String("inf_neg"))
        return GdValue::number(-INFINITY);
      if (id == QLatin1String("nan"))
        return GdValue::number(NAN);
      if (peek(QLatin1Char('('))) {
        ++p;
        GdValue v;
        v.kind = GdValue::Kind::Call;
        v.s = id;
        if (peek(QLatin1Char(')'))) {
          ++p;
          return v;
        }
        while (ok) {
          v.items.append(value());
          if (peek(QLatin1Char(','))) {
            ++p;
            continue;
          }
          if (peek(QLatin1Char(')'))) {
            ++p;
            break;
          }
          ok = false;
        }
        return v;
      }
      ok = false;  // typed arrays (Array[int]([...])) and the like: kept raw
      return {};
    }
    ok = false;
    return {};
  }
};

QString itemText(const GdValue &v, bool inCall) {
  if (v.kind == GdValue::Kind::Float)
    return gdNumber(v.f, !inCall);
  return v.toText();
}

bool isStringish(GdValue::Kind k) {
  return k == GdValue::Kind::String || k == GdValue::Kind::StringName || k == GdValue::Kind::NodePath;
}
bool isNumber(GdValue::Kind k) { return k == GdValue::Kind::Int || k == GdValue::Kind::Float; }

} // namespace

GdValue GdValue::parse(const QString &text) {
  Parser ps{text};
  GdValue v = ps.value();
  ps.ws();
  if (!ps.ok || ps.p != text.size())
    return raw(text.trimmed());
  return v;
}

QString GdValue::toText() const {
  switch (kind) {
    case Kind::Null: return QStringLiteral("null");
    case Kind::Bool: return b ? QStringLiteral("true") : QStringLiteral("false");
    case Kind::Int: return QString::number(i);
    case Kind::Float: return gdNumber(f, true);
    case Kind::String: return gdString(s);
    case Kind::StringName: return QLatin1Char('&') + gdString(s);
    case Kind::NodePath: return QLatin1Char('^') + gdString(s);
    case Kind::Array: {
      QStringList parts;
      for (const auto &e: items)
        parts << itemText(e, false);
      return QLatin1Char('[') + parts.join(QStringLiteral(", ")) + QLatin1Char(']');
    }
    case Kind::Dict: {
      if (dict.isEmpty())
        return QStringLiteral("{}");
      QStringList parts;
      for (const auto &[k, v]: dict)
        parts << k.toText() + QStringLiteral(": ") + v.toText();
      return QStringLiteral("{\n") + parts.join(QStringLiteral(",\n")) + QStringLiteral("\n}");
    }
    case Kind::Call: {
      QStringList parts;
      for (const auto &e: items)
        parts << itemText(e, true);
      return s + QLatin1Char('(') + parts.join(QStringLiteral(", ")) + QLatin1Char(')');
    }
    case Kind::Raw: return s;
  }
  return {};
}

bool GdValue::equals(const GdValue &o, double eps) const {
  if (kind == Kind::Raw || o.kind == Kind::Raw)
    return toText() == o.toText();
  if (isNumber(kind) && isNumber(o.kind)) {
    const double a = toNumber(), c = o.toNumber();
    if (std::isnan(a) || std::isnan(c))
      return std::isnan(a) && std::isnan(c);
    return std::fabs(a - c) <= eps * std::max(1.0, std::max(std::fabs(a), std::fabs(c)));
  }
  if (isStringish(kind) && isStringish(o.kind))
    return s == o.s && (kind == o.kind || kind != Kind::NodePath) && (kind == o.kind || o.kind != Kind::NodePath);
  if (kind != o.kind)
    return false;
  switch (kind) {
    case Kind::Null: return true;
    case Kind::Bool: return b == o.b;
    case Kind::Call:
      if (s != o.s)
        return false;
      [[fallthrough]];
    case Kind::Array:
      if (items.size() != o.items.size())
        return false;
      for (int k = 0; k < items.size(); ++k)
        if (!items[k].equals(o.items[k], eps))
          return false;
      return true;
    case Kind::Dict:
      if (dict.size() != o.dict.size())
        return false;
      for (const auto &[k, v]: dict) {
        bool found = false;
        for (const auto &[k2, v2]: o.dict)
          if (k.equals(k2, eps)) {
            if (!v.equals(v2, eps))
              return false;
            found = true;
            break;
          }
        if (!found)
          return false;
      }
      return true;
    default: return false;
  }
}

namespace {

// End of a value at `p`: a property runs to the line end once brackets and strings close (dictionaries span
// lines); an attribute stops at whitespace or ']' at depth 0.
int valueEnd(const QString &t, int p, bool attr) {
  int depth = 0;
  while (p < t.size()) {
    const QChar c = t[p];
    if (c == QLatin1Char('"')) {
      ++p;
      while (p < t.size() && t[p] != QLatin1Char('"')) {
        if (t[p] == QLatin1Char('\\'))
          ++p;
        ++p;
      }
      ++p;
      continue;
    }
    if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) {
      ++depth;
    } else if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) {
      if (depth == 0 && attr)
        return p;
      --depth;
    } else if (depth <= 0) {
      if (c == QLatin1Char('\n'))
        return p;
      if (attr && c.isSpace())
        return p;
    }
    ++p;
  }
  return int(t.size());
}

QString trimRight(QString s) {
  while (!s.isEmpty() && s.back().isSpace())
    s.chop(1);
  return s;
}

} // namespace

GdTextFile GdTextFile::parse(const QString &text) {
  GdTextFile f;
  f.m_original = text;
  Section cur;
  bool spacedSeen = false, compactSeen = false;
  int p = 0;
  const int n = int(text.size());
  while (p < n) {
    int eol = text.indexOf(QLatin1Char('\n'), p);
    if (eol < 0)
      eol = n;
    const QString line = text.mid(p, eol - p);
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
      if (!cur.tag.isEmpty() && cur.props.isEmpty())
        cur.blankAfterHeader = true;
      p = eol + 1;
      continue;
    }
    if (line.startsWith(QLatin1Char('['))) {
      if (!cur.tag.isEmpty() || !cur.props.isEmpty())
        f.sections.append(cur);
      cur = Section();
      const int hs = p + 1;
      int e = hs;
      while (e < n && !text[e].isSpace() && text[e] != QLatin1Char(']'))
        ++e;
      cur.tag = text.mid(hs, e - hs);
      int q = e;
      while (q < n) {
        while (q < n && (text[q] == QLatin1Char(' ') || text[q] == QLatin1Char('\t')))
          ++q;
        if (q >= n || text[q] == QLatin1Char(']') || text[q] == QLatin1Char('\n'))
          break;
        const int eq = text.indexOf(QLatin1Char('='), q);
        if (eq < 0)
          break;
        Attr a;
        a.key = text.mid(q, eq - q).trimmed();
        const int vs = eq + 1;
        const int ve = valueEnd(text, vs, true);
        a.raw = text.mid(vs, ve - vs);
        cur.attrs.append(a);
        q = ve;
      }
      const int close = text.indexOf(QLatin1Char('\n'), q);
      p = close < 0 ? n : close + 1;
      continue;
    }
    if (trimmed.startsWith(QLatin1Char(';')) || trimmed.startsWith(QLatin1Char('#'))) {
      cur.props.append({QString(), line});
      p = eol + 1;
      continue;
    }
    const int eq = text.indexOf(QLatin1Char('='), p);
    if (eq < 0 || eq > eol) {
      cur.props.append({QString(), line});
      p = eol + 1;
      continue;
    }
    Prop pr;
    pr.key = text.mid(p, eq - p).trimmed();
    if (eq > p && text[eq - 1] == QLatin1Char(' '))
      spacedSeen = true;
    else
      compactSeen = true;
    int vs = eq + 1;
    while (vs < n && text[vs] == QLatin1Char(' '))
      ++vs;
    const int ve = valueEnd(text, vs, false);
    pr.raw = trimRight(text.mid(vs, ve - vs));
    cur.props.append(pr);
    p = ve < n ? ve + 1 : n;
  }
  if (!cur.tag.isEmpty() || !cur.props.isEmpty())
    f.sections.append(cur);
  f.m_importStyle = compactSeen && !spacedSeen;
  return f;
}

GdTextFile GdTextFile::create(const QString &tag, const QVector<QPair<QString, GdValue>> &attrs, bool importStyle) {
  GdTextFile f;
  f.m_importStyle = importStyle;
  f.m_dirty = true;
  Section s;
  s.tag = tag;
  s.blankAfterHeader = importStyle;
  for (const auto &[k, v]: attrs)
    s.attrs.append({k, v.toText()});
  f.sections.append(s);
  return f;
}

QString GdTextFile::toText() const {
  if (!m_dirty)
    return m_original;
  QString out;
  const QString sep = m_importStyle ? QStringLiteral("=") : QStringLiteral(" = ");
  for (int i = 0; i < sections.size(); ++i) {
    const Section &s = sections[i];
    if (!s.tag.isEmpty()) {
      const bool chained = s.tag == QLatin1String("ext_resource") && i > 0
          && sections[i - 1].tag == QLatin1String("ext_resource") && sections[i - 1].props.isEmpty();
      if (!out.isEmpty() && !chained)
        out += QLatin1Char('\n');
      out += QLatin1Char('[') + s.tag;
      for (const Attr &a: s.attrs)
        out += QLatin1Char(' ') + a.key + QLatin1Char('=') + a.raw;
      out += QStringLiteral("]\n");
      if (s.blankAfterHeader && !s.props.isEmpty())
        out += QLatin1Char('\n');
    }
    for (const Prop &p: s.props) {
      if (p.key.isEmpty())
        out += p.raw + QLatin1Char('\n');
      else
        out += p.key + sep + p.raw + QLatin1Char('\n');
    }
  }
  return out;
}

int GdTextFile::find(const QString &tag, int from) const {
  for (int i = std::max(0, from); i < sections.size(); ++i)
    if (sections[i].tag == tag)
      return i;
  return -1;
}

int GdTextFile::ensureSection(const QString &tag) {
  const int i = find(tag);
  if (i >= 0)
    return i;
  Section s;
  s.tag = tag;
  s.blankAfterHeader = m_importStyle;
  sections.append(s);
  m_dirty = true;
  return int(sections.size()) - 1;
}

GdValue GdTextFile::attr(int sec, const QString &key) const {
  if (sec < 0 || sec >= sections.size())
    return {};
  for (const Attr &a: sections[sec].attrs)
    if (a.key == key)
      return GdValue::parse(a.raw);
  return {};
}

bool GdTextFile::setAttr(int sec, const QString &key, const GdValue &v) {
  if (sec < 0 || sec >= sections.size())
    return false;
  for (Attr &a: sections[sec].attrs)
    if (a.key == key) {
      if (GdValue::parse(a.raw).equals(v))
        return false;
      a.raw = v.toText();
      m_dirty = true;
      return true;
    }
  sections[sec].attrs.append({key, v.toText()});
  m_dirty = true;
  return true;
}

int GdTextFile::findProp(int sec, const QString &key) const {
  if (sec < 0 || sec >= sections.size())
    return -1;
  const auto &ps = sections[sec].props;
  for (int k = 0; k < ps.size(); ++k)
    if (ps[k].key == key)
      return k;
  return -1;
}

GdValue GdTextFile::prop(int sec, const QString &key) const {
  const int k = findProp(sec, key);
  return k < 0 ? GdValue() : GdValue::parse(sections[sec].props[k].raw);
}

bool GdTextFile::setProp(int sec, const QString &key, const GdValue &v) {
  if (sec < 0 || sec >= sections.size())
    return false;
  const int k = findProp(sec, key);
  if (k >= 0) {
    if (GdValue::parse(sections[sec].props[k].raw).equals(v))
      return false;
    sections[sec].props[k].raw = v.toText();
  } else {
    sections[sec].props.append({key, v.toText()});
  }
  m_dirty = true;
  return true;
}

bool GdTextFile::removeProp(int sec, const QString &key) {
  const int k = findProp(sec, key);
  if (k < 0)
    return false;
  sections[sec].props.removeAt(k);
  m_dirty = true;
  return true;
}

QString GdTextFile::extResource(const QString &type, const QString &path, const QString &uid) {
  int last = -1;
  QSet<QString> ids;
  for (int i = 0; i < sections.size(); ++i) {
    if (sections[i].tag != QLatin1String("ext_resource"))
      continue;
    last = i;
    const QString id = attr(i, QStringLiteral("id")).toString();
    ids.insert(id);
    if (attr(i, QStringLiteral("path")).toString() == path) {
      if (!uid.isEmpty())
        setAttr(i, QStringLiteral("uid"), GdValue::string(uid));
      return id;
    }
  }
  const QString h = QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(5));
  int n = int(ids.size()) + 1;
  QString id = QStringLiteral("%1_%2").arg(n).arg(h);
  while (ids.contains(id))
    id = QStringLiteral("%1_%2").arg(++n).arg(h);
  Section s;
  s.tag = QStringLiteral("ext_resource");
  s.attrs.append({QStringLiteral("type"), gdString(type)});
  if (!uid.isEmpty())
    s.attrs.append({QStringLiteral("uid"), gdString(uid)});
  s.attrs.append({QStringLiteral("path"), gdString(path)});
  s.attrs.append({QStringLiteral("id"), gdString(id)});
  const int at = last >= 0 ? last + 1 : std::min<int>(1, int(sections.size()));
  sections.insert(at, s);
  m_dirty = true;
  return id;
}

int GdTextFile::pruneExtResources() {
  static const QRegularExpression re(QStringLiteral("ExtResource\\(\\s*\"([^\"]*)\"\\s*\\)"));
  QSet<QString> used;
  for (const Section &s: sections) {
    if (s.tag == QLatin1String("ext_resource"))
      continue;
    for (const Attr &a: s.attrs)
      for (auto it = re.globalMatch(a.raw); it.hasNext();)
        used.insert(it.next().captured(1));
    for (const Prop &p: s.props)
      for (auto it = re.globalMatch(p.raw); it.hasNext();)
        used.insert(it.next().captured(1));
  }
  int removed = 0;
  for (int i = int(sections.size()) - 1; i >= 0; --i)
    if (sections[i].tag == QLatin1String("ext_resource")
        && !used.contains(attr(i, QStringLiteral("id")).toString())) {
      sections.removeAt(i);
      ++removed;
    }
  if (removed)
    m_dirty = true;
  return removed;
}

int GdTextFile::rootNode() const {
  for (int i = 0; i < sections.size(); ++i)
    if (sections[i].tag == QLatin1String("node") && attr(i, QStringLiteral("parent")).isNull())
      return i;
  return -1;
}

QString GdTextFile::nodePath(int sec) const {
  if (sec < 0 || sec >= sections.size() || sections[sec].tag != QLatin1String("node"))
    return {};
  const GdValue parent = attr(sec, QStringLiteral("parent"));
  if (parent.isNull())
    return QStringLiteral(".");
  const QString name = attr(sec, QStringLiteral("name")).toString();
  const QString pp = parent.toString();
  return pp == QLatin1String(".") ? name : pp + QLatin1Char('/') + name;
}

int GdTextFile::nodeByProp(const QString &key, const GdValue &value) const {
  for (int i = 0; i < sections.size(); ++i)
    if (sections[i].tag == QLatin1String("node") && findProp(i, key) >= 0 && prop(i, key).equals(value))
      return i;
  return -1;
}

QVector<int> GdTextFile::children(const QString &path) const {
  QVector<int> out;
  for (int i = 0; i < sections.size(); ++i)
    if (sections[i].tag == QLatin1String("node") && attr(i, QStringLiteral("parent")).toString() == path
        && !attr(i, QStringLiteral("parent")).isNull())
      out.append(i);
  return out;
}

QString GdTextFile::uniqueChildName(const QString &parentPath, const QString &wanted, int except) const {
  QSet<QString> taken;
  for (const int c: children(parentPath))
    if (c != except)
      taken.insert(attr(c, QStringLiteral("name")).toString());
  const QString base = validNodeName(wanted);
  QString name = base;
  for (int k = 2; taken.contains(name); ++k)
    name = QStringLiteral("%1_%2").arg(base).arg(k);
  return name;
}

int GdTextFile::addNode(const QString &name, const QString &type, const QString &parentPath,
                        const QString &instanceExtId) {
  Section s;
  s.tag = QStringLiteral("node");
  s.attrs.append({QStringLiteral("name"), gdString(name)});
  if (!type.isEmpty() && instanceExtId.isEmpty())
    s.attrs.append({QStringLiteral("type"), gdString(type)});
  if (!parentPath.isEmpty())
    s.attrs.append({QStringLiteral("parent"), gdString(parentPath)});
  if (!instanceExtId.isEmpty())
    s.attrs.append({QStringLiteral("instance"), GdValue::extResource(instanceExtId).toText()});
  int at = int(sections.size());
  for (int i = int(sections.size()) - 1; i >= 0; --i)
    if (sections[i].tag == QLatin1String("node") || sections[i].tag == QLatin1String("sub_resource")
        || sections[i].tag == QLatin1String("ext_resource") || i == 0) {
      at = i + 1;
      break;
    }
  sections.insert(at, s);
  m_dirty = true;
  return at;
}

bool GdTextFile::renameNode(int sec, const QString &name) {
  const QString oldPath = nodePath(sec);
  if (oldPath.isEmpty() || oldPath == QLatin1String(".") || attr(sec, QStringLiteral("name")).toString() == name)
    return false;
  const GdValue parent = attr(sec, QStringLiteral("parent"));
  const QString newPath = parent.toString() == QLatin1String(".") ? name : parent.toString() + QLatin1Char('/') + name;
  const auto moved = [&](const QString &p) -> QString {
    if (p == oldPath)
      return newPath;
    if (p.startsWith(oldPath + QLatin1Char('/')))
      return newPath + p.mid(oldPath.size());
    return {};
  };
  for (int i = 0; i < sections.size(); ++i) {
    const QString &tag = sections[i].tag;
    const QStringList keys = tag == QLatin1String("node")         ? QStringList {QStringLiteral("parent")}
                             : tag == QLatin1String("connection") ? QStringList {QStringLiteral("from"), QStringLiteral("to")}
                             : tag == QLatin1String("editable")   ? QStringList {QStringLiteral("path")}
                                                                  : QStringList {};
    for (const QString &k: keys) {
      const GdValue v = attr(i, k);
      if (v.kind != GdValue::Kind::String)
        continue;
      if (const QString np = moved(v.s); !np.isEmpty())
        setAttr(i, k, GdValue::string(np));
    }
  }
  setAttr(sec, QStringLiteral("name"), GdValue::string(name));
  return true;
}

int GdTextFile::removeNodeTree(int sec) {
  const QString path = nodePath(sec);
  if (path.isEmpty() || path == QLatin1String("."))
    return 0;
  const auto inside = [&path](const QString &p) { return p == path || p.startsWith(path + QLatin1Char('/')); };
  int removed = 0;
  for (int i = int(sections.size()) - 1; i >= 0; --i) {
    const QString &tag = sections[i].tag;
    bool drop = false;
    if (tag == QLatin1String("node"))
      drop = inside(nodePath(i));
    else if (tag == QLatin1String("connection"))
      drop = inside(attr(i, QStringLiteral("from")).toString()) || inside(attr(i, QStringLiteral("to")).toString());
    else if (tag == QLatin1String("editable"))
      drop = inside(attr(i, QStringLiteral("path")).toString());
    if (drop) {
      sections.removeAt(i);
      ++removed;
    }
  }
  if (removed)
    m_dirty = true;
  return removed;
}

QString readUid(const QString &absPath) {
  const QString suffix = QFileInfo(absPath).suffix().toLower();
  if (suffix == QLatin1String("gd") || suffix == QLatin1String("gdshader")) {
    // Scripts and shaders keep theirs in <file>.uid.
    QFile u(absPath + QStringLiteral(".uid"));
    if (!u.open(QIODevice::ReadOnly | QIODevice::Text))
      return {};
    const QString id = QString::fromUtf8(u.readAll()).trimmed();
    return id.startsWith(QLatin1String("uid://")) ? id : QString();
  }
  const bool text = suffix == QLatin1String("tscn") || suffix == QLatin1String("tres");
  QFile f(text ? absPath : absPath + QStringLiteral(".import"));
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return {};
  const GdTextFile file = GdTextFile::parse(QString::fromUtf8(f.readAll()));
  const int sec = text ? 0 : file.find(QStringLiteral("remap"));
  if (sec < 0)
    return {};
  if (text) {
    const GdValue v = file.attr(sec, QStringLiteral("uid"));
    return v.kind == GdValue::Kind::String ? v.s : QString();
  }
  const GdValue v = file.prop(sec, QStringLiteral("uid"));
  return v.kind == GdValue::Kind::String ? v.s : QString();
}

int writeIfChanged(const QString &path, const QByteArray &bytes, QString *error) {
  {
    QFile cur(path);
    if (cur.exists() && cur.size() == bytes.size() && cur.open(QIODevice::ReadOnly) && cur.readAll() == bytes)
      return 0;
  }
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
    if (error)
      *error = QStringLiteral("Cannot write %1: %2").arg(path, f.errorString());
    return -1;
  }
  return 1;
}

} // namespace eng::godot
