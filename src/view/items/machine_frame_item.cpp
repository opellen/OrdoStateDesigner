#include "view/items/machine_frame_item.h"

#include <QColor>
#include <QFont>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPen>

#include "constants/design_tokens.h"
#include "view/geometry/edge_router.h"  // PortSide, sidePortAnchor
#include "view/items/handle_item.h"

namespace app {

namespace {

// N, E, S, W; matches handles_'s array order.
constexpr std::array<PortSide, 4> kHandleSides{PortSide::Top, PortSide::Right, PortSide::Bottom, PortSide::Left};

constexpr qreal kCornerRadius = 10.0;
constexpr qreal kHeaderHeight = 28.0;
// Hit-surface width of the border band.
constexpr qreal kBorderBandWidth = 8.0;
// Translucent so the canvas grid shows through the interior.
QColor frameFill() { return design::color(design::kFrameFill); }
QColor frameBorder() { return design::color(design::kContainerBorder); }
QColor headerFill() { return design::color(design::kContainerHeaderFill); }
QColor headerText() { return design::color(design::kContainerHeaderText); }
QColor selectedBorderColor() { return design::color(design::kAccentInteractive); }

}  // namespace

MachineFrameItem::MachineFrameItem() {
    setZValue(-10.0);  // under the edge band (0)
    // Right button too: otherwise a right-click on the title reaches the scene
    // as an empty-canvas press and clears the selection before the menu opens.
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);

    // Wire-drag side handles: hidden until selected, placed by repositionHandles().
    for (int i = 0; i < 4; ++i) {
        auto* handle = new HandleItem(kHandleSides[i], this);
        handle->setVisible(false);
        handle->setOnDragStarted([this, side = kHandleSides[i]] {
            if (onWireDragStarted_) {
                onWireDragStarted_(side);
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
}

void MachineFrameItem::setGeometryAndName(const QRectF& sceneRect, const QString& name) {
    prepareGeometryChange();
    setPos(sceneRect.topLeft());
    rect_ = QRectF(0.0, 0.0, sceneRect.width(), sceneRect.height());
    name_ = name;
    repositionHandles();  // the frame refits constantly; keep handles on it
    update();
}

void MachineFrameItem::setOnDragStarted(std::function<void(QPointF)> cb) { onDragStarted_ = std::move(cb); }
void MachineFrameItem::setOnDragMoved(std::function<void(QPointF)> cb) { onDragMoved_ = std::move(cb); }
void MachineFrameItem::setOnDragFinished(std::function<void(QPointF)> cb) { onDragFinished_ = std::move(cb); }
void MachineFrameItem::setOnDragReverted(std::function<void()> cb) { onDragReverted_ = std::move(cb); }
void MachineFrameItem::setOnWireDragStarted(std::function<void(PortSide)> cb) { onWireDragStarted_ = std::move(cb); }
void MachineFrameItem::setOnWireDragMoved(std::function<void(QPointF)> cb) { onWireDragMoved_ = std::move(cb); }
void MachineFrameItem::setOnWireDragFinished(std::function<void(QPointF)> cb) { onWireDragFinished_ = std::move(cb); }

void MachineFrameItem::repositionHandles() {
    for (int i = 0; i < 4; ++i) {
        if (handles_[i] != nullptr) {
            handles_[i]->setPos(sidePortAnchor(rect_, kHandleSides[i], 0.0));
        }
    }
}

void MachineFrameItem::updateHandleVisibility() {
    const bool visible = isSelected();
    for (HandleItem* handle : handles_) {
        if (handle != nullptr) {
            handle->setVisible(visible);
        }
    }
}

bool MachineFrameItem::debugSideHandlesVisible() const {
    for (const HandleItem* handle : handles_) {
        if (handle == nullptr || !handle->isVisible()) {
            return false;
        }
    }
    return true;
}

QPointF MachineFrameItem::debugHandleScenePos(PortSide side) const {
    for (int i = 0; i < 4; ++i) {
        if (kHandleSides[i] == side && handles_[i] != nullptr) {
            return handles_[i]->scenePos();
        }
    }
    return QPointF();
}

QVariant MachineFrameItem::itemChange(GraphicsItemChange change, const QVariant& value) {
    if (change == ItemSelectedHasChanged) {
        updateHandleVisibility();
        update();
        return value;
    }
    return QGraphicsItem::itemChange(change, value);
}

QRectF MachineFrameItem::boundingRect() const { return rect_.adjusted(-1.0, -1.0, 1.0, 1.0); }

QPainterPath MachineFrameItem::shape() const {
    QPainterPath path;
    path.addRect(QRectF(0.0, 0.0, rect_.width(), kHeaderHeight));  // header band, full width

    QPainterPath outer;
    outer.addRoundedRect(rect_, kCornerRadius, kCornerRadius);
    QPainterPath inner;
    const QRectF innerRect = rect_.adjusted(kBorderBandWidth, kBorderBandWidth, -kBorderBandWidth, -kBorderBandWidth);
    const qreal innerRadius = kCornerRadius > kBorderBandWidth ? kCornerRadius - kBorderBandWidth : 0.0;
    inner.addRoundedRect(innerRect, innerRadius, innerRadius);

    path.addPath(outer.subtracted(inner));
    return path;
}

void MachineFrameItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);
    const bool selected = isSelected();
    painter->setPen(QPen(selected ? selectedBorderColor() : frameBorder(), selected ? 2.0 : 1.5));
    painter->setBrush(frameFill());
    painter->drawRoundedRect(rect_, kCornerRadius, kCornerRadius);

    // Header band, clipped so it follows the rounded top corners.
    QPainterPath clip;
    clip.addRoundedRect(rect_, kCornerRadius, kCornerRadius);
    painter->save();
    painter->setClipPath(clip);
    painter->fillRect(QRectF(0.0, 0.0, rect_.width(), kHeaderHeight), headerFill());
    painter->restore();

    QFont font = painter->font();
    font.setPixelSize(13);
    font.setBold(true);
    painter->setFont(font);
    painter->setPen(headerText());
    painter->drawText(QRectF(12.0, 0.0, rect_.width() - 24.0, kHeaderHeight), Qt::AlignVCenter | Qt::AlignLeft,
                      name_.trimmed().isEmpty() ? QStringLiteral("(unnamed machine)") : name_);
}

void MachineFrameItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    // Right press keeps an existing selection, else selects this frame. The base
    // impl ignores non-left presses here and the scene would clear the selection.
    if (event->button() == Qt::RightButton) {
        if (!(flags() & ItemIsSelectable)) {
            event->ignore();  // Simulate mode: nothing to select, the view's menu still opens
            return;
        }
        if (!isSelected()) {
            if (scene() != nullptr && !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
                scene()->clearSelection();
            }
            setSelected(true);
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        dragStartScenePos_ = event->scenePos();
        leftPressed_ = true;
        dragReported_ = false;
    }
    // Base impl handles selection; the item is not movable, the drag below moves
    // the states via the presenter.
    QGraphicsItem::mousePressEvent(event);
}

void MachineFrameItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (!leftPressed_) {
        QGraphicsItem::mouseMoveEvent(event);
        return;
    }
    event->accept();
    const QPointF scenePos = event->scenePos();
    // A held press becomes a drag: report once, with the press position as delta origin.
    if (!dragReported_ && scenePos != dragStartScenePos_) {
        dragReported_ = true;
        if (onDragStarted_) {
            onDragStarted_(dragStartScenePos_);
        }
    }
    if (dragReported_ && onDragMoved_) {
        onDragMoved_(scenePos);
    }
}

void MachineFrameItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    QGraphicsItem::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const bool reported = dragReported_;
    leftPressed_ = false;
    dragReported_ = false;
    if (!reported) {
        return;  // plain click, already selected by the base impl
    }
    // Commit only on actual movement; a drag that returns exactly to the press
    // position just closes the session.
    if (event->scenePos() != dragStartScenePos_) {
        if (onDragFinished_) {
            onDragFinished_(event->scenePos());
        }
    } else if (onDragReverted_) {
        onDragReverted_();
    }
}

}  // namespace app
