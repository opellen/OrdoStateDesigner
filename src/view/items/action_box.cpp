#include "view/items/action_box.h"

#include <algorithm>

#include <QColor>
#include <QCursor>
#include <QFontMetrics>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QStyleOptionGraphicsItem>

#include "constants/design_tokens.h"
#include "view/canvas/canvas_presenter.h"  // SelectionKind's full definition
#include "view/shell/icons.h"  // assetPixmap: the tinted-SVG renderer for icon segments

namespace app {

namespace {

constexpr qreal kRowHeight = 26.0;
constexpr qreal kSegmentPaddingX = 10.0;
constexpr qreal kMinSegmentWidth = 30.0;
constexpr qreal kBelowGap = 8.0;  // gap between the selection and the box
// Icon segments are a fixed square slot, whatever the glyph's shape.
constexpr qreal kIconSlotPx = kRowHeight - 8.0;

QColor capsuleBackground() { return design::color(design::kCapsuleFill); }
QColor capsuleBorder() { return design::color(design::kAccentInteractive); }
QColor segmentDivider() { return design::color(design::kCapsuleDivider); }
QColor segmentHover() { return design::color(design::kCanvasHoverOverlay); }
QColor labelColor(bool enabled) {
    return enabled ? design::color(design::kNodeName) : design::color(design::kCapsuleLabelDisabled);
}

}  // namespace

ActionBoxItem::ActionBoxItem() {
    setZValue(100.0);  // interaction-overlay z band
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setCursor(QCursor(Qt::PointingHandCursor));
    font_.setPointSizeF(9.0);
    setVisible(false);
}

void ActionBoxItem::showFor(SelectionKind kind, const QRectF& anchorSceneRect, bool reverseEnabled) {
    prepareGeometryChange();
    segments_.clear();
    if (kind == SelectionKind::Frame) {
        // Icon-only; the verb names ride the hover tooltips. The "more" menu
        // carries Add State Here.
        segments_.push_back(
            {ActionBoxVerb::AddMachineEvent, QString(), QStringLiteral("Event"), true, QRectF(), "add"});
        segments_.push_back({ActionBoxVerb::AddMachineSelf, QString(), QStringLiteral("Self transition"), true,
                             QRectF(), "self-loop"});
        segments_.push_back({ActionBoxVerb::AddNote, QString(), QStringLiteral("Note"), true, QRectF(), "note"});
        segments_.push_back({ActionBoxVerb::FrameMore, QString(), QStringLiteral("More"), true, QRectF(), "more"});
    } else if (kind == SelectionKind::State) {
        segments_.push_back(
            {ActionBoxVerb::AddTransition, QString(), QStringLiteral("Transition"), true, QRectF(), "add"});
        segments_.push_back({ActionBoxVerb::AddSelfTransition, QString(), QStringLiteral("Self transition"), true,
                             QRectF(), "self-loop"});
        segments_.push_back({ActionBoxVerb::StateMore, QString(), QStringLiteral("More"), true, QRectF(), "more"});
    } else if (kind == SelectionKind::Transition) {
        segments_.push_back({ActionBoxVerb::SetGuard, QStringLiteral("Guard"), QStringLiteral("Guard"), true, QRectF()});
        segments_.push_back(
            {ActionBoxVerb::SetAction, QStringLiteral("Action"), QStringLiteral("Action"), true, QRectF()});
        segments_.push_back({ActionBoxVerb::ReverseDirection, QString(), QStringLiteral("Reverse direction"),
                             reverseEnabled, QRectF(), "reverse"});
        segments_.push_back(
            {ActionBoxVerb::TransitionMore, QString(), QStringLiteral("More"), true, QRectF(), "more"});
    } else if (kind == SelectionKind::Multi) {
        // Whole-selection verbs; Color opens the swatch palette.
        segments_.push_back({ActionBoxVerb::ZoomToSelection, QStringLiteral("Zoom to Selection"),
                             QStringLiteral("Zoom to Selection"), true, QRectF()});
        segments_.push_back(
            {ActionBoxVerb::MultiColor, QStringLiteral("Color"), QStringLiteral("Color"), true, QRectF()});
        segments_.push_back({ActionBoxVerb::DeleteSelection, QStringLiteral("Delete"), QStringLiteral("Delete"),
                             true, QRectF()});
    } else {
        hideBox();
        return;  // no variant for this kind (None/Note)
    }
    layout();
    hoveredIndex_ = -1;
    if (kind == SelectionKind::State || kind == SelectionKind::Transition) {
        // Above the selection so it does not cover outgoing wires, arrows or
        // interior contents.
        setPos(anchorSceneRect.center().x() - capsuleRect_.width() / 2.0,
               anchorSceneRect.top() - capsuleRect_.height() - kBelowGap);
    } else {
        setPos(anchorSceneRect.center().x() - capsuleRect_.width() / 2.0, anchorSceneRect.bottom() + kBelowGap);
    }
    setVisible(true);
}

void ActionBoxItem::hideBox() {
    if (isVisible()) {
        setVisible(false);
    }
    hoveredIndex_ = -1;
}

void ActionBoxItem::setOnVerb(std::function<void(ActionBoxVerb)> cb) { onVerb_ = std::move(cb); }

std::vector<QString> ActionBoxItem::debugSegmentLabels() const {
    std::vector<QString> labels;
    labels.reserve(segments_.size());
    for (const Segment& segment : segments_) {
        labels.push_back(segment.label);
    }
    return labels;
}

std::vector<QString> ActionBoxItem::debugSegmentIconSlugs() const {
    std::vector<QString> slugs;
    slugs.reserve(segments_.size());
    for (const Segment& segment : segments_) {
        slugs.push_back(segment.iconSlug != nullptr ? QString::fromLatin1(segment.iconSlug) : QString());
    }
    return slugs;
}

std::vector<QString> ActionBoxItem::debugSegmentTooltips() const {
    std::vector<QString> tooltips;
    tooltips.reserve(segments_.size());
    for (const Segment& segment : segments_) {
        tooltips.push_back(segment.tooltip);
    }
    return tooltips;
}

void ActionBoxItem::layout() {
    const QFontMetricsF metrics(font_);
    qreal x = 0.0;
    for (Segment& segment : segments_) {
        const qreal contentWidth =
            segment.iconSlug != nullptr ? kIconSlotPx : metrics.horizontalAdvance(segment.label);
        const qreal width = std::max(kMinSegmentWidth, contentWidth + kSegmentPaddingX * 2.0);
        segment.rect = QRectF(x, 0.0, width, kRowHeight);
        x += width;
    }
    capsuleRect_ = QRectF(0.0, 0.0, x, kRowHeight);
}

QRectF ActionBoxItem::boundingRect() const { return capsuleRect_.adjusted(-1.0, -1.0, 1.0, 1.0); }

void ActionBoxItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(capsuleBorder(), 1.2));
    painter->setBrush(capsuleBackground());
    painter->drawRoundedRect(capsuleRect_, kRowHeight / 2.0, kRowHeight / 2.0);

    painter->setFont(font_);
    // Render icons at devicePixelRatio times the view zoom; dpr alone blurs
    // them when zoomed in. assetPixmap tags the ratio back, so the slot stays
    // kIconSlotPx in item coordinates.
    const qreal dpr = (painter->device() != nullptr) ? painter->device()->devicePixelRatio() : 1.0;
    const qreal iconRatio = dpr * QStyleOptionGraphicsItem::levelOfDetailFromTransform(painter->worldTransform());
    lastIconRenderRatio_ = iconRatio;
    for (size_t i = 0; i < segments_.size(); ++i) {
        const Segment& segment = segments_[i];
        if (static_cast<int>(i) == hoveredIndex_ && segment.enabled) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(segmentHover());
            painter->drawRoundedRect(segment.rect.adjusted(2.0, 2.0, -2.0, -2.0), kRowHeight / 2.0 - 2.0,
                                      kRowHeight / 2.0 - 2.0);
        }
        if (i > 0) {
            painter->setPen(QPen(segmentDivider(), 1.0));
            painter->drawLine(segment.rect.topLeft(), segment.rect.bottomLeft());
        }
        const QColor currentColor = labelColor(segment.enabled);
        if (segment.iconSlug != nullptr) {
            const QPixmap pixmap = icons::assetPixmap(segment.iconSlug, qRound(kIconSlotPx), currentColor, iconRatio);
            const QPointF topLeft = segment.rect.center() - QPointF(kIconSlotPx / 2.0, kIconSlotPx / 2.0);
            painter->drawPixmap(topLeft, pixmap);
        } else {
            painter->setPen(currentColor);
            painter->drawText(segment.rect, Qt::AlignCenter, segment.label);
        }
    }
}

void ActionBoxItem::hoverMoveEvent(QGraphicsSceneHoverEvent* event) {
    int index = -1;
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (segments_[i].rect.contains(event->pos())) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index != hoveredIndex_) {
        hoveredIndex_ = index;
        // Per-segment tooltip on the single item.
        setToolTip(index >= 0 ? segments_[static_cast<size_t>(index)].tooltip : QString());
        update();
    }
}

void ActionBoxItem::hoverLeaveEvent(QGraphicsSceneHoverEvent*) {
    if (hoveredIndex_ != -1) {
        hoveredIndex_ = -1;
        setToolTip(QString());
        update();
    }
}

void ActionBoxItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    event->accept();  // never falls through to the canvas's own empty-area click handling
    for (const Segment& segment : segments_) {
        if (segment.rect.contains(event->pos())) {
            if (segment.enabled && onVerb_) {
                onVerb_(segment.verb);
            }
            return;
        }
    }
}

}  // namespace app
