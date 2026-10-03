#include "view/items/state_item.h"

#include "infra/expression.h"

#include <algorithm>
#include <cmath>

#include <QBrush>
#include <QColor>
#include <QCursor>
#include <QFontMetricsF>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPen>
#include <QStyleOptionGraphicsItem>

#include "constants/design_tokens.h"
#include "view/geometry/element_colors.h"  // elementAccent
#include "view/items/handle_item.h"

namespace app {

namespace {

constexpr qreal kMinWidth = 80.0;
constexpr qreal kPaddingX = 12.0;
constexpr qreal kPaddingY = 8.0;
constexpr qreal kEntryTopGap = 4.0;
constexpr qreal kEntryLineGap = 2.0;
constexpr qreal kCornerRadius = 8.0;
constexpr qreal kFinalRingInset = 4.0;
constexpr int kMaxEntryLines = 4;  // max action lines shown per kind

constexpr qreal kGridStep = 24.0;  // matches CanvasView's grid step

// N, E, S, W; matches handles_'s array order.
constexpr std::array<PortSide, 4> kHandleSides{PortSide::Top, PortSide::Right, PortSide::Bottom, PortSide::Left};

QColor fillColor() { return design::color(design::kNodeFill); }
QColor borderColor() { return design::color(design::kNodeBorder); }  // resting: unselected, unhovered
QColor hoverBorderColor() { return design::color(design::kNodeBorderHover); }
QColor selectedBorderColor() { return design::color(design::kAccentInteractive); }
QColor dropHighlightColor() { return design::color(design::kCanvasHighlight); }
QColor nameColor() { return design::color(design::kNodeName); }
QColor entryColor() { return design::color(design::kNodeBody); }  // dimmer than the name

struct ActionSpan {
    QString text;
    QColor color;
};

void tokenizeArguments(const QString& text, QVector<ActionSpan>& spans) {
    int i = 0;
    const int len = text.size();
    while (i < len) {
        const QChar ch = text[i];
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            const QChar quote = ch;
            const int start = i++;
            while (i < len && text[i] != quote) {
                if (text[i] == QLatin1Char('\\') && i + 1 < len) {
                    i += 2;
                } else {
                    ++i;
                }
            }
            if (i < len) ++i;
            spans.append({text.mid(start, i - start), design::color(design::kLogicLiteral)});  // literal
        } else if (ch.isDigit()) {
            const int start = i++;
            while (i < len && (text[i].isDigit() || text[i] == QLatin1Char('.'))) {
                ++i;
            }
            spans.append({text.mid(start, i - start), design::color(design::kLogicLiteral)});  // number
        } else if (ch.isLetter() || ch == QLatin1Char('_')) {
            const int start = i++;
            while (i < len && (text[i].isLetterOrNumber() || text[i] == QLatin1Char('_'))) {
                ++i;
            }
            spans.append({text.mid(start, i - start), design::color(design::kLogicVariable)});  // identifier
        } else {
            const int start = i++;
            while (i < len && !text[i].isLetterOrNumber() && text[i] != QLatin1Char('_') &&
                   text[i] != QLatin1Char('"') && text[i] != QLatin1Char('\'')) {
                ++i;
            }
            spans.append({text.mid(start, i - start), design::color(design::kCodePunctuation)});  // punctuation
        }
    }
}

QVector<ActionSpan> tokenizeActionLine(const QString& line) {
    QVector<ActionSpan> spans;
    if (line.isEmpty()) {
        return spans;
    }

    int prefixEnd = -1;
    if (line.startsWith(QStringLiteral("entry / "))) {
        prefixEnd = 8;
        spans.append({QStringLiteral("entry / "), design::color(design::kLogicHook)});
    } else if (line.startsWith(QStringLiteral("exit / "))) {
        prefixEnd = 7;
        spans.append({QStringLiteral("exit / "), design::color(design::kLogicHook)});
    } else if (line.startsWith(QStringLiteral("entry: "))) {
        prefixEnd = 7;
        spans.append({QStringLiteral("entry / "), design::color(design::kLogicHook)});
    } else if (line.startsWith(QStringLiteral("exit: "))) {
        prefixEnd = 6;
        spans.append({QStringLiteral("exit / "), design::color(design::kLogicHook)});
    }

    const QString body = (prefixEnd >= 0) ? line.mid(prefixEnd) : line;
    if (body.isEmpty()) {
        return spans;
    }

    if (expr::parseSendToForm(body).ok || expr::parseSendParentForm(body).ok) {
        const int openParen = body.indexOf(QLatin1Char('('));
        if (openParen > 0) {
            spans.append({body.left(openParen), design::color(design::kLogicSend)});
            tokenizeArguments(body.mid(openParen), spans);
            return spans;
        }
        spans.append({body, design::color(design::kLogicSend)});
        return spans;
    }

    if (expr::parseRaiseForm(body).ok) {
        const int openParen = body.indexOf(QLatin1Char('('));
        if (openParen > 0) {
            spans.append({body.left(openParen), design::color(design::kLogicRaise)});
            tokenizeArguments(body.mid(openParen), spans);
            return spans;
        }
        spans.append({body, design::color(design::kLogicRaise)});
        return spans;
    }

    if (expr::parseAssignForm(body).ok) {
        const int openParen = body.indexOf(QLatin1Char('('));
        if (openParen > 0) {
            spans.append({body.left(openParen), design::color(design::kLogicVariable)});  // assign target
            tokenizeArguments(body.mid(openParen), spans);
            return spans;
        }
        spans.append({body, design::color(design::kLogicVariable)});
        return spans;
    }

    // Any other in-node action call (not raise/sendTo/sendParent/assign) is a
    // plain action hook -- the same thing a transition label paints in
    // logic-action, so the node body uses that role too.
    const int openParen = body.indexOf(QLatin1Char('('));
    if (openParen > 0) {
        spans.append({body.left(openParen), design::color(design::kLogicAction)});
        tokenizeArguments(body.mid(openParen), spans);
    } else {
        spans.append({body, design::color(design::kLogicAction)});
    }

    return spans;
}

void drawActionSpans(QPainter* painter, const QRectF& lineRect, const QVector<ActionSpan>& spans,
                     const QFontMetricsF& metrics, qreal maxWidth = -1.0) {
    painter->save();
    if (maxWidth > 0.0) {
        painter->setClipRect(lineRect);
    }
    qreal curX = lineRect.left();
    const qreal baselineY = lineRect.top();
    const qreal h = lineRect.height();

    for (const auto& span : spans) {
        if (maxWidth > 0.0 && curX >= lineRect.left() + maxWidth) {
            break;
        }
        const qreal spanWidth = metrics.horizontalAdvance(span.text);
        painter->setPen(span.color);
        painter->drawText(QRectF(curX, baselineY, spanWidth + 1.0, h), Qt::AlignLeft | Qt::AlignVCenter, span.text);
        curX += spanWidth;
    }
    painter->restore();
}

// Container styling mirrors MachineFrameItem's header. kContainerHeaderHeight
// is the name line only; containerHeaderHeight() adds the action-line block.
constexpr qreal kContainerHeaderHeight = 28.0;
constexpr qreal kContainerCornerRadius = 10.0;
constexpr qreal kContainerBorderBandWidth = 8.0;
QColor containerFillColor() { return design::color(design::kContainerFill); }
QColor containerBorderColor() { return design::color(design::kContainerBorder); }
QColor containerHeaderFillColor() { return design::color(design::kContainerHeaderFill); }
QColor containerHeaderTextColor() { return design::color(design::kContainerHeaderText); }

// Simulate-mode presentation; CanvasPresenter::updateSimPresentation() is the
// only caller of setDimmed/setActiveGlow.
constexpr qreal kDimmedOpacity = 0.3;
// The active border reuses the selection blue; the two never coincide because
// Simulate disables selection.
constexpr qreal kActiveGlowBorderWidth = 2.0;
QColor activeGlowShadowColor() { return design::color(design::kNodeActiveGlow); }
constexpr qreal kActiveGlowBlurRadius = 16.0;

// "entry / x()" / "exit / y()" lines, capped at kMaxEntryLines each.
QStringList formatActionLines(const QString& prefix, const QStringList& actions) {
    QStringList lines;
    const int count = std::min(static_cast<int>(actions.size()), kMaxEntryLines);
    for (int i = 0; i < count; ++i) {
        lines.push_back(QStringLiteral("%1 / %2").arg(prefix, actions[i]));
    }
    return lines;
}

QStringList visibleEntryLines(const QStringList& entryActions) {
    return formatActionLines(QStringLiteral("entry"), entryActions);
}

QStringList visibleExitLines(const QStringList& exitActions) {
    return formatActionLines(QStringLiteral("exit"), exitActions);
}

}  // namespace

namespace {

// The "+" quick handle: a small circled plus at the selected node's top-right
// corner. Click-only input sensor; the menu it opens lives on CanvasPresenter.
class QuickAddHandleItem : public QGraphicsItem {
public:
    explicit QuickAddHandleItem(std::function<void()> onClicked, QGraphicsItem* parent)
        : QGraphicsItem(parent), onClicked_(std::move(onClicked)) {
        setAcceptedMouseButtons(Qt::LeftButton);
        setCursor(Qt::PointingHandCursor);
    }

    QRectF boundingRect() const override { return QRectF(-9.0, -9.0, 18.0, 18.0); }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(Qt::NoPen);
        painter->setBrush(design::color(design::kAccentInteractive));        painter->drawEllipse(QRectF(-8.0, -8.0, 16.0, 16.0));
        painter->setPen(QPen(design::color(design::kTextOnAccent), 1.8, Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(QPointF(-4.0, 0.0), QPointF(4.0, 0.0));
        painter->drawLine(QPointF(0.0, -4.0), QPointF(0.0, 4.0));
    }

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        if (onClicked_) {
            onClicked_();
        }
        event->accept();
    }

private:
    std::function<void()> onClicked_;
};

}  // namespace

StateItem::StateItem(quint64 id, QString name, StateKind kind, QStringList entryActions, QStringList exitActions)
    : id_(id),
      name_(std::move(name)),
      kind_(kind),
      entryActions_(std::move(entryActions)),
      exitActions_(std::move(exitActions)) {
    nameFont_.setBold(true);
    nameFont_.setPointSizeF(10.0);
    entryFont_.setPointSizeF(8.0);

    // ItemSendsGeometryChanges so itemChange can snap and re-route live.
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
    setZValue(10.0);  // grid < edges(0) < nodes(10) < labels(20)

    // Created before recomputeGeometry() below, which repositions them.
    for (int i = 0; i < 4; ++i) {
        auto* handle = new HandleItem(kHandleSides[i], this);
        handle->setVisible(false);  // hidden until selected
        handle->setOnDragStarted([this, side = kHandleSides[i]] {
            if (onWireDragStarted_) {
                onWireDragStarted_(id_, side);
            }
        });
        handle->setOnDragMoved([this](QPointF scenePos) {
            if (onWireDragMoved_) {
                onWireDragMoved_(scenePos);
            }
        });
        handle->setOnDragFinished([this](QPointF scenePos) {
            if (onWireDragFinished_) {
                onWireDragFinished_(scenePos);
            }
        });
        handles_[i] = handle;
    }
    quickAddHandle_ = new QuickAddHandleItem(
        [this] {
            if (onQuickAddClicked_) {
                onQuickAddClicked_(id_);
            }
        },
        this);
    quickAddHandle_->setVisible(false);  // shown with the side handles, on selection

    recomputeGeometry();
}

void StateItem::setOnQuickAddClicked(std::function<void(quint64)> cb) { onQuickAddClicked_ = std::move(cb); }
void StateItem::setPositionAdjuster(std::function<QPointF(QPointF)> adjuster) { positionAdjuster_ = std::move(adjuster); }

void StateItem::setName(const QString& name) {
    if (name_ == name) {
        return;
    }
    name_ = name;
    recomputeGeometry();
}

void StateItem::setKind(StateKind kind) {
    if (kind_ == kind) {
        return;
    }
    kind_ = kind;
    update();
}

void StateItem::setEntryActions(const QStringList& entryActions) {
    if (entryActions_ == entryActions) {
        return;
    }
    entryActions_ = entryActions;
    recomputeGeometry();
}

void StateItem::setColor(ElementColor color) {
    if (color_ == color) {
        return;
    }
    color_ = color;
    update();
}

void StateItem::setExitActions(const QStringList& exitActions) {
    if (exitActions_ == exitActions) {
        return;
    }
    exitActions_ = exitActions;
    // No recomputeGeometry(): exitActions_ never sizes the leaf rect_, so a
    // repaint suffices.
    update();
}

QRectF StateItem::sceneRect() const {
    return QRectF(pos(), containerMode_ ? containerRect_.size() : rect_.size());
}

void StateItem::setOnMoved(std::function<void()> cb) { onMoved_ = std::move(cb); }
void StateItem::setOnDragStarted(std::function<void()> cb) { onDragStarted_ = std::move(cb); }
void StateItem::setOnDragFinished(std::function<void(QPointF)> cb) { onDragFinished_ = std::move(cb); }
void StateItem::setOnDragReverted(std::function<void()> cb) { onDragReverted_ = std::move(cb); }
void StateItem::setOnRenameRequested(std::function<void()> cb) { onRenameRequested_ = std::move(cb); }
void StateItem::setOnWireDragStarted(std::function<void(quint64, PortSide)> cb) { onWireDragStarted_ = std::move(cb); }
void StateItem::setOnWireDragMoved(std::function<void(QPointF)> cb) { onWireDragMoved_ = std::move(cb); }
void StateItem::setOnWireDragFinished(std::function<void(QPointF)> cb) { onWireDragFinished_ = std::move(cb); }

void StateItem::setContainerGeometry(const QRectF& sceneRect) {
    prepareGeometryChange();
    containerMode_ = true;
    setPos(sceneRect.topLeft());
    containerRect_ = QRectF(0.0, 0.0, sceneRect.width(), sceneRect.height());
    // Handles follow the derived box; the visibility refresh covers a leaf
    // turning into a container while selected.
    repositionHandles();
    updateHandleVisibility();
    update();
}

void StateItem::setLeafMode() {
    if (!containerMode_) {
        return;    }
    prepareGeometryChange();
    containerMode_ = false;
    setZValue(10.0);  // container z is presenter-assigned
    recomputeGeometry();       // rebuilds rect_ and repositions the handles
    updateHandleVisibility();  // may already be selected
}

qreal StateItem::containerHeaderHeight() const {
    // Name row plus action lines; unlike the leaf rect this counts exit lines too.
    qreal height = kContainerHeaderHeight;
    const QStringList entryLines = visibleEntryLines(entryActions_);
    const QStringList exitLines = visibleExitLines(exitActions_);
    const int lineCount = static_cast<int>(entryLines.size() + exitLines.size());
    if (lineCount > 0) {
        const QFontMetricsF entryMetrics(entryFont_);
        height += kEntryTopGap + lineCount * entryMetrics.height() + std::max(0, lineCount - 1) * kEntryLineGap;
    }
    return height;
}

qreal StateItem::containerMinHeaderWidth() const {
    const QFontMetricsF nameMetrics(nameFont_);
    return nameMetrics.horizontalAdvance(name_.isEmpty() ? QStringLiteral(" ") : name_) + kPaddingX * 2.0;
}

void StateItem::setDropHighlighted(bool highlighted) {
    if (dropHighlighted_ == highlighted) {
        return;
    }
    dropHighlighted_ = highlighted;
    update();
}

void StateItem::setDimmed(bool dimmed) {
    if (dimmed_ == dimmed) {
        return;
    }
    dimmed_ = dimmed;
    setOpacity(dimmed_ ? kDimmedOpacity : 1.0);
}

void StateItem::setActiveGlow(bool active) {
    if (activeGlow_ == active) {
        return;
    }
    activeGlow_ = active;
    // prepareGeometryChange() first: the effect changes the bounding rect and
    // Qt does not re-index the item or its children on its own.
    if (activeGlow_) {
        auto* effect = new QGraphicsDropShadowEffect();
        effect->setColor(activeGlowShadowColor());
        effect->setBlurRadius(kActiveGlowBlurRadius);
        effect->setOffset(0.0, 0.0);
        prepareGeometryChange();
        setGraphicsEffect(effect);  // takes ownership, deleting any prior effect
    } else {
        prepareGeometryChange();
        setGraphicsEffect(nullptr);  // deletes the effect
    }
    update();  // paint()'s border color depends on activeGlow_
}

void StateItem::setBreakpoint(bool hasBp) {
    if (hasBreakpoint_ == hasBp) {
        return;
    }
    hasBreakpoint_ = hasBp;
    update();
}

void StateItem::setBreakpointHit(bool hit) {
    if (breakpointHit_ == hit) {
        return;
    }
    breakpointHit_ = hit;
    if (breakpointHit_) {
        auto* effect = new QGraphicsDropShadowEffect();
        effect->setColor(design::color(design::kBreakpointHalo));  // Amber halo on breakpoint hit
        effect->setBlurRadius(20.0);
        effect->setOffset(0.0, 0.0);
        prepareGeometryChange();  // same index re-filing as setActiveGlow
        setGraphicsEffect(effect);
    } else if (activeGlow_) {
        auto* effect = new QGraphicsDropShadowEffect();
        effect->setColor(activeGlowShadowColor());
        effect->setBlurRadius(kActiveGlowBlurRadius);
        effect->setOffset(0.0, 0.0);
        prepareGeometryChange();
        setGraphicsEffect(effect);
    } else {
        prepareGeometryChange();
        setGraphicsEffect(nullptr);
    }
    update();
}

void StateItem::setOnBreakpointToggled(std::function<void(quint64)> cb) {
    onBreakpointToggled_ = std::move(cb);
}

QRectF StateItem::boundingRect() const {
    // Container: +-1 pen-width margin.
    return containerMode_ ? containerRect_.adjusted(-1.0, -1.0, 1.0, 1.0) : rect_;
}

QPainterPath StateItem::shape() const {
    if (!containerMode_) {
        QPainterPath path;
        path.addRect(rect_);
        return path;
    }
    // Container: border ring + header band only, so the interior stays click-through.
    QPainterPath path;
    // Header band, full width, as tall as containerHeaderHeight().
    path.addRect(QRectF(0.0, 0.0, containerRect_.width(), containerHeaderHeight()));

    QPainterPath outer;
    outer.addRoundedRect(containerRect_, kContainerCornerRadius, kContainerCornerRadius);
    QPainterPath inner;
    const QRectF innerRect = containerRect_.adjusted(kContainerBorderBandWidth, kContainerBorderBandWidth,
                                                       -kContainerBorderBandWidth, -kContainerBorderBandWidth);
    const qreal innerRadius =
        kContainerCornerRadius > kContainerBorderBandWidth ? kContainerCornerRadius - kContainerBorderBandWidth : 0.0;
    inner.addRoundedRect(innerRect, innerRadius, innerRadius);

    path.addPath(outer.subtracted(inner));
    return path;
}

void StateItem::recomputeGeometry() {
    prepareGeometryChange();

    const QFontMetricsF nameMetrics(nameFont_);
    const QFontMetricsF entryMetrics(entryFont_);
    const QStringList entryLines = visibleEntryLines(entryActions_);

    qreal textWidth = nameMetrics.horizontalAdvance(name_.isEmpty() ? QStringLiteral(" ") : name_);
    for (const QString& line : entryLines) {
        textWidth = std::max(textWidth, entryMetrics.horizontalAdvance(line));
    }

    const qreal width = std::max(kMinWidth, textWidth + kPaddingX * 2.0);
    qreal height = kPaddingY * 2.0 + nameMetrics.height();
    if (!entryLines.isEmpty()) {
        height += kEntryTopGap + entryLines.size() * entryMetrics.height() +
                  std::max(0, static_cast<int>(entryLines.size()) - 1) * kEntryLineGap;
    }

    rect_ = QRectF(0.0, 0.0, width, height);
    repositionHandles();
}

void StateItem::repositionHandles() {
    // Handles track the box currently drawn (rect_ or containerRect_, both local
    // with (0,0) top-left); setContainerGeometry() re-runs this on every change.
    const QRectF& box = containerMode_ ? containerRect_ : rect_;
    for (int i = 0; i < 4; ++i) {
        if (handles_[i] != nullptr) {
            handles_[i]->setPos(sidePortAnchor(box, kHandleSides[i], 0.0));
        }
    }
    if (quickAddHandle_ != nullptr) {
        quickAddHandle_->setPos(box.right() + 12.0, box.top() - 12.0);
    }
}

void StateItem::updateHandleVisibility() {
    // Leaf and container modes both show the handles while selected.
    const bool visible = isSelected();
    for (HandleItem* handle : handles_) {
        if (handle != nullptr) {
            handle->setVisible(visible);
        }
    }
    if (quickAddHandle_ != nullptr) {
        quickAddHandle_->setVisible(visible);
    }
}

bool StateItem::debugSideHandlesVisible() const {
    for (const HandleItem* handle : handles_) {
        if (handle == nullptr || !handle->isVisible()) {
            return false;
        }
    }
    return true;
}

QPointF StateItem::debugHandleScenePos(PortSide side) const {
    for (int i = 0; i < 4; ++i) {
        if (kHandleSides[i] == side && handles_[i] != nullptr) {
            return handles_[i]->scenePos();
        }
    }
    return QPointF();
}

QVariant StateItem::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemPositionChange && scene() != nullptr) {
        // Soft snap: each axis snaps independently, only within kSnapRadius of a
        // grid line. View-local; MoveStateRequested is sent only from
        // mouseReleaseEvent.
        constexpr qreal kSnapRadius = 4.0;
        const QPointF proposed = value.toPointF();
        QPointF newPos = proposed;

        // Alignment guides go first: an axis the adjuster moved skips the grid snap.
        bool xAligned = false;
        bool yAligned = false;
        if (positionAdjuster_) {
            newPos = positionAdjuster_(proposed);
            xAligned = newPos.x() != proposed.x();
            yAligned = newPos.y() != proposed.y();
        }

        // Snap only while this item is under the user's button. Other setPos
        // calls are derived (container hulls, subtree deltas, undo echoes);
        // snapping them makes parent and children chase each other through
        // onMoved_ until the stack overflows.
        if (leftPressed_) {
            if (!xAligned) {
                const qreal snappedX = std::round(newPos.x() / kGridStep) * kGridStep;
                if (std::abs(newPos.x() - snappedX) <= kSnapRadius) {
                    newPos.setX(snappedX);
                }
            }
            if (!yAligned) {
                const qreal snappedY = std::round(newPos.y() / kGridStep) * kGridStep;
                if (std::abs(newPos.y() - snappedY) <= kSnapRadius) {
                    newPos.setY(snappedY);
                }
            }
        }
        return newPos;
    }
    if (change == ItemPositionHasChanged && scene() != nullptr) {
        // A held press becoming a drag is reported before the re-route so the
        // session is open while geometry updates; presenter-driven setPos has no
        // button down and never counts.
        if (leftPressed_ && !dragReported_ && pos() != dragStartPos_) {
            dragReported_ = true;
            if (onDragStarted_) {
                onDragStarted_();
            }
        }
        // Live re-route goes through the presenter: routing recomputes a whole
        // per-side port group at once, so this item keeps no list of transitions.
        if (onMoved_) {
            onMoved_();
        }
        return value;
    }
    if (change == ItemSelectedHasChanged) {
        updateHandleVisibility();
        update();
        return value;
    }
    return QGraphicsItem::itemChange(change, value);
}

void StateItem::hoverEnterEvent(QGraphicsSceneHoverEvent* event) {
    hovered_ = true;
    setCursor(Qt::PointingHandCursor);
    update();
    QGraphicsItem::hoverEnterEvent(event);
}

void StateItem::hoverLeaveEvent(QGraphicsSceneHoverEvent* event) {
    hovered_ = false;
    unsetCursor();
    update();
    QGraphicsItem::hoverLeaveEvent(event);
}

void StateItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        dragStartPos_ = pos();
        lastMouseScenePos_ = event->scenePos();
        leftPressed_ = true;
        dragReported_ = false;
        // Sampled before the base impl, whose press selects the item; afterwards
        // a first click and a click on a selected item look the same.
        wasSelectedAtPress_ = isSelected();
    }
    // Base impl handles selection and the move (via itemChange).
    QGraphicsItem::mousePressEvent(event);
}

void StateItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    lastMouseScenePos_ = event->scenePos();
    QGraphicsItem::mouseMoveEvent(event);
}

void StateItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    lastMouseScenePos_ = event->scenePos();
    QGraphicsItem::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton) {
        return;
    }
    // Cleared before any callback runs: a commit that echoes back as a
    // setPos must not look like more of this gesture.
    const bool reported = dragReported_;
    leftPressed_ = false;
    dragReported_ = false;
    // Commit only on actual movement; a plain click never sends MoveStateRequested.
    if (pos() != dragStartPos_) {
        if (onDragFinished_) {
            onDragFinished_(pos());
        }
        return;
    }
    // Reported, yet back on the press position: nothing to commit, but the
    // session opened by onDragStarted_ still has to close.
    if (reported && onDragReverted_) {
        onDragReverted_();
    }

    // A stationary release in the top-left 18x18 zone toggles the breakpoint.
    if (onBreakpointToggled_) {
        const QRectF bpClickArea(0.0, 0.0, 18.0, 18.0);
        if (bpClickArea.contains(event->pos())) {
            onBreakpointToggled_(id_);
            return;
        }
    }

    // Rename gesture; isSelected() is re-checked so a Ctrl+click that just
    // deselected the item does not rename it.
    if (wasSelectedAtPress_ && isSelected() && onRenameRequested_) {
        onRenameRequested_();
    }
}

void StateItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    if (containerMode_) {
        paintContainer(painter);
        return;
    }
    painter->setRenderHint(QPainter::Antialiasing);

    painter->setBrush(QBrush(fillColor()));
    QColor border;
    qreal borderWidth;
    if (breakpointHit_) {
        border = design::color(design::kBreakpointHit);  // Amber halo border on breakpoint hit
        borderWidth = 3.0;
    } else if (activeGlow_) {
        // Simulate's active state: highest precedence.
        border = selectedBorderColor();
        borderWidth = kActiveGlowBorderWidth;
    } else if (dropHighlighted_) {
        // Wins over selected/hover during another node's wire drag.
        border = dropHighlightColor();
        borderWidth = 2.0;
    } else if (isSelected()) {
        border = selectedBorderColor();
        borderWidth = 2.0;
    } else if (hovered_) {
        border = hoverBorderColor();
        borderWidth = 1.0;
    } else if (color_ != ElementColor::Default) {
        // The resting border carries the element color.
        border = elementAccent(color_);
        borderWidth = 1.5;
    } else {
        border = borderColor();
        borderWidth = 1.0;
    }
    painter->setPen(QPen(border, borderWidth));
    painter->drawRoundedRect(rect_, kCornerRadius, kCornerRadius);

    if (kind_ == StateKind::Final) {
        // Double-ring: a second inset border, no fill, same subtle color.
        const QRectF inner = rect_.adjusted(kFinalRingInset, kFinalRingInset, -kFinalRingInset, -kFinalRingInset);
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(inner, std::max(0.0, kCornerRadius - kFinalRingInset),
                                  std::max(0.0, kCornerRadius - kFinalRingInset));
    }
    // The initial state has no box-level treatment; the presenter places a
    // separate marker item next to it.

    const QFontMetricsF nameMetrics(nameFont_);
    QRectF nameRect(rect_.left() + kPaddingX, rect_.top() + kPaddingY, rect_.width() - kPaddingX * 2.0,
                    nameMetrics.height());
    painter->setFont(nameFont_);
    painter->setPen(nameColor());
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, name_);

    const QStringList entryLines = visibleEntryLines(entryActions_);
    if (!entryLines.isEmpty()) {
        const QFontMetricsF entryMetrics(entryFont_);
        painter->setFont(entryFont_);
        qreal y = nameRect.bottom() + kEntryTopGap;
        for (const QString& line : entryLines) {
            const QRectF lineRect(rect_.left() + kPaddingX, y, rect_.width() - kPaddingX * 2.0, entryMetrics.height());
            const auto spans = tokenizeActionLine(line);
            drawActionSpans(painter, lineRect, spans, entryMetrics);
            y += entryMetrics.height() + kEntryLineGap;
        }
    }

    // Breakpoint pin badge (top-left 16x16 affordance)
    if (hasBreakpoint_ || hovered_) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QPointF pinCenter(rect_.left() + 8.0, rect_.top() + 8.0);
        if (hasBreakpoint_) {
            const QColor pinBg =
                breakpointHit_ ? design::color(design::kBreakpointHit) : design::color(design::kBreakpoint);
            painter->setPen(QPen(design::color(design::kTextOnAccent), 1.2));
            painter->setBrush(pinBg);
            painter->drawEllipse(pinCenter, 4.5, 4.5);
            painter->setPen(Qt::NoPen);
            painter->setBrush(design::color(design::kTextOnAccent));
            painter->drawEllipse(pinCenter, 1.5, 1.5);
        } else if (hovered_) {
            painter->setPen(QPen(design::color(design::kBreakpointRing), 1.0));
            painter->setBrush(design::color(design::kBreakpointFill));
            painter->drawEllipse(pinCenter, 4.0, 4.0);
        }
        painter->restore();
    }
}

// Container mode's paint(): rounded fill and border with a clipped header band.
// Parallel gets a dashed border; its regions are just its children's boxes.
void StateItem::paintContainer(QPainter* painter) const {
    painter->setRenderHint(QPainter::Antialiasing);

    QColor border;
    qreal borderWidth;
    if (breakpointHit_) {
        border = design::color(design::kBreakpointHit);  // Amber halo border on breakpoint hit
        borderWidth = 3.0;
    } else if (activeGlow_) {
        // Same precedence order as the leaf branch.
        border = selectedBorderColor();
        borderWidth = kActiveGlowBorderWidth;
    } else if (dropHighlighted_) {
        border = dropHighlightColor();
        borderWidth = 2.0;
    } else if (isSelected()) {
        border = selectedBorderColor();
        borderWidth = 2.0;
    } else if (hovered_) {
        border = hoverBorderColor();
        borderWidth = 1.0;
    } else if (color_ != ElementColor::Default) {
        border = elementAccent(color_);        borderWidth = 1.5;
    } else {
        border = containerBorderColor();
        borderWidth = 1.5;
    }
    QPen pen(border, borderWidth);
    if (kind_ == StateKind::Parallel) {
        pen.setStyle(Qt::DashLine);
    }
    painter->setPen(pen);
    painter->setBrush(QBrush(containerFillColor()));
    painter->drawRoundedRect(containerRect_, kContainerCornerRadius, kContainerCornerRadius);

    // Header band, clipped to the rounded top corners; grows past the name row
    // when action lines need room.
    const qreal headerHeight = containerHeaderHeight();
    QPainterPath clip;
    clip.addRoundedRect(containerRect_, kContainerCornerRadius, kContainerCornerRadius);
    painter->save();
    painter->setClipPath(clip);
    painter->fillRect(QRectF(0.0, 0.0, containerRect_.width(), headerHeight), containerHeaderFillColor());
    painter->restore();

    // A container's width comes from its children and can be narrower than the
    // name, so the name is elided rather than overflowing.
    const qreal textWidth = containerRect_.width() - kPaddingX * 2.0;
    const QFontMetricsF nameMetrics(nameFont_);
    painter->setFont(nameFont_);
    painter->setPen(containerHeaderTextColor());
    painter->drawText(QRectF(kPaddingX, 0.0, textWidth, kContainerHeaderHeight), Qt::AlignLeft | Qt::AlignVCenter,
                      nameMetrics.elidedText(name_, Qt::ElideRight, textWidth));

    // Entry lines then exit lines below the name row, each elided like the name.
    const QStringList entryLines = visibleEntryLines(entryActions_);
    const QStringList exitLines = visibleExitLines(exitActions_);
    if (!entryLines.isEmpty() || !exitLines.isEmpty()) {
        const QFontMetricsF entryMetrics(entryFont_);
        painter->setFont(entryFont_);
        qreal y = kContainerHeaderHeight + kEntryTopGap;
        for (const QStringList* lines : {&entryLines, &exitLines}) {
            for (const QString& line : *lines) {
                const QRectF lineRect(kPaddingX, y, textWidth, entryMetrics.height());
                const QString displayLine = (entryMetrics.horizontalAdvance(line) > textWidth)
                                                ? entryMetrics.elidedText(line, Qt::ElideRight, textWidth)
                                                : line;
                const auto spans = tokenizeActionLine(displayLine);
                drawActionSpans(painter, lineRect, spans, entryMetrics, textWidth);
                y += entryMetrics.height() + kEntryLineGap;
            }
        }
    }

    // Breakpoint pin badge for container mode
    if (hasBreakpoint_ || hovered_) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QPointF pinCenter(containerRect_.left() + 8.0, containerRect_.top() + 8.0);
        if (hasBreakpoint_) {
            const QColor pinBg =
                breakpointHit_ ? design::color(design::kBreakpointHit) : design::color(design::kBreakpoint);
            painter->setPen(QPen(design::color(design::kTextOnAccent), 1.2));
            painter->setBrush(pinBg);
            painter->drawEllipse(pinCenter, 4.5, 4.5);
            painter->setPen(Qt::NoPen);
            painter->setBrush(design::color(design::kTextOnAccent));
            painter->drawEllipse(pinCenter, 1.5, 1.5);
        } else if (hovered_) {
            painter->setPen(QPen(design::color(design::kBreakpointRing), 1.0));
            painter->setBrush(design::color(design::kBreakpointFill));
            painter->drawEllipse(pinCenter, 4.0, 4.0);
        }
        painter->restore();
    }
}

}  // namespace app
