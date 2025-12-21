#pragma once

#include <map>
#include <vector>

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;
class QTimer;

namespace vk {
class Engine3D;
}

namespace ui {

class ModifierEditor;

// Inspector › Modifiers: the selected modified object's stack, one card per modifier (Blender style).
// Material mode: only Edit Source (a modified object's faces are textured on its sources).
class ModifierPanel final : public QWidget {
  Q_OBJECT

public:
  explicit ModifierPanel(vk::Engine3D *engine, QWidget *parent = nullptr);
  // A host to edit, or brushes a host could be made of.
  [[nodiscard]] static bool relevant(vk::Engine3D *engine);

private:
  void refresh();
  void rebuildCards();

  struct Card {
    quint32 id = 0;
    QString kind;
    QWidget *box = nullptr;
    QToolButton *expand = nullptr, *enabled = nullptr, *editSource = nullptr;
    QLabel *title = nullptr, *status = nullptr;
    ModifierEditor *editor = nullptr;
  };

  vk::Engine3D *m_engine;
  QTimer *m_timer;
  QLineEdit *m_name;
  QPushButton *m_editSource;
  QCheckBox *m_showResult;
  QToolButton *m_add;
  QWidget *m_sourceRow;
  QPushButton *m_addSel, *m_remSel;
  QWidget *m_footer;
  QVBoxLayout *m_cardsLayout;
  QWidget *m_cards;
  std::vector<Card> m_cardList;
  quint32 m_host = 0;
  quint32 m_cardsHost = 0; // the host the cards and their callbacks were built for
  std::vector<std::pair<quint32, QString>> m_shape; // the stack the cards were built for
};

} // namespace ui
