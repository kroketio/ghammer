#pragma once

// Layer 1 of the material property stack: a texture's values before any override, from its tags / name words
// (rules in :/assets/material_defaults.json) and the maps it has.

#include <QJsonObject>
#include <QStringList>

#include <vector>

#include "material_props.h"
#include "texture_entry.h"

class MaterialDefaults {
public:
  struct Rule {
    QString name;
    QStringList match;  // lower-case words
    MaterialProps props;
  };

  // The rules from the bundled resource (loaded once, thread-safe).
  static const MaterialDefaults &instance();

  // Parses a rules document (same format as the resource); used by tests.
  static MaterialDefaults fromJson(const QJsonObject &doc, QStringList *errors = nullptr);

  // Words the rules are matched against: tags plus the texture name split on
  // CamelCase, digits and '_' ("RustyMetalPlate002" -> rusty, metal, plate).
  static QStringList matchWords(const TextureEntry &e);

  // Sparse layer: rules matched by the tags, then rules matched by the name
  // words (first rule per key wins), then the map-aware values:
  //   roughness map present  -> roughness 1.0, only when no rule set it
  //   metal map present      -> metallic 1.0, only when no rule set it
  //   ARM layout             -> ao/roughness/metal channels red/green/blue
  //   separate maps          -> channels red (Godot's default)
  //   emission map           -> emission_enabled
  //   alpha / opacity map    -> transparency
  [[nodiscard]] MaterialProps compute(const TextureEntry &e) const;

  // Name of the first rule that matched (tags before name words), or empty.
  [[nodiscard]] QString matchedRule(const TextureEntry &e) const;

  [[nodiscard]] const std::vector<Rule> &rules() const { return m_rules; }

private:
  std::vector<Rule> m_rules;
  MaterialProps m_fallback;
};
