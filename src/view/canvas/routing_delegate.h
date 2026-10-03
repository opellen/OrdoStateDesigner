#pragma once

#include <functional>
#include <optional>

#include <QHash>
#include <QPointF>
#include <QSet>
#include <QtGlobal>

#include "model/machine.h"
#include "view/geometry/edge_router.h"
#include "view/geometry/node_anchors.h"
#include "view/geometry/pill_port_resolver.h"

class QGraphicsEllipseItem;
class QGraphicsPathItem;
class QGraphicsPolygonItem;
class QGraphicsScene;

namespace app {

class ItemRegistryDelegate;
class MachineFrameItem;

// Route and marker refresh for the canvas: wire routing, the machine frame,
// initial markers, and container hulls, plus the caches those own. Never
// sends intents.
class RoutingDelegate {
public:
    // machine() is the current document or nullptr. frameItem() peeks without
    // creating (a root wire before the first refreshFrame() stays hidden);
    // ensureFrameItem() creates. afterFrameRefresh() runs at the end of every
    // refreshFrame(), so geometry-derived overlays can re-anchor there.
    RoutingDelegate(QGraphicsScene* scene, const ItemRegistryDelegate& items,
                    std::function<const Machine*()> machine,
                    std::function<MachineFrameItem*()> frameItem,
                    std::function<MachineFrameItem*()> ensureFrameItem,
                    std::function<void()> afterFrameRefresh);

    // A container's box is always the live union of its children, mid-drag
    // included; it cannot also answer "is the child leaving?". Leaving a
    // parent takes the explicit Move Out One Level verb.
    void rerouteTransition(quint64 transitionId);
    void rerouteTouching(quint64 stateId);
    void refreshFrame();
    void refreshInitialMarker();
    void refreshContainers();
    void refreshCompoundInitialMarkers();

    // The route builder for every wire form, also used by the live pill-drag
    // preview. Self/targetless/root wires attach through the pill's ports.
    // A normal edge routes node to node and its pill is only placed at
    // `pillCenter`: its PortAssignment has hasEntry/hasExit false and
    // `incumbent` is ignored.
    bool routePillPorts(const Transition& transition, QPointF pillCenter,
                        const std::optional<PortAssignment>& incumbent, RoutedEdge* routedOut,
                        PortAssignment* portsOut, qreal fraction = 0.5) const;

    // Routes a bendpointed non-root, non-targetless wire along its authored
    // course; the pill rides the course. False when there is no course or an
    // endpoint item is missing, so the caller falls back to routePillPorts.
    // Preview and commit both use it, so they never disagree.
    bool routeManual(const Transition& transition, QPointF labelOffset, RoutedEdge* routedOut) const;
    // routeManual over an uncommitted course, so a live segment drag looks
    // exactly like what release will produce.
    bool routeManualCourse(const Transition& transition, const QVector<QPointF>& bendpoints,
                           QPointF labelOffset, RoutedEdge* routedOut) const;

    // Extra frame content during a segment drag; set on each move, clear on release.
    void setTransientContentRect(const QRectF& rect) { transientContentRect_ = rect; }
    void clearTransientContentRect() { transientContentRect_ = QRectF(); }

    // Gathers inputs for computeNodeAnchors(). Named differently so it does
    // not hide that free function inside this class.
    QHash<quint64, EndpointAnchor> nodeAnchorsFor(quint64 stateId) const;

    bool refreshingContainers() const { return refreshingContainers_; }

    // Container passes run so far; a feedback loop yields correct geometry
    // exponentially many times, so probes assert the count.
    int debugRefreshPassCount() const { return refreshPassCount_; }
    void debugResetRefreshPassCount() { refreshPassCount_ = 0; }
    quint64 initialMarkerStateId() const { return initialMarkerStateId_; }
    bool hasCompoundInitialMarker(quint64 stateId) const { return compoundInitialDots_.contains(stateId); }
    bool hasLabelBaseAnchor(quint64 transitionId) const { return labelBaseAnchors_.contains(transitionId); }
    QPointF labelBaseAnchor(quint64 transitionId) const { return labelBaseAnchors_.value(transitionId); }
    bool hasAutoLabelBaseAnchor(quint64 transitionId) const {
        return autoLabelBaseAnchors_.contains(transitionId) || labelBaseAnchors_.contains(transitionId);
    }
    QPointF autoLabelBaseAnchor(quint64 transitionId) const {
        return autoLabelBaseAnchors_.value(transitionId, labelBaseAnchors_.value(transitionId));
    }
    bool isLabelDetached(quint64 transitionId) const {
        return detachedLabelTransitions_.contains(transitionId);
    }
    void setLabelDetached(quint64 transitionId, bool detached) {
        if (detached) {
            detachedLabelTransitions_.insert(transitionId);
        } else {
            detachedLabelTransitions_.remove(transitionId);
        }
    }
    // SimPresentation's Scene struct carries the marker trio for dimming.
    QGraphicsEllipseItem* initialDot() const { return initialDot_; }
    QGraphicsPathItem* initialStub() const { return initialStub_; }
    QGraphicsPolygonItem* initialArrow() const { return initialArrow_; }

    // Call when a transition's visual is removed.
    void forgetLabelBase(quint64 transitionId) {
        labelBaseAnchors_.remove(transitionId);
        autoLabelBaseAnchors_.remove(transitionId);
        detachedLabelTransitions_.remove(transitionId);
    }
    // After the presenter's scene_->clear(): forget dangling pointers and caches.
    void forgetSceneItems();

private:
    bool rootFrameEdgePoint(const Transition& transition, QPointF* edgePointOut) const;
    // The frame side nearest the pill; the root stub leaves from there.
    PortSide rootStubSide(const QRectF& frameRect, QPointF pillCenter, QPointF* edgePointOut) const;

    QGraphicsScene* scene_;
    const ItemRegistryDelegate& items_;
    std::function<const Machine*()> machine_;
    std::function<MachineFrameItem*()> frameItem_;
    std::function<MachineFrameItem*()> ensureFrameItem_;
    std::function<void()> afterFrameRefresh_;

    QHash<quint64, QPointF> labelBaseAnchors_;
    QHash<quint64, QPointF> autoLabelBaseAnchors_;
    QSet<quint64> detachedLabelTransitions_;
    QRectF transientContentRect_;

    QGraphicsEllipseItem* initialDot_ = nullptr;
    QGraphicsPathItem* initialStub_ = nullptr;
    QGraphicsPolygonItem* initialArrow_ = nullptr;
    quint64 initialMarkerStateId_ = 0;

    QHash<quint64, QGraphicsEllipseItem*> compoundInitialDots_;
    QHash<quint64, QGraphicsPolygonItem*> compoundInitialArrows_;

    // True during refreshContainers(), whose hull setPos re-enters a dragged
    // container's itemChange. Drag corrections must skip that reentrant move,
    // which has to stay an exact no-op or the drag feeds back on itself.
    bool refreshingContainers_ = false;
    int refreshPassCount_ = 0;
};

}  // namespace app
