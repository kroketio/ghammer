#pragma once

// Entity I/O export to Godot: a GDScript of constant data (entities, connections, trigger hulls) read by
// the gHammer addon's IOMap at runtime. No code generated per connection; one way only.

#include <QHash>
#include <QString>
#include <QStringList>

#include <map>
#include <vector>

#include <glm/glm.hpp>

#include "entity.h"

namespace eng::godot {

struct ExportOptions {
  QString mapName;    // "e1m1": the file is <dir>/<mapName>_io.gd
  QString sourcePath; // the .ghmap, for the header comment
  // Editor world units -> metres.
  double unitScale = kMetersPerWorldUnit;
  // Entities already in the level scene (lights, brush entity meshes): entity id -> node path below the level
  // root. The runtime adopts that node.
  QHash<quint32, QString> nodePaths;
};

// Brush entity hulls: entity id -> one point list per brush, world space.
using HullMap = std::map<quint32, std::vector<std::vector<glm::vec3>>>;

// The script text (pure; used by the tests).
QString buildScript(const std::vector<Entity> &entities, const HullMap &hulls, const ExportOptions &opt,
                    int *entityCount = nullptr, int *connectionCount = nullptr);

// The nearest folder at or above `dir` holding a project.godot, or empty.
QString findProjectRoot(const QString &dir);
// Writes the bundled addon (:/ghammer/addons/ghammer) to <projectRoot>/addons/ghammer, overwriting.
// Returns the file count, -1 on error.
int writeAddon(const QString &projectRoot, QString *error = nullptr);

} // namespace eng::godot
