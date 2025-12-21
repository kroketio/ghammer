#pragma once

#include <QString>

namespace ui {

// Shared dialog look. Apply with ui::scale::setScaledStyleSheet() (sizes are theme tokens);
// widgets opt in by object name:
//
//   dlgPage      page background          dlgCard     rounded card
//   dlgRow       row inside a card (property first=true/false draws the divider)
//   dlgTitle     page title               dlgSubtitle muted line under it
//   dlgSection   heading above a card
//   dlgDesc      muted description text   dlgFooter   bottom button bar
//   dlgPrimary   accent QPushButton       dlgFlat     outlined QPushButton
//   dlgList      QListWidget / QTreeWidget sidebar-style list
//   dlgTree      QTreeWidget sidebar with headings and sub-items (as Preferences; ui::subItemGlyph)
//   tbLink       QToolButton drawn as a text link ("Show 25 more", "Clear all")
inline QString dialogStyle() {
  return QStringLiteral(R"(
#dlgPage { background: #101428; }
#dlgTitle { font-size: $font_page; font-weight: 600; background: transparent; }
#dlgSubtitle { color: #8f9bbd; background: transparent; }
#dlgSection { color: #8f9bbd; font-size: $font_heading; font-weight: 600; background: transparent; }
#dlgDesc { color: #8a95b5; font-size: $font_caption; background: transparent; }
#dlgCard { background: #151a33; border: 1px solid #232A4E; border-radius: 8px; }
#dlgRow { background: transparent; border: none; }
#dlgRow[first="false"] { border-top: 1px solid #222949; }
#dlgCard QLabel, #dlgCard QCheckBox, #dlgCard QSlider, #dlgEditorBox { background: transparent; }
#dlgFooter { background: #0c1022; border-top: 1px solid #1f2747; }
QPushButton#dlgPrimary { background: #4866C5; color: white; border: none; border-radius: 5px; padding: 6px 22px; }
QPushButton#dlgPrimary:hover { background: #6A84D9; }
QPushButton#dlgPrimary:disabled { background: #2a3360; color: #7b86a8; }
QPushButton#dlgFlat { background: transparent; border: 1px solid #346077; border-radius: 5px; padding: 6px 14px; }
QPushButton#dlgFlat:hover { background: #171C38; }
QPushButton#dlgFlat:disabled { color: #59627f; border-color: #2a3360; }
QToolButton#dlgTool { background: transparent; border: 1px solid #2a3360; border-radius: 5px; padding: 4px 8px; }
QToolButton#dlgTool:hover { background: #171C38; border-color: #4866C5; }
QToolButton#dlgTool:disabled { color: #59627f; }
#dlgList { background: #0c1022; border: 1px solid #1f2747; border-radius: 8px; outline: 0; padding: 4px; }
#dlgList::item { padding: 6px; border: none; border-radius: 6px; margin: 1px 2px; }
#dlgList::item:hover { background: #171C38; }
#dlgList::item:selected { background: #4866C5; color: white; }
#dlgTree { background: #0c1022; border: 1px solid #1f2747; border-radius: 8px; outline: 0; padding: 4px;
  show-decoration-selected: 0; selection-background-color: transparent; }
#dlgTree::item { padding: 5px 6px; border: none; border-radius: 5px; margin: 1px 2px; }
#dlgTree::item:hover { background: #171C38; }
#dlgTree::item:selected { background: #4866C5; color: white; }
#dlgTree::branch, #dlgTree::branch:selected, #dlgTree::branch:hover { image: none; border: none;
  background: transparent; }
QToolButton#tbLink { background: transparent; border: none; color: #7fa6ff; padding: 2px 4px; }
QToolButton#tbLink:hover { color: #a9c3ff; text-decoration: underline; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; border: none; }
QScrollBar::handle:vertical { background: #2a3360; border-radius: 4px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #3a4580; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: none; background: none; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; border: none; }
QScrollBar::handle:horizontal { background: #2a3360; border-radius: 4px; min-width: 30px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; border: none; background: none; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }
)");
}

} // namespace ui
