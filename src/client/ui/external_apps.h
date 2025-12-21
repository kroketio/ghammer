#pragma once

// Opening files in other programs. External/ImageEditor defaults to GIMP found on PATH at
// startup; a saved choice wins.

#include <QString>

class QWidget;

namespace ui::external {

// GIMP's full path from PATH, or empty.
[[nodiscard]] QString detectImageEditor();

// Opens `path` with External/ImageEditorArgs ({file} = path); shows a message box on failure.
bool openImage(const QString &path, QWidget *parent);

// The runnable program `path` names (an executable, or a macOS .app's Contents/MacOS program), or empty.
[[nodiscard]] QString programPath(const QString &path);
// Asks for a program until a runnable one is picked; empty when cancelled.
QString pickProgram(QWidget *parent, const QString &title, const QString &start = {});
void warnNotProgram(QWidget *parent, const QString &path);

// Runs `godot --editor --path dir` with the project setting Godot/Executable (asked for once).
// False when it did not start.
bool startGodot(const QString &dir, QWidget *parent);

} // namespace ui::external
