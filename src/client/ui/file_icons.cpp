#include "file_icons.h"

#include <QDir>
#include <QFontMetrics>
#include <QFileInfo>
#include <QHash>

#include "client/ui/action_manager.h"

namespace ui {

QString iconNameForSuffix(const QString &suffix) {
  static const QHash<QString, QString> map = [] {
    QHash<QString, QString> m;
    for (const char *s: {"png", "jpg", "jpeg", "webp", "tga", "bmp", "exr", "hdr", "tif", "tiff", "gif", "svg", "ktx",
                         "dds"})
      m.insert(QLatin1String(s), QStringLiteral("file_image"));
    for (const char *s: {"ghmap", "vmf", "obj", "glb", "gltf", "fbx", "dae", "blend", "3ds", "ply", "stl"})
      m.insert(QLatin1String(s), QStringLiteral("file_3d"));
    for (const char *s: {"txt", "md", "json", "cfg", "ini", "cube", "log", "csv", "xml", "tscn", "tres", "import",
                         "godot", "yaml", "yml"})
      m.insert(QLatin1String(s), QStringLiteral("file_text"));
    for (const char *s: {"gd", "py", "sh", "cpp", "h", "hpp", "c", "js", "qml", "glsl", "frag", "vert", "lua"})
      m.insert(QLatin1String(s), QStringLiteral("file_script"));
    for (const char *s: {"wav", "ogg", "mp3", "flac", "opus"})
      m.insert(QLatin1String(s), QStringLiteral("file_sound"));
    for (const char *s: {"mp4", "webm", "mkv", "mov", "avi"})
      m.insert(QLatin1String(s), QStringLiteral("file_movie"));
    for (const char *s: {"zip", "tar", "gz", "xz", "7z", "rar", "bz2", "zst"})
      m.insert(QLatin1String(s), QStringLiteral("file_archive"));
    for (const char *s: {"ttf", "otf", "woff", "woff2"})
      m.insert(QLatin1String(s), QStringLiteral("file_font"));
    return m;
  }();
  return map.value(suffix.toLower(), QStringLiteral("file_blank"));
}

QString homeShortPath(const QString &path) {
  QString p = QDir::toNativeSeparators(path);
  const QString home = QDir::toNativeSeparators(QDir::homePath());
  if (p == home || p.startsWith(home + QDir::separator()))
    p = QStringLiteral("~") + p.mid(home.size());
  return p;
}

QString elidePath(const QString &path, const QFontMetrics &fm, const int width) {
  if (fm.horizontalAdvance(path) <= width)
    return path;
  const QChar sep = path.contains(QLatin1Char('\\')) && !path.contains(QLatin1Char('/')) ? QLatin1Char('\\')
                                                                                         : QLatin1Char('/');
  const QStringList parts = path.split(sep);
  const int n = int(parts.size());
  const QString dots = QStringLiteral("…");
  const auto join = [&](int head, int tail) {
    QStringList out = parts.mid(0, head);
    out << dots;
    out += parts.mid(n - tail);
    return out.join(sep);
  };
  if (n < 3)
    return fm.elidedText(path, Qt::ElideMiddle, width);
  // Keep the first and last folder; then add from the end and the start in turn while it fits.
  int head = 1, tail = 1;
  if (fm.horizontalAdvance(join(head, tail)) > width)
    return fm.elidedText(path, Qt::ElideMiddle, width);
  bool grew = true;
  while (grew && head + tail < n - 1) {
    grew = false;
    if (head + tail < n - 1 && fm.horizontalAdvance(join(head, tail + 1)) <= width) {
      ++tail;
      grew = true;
    }
    if (head + tail < n - 1 && fm.horizontalAdvance(join(head + 1, tail)) <= width) {
      ++head;
      grew = true;
    }
  }
  return join(head, tail);
}

QIcon iconForFile(const QString &path) {
  const QFileInfo fi(path);
  if (fi.isDir())
    return ActionManager::icon(QFileInfo::exists(QDir(path).filePath(QStringLiteral("project.godot")))
                                   ? QStringLiteral("Godot")
                                   : QStringLiteral("file_folder"));
  const QString suffix = fi.suffix().toLower();
  if (suffix == QLatin1String("tscn"))
    return ActionManager::icon(QStringLiteral("PackedScene"));
  if (suffix == QLatin1String("godot"))
    return ActionManager::icon(QStringLiteral("Godot"));
  if (suffix == QLatin1String("tres"))
    return ActionManager::icon(QStringLiteral("Object"));
  if (suffix == QLatin1String("gd"))
    return ActionManager::icon(QStringLiteral("file_script"));
  return ActionManager::icon(iconNameForSuffix(suffix));
}

} // namespace ui
