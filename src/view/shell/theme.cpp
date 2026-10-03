#include "view/shell/theme.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>

#include "constants/design_tokens.h"

namespace app::theme {

namespace {

// Surfaces by depth: bg0 deepest (window ground, canvas surround), bg1 panel,
// bg2 raised chrome (borders, headers, buttons). bg2 uses kSurfaceHover
// because it has the same value. QColor has no constexpr string constructor.
const QColor kBg0(QString::fromLatin1(design::kSurface0));
const QColor kBg1(QString::fromLatin1(design::kSurface1));
const QColor kBg2(QString::fromLatin1(design::kSurfaceHover));
const QColor kOrdoBlue(QString::fromLatin1(design::kAccentBrand));
const QColor kText(QString::fromLatin1(design::kTextPrimary));
const QColor kTextDisabled(QString::fromLatin1(design::kTextDisabled));
const QColor kTextOnAccent(QString::fromLatin1(design::kTextOnAccent));

}  // namespace

void apply(QApplication& application) {
    // Fusion, not the platform style: the Windows native styles resolve a
    // number of controls (menus, combo popups, spin buttons) straight from
    // system theme bitmaps that a stylesheet cannot fully recolor.
    application.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette palette;
    palette.setColor(QPalette::Window, kBg1);
    palette.setColor(QPalette::WindowText, kText);
    palette.setColor(QPalette::Base, kBg0);
    palette.setColor(QPalette::AlternateBase, kBg1);
    palette.setColor(QPalette::Text, kText);
    palette.setColor(QPalette::PlaceholderText, kTextDisabled);
    palette.setColor(QPalette::Button, kBg2);
    palette.setColor(QPalette::ButtonText, kText);
    palette.setColor(QPalette::BrightText, kTextOnAccent);
    palette.setColor(QPalette::Highlight, kOrdoBlue);
    palette.setColor(QPalette::HighlightedText, kTextOnAccent);
    palette.setColor(QPalette::ToolTipBase, kBg1);
    palette.setColor(QPalette::ToolTipText, kText);
    palette.setColor(QPalette::Link, QColor(QString::fromLatin1(design::kAccentInteractive)));
    palette.setColor(QPalette::Disabled, QPalette::Text, kTextDisabled);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, kTextDisabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, kTextDisabled);
    application.setPalette(palette);

    // Pin only the base size; without it unstyled text follows the OS message
    // font (9pt on Windows) or Qt's 12pt fallback offscreen. Canvas items set
    // their own sizes.
    QFont base = application.font();
    base.setPixelSize(design::kTypeSizePx);
    application.setFont(base);

    application.setStyleSheet(appStyleSheet());
}

QString appStyleSheet() {
    // Every color is a role placeholder (e.g. {surface-0}) resolved from the
    // design tokens by design::resolveRoles() below, by role not by value
    // ({outline} and {surface-hover} may share a hex). No literal hex here.
    const QString kTemplate = QStringLiteral(R"qss(
QMainWindow {
    background: {surface-0};
}

/* ---- menus ------------------------------------------------------------ */
QMenuBar {
    background: {surface-1};
    color: {text-primary};
    border-bottom: 1px solid {outline};
}
QMenuBar::item {
    padding: 4px 10px;
    background: transparent;
}
QMenuBar::item:selected {
    background: {surface-hover};
}
QMenu {
    background: {surface-1};
    color: {text-primary};
    border: 1px solid {outline};
    padding: 4px 0;
}
QMenu::item {
    padding: 5px 24px 5px 16px;
}
QMenu::item:selected {
    background: {accent-brand};
    color: {text-on-accent};
}
QMenu::item:disabled {
    color: {text-disabled};
}
QMenu::separator {
    height: 1px;
    background: {outline};
    margin: 4px 8px;
}

/* ---- activity rail (VSCode activity-bar look, DESIGN decision 11) ----- */
QToolBar#activityRail {
    background: {surface-1};
    border: none;
    border-right: 1px solid {outline};
    padding: 2px 0;
    spacing: 0;
}
QToolBar#activityRail QToolButton {
    background: transparent;
    border: none;
    border-left: 2px solid transparent;
    border-right: 2px solid transparent;
    padding: 9px 8px;
    margin: 0;
}
QToolBar#activityRail QToolButton:hover {
    background: {surface-hover};
}
QToolBar#activityRail QToolButton:checked {
    background: {surface-hover};
    border-left: 2px solid {accent-interactive};
}
QToolBar#activityRail QToolButton:pressed {
    background: {surface-pressed};
}
/* Without an explicit rule the style's default separator renders as a stub
   mark at the rail's edge, which reads as noise rather than as a group
   boundary -- and the boundary is load-bearing here: it is what separates
   the mode toggle, the sim transport, the view command, and Generate C++
   (the one rail action with filesystem side effects). Inset horizontally so
   the line reads as a divider between groups, not as another rail edge. */
QToolBar#activityRail::separator {
    background: {outline-strong};
    height: 1px;
    margin: 7px 9px;
}

/* ---- splitters -------------------------------------------------------- */
QSplitter::handle {
    background: {surface-0};
}
QSplitter::handle:horizontal {
    width: 3px;
}
QSplitter::handle:vertical {
    height: 3px;
}
QSplitter::handle:hover {
    background: {accent-interactive};
}

/* ---- item views ------------------------------------------------------- */
QListWidget, QTreeWidget, QTreeView, QListView {
    background: {surface-1};
    color: {text-primary};
    border: none;
    outline: none;
}
QListWidget::item, QTreeWidget::item {
    padding: 3px 4px;
}
QListWidget::item:hover, QTreeWidget::item:hover {
    background: {surface-hover};
}
QListWidget::item:selected, QTreeWidget::item:selected {
    background: {surface-hover};
    color: {text-primary};
}

/* ---- tabs (bottom Trace/Problems, Inspector) -------------------------- */
QTabWidget::pane {
    background: {surface-1};
    border: none;
    border-top: 1px solid {outline};
}
QTabBar::tab {
    background: transparent;
    color: {text-secondary};
    padding: 5px 14px;
    border: none;
    border-bottom: 1px solid transparent;
}
QTabBar::tab:hover {
    color: {text-primary};
}
QTabBar::tab:selected {
    color: {text-primary};
    border-bottom: 1px solid {accent-interactive};
}

/* ---- editor-group tabs (VSCode editor tabs, DESIGN decision 4) -------- */
QTabBar#editorTabBar {
    background: {surface-0};
    qproperty-drawBase: 0;
}
QTabBar#editorTabBar::tab {
    background: {surface-0};
    color: {text-secondary};
    padding: 5px 8px 5px 8px;
    border: none;
    border-right: 1px solid {outline};
    border-top: 1px solid transparent;
    border-bottom: none;
}
QTabBar#editorTabBar::tab:hover {
    background: {surface-hover};
    color: {text-primary};
}
QTabBar#editorTabBar::tab:selected {
    background: {surface-1};
    color: {text-primary};
    border-top: 1px solid {accent-interactive};
}
QTabBar#editorTabBar QToolButton {
    background: transparent;
    border: none;
    border-radius: 3px;
}
QTabBar#editorTabBar QToolButton:hover {
    background: {surface-hover};
}

/* ---- tables ----------------------------------------------------------- */
QTableWidget, QTableView {
    background: {surface-1};
    color: {text-primary};
    border: none;
    gridline-color: {outline};
}
QHeaderView::section {
    background: {surface-0};
    color: {text-secondary};
    border: none;
    border-bottom: 1px solid {outline};
    padding: 4px 8px;
}

/* ---- inputs ----------------------------------------------------------- */
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
    background: {surface-2};
    color: {text-primary};
    border: 1px solid {outline};
    border-radius: 3px;
    padding: 3px 6px;
    selection-background-color: {accent-brand};
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border: 1px solid {outline-focus};
}
QSpinBox, QDoubleSpinBox {
    padding-right: 20px;
}
QSpinBox::up-button, QDoubleSpinBox::up-button {
    subcontrol-origin: border;
    subcontrol-position: top right;
    width: 17px;
    background: {surface-2};
    border-left: 1px solid {outline};
    border-bottom: 1px solid {outline};
    border-top-right-radius: 3px;
}
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover {
    background: {surface-hover};
}
QSpinBox::up-button:pressed, QDoubleSpinBox::up-button:pressed {
    background: {accent-brand};
}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    width: 7px;
    height: 5px;
}
QSpinBox::down-button, QDoubleSpinBox::down-button {
    subcontrol-origin: border;
    subcontrol-position: bottom right;
    width: 17px;
    background: {surface-2};
    border-left: 1px solid {outline};
    border-bottom-right-radius: 3px;
}
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
    background: {surface-hover};
}
QSpinBox::down-button:pressed, QDoubleSpinBox::down-button:pressed {
    background: {accent-brand};
}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    width: 7px;
    height: 5px;
}
QComboBox QAbstractItemView {
    background: {surface-1};
    color: {text-primary};
    border: 1px solid {outline};
    selection-background-color: {accent-brand};
}

/* ---- buttons ---------------------------------------------------------- */
QPushButton {
    background: {surface-hover};
    color: {text-primary};
    border: 1px solid {outline-strong};
    border-radius: 3px;
    padding: 4px 12px;
    min-height: 20px;
}
QCheckBox {
    color: {text-primary};
    min-height: 18px;
}
QPushButton:hover {
    background: {surface-pressed};
    border: 1px solid {outline-hover};
}
/* QSS replaces the native sunken-bevel press feedback wholesale, so the
   :pressed state must carry the whole signal itself -- Ordo Blue, matching
   :checked, unmistakable even on a momentary click. */
QPushButton:pressed {
    background: {accent-brand};
    border: 1px solid {accent-brand};
    color: {text-on-accent};
}
QPushButton:checked {
    background: {accent-brand};
    color: {text-on-accent};
    border: 1px solid {accent-brand};
}
QPushButton:disabled {
    background: {surface-2};
    color: {text-disabled};
    border: 1px solid {outline};
}

/* ---- scrollbars (VSCode-thin, no arrow buttons) ----------------------- */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 0;
}
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 0;
}
QScrollBar::handle {
    background: {outline-strong};
    border-radius: 5px;
    min-height: 24px;
    min-width: 24px;
}
QScrollBar::handle:hover {
    background: {outline-hover};
}
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0;
    height: 0;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: transparent;
}

/* ---- status bar / tooltips / dialogs ---------------------------------- */
QStatusBar {
    background: {surface-1};
    color: {text-secondary};
    border-top: 1px solid {outline};
}
QStatusBar QLabel {
    color: {text-secondary};
}
QToolTip {
    background: {surface-1};
    color: {text-primary};
    border: 1px solid {outline};
    padding: 3px 6px;
}
QDialog, QMessageBox, QInputDialog {
    background: {surface-1};
}
)qss");

    return design::resolveRoles(kTemplate);
}

}  // namespace app::theme
