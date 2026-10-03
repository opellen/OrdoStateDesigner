#pragma once

// The design system's named colors, spacing and type-ramp fragments, shared by every
// screen that draws chrome. Includes no project headers, only Qt primitives. A value shared
// by 2+ screens belongs here; one screen's own layout constant stays in that screen.

#include <QColor>
#include <QString>

namespace app {
namespace design {

// Surfaces (elevation, darkest to brightest).
constexpr const char* kSurface0 = "#0F172A";        // window/splitter background
constexpr const char* kSurface1 = "#111827";        // panel/chrome background
constexpr const char* kSurface2 = "#151A21";        // fields/cards atop a panel
constexpr const char* kSurfaceHover = "#1F2933";    // hover state layer
constexpr const char* kSurfacePressed = "#263140";  // pressed state layer

// Outline.
constexpr const char* kOutline = "#1F2933";        // panel/field borders, default
constexpr const char* kOutlineStrong = "#2A3441";  // dividers, emphasized borders
constexpr const char* kOutlineFocus = "#3B82F6";   // focus ring -- the only one
constexpr const char* kOutlineHover = "#3B4A5C";   // border of a hovered control, scroll-thumb hover

// Text (3-step hierarchy: one visible step between values and labels).
constexpr const char* kTextPrimary = "#D9DBE8";    // values, body -- what the user reads
constexpr const char* kTextSecondary = "#C5C8D4";  // labels, chevrons -- one step below kTextPrimary
constexpr const char* kTextDisabled = "#5A6272";   // disabled, placeholder, empty-list copy
constexpr const char* kTextOnAccent = "#FFFFFF";   // text/glyphs on an accent fill -- not a hierarchy step

// Accents.
constexpr const char* kAccentBrand = "#2D63D7";        // primary button fill
constexpr const char* kAccentInteractive = "#3B82F6";  // selection, focus, active
constexpr const char* kAccentPulse = "#38BDF8";        // transient live/run emphasis ONLY
constexpr const char* kDanger = "#EF4444";             // destructive, error

// Icon tints (3-state: normal/active/disabled). kIconNormal is kTextPrimary; the other
// two are icon-only values, not text roles.
constexpr const char* kIconNormal = kTextPrimary;  // #D9DBE8
constexpr const char* kIconActive = "#E5E9F0";     // On-state tint
constexpr const char* kIconDisabled = "#4A5261";   // Disabled tint

// Live-simulation status dot: marks a running machine/state (icons::statusDot(), the
// MACHINES tree row dot). Dots only, never text or backgrounds.
constexpr const char* kStatusLive = "#22C55E";  // status-live: simulating machine / live state
constexpr const char* kStatusIdle = "#5A5F6E";  // status-idle: machine not simulating

// Feedback. Text/borders take success/danger, backgrounds the -surface role; no second
// red for error copy.
constexpr const char* kSuccess = "#34D399";
constexpr const char* kSuccessSurface = "#122118";
constexpr const char* kDangerSurface = "#261B1E";
constexpr const char* kWarning = "#F59E0B";

// Alpha roles below use Qt's #AARRGGBB form (alpha FIRST). Read them through
// color() for QPainter and qss() for style sheets (qss() turns them into
// rgba(), which every QSS property accepts).

// Overlays.
constexpr const char* kSurfaceOverlay = "#EB111827";        // floating panel over the canvas (minimap)
constexpr const char* kScrim = "#AF0F172A";                 // modal progress backdrop
constexpr const char* kShadow = "#B4000000";                // drop shadow of floating items
constexpr const char* kAccentInteractiveSoft = "#B43B82F6"; // drop-zone / code-lens border
constexpr const char* kDropZoneFill = "#373B82F6";          // editor-area drop target fill

// Canvas. Selection reuses kAccentInteractive and the live-run pulse reuses kAccentPulse.
constexpr const char* kCanvasBackground = "#0F172A";
constexpr const char* kCanvasGrid = "#21222C";
constexpr const char* kCanvasHighlight = "#38BDF8";  // hover/port/drop/wire-drag/bend-handle feedback
constexpr const char* kCanvasHoverOverlay = "#18FFFFFF";
constexpr const char* kNodeFill = "#1F2933";
constexpr const char* kNodeBorder = "#26FFFFFF";
constexpr const char* kNodeBorderHover = "#4DFFFFFF";
constexpr const char* kNodeName = "#F7F8FA";
constexpr const char* kNodeBody = "#82FFFFFF";
constexpr const char* kNodeActiveGlow = "#733B82F6";
constexpr const char* kContainerFill = "#131B2F";
constexpr const char* kContainerBorder = "#263049";
constexpr const char* kContainerHeaderFill = "#182238";
constexpr const char* kContainerHeaderText = "#C7CEDB";
constexpr const char* kFrameFill = "#A8131B2F";
constexpr const char* kNoteText = "#C2C8D6";
constexpr const char* kNotePlaceholder = "#6E7687";
constexpr const char* kNoteHoverFill = "#0EFFFFFF";
constexpr const char* kNoteFold = "#22FFFFFF";
constexpr const char* kEdge = "#464A62";
constexpr const char* kEdgeSelected = "#FFFFFF";
constexpr const char* kEdgeLabelFill = "#E61F2933";
constexpr const char* kEdgeLabelFillBlank = "#AA1F2933";
constexpr const char* kEdgeLabelText = "#F7F8FA";
constexpr const char* kEdgeLabelTextBlank = "#6EFFFFFF";
constexpr const char* kBendHandleBorder = "#DCFFFFFF";
constexpr const char* kFireableFill = "#2563EB";
constexpr const char* kWaitingRing = "#8C38BDF8";
constexpr const char* kWaitingText = "#DC38BDF8";
constexpr const char* kInitialMarker = "#D9DBE8";
constexpr const char* kGuideAlignment = "#FF0066";
constexpr const char* kGuideRouting = "#B4A0AFC8";
constexpr const char* kOnboardingHint = "#607080";
constexpr const char* kBreakpoint = "#EF4444";
constexpr const char* kBreakpointHit = "#F59E0B";
constexpr const char* kBreakpointRing = "#78EF4444";
constexpr const char* kBreakpointFill = "#3CEF4444";
constexpr const char* kBreakpointHalo = "#B4F59E0B";
constexpr const char* kCapsuleFill = "#F01F2933";
constexpr const char* kCapsuleDivider = "#28FFFFFF";
constexpr const char* kCapsuleLabelDisabled = "#5AF7F8FA";
constexpr const char* kLensFill = "#F20F172A";
constexpr const char* kLensHeaderFill = "#EB1E293B";
constexpr const char* kLensSeparator = "#C8334155";
constexpr const char* kLensClose = "#94A3B8";
constexpr const char* kLensCloseHover = "#F8FAFC";
constexpr const char* kLensCloseHoverFill = "#50EF4444";

// Code and logic: one syntax palette for every code display, one logic palette for
// everything that shows the machine's logic.
constexpr const char* kCodeKeyword = "#C084FC";  // keywords, preprocessor
constexpr const char* kCodeType = "#38BDF8";
constexpr const char* kCodeString = "#34D399";
constexpr const char* kCodeNumber = "#34D399";
constexpr const char* kCodeComment = "#64748B";
constexpr const char* kCodePunctuation = "#94A3B8";
constexpr const char* kCodeIdentifier = "#F1F5F9";
constexpr const char* kLogicVariable = "#38BDF8";  // context references, assign targets, guards
constexpr const char* kLogicLiteral = "#34D399";
constexpr const char* kLogicAction = "#34D399";
constexpr const char* kLogicRaise = "#C084FC";
constexpr const char* kLogicSend = "#818CF8";
constexpr const char* kLogicAlways = "#FBBF24";
constexpr const char* kLogicWildcard = "#FB923C";
constexpr const char* kLogicAfter = "#FB7185";
constexpr const char* kLogicHook = "#64748B";      // "entry /" and "exit /" lead-ins

// Element palette: the user-assignable node/note colors, part of saved documents' appearance.
constexpr const char* kElementRed = "#EF4444";
constexpr const char* kElementOrange = "#F97316";
constexpr const char* kElementYellow = "#EAB308";
constexpr const char* kElementGreen = "#22C55E";
constexpr const char* kElementBlue = "#0EA5E9";
constexpr const char* kElementPurple = "#A855F7";
constexpr const char* kElementPink = "#EC4899";
constexpr const char* kElementGray = "#94A3B8";
constexpr const char* kElementDefaultSwatch = "#334155";

// Accessors. color() parses both #RRGGBB and #AARRGGBB; qss() hands an opaque
// token back unchanged and spells an alpha token as rgba(), so a style sheet
// never has to know which kind it got.
inline QColor color(const char* token) { return QColor(QString::fromLatin1(token)); }
inline QString qss(const char* token) {
    const QColor c = color(token);
    if (c.alpha() == 255) {
        return QString::fromLatin1(token);
    }
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

// Style-sheet templates name colors by role ("{surface-2}", "{outline-focus}", ...).
// resolveRoles() fills every placeholder from this table (alpha roles arrive as rgba()).
struct RolePlaceholder {
    const char* placeholder;
    const char* token;
};
inline constexpr RolePlaceholder kRolePlaceholders[] = {
    {"{surface-0}", kSurface0}, {"{surface-1}", kSurface1}, {"{surface-2}", kSurface2},
    {"{surface-hover}", kSurfaceHover}, {"{surface-pressed}", kSurfacePressed},
    {"{outline}", kOutline}, {"{outline-strong}", kOutlineStrong}, {"{outline-focus}", kOutlineFocus},
    {"{outline-hover}", kOutlineHover},
    {"{text-primary}", kTextPrimary}, {"{text-secondary}", kTextSecondary}, {"{text-disabled}", kTextDisabled},
    {"{text-on-accent}", kTextOnAccent},
    {"{accent-brand}", kAccentBrand}, {"{accent-interactive}", kAccentInteractive}, {"{accent-pulse}", kAccentPulse},
    {"{danger}", kDanger}, {"{danger-surface}", kDangerSurface}, {"{success}", kSuccess},
    {"{success-surface}", kSuccessSurface}, {"{warning}", kWarning},
    {"{icon-normal}", kIconNormal}, {"{icon-active}", kIconActive}, {"{icon-disabled}", kIconDisabled},
    {"{status-live}", kStatusLive}, {"{status-idle}", kStatusIdle},
    {"{surface-overlay}", kSurfaceOverlay}, {"{scrim}", kScrim}, {"{shadow}", kShadow},
    {"{accent-interactive-soft}", kAccentInteractiveSoft}, {"{drop-zone-fill}", kDropZoneFill},
    {"{code-keyword}", kCodeKeyword}, {"{code-type}", kCodeType}, {"{code-string}", kCodeString},
    {"{code-number}", kCodeNumber}, {"{code-comment}", kCodeComment}, {"{code-punctuation}", kCodePunctuation},
    {"{code-identifier}", kCodeIdentifier},
    {"{logic-variable}", kLogicVariable}, {"{logic-literal}", kLogicLiteral}, {"{logic-action}", kLogicAction},
    {"{logic-raise}", kLogicRaise}, {"{logic-send}", kLogicSend}, {"{logic-always}", kLogicAlways},
    {"{logic-wildcard}", kLogicWildcard}, {"{logic-after}", kLogicAfter}, {"{logic-hook}", kLogicHook},
    {"{node-border}", kNodeBorder}, {"{element-default-swatch}", kElementDefaultSwatch},
};

inline QString resolveRoles(QString qssTemplate) {
    for (const RolePlaceholder& role : kRolePlaceholders) {
        qssTemplate.replace(QLatin1String(role.placeholder), qss(role.token));
    }
    // A "{name}" left behind is a missing role or a typo. Real QSS rule bodies contain a
    // space or ':', so they are never mistaken for a placeholder.
    for (qsizetype open = qssTemplate.indexOf(QLatin1Char('{')); open >= 0;
         open = qssTemplate.indexOf(QLatin1Char('{'), open + 1)) {
        qsizetype i = open + 1;
        while (i < qssTemplate.size() && (qssTemplate[i].isLower() || qssTemplate[i].isDigit() ||
                                          qssTemplate[i] == QLatin1Char('-'))) {
            ++i;
        }
        if (i > open + 1 && i < qssTemplate.size() && qssTemplate[i] == QLatin1Char('}')) {
            qWarning("design::resolveRoles: unresolved role placeholder %s",
                     qPrintable(qssTemplate.mid(open, i - open + 1)));
        }
    }
    return qssTemplate;
}

// Spacing (4px grid) and the row constants shared by every form/row builder.
constexpr int kSpace1 = 4;
constexpr int kSpace2 = 8;
constexpr int kSpace3 = 12;
constexpr int kSpace4 = 16;
constexpr int kRowHeight = 28;
// Shared component geometry: fields/cards/buttons share one corner radius, borders and
// dividers one hairline width, and a selected row's left bar is 2px wide.
constexpr int kRadius = 4;
constexpr int kHairline = 1;
constexpr int kSelectionBarWidth = 2;
// The panel's label column is a fixed width (about 40% of the ~300px panel) so the value
// column starts at the same x in every form/row builder.
constexpr int kRowLabelColumnWidth = 96;

// Section-header geometry: the icon is the left anchor (bright, 16px), the chevron sits at
// the far right as a small dim state indicator.
constexpr int kSectionIconX = 8;             // 16px tinted icons::asset() pixmap, the row's left anchor
constexpr int kSectionIconSize = 16;
constexpr int kSectionTitleX = 34;           // after icon (8 + 16 + 10); 8 when a section has no icon
constexpr int kSectionChevronRightInset = 16;  // arrow centered this far from the RIGHT edge, ~8px span
// The header owns the gap between two sections' hairlines, split evenly above and below
// its row, so a collapsed header's content is centered between hairlines.
constexpr int kSectionHeaderPad = kSpace2;
// Row-gutter icon buttons: a fixed slot holding a tinted SVG glyph, shared by row actions
// and section-header actions.
constexpr int kRowIconButtonSize = 22;
constexpr int kRowIconGlyphSize = 14;

// Type ramp: ready-made QSS property fragments (color + size + weight), not full rules.
// QSS has no text-transform/letter-spacing, so the section title's uppercase and spacing
// are applied to the text and QFont directly (see applyTypeSectionFont() in
// inspector_panel_internal.h). Section titles are one brightness step above labels.
// The ramp's one size, also the application's base font (theme::apply()).
constexpr int kTypeSizePx = 12;
inline const QString kTypeSection = QStringLiteral("color: %1; font-size: %2px; font-weight: 600;")
                                         .arg(QString::fromLatin1(kTextPrimary))
                                         .arg(kTypeSizePx);
inline const QString kTypeLabel = QStringLiteral("color: %1; font-size: %2px; font-weight: 400;")
                                       .arg(QString::fromLatin1(kTextSecondary))
                                       .arg(kTypeSizePx);
inline const QString kTypeValue = QStringLiteral("color: %1; font-size: %2px; font-weight: 400;")
                                       .arg(QString::fromLatin1(kTextPrimary))
                                       .arg(kTypeSizePx);
inline const QString kTypeMono = QStringLiteral(
    "color: %1; font-size: %2px; font-weight: 400; font-family: Consolas, monospace;")
    .arg(QString::fromLatin1(kTextPrimary))
    .arg(kTypeSizePx);

// Button classes. Primary (filled kAccentBrand, white text) is capped at one per screen.
// Secondary (transparent fill, kOutline border) is deliberately quieter than the app's
// default QPushButton style. Each carries its own :disabled rule so it does not fall
// through to theme.cpp's generic one.
inline const QString kButtonPadding = QStringLiteral("%1px %2px").arg(kSpace1).arg(kSpace3);

inline const QString kPrimaryButtonStyle =
    resolveRoles(QStringLiteral(
                     "QPushButton { background: %1; color: {text-on-accent}; border: %2px solid %1; "
                     "border-radius: %3px; padding: %4; font-weight: 600; }"
                     "QPushButton:hover:enabled { background: %5; border-color: %5; }"
                     "QPushButton:pressed:enabled { background: %1; border-color: %1; }"
                     "QPushButton:disabled { background: %6; color: %7; border: %2px solid %6; }"))
        .arg(QString::fromLatin1(kAccentBrand))
        .arg(kHairline)
        .arg(kRadius)
        .arg(kButtonPadding)
        .arg(QString::fromLatin1(kAccentInteractive))
        .arg(QString::fromLatin1(kSurface2))
        .arg(QString::fromLatin1(kTextDisabled));
inline const QString kSecondaryButtonStyle =
    QStringLiteral(
        "QPushButton { background: transparent; color: %1; border: %2px solid %3; border-radius: %4px; "
        "padding: %5; font-weight: 400; }"
        "QPushButton:hover:enabled { background: %6; color: %7; }"
        "QPushButton:pressed:enabled { background: %8; }"
        "QPushButton:disabled { background: transparent; color: %9; border: %2px solid %3; }")
        .arg(QString::fromLatin1(kTextSecondary))
        .arg(kHairline)
        .arg(QString::fromLatin1(kOutline))
        .arg(kRadius)
        .arg(kButtonPadding)
        .arg(QString::fromLatin1(kSurfaceHover))
        .arg(QString::fromLatin1(kTextPrimary))
        .arg(QString::fromLatin1(kSurfacePressed))
        .arg(QString::fromLatin1(kTextDisabled));

}  // namespace design
}  // namespace app
