#pragma once

#include <QIcon>
#include <QString>

namespace ui {

// The ActionManager::icon name for a suffix ("file_blank" when unknown).
QString iconNameForSuffix(const QString &suffix);
// By type; Godot files and Godot project folders get Godot's icons.
QIcon iconForFile(const QString &path);

// `path` shortened to `width` px by dropping whole folders from the middle (the last one always stays).
QString elidePath(const QString &path, const QFontMetrics &fm, int width);
// Home as "~", native separators.
QString homeShortPath(const QString &path);

} // namespace ui
