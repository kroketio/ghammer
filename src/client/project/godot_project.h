#pragma once

#include <QString>

namespace proj {

// Non-default settings of a new Godot project (0 = Godot's default). Written once, at creation.
struct NewGodotSettings {
  int viewportWidth = 0;
  int viewportHeight = 0;
  int maxFps = 0;
};

// A new, empty Godot project in `dir`: what Godot's project manager writes for Forward+ (project.godot,
// icon.svg, .editorconfig). The editor never edits it afterwards. `name` is config/name.
// Fails (with `error`) when `dir` already holds a project.godot or a file cannot be written.
bool writeNewGodotProject(const QString &dir, const QString &name, QString *error = nullptr,
                          const NewGodotSettings &settings = {});

// Exposed for the tests.
QString newProjectGodotText(const QString &name, const NewGodotSettings &settings = {});

// The config/name of the Godot project in `dir`, or empty.
QString godotProjectName(const QString &dir);

} // namespace proj
