#pragma once

#include <QString>

class QApplication;

namespace app::theme {

// App-wide dark theme: Fusion style, a dark QPalette (covers widgets no
// stylesheet rule names, e.g. dialogs and combo popups) and one application
// stylesheet. Call once from main() before any widget is constructed.
// Per-widget stylesheets still win where set.
void apply(QApplication& application);

// The stylesheet half of apply(); usable without a QApplication.
QString appStyleSheet();

}  // namespace app::theme
