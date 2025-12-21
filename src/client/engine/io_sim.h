#pragma once

// Entity I/O simulator: runs a map's logic without touching the document. The reference behaviour of every
// built-in class: godot/addons/ghammer/entities/*.gd must match it (pinned by brush_tests). Deterministic: time
// only moves on advance / step.

#include <QHash>
#include <QString>
#include <QStringList>

#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include "entity.h"

namespace eng::io {

struct SimLog {
  enum class Kind {
    Fired,    // an output fired and queued an input (deliverAt = when it arrives)
    Delivered, // an input reached its target
    Note,     // anything else for the designer
  };
  Kind kind = Kind::Note;
  double time = 0.0;      // simulation time of the entry
  double deliverAt = 0.0; // Fired: arrival time
  quint32 from = 0;       // entity that fired (0 = the console / the designer)
  quint32 to = 0;         // target entity
  QString output, input, param;
  QString text;           // human-readable line
};

class Simulator {
public:
  explicit Simulator(std::vector<Entity> entities, quint32 seed = 1);

  // Initial state, timers, logic_auto OnMapSpawn, relay OnSpawn. Call once.
  void start();
  [[nodiscard]] double time() const { return m_now; }

  // Delivers everything due within dt; stops early (with a note) past `budget` events: a zero-delay loop.
  void advance(double dt, int budget = 20000);
  // Delivers the next n queued events, jumping time to each. Returns how many.
  int step(int n = 1);

  // ent_fire: queues `input` on everything `target` names ("#<id>" = that entity). Returns the count (0 logs a note).
  int fireInput(const QString &target, const QString &input, const QString &param = {}, double delay = 0.0,
                quint32 activator = 0);
  // As if the entity raised it (a trigger touched).
  bool fireOutput(quint32 entity, const QString &output, const QString &param = {}, quint32 activator = 0);
  // Godot's IO.raise(name): OnRaised on every logic_event of that name.
  int raise(const QString &eventName, const QString &param = {});

  [[nodiscard]] size_t pendingCount() const { return m_queue.size(); }
  [[nodiscard]] QStringList dumpQueue() const;
  std::vector<SimLog> takeLog();

  [[nodiscard]] const std::vector<Entity> &entities() const { return m_ents; }
  [[nodiscard]] bool killed(quint32 id) const;
  // Short state readout ("value 3", "enabled", ...).
  [[nodiscard]] QString stateText(quint32 id) const;
  [[nodiscard]] double numericState(quint32 id) const; // counter / compare value, branch 0/1

private:
  struct Event {
    quint32 target = 0;
    QString input;
    QString param;
    quint32 activator = 0;
    quint32 caller = 0;
    int gen = 0; // internal timer events: stale when the generation moved on
  };
  struct State {
    bool enabled = true;
    bool killed = false;
    double value = 0.0;
    double compare = 0.0;
    double min = 0.0, max = 0.0;
    bool on = false;         // light on, door open, sound playing, branch value
    bool locked = false;
    bool moving = false;
    int phase = 0;           // timer oscillator: 0 -> next is Low
    int gen = 0;             // timer / door generation
    double interval = 1.0;   // timer
    double nextTick = -1.0;  // timer
    bool useRandom = false;
    double lower = 1.0, upper = 5.0;
    QVector<int> shuffle;    // logic_case PickRandomShuffle bag
    QVector<int> values;     // logic_multicompare
    QVector<int> fired;      // per-connection fire counts
    double speed = 100.0;    // door
  };

  Entity *ent(quint32 id);
  const Entity *ent(quint32 id) const;
  State &st(quint32 id) { return m_state[id]; }
  void schedule(double at, Event e);
  void deliver(const Event &e);
  void fire(quint32 from, const QString &output, const QString &value, quint32 activator);
  void note(const QString &text, quint32 entity = 0);
  QString nameOf(quint32 id) const;
  double randomInterval(const State &s);
  void armTimer(quint32 id);
  void counterSet(quint32 id, double v, bool fireOutputs, quint32 activator);
  void doorMove(quint32 id, bool open, quint32 activator);

  std::vector<Entity> m_ents;
  QHash<quint32, int> m_index;
  // unordered_map: references stay valid when entries are added.
  std::unordered_map<quint32, State> m_state;
  std::map<std::pair<double, quint64>, Event> m_queue;
  quint64 m_seq = 0;
  double m_now = 0.0;
  std::vector<SimLog> m_log;
  std::mt19937 m_rng;
};

} // namespace eng::io
