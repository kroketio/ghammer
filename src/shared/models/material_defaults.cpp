#include "material_defaults.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QtDebug>

using matprops::Channel;
using matprops::Key;

const MaterialDefaults &MaterialDefaults::instance() {
  static const MaterialDefaults s = [] {
    QFile f(QStringLiteral(":/assets/material_defaults.json"));
    if (!f.open(QIODevice::ReadOnly)) {
      qWarning() << "material defaults: cannot open" << f.fileName();
      return MaterialDefaults();
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError)
      qWarning() << "material defaults: parse error" << pe.errorString();
    QStringList errors;
    MaterialDefaults d = fromJson(doc.object(), &errors);
    for (const auto &e: errors)
      qWarning() << "material defaults:" << e;
    return d;
  }();
  return s;
}

MaterialDefaults MaterialDefaults::fromJson(const QJsonObject &doc, QStringList *errors) {
  MaterialDefaults d;
  for (const auto &rv: doc.value(QStringLiteral("rules")).toArray()) {
    const QJsonObject ro = rv.toObject();
    Rule r;
    r.name = ro.value(QStringLiteral("name")).toString();
    for (const auto &m: ro.value(QStringLiteral("match")).toArray())
      r.match.append(m.toString().trimmed().toLower());
    QStringList errs;
    r.props = MaterialProps::fromJson(ro.value(QStringLiteral("props")).toObject(), &errs);
    if (errors)
      for (const auto &e: errs)
        errors->append(QStringLiteral("rule '%1': %2").arg(r.name, e));
    if (!r.match.isEmpty())
      d.m_rules.push_back(std::move(r));
  }
  d.m_fallback =
      MaterialProps::fromJson(doc.value(QStringLiteral("fallback")).toObject().value(QStringLiteral("props")).toObject(),
                              errors);
  return d;
}

namespace {

QStringList tagWords(const TextureEntry &e) {
  QSet<QString> words;
  for (const auto &t: e.tags) {
    const QString w = t.trimmed().toLower();
    if (w.isEmpty())
      continue;
    words.insert(w);
    for (const auto &part: w.split(QRegularExpression(QStringLiteral("[\\s_-]+")), Qt::SkipEmptyParts))
      words.insert(part);
  }
  QStringList out(words.begin(), words.end());
  out.sort();
  return out;
}

QStringList nameWords(const TextureEntry &e) {
  // CamelCase / digits / separators: "RustyMetalPlate002" -> rusty metal plate.
  static const QRegularExpression camel(QStringLiteral("[A-Z]?[a-z]+|[A-Z]+(?![a-z])"));
  QSet<QString> words;
  auto it = camel.globalMatch(e.name);
  while (it.hasNext())
    words.insert(it.next().captured().toLower());
  QStringList out(words.begin(), words.end());
  out.sort();
  return out;
}

} // namespace

QStringList MaterialDefaults::matchWords(const TextureEntry &e) {
  QStringList out = tagWords(e) + nameWords(e);
  out.sort();
  out.removeDuplicates();
  return out;
}

namespace {

bool ruleMatches(const MaterialDefaults::Rule &r, const QStringList &words) {
  for (const auto &w: words) {
    for (const auto &m: r.match) {
      if (w == m)
        return true;
      // cheap plural folding: "bricks" matches "brick"
      if (w.size() == m.size() + 1 && w.endsWith(QLatin1Char('s')) && w.startsWith(m))
        return true;
    }
  }
  return false;
}

} // namespace

MaterialProps MaterialDefaults::compute(const TextureEntry &e) const {
  MaterialProps out;
  // Tags first, then name words: a "concrete" tag beats a "ConcretePlaster" name's plaster.
  for (const QStringList &words: {tagWords(e), nameWords(e)})
    for (const auto &r: m_rules) {
      if (!ruleMatches(r, words))
        continue;
      // first rule per key wins
      out.overlay(r.props.masked(r.props.setMask & ~out.setMask));
    }
  out.overlay(m_fallback.masked(m_fallback.setMask & ~out.setMask));

  // A roughness / metal map with no rule value: 1.0, so the map drives it
  // unscaled (Godot multiplies the value by the map). A tag value always wins.
  const MapLayout layout = e.layout();
  const bool roughMap = layout == MapLayout::Arm || e.hasMap(MapType::Roughness);
  const bool metalMap = layout == MapLayout::Arm || e.hasMap(MapType::Metalness);
  if (roughMap && !out.isSet(Key::Roughness))
    out.setValue(Key::Roughness, 1.0);
  if (metalMap && !out.isSet(Key::Metallic))
    out.setValue(Key::Metallic, 1.0);
  if (layout == MapLayout::Arm) {
    out.setValue(Key::AoChannel, QString::fromLatin1(matprops::channelName(Channel::Red)));
    out.setValue(Key::RoughnessChannel, QString::fromLatin1(matprops::channelName(Channel::Green)));
    out.setValue(Key::MetallicChannel, QString::fromLatin1(matprops::channelName(Channel::Blue)));
  } else {
    out.setValue(Key::AoChannel, QString::fromLatin1(matprops::channelName(Channel::Red)));
    out.setValue(Key::RoughnessChannel, QString::fromLatin1(matprops::channelName(Channel::Red)));
    out.setValue(Key::MetallicChannel, QString::fromLatin1(matprops::channelName(Channel::Red)));
  }
  if (e.hasMap(MapType::Emission))
    out.setValue(Key::EmissionEnabled, true);
  if (e.alpha || e.hasMap(MapType::Opacity))
    out.setValue(Key::Transparency, true);
  return out;
}

QString MaterialDefaults::matchedRule(const TextureEntry &e) const {
  for (const QStringList &words: {tagWords(e), nameWords(e)})
    for (const auto &r: m_rules)
      if (ruleMatches(r, words))
        return r.name;
  return {};
}
