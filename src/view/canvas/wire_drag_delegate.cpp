#include "view/canvas/wire_drag_delegate.h"

#include <algorithm>
#include <cmath>

#include <QBrush>
#include <QColor>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QPen>

#include "constants/design_tokens.h"
#include "view/canvas/item_registry_delegate.h"
#include "view/geometry/node_anchors.h"  // sideFor
#include "view/items/state_item.h"

namespace app {

namespace {

constexpr qreal kWireSnapRadius = 20.0;  // scene px

// 0 when `point` is inside `rect`.
qreal distanceToRect(const QRectF& rect, QPointF point) {
    const qreal dx = std::max({rect.left() - point.x(), 0.0, point.x() - rect.right()});
    const qreal dy = std::max({rect.top() - point.y(), 0.0, point.y() - rect.bottom()});
    return std::hypot(dx, dy);
}

}  // namespace

WireDragDelegate::WireDragDelegate(QGraphicsScene* scene, const ItemRegistryDelegate& items)
    : scene_(scene), items_(items) {}

WireDragDelegate::SnapResult WireDragDelegate::scan(QPointF scenePos) const {
    StateItem* best = nullptr;
    qreal bestDist = kWireSnapRadius;
    for (auto it = items_.stateItems().constBegin(); it != items_.stateItems().constEnd(); ++it) {
        const qreal dist = distanceToRect(it.value()->sceneRect(), scenePos);
        if (dist <= bestDist) {
            bestDist = dist;
            best = it.value();
        }
    }

    SnapResult result;
    result.freeEnd = scenePos;
    if (best != nullptr) {
        result.targetId = best->id();
        // Same side heuristic as committed routing.
        result.freeEnd = sidePortAnchor(best->sceneRect(), sideFor(best->sceneRect().center(), scenePos), 0.0);
    }
    return result;
}

void WireDragDelegate::ensureOverlay() {
    if (path_ == nullptr) {
        path_ = scene_->addPath(QPainterPath(), QPen(design::color(design::kEdgeSelected), 2.0));
        path_->setZValue(100.0);  // interaction-overlay band
    }
    if (dot_ == nullptr) {
        dot_ = scene_->addEllipse(QRectF(-4.0, -4.0, 8.0, 8.0), QPen(Qt::NoPen),
                                  QBrush(design::color(design::kCanvasHighlight)));
        dot_->setZValue(100.0);
    }
    path_->setVisible(true);
    dot_->setVisible(true);
}

void WireDragDelegate::updateVisual(QPointF startAnchor, PortSide side, QPointF freeEnd) {
    if (path_ == nullptr || dot_ == nullptr) {
        return;
    }
    path_->setPath(buildWireDragPreviewPath(startAnchor, side, freeEnd));
    dot_->setPos(freeEnd);
}

void WireDragDelegate::hideOverlay() {
    if (path_ != nullptr) {
        path_->setVisible(false);
    }
    if (dot_ != nullptr) {
        dot_->setVisible(false);
    }
}

void WireDragDelegate::forgetSceneItems() {
    path_ = nullptr;
    dot_ = nullptr;
}

}  // namespace app
