#pragma once

#include <functional>

#include <QGraphicsItem>
#include <QPointF>
#include <QRectF>

#include "view/geometry/edge_router.h"

class QGraphicsSceneMouseEvent;

namespace app {

// One of a selected StateItem's four wire-drag-source handles. Not a resize
// grip: node boxes are content-sized.
// A pure input sensor: forwards press/move/release scene positions through the
// callbacks below (installed by the owning StateItem); it holds no drag-preview,
// snap or routing logic. Child of its StateItem, which positions it via
// repositionHandles().
class HandleItem : public QGraphicsItem {
public:
    enum { Type = QGraphicsItem::UserType + 4 };
    int type() const override { return Type; }

    HandleItem(PortSide side, QGraphicsItem* parent);

    PortSide side() const { return side_; }

    void setOnDragStarted(std::function<void()> cb);
    void setOnDragMoved(std::function<void(QPointF scenePos)> cb);
    void setOnDragFinished(std::function<void(QPointF scenePos)> cb);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
    PortSide side_;
    bool dragging_ = false;

    std::function<void()> onDragStarted_;
    std::function<void(QPointF)> onDragMoved_;
    std::function<void(QPointF)> onDragFinished_;
};

}  // namespace app
