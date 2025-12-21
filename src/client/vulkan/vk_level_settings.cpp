// Level-scope settings: the active scene's LCFG values as a prefs Store.

#include "vk_engine.h"

#include <QJsonValue>

#include "client/prefs/preferences.h"

namespace vk {

namespace {

class LevelStore final : public prefs::Store {
public:
  explicit LevelStore(Engine3D &engine) : m_engine(engine) {}
  QVariant getRaw(const QString &name, const QVariant &defaultValue) const override {
    const QVariant v = m_engine.levelSetting(name);
    return v.isValid() ? v : defaultValue;
  }
  void setRaw(const QString &name, const QVariant &value) override { m_engine.setLevelSetting(name, value); }
  void removeRaw(const QString &name) override { m_engine.setLevelSetting(name, {}); }

private:
  Engine3D &m_engine;
};

} // namespace

std::unique_ptr<prefs::Store> makeLevelStore(Engine3D &engine) {
  return std::make_unique<LevelStore>(engine);
}

QVariant Engine3D::levelSetting(const QString &key) const {
  if (!m_scene)
    return {};
  const QJsonValue v = m_scene->levelSettings.value(key);
  return v.isUndefined() ? QVariant() : v.toVariant();
}

void Engine3D::applyLevelSetting(const QString &key, const QJsonValue &value) {
  if (!m_scene)
    return;
  if (value.isUndefined() || value.isNull())
    m_scene->levelSettings.remove(key);
  else
    m_scene->levelSettings.insert(key, value);
  emit levelSettingsChanged();
}

void Engine3D::setLevelSetting(const QString &key, const QVariant &value) {
  if (!m_scene)
    return;
  const QJsonValue before = m_scene->levelSettings.value(key);
  const QJsonValue after = value.isValid() ? QJsonValue::fromVariant(value) : QJsonValue(QJsonValue::Undefined);
  if (before == after)
    return;
  applyLevelSetting(key, after);
  if (!m_undoStack || m_undoStack->isApplying())
    return;
  const prefs::Def *def = prefs::get().def(key);
  const QString label = tr("Level setting: %1").arg(def ? def->label : key);
  m_undoStack->push(std::make_unique<eng::LambdaCommand>(
      label, [this, key, before] { applyLevelSetting(key, before); },
      [this, key, after] { applyLevelSetting(key, after); }, QStringLiteral("levelSetting:") + key));
}

} // namespace vk
