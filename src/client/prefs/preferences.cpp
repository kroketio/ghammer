#include "preferences.h"

#include <QSet>

#include "shared/lib/config.h"

namespace prefs {

namespace {
class AppStore final : public Store {
public:
  QVariant getRaw(const QString &name, const QVariant &defaultValue) const override {
    return config()->getRaw(name, defaultValue);
  }
  void setRaw(const QString &name, const QVariant &value) override { config()->setRaw(name, value); }
  void removeRaw(const QString &name) override { config()->removeRaw(name); }
};
} // namespace

Preferences &Preferences::instance() {
  static Preferences *p = [] {
    auto *inst = new Preferences();
    registerBuiltins(*inst);
    return inst;
  }();
  return *p;
}

Preferences::Preferences() : QObject(nullptr) {
  static AppStore app;
  m_stores[int(Scope::App)] = &app;
}

Store *Preferences::store(Scope scope) const {
  return m_stores[int(scope)];
}

QString Preferences::scopeLabel(Scope scope) const {
  return m_labels[int(scope)];
}

void Preferences::setStore(Scope scope, Store *store, const QString &label) {
  if (scope == Scope::App)
    return;
  const bool same = m_stores[int(scope)] == store && m_labels[int(scope)] == label;
  m_stores[int(scope)] = store;
  m_labels[int(scope)] = label;
  refreshScope(scope);
  if (!same)
    emit storeChanged(scope);
}

void Preferences::refreshScope(Scope scope) {
  for (const Def &d: m_defs) {
    if (d.scope != scope)
      continue;
    const auto cached = m_cache.constFind(d.key);
    if (cached == m_cache.constEnd())
      continue; // never read: nothing to tell anyone
    const QVariant before = *cached;
    const QVariant now = readStored(d);
    m_cache.insert(d.key, now);
    if (now != before)
      emit changed(d.key, now);
  }
}

QVariant Preferences::readStored(const Def &d) const {
  const Store *s = m_stores[int(d.scope)];
  const QVariant raw = s ? s->getRaw(d.key, d.defaultValue) : d.defaultValue;
  return normalize(d, raw.isValid() ? raw : d.defaultValue);
}

void Preferences::declare(const Def &def) {
  if (m_index.contains(def.key))
    return;
  m_index.insert(def.key, m_defs.size());
  m_defs.push_back(def);
}

const Def *Preferences::def(const QString &key) const {
  const auto it = m_index.constFind(key);
  return it == m_index.constEnd() ? nullptr : &m_defs[*it];
}

QStringList Preferences::categories() const {
  QStringList out;
  QSet<QString> seen;
  for (const auto &d: m_defs) {
    if (!seen.contains(d.category)) {
      seen.insert(d.category);
      out.push_back(d.category);
    }
  }
  return out;
}

QVariant Preferences::normalize(const Def &d, const QVariant &v) const {
  switch (d.type) {
    case Type::Bool:
      return v.toBool();
    case Type::Int: {
      int x = v.toInt();
      if (d.maxValue > d.minValue)
        x = std::clamp(x, int(d.minValue), int(d.maxValue));
      return x;
    }
    case Type::Double: {
      double x = v.toDouble();
      if (d.maxValue > d.minValue)
        x = std::clamp(x, d.minValue, d.maxValue);
      return x;
    }
    case Type::Color: {
      const QColor col = v.typeId() == QMetaType::QColor ? v.value<QColor>() : QColor(v.toString());
      return col.isValid() ? col.name(QColor::HexArgb) : d.defaultValue.toString();
    }
    case Type::Choice: {
      if (!d.choiceValues.isEmpty()) {
        for (const auto &cv: d.choiceValues)
          if (cv == v || cv.toString() == v.toString())
            return cv;
        return d.defaultValue;
      }
      return std::clamp(v.toInt(), 0, int(std::max<qsizetype>(0, d.choices.size() - 1)));
    }
    case Type::Key:
      return v.typeId() == QMetaType::QKeySequence
                 ? v.value<QKeySequence>().toString(QKeySequence::PortableText)
                 : v.toString();
    default:
      return v.toString();
  }
}

QVariant Preferences::value(const QString &key) const {
  const auto cached = m_cache.constFind(key);
  if (cached != m_cache.constEnd())
    return *cached;
  const Def *d = def(key);
  if (!d)
    return {};
  const QVariant v = readStored(*d);
  m_cache.insert(key, v);
  return v;
}

QColor Preferences::colorValue(const QString &key) const {
  return QColor(value(key).toString());
}

QKeySequence Preferences::keyValue(const QString &key) const {
  return QKeySequence::fromString(value(key).toString(), QKeySequence::PortableText);
}

void Preferences::setValue(const QString &key, const QVariant &v) {
  const Def *d = def(key);
  if (!d)
    return;
  Store *s = m_stores[int(d->scope)];
  if (!s)
    return; // no project / level open: nothing to write to
  const QVariant n = normalize(*d, v);
  if (value(key) == n)
    return;
  // Cache first: a level store's write records an undo step whose refreshScope must see the new value.
  m_cache.insert(key, n);
  if (n == normalize(*d, d->defaultValue))
    s->removeRaw(key);
  else
    s->setRaw(key, n);
  emit changed(key, n);
}

void Preferences::reset(const QString &key) {
  if (const Def *d = def(key))
    setValue(key, d->defaultValue);
}

void Preferences::resetCategory(const QString &category) {
  for (const auto &d: m_defs)
    if (d.category == category || d.category.startsWith(category + QLatin1Char('/')))
      reset(d.key);
}

bool Preferences::isDefault(const QString &key) const {
  const Def *d = def(key);
  return d && value(key) == normalize(*d, d->defaultValue);
}

void Preferences::watch(const QString &key, QObject *context,
                        const std::function<void(const QVariant &)> &fn) {
  fn(value(key));
  connect(this, &Preferences::changed, context, [key, fn](const QString &k, const QVariant &v) {
    if (k == key)
      fn(v);
  });
}

} // namespace prefs
