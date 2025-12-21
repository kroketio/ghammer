#pragma once

// Godot's text formats (.tscn, .tres, .import), edited in place: the export changes only what the editor owns
// and keeps everything else (user nodes / properties, uids, ext_resource ids). Values compare by meaning, so Godot's
// own number formatting is no change. Godot 4.8+ (format=3, no load_steps).

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include <glm/glm.hpp>

class QColor;

namespace eng::godot {

// One Variant in Godot's text syntax.
struct GdValue {
  enum class Kind { Null, Bool, Int, Float, String, StringName, NodePath, Array, Dict, Call, Raw };
  Kind kind = Kind::Null;
  bool b = false;
  qint64 i = 0;
  double f = 0.0;
  QString s;                                // string / StringName / NodePath / call name / raw text
  QVector<GdValue> items;                   // array elements, call arguments
  QVector<QPair<GdValue, GdValue>> dict;    // dictionary entries in order

  static GdValue boolean(bool v);
  static GdValue integer(qint64 v);
  static GdValue number(double v);
  static GdValue string(const QString &v);
  static GdValue call(const QString &name, const QVector<GdValue> &args);
  static GdValue raw(const QString &text);
  static GdValue color(const QColor &c);  // sRGB components, as Godot stores Color
  // Basis as its three column axes (local X, Y, Z), then the origin.
  static GdValue transform(const glm::vec3 &x, const glm::vec3 &y, const glm::vec3 &z, const glm::vec3 &origin);
  static GdValue extResource(const QString &id);

  // Unparsable text comes back as Kind::Raw (compared verbatim).
  static GdValue parse(const QString &text);
  [[nodiscard]] QString toText() const;
  // Numbers within `eps`, Int == Float when equal, dictionaries regardless of key order.
  [[nodiscard]] bool equals(const GdValue &o, double eps = 1e-5) const;

  [[nodiscard]] bool isNull() const { return kind == Kind::Null; }
  [[nodiscard]] double toNumber(double fallback = 0.0) const;
  [[nodiscard]] QString toString() const { return s; }
  [[nodiscard]] const GdValue *find(const QString &key) const;
  void set(const QString &key, const GdValue &v);  // dictionaries only
};

// Godot's number spelling: "1.0" / "0.25" for a property, "1" inside Color() / Transform3D().
// 7 significant digits (values come from floats).
QString gdNumber(double v, bool forceDecimal);
QString gdString(const QString &s);

// A .tscn / .tres / .import / project.godot file as sections of properties.
class GdTextFile {
public:
  struct Attr {
    QString key;
    QString raw;
  };
  struct Prop {
    QString key;  // empty: a line kept verbatim (comment)
    QString raw;
  };
  struct Section {
    QString tag;  // "gd_scene", "ext_resource", "node", "resource", "params"... empty: before any section
    QVector<Attr> attrs;
    QVector<Prop> props;
    bool blankAfterHeader = false;  // .import style
  };

  static GdTextFile parse(const QString &text);
  // `tag` e.g. "gd_scene" with attributes format=3.
  static GdTextFile create(const QString &tag, const QVector<QPair<QString, GdValue>> &attrs, bool importStyle = false);
  [[nodiscard]] QString toText() const;
  // Changed since parse(); toText() of an unchanged file is the original text, byte for byte.
  [[nodiscard]] bool isDirty() const { return m_dirty; }

  QVector<Section> sections;

  [[nodiscard]] int find(const QString &tag, int from = 0) const;
  int ensureSection(const QString &tag);  // appended when missing
  [[nodiscard]] GdValue attr(int sec, const QString &key) const;
  bool setAttr(int sec, const QString &key, const GdValue &v);
  // Properties compare by meaning; a new key is appended.
  [[nodiscard]] int findProp(int sec, const QString &key) const;
  [[nodiscard]] GdValue prop(int sec, const QString &key) const;
  bool setProp(int sec, const QString &key, const GdValue &v);
  bool removeProp(int sec, const QString &key);

  // The id of the entry with this path (id and uid kept), or a new one. `uid` may be empty.
  QString extResource(const QString &type, const QString &path, const QString &uid = {});
  // Returns how many were removed.
  int pruneExtResources();

  [[nodiscard]] int rootNode() const;
  [[nodiscard]] QString nodePath(int sec) const;  // "." for the root, "Lights/Lamp" below it
  [[nodiscard]] int nodeByProp(const QString &key, const GdValue &value) const;
  [[nodiscard]] QVector<int> children(const QString &path) const;
  // Appended after the other nodes (before connections / editable lines).
  int addNode(const QString &name, const QString &type, const QString &parentPath, const QString &instanceExtId = {});
  // Descendants and [connection] / [editable] lines follow.
  bool renameNode(int sec, const QString &name);
  // Also every [connection] / [editable] line into the subtree.
  int removeNodeTree(int sec);
  // A free child name ("Lamp", "Lamp_2"...), case-sensitive as in Godot. `except`: a node whose own name
  // does not count (the one being renamed).
  [[nodiscard]] QString uniqueChildName(const QString &parentPath, const QString &wanted, int except = -1) const;

private:
  QString m_original;
  bool m_dirty = false;
  bool m_importStyle = false;  // key=value without spaces, blank line after headers
};

// Godot node names cannot hold . : @ / " %.
QString validNodeName(const QString &name);

// Godot's uid: the .tscn / .tres header, <file>.uid for scripts, else the [remap] of <file>.import.
QString readUid(const QString &absPath);

// Writes through QSaveFile only when the bytes differ. Returns 1 written, 0 unchanged, -1 error.
int writeIfChanged(const QString &path, const QByteArray &bytes, QString *error = nullptr);

} // namespace eng::godot
