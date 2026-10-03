#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>

namespace app::icons {

// Activity-rail glyphs, rendered from the vendored SVGs in resources/icons/
// and tinted at runtime to the rail's state colors.
enum class IconId {
    Design,       // pencil -- edit the machine
    Simulate,     // beaker -- run it under the interpreter
    Run,          // filled play triangle
    Pause,        // two bars
    Reset,        // counter-clockwise arc + arrowhead
    Fit,          // four viewport corner brackets
    GenerateCpp,  // document + out-arrow (write generated files)
    Hook,         // hook curve glyph (named hook)
    Expr,         // expression glyph (inline expression)
    Assign,       // = / assignment glyph (assign action)
    Raise,        // raise glyph (raise action)
    SendTo,       // right arrow glyph (sendTo action)
    SendParent,   // upward arrow glyph (sendParent action)
};

// A ready-to-use rail icon: neutral gray in the Normal/Off state, white in
// the On (checked) state, dimmed when disabled -- the three states the
// rail's checkable mode toggle and enable-gated transport actions need.
QIcon icon(IconId id);

// The running-indicator circle an editor tab shows left of its title
// (concept PNG's tab dots): filled green while the machine is actively
// simulating, dim gray otherwise.
QIcon statusDot(bool active);

// A quiet gray x for the editor tabs' close buttons -- replaces the
// platform style's loud default.
QIcon closeGlyph();

// A six-state icon (Normal/Active/Disabled x Off/On) for any vendored slug in
// resources/icons/ (e.g. "check", "warning"), same assembly as icon().
QIcon asset(const char* slug);

// One tinted pixmap for QPainter call sites. `size` is LOGICAL; the SVG is
// rendered at size x devicePixelRatio and the pixmap carries that ratio.
// Pass the target's own ratio (devicePixelRatioF()): 1.0 looks jagged at 150%.
QPixmap assetPixmap(const char* slug, int size, const QColor& tint, qreal devicePixelRatio);

// The application icon (title bar, taskbar, Alt+Tab): the brand mark in its own
// two colors (never tinted), rendered at every size Windows asks for.
QIcon appIcon();

}  // namespace app::icons
