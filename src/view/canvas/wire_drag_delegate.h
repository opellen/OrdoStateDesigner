#pragma once

#include <QPointF>

#include "view/geometry/edge_router.h"

class QGraphicsEllipseItem;
class QGraphicsPathItem;
class QGraphicsScene;

namespace app {

class ItemRegistryDelegate;

// Wire-drag mechanics shared by new-wire and reconnect drags: the preview
// overlay and the magnetic snap scan. The presenter owns the session payload,
// the drop highlight, and the commit.
class WireDragDelegate {
public:
    // targetId 0 = no candidate. freeEnd is the candidate's nearest side port,
    // or the raw cursor position.
    struct SnapResult {
        quint64 targetId = 0;
        QPointF freeEnd;
    };

    WireDragDelegate(QGraphicsScene* scene, const ItemRegistryDelegate& items);

    // Nearest state within the snap radius of its rect, the source included
    // (releasing on the source makes a self-transition).
    SnapResult scan(QPointF scenePos) const;

    // The overlay is created lazily and only hidden between drags.
    void ensureOverlay();
    void updateVisual(QPointF startAnchor, PortSide side, QPointF freeEnd);
    void hideOverlay();

    // After the presenter's scene_->clear(): forget the dangling pointers.
    void forgetSceneItems();

private:
    QGraphicsScene* scene_;
    const ItemRegistryDelegate& items_;

    QGraphicsPathItem* path_ = nullptr;
    QGraphicsEllipseItem* dot_ = nullptr;
};

}  // namespace app
