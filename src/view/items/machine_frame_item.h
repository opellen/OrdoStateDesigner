#pragma once

#include <array>
#include <functional>

#include <QGraphicsItem>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>

class QGraphicsSceneMouseEvent;

namespace app {

class HandleItem;

// PortSide is defined in edge_router.h; the wire setters only pass it through.
enum class PortSide;

// The machine frame: auto-fits around every state and non-root pill (see
// CanvasPresenter::refreshFrame), selectable and draggable as a whole-machine
// move, but only from its border/header band. shape() returns just that band,
// so the interior stays click-through for hit tests and for the items inside.
class MachineFrameItem : public QGraphicsItem {
public:
    enum { Type = QGraphicsItem::UserType + 5 };
    int type() const override { return Type; }

    MachineFrameItem();

    // `sceneRect` is the full frame INCLUDING the header band; `name` fills
    // the header (blank draws as "(unnamed machine)").
    void setGeometryAndName(const QRectF& sceneRect, const QString& name);
    QRectF sceneFrameRect() const { return QRectF(pos(), rect_.size()); }

    // Whole-machine drag from the border/header, in scene positions (the item
    // itself never moves). Started fires once on the first real move with the
    // press position (the delta origin); a stationary click never fires it. Moved
    // follows with the live position. Exactly one of Finished (release away from
    // the press position) or Reverted (release exactly on it) follows every Started.
    void setOnDragStarted(std::function<void(QPointF grabScenePos)> cb);
    void setOnDragMoved(std::function<void(QPointF scenePos)> cb);
    void setOnDragFinished(std::function<void(QPointF scenePos)> cb);
    void setOnDragReverted(std::function<void()> cb);

    // Wire drags forwarded from the pressed HandleItem child. The presenter opens
    // a WireDrag session with sourceId 0: a stationary click commits a targetless
    // machine event, a drag onto a state a targeted root transition.
    void setOnWireDragStarted(std::function<void(PortSide side)> cb);
    void setOnWireDragMoved(std::function<void(QPointF scenePos)> cb);
    void setOnWireDragFinished(std::function<void(QPointF scenePos)> cb);

    bool debugSideHandlesVisible() const;
    QPointF debugHandleScenePos(PortSide side) const;

    QRectF boundingRect() const override;
    // Border ring + header band only; hit-testing is shape()-based.
    QPainterPath shape() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
    void repositionHandles();
    void updateHandleVisibility();

    QRectF rect_;  // local, origin at (0,0); pos() carries the scene topLeft
    QString name_;

    bool leftPressed_ = false;
    bool dragReported_ = false;  // onDragStarted_ already fired for this gesture
    QPointF dragStartScenePos_;
    std::array<HandleItem*, 4> handles_{};  // Top, Right, Bottom, Left

    std::function<void(QPointF)> onDragStarted_;
    std::function<void(QPointF)> onDragMoved_;
    std::function<void(QPointF)> onDragFinished_;
    std::function<void()> onDragReverted_;
    std::function<void(PortSide)> onWireDragStarted_;
    std::function<void(QPointF)> onWireDragMoved_;
    std::function<void(QPointF)> onWireDragFinished_;
};

}  // namespace app
