#pragma once

// Helpers shared by the Inspector's two translation units (the panel and the adapter), as inline
// definitions in a named detail namespace; both .cpp files say `using namespace inspector_detail;`.
// Not part of the Inspector's public surface. Also holds the collapsible-section builder shared with
// LogicPanel, whose header includes this one, so nothing here may assume InspectorPanel is complete.
// Design tokens live in constants/design_tokens.h; this file keeps composed component styles and widgets.


#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringListModel>
#include <QSyntaxHighlighter>
#include <QTabWidget>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "constants/design_tokens.h"
#include "infra/code_generator.h"
#include "infra/expression.h"
#include "infra/logic_inventory.h"
#include "view/geometry/element_colors.h"
#include "view/shell/icons.h"  // SectionHeaderButton's identity icons


namespace app {
namespace inspector_detail {

// Design tokens (kSurface0, kTypeLabel, kRowLabelColumnWidth, ...) resolve unqualified or via inspector_detail::.
using namespace design;

// Section-title font as a real QFont (12px/600): SectionHeaderButton paints with font(), which QSS cannot reach.
inline void applyTypeSectionFont(QWidget* widget) {
    QFont font = widget->font();
    font.setPixelSize(kTypeSizePx);
    font.setWeight(QFont::DemiBold);
    widget->setFont(font);
}

// Entry actions: displayed one per line; input accepts newline or comma separators, trimmed, empties dropped.
inline QStringList splitEntryActions(const QString& text) {
    QStringList result;
    const QStringList rawTokens = text.split(QRegularExpression(QStringLiteral("[\\n,]")), Qt::SkipEmptyParts);
    for (const QString& raw : rawTokens) {
        const QString trimmed = raw.trimmed();
        if (!trimmed.isEmpty()) {
            result.push_back(trimmed);
        }
    }
    return result;
}

inline QString joinEntryActions(const QStringList& actions) { return actions.join(QStringLiteral("\n")); }

// Tags: one comma-separated line, trimmed, empties dropped.
inline QStringList splitTags(const QString& text) {
    QStringList result;
    for (const QString& raw : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed = raw.trimmed();
        if (!trimmed.isEmpty()) {
            result.push_back(trimmed);
        }
    }
    return result;
}

inline QString joinTags(const QStringList& tags) { return tags.join(QStringLiteral(", ")); }

inline QString formatPosition(QPointF pos) {
    return QStringLiteral("%1, %2").arg(QString::number(pos.x(), 'f', 0), QString::number(pos.y(), 'f', 0));
}

inline const QString kCollapsibleSectionPlaceholderStyle =
    QStringLiteral("color: %1; padding: 2px 20px 8px 20px;").arg(QString::fromLatin1(kTextDisabled));

// Keeps the header text and its sectionBaseTitle property in sync; route every header text update
// (e.g. count refreshes) here rather than calling setText(), which would let the two drift.
inline void setSectionHeaderTitle(QToolButton* header, const QString& baseTitle) {
    header->setProperty("sectionBaseTitle", baseTitle);
    header->setText(baseTitle);
}

// Paints its own label: neither QSS `text-align` nor TextBesideIcon left-aligns a text-only
// QToolButton under this app's style. Checkable/clickable behavior is still QToolButton's.
// Layout: bright 16px icon at the left, dominant title, small dim chevron at the far right.

class SectionHeaderButton : public QToolButton {
public:
    explicit SectionHeaderButton(QWidget* parent = nullptr) : QToolButton(parent) {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);  // repaint on hover without a stylesheet driving it
        setToolButtonStyle(Qt::ToolButtonTextOnly);  // sizeHint() needs this for the font-metrics row height
        // Expanding: the default policy sizes the button to its text, so the header must be forced to span the row.
        setSizePolicy(QSizePolicy::Expanding, sizePolicy().verticalPolicy());
        // Room for the drawn chevron/icon plus the symmetric pad.
        setMinimumHeight(kRowHeight + 2 * kSectionHeaderPad);
    }

    // The band the icon/title/chevron center in (everything below the top hairline).
    // Public so probes measure the same rect paint uses.
    QRect contentRect() const { return rect().adjusted(0, kHairline, 0, 0); }

    // Identity icon: a slug into icons::asset()'s set, or nullptr for none. Static string literals only; not owned.
    void setIconSlug(const char* slug) {
        iconSlug_ = slug;
        update();
    }
    const char* iconSlug() const { return iconSlug_; }

    // A non-collapsible header shows no chevron but keeps the icon/title geometry.
    void setChevronVisible(bool visible) {
        chevronVisible_ = visible;
        update();
    }

    // A section-level action in the header's right gutter, left of the chevron. It is a child of the
    // header so the hairline spans the row. Expects a fixed-size button (makeSectionHeaderAction());
    // the header takes ownership.
    void setTrailingAction(QToolButton* action) {
        trailingAction_ = action;
        action->setParent(this);
        action->show();
        placeTrailingAction();
        update();
    }
    QToolButton* trailingAction() const { return trailingAction_; }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QToolButton::resizeEvent(event);
        placeTrailingAction();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const bool hovered = isEnabled() && underMouse();
        if (hovered) {
            painter.fillRect(rect(), QColor(QString::fromLatin1(kSurfaceHover)));
        }
        // Top hairline, filled as a rect rather than drawLine(0, 0, ...): a 1px pen centered on y=0
        // can rasterize outside the widget's clip at 150% scale and vanish.
        painter.fillRect(QRect(0, 0, width(), kHairline), QColor(QString::fromLatin1(kOutlineStrong)));

        // Icon and title share one brightness; only the chevron stays dim.
        const QColor titleColor = !isEnabled() ? QColor(QString::fromLatin1(kTextDisabled))
                                               : QColor(QString::fromLatin1(kTextPrimary));
        const QColor chevronColor = !isEnabled() ? QColor(QString::fromLatin1(kTextDisabled))
                                   : hovered     ? titleColor
                                                 : QColor(QString::fromLatin1(kTextSecondary));
        const QRect content = contentRect();
        const int cy = content.center().y();

        int textX = kSectionIconX;
        if (iconSlug_ != nullptr) {
            // assetPixmap caches per slug, so per-paint calls are composition cost only.
            painter.drawPixmap(kSectionIconX, cy - kSectionIconSize / 2,
                               icons::assetPixmap(iconSlug_, kSectionIconSize, titleColor, devicePixelRatioF()));
            textX = kSectionTitleX;
        }

        painter.setFont(font());  // 12px/600 via applyTypeSectionFont()
        painter.setPen(titleColor);
        const int textRightInset = trailingAction_ != nullptr ? width() - trailingAction_->x() + kSpace2 : kSectionTitleX;
        painter.drawText(content.adjusted(textX, 0, -textRightInset, 0), Qt::AlignLeft | Qt::AlignVCenter, text());

        if (chevronVisible_) {
            const int cx = width() - kSectionChevronRightInset;
            painter.setRenderHint(QPainter::Antialiasing, true);
            QPen chevronPen(chevronColor, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(chevronPen);
            QPainterPath arrow;
            if (isChecked()) {  // expanded: tailless down arrow
                arrow.moveTo(cx - 4, cy - 2);
                arrow.lineTo(cx, cy + 2.5);
                arrow.lineTo(cx + 4, cy - 2);
            } else {  // collapsed: tailless right arrow
                arrow.moveTo(cx - 2, cy - 4);
                arrow.lineTo(cx + 2.5, cy);
                arrow.lineTo(cx - 2, cy + 4);
            }
            painter.drawPath(arrow);
            painter.setRenderHint(QPainter::Antialiasing, false);
        }
    }

private:
    void placeTrailingAction() {
        if (trailingAction_ == nullptr) {
            return;
        }
        // kSpace4 left of the chevron's center, leaving a kSpace3 gap to the arrow.
        QRect slot(QPoint(0, 0), trailingAction_->size());
        slot.moveCenter(QPoint(0, contentRect().center().y()));
        slot.moveRight(width() - kSectionChevronRightInset - kSpace4);
        trailingAction_->setGeometry(slot);
    }

    const char* iconSlug_ = nullptr;
    bool chevronVisible_ = true;
    QToolButton* trailingAction_ = nullptr;
};

// The disclosure idiom shared by LogicPanel and InspectorPanel's Machine tab: a checkable header,
// a body whose visibility follows the header, and a container wrapping both.
struct CollapsibleSection {
    SectionHeaderButton* header = nullptr;  // the checkable disclosure toggle (+ its trailing action, if any)
    QWidget* body = nullptr;
    // Wraps header + body so a probe can address the whole section.
    QWidget* container = nullptr;
};

// `placeholderText` is optional: an empty string skips the placeholder label. Otherwise body's
// layout item 0 is that QLabel, ready for the caller to grab and toggle.
inline CollapsibleSection buildCollapsibleSection(QWidget* parent, const QString& title,
                                                   const QString& placeholderText, bool enabled,
                                                   bool defaultExpanded = true,
                                                   const char* iconSlug = nullptr) {
    CollapsibleSection section;
    auto* headerButton = new SectionHeaderButton(parent);
    headerButton->setCheckable(true);
    headerButton->setChecked(defaultExpanded);
    headerButton->setEnabled(enabled);
    headerButton->setIconSlug(iconSlug);
    section.header = headerButton;
    setSectionHeaderTitle(section.header, title);
    applyTypeSectionFont(section.header);  // 12px/600 -- the paintEvent renders with font(), not QSS
    // A caller's section action goes into the header (setTrailingAction()), never beside it.

    section.body = new QWidget(parent);
    section.body->setVisible(defaultExpanded);
    auto* bodyLayout = new QVBoxLayout(section.body);
    // The gap below an expanded body is its bottom margin; a hidden body takes it along.
    bodyLayout->setContentsMargins(0, 0, 0, kSpace4);
    bodyLayout->setSpacing(kSpace1);
    if (!placeholderText.isEmpty()) {
        auto* placeholder = new QLabel(placeholderText, section.body);
        placeholder->setStyleSheet(kCollapsibleSectionPlaceholderStyle);
        // Word-wrapped so the minimumSizeHint is the longest word: a section must not dictate the side bar's width.
        placeholder->setWordWrap(true);
        bodyLayout->addWidget(placeholder);
    }

    section.container = new QWidget(parent);
    auto* containerLayout = new QVBoxLayout(section.container);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    // Header-to-first-row is kSpace3: the header's bottom pad plus this spacing, which only shows
    // while `body` is expanded (a hidden item adds no spacing in Qt box layouts).
    containerLayout->setSpacing(kSpace3 - kSectionHeaderPad);
    containerLayout->addWidget(section.header);
    containerLayout->addWidget(section.body);

    // The header's checked state is the section's open state (view-local, not persisted); the chevron is painted from isChecked().
    QWidget* body = section.body;
    QObject::connect(section.header, &QToolButton::toggled, parent,
                     [body](bool expanded) { body->setVisible(expanded); });
    return section;
}

// Scoped to `::title`: a bare declaration would cascade its font-size into unstyled children,
// such as an invocation entry's Done/Error buttons.
inline const QString kGroupBoxTitleStyle =
    QStringLiteral("QGroupBox::title { %1 }").arg(kTypeSection);
inline const QString kChipStyle = QStringLiteral("color: %1; background: %2; border-radius: 4px; padding: 2px 8px;")
                                       .arg(QString::fromLatin1(kAccentPulse), QString::fromLatin1(kSurfaceHover));

// Row-gutter icon buttons: a fixed slot for a tinted SVG glyph, transparent when idle, with a
// surface-hover fill on hover and :checked (the glyph's own tint distinguishes active from hovered).
inline const QString kRowIconButtonStyle =
    QStringLiteral(
        "QToolButton { border: none; border-radius: 3px; background: transparent; padding: 0px; }"
        "QToolButton:hover { background: %1; }"
        "QToolButton:checked { background: %1; }")
        .arg(QString::fromLatin1(kSurfaceHover));

// A section header's trailing action (Context/Types "+"): the same row-gutter
// icon button as the rows' own trace/delete, attached inside the header.
inline QToolButton* makeSectionHeaderAction(const CollapsibleSection& section, const char* iconSlug,
                                            const QString& toolTip) {
    auto* button = new QToolButton();
    button->setIcon(icons::asset(iconSlug));
    button->setIconSize(QSize(kRowIconGlyphSize, kRowIconGlyphSize));
    button->setFixedSize(kRowIconButtonSize, kRowIconButtonSize);
    button->setToolTip(toolTip);
    button->setCursor(Qt::PointingHandCursor);
    button->setStyleSheet(kRowIconButtonStyle);
    section.header->setTrailingAction(button);
    return button;
}

// Minimal C++ highlighter for the Code tab's read-only preview: keywords, preprocessor, strings,
// numbers, and comments (including /* */ block state).
class CppHighlighter : public QSyntaxHighlighter {
public:
    explicit CppHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {
        // Shared syntax palette; keyword and preprocessor share code-keyword.
        keywordFormat_.setForeground(design::color(kCodeKeyword));
        preprocessorFormat_.setForeground(design::color(kCodeKeyword));
        stringFormat_.setForeground(design::color(kCodeString));
        numberFormat_.setForeground(design::color(kCodeNumber));
        commentFormat_.setForeground(design::color(kCodeComment));

        static const char* kKeywords[] = {
            "alignas",  "auto",     "bool",     "break",    "case",     "catch",   "char",      "class",
            "const",    "constexpr", "continue", "default",  "delete",   "do",      "double",    "else",
            "enum",     "explicit", "false",    "final",    "float",    "for",     "if",        "inline",
            "int",      "namespace", "new",      "nullptr",  "operator", "override", "private",  "protected",
            "public",   "return",   "static",   "struct",   "switch",   "template", "this",     "true",
            "typename", "unsigned", "using",    "virtual",  "void",     "while",
        };
        QStringList words;
        for (const char* keyword : kKeywords) {
            words.push_back(QString::fromLatin1(keyword));
        }
        keywordPattern_ = QRegularExpression(QStringLiteral("\\b(%1)\\b").arg(words.join(QLatin1Char('|'))));
    }

protected:
    void highlightBlock(const QString& text) override {
        static const QRegularExpression preprocessor(QStringLiteral("^\\s*#[^\\n]*"));
        static const QRegularExpression stringLiteral(QStringLiteral("\"(?:[^\"\\\\]|\\\\.)*\""));
        static const QRegularExpression number(QStringLiteral("\\b[0-9][0-9a-fA-Fx.uUlL]*\\b"));
        static const QRegularExpression lineComment(QStringLiteral("//[^\\n]*"));
        static const QRegularExpression blockCommentStart(QStringLiteral("/\\*"));
        static const QRegularExpression blockCommentEnd(QStringLiteral("\\*/"));

        applyRule(text, keywordPattern_, keywordFormat_);
        applyRule(text, number, numberFormat_);
        applyRule(text, preprocessor, preprocessorFormat_);
        applyRule(text, stringLiteral, stringFormat_);
        applyRule(text, lineComment, commentFormat_);

        // /* */ spans via the standard block-state idiom.
        setCurrentBlockState(0);
        int start = previousBlockState() == 1 ? 0 : text.indexOf(blockCommentStart);
        while (start >= 0) {
            const QRegularExpressionMatch endMatch = blockCommentEnd.match(text, start);
            if (!endMatch.hasMatch()) {
                setCurrentBlockState(1);
                setFormat(start, text.length() - start, commentFormat_);
                break;
            }
            const int length = endMatch.capturedEnd() - start;
            setFormat(start, length, commentFormat_);
            start = text.indexOf(blockCommentStart, start + length);
        }
    }

private:
    void applyRule(const QString& text, const QRegularExpression& pattern, const QTextCharFormat& format) {
        QRegularExpressionMatchIterator it = pattern.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), format);
        }
    }

    QRegularExpression keywordPattern_;
    QTextCharFormat keywordFormat_;
    QTextCharFormat preprocessorFormat_;
    QTextCharFormat stringFormat_;
    QTextCharFormat numberFormat_;
    QTextCharFormat commentFormat_;
};

// Roomier margins and row spacing than QFormLayout's defaults, so every tab reads as one panel.
// Call after all rows are added: QFormLayout creates each row's label QLabel only when addRow(QString, ...) runs.
inline void styleInspectorForm(QFormLayout* form) {
    form->setContentsMargins(14, 14, 14, 10);
    form->setVerticalSpacing(10);
    form->setHorizontalSpacing(12);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // One shared label/value axis: every label gets the same fixed width so the value column lines
    // up across tabs; word-wrap so a long label wraps instead of being clipped.
    for (int row = 0; row < form->rowCount(); ++row) {
        if (QLayoutItem* labelItem = form->itemAt(row, QFormLayout::LabelRole)) {
            if (auto* label = qobject_cast<QLabel*>(labelItem->widget())) {
                label->setFixedWidth(kRowLabelColumnWidth);
                label->setWordWrap(true);
                label->setStyleSheet(kTypeLabel);
                label->setMinimumHeight(kRowHeight);
            }
        }
        // Row height is only a floor; taller fields (fixed-height plain-text edits) are unaffected.
        if (QLayoutItem* fieldItem = form->itemAt(row, QFormLayout::FieldRole)) {
            if (QWidget* field = fieldItem->widget()) {
                field->setMinimumHeight(kRowHeight);
            }
        }
    }
}

// The C++ root namespace the Code tab's live preview generates under; matches the project manifest's
// default rootNamespace until a per-project value is wired through.
constexpr const char* kGeneratedRootNamespace = "app::generated";

// Color palette selector widget for Inspector rows
class ColorPaletteWidget : public QWidget {
public:
    explicit ColorPaletteWidget(std::function<void(ElementColor)> onPicked, QWidget* parent = nullptr)
        : QWidget(parent), onPicked_(std::move(onPicked)) {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 2, 0, 2);
        layout->setSpacing(6);

        const QVector<QPair<ElementColor, QString>> colors = {
            {ElementColor::Default, QStringLiteral("Default")},
            {ElementColor::Red, QStringLiteral("Red")},
            {ElementColor::Orange, QStringLiteral("Orange")},
            {ElementColor::Yellow, QStringLiteral("Yellow")},
            {ElementColor::Green, QStringLiteral("Green")},
            {ElementColor::Blue, QStringLiteral("Blue")},
            {ElementColor::Purple, QStringLiteral("Purple")},
            {ElementColor::Pink, QStringLiteral("Pink")},
            {ElementColor::Gray, QStringLiteral("Gray")},
        };

        for (const auto& pair : colors) {
            const ElementColor color = pair.first;
            const QString& name = pair.second;
            auto* btn = new QToolButton(this);
            btn->setFixedSize(18, 18);
            btn->setToolTip(name);
            btn->setCursor(Qt::PointingHandCursor);
            buttons_.insert(color, btn);

            QObject::connect(btn, &QToolButton::clicked, this, [this, color] {
                setColor(color);
                if (onPicked_) {
                    onPicked_(color);
                }
            });
            layout->addWidget(btn);
        }
        layout->addStretch();
        updateStyles();
    }

    ElementColor color() const { return currentColor_; }

    void setColor(ElementColor color) {
        if (currentColor_ == color) {
            return;
        }
        currentColor_ = color;
        updateStyles();
    }

private:
    void updateStyles() {
        for (auto it = buttons_.begin(); it != buttons_.end(); ++it) {
            const ElementColor c = it.key();
            QToolButton* btn = it.value();
            const bool isSelected = (c == currentColor_);

            const QColor accent = elementAccent(c);
            const QString bg = accent.isValid() ? accent.name() : QString::fromLatin1(kElementDefaultSwatch);

            if (isSelected) {
                btn->setStyleSheet(resolveRoles(QStringLiteral(
                    "QToolButton { background-color: %1; border: 2px solid {text-on-accent}; border-radius: 9px; }"
                )).arg(bg));
            } else {
                btn->setStyleSheet(resolveRoles(QStringLiteral(
                    "QToolButton { background-color: %1; border: 1px solid {node-border}; border-radius: 9px; }"
                    "QToolButton:hover { border: 1px solid {text-secondary}; }"
                )).arg(bg));
            }
        }
    }

    ElementColor currentColor_ = ElementColor::Default;
    QMap<ElementColor, QToolButton*> buttons_;
    std::function<void(ElementColor)> onPicked_;
};

}  // namespace inspector_detail
}  // namespace app
