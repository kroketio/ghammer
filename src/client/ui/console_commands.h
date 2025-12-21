#pragma once

// Console command registry. The I/O commands mirror Source's console (ent_fire, ...), plus io_* for the simulator.

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace ui {

class ConsoleCommands {
public:
  struct Command {
    QString name;
    QString usage;
    QString help;
    std::function<void(const QStringList &args)> run;
  };

  static ConsoleCommands &instance();
  void add(Command c);
  // Parses and runs one line. False on error.
  bool run(const QString &line);
  [[nodiscard]] QStringList names() const;
  // Splits on spaces, keeping "quoted strings" together.
  static QStringList tokenize(const QString &line);

private:
  ConsoleCommands();
  std::vector<Command> m_commands;
};

} // namespace ui
