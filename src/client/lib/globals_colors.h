#pragma once
#include <string>
#include <map>
#include <algorithm>
#include <vector>

#include <QString>

namespace colors {
  inline const std::map<std::string, const char*> all = {
    {"color_active", "#4866C5"},
    {"color_highlight", "#4D92B6"},
    {"color_btn_highlight", "#6A84D9"},
    {"color_border", "#346077"},
    {"color_grey", "#171C38"},
    {"color_bg", "#101428"},
    {"color_bg_light", "#232A4E"},
  };

  // Longest name first: $color_bg is a prefix of $color_bg_light.
  inline QString injectStyleSheetColors(QString& stylesheet) {
    std::vector<std::string> names;
    names.reserve(all.size());
    for (const auto& [name, hex] : all)
      names.push_back(name);
    std::sort(names.begin(), names.end(),
              [](const std::string &a, const std::string &b) { return a.size() > b.size(); });

    for (const auto &name : names)
      stylesheet.replace(QStringLiteral("$%1").arg(QString::fromStdString(name)),
                         QString::fromStdString(all.at(name)));
    return stylesheet;
  }
}
