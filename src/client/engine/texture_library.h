#pragma once

#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>

namespace eng {

// A folder of plain image files used as materials, one per base name, map type from the usual suffixes
// (ambientCG / Poly Haven / Substance). Name = path relative to the root without map suffix / extension, lower-case,
// '/' separators ("walls/bricks01"); faces store it, so maps stay portable between machines.
class TextureLibrary final : public QObject {
  Q_OBJECT

public:
  struct Entry {
    QString name;        // material name
    QString display;     // base file name as found on disk
    QString category;    // first path component, "" at the root
    QString albedo, normal, roughness, metallic, ao, arm, height, emissive;
    QSize size;          // albedo pixel size
    bool normalIsDirectX = false;
  };

  static TextureLibrary &instance();

  // Rescans in the background; changed() fires when the new index is live.
  void setRoot(const QString &dir);
  [[nodiscard]] QString root() const { return m_root; }
  void rescan() { setRoot(m_root); }

  [[nodiscard]] bool find(const QString &name, Entry &out) const;
  [[nodiscard]] QVector<Entry> entries() const;
  [[nodiscard]] QStringList categories() const;
  [[nodiscard]] bool scanning() const { return m_scanning; }

signals:
  void changed();

private:
  TextureLibrary();
  static QVector<Entry> scan(const QString &root);

  QString m_root;
  mutable QMutex m_mutex;
  QVector<Entry> m_entries;
  QHash<QString, int> m_index;
  bool m_scanning = false;
  quint64 m_generation = 0;
};

} // namespace eng
