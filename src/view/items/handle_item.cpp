#include "view/items/handle_item.h"

#include <QBrush>
#include <QColor>
#include <QCursor>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPen>
#include <QStyleOptionGraphicsItem>

#include "constants/design_tokens.h"

namespace app {

namespace {

constexpr qreal kHalfSize = 4.0;  // 8x8 px handles

}  // namespace

HandleItem::HandleItem(PortSide side, QGraphicsItem* parent) : QGraphicsItem(parent), side_(side) {
    // Cross cursor tells "starts a wire" apart from "drags the node".
    setCursor(Qt::CrossCursor);
}

void HandleItem::setOnDragStarted(std::function<void()> cb) { onDragStarted_ = std::move(cb); }
void HandleItem::setOnDragMoved(std::function<void(QPointF)> cb) { onDragMoved_ = std::move(cb); }
void HandleItem::setOnDragFinished(std::function<void(QPointF)> cb) { onDragFinished_ = std::move(cb); }

QRectF HandleItem::boundingRect() const { return QRectF(-kHalfSize, -kHalfSize, kHalfSize * 2.0, kHalfSize * 2.0); }

void HandleItem::paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) {
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(QBrush(design::color(design::kEdgeSelected)));
    // Thin accent outline for contrast against the node header and other handles.
    painter->setPen(QPen(design::color(design::kAccentInteractive), 1.0));
    painter->drawRect(boundingRect());
}

void HandleItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QGraphicsItem::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    event->accept();
    if (onDragStarted_) {
        onDragStarted_();
    }
}

void HandleItem::mouseMoveEvent(QGraphicsSceneMouseEvent* event) {
    if (!dragging_) {
        QGraphicsItem::mouseMoveEvent(event);
        return;
    }
    event->accept();
    if (onDragMoved_) {
        onDragMoved_(event->scenePos());
    }
}

void HandleItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* event) {
    if (!dragging_) {
        QGraphicsItem::mouseReleaseEvent(event);
        return;
    }
    // Cleared even if the presenter already canceled the drag (Esc): this item
    // still holds the mouse grab and must stop treating moves as a drag.
    dragging_ = false;
    event->accept();
    if (onDragFinished_) {
        onDragFinished_(event->scenePos());
    }
}

}  // namespace app
