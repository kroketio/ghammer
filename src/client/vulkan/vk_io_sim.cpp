// The I/O simulator: eng::io::Simulator on a copy of the entities at 60 Hz; the document never changes.

#include <QElapsedTimer>
#include <QTimer>

#include "vk_engine.h"

#include "client/engine/io_sim.h"
#include "client/engine/scene.h"
#include "client/prefs/preferences.h"

namespace vk {

bool Engine3D::ioSimRunning() const { return bool(m_ioSim); }

void Engine3D::ioSimStart() {
  if (m_ioSim || !m_scene)
    return;
  m_ioSim = std::make_unique<eng::io::Simulator>(m_scene->entities, 1u);
  m_ioSimPaused = false;
  m_ioPulses.clear();
  m_ioFlash.clear();
  if (!m_ioSimTimer) {
    m_ioSimTimer = new QTimer(this);
    m_ioSimTimer->setInterval(16);
    connect(m_ioSimTimer, &QTimer::timeout, this, &Engine3D::ioSimTick);
  }
  m_ioSimClock = std::make_unique<QElapsedTimer>();
  m_ioSimClock->start();
  m_ioSim->start();
  drainIOSimLog();
  m_ioSimTimer->start();
  ++m_viewEpoch;
  emit ioSimChanged();
  requestRedraw();
}

void Engine3D::ioSimStop() {
  if (!m_ioSim)
    return;
  qInfo().noquote() << QStringLiteral("[io] simulation stopped at t=%1 s").arg(m_ioSim->time(), 0, 'f', 2);
  emit ioSimLogged(m_ioSim->time(), QStringLiteral("simulation stopped"), 0, 0, true);
  m_ioSim.reset();
  if (m_ioSimTimer)
    m_ioSimTimer->stop();
  m_ioPulses.clear();
  m_ioFlash.clear();
  ++m_viewEpoch;
  ++m_entityEpoch; // marker tints go back to normal
  emit entitiesChanged();
  emit ioSimChanged();
  requestRedraw();
}

void Engine3D::setIOSimPaused(const bool paused) {
  if (!m_ioSim || m_ioSimPaused == paused)
    return;
  m_ioSimPaused = paused;
  if (m_ioSimClock)
    m_ioSimClock->restart();
  qInfo().noquote() << (paused ? QStringLiteral("[io] paused") : QStringLiteral("[io] resumed"));
  emit ioSimChanged();
}

int Engine3D::ioSimStep(const int n) {
  if (!m_ioSim)
    ioSimStart();
  if (!m_ioSim)
    return 0;
  setIOSimPaused(true);
  const int done = m_ioSim->step(n);
  if (!done)
    qInfo("[io] the event queue is empty");
  drainIOSimLog();
  emit ioSimChanged();
  requestRedraw();
  return done;
}

int Engine3D::ioSimFire(const QString &target, const QString &input, const QString &param, const double delay) {
  if (!m_ioSim)
    ioSimStart();
  if (!m_ioSim)
    return 0;
  const int n = m_ioSim->fireInput(target, input, param, delay);
  drainIOSimLog();
  return n;
}

bool Engine3D::ioSimFireOutput(const quint32 entity, const QString &output, const QString &param) {
  if (!m_ioSim)
    ioSimStart();
  if (!m_ioSim)
    return false;
  const bool ok = m_ioSim->fireOutput(entity, output, param);
  drainIOSimLog();
  return ok;
}

int Engine3D::ioSimRaise(const QString &eventName, const QString &param) {
  if (!m_ioSim)
    ioSimStart();
  if (!m_ioSim)
    return 0;
  const int n = m_ioSim->raise(eventName, param);
  drainIOSimLog();
  return n;
}

QStringList Engine3D::ioSimQueue() const { return m_ioSim ? m_ioSim->dumpQueue() : QStringList(); }

double Engine3D::ioSimTime() const { return m_ioSim ? m_ioSim->time() : 0.0; }

QString Engine3D::ioSimState(const quint32 id) const { return m_ioSim ? m_ioSim->stateText(id) : QString(); }

bool Engine3D::ioSimKilled(const quint32 id) const { return m_ioSim && m_ioSim->killed(id); }

void Engine3D::ioSimTick() {
  if (!m_ioSim)
    return;
  const double real = m_ioSimClock ? double(m_ioSimClock->restart()) / 1000.0 : 0.016;
  if (!m_ioSimPaused) {
    const double scale = std::clamp(prefs::get().value(QStringLiteral("IO/SimTimeScale")).toDouble(), 0.01, 100.0);
    m_ioSim->advance(std::min(real, 0.25) * scale);
    drainIOSimLog();
  }
  const double now = m_ioSim->time();
  m_ioPulses.erase(std::remove_if(m_ioPulses.begin(), m_ioPulses.end(),
                                  [now](const ActivePulse &p) { return now > p.t1 + 0.05; }),
                   m_ioPulses.end());
  for (auto it = m_ioFlash.begin(); it != m_ioFlash.end();)
    it = now - it->second > 0.35 ? m_ioFlash.erase(it) : std::next(it);
  // Marker tints only need a redraw (m_entityEpoch would also resync props and lights).
  ++m_viewEpoch;
  emit ioSimChanged();
  requestRedraw();
}

void Engine3D::drainIOSimLog() {
  if (!m_ioSim)
    return;
  for (const auto &l: m_ioSim->takeLog()) {
    switch (l.kind) {
      case eng::io::SimLog::Kind::Fired: {
        qInfo().noquote() << QStringLiteral("[io] t=%1  %2").arg(l.time, 0, 'f', 2).arg(l.text);
        emit ioSimLogged(l.time, l.text, l.from, l.to, false);
        // Sender to target over the delay (a visible minimum); console inputs start at the target.
        ActivePulse p;
        p.from = l.from ? l.from : l.to;
        p.to = l.to;
        p.t0 = l.time;
        p.t1 = std::max(l.deliverAt, l.time + 0.35);
        m_ioPulses.push_back(p);
        if (m_ioPulses.size() > 512)
          m_ioPulses.erase(m_ioPulses.begin());
        break;
      }
      case eng::io::SimLog::Kind::Delivered:
        m_ioFlash[l.to] = l.time;
        break;
      case eng::io::SimLog::Kind::Note:
        qInfo().noquote() << QStringLiteral("[io] t=%1  %2").arg(l.time, 0, 'f', 2).arg(l.text);
        emit ioSimLogged(l.time, l.text, l.from, l.to, true);
        break;
    }
  }
}

std::vector<IOPulse> Engine3D::ioPulses() const {
  std::vector<IOPulse> out;
  if (!m_ioSim || !m_scene)
    return out;
  const double now = m_ioSim->time();
  for (const auto &p: m_ioPulses) {
    const eng::Entity *a = m_scene->findEntity(p.from);
    const eng::Entity *b = m_scene->findEntity(p.to);
    if (!a || !b || now < p.t0)
      continue;
    IOPulse q;
    q.a = a->origin;
    q.b = b->origin;
    q.t = float(std::clamp((now - p.t0) / std::max(1e-6, p.t1 - p.t0), 0.0, 1.0));
    out.push_back(q);
  }
  return out;
}

std::vector<Engine3D::IOPulseLink> Engine3D::ioPulseLinks() const {
  std::vector<IOPulseLink> out;
  if (!m_ioSim)
    return out;
  const double now = m_ioSim->time();
  for (const auto &p: m_ioPulses) {
    if (now < p.t0)
      continue;
    out.push_back({p.from, p.to, float(std::clamp((now - p.t0) / std::max(1e-6, p.t1 - p.t0), 0.0, 1.0))});
  }
  return out;
}

bool Engine3D::ioSimFlashing(const quint32 id) const { return m_ioSim && m_ioFlash.count(id) != 0; }

} // namespace vk
