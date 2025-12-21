#include "io_sim.h"

#include <QColor>

#include <algorithm>
#include <cmath>

#include "io.h"

namespace eng::io {

namespace {

const QString kTick = QStringLiteral("__tick");
const QString kArrive = QStringLiteral("__arrive");

bool is(const QString &a, const char *b) { return a.compare(QLatin1String(b), Qt::CaseInsensitive) == 0; }

QString num(const double v) { return QString::number(v, 'g', 6); }

double toNum(const QString &s, const double fallback = 0.0) {
  bool ok = false;
  const double v = s.trimmed().toDouble(&ok);
  return ok ? v : fallback;
}

bool toBool(const QString &s) {
  QVariant v;
  return parseParam(ParamType::Bool, s, &v) && v.toBool();
}

} // namespace

Simulator::Simulator(std::vector<Entity> entities, const quint32 seed) : m_ents(std::move(entities)), m_rng(seed) {
  for (int i = 0; i < int(m_ents.size()); ++i)
    m_index.insert(m_ents[size_t(i)].id, i);
}

Entity *Simulator::ent(const quint32 id) {
  const auto it = m_index.constFind(id);
  return it == m_index.constEnd() ? nullptr : &m_ents[size_t(*it)];
}

const Entity *Simulator::ent(const quint32 id) const {
  const auto it = m_index.constFind(id);
  return it == m_index.constEnd() ? nullptr : &m_ents[size_t(*it)];
}

bool Simulator::killed(const quint32 id) const {
  const auto it = m_state.find(id);
  return it != m_state.end() && it->second.killed;
}

QString Simulator::nameOf(const quint32 id) const {
  if (id == 0)
    return QStringLiteral("console");
  const Entity *e = ent(id);
  if (!e)
    return QStringLiteral("#%1").arg(id);
  return e->name.isEmpty() ? QStringLiteral("%1#%2").arg(e->classname).arg(id & 0xffff) : e->name;
}

void Simulator::note(const QString &text, const quint32 entity) {
  SimLog l;
  l.kind = SimLog::Kind::Note;
  l.time = m_now;
  l.from = entity;
  l.text = text;
  m_log.push_back(l);
}

std::vector<SimLog> Simulator::takeLog() {
  std::vector<SimLog> out;
  out.swap(m_log);
  return out;
}

void Simulator::schedule(const double at, Event e) {
  m_queue.emplace(std::make_pair(std::max(at, m_now), m_seq++), std::move(e));
}

double Simulator::randomInterval(const State &s) {
  double lo = std::min(s.lower, s.upper), hi = std::max(s.lower, s.upper);
  if (hi <= lo)
    return std::max(0.01, lo);
  std::uniform_real_distribution<double> d(lo, hi);
  return std::max(0.01, d(m_rng));
}

void Simulator::armTimer(const quint32 id) {
  State &s = st(id);
  ++s.gen;
  s.nextTick = -1.0;
  if (!s.enabled || s.killed)
    return;
  const double dt = s.useRandom ? randomInterval(s) : s.interval;
  if (dt <= 0.0)
    return; // a zero interval would fire forever in one instant
  s.nextTick = m_now + std::max(0.01, dt);
  Event e;
  e.target = id;
  e.input = kTick;
  e.gen = s.gen;
  e.caller = id;
  schedule(s.nextTick, e);
}

void Simulator::start() {
  for (const auto &e: m_ents) {
    State &s = st(e.id);
    s.fired = QVector<int>(e.outputs.size(), 0);
    s.enabled = !e.value(QStringLiteral("startDisabled")).toBool();
    const QString c = e.classname;
    if (c == QLatin1String("logic_timer")) {
      s.interval = e.num(QStringLiteral("refireTime"), 1.0);
      s.useRandom = e.value(QStringLiteral("useRandomTime")).toBool();
      s.lower = e.num(QStringLiteral("lowerRandomBound"), 1.0);
      s.upper = e.num(QStringLiteral("upperRandomBound"), 5.0);
    } else if (c == QLatin1String("logic_branch")) {
      s.on = e.value(QStringLiteral("initialValue")).toBool();
    } else if (c == QLatin1String("logic_compare")) {
      s.value = e.num(QStringLiteral("initialValue"));
      s.compare = e.num(QStringLiteral("compareValue"));
    } else if (c == QLatin1String("logic_multicompare")) {
      s.compare = e.num(QStringLiteral("referenceValue"));
    } else if (c == QLatin1String("math_counter")) {
      s.value = e.num(QStringLiteral("startValue"));
      s.min = e.num(QStringLiteral("min"));
      s.max = e.num(QStringLiteral("max"));
    } else if (c == QLatin1String("light") || c == QLatin1String("light_spot")) {
      s.on = e.value(QStringLiteral("enabled")).toBool();
    } else if (c == QLatin1String("ambient_sound")) {
      s.on = !e.value(QStringLiteral("startSilent")).toBool();
    } else if (c == QLatin1String("func_door")) {
      s.locked = e.value(QStringLiteral("locked")).toBool();
      s.speed = e.num(QStringLiteral("speed"), 100.0);
    }
  }
  note(QStringLiteral("simulation started (%1 entities)").arg(m_ents.size()));
  for (const auto &e: m_ents) {
    if (e.classname == QLatin1String("logic_timer"))
      armTimer(e.id);
    else if (e.classname == QLatin1String("logic_relay"))
      fire(e.id, QStringLiteral("OnSpawn"), {}, 0);
  }
  for (const auto &e: m_ents)
    if (e.classname == QLatin1String("logic_auto")) {
      const QString gs = e.value(QStringLiteral("globalState")).toString();
      if (!gs.isEmpty()) {
        note(QStringLiteral("%1: global state \"%2\" is off in the editor; not firing").arg(nameOf(e.id), gs), e.id);
        continue;
      }
      fire(e.id, QStringLiteral("OnMapSpawn"), {}, 0);
      fire(e.id, QStringLiteral("OnNewGame"), {}, 0);
      if (e.value(QStringLiteral("removeOnFire")).toBool())
        st(e.id).killed = true;
    }
}

void Simulator::advance(const double dt, const int budget) {
  const double end = m_now + std::max(0.0, dt);
  int n = 0;
  while (!m_queue.empty() && m_queue.begin()->first.first <= end) {
    if (++n > budget) {
      note(QStringLiteral("more than %1 events in one step: stopped (zero-delay loop?)").arg(budget));
      return;
    }
    auto it = m_queue.begin();
    m_now = it->first.first;
    const Event e = it->second;
    m_queue.erase(it);
    deliver(e);
  }
  m_now = end;
}

int Simulator::step(const int count) {
  int n = 0;
  while (n < count && !m_queue.empty()) {
    auto it = m_queue.begin();
    m_now = std::max(m_now, it->first.first);
    const Event e = it->second;
    m_queue.erase(it);
    deliver(e);
    if (!e.input.startsWith(QLatin1String("__")))
      ++n;
  }
  return n;
}

QStringList Simulator::dumpQueue() const {
  QStringList out;
  for (const auto &[key, e]: m_queue) {
    if (e.input.startsWith(QLatin1String("__")))
      continue;
    out << QStringLiteral("t=%1  %2 → %3.%4(%5)")
               .arg(key.first, 0, 'f', 2)
               .arg(nameOf(e.caller), nameOf(e.target), e.input, e.param);
  }
  return out;
}

void Simulator::fire(const quint32 from, const QString &output, const QString &value, const quint32 activator) {
  Entity *e = ent(from);
  if (!e || killed(from))
    return;
  State &s = st(from);
  if (s.fired.size() != e->outputs.size())
    s.fired.resize(e->outputs.size());
  for (int i = 0; i < e->outputs.size(); ++i) {
    const Connection c = e->outputs[i]; // copy: AddOutput may grow the list
    if (c.disabled || c.output.compare(output, Qt::CaseInsensitive) != 0)
      continue;
    if (c.times >= 0 && st(from).fired[i] >= c.times)
      continue;
    ++st(from).fired[i];
    const QString param = c.param.isEmpty() ? value : c.param;
    std::vector<quint32> targets;
    if (is(c.target, "!self") || is(c.target, "!caller"))
      targets.push_back(from);
    else if (is(c.target, "!activator")) {
      if (activator)
        targets.push_back(activator);
      else
        note(QStringLiteral("%1.%2: !activator is nobody here").arg(nameOf(from), output), from);
    } else if (isRuntimeTarget(c.target) || isExternalTarget(c.target)) {
      note(QStringLiteral("%1.%2 → %3.%4(%5): outside the map, Godot handles it")
               .arg(nameOf(from), output, c.target, c.input, param),
           from);
    } else {
      targets = resolve(m_ents, c.target, from).ids;
      if (targets.empty())
        note(QStringLiteral("%1.%2: target \"%3\" matches nothing").arg(nameOf(from), output, c.target), from);
    }
    for (const quint32 t: targets) {
      Event ev;
      ev.target = t;
      ev.input = c.input;
      ev.param = param;
      ev.activator = activator;
      ev.caller = from;
      SimLog l;
      l.kind = SimLog::Kind::Fired;
      l.time = m_now;
      l.deliverAt = m_now + std::max(0.0f, c.delay);
      l.from = from;
      l.to = t;
      l.output = output;
      l.input = c.input;
      l.param = param;
      l.text = QStringLiteral("%1.%2 → %3.%4(%5)%6")
                   .arg(nameOf(from), output, nameOf(t), c.input, param,
                        c.delay > 0 ? QStringLiteral(" in %1 s").arg(double(c.delay)) : QString());
      m_log.push_back(l);
      schedule(m_now + std::max(0.0f, c.delay), ev);
    }
  }
}

int Simulator::fireInput(const QString &target, const QString &input, const QString &param, const double delay,
                         const quint32 activator) {
  Resolution r;
  if (target.startsWith('#')) { // one entity by id (the Fire Input dialog)
    bool ok = false;
    const quint32 id = target.mid(1).toUInt(&ok);
    if (ok && ent(id))
      r.ids.push_back(id);
  } else {
    r = resolve(m_ents, target, 0);
  }
  if (r.ids.empty()) {
    note(QStringLiteral("ent_fire: \"%1\" matches no entity").arg(target));
    return 0;
  }
  for (const quint32 t: r.ids) {
    Event e;
    e.target = t;
    e.input = input;
    e.param = param;
    e.activator = activator;
    SimLog l;
    l.kind = SimLog::Kind::Fired;
    l.time = m_now;
    l.deliverAt = m_now + std::max(0.0, delay);
    l.to = t;
    l.input = input;
    l.param = param;
    l.text = QStringLiteral("console → %1.%2(%3)").arg(nameOf(t), input, param);
    m_log.push_back(l);
    schedule(m_now + std::max(0.0, delay), e);
  }
  return int(r.ids.size());
}

bool Simulator::fireOutput(const quint32 entity, const QString &output, const QString &param, const quint32 activator) {
  const Entity *e = ent(entity);
  if (!e || killed(entity))
    return false;
  const EntityClass *cls = e->cls();
  if (cls && !cls->output(output))
    note(QStringLiteral("%1 has no output \"%2\" (firing anyway)").arg(nameOf(entity), output), entity);
  if (e->classname.startsWith(QLatin1String("trigger_")) && !st(entity).enabled) {
    note(QStringLiteral("%1 is disabled").arg(nameOf(entity)), entity);
    return false;
  }
  fire(entity, output, param, activator);
  if (e->classname == QLatin1String("trigger_once") && (is(output, "OnTrigger") || is(output, "OnStartTouch"))) {
    if (is(output, "OnStartTouch"))
      fire(entity, QStringLiteral("OnTrigger"), param, activator);
    st(entity).killed = true;
    note(QStringLiteral("%1 removed itself").arg(nameOf(entity)), entity);
  }
  return true;
}

int Simulator::raise(const QString &eventName, const QString &param) {
  int n = 0;
  for (const auto &e: m_ents)
    if (e.classname == QLatin1String("logic_event")
        && e.value(QStringLiteral("eventName")).toString().compare(eventName, Qt::CaseInsensitive) == 0
        && st(e.id).enabled && !killed(e.id)) {
      fire(e.id, QStringLiteral("OnRaised"), param, 0);
      ++n;
    }
  if (!n)
    note(QStringLiteral("IO.raise(\"%1\"): no enabled logic_event has that name").arg(eventName));
  return n;
}

void Simulator::counterSet(const quint32 id, double v, const bool fireOutputs, const quint32 activator) {
  State &s = st(id);
  const bool limited = s.max > s.min;
  const double old = s.value;
  if (limited)
    v = std::clamp(v, s.min, s.max);
  s.value = v;
  if (!fireOutputs || v == old)
    return;
  fire(id, QStringLiteral("OutValue"), num(v), activator);
  if (!limited)
    return;
  if (v == s.max)
    fire(id, QStringLiteral("OnHitMax"), {}, activator);
  if (v == s.min)
    fire(id, QStringLiteral("OnHitMin"), {}, activator);
  if (old == s.max && v != s.max)
    fire(id, QStringLiteral("OnChangedFromMax"), {}, activator);
  if (old == s.min && v != s.min)
    fire(id, QStringLiteral("OnChangedFromMin"), {}, activator);
}

void Simulator::doorMove(const quint32 id, const bool open, const quint32 activator) {
  State &s = st(id);
  if (s.locked) {
    fire(id, QStringLiteral("OnLockedUse"), {}, activator);
    return;
  }
  if (s.on == open && !s.moving)
    return;
  s.on = open;
  s.moving = true;
  ++s.gen;
  fire(id, open ? QStringLiteral("OnOpen") : QStringLiteral("OnClose"), {}, activator);
  // Travel distance is unknown here; one door width (~64 Source units) at the door's speed orders events well enough.
  Event e;
  e.target = id;
  e.input = kArrive;
  e.gen = s.gen;
  e.caller = id;
  e.activator = activator;
  schedule(m_now + std::clamp(64.0 / std::max(1.0, s.speed), 0.05, 10.0), e);
}

void Simulator::deliver(const Event &ev) {
  Entity *e = ent(ev.target);
  if (!e)
    return;
  State &s = st(ev.target);
  const quint32 id = ev.target;
  const QString &in = ev.input;
  const QString c = e->classname;
  const quint32 act = ev.activator;

  if (in == kTick) {
    if (ev.gen != s.gen || s.killed || !s.enabled)
      return;
    if (e->value(QStringLiteral("oscillator")).toBool()) {
      fire(id, s.phase == 0 ? QStringLiteral("OnTimerLow") : QStringLiteral("OnTimerHigh"), {}, act);
      s.phase ^= 1;
    } else {
      fire(id, QStringLiteral("OnTimer"), {}, act);
    }
    armTimer(id);
    return;
  }
  if (in == kArrive) {
    if (ev.gen != s.gen)
      return;
    s.moving = false;
    fire(id, s.on ? QStringLiteral("OnFullyOpen") : QStringLiteral("OnFullyClosed"), {}, act);
    const double wait = e->num(QStringLiteral("wait"), 4.0);
    if (s.on && wait >= 0.0) {
      Event close;
      close.target = id;
      close.input = QStringLiteral("Close");
      close.caller = id;
      schedule(m_now + wait, close);
    }
    return;
  }

  if (s.killed)
    return;
  SimLog l;
  l.kind = SimLog::Kind::Delivered;
  l.time = m_now;
  l.from = ev.caller;
  l.to = id;
  l.input = in;
  l.param = ev.param;
  l.text = QStringLiteral("%1.%2(%3)").arg(nameOf(id), in, ev.param);
  m_log.push_back(l);

  if (is(in, "Kill")) {
    fire(id, QStringLiteral("OnKilled"), {}, act);
    s.killed = true;
    return;
  }
  for (int u = 1; u <= 4; ++u)
    if (in.compare(QStringLiteral("FireUser%1").arg(u), Qt::CaseInsensitive) == 0) {
      fire(id, QStringLiteral("OnUser%1").arg(u), ev.param, act);
      return;
    }
  if (is(in, "AddOutput")) {
    Connection add;
    if (parseAddOutput(ev.param, &add)) {
      e->outputs.push_back(add);
      s.fired.push_back(0);
      note(QStringLiteral("%1 AddOutput: %2").arg(nameOf(id), formatAddOutput(add)), id);
    } else {
      note(QStringLiteral("%1 AddOutput: cannot parse \"%2\"").arg(nameOf(id), ev.param), id);
    }
    return;
  }
  const bool enableIO = is(in, "Enable") || is(in, "Disable") || is(in, "Toggle");
  auto unknown = [&] { note(QStringLiteral("%1 (%2) has no input \"%3\"; ignored").arg(nameOf(id), c, in), id); };

  if (c == QLatin1String("logic_relay")) {
    if (enableIO) {
      s.enabled = is(in, "Enable") ? true : is(in, "Disable") ? false : !s.enabled;
      return;
    }
    if (is(in, "CancelPending")) {
      for (auto it = m_queue.begin(); it != m_queue.end();)
        it = it->second.caller == id ? m_queue.erase(it) : std::next(it);
      return;
    }
    if (is(in, "Trigger") || is(in, "TriggerWithParameter")) {
      if (!s.enabled) {
        note(QStringLiteral("%1 is disabled; %2 ignored").arg(nameOf(id), in), id);
        return;
      }
      if (!e->value(QStringLiteral("fastRetrigger")).toBool()) {
        bool pending = false;
        for (const auto &[k, q]: m_queue)
          pending = pending || (q.caller == id && !q.input.startsWith(QLatin1String("__")));
        if (pending) {
          note(QStringLiteral("%1 is still waiting for its delayed outputs; %2 ignored").arg(nameOf(id), in), id);
          return;
        }
      }
      if (is(in, "Trigger"))
        fire(id, QStringLiteral("OnTrigger"), {}, act);
      else
        fire(id, QStringLiteral("OnTriggerParameter"), ev.param, act);
      if (e->value(QStringLiteral("onlyOnce")).toBool())
        s.killed = true;
      return;
    }
  } else if (c == QLatin1String("logic_timer")) {
    if (enableIO) {
      s.enabled = is(in, "Enable") ? true : is(in, "Disable") ? false : !s.enabled;
      armTimer(id);
      return;
    }
    if (is(in, "RefireTime")) {
      s.interval = toNum(ev.param, s.interval);
      armTimer(id);
    } else if (is(in, "ResetTimer")) {
      armTimer(id);
    } else if (is(in, "FireTimer")) {
      Event t;
      t.target = id;
      t.input = kTick;
      t.gen = s.gen;
      t.caller = id;
      // Fire now, then restart the countdown (as the tick does).
      const bool en = s.enabled;
      s.enabled = true;
      deliver(t);
      s.enabled = en;
      if (!en)
        armTimer(id);
    } else if (is(in, "LowerRandomBound")) {
      s.lower = toNum(ev.param, s.lower);
    } else if (is(in, "UpperRandomBound")) {
      s.upper = toNum(ev.param, s.upper);
    } else if (is(in, "UseRandomTime")) {
      s.useRandom = toBool(ev.param);
      if (!s.useRandom)
        s.interval = s.upper;
    } else if (is(in, "AddToTimer") || is(in, "SubtractFromTimer")) {
      if (s.nextTick < 0.0)
        return;
      const double d = toNum(ev.param) * (is(in, "AddToTimer") ? 1.0 : -1.0);
      ++s.gen;
      s.nextTick = std::max(m_now, s.nextTick + d);
      Event t;
      t.target = id;
      t.input = kTick;
      t.gen = s.gen;
      t.caller = id;
      schedule(s.nextTick, t);
    } else {
      unknown();
    }
    return;
  } else if (c == QLatin1String("logic_branch")) {
    auto test = [&] { fire(id, s.on ? QStringLiteral("OnTrue") : QStringLiteral("OnFalse"), {}, act); };
    if (is(in, "SetValue"))
      s.on = toBool(ev.param);
    else if (is(in, "SetValueTest")) {
      s.on = toBool(ev.param);
      test();
    } else if (is(in, "Toggle"))
      s.on = !s.on;
    else if (is(in, "ToggleTest")) {
      s.on = !s.on;
      test();
    } else if (is(in, "Test"))
      test();
    else
      return unknown();
    return;
  } else if (c == QLatin1String("logic_case")) {
    auto caseOutput = [](int i) { return QStringLiteral("OnCase%1").arg(i, 2, 10, QChar('0')); };
    if (is(in, "InValue")) {
      const bool multi = e->value(QStringLiteral("multipleCasesAllowed")).toBool();
      bool hit = false;
      for (int i = 1; i <= 16; ++i) {
        const QString v = e->value(QStringLiteral("case%1").arg(i, 2, 10, QChar('0'))).toString();
        if (!v.isEmpty() && v == ev.param) {
          fire(id, caseOutput(i), {}, act);
          hit = true;
          if (!multi)
            break;
        }
      }
      if (!hit)
        fire(id, QStringLiteral("OnDefault"), ev.param, act);
      fire(id, QStringLiteral("OnUsed"), ev.param, act);
      return;
    }
    if (is(in, "PickRandom") || is(in, "PickRandomShuffle")) {
      QVector<int> wired;
      for (int i = 1; i <= 16; ++i)
        for (const auto &cn: e->outputs)
          if (!cn.disabled && cn.output.compare(caseOutput(i), Qt::CaseInsensitive) == 0) {
            wired.push_back(i);
            break;
          }
      if (wired.isEmpty()) {
        note(QStringLiteral("%1: no OnCase output is connected").arg(nameOf(id)), id);
        return;
      }
      int pick;
      if (is(in, "PickRandomShuffle")) {
        s.shuffle.removeIf([&](int i) { return !wired.contains(i); });
        if (s.shuffle.isEmpty()) {
          s.shuffle = wired;
          std::shuffle(s.shuffle.begin(), s.shuffle.end(), m_rng);
        }
        pick = s.shuffle.takeLast();
      } else {
        std::uniform_int_distribution<int> d(0, int(wired.size()) - 1);
        pick = wired[d(m_rng)];
      }
      fire(id, caseOutput(pick), {}, act);
      return;
    }
    return unknown();
  } else if (c == QLatin1String("logic_compare")) {
    auto compare = [&] {
      const QString v = num(s.value);
      if (s.value < s.compare)
        fire(id, QStringLiteral("OnLessThan"), v, act);
      else if (s.value > s.compare)
        fire(id, QStringLiteral("OnGreaterThan"), v, act);
      if (s.value == s.compare)
        fire(id, QStringLiteral("OnEqualTo"), v, act);
      else
        fire(id, QStringLiteral("OnNotEqualTo"), v, act);
    };
    if (is(in, "SetValue"))
      s.value = toNum(ev.param, s.value);
    else if (is(in, "SetValueCompare")) {
      s.value = toNum(ev.param, s.value);
      compare();
    } else if (is(in, "SetCompareValue"))
      s.compare = toNum(ev.param, s.compare);
    else if (is(in, "Compare"))
      compare();
    else
      return unknown();
    return;
  } else if (c == QLatin1String("logic_multicompare")) {
    auto compare = [&] {
      bool equal = !s.values.isEmpty();
      const bool toRef = e->value(QStringLiteral("shouldCompareToValue")).toBool();
      for (const int v: s.values)
        equal = equal && v == (toRef ? int(s.compare) : s.values.front());
      fire(id, equal ? QStringLiteral("OnEqual") : QStringLiteral("OnNotEqual"), {}, act);
    };
    if (is(in, "Value"))
      s.values.push_back(int(std::lround(toNum(ev.param))));
    else if (is(in, "ValueCompare")) {
      s.values.push_back(int(std::lround(toNum(ev.param))));
      compare();
    } else if (is(in, "CompareValues"))
      compare();
    else
      return unknown();
    return;
  } else if (c == QLatin1String("math_counter")) {
    if (is(in, "Enable") || is(in, "Disable")) {
      s.enabled = is(in, "Enable");
      return;
    }
    if (is(in, "SetHitMax")) {
      s.max = toNum(ev.param, s.max);
      return;
    }
    if (is(in, "SetHitMin")) {
      s.min = toNum(ev.param, s.min);
      return;
    }
    if (is(in, "GetValue")) {
      fire(id, QStringLiteral("OnGetValue"), num(s.value), act);
      return;
    }
    if (!s.enabled) {
      note(QStringLiteral("%1 is disabled; %2 ignored").arg(nameOf(id), in), id);
      return;
    }
    const double p = toNum(ev.param);
    if (is(in, "Add"))
      counterSet(id, s.value + p, true, act);
    else if (is(in, "Subtract"))
      counterSet(id, s.value - p, true, act);
    else if (is(in, "Multiply"))
      counterSet(id, s.value * p, true, act);
    else if (is(in, "Divide")) {
      if (p == 0.0)
        note(QStringLiteral("%1: Divide by 0 ignored").arg(nameOf(id)), id);
      else
        counterSet(id, s.value / p, true, act);
    } else if (is(in, "SetValue"))
      counterSet(id, p, true, act);
    else if (is(in, "SetValueNoFire"))
      counterSet(id, p, false, act);
    else
      return unknown();
    return;
  } else if (c == QLatin1String("logic_event")) {
    if (enableIO) {
      s.enabled = is(in, "Enable") ? true : is(in, "Disable") ? false : !s.enabled;
      return;
    }
    if (is(in, "Emit")) {
      if (s.enabled)
        note(QStringLiteral("→ Godot: IO.event(\"%1\", %2)")
                 .arg(e->value(QStringLiteral("eventName")).toString(), ev.param.isEmpty() ? QStringLiteral("null")
                                                                                           : QStringLiteral("\"%1\"").arg(ev.param)),
             id);
      return;
    }
    return unknown();
  } else if (c == QLatin1String("light") || c == QLatin1String("light_spot")) {
    if (is(in, "TurnOn") || is(in, "TurnOff") || is(in, "Toggle")) {
      const bool on = is(in, "TurnOn") ? true : is(in, "TurnOff") ? false : !s.on;
      if (on != s.on) {
        s.on = on;
        fire(id, on ? QStringLiteral("OnTurnedOn") : QStringLiteral("OnTurnedOff"), {}, act);
      }
      return;
    }
    if (is(in, "SetColor") || is(in, "SetEnergy"))
      return;
    return unknown();
  } else if (c == QLatin1String("func_door")) {
    if (is(in, "Open"))
      doorMove(id, true, act);
    else if (is(in, "Close"))
      doorMove(id, false, act);
    else if (is(in, "Toggle"))
      doorMove(id, !s.on, act);
    else if (is(in, "Lock"))
      s.locked = true;
    else if (is(in, "Unlock"))
      s.locked = false;
    else if (is(in, "SetSpeed"))
      s.speed = toNum(ev.param, s.speed);
    else
      return unknown();
    return;
  } else if (c == QLatin1String("ambient_sound")) {
    if (is(in, "PlaySound"))
      s.on = true;
    else if (is(in, "StopSound"))
      s.on = false;
    else if (is(in, "ToggleSound"))
      s.on = !s.on;
    else if (!is(in, "Volume"))
      return unknown();
    return;
  } else if (c.startsWith(QLatin1String("trigger_"))) {
    if (enableIO) {
      s.enabled = is(in, "Enable") ? true : is(in, "Disable") ? false : !s.enabled;
      return;
    }
    if (is(in, "TouchTest")) {
      fire(id, QStringLiteral("OnNotTouching"), {}, act); // nothing touches in the editor
      return;
    }
    return unknown();
  } else if (c == QLatin1String("func_wall") || c == QLatin1String("func_illusionary")) {
    if (enableIO) {
      s.enabled = is(in, "Enable") ? true : is(in, "Disable") ? false : !s.enabled;
      return;
    }
    return unknown();
  }

  // Classes without behaviour here (custom JSON, playerproxy...): flag inputs the class does not even declare.
  if (const EntityClass *cls = e->cls(); !cls || !cls->input(in))
    note(QStringLiteral("%1 (%2) has no input \"%3\"; ignored").arg(nameOf(id), c, in), id);
}

double Simulator::numericState(const quint32 id) const {
  const auto it = m_state.find(id);
  if (it == m_state.end())
    return 0.0;
  const Entity *e = ent(id);
  if (e && e->classname == QLatin1String("logic_branch"))
    return it->second.on ? 1.0 : 0.0;
  return it->second.value;
}

QString Simulator::stateText(const quint32 id) const {
  const auto it = m_state.find(id);
  const Entity *e = ent(id);
  if (it == m_state.end() || !e)
    return {};
  const State &s = it->second;
  if (s.killed)
    return QStringLiteral("killed");
  const QString c = e->classname;
  if (c == QLatin1String("math_counter") || c == QLatin1String("logic_compare"))
    return QStringLiteral("value %1%2").arg(num(s.value), s.enabled ? QString() : QStringLiteral(" (disabled)"));
  if (c == QLatin1String("logic_branch"))
    return s.on ? QStringLiteral("true") : QStringLiteral("false");
  if (c == QLatin1String("logic_timer"))
    return s.enabled && s.nextTick >= 0.0 ? QStringLiteral("next in %1 s").arg(s.nextTick - m_now, 0, 'f', 1)
                                          : QStringLiteral("stopped");
  if (c == QLatin1String("light") || c == QLatin1String("light_spot"))
    return s.on ? QStringLiteral("on") : QStringLiteral("off");
  if (c == QLatin1String("func_door"))
    return QStringLiteral("%1%2%3").arg(s.on ? "open" : "closed", s.moving ? " (moving)" : "", s.locked ? ", locked" : "");
  return s.enabled ? QString() : QStringLiteral("disabled");
}

} // namespace eng::io
