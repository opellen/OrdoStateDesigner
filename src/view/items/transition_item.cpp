#include "view/items/transition_item.h"

#include "constants/design_tokens.h"
#include "infra/expression.h"
#include "view/geometry/element_colors.h"  // elementAccent

#include <algorithm>
#include <cmath>
#include <utility>

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QFontMetricsF>
#include <QGraphicsScene>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QLineF>
#include <QPainter>
#include <QPainterPathStroker>
#include <QPen>
#include <QStyleOptionGraphicsItem>

namespace app {

namespace {

QColor edgeColor() { return design::color(design::kEdge); }  // unselected stroke
// Selected: solid white line plus cyan port-anchor dots.
QColor selectedLineColor() { return design::color(design::kEdgeSelected); }
QColor portDotColor() { return design::color(design::kCanvasHighlight); }
constexpr qreal kPortDotRadius = 4.0;  // 8px diameter
constexpr qreal kStrokeWidth = 2.0;
constexpr qreal kHitStrokeWidth = 14.0;  // widened hit path

constexpr qreal kDimmedOpacity = 0.3;  // Simulate-mode dimming

}  // namespace

TransitionItem::TransitionItem(quint64 id) : id_(id) {
    setZValue(0.0);  // edges under nodes (StateItem, zValue 10)
    setFlag(ItemIsSelectable, true);
    setAcceptHoverEvents(true);
}

void TransitionItem::setRoute(const RoutedEdge& routed) {
    prepareGeometryChange();
    routed_ = routed;
    updateContinuousPath();
    update();
}

void TransitionItem::setOnSelectedChanged(std::function<void(bool)> cb) { onSelectedChanged_ = std::move(cb); }

void TransitionItem::setOnReconnectDrag(std::function<void(QPointF)> started, std::function<void(QPointF)> moved,
                                         std::function<void(QPointF)> finished) {
    onReconnectStarted_ = std::move(started);
    onReconnectMoved_ = std::move(moved);
    onReconnectFinished_ = std::move(finished);
}

void TransitionItem::setOnSegmentDrag(std::function<void(int, bool, bool, QPointF)> started,
                                      std::function<void(QPointF)> moved,
                                      std::function<void(QPointF)> finished) {
    onSegmentDragStarted_ = std::move(started);
    onSegmentDragMoved_ = std::move(moved);
    onSegmentDragFinished_ = std::move(finished);
}

void TransitionItem::setActiveDragLine(QLineF line) {
    activeDragLine_ = line;
    hasActiveDragLine_ = true;
    update();
}

void TransitionItem::clearActiveDragLine() {
    if (!hasActiveDragLine_) {
        return;
    }
    hasActiveDragLine_ = false;
    activeDragLine_ = QLineF();
    update();
}

WireSegment TransitionItem::hitSegmentAt(QPointF scenePos) const {
    constexpr qreal kHitRadius = 7.0;
    const auto checkSegments = [&](const QVector<WireSegment>& segments) -> WireSegment {
        for (const auto& seg : segments) {
            const QLineF line = seg.line;
            const QPointF v = line.p2() - line.p1();
            const qreal lenSq = v.x() * v.x() + v.y() * v.y();
            if (lenSq < 9.0) continue;
            const qreal t = std::clamp(QPointF::dotProduct(scenePos - line.p1(), v) / lenSq, 0.0, 1.0);
            const QPointF proj = line.p1() + t * v;
            const qreal distSq = QPointF::dotProduct(scenePos - proj, scenePos - proj);
            if (distSq <= kHitRadius * kHitRadius) {
                return seg;
            }
        }
        return WireSegment{};
    };

    const QVector<QPointF> canonical = canonicalWaypoints(routed_);
    const QVector<WireSegment> segs = extractWireSegments(canonical, true);
    return checkSegments(segs);
}

void TransitionItem::hoverMoveEvent(QGraphicsSceneHoverEvent* event) {
    if (reconnecting_ || segmentDragging_) {
        QGraphicsItem::hoverMoveEvent(event);
        return;
    }
    const WireSegment hit = hitSegmentAt(event->scenePos());
    if (hit.segmentIndex >= 0) {
        if (!(hoveredSegment_ == hit)) {
            hoveredSegment_ = hit;
            setCursor(hit.orientation == WireSegment::Orientation::Horizontal ? Qt::SizeVerCursor : Qt::SizeHorCursor);
            update();
        }
    } else {
        if (hoveredSegment_.segmentIndex >= 0) {
            hoveredSegment_ = WireSegment{};
            unsetCursor();
            update();
        }
    }
    QGraphicsItem::hoverMoveEvent(event);
}

void TransitionItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) {
    if (hoveredSegment_.segmentIndex >= 0) {
        hoveredSegment_ = WireSegment{};
        unsetCursor();
        update();
    }
    QGraphicsItem::hoverLeaveEvent(event);
}

void TransitionItem::setDimmed(bool dimmed) { setOpacity(dimmed ? kDimmedOpacity : 1.0); }

void TransitionItem::setColor(ElementColor color) {
    if (color_ == color) {
        return;
    }
    color_ = color;
    update();
}

void TransitionItem::setPulseColor(const QColor& color) {
    pulseColor_ = color;
    pulsing_ = true;
    update();
}

void TransitionItem::clearPulse() {
    pulsing_ = false;
    update();
}

void TransitionItem::setHighlighted(bool highlighted) {
    if (highlighted_ == highlighted) {
        return;
    }
    highlighted_ = highlighted;
    update();
}

void TransitionItem::setTransientLabelAnchor(QPointF anchor) {
    if (routed_.labelAnchor != anchor) {
        prepareGeometryChange();
        routed_.labelAnchor = anchor;
        update();
    }
}

void TransitionItem::setTransientLeaderLine(bool visible) {
    if (transientLeaderVisible_ != visible) {
        prepareGeometryChange();
        transientLeaderVisible_ = visible;
        update();
    }
}

void TransitionItem::updateContinuousPath() {
    const QVector<QPointF> canonical = canonicalWaypoints(routed_);
    if (canonical.size() >= 2) {
        continuousPath_ = buildStyledPath(canonical, 10.0);
    } else {
        continuousPath_ = QPainterPath();
    }
}

QRectF TransitionItem::boundingRect() const {
    QRectF r;
    // The wire is one canonical Manhattan polyline node to node; the pill's opaque
    // capsule masks the segment beneath it. The halves are only a fallback when
    // canonicalWaypoints() yields < 2 points (continuousPath_ empty).
    if (!continuousPath_.isEmpty()) {
        r = continuousPath_.boundingRect();
    } else {
        r = routed_.sourceHalf.boundingRect().united(routed_.targetHalf.boundingRect());
    }
    r = r.united(routed_.arrowhead.boundingRect());
    for (const auto& branch : routed_.secondaryBranches) {
        r = r.united(branch.path.boundingRect()).united(branch.arrowhead.boundingRect());
    }
    if (transientLeaderVisible_ && !routed_.labelBase.isNull() && !routed_.labelAnchor.isNull()) {
        r = r.united(QRectF(routed_.labelBase, routed_.labelAnchor).normalized());
    }
    // Half the hit-stroke width plus slack, so shape() stays inside boundingRect().
    constexpr qreal kMargin = kHitStrokeWidth / 2.0 + 1.0;
    return r.adjusted(-kMargin, -kMargin, kMargin, kMargin);
}

QPainterPath TransitionItem::shape() const {
    QPainterPath combined;
    if (!continuousPath_.isEmpty()) {
        combined = continuousPath_;
    } else {
        combined = routed_.sourceHalf;
        combined.addPath(routed_.targetHalf);
    }
    for (const auto& branch : routed_.secondaryBranches) {
        combined.addPath(branch.path);
    }
    QPainterPathStroker stroker;
    stroker.setWidth(kHitStrokeWidth);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    QPainterPath result = stroker.createStroke(combined);
    result.addPolygon(routed_.arrowhead);
    for (const auto& branch : routed_.secondaryBranches) {
        result.addPolygon(branch.arrowhead);
    }
    return result;
}

QVariant TransitionItem::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemSelectedHasChanged) {
        if (onSelectedChanged_) {
            onSelectedChanged_(value.toBool());
        }
        update();
    }
    return QGraphicsItem::itemChange(change, value);
}

void TransitionItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    if (event->button() == Qt::RightButton) {
        // A right-click keeps a selection it lands inside (a multi-selection
        // included -- the context menu's Zoom to Selection acts on all of it);
        // an unselected edge is selected alone.
        if (!isSelected()) {
            if (scene() != nullptr && !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
                scene()->clearSelection();
            }
            setSelected(true);
        }
        update();
        event->accept();
        return;
    }
    // A press near the arrowhead of an already selected edge grabs the endpoint.
    constexpr qreal kReconnectGrabRadius = 14.0;
    if (isSelected() && onReconnectStarted_ && !routed_.arrowhead.isEmpty() &&
        event->button() == Qt::LeftButton &&
        QLineF(event->scenePos(), routed_.arrowhead.at(0)).length() <= kReconnectGrabRadius) {
        reconnecting_ = true;
        onReconnectStarted_(event->scenePos());
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        const WireSegment hit = hitSegmentAt(event->scenePos());
        if (hit.segmentIndex >= 0 && onSegmentDragStarted_) {
            segmentDragging_ = true;
            activeSegment_ = hit;
            dragStartPos_ = event->scenePos();
            onSegmentDragStarted_(hit.segmentIndex, hit.isSourceHalf,
                                  hit.orientation == WireSegment::Orientation::Horizontal,
                                  event->scenePos());
            event->accept();
            return;
        }
    }
    QGraphicsItem::mousePressEvent(event);
}

void TransitionItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (reconnecting_) {
        if (onReconnectMoved_) {
            onReconnectMoved_(event->scenePos());
        }
        event->accept();
        return;
    }
    if (segmentDragging_) {
        if (onSegmentDragMoved_) {
            onSegmentDragMoved_(event->scenePos());
        }
        event->accept();
        return;
    }
    QGraphicsItem::mouseMoveEvent(event);
}

void TransitionItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    if (reconnecting_) {
        reconnecting_ = false;
        if (onReconnectFinished_) {
            onReconnectFinished_(event->scenePos());
        }
        event->accept();
        return;
    }
    if (segmentDragging_) {
        segmentDragging_ = false;
        hoveredSegment_ = WireSegment{};
        unsetCursor();
        update();
        if (onSegmentDragFinished_) {
            onSegmentDragFinished_(event->scenePos());
        }
        event->accept();
        return;
    }
    QGraphicsItem::mouseReleaseEvent(event);
}

void TransitionItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);

    const bool selected = isSelected();
    const bool highlighted = highlighted_;
    // The firing pulse takes precedence over selected.
    const QColor lineColor =
        pulsing_ ? pulseColor_
                 : (highlighted ? design::color(design::kCanvasHighlight)
                                : (selected ? selectedLineColor()
                                            : (color_ != ElementColor::Default ? elementAccent(color_) : edgeColor())));

    QPen pen(lineColor, highlighted ? (kStrokeWidth + 1.0) : kStrokeWidth);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    if (!continuousPath_.isEmpty()) {
        painter->drawPath(continuousPath_);
    } else {
        painter->drawPath(routed_.sourceHalf);
        painter->drawPath(routed_.targetHalf);
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(QBrush(lineColor));
    painter->drawPolygon(routed_.arrowhead);

    for (const auto& branch : routed_.secondaryBranches) {
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(branch.path);

        painter->setPen(Qt::NoPen);
        painter->setBrush(QBrush(lineColor));
        painter->drawPolygon(branch.arrowhead);
    }

    if (selected && routed_.sourceHalf.elementCount() > 0 && !routed_.arrowhead.isEmpty()) {
        // Port-anchor dots: the source half's first point and the arrowhead apex;
        // RoutedEdge does not store the anchors separately and both sit at the
        // node edge.
        const QPointF sourceDot = routed_.sourceHalf.pointAtPercent(0.0);
        const QPointF targetDot = routed_.arrowhead.at(0);
        painter->setBrush(QBrush(portDotColor()));
        painter->drawEllipse(sourceDot, kPortDotRadius, kPortDotRadius);
        painter->drawEllipse(targetDot, kPortDotRadius, kPortDotRadius);
        for (const auto& branch : routed_.secondaryBranches) {
            if (!branch.arrowhead.isEmpty()) {
                painter->drawEllipse(branch.arrowhead.at(0), kPortDotRadius, kPortDotRadius);
            }
        }
    }

    // Dashed leader line from the wire's base anchor to the displaced label, shown
    // only during an active pill drag.
    if (transientLeaderVisible_ && !routed_.labelBase.isNull() && !routed_.labelAnchor.isNull()) {
        const QPointF offset = routed_.labelAnchor - routed_.labelBase;
        if (offset.manhattanLength() > 12.0) {
            painter->save();
            QPen guidePen(design::color(design::kGuideRouting), 1.2, Qt::DashLine);
            guidePen.setDashPattern({4.0, 3.0});
            painter->setPen(guidePen);
            painter->setBrush(Qt::NoBrush);
            painter->drawLine(routed_.labelBase, routed_.labelAnchor);
            painter->restore();
        }
    }

    // Handles bind to the orthogonal skeleton, not the filleted paint: cyan
    // circles = port anchors, cyan squares = bend vertices, white squares =
    // segment midpoints (grab to translate).
    WireSegment displaySegment = hoveredSegment_;
    QVector<QPointF> skeleton;
    QVector<WireSegment> segs;
    if (selected || segmentDragging_ || hasActiveDragLine_) {
        skeleton = canonicalWaypoints(routed_);
        segs = extractWireSegments(skeleton, true);
    }
    if (hasActiveDragLine_) {
        // The presenter's exact segment wins over any index lookup.
        displaySegment.segmentIndex = 0;
        displaySegment.line = activeDragLine_;
    } else if (segmentDragging_) {
        bool found = false;
        for (const auto& s : segs) {
            if (s.segmentIndex == activeSegment_.segmentIndex) {
                displaySegment = s;
                found = true;
                break;
            }
        }
        if (!found && !segs.isEmpty()) {
            displaySegment = segs.front();
        }
    }
    if (displaySegment.segmentIndex >= 0) {
        QPen hoverPen(design::color(design::kCanvasHighlight), kStrokeWidth + 2.0);
        hoverPen.setCapStyle(Qt::RoundCap);
        painter->setPen(hoverPen);
        painter->drawLine(displaySegment.line);

        // Grab handle at the segment midpoint.
        const QPointF mid = displaySegment.line.center();
        const QRectF handleRect(mid.x() - 4.0, mid.y() - 4.0, 8.0, 8.0);
        painter->setPen(QPen(design::color(design::kCanvasHighlight), 1.5));
        painter->setBrush(QBrush(design::color(design::kEdgeSelected)));
        painter->drawRoundedRect(handleRect, 2.0, 2.0);
    } else if (selected) {
        for (const auto& s : segs) {
            const QPointF mid = s.line.center();
            painter->setPen(QPen(design::color(design::kCanvasHighlight), 1.0));
            painter->setBrush(QBrush(design::color(design::kEdgeSelected)));
            painter->drawRect(QRectF(mid.x() - 3.0, mid.y() - 3.0, 6.0, 6.0));
        }
    }
    // Bend-vertex handles at the skeleton's interior corners, drawn last so
    // they stay legible over the segment highlight on short segments.
    if (skeleton.size() > 2) {
        painter->setPen(QPen(design::color(design::kBendHandleBorder), 1.0));
        painter->setBrush(QBrush(design::color(design::kCanvasHighlight)));
        for (int i = 1; i < skeleton.size() - 1; ++i) {
            painter->drawRect(QRectF(skeleton[i].x() - 3.0, skeleton[i].y() - 3.0, 6.0, 6.0));
        }
    }
}

namespace {

constexpr qreal kLabelPaddingX = 10.0;
constexpr qreal kLabelHeight = 18.0;
constexpr qreal kTwoLineHeight = 36.0;
constexpr qreal kThreeLineHeight = 52.0;
constexpr qreal kMaxLabelWidth = 280.0;
const QChar kBlankGlyph(0x00B7);  // "." -- MIDDLE DOT placeholder
constexpr qreal kSelectedRingInset = 1.5;
constexpr qreal kSelectedRingWidth = 1.5;

QColor guardTextColor() { return design::color(design::kLogicVariable); }
QColor actionTextColor() { return design::color(design::kLogicAction); }
QColor raiseTextColor() { return design::color(design::kLogicRaise); }
QColor actorCommTextColor() { return design::color(design::kLogicSend); }
QColor alwaysTextColor() { return design::color(design::kLogicAlways); }
QColor wildcardTextColor() { return design::color(design::kLogicWildcard); }
QColor timeTriggerTextColor() { return design::color(design::kLogicAfter); }
QColor effectiveActionTextColor(const QString& action) {
    if (expr::parseSendToForm(action).ok || expr::parseSendParentForm(action).ok) {
        return actorCommTextColor();
    }
    if (expr::parseRaiseForm(action).ok) {
        return raiseTextColor();
    }
    return actionTextColor();
}

QColor labelBackground(bool blank) {
    return blank ? design::color(design::kEdgeLabelFillBlank) : design::color(design::kEdgeLabelFill);
}
QColor labelText(bool blank) {
    return blank ? design::color(design::kEdgeLabelTextBlank) : design::color(design::kEdgeLabelText);
}

QColor effectiveEventTextColor(const QString& event, bool always, bool blank) {
    if (always) {
        return alwaysTextColor();
    }
    if (expr::parseTimeTrigger(event).ok) {
        return timeTriggerTextColor();
    }
    if (app::isWildcardEventDescriptor(event)) {
        return wildcardTextColor();
    }
    return labelText(blank);
}

QColor selectedRingColor() { return design::color(design::kAccentInteractive); }

// Simulate-mode presentation. The waiting ring draws on top of labelBackground()
// rather than replacing it.
QColor fireableBackground() { return design::color(design::kFireableFill); }
QColor waitingRingColor() { return design::color(design::kWaitingRing); }
QColor waitingTextColor() { return design::color(design::kWaitingText); }
QColor highlightedRingColor() { return design::color(design::kCanvasHighlight); }

}  // namespace

QString transitionPillEventText(const Transition& transition) {
    if (transition.isAlways()) {
        return QStringLiteral("always");
    }
    if (!transition.event.trimmed().isEmpty()) {
        return transition.event;
    }
    if (transition.delayMs > 0) {
        return transition.periodic ? QStringLiteral("every %1ms").arg(transition.delayMs)
                                   : QStringLiteral("after %1ms").arg(transition.delayMs);
    }
    return QString();
}

TransitionLabelItem::TransitionLabelItem() {
    font_.setPointSizeF(8.0);
    setZValue(20.0);  // labels above nodes
    setAcceptHoverEvents(true);
    recomputeGeometry();
}

void TransitionLabelItem::setEvent(const QString& event) {
    setLabelData(event, guard_, action_, always_);
}

void TransitionLabelItem::setLabelData(const QString& event, const QString& guard, const QString& action, bool always) {
    if (event_ == event && guard_ == guard && action_ == action && always_ == always) {
        return;
    }
    event_ = event;
    guard_ = guard;
    action_ = action;
    always_ = always;
    recomputeGeometry();
    update();
}

void TransitionLabelItem::setAnchor(QPointF sceneAnchor) { setPos(sceneAnchor); }

// rect_ is centered on (0,0) and pos() is the pill's center, so translating by
// the center is the whole local-to-scene mapping.
QRectF TransitionLabelItem::sceneRectAt(QPointF center) const { return rect_.translated(center); }
QRectF TransitionLabelItem::sceneRect() const { return sceneRectAt(pos()); }

void TransitionLabelItem::setEdgeItem(TransitionItem* edge) { edge_ = edge; }
void TransitionLabelItem::setInteractive(bool interactive) { interactive_ = interactive; }

void TransitionLabelItem::setFireable(bool fireable) {
    if (fireable_ == fireable) {
        return;
    }
    fireable_ = fireable;
    if (fireable_) {
        setCursor(Qt::PointingHandCursor);
    } else {
        unsetCursor();
    }
    update();
}

void TransitionLabelItem::setWaitingAccent(bool waiting) {
    if (waitingAccent_ == waiting) {
        return;
    }
    waitingAccent_ = waiting;
    update();
}

void TransitionLabelItem::setDimmed(bool dimmed) { setOpacity(dimmed ? kDimmedOpacity : 1.0); }

void TransitionLabelItem::setHighlighted(bool highlighted) {
    if (highlighted_ == highlighted) {
        return;
    }
    highlighted_ = highlighted;
    update();
}

void TransitionLabelItem::setOnFireClicked(std::function<void()> cb) { onFireClicked_ = std::move(cb); }
void TransitionLabelItem::setOnEditRequested(std::function<void()> cb) { onEditRequested_ = std::move(cb); }
void TransitionLabelItem::setOnDragFinished(std::function<void(QPointF)> cb) { onDragFinished_ = std::move(cb); }
void TransitionLabelItem::setOnDragMoved(std::function<void(QPointF)> cb) { onDragMoved_ = std::move(cb); }

QRectF TransitionLabelItem::boundingRect() const {
    // Wide enough for the selected-ring stroke drawn beyond rect_ in paint().
    constexpr qreal kMargin = kSelectedRingInset + kSelectedRingWidth;
    return rect_.adjusted(-kMargin, -kMargin, kMargin, kMargin);
}

void TransitionLabelItem::recomputeGeometry() {
    prepareGeometryChange();
    const bool hasEvent = !event_.trimmed().isEmpty() || always_;
    const bool hasGuard = !guard_.trimmed().isEmpty();
    const bool hasAction = !action_.trimmed().isEmpty();
    const bool blank = !hasEvent && !hasGuard && !hasAction;

    const QFontMetricsF metrics(font_);
    const QString eventDisplay = always_ ? QStringLiteral("always")
                                         : (hasEvent ? event_ : (blank ? QString(kBlankGlyph) : QString()));

    int lineCount = 1;
    qreal w1 = 0.0;
    qreal w2 = 0.0;
    qreal w3 = 0.0;

    const QString guardText = hasGuard ? QStringLiteral("[%1]").arg(guard_) : QString();
    const QString actionText = hasAction ? QStringLiteral("/ %1").arg(action_) : QString();

    if (hasAction) {
        if (hasEvent && hasGuard) {
            const QString combinedL1 = eventDisplay + QStringLiteral(" ") + guardText;
            if (metrics.horizontalAdvance(combinedL1) <= 220.0) {
                // 2 lines: L1 = event [guard], L2 = / action
                lineCount = 2;
                w1 = metrics.horizontalAdvance(combinedL1);
                w2 = metrics.horizontalAdvance(actionText);
            } else {
                // 3 lines: L1 = event, L2 = [guard], L3 = / action
                lineCount = 3;
                w1 = metrics.horizontalAdvance(eventDisplay);
                w2 = metrics.horizontalAdvance(guardText);
                w3 = metrics.horizontalAdvance(actionText);
            }
        } else if (hasGuard) {
            // No event, has guard & action -> 2 lines: L1 = [guard], L2 = / action
            lineCount = 2;
            w1 = metrics.horizontalAdvance(guardText);
            w2 = metrics.horizontalAdvance(actionText);
        } else if (hasEvent) {
            // Has event & action, no guard -> 2 lines: L1 = event, L2 = / action
            lineCount = 2;
            w1 = metrics.horizontalAdvance(eventDisplay);
            w2 = metrics.horizontalAdvance(actionText);
        } else {
            // Action only -> 1 line: / action
            lineCount = 1;
            w1 = metrics.horizontalAdvance(actionText);
        }
    } else if (hasGuard && hasEvent) {
        const QString combinedL1 = eventDisplay + QStringLiteral(" ") + guardText;
        if (metrics.horizontalAdvance(combinedL1) <= 220.0) {
            lineCount = 1;
            w1 = metrics.horizontalAdvance(combinedL1);
        } else {
            // 2 lines: L1 = event, L2 = [guard]
            lineCount = 2;
            w1 = metrics.horizontalAdvance(eventDisplay);
            w2 = metrics.horizontalAdvance(guardText);
        }
    } else {
        // Single line: event only, guard only, or blank
        lineCount = 1;
        if (hasGuard) {
            w1 = metrics.horizontalAdvance(guardText);
        } else {
            w1 = metrics.horizontalAdvance(eventDisplay);
        }
    }

    qreal contentWidth = 0.0;
    qreal contentHeight = kLabelHeight;

    if (lineCount == 1) {
        contentHeight = kLabelHeight;
        const qreal rawWidth = std::min(kMaxLabelWidth, w1);
        contentWidth = std::max(rawWidth + kLabelPaddingX * 2.0, kLabelHeight);
    } else if (lineCount == 2) {
        contentHeight = kTwoLineHeight;
        const qreal maxTextW = std::min(kMaxLabelWidth, std::max(w1, w2));
        contentWidth = std::max(maxTextW + kLabelPaddingX * 2.0, 72.0);
    } else {
        contentHeight = kThreeLineHeight;
        const qreal maxTextW = std::min(kMaxLabelWidth, std::max({w1, w2, w3}));
        contentWidth = std::max(maxTextW + kLabelPaddingX * 2.0, 80.0);
    }

    rect_ = QRectF(-contentWidth / 2.0, -contentHeight / 2.0, contentWidth, contentHeight);

    // Tooltip shows the full text when it may be elided or carries extra semantics.
    const bool isWildcard = !always_ && app::isWildcardEventDescriptor(event_);
    const auto timeTrigger = expr::parseTimeTrigger(event_);
    if (hasGuard || hasAction || isWildcard || timeTrigger.ok) {
        QString tip = eventDisplay;
        if (isWildcard) {
            tip += (event_ == QStringLiteral("*")) ? QStringLiteral(" (universal wildcard)")
                                                   : QStringLiteral(" (prefix wildcard)");
        } else if (timeTrigger.ok) {
            tip += timeTrigger.isEvery()
                       ? QStringLiteral(" (periodic timer: %1 ms)").arg(timeTrigger.durationMs)
                       : QStringLiteral(" (delayed timer: %1 ms)").arg(timeTrigger.durationMs);
        }
        if (hasGuard) {
            tip += QStringLiteral(" [%1]").arg(guard_);
        }
        if (hasAction) {
            tip += QStringLiteral("\n/ %1").arg(action_);
        }
        setToolTip(tip);
    } else {
        setToolTip(QString());
    }
}

void TransitionLabelItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);
    const bool hasEvent = !event_.trimmed().isEmpty() || always_;
    const bool hasGuard = !guard_.trimmed().isEmpty();
    const bool hasAction = !action_.trimmed().isEmpty();
    const bool blank = !hasEvent && !hasGuard && !hasAction;
    const bool isMultiLine = (rect_.height() > kLabelHeight + 2.0);
    const bool isThreeLine = (rect_.height() > kTwoLineHeight + 2.0);
    const qreal cornerRadius = isMultiLine ? 8.0 : (rect_.height() / 2.0);

    painter->setPen(Qt::NoPen);
    painter->setBrush(QBrush(fireable_ ? fireableBackground() : labelBackground(blank)));
    painter->drawRoundedRect(rect_, cornerRadius, cornerRadius);

    if (waitingAccent_) {
        const QRectF ring = rect_.adjusted(-kSelectedRingInset, -kSelectedRingInset, kSelectedRingInset,
                                            kSelectedRingInset);
        const qreal ringRadius = isMultiLine ? 9.0 : (ring.height() / 2.0);
        painter->setPen(QPen(waitingRingColor(), kSelectedRingWidth));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(ring, ringRadius, ringRadius);
    } else if (edge_ != nullptr && edge_->isSelected()) {
        const QRectF ring = rect_.adjusted(-kSelectedRingInset, -kSelectedRingInset, kSelectedRingInset,
                                            kSelectedRingInset);
        const qreal ringRadius = isMultiLine ? 9.0 : (ring.height() / 2.0);
        painter->setPen(QPen(selectedRingColor(), kSelectedRingWidth));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(ring, ringRadius, ringRadius);
    } else if (highlighted_) {
        const QRectF ring = rect_.adjusted(-kSelectedRingInset, -kSelectedRingInset, kSelectedRingInset,
                                            kSelectedRingInset);
        const qreal ringRadius = isMultiLine ? 9.0 : (ring.height() / 2.0);
        painter->setPen(QPen(highlightedRingColor(), kSelectedRingWidth + 0.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(ring, ringRadius, ringRadius);
    }

    QFont paintFont = font_;
    paintFont.setBold(fireable_);
    painter->setFont(paintFont);
    const QFontMetricsF metrics(paintFont);

    const QString eventDisplay = always_ ? QStringLiteral("always")
                                         : (hasEvent ? event_ : (blank ? QString(kBlankGlyph) : QString()));

    auto drawCentered = [&](const QRectF& r, const QString& text, const QColor& color) {
        if (text.isEmpty()) return;
        const qreal w = metrics.horizontalAdvance(text);
        const QString elided = (w > r.width()) ? metrics.elidedText(text, Qt::ElideRight, r.width()) : text;
        const qreal ew = metrics.horizontalAdvance(elided);
        const qreal curX = r.left() + std::max(0.0, (r.width() - ew) / 2.0);
        painter->setPen(fireable_ ? design::color(design::kTextOnAccent) : color);
        painter->drawText(QRectF(curX, r.top(), ew, r.height()), Qt::AlignVCenter | Qt::AlignLeft, elided);
    };

    if (!isMultiLine) {
        // --- Single-line rendering ---
        if (hasGuard && !hasEvent) {
            const QString guardStr = QStringLiteral("[%1]").arg(guard_);
            drawCentered(rect_, guardStr, guardTextColor());
        } else if (hasAction && !hasEvent && !hasGuard) {
            const QString actStr = QStringLiteral("/ %1").arg(action_);
            drawCentered(rect_, actStr, effectiveActionTextColor(action_));
        } else if (hasEvent && hasGuard) {
            const QString guardStr = QStringLiteral("[%1]").arg(guard_);
            const qreal gap = metrics.horizontalAdvance(QStringLiteral(" "));
            const qreal evW = metrics.horizontalAdvance(eventDisplay);
            const qreal gdW = metrics.horizontalAdvance(guardStr);
            const qreal totalW = evW + gap + gdW;
            const qreal availW = rect_.width() - kLabelPaddingX * 2.0;

            qreal curX = rect_.left() + std::max(kLabelPaddingX, (rect_.width() - totalW) / 2.0);

            painter->setPen(fireable_ ? design::color(design::kTextOnAccent)
                                      : effectiveEventTextColor(event_, always_, false));
            const QString elidedEv = (totalW > availW) ? metrics.elidedText(eventDisplay, Qt::ElideRight, availW * 0.5) : eventDisplay;
            painter->drawText(QRectF(curX, rect_.top(), metrics.horizontalAdvance(elidedEv), rect_.height()),
                              Qt::AlignVCenter | Qt::AlignLeft, elidedEv);
            curX += metrics.horizontalAdvance(elidedEv) + gap;

            painter->setPen(fireable_ ? design::color(design::kTextOnAccent) : guardTextColor());
            const qreal remainingW = std::max(10.0, rect_.right() - kLabelPaddingX - curX);
            const QString elidedGd = (metrics.horizontalAdvance(guardStr) > remainingW)
                                         ? metrics.elidedText(guardStr, Qt::ElideRight, remainingW)
                                         : guardStr;
            painter->drawText(QRectF(curX, rect_.top(), remainingW, rect_.height()),
                              Qt::AlignVCenter | Qt::AlignLeft, elidedGd);
        } else {
            if (fireable_) {
                painter->setPen(design::color(design::kTextOnAccent));
            } else if (waitingAccent_) {
                painter->setPen(waitingTextColor());
            } else {
                painter->setPen(effectiveEventTextColor(event_, always_, blank));
            }
            painter->drawText(rect_, Qt::AlignCenter, eventDisplay);
        }
    } else if (isThreeLine) {
        // --- Three-line rendering ---
        // L1: Event, L2: [Guard], L3: / Action
        const QRectF r1(rect_.left() + kLabelPaddingX, rect_.top() + 3.0, rect_.width() - kLabelPaddingX * 2.0, 14.0);
        const QRectF r2(rect_.left() + kLabelPaddingX, rect_.top() + 19.0, rect_.width() - kLabelPaddingX * 2.0, 14.0);
        const QRectF r3(rect_.left() + kLabelPaddingX, rect_.top() + 35.0, rect_.width() - kLabelPaddingX * 2.0, 14.0);

        drawCentered(r1, eventDisplay, effectiveEventTextColor(event_, always_, false));
        drawCentered(r2, QStringLiteral("[%1]").arg(guard_), guardTextColor());
        drawCentered(r3, QStringLiteral("/ %1").arg(action_), effectiveActionTextColor(action_));
    } else {
        // --- Two-line rendering ---
        const QRectF r1(rect_.left() + kLabelPaddingX, rect_.top() + 3.0, rect_.width() - kLabelPaddingX * 2.0, 14.0);
        const QRectF r2(rect_.left() + kLabelPaddingX, rect_.top() + 19.0, rect_.width() - kLabelPaddingX * 2.0, 14.0);

        if (hasAction && hasGuard && hasEvent) {
            // L1: Event [Guard], L2: / Action
            const QString guardStr = QStringLiteral("[%1]").arg(guard_);
            const qreal gap = metrics.horizontalAdvance(QStringLiteral(" "));
            const qreal evW = metrics.horizontalAdvance(eventDisplay);
            const qreal gdW = metrics.horizontalAdvance(guardStr);
            const qreal totalW = evW + gap + gdW;
            qreal curX = r1.left() + std::max(0.0, (r1.width() - totalW) / 2.0);

            painter->setPen(fireable_ ? design::color(design::kTextOnAccent)
                                      : effectiveEventTextColor(event_, always_, false));
            const QString elidedEv = (totalW > r1.width()) ? metrics.elidedText(eventDisplay, Qt::ElideRight, r1.width() * 0.6) : eventDisplay;
            painter->drawText(QRectF(curX, r1.top(), metrics.horizontalAdvance(elidedEv), r1.height()),
                              Qt::AlignVCenter | Qt::AlignLeft, elidedEv);
            curX += metrics.horizontalAdvance(elidedEv) + gap;

            painter->setPen(fireable_ ? design::color(design::kTextOnAccent) : guardTextColor());
            const qreal remainingW = std::max(10.0, r1.right() - curX);
            const QString elidedGd = (metrics.horizontalAdvance(guardStr) > remainingW)
                                         ? metrics.elidedText(guardStr, Qt::ElideRight, remainingW)
                                         : guardStr;
            painter->drawText(QRectF(curX, r1.top(), remainingW, r1.height()),
                              Qt::AlignVCenter | Qt::AlignLeft, elidedGd);

            // L2: / Action
            drawCentered(r2, QStringLiteral("/ %1").arg(action_), effectiveActionTextColor(action_));
        } else if (hasAction && hasGuard && !hasEvent) {
            // L1: [Guard], L2: / Action
            drawCentered(r1, QStringLiteral("[%1]").arg(guard_), guardTextColor());
            drawCentered(r2, QStringLiteral("/ %1").arg(action_), effectiveActionTextColor(action_));
        } else if (hasAction && !hasGuard && hasEvent) {
            // L1: Event, L2: / Action
            drawCentered(r1, eventDisplay, effectiveEventTextColor(event_, always_, false));
            drawCentered(r2, QStringLiteral("/ %1").arg(action_), effectiveActionTextColor(action_));
        } else {
            // !hasAction && hasGuard && hasEvent: L1 = Event, L2 = [Guard]
            drawCentered(r1, eventDisplay, effectiveEventTextColor(event_, always_, false));
            drawCentered(r2, QStringLiteral("[%1]").arg(guard_), guardTextColor());
        }
    }
}

void TransitionLabelItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    if (fireable_ && onFireClicked_ && event->button() == Qt::LeftButton) {
        // Simulate click-to-fire replaces edge selection; Simulate also sets
        // interactive_ false.
        onFireClicked_();
        update();
        event->accept();
        return;
    }
    if (interactive_ && edge_ != nullptr) {
        if (event->button() == Qt::LeftButton) {
            // The click action (select, or edit) is deferred to release: a drag
            // must neither select nor open the editor.
            dragging_ = false;
            grabOffset_ = event->scenePos() - pos();
            event->accept();
            return;
        }
        if (event->button() == Qt::RightButton) {
            // Right-click acts on edge_, keeping a selection it is already in.
            if (!edge_->isSelected()) {
                if (scene() != nullptr && !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
                    scene()->clearSelection();
                }
                edge_->setSelected(true);
            }
            update();
            event->accept();
            return;
        }
    }
    QGraphicsItem::mousePressEvent(event);
}

void TransitionLabelItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (interactive_ && (event->buttons() & Qt::LeftButton)) {
        if (!dragging_ &&
            (event->scenePos() - (pos() + grabOffset_)).manhattanLength() > QApplication::startDragDistance()) {
            dragging_ = true;
        }
        if (dragging_) {
            // Soft snap as in StateItem: each axis snaps only within a few px of a
            // 24px grid line.
            constexpr qreal kGridStep = 24.0;
            constexpr qreal kSnapRadius = 4.0;
            QPointF target = event->scenePos() - grabOffset_;
            const qreal snappedX = std::round(target.x() / kGridStep) * kGridStep;
            const qreal snappedY = std::round(target.y() / kGridStep) * kGridStep;
            if (std::abs(target.x() - snappedX) <= kSnapRadius) {
                target.setX(snappedX);
            }
            if (std::abs(target.y() - snappedY) <= kSnapRadius) {
                target.setY(snappedY);
            }
            setPos(target);  // view-local live move; committed on release
            if (onDragMoved_) {
                onDragMoved_(pos());  // pos() is the pill's center
            }
            event->accept();
            return;
        }
    }
    QGraphicsItem::mouseMoveEvent(event);
}

void TransitionLabelItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    if (interactive_ && edge_ != nullptr && event->button() == Qt::LeftButton) {
        if (dragging_) {
            dragging_ = false;
            if (onDragFinished_) {
                onDragFinished_(pos());  // pos() is the pill's center
            }
            event->accept();
            return;
        }
        // No drag happened: perform the deferred click action.
        if (edge_->isSelected() && onEditRequested_ &&
            !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
            onEditRequested_();
            event->accept();
            return;
        }
        if (scene() != nullptr && !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
            scene()->clearSelection();
        }
        edge_->setSelected(true);
        update();
        event->accept();
        return;
    }
    QGraphicsItem::mouseReleaseEvent(event);
}

void TransitionLabelItem::hoverMoveEvent(QGraphicsSceneHoverEvent* event) {
    if (fireable_) {
        setCursor(Qt::PointingHandCursor);
    } else {
        setCursor(Qt::ArrowCursor);
    }
    event->accept();
}

void TransitionLabelItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) {
    unsetCursor();
    QGraphicsItem::hoverLeaveEvent(event);
}

}  // namespace app
