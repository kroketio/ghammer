#include "io.h"

#include <QColor>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace eng::io {

QString paramTypeName(const ParamType t) {
  switch (t) {
    case ParamType::Void: return QStringLiteral("void");
    case ParamType::Bool: return QStringLiteral("bool");
    case ParamType::Int: return QStringLiteral("int");
    case ParamType::Float: return QStringLiteral("float");
    case ParamType::String: return QStringLiteral("string");
    case ParamType::Vec3: return QStringLiteral("vector3");
    case ParamType::Color: return QStringLiteral("color");
    case ParamType::Target: return QStringLiteral("target");
    case ParamType::Variant: return QStringLiteral("variant");
  }
  return QStringLiteral("void");
}

ParamType paramTypeFromString(const QString &s) {
  static const QHash<QString, ParamType> map = {
      {"void", ParamType::Void},       {"", ParamType::Void},           {"bool", ParamType::Bool},
      {"int", ParamType::Int},         {"integer", ParamType::Int},     {"float", ParamType::Float},
      {"string", ParamType::String},   {"vector3", ParamType::Vec3},    {"vec3", ParamType::Vec3},
      {"vector", ParamType::Vec3},     {"color", ParamType::Color},     {"color255", ParamType::Color},
      {"target", ParamType::Target},   {"targetname", ParamType::Target}, {"variant", ParamType::Variant}};
  return map.value(s.trimmed().toLower(), ParamType::Variant);
}

namespace {

QStringList numbers(const QString &s) {
  static const QRegularExpression sep(QStringLiteral("[\\s,]+"));
  return s.trimmed().split(sep, Qt::SkipEmptyParts);
}

} // namespace

bool parseParam(const ParamType t, const QString &raw, QVariant *out) {
  const QString s = raw.trimmed();
  if (s.isEmpty()) {
    if (out)
      *out = QVariant();
    return true;
  }
  bool ok = false;
  switch (t) {
    case ParamType::Void:
    case ParamType::String:
    case ParamType::Target:
    case ParamType::Variant:
      if (out)
        *out = s;
      return true;
    case ParamType::Bool: {
      const QString l = s.toLower();
      if (l == "1" || l == "true" || l == "yes" || l == "on") {
        if (out)
          *out = true;
        return true;
      }
      if (l == "0" || l == "false" || l == "no" || l == "off") {
        if (out)
          *out = false;
        return true;
      }
      return false;
    }
    case ParamType::Int: {
      const int v = s.toInt(&ok);
      if (!ok) { // Source passes floats to int inputs all the time ("1.000")
        const double d = s.toDouble(&ok);
        if (ok && out)
          *out = int(std::lround(d));
        return ok;
      }
      if (out)
        *out = v;
      return true;
    }
    case ParamType::Float: {
      const double v = s.toDouble(&ok);
      if (ok && out)
        *out = v;
      return ok;
    }
    case ParamType::Vec3: {
      const QStringList p = numbers(s);
      if (p.size() != 3)
        return false;
      QVariantList v;
      for (const auto &n: p) {
        const double d = n.toDouble(&ok);
        if (!ok)
          return false;
        v << d;
      }
      if (out)
        *out = v;
      return true;
    }
    case ParamType::Color: {
      if (s.startsWith('#')) {
        const QColor c(s);
        if (!c.isValid())
          return false;
        if (out)
          *out = c;
        return true;
      }
      const QStringList p = numbers(s);
      if (p.size() != 3 && p.size() != 4)
        return false;
      int v[4] = {0, 0, 0, 255};
      for (int i = 0; i < p.size(); ++i) {
        v[i] = p[i].toInt(&ok);
        if (!ok || v[i] < 0 || v[i] > 255)
          return false;
      }
      if (out)
        *out = QColor(v[0], v[1], v[2], v[3]);
      return true;
    }
  }
  return false;
}

QString snake(const QString &s) {
  QString out;
  out.reserve(s.size() + 8);
  for (int i = 0; i < s.size(); ++i) {
    const QChar c = s[i];
    if (c.isUpper()) {
      const bool prevLowerOrDigit = i > 0 && (s[i - 1].isLower() || s[i - 1].isDigit());
      const bool acronymEnd = i > 0 && s[i - 1].isUpper() && i + 1 < s.size() && s[i + 1].isLower();
      if ((prevLowerOrDigit || acronymEnd) && !out.endsWith('_'))
        out += '_';
      out += c.toLower();
    } else if (c == ' ' || c == '-' || c == '.') {
      if (!out.endsWith('_'))
        out += '_';
    } else {
      out += c;
    }
  }
  return out;
}

QString godotGroup(const QString &targetname) {
  if (targetname.startsWith('@'))
    return targetname.mid(1);
  return QStringLiteral("io_") + targetname.toLower();
}

bool isRuntimeTarget(const QString &t) {
  return t.compare(QLatin1String("!activator"), Qt::CaseInsensitive) == 0 ||
         t.compare(QLatin1String("!caller"), Qt::CaseInsensitive) == 0 ||
         t.compare(QLatin1String("!player"), Qt::CaseInsensitive) == 0;
}

bool isExternalTarget(const QString &t) { return t.size() > 1 && t.startsWith('@'); }

bool isWildcard(const QString &t) { return t.contains('*') || t.contains('?'); }

namespace {

bool matchName(const QString &pattern, const QString &name) {
  if (name.isEmpty())
    return false;
  if (!isWildcard(pattern))
    return pattern.compare(name, Qt::CaseInsensitive) == 0;
  // Caches compiled wildcards: resolve() runs per connection.
  static thread_local QHash<QString, QRegularExpression> cache;
  auto it = cache.find(pattern);
  if (it == cache.end()) {
    if (cache.size() > 256)
      cache.clear();
    it = cache.insert(pattern, QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern),
                                                  QRegularExpression::CaseInsensitiveOption));
  }
  return it->match(name).hasMatch();
}

bool hasIO(const Entity &e) {
  const EntityClass *c = e.cls();
  return c && c->io;
}

} // namespace

bool addresses(const QString &target, const Entity &e) {
  if (target.isEmpty() || !hasIO(e))
    return false;
  return matchName(target, e.name) || matchName(target, e.classname);
}

Resolution resolve(const std::vector<Entity> &ents, const QString &rawTarget, const quint32 self) {
  Resolution r;
  const QString target = rawTarget.trimmed();
  if (target.isEmpty())
    return r;
  if (target.compare(QLatin1String("!self"), Qt::CaseInsensitive) == 0) {
    for (const auto &e: ents)
      if (e.id == self && hasIO(e))
        r.ids.push_back(e.id);
    r.kind = r.ids.empty() ? TargetKind::Unresolved : TargetKind::Resolved;
    return r;
  }
  if (isRuntimeTarget(target)) {
    r.kind = TargetKind::Runtime;
    return r;
  }
  if (isExternalTarget(target)) {
    r.kind = TargetKind::External;
    return r;
  }
  for (const auto &e: ents)
    if (addresses(target, e))
      r.ids.push_back(e.id);
  r.kind = r.ids.empty() ? TargetKind::Unresolved : TargetKind::Resolved;
  return r;
}

namespace {

const Entity *byId(const std::vector<Entity> &ents, const quint32 id) {
  for (const auto &e: ents)
    if (e.id == id)
      return &e;
  return nullptr;
}

} // namespace

std::vector<IODef> inputsForTarget(const std::vector<Entity> &ents, const QString &target, const quint32 self) {
  std::vector<IODef> out;
  QSet<QString> seen;
  auto addClass = [&](const EntityClass *c) {
    if (!c || !c->io)
      return;
    for (const auto &d: c->inputs)
      if (!seen.contains(d.name.toLower())) {
        seen.insert(d.name.toLower());
        out.push_back(d);
      }
  };
  const Resolution r = resolve(ents, target, self);
  if (r.kind == TargetKind::Resolved) {
    QSet<QString> classes;
    for (const quint32 id: r.ids)
      if (const Entity *e = byId(ents, id); e && !classes.contains(e->classname)) {
        classes.insert(e->classname);
        addClass(e->cls());
      }
  } else if (r.kind == TargetKind::Runtime || r.kind == TargetKind::External) {
    for (const auto &c: EntityRegistry::instance().classes())
      addClass(&c);
  }
  return out;
}

std::vector<IncomingRef> incoming(const std::vector<Entity> &ents, const quint32 id) {
  std::vector<IncomingRef> out;
  const Entity *me = byId(ents, id);
  if (!me || !hasIO(*me))
    return out;
  for (const auto &src: ents)
    for (int i = 0; i < src.outputs.size(); ++i) {
      const QString &t = src.outputs[i].target;
      const bool hit = t.compare(QLatin1String("!self"), Qt::CaseInsensitive) == 0 ? src.id == id : addresses(t, *me);
      if (hit)
        out.push_back({src.id, i});
    }
  return out;
}

int countNameReferences(const std::vector<Entity> &ents, const QString &name) {
  if (name.isEmpty())
    return 0;
  int n = 0;
  for (const auto &e: ents) {
    for (const auto &c: e.outputs)
      if (c.target.compare(name, Qt::CaseInsensitive) == 0)
        ++n;
    if (const EntityClass *cls = e.cls())
      for (const auto &p: cls->props)
        if (p.type == PropType::Target && e.value(p.key).toString().compare(name, Qt::CaseInsensitive) == 0)
          ++n;
  }
  return n;
}

bool renameReferences(Entity &e, const QString &from, const QString &to) {
  if (from.isEmpty())
    return false;
  bool changed = false;
  for (auto &c: e.outputs)
    if (c.target.compare(from, Qt::CaseInsensitive) == 0) {
      c.target = to;
      changed = true;
    }
  if (const EntityClass *cls = e.cls())
    for (const auto &p: cls->props)
      if (p.type == PropType::Target && e.props.contains(p.key) &&
          e.props.value(p.key).toString().compare(from, Qt::CaseInsensitive) == 0) {
        e.props[p.key] = to;
        changed = true;
      }
  return changed;
}

bool parseAddOutput(const QString &s, Connection *out) {
  const QString t = s.trimmed();
  const int sp = t.indexOf(' ');
  if (sp <= 0)
    return false;
  Connection c;
  c.output = t.left(sp).trimmed();
  const QStringList f = t.mid(sp + 1).trimmed().split(':');
  if (f.isEmpty() || f[0].trimmed().isEmpty())
    return false;
  c.target = f[0].trimmed();
  c.input = f.value(1).trimmed();
  if (c.input.isEmpty())
    c.input = QStringLiteral("Use"); // GoldSrc-style default, as in Source
  c.param = f.value(2);
  bool ok = true;
  if (f.size() > 3 && !f[3].trimmed().isEmpty())
    c.delay = f[3].trimmed().toFloat(&ok);
  if (!ok)
    return false;
  if (f.size() > 4 && !f[4].trimmed().isEmpty()) {
    c.times = f[4].trimmed().toInt(&ok);
    if (!ok)
      return false;
    if (c.times == 0)
      c.times = -1; // Source: 0 or -1 = infinite
  }
  if (out)
    *out = c;
  return true;
}

QString formatAddOutput(const Connection &c) {
  return QStringLiteral("%1 %2:%3:%4:%5:%6")
      .arg(c.output, c.target, c.input, c.param, QString::number(double(c.delay)), QString::number(c.times));
}

namespace {

using Sev = Issue::Severity;

void checkConnection(const std::vector<Entity> &ents, const Entity &owner, const int index, std::vector<Issue> &out) {
  const Connection &c = owner.outputs[index];
  auto add = [&](const Sev s, const QString &text) { out.push_back({s, owner.id, index, text}); };
  const EntityClass *cls = owner.cls();
  if (cls && !cls->io) {
    add(Sev::Error, QStringLiteral("%1 has no I/O; its outputs never fire").arg(cls->label));
    return;
  }
  if (c.output.trimmed().isEmpty())
    add(Sev::Error, QStringLiteral("Output is empty"));
  else if (cls && !cls->output(c.output))
    add(Sev::Error, QStringLiteral("%1 has no output \"%2\"").arg(cls->label, c.output));
  if (c.target.trimmed().isEmpty()) {
    add(Sev::Error, QStringLiteral("Target is empty"));
    return;
  }
  if (c.input.trimmed().isEmpty())
    add(Sev::Error, QStringLiteral("Input is empty"));
  if (c.delay < 0.0f)
    add(Sev::Warning, QStringLiteral("Negative delay (%1 s)").arg(double(c.delay)));
  if (c.times == 0)
    add(Sev::Warning, QStringLiteral("Times is 0: the connection never fires"));

  const Resolution r = resolve(ents, c.target, owner.id);
  if (r.kind == TargetKind::Unresolved) {
    add(Sev::Warning, QStringLiteral("Target \"%1\" matches no entity").arg(c.target));
    return;
  }
  if (r.kind != TargetKind::Resolved || c.input.trimmed().isEmpty())
    return; // runtime / external: the input cannot be checked here

  int accepting = 0;
  const IODef *def = nullptr;
  for (const quint32 id: r.ids)
    if (const Entity *t = byId(ents, id))
      if (const EntityClass *tc = t->cls())
        if (const IODef *d = tc->input(c.input)) {
          ++accepting;
          def = d;
        }
  if (accepting == 0) {
    add(Sev::Error, QStringLiteral("No entity named \"%1\" has an input \"%2\"").arg(c.target, c.input));
    return;
  }
  if (accepting < int(r.ids.size()))
    add(Sev::Warning, QStringLiteral("Only %1 of %2 targets accept \"%3\"").arg(accepting).arg(r.ids.size()).arg(c.input));
  if (def) {
    if (def->param == ParamType::Void && !c.param.trimmed().isEmpty())
      add(Sev::Info, QStringLiteral("%1 takes no parameter; \"%2\" is ignored").arg(def->name, c.param));
    else if (!parseParam(def->param, c.param))
      add(Sev::Warning,
          QStringLiteral("Parameter \"%1\" is not a valid %2").arg(c.param, paramTypeName(def->param)));
  }
}

// Zero-delay loops. Nodes are (entity, output); a zero-delay connection links (owner, output) to (target, o)
// for every output o the target's input fires at once (IODef::fires). A connection inside one strongly connected
// component can feed itself in the same instant.
void checkLoops(const std::vector<Entity> &ents, std::vector<Issue> &out) {
  QHash<QPair<quint32, QString>, int> nodeIndex;
  auto node = [&](const quint32 id, const QString &output) {
    const auto key = qMakePair(id, output.toLower());
    auto it = nodeIndex.constFind(key);
    if (it != nodeIndex.constEnd())
      return *it;
    const int n = int(nodeIndex.size());
    nodeIndex.insert(key, n);
    return n;
  };
  struct Edge {
    int from, to;
    quint32 owner;
    int conn;
  };
  std::vector<Edge> edges;
  for (const auto &e: ents) {
    if (!hasIO(e))
      continue;
    for (int i = 0; i < e.outputs.size(); ++i) {
      const Connection &c = e.outputs[i];
      if (c.disabled || c.delay > 0.0f || c.times == 0)
        continue;
      const Resolution r = resolve(ents, c.target, e.id);
      if (r.kind != TargetKind::Resolved)
        continue;
      const int from = node(e.id, c.output);
      for (const quint32 tid: r.ids) {
        const Entity *t = byId(ents, tid);
        const EntityClass *tc = t ? t->cls() : nullptr;
        const IODef *d = tc ? tc->input(c.input) : nullptr;
        if (!d)
          continue;
        for (const auto &f: d->fires)
          edges.push_back({from, node(tid, f), e.id, i});
      }
    }
  }
  if (edges.empty())
    return;
  const int n = int(nodeIndex.size());
  std::vector<std::vector<int>> adj(n);
  for (const auto &ed: edges)
    adj[ed.from].push_back(ed.to);

  // Tarjan's SCC, iterative so a long chain cannot overflow the stack.
  std::vector<int> index(n, -1), low(n, 0), comp(n, -1);
  std::vector<bool> onStack(n, false);
  std::vector<int> stack;
  int counter = 0, comps = 0;
  for (int s = 0; s < n; ++s) {
    if (index[s] != -1)
      continue;
    std::vector<std::pair<int, size_t>> work {{s, 0}};
    while (!work.empty()) {
      auto &[v, it] = work.back();
      if (it == 0 && index[v] == -1) {
        index[v] = low[v] = counter++;
        stack.push_back(v);
        onStack[v] = true;
      }
      if (it < adj[v].size()) {
        const int w = adj[v][it++];
        if (index[w] == -1)
          work.push_back({w, 0});
        else if (onStack[w])
          low[v] = std::min(low[v], index[w]);
        continue;
      }
      if (low[v] == index[v]) {
        int w;
        do {
          w = stack.back();
          stack.pop_back();
          onStack[w] = false;
          comp[w] = comps;
        } while (w != v);
        ++comps;
      }
      const int done = v;
      work.pop_back();
      if (!work.empty())
        low[work.back().first] = std::min(low[work.back().first], low[done]);
    }
  }
  std::vector<int> compSize(comps, 0);
  for (int v = 0; v < n; ++v)
    ++compSize[comp[v]];
  QSet<QPair<quint32, int>> reported;
  for (const auto &ed: edges) {
    const bool loop = comp[ed.from] == comp[ed.to] && (compSize[comp[ed.from]] > 1 || ed.from == ed.to);
    if (!loop || reported.contains(qMakePair(ed.owner, ed.conn)))
      continue;
    reported.insert(qMakePair(ed.owner, ed.conn));
    out.push_back({Sev::Warning, ed.owner, ed.conn,
                   QStringLiteral("Part of a zero-delay loop: this can fire forever in one frame")});
  }
}

void checkTargetProps(const std::vector<Entity> &ents, const Entity &e, std::vector<Issue> &out) {
  const EntityClass *cls = e.cls();
  if (!cls)
    return;
  for (const auto &p: cls->props) {
    if (p.type != PropType::Target)
      continue;
    const QString v = e.value(p.key).toString().trimmed();
    if (v.isEmpty())
      continue;
    if (resolve(ents, v, e.id).kind == TargetKind::Unresolved)
      out.push_back({Sev::Warning, e.id, -1, QStringLiteral("%1 \"%2\" matches no entity").arg(p.label, v)});
  }
}

} // namespace

std::vector<Issue> validateConnection(const std::vector<Entity> &ents, const Entity &owner, const int index) {
  std::vector<Issue> out;
  if (index < 0 || index >= owner.outputs.size() || owner.outputs[index].disabled)
    return out;
  checkConnection(ents, owner, index, out);
  return out;
}

std::vector<Issue> validate(const std::vector<Entity> &ents) {
  std::vector<Issue> out;
  for (const auto &e: ents) {
    for (int i = 0; i < e.outputs.size(); ++i)
      if (!e.outputs[i].disabled)
        checkConnection(ents, e, i, out);
    checkTargetProps(ents, e, out);
  }
  checkLoops(ents, out);
  return out;
}

} // namespace eng::io
