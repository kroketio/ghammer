#pragma once

#include <functional>

#include <QJsonObject>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QTimer;
class QToolButton;

namespace ui {

class FlowLayout;
class TagChip;

// The tag filter of a texture sidebar. It owns the included / excluded tags; the window puts them in its
// query on changed() and hands back each result (setResult).
class TagPicker final : public QWidget {
  Q_OBJECT

public:
  explicit TagPicker(QWidget *parent = nullptr);

  [[nodiscard]] const QStringList &tags() const { return m_tags; }
  [[nodiscard]] const QStringList &notTags() const { return m_notTags; }
  [[nodiscard]] bool empty() const { return m_tags.isEmpty() && m_notTags.isEmpty(); }
  void addTag(const QString &tag, bool exclude);
  void removeTag(const QString &tag);
  // Also the filter text; `notify` emits changed().
  void clear(bool notify);

  void setPopularFilter(const QJsonObject &filter);
  void refreshPopular();
  // `query` is used by the filter field's tag search.
  void setResult(const QVector<QPair<QString, int>> &facets, int distinctTags, const QJsonObject &query);

  // Removable chips for the active tags ("not x" for excluded ones).
  void addActiveChips(FlowLayout *flow, QWidget *box);

signals:
  void changed();

private:
  void rebuildPopular(const QVector<QPair<QString, int>> &tags);
  void rebuildList();

  QStringList m_tags, m_notTags;
  QJsonObject m_popularFilter{{QStringLiteral("sky"), false}};  // materials, not skyboxes, by default
  QVector<QPair<QString, int>> m_popular;
  QVector<QPair<QString, int>> m_facets;
  int m_distinct = 0;
  QJsonObject m_query;
  int m_limit = 25;

  QWidget *m_popularBox = nullptr;
  FlowLayout *m_popularFlow = nullptr;
  QLabel *m_summary = nullptr;
  QLineEdit *m_filter = nullptr;
  QListWidget *m_list = nullptr;
  QToolButton *m_more = nullptr;
  QTimer *m_debounce = nullptr;
};

TagChip *addFilterChip(FlowLayout *flow, QWidget *box, const QString &label, bool exclude,
                       std::function<void()> onRemove);

} // namespace ui
