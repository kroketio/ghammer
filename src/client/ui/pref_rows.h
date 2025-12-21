#pragma once

// Rows of the Preferences cards for the hand-made pages (Local Server, Paths), drawn as the generated pages.

#include <QString>

class QFrame;
class QHBoxLayout;
class QLineEdit;
class QWidget;

namespace ui {

// A read-only value in the editor column, with a copy button (and an eye for secrets). More buttons go into
// prefFieldButtons(box).
QWidget *prefValueField(QWidget *parent, QLineEdit *&line, bool secret, bool copy);
QHBoxLayout *prefFieldButtons(QWidget *field);
QFrame *prefRow(QWidget *card, const QString &label, const QString &tooltip, QWidget *editor, bool first);

} // namespace ui
