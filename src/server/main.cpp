#include "ghammer_version.h"
#include <QCoreApplication>
#include <QResource>
#include <QCommandLineParser>
#include <QDir>
#include <QHostAddress>
#include <QSysInfo>

#include <cstdio>
#include <cstdlib>

#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "shared/lib/logger.h"
#include "server/ctx.h"
#include "server/globals.h"
#include "server/parent_watch.h"

namespace {

// Bold section headings when stdout is a terminal.
bool stdoutIsTerminal() {
#if defined(Q_OS_WIN)
  return false;
#else
  return isatty(fileno(stdout)) != 0;
#endif
}

QString heading(const QString &text) {
  return stdoutIsTerminal() ? QStringLiteral("\033[1m%1\033[0m").arg(text) : text;
}

QString banner() {
  return QStringLiteral("gHammer asset server %1  (Qt %2, %3 %4)")
      .arg(QStringLiteral(GHAMMER_VERSION), QString::fromLatin1(qVersion()), QSysInfo::prettyProductName(),
           QSysInfo::currentCpuArchitecture());
}

// Where the data lives, as Utils::init() will resolve it.
QString configDirFor(const QString &option) {
  if (!option.isEmpty())
    return QDir(option).absolutePath();
  if (const QString over = qEnvironmentVariable("ASSET_SERVER_CONFIG_DIR"); !over.isEmpty())
    return QDir(over).absolutePath();
  return QDir::homePath() + QStringLiteral("/.config/") + QCoreApplication::applicationName();
}

void printHelp(const QCommandLineParser &parser, const QString &configDir) {
  QString out;
  out += heading(banner()) + QStringLiteral("\n\n");
  out += parser.helpText();
  out += QStringLiteral("\n") + heading(QStringLiteral("Network")) + QStringLiteral("\n");
  out += QStringLiteral(
      "  HTTP and the WebSocket control channel (/ws) share one port.\n"
      "  By default it listens on every interface (0.0.0.0); --host 127.0.0.1 keeps\n"
      "  it to this machine.\n");
  out += QStringLiteral("\n") + heading(QStringLiteral("Authentication")) + QStringLiteral("\n");
  out += QStringLiteral(
      "  --auth user:password requires HTTP Basic authentication on every route\n"
      "  and on the /ws upgrade. Without it, anyone who can reach the port can use it.\n"
      "  Editors enter these credentials in Preferences > Asset Server.\n");
  out += QStringLiteral("\n") + heading(QStringLiteral("Data")) + QStringLiteral("\n");
  out += QStringLiteral("  %1\n").arg(QDir::toNativeSeparators(configDir));
  out += QStringLiteral("  Assets, catalogue, derivative cache and settings.json. Moved with\n"
                        "  --config-dir or the ASSET_SERVER_CONFIG_DIR environment variable.\n");
  out += QStringLiteral("\n") + heading(QStringLiteral("Examples")) + QStringLiteral("\n");
  out += QStringLiteral(
      "  ghammer_server\n"
      "      Every interface, port 3000, no authentication.\n"
      "  ghammer_server --port 3000 --auth alice:s3cret\n"
      "      A shared server on the network, with a password.\n"
      "  ghammer_server --host 127.0.0.1 --port 41234 --auth local-1a2b3c:... --parent-pid 1234\n"
      "      How the editor starts its local server (random port and credentials).\n");
  fputs(out.toLocal8Bit().constData(), stdout);
  fflush(stdout);
}

[[noreturn]] void fail(const QString &message) {
  fprintf(stderr, "ghammer_server: %s\nTry 'ghammer_server --help'.\n", message.toLocal8Bit().constData());
  ::exit(2);
}

} // namespace

int main(int argc, char *argv[]) {
  // shared_assets.qrc lives in the static `shared` library: referencing its
  // init function is what links it in.
  Q_INIT_RESOURCE(shared_assets);

  // Data (textures, catalogue, cache) lives in ~/.config/ghammer (or --config-dir).
  QCoreApplication::setApplicationName("ghammer");
  QCoreApplication::setApplicationVersion(QStringLiteral(GHAMMER_VERSION));
  QCoreApplication::setOrganizationDomain("kroket.io");
  QCoreApplication::setOrganizationName("Kroket Ltd.");
  QCoreApplication app(argc, argv);

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("Serves the gHammer asset library to gHammer level editors, and tells every connected\n"
                     "editor about changes as they happen."));
  QCommandLineOption helpOpt({"h", "help"}, "Show this help and exit.");
  QCommandLineOption versionOpt({"v", "version"}, "Show the version and exit.");
  QCommandLineOption hostOpt("host", "Address to listen on (default 0.0.0.0: every interface).", "address",
                             "0.0.0.0");
  QCommandLineOption portOpt("port", "HTTP / WebSocket port (default 3000).", "port", "3000");
  QCommandLineOption authOpt("auth", "Require HTTP Basic authentication with these credentials.", "user:password");
  QCommandLineOption configOpt("config-dir", "Use this data directory instead of ~/.config/ghammer.", "dir");
  QCommandLineOption threadsOpt("threads", "Derivative builder threads (default: cores - 1).", "n", "0");
  QCommandLineOption parentOpt("parent-pid",
                               "Started by an editor: exit when the process with this id exits.", "pid");
  parser.addOptions({helpOpt, versionOpt, hostOpt, portOpt, authOpt, configOpt, threadsOpt, parentOpt});
  parser.process(app);

  if (parser.isSet(helpOpt)) {
    printHelp(parser, configDirFor(parser.value(configOpt)));
    return 0;
  }
  if (parser.isSet(versionOpt)) {
    printf("ghammer_server %s (Qt %s)\n", GHAMMER_VERSION, qVersion());
    return 0;
  }

  QString host = parser.value(hostOpt).trimmed();
  if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
    host = QStringLiteral("127.0.0.1");
  if (QHostAddress(host).isNull())
    fail(QStringLiteral("--host: '%1' is not an IP address").arg(host));
  g::host = host;

  bool ok = false;
  const uint port = parser.value(portOpt).toUInt(&ok);
  if (!ok || port == 0 || port > 65535)
    fail(QStringLiteral("--port: '%1' is not a port number").arg(parser.value(portOpt)));
  g::port = quint16(port);

  // --auth user:password (the password may contain ':')
  if (parser.isSet(authOpt)) {
    const QString auth = parser.value(authOpt);
    const qsizetype colon = auth.indexOf(QLatin1Char(':'));
    if (colon <= 0 || colon == auth.size() - 1)
      fail(QStringLiteral("--auth needs user:password"));
    g::authUser = auth.left(colon);
    g::authPassword = auth.mid(colon + 1);
  }

  if (parser.isSet(parentOpt)) {
    const qint64 pid = parser.value(parentOpt).toLongLong(&ok);
    if (!ok || pid <= 0)
      fail(QStringLiteral("--parent-pid: '%1' is not a process id").arg(parser.value(parentOpt)));
    g::parentPid = pid;
  }

  g::derivativeThreads = parser.value(threadsOpt).toInt();
  if (parser.isSet(configOpt))
    qputenv("ASSET_SERVER_CONFIG_DIR", parser.value(configOpt).toUtf8());

  qInstallMessageHandler(customMessageHandler);
  qInfo().noquote() << banner();
  watchParent(g::parentPid);

  new Ctx();

  return QCoreApplication::exec();
}
