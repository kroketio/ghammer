#pragma once

#include <QColor>
#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <functional>

// Settings at three scopes, each declared once in pref_defs.cpp (the dialog is built from that table).
// Readers never name the scope. Stores (Config's raw API):
//   App      the shared Config JSON
//   Project  <project>/.ghammer/settings.json (proj::ProjectConfig)
//   Level    the map's LCFG chunk; writes are undo steps (Engine3D)
// A scope without a store reads its defaults and ignores writes. Only values off their default are stored.
// Per-map scene settings (sun, fog, ...) are not settings here: they are saved in SCNE.
namespace prefs {

enum class Scope {
  App,
  Project,
  Level,
};

class Store {
public:
  virtual ~Store() = default;
  [[nodiscard]] virtual QVariant getRaw(const QString &name, const QVariant &defaultValue = {}) const = 0;
  virtual void setRaw(const QString &name, const QVariant &value) = 0;
  virtual void removeRaw(const QString &name) = 0;
};

enum class Type {
  Bool,
  Int,
  Double,
  String,
  Color,
  Choice, // int index into `choices`, or the value list in `choiceValues`
  Path,   // directory
  File,
  Program, // an executable (checked when picked)
  Key,    // QKeySequence, stored as portable text
};

struct Def {
  QString key;
  QString category;   // "General", "Viewport", ...; "/" makes a sub-page
  QString label;
  QString tooltip;
  Type type = Type::Bool;
  QVariant defaultValue;
  double minValue = 0.0;
  double maxValue = 0.0;
  double step = 1.0;
  int decimals = 2;
  QString suffix;
  QStringList choices;           // labels for Choice
  QVariantList choiceValues;     // stored values for Choice (defaults to indices)
  bool buttons = false;          // Choice shown as a row of toggle buttons, not a combo box
  bool restartRequired = false;
  bool secret = false;           // String shown masked while not being edited (passwords)
  // The row is enabled only while the setting `enabledKey` has `enabledValue`.
  QString enabledKey;
  QVariant enabledValue;
  Scope scope = Scope::App;
};

class Preferences final : public QObject {
  Q_OBJECT

public:
  static Preferences &instance();

  [[nodiscard]] const QVector<Def> &defs() const { return m_defs; }
  [[nodiscard]] const Def *def(const QString &key) const;
  [[nodiscard]] QStringList categories() const;

  [[nodiscard]] QVariant value(const QString &key) const;
  [[nodiscard]] bool boolValue(const QString &key) const { return value(key).toBool(); }
  [[nodiscard]] int intValue(const QString &key) const { return value(key).toInt(); }
  [[nodiscard]] double doubleValue(const QString &key) const { return value(key).toDouble(); }
  [[nodiscard]] QString stringValue(const QString &key) const { return value(key).toString(); }
  [[nodiscard]] QColor colorValue(const QString &key) const;
  [[nodiscard]] QKeySequence keyValue(const QString &key) const;

  // Clamps to the declared range and emits changed() only when the value moved.
  void setValue(const QString &key, const QVariant &value);
  void reset(const QString &key);
  void resetCategory(const QString &category);
  [[nodiscard]] bool isDefault(const QString &key) const;

  // Runtime registration (e.g. action shortcuts). Declaring the same key twice keeps the first.
  void declare(const Def &def);

  // Calls `fn` now and whenever `key` changes, including on a project / level switch. Dies with `context`.
  void watch(const QString &key, QObject *context, const std::function<void(const QVariant &)> &fn);

  // Setting a store (nullptr = none) re-reads the scope and emits changed() for every value that moved.
  // `label` names it in the dialog. App always has one.
  void setStore(Scope scope, Store *store, const QString &label = {});
  [[nodiscard]] Store *store(Scope scope) const;
  [[nodiscard]] bool available(Scope scope) const { return store(scope) != nullptr; }
  [[nodiscard]] QString scopeLabel(Scope scope) const;
  // The store changed behind the registry's back (undo of a level setting, ...): re-read, emit what moved.
  void refreshScope(Scope scope);

signals:
  void changed(const QString &key, const QVariant &value);
  void storeChanged(prefs::Scope scope);

private:
  Preferences();
  [[nodiscard]] QVariant normalize(const Def &d, const QVariant &v) const;
  [[nodiscard]] QVariant readStored(const Def &d) const;

  QVector<Def> m_defs;
  QHash<QString, int> m_index;
  mutable QHash<QString, QVariant> m_cache;
  Store *m_stores[3] = {nullptr, nullptr, nullptr};
  QString m_labels[3];
};

inline Preferences &get() { return Preferences::instance(); }

// Raw access for keys outside the registry. Null when that scope has nothing open.
inline Store *store(Scope scope) { return Preferences::instance().store(scope); }

inline bool b(const char *key) { return Preferences::instance().boolValue(QString::fromLatin1(key)); }
inline int i(const char *key) { return Preferences::instance().intValue(QString::fromLatin1(key)); }
inline double d(const char *key) { return Preferences::instance().doubleValue(QString::fromLatin1(key)); }
inline QString s(const char *key) { return Preferences::instance().stringValue(QString::fromLatin1(key)); }
inline QColor c(const char *key) { return Preferences::instance().colorValue(QString::fromLatin1(key)); }

// Called once by Preferences::instance().
void registerBuiltins(Preferences &p);

} // namespace prefs
