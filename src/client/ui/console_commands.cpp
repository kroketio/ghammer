#include "console_commands.h"

#include <QDebug>

#include "client/engine/entity.h"
#include "client/lib/globals.h"
#include "client/prefs/preferences.h"
#include "client/vulkan/vk_engine.h"

namespace ui {

namespace {

vk::Engine3D *E() { return g::engine3D; }

quint32 entityByName(const QString &name) {
  const auto *ents = E() ? E()->sceneEntities() : nullptr;
  if (!ents)
    return 0;
  if (name.startsWith('#'))
    return name.mid(1).toUInt();
  for (const auto &e: *ents)
    if (e.name.compare(name, Qt::CaseInsensitive) == 0)
      return e.id;
  return 0;
}

} // namespace

ConsoleCommands &ConsoleCommands::instance() {
  static ConsoleCommands c;
  return c;
}

void ConsoleCommands::add(Command c) {
  for (auto &x: m_commands)
    if (x.name == c.name) {
      x = std::move(c);
      return;
    }
  m_commands.push_back(std::move(c));
}

QStringList ConsoleCommands::names() const {
  QStringList out;
  for (const auto &c: m_commands)
    out << c.name;
  return out;
}

QStringList ConsoleCommands::tokenize(const QString &line) {
  QStringList out;
  QString cur;
  bool quoted = false, any = false;
  for (const QChar ch: line) {
    if (ch == '"') {
      quoted = !quoted;
      any = true;
    } else if (ch.isSpace() && !quoted) {
      if (any || !cur.isEmpty())
        out << cur;
      cur.clear();
      any = false;
    } else {
      cur += ch;
    }
  }
  if (any || !cur.isEmpty())
    out << cur;
  return out;
}

bool ConsoleCommands::run(const QString &line) {
  const QStringList tok = tokenize(line.trimmed());
  if (tok.isEmpty())
    return true;
  qInfo().noquote() << QStringLiteral("] %1").arg(line.trimmed());
  for (const auto &c: m_commands)
    if (c.name.compare(tok[0], Qt::CaseInsensitive) == 0) {
      if (!E()) {
        qWarning("no engine");
        return false;
      }
      c.run(tok.mid(1));
      return true;
    }
  qWarning().noquote() << QStringLiteral("unknown command \"%1\" (type help)").arg(tok[0]);
  return false;
}

ConsoleCommands::ConsoleCommands() {
  add({"help", "help", "List the commands", [this](const QStringList &) {
         for (const auto &c: m_commands)
           qInfo().noquote() << QStringLiteral("  %1 — %2").arg(c.usage, -44).arg(c.help);
       }});
  add({"ent_fire", "ent_fire <target> <input> [param] [delay]",
       "Send an input in the I/O simulator (starts it). Target: name, door_*, classname or #id",
       [](const QStringList &a) {
         if (a.size() < 2) {
           qWarning("usage: ent_fire <target> <input> [param] [delay]");
           return;
         }
         E()->ioSimFire(a[0], a[1], a.value(2), a.value(3).toDouble());
       }});
  add({"ent_fire_output", "ent_fire_output <entity> <output> [param]",
       "Fire an entity's output as if it happened (a trigger being touched)", [](const QStringList &a) {
         if (a.size() < 2) {
           qWarning("usage: ent_fire_output <entity> <output> [param]");
           return;
         }
         const quint32 id = entityByName(a[0]);
         if (!id) {
           qWarning().noquote() << QStringLiteral("no entity named \"%1\"").arg(a[0]);
           return;
         }
         E()->ioSimFireOutput(id, a[1], a.value(2));
       }});
  add({"io_raise", "io_raise <event> [param]", "What Godot's IO.raise(event) does: fires matching logic_event OnRaised",
       [](const QStringList &a) {
         if (a.isEmpty()) {
           qWarning("usage: io_raise <event> [param]");
           return;
         }
         E()->ioSimRaise(a[0], a.value(1));
       }});
  add({"io_start", "io_start", "Start the I/O simulation (logic_auto fires)", [](const QStringList &) { E()->ioSimStart(); }});
  add({"io_stop", "io_stop", "Stop the I/O simulation", [](const QStringList &) { E()->ioSimStop(); }});
  add({"io_pause", "io_pause", "Pause / resume the I/O simulation (ent_pause)",
       [](const QStringList &) { E()->setIOSimPaused(!E()->ioSimPaused()); }});
  add({"io_step", "io_step [n]", "Deliver the next n queued events (ent_step)",
       [](const QStringList &a) { E()->ioSimStep(std::max(1, a.value(0, QStringLiteral("1")).toInt())); }});
  add({"io_timescale", "io_timescale <x>", "Simulation speed (1 = real time)", [](const QStringList &a) {
         if (a.isEmpty()) {
           qInfo().noquote() << QStringLiteral("io_timescale = %1").arg(prefs::get().value(QStringLiteral("IO/SimTimeScale")).toDouble());
           return;
         }
         prefs::get().setValue(QStringLiteral("IO/SimTimeScale"), std::clamp(a[0].toDouble(), 0.05, 20.0));
       }});
  add({"dumpeventqueue", "dumpeventqueue", "List the queued I/O events", [](const QStringList &) {
         const QStringList q = E()->ioSimQueue();
         if (!E()->ioSimRunning())
           qInfo("the I/O simulation is not running");
         else if (q.isEmpty())
           qInfo("the event queue is empty");
         for (const auto &l: q)
           qInfo().noquote() << QStringLiteral("  %1").arg(l);
       }});
  add({"ent_state", "ent_state <entity>", "Print an entity's simulated state", [](const QStringList &a) {
         const quint32 id = entityByName(a.value(0));
         if (!id) {
           qWarning("usage: ent_state <entity name or #id>");
           return;
         }
         qInfo().noquote() << QStringLiteral("%1: %2").arg(a[0], E()->ioSimRunning() ? E()->ioSimState(id) : QStringLiteral("(not running)"));
       }});
}

} // namespace ui
