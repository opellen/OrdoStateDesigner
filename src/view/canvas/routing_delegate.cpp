#include "view/canvas/routing_delegate.h"

#include <algorithm>
#include <cmath>

#include <QBrush>
#include <QColor>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsScene>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QRectF>
#include <QVector>

#include "constants/design_tokens.h"
#include "view/canvas/item_registry_delegate.h"
#include "view/geometry/auto_layout.h"
#include "view/items/machine_frame_item.h"
#include "view/items/state_item.h"
#include "view/items/transition_item.h"

namespace app {

namespace {

QColor initialMarkerColor() { return design::color(design::kInitialMarker); }

constexpr qreal kInitialDotRadius = 6.0;
constexpr qreal kInitialStubLength = 28.0;
constexpr qreal kInitialArrowLength = 10.0;
constexpr qreal kInitialArrowHalfWidth = 5.0;

constexpr qreal kCompoundInitialDotRadius = 4.0;
constexpr qreal kCompoundInitialArrowLength = 8.0;
constexpr qreal kCompoundInitialArrowHalfWidth = 4.0;
constexpr qreal kCompoundInitialGap = 10.0;  // dot's fixed inset off the child's own top-left corner

// Shared with auto-layout, which reserves exactly this hull.
constexpr qreal kContainerPadding = kContainerHullPadding;

const Transition* findTransition(const Machine& machine, quint64 id) {
    for (const Transition& transition : machine.transitions) {
        if (transition.id == id) {
            return &transition;
        }
    }
    return nullptr;
}

const State* findState(const Machine& machine, quint64 id) {
    for (const State& state : machine.states) {
        if (state.id == id) {
            return &state;
        }
    }
    return nullptr;
}

}  // namespace

RoutingDelegate::RoutingDelegate(QGraphicsScene* scene, const ItemRegistryDelegate& items,
                                 std::function<const Machine*()> machine,
                                 std::function<MachineFrameItem*()> frameItem,
                                 std::function<MachineFrameItem*()> ensureFrameItem,
                                 std::function<void()> afterFrameRefresh)
    : scene_(scene),
      items_(items),
      machine_(std::move(machine)),
      frameItem_(std::move(frameItem)),
      ensureFrameItem_(std::move(ensureFrameItem)),
      afterFrameRefresh_(std::move(afterFrameRefresh)) {}

void RoutingDelegate::rerouteTouching(quint64 stateId) {
    const Machine* machine = machine_();
    if (machine == nullptr) {
        return;
    }
    // Re-fit the frame (and reroute root wires, which anchor to it) first.
    refreshFrame();
    for (const Transition& transition : machine->transitions) {
        if (transition.from == stateId || transition.to == stateId ||
            transition.effectiveTargets().contains(stateId)) {
            rerouteTransition(transition.id);
        }
    }
}

void RoutingDelegate::refreshFrame() {
    MachineFrameItem* frame = ensureFrameItem_();
    const Machine* machine = machine_();
    if (items_.stateItems().isEmpty()) {
        frame->setVisible(false);
    } else {
        QRectF content;
        for (StateItem* item : items_.stateItems()) {
            content = content.isNull() ? item->sceneRect() : content.united(item->sceneRect());
        }
        // Non-root pills and bendpoints grow the frame too. Root pills are
        // anchored to the frame itself, so including them would feed back.
        if (machine != nullptr) {
            for (const Transition& transition : machine->transitions) {
                if (transition.from == 0) {
                    continue;
                }
                if (TransitionLabelItem* label = items_.transitionLabels().value(transition.id, nullptr)) {
                    content = content.united(label->sceneRect());
                }
                // A tiny rect, not a zero-size one: united() ignores null rects.
                for (const QPointF& bp : transition.manualBendpoints) {
                    content = content.united(QRectF(bp - QPointF(1.0, 1.0), QSizeF(2.0, 2.0)));
                }
            }
        }
        // A live segment drag's uncommitted skeleton.
        if (!transientContentRect_.isNull()) {
            content = content.united(transientContentRect_);
        }
        // 40px breathing room, plus the 28px header band on top.
        frame->setGeometryAndName(content.adjusted(-40.0, -68.0, 40.0, 40.0),
                                  machine != nullptr ? machine->name : QString());
        frame->setVisible(true);
    }
    if (machine != nullptr) {
        for (const Transition& transition : machine->transitions) {
            if (transition.from == 0) {
                rerouteTransition(transition.id);
            }
        }
    }
    afterFrameRefresh_();
}

bool RoutingDelegate::rootFrameEdgePoint(const Transition& transition, QPointF* edgePointOut) const {
    const Machine* machine = machine_();
    MachineFrameItem* frame = frameItem_();
    if (machine == nullptr || frame == nullptr || !frame->isVisible()) {
        return false;
    }
    int slot = 0;
    for (const Transition& other : machine->transitions) {
        if (other.id < transition.id && other.from == 0) {
            ++slot;
        }
    }
    const QRectF frameRect = frame->sceneFrameRect();
    *edgePointOut = QPointF(frameRect.right(), frameRect.top() + 64.0 + 40.0 * static_cast<qreal>(slot));
    return true;
}

// The pill's position formula (right-slot base + offset) never changes; only
// the stub follows the frame side nearest the pill. The edge point is the
// pill center projected onto that side, kept 24px clear of the corners.
PortSide RoutingDelegate::rootStubSide(const QRectF& frameRect, QPointF pillCenter, QPointF* edgePointOut) const {
    const PortSide side = sideFor(frameRect.center(), pillCenter);
    const qreal inset = 24.0;
    QPointF edgePoint;
    switch (side) {
        case PortSide::Left:
            edgePoint = QPointF(frameRect.left(),
                                 std::clamp(pillCenter.y(), frameRect.top() + inset, frameRect.bottom() - inset));
            break;
        case PortSide::Right:
            edgePoint = QPointF(frameRect.right(),
                                 std::clamp(pillCenter.y(), frameRect.top() + inset, frameRect.bottom() - inset));
            break;
        case PortSide::Top:
            edgePoint = QPointF(std::clamp(pillCenter.x(), frameRect.left() + inset, frameRect.right() - inset),
                                 frameRect.top());
            break;
        case PortSide::Bottom:
            edgePoint = QPointF(std::clamp(pillCenter.x(), frameRect.left() + inset, frameRect.right() - inset),
                                 frameRect.bottom());
            break;
    }
    *edgePointOut = edgePoint;
    return side;
}

bool RoutingDelegate::routePillPorts(const Transition& transition, QPointF pillCenter,
                                     const std::optional<PortAssignment>& incumbent, RoutedEdge* routedOut,
                                     PortAssignment* portsOut, qreal fraction) const {
    TransitionLabelItem* labelItem = items_.transitionLabels().value(transition.id, nullptr);
    if (labelItem == nullptr) {
        return false;
    }

    // Self/targetless/root wires attach to the pill's rect at `pillCenter`.
    const QRectF pillRect = labelItem->sceneRectAt(pillCenter);

    PortAssignment ports;
    if (transition.from == 0) {
        QPointF edgePoint;
        if (!rootFrameEdgePoint(transition, &edgePoint)) {
            return false;
        }
        // Re-anchor the stub to the frame side nearest the pill.
        if (MachineFrameItem* frame = frameItem_()) {
            rootStubSide(frame->sceneFrameRect(), pillRect.center(), &edgePoint);
        }
        // The frame edge point is passed as a degenerate counterpart rect.
        ports = resolvePillPorts(EdgeClass::Root, QRectF(), QRectF(edgePoint, QSizeF(0.0, 0.0)), pillRect, incumbent);
        // Arrow into the frame only when there is a target or the machine targets itself.
        ports.arrowAtTarget = transition.to != 0 || transition.machineSelf;
    } else {
        StateItem* sourceItem = items_.stateItems().value(transition.from, nullptr);
        if (sourceItem == nullptr) {
            return false;
        }
        const QRectF sourceRect = sourceItem->sceneRect();
        if (transition.from == transition.to) {
            ports = resolvePillPorts(EdgeClass::Self, sourceRect, sourceRect, pillRect, incumbent);
        } else if (transition.to == 0) {
            ports = resolvePillPorts(EdgeClass::Targetless, sourceRect, QRectF(), pillRect, incumbent);
        } else {
            // A normal edge is one orthogonal polyline from node anchor to
            // node anchor; the pill does not shape it.
            const QHash<quint64, EndpointAnchor> fromAnchors = nodeAnchorsFor(transition.from);
            const QHash<quint64, EndpointAnchor> toAnchors = nodeAnchorsFor(transition.to);
            if (!fromAnchors.contains(transition.id) || !toAnchors.contains(transition.id)) {
                return false;  // an endpoint's item isn't there yet
            }
            const EndpointAnchor sourceEnd = fromAnchors.value(transition.id);
            const EndpointAnchor targetEnd = toAnchors.value(transition.id);

            RoutedEdge unified = routeUnifiedEdge(sourceEnd.anchor, sourceEnd.side, targetEnd.anchor,
                                                  targetEnd.side, 10.0, QPointF(), fraction, std::nullopt,
                                                  std::nullopt, transition.labelRatio);
            // labelBase stays the route's own point, the origin the persisted
            // offset diffs against; labelAnchor is just where the caller wants the pill.
            unified.labelAnchor = pillCenter;
            *routedOut = unified;

            // No pill ports: just the node ends and the pill's spot.
            PortAssignment normalPorts;
            normalPorts.cls = EdgeClass::Normal;
            normalPorts.pillCenter = pillCenter;
            normalPorts.sourceAnchor = sourceEnd.anchor;
            normalPorts.sourceAnchorSide = sourceEnd.side;
            normalPorts.targetAnchor = targetEnd.anchor;
            normalPorts.targetAnchorSide = targetEnd.side;
            normalPorts.hasEntry = false;
            normalPorts.hasExit = false;
            *portsOut = normalPorts;
            return true;
        }
    }
    *routedOut = routeThroughPorts(ports);
    *portsOut = ports;
    return true;
}

bool RoutingDelegate::routeManual(const Transition& transition, QPointF labelOffset, RoutedEdge* routedOut) const {
    return routeManualCourse(transition, transition.manualBendpoints, labelOffset, routedOut);
}

bool RoutingDelegate::routeManualCourse(const Transition& transition, const QVector<QPointF>& bendpoints,
                                        QPointF labelOffset, RoutedEdge* routedOut) const {
    if (bendpoints.isEmpty() || transition.from == 0 || transition.to == 0) {
        return false;
    }
    StateItem* sourceItem = items_.stateItems().value(transition.from, nullptr);
    StateItem* targetItem = items_.stateItems().value(transition.to, nullptr);
    TransitionLabelItem* labelItem = items_.transitionLabels().value(transition.id, nullptr);
    if (sourceItem == nullptr || targetItem == nullptr || labelItem == nullptr) {
        return false;
    }
    RoutedEdge routed = routeManualEdge(sourceItem->sceneRect(), targetItem->sceneRect(), bendpoints, 10.0,
                                        labelOffset, labelItem->sceneRect().size());
    // A course that node moves made degenerate (through a node, reversed
    // arrow) falls back to the auto route until the next segment drag.
    if (!manualRouteWellFormed(routed, sourceItem->sceneRect(), targetItem->sceneRect())) {
        return false;
    }
    *routedOut = routed;
    return true;
}

void RoutingDelegate::rerouteTransition(quint64 transitionId) {
    const Machine* machine = machine_();
    if (machine == nullptr) {
        return;
    }
    TransitionItem* edgeItem = items_.transitionItems().value(transitionId, nullptr);
    TransitionLabelItem* labelItem = items_.transitionLabels().value(transitionId, nullptr);
    if (edgeItem == nullptr || labelItem == nullptr) {
        return;
    }
    const Transition* transition = findTransition(*machine, transitionId);
    if (transition == nullptr) {
        return;
    }

    // `base` is the origin the persisted pill offset is relative to. Root,
    // self, and targetless forms compute it here; a normal edge gets it from
    // its routed wire below.
    QPointF base;
    qreal fraction = 0.5;
    if (transition->from == 0) {
        // Root pills stack by id outside the frame's right edge; hidden while
        // the machine is empty.
        QPointF edgePoint;
        if (!rootFrameEdgePoint(*transition, &edgePoint)) {
            edgeItem->setVisible(false);
            labelItem->setVisible(false);
            return;
        }
        edgeItem->setVisible(true);
        labelItem->setVisible(true);
        // Always the right-slot anchor, so a pill never jumps; only the stub
        // changes sides.
        base = routeRootStub(edgePoint, /*arrowIntoFrame=*/transition->to != 0 || transition->machineSelf)
                   .labelAnchor;
    } else {
        StateItem* sourceItem = items_.stateItems().value(transition->from, nullptr);
        if (sourceItem == nullptr) {
            return;  // an endpoint's StateItem doesn't exist yet
        }
        const bool selfLoop = transition->from == transition->to && transition->to != 0;
        if (selfLoop || transition->to == 0) {
            // Slot = 0-based rank by id among this node's wires of the same
            // shape. These never occupy side ports.
            int slot = 0;
            for (const Transition& other : machine->transitions) {
                if (other.id >= transition->id || other.from != transition->from) {
                    continue;
                }
                const bool otherSelfLoop = other.from == other.to && other.to != 0;
                if (selfLoop ? otherSelfLoop : other.to == 0) {
                    ++slot;
                }
            }
            const QRectF rect = sourceItem->sceneRect();
            const QSizeF pillSize = labelItem != nullptr ? labelItem->boundingRect().size() : QSizeF();
            base = (selfLoop ? routeSelfLoop(rect, slot, 10.0, pillSize) : routeTargetlessStub(rect, slot)).labelAnchor;
        } else {
            // Only the pill's fraction along the wire is decided here:
            // parallel edges between the same two nodes stagger their pills.
            fraction = 0.5;
            const quint64 u = std::min(transition->from, transition->to);
            const quint64 v = std::max(transition->from, transition->to);
            QVector<quint64> corridorIds;
            for (const Transition& other : machine->transitions) {
                if (other.from == 0 || other.to == 0 || other.from == other.to) {
                    continue;
                }
                const quint64 otherU = std::min(other.from, other.to);
                const quint64 otherV = std::max(other.from, other.to);
                if (otherU == u && otherV == v) {
                    corridorIds.push_back(other.id);
                }
            }
            std::sort(corridorIds.begin(), corridorIds.end());
            const int K = corridorIds.size();
            const int corridorIndex = corridorIds.indexOf(transitionId);

            qreal corridorT = 0.5;
            if (K == 2) {
                corridorT = (corridorIndex == 0) ? 0.35 : 0.65;
            } else if (K > 2) {
                const qreal minT = 0.28;
                const qreal maxT = 0.72;
                corridorT = minT + (maxT - minT) * static_cast<qreal>(corridorIndex) / static_cast<qreal>(K - 1);
            }
            // corridorT runs from the lower id; flip it for wires going the other way.
            fraction = (transition->from == u) ? corridorT : (1.0 - corridorT);
        }
    }
    // What a plain (non-Alt) pill drag diffs against; equals `base` for a normal edge.
    const bool normalEdge = transition->from != 0 && transition->to != 0 && transition->from != transition->to;
    QPointF autoBase = base;
    RoutedEdge routed;
    if (routeManual(*transition, transition->labelOffset, &routed)) {
        // The offset must be relative to a point on this wire.
        base = routed.labelBase;
    } else {
        // Not a drag, so no hysteresis. A detached (Alt-dragged) pill leaves
        // the wire routed at its base; the offset only moves the label.
        if (transition->labelOffset.isNull()) {
            detachedLabelTransitions_.remove(transitionId);
        }
        const bool detached = isLabelDetached(transitionId);
        const QPointF targetCenter = detached ? base : (base + transition->labelOffset);
        PortAssignment ports;
        if (!routePillPorts(*transition, targetCenter, std::nullopt, &routed, &ports, fraction)) {
            return;
        }
        if (normalEdge) {
            // Pill = wire base + offset, detached or not; `detached` only
            // draws the leader line.
            base = routed.labelBase;
            routed.labelAnchor = base + transition->labelOffset;
        } else {
            if (!routed.labelBase.isNull()) {
                base = routed.labelBase;
            }
            if (detached) {
                routed.labelAnchor = base + transition->labelOffset;
            } else {
                routed.labelAnchor = base;
            }
        }
    }
    if (normalEdge) {
        autoBase = base;
    }
    labelBaseAnchors_.insert(transitionId, base);
    autoLabelBaseAnchors_.insert(transitionId, autoBase);
    if (transition->isMultiTarget()) {
        const QRectF pillRect = labelItem->sceneRectAt(routed.labelAnchor);
        const QList<quint64> allTargets = transition->effectiveTargets();
        for (int i = 1; i < allTargets.size(); ++i) {
            const quint64 secTargetId = allTargets[i];
            StateItem* secTargetItem = items_.stateItems().value(secTargetId, nullptr);
            if (secTargetItem == nullptr) {
                continue;
            }
            routed.secondaryBranches.push_back(
                routeTargetBranch(pillRect, secTargetItem->sceneRect(), secTargetId));
        }
    }
    edgeItem->setRoute(routed);
    labelItem->setAnchor(routed.labelAnchor);
}

QHash<quint64, EndpointAnchor> RoutingDelegate::nodeAnchorsFor(quint64 stateId) const {
    const Machine* machine = machine_();
    StateItem* item = items_.stateItems().value(stateId, nullptr);
    if (item == nullptr || machine == nullptr) {
        return {};
    }
    const QRectF rect = item->sceneRect();

    const QVector<TouchingTransition> touching = touchingTransitionsFor(*machine, stateId, [this](quint64 id) {
        StateItem* other = items_.stateItems().value(id, nullptr);
        return other != nullptr ? other->sceneRect() : QRectF();
    });
    return computeNodeAnchors(rect, touching);
}

void RoutingDelegate::refreshInitialMarker() {
    initialMarkerStateId_ = 0;
    // Plain delete, never removeItem() first (see ItemRegistryDelegate).
    if (initialDot_ != nullptr) {
        delete initialDot_;
        initialDot_ = nullptr;
    }
    if (initialStub_ != nullptr) {
        delete initialStub_;
        initialStub_ = nullptr;
    }
    if (initialArrow_ != nullptr) {
        delete initialArrow_;
        initialArrow_ = nullptr;
    }
    const Machine* machine = machine_();
    if (machine == nullptr) {
        return;
    }

    const State* initial = findState(*machine, machine->initialStateId);
    if (initial == nullptr) {
        return;  // no initial state set (id 0 resolves to no state)
    }
    StateItem* item = items_.stateItems().value(initial->id, nullptr);
    if (item == nullptr) {
        return;
    }

    const QRectF rect = item->sceneRect();
    const QPointF portAnchor(rect.left(), rect.center().y());
    const QPointF dotCenter(portAnchor.x() - kInitialStubLength, portAnchor.y());
    const QColor color = initialMarkerColor();

    initialDot_ = scene_->addEllipse(dotCenter.x() - kInitialDotRadius, dotCenter.y() - kInitialDotRadius,
                                     kInitialDotRadius * 2.0, kInitialDotRadius * 2.0, QPen(Qt::NoPen),
                                     QBrush(color));
    initialDot_->setZValue(11.0);  // just above node boxes (10) so the marker isn't occluded

    // The stub stops short of the box; the arrowhead covers the rest.
    const QPointF travelDir(1.0, 0.0);
    const QPointF lineStop = portAnchor - travelDir * kInitialArrowLength;
    QPainterPath stubPath(QPointF(dotCenter.x() + kInitialDotRadius, dotCenter.y()));
    stubPath.lineTo(lineStop);
    initialStub_ = scene_->addPath(stubPath, QPen(color, 2.0));
    initialStub_->setZValue(11.0);

    const QPolygonF arrow = buildArrowhead(lineStop, travelDir, kInitialArrowLength, kInitialArrowHalfWidth);
    initialArrow_ = scene_->addPolygon(arrow, QPen(Qt::NoPen), QBrush(color));
    initialArrow_->setZValue(11.0);
    initialMarkerStateId_ = initial->id;
}

void RoutingDelegate::refreshContainers() {
    // Re-entrant by design (moving a container re-enters via its move
    // callback) and bounded only while derived positions are stable. The
    // depth cap turns a runaway chase into a warning instead of a stack overflow.
    ++refreshPassCount_;
    static int s_depth = 0;
    struct DepthGuard {
        DepthGuard() { ++s_depth; }
        ~DepthGuard() { --s_depth; }
    } depthGuard;
    if (s_depth > 32) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning() << "sd.routing: refreshContainers cascade exceeded depth 32 -- refresh dropped; a derived "
                          "geometry rule is chasing itself";
        }
        return;
    }

    const Machine* machinePtr = machine_();
    if (machinePtr == nullptr) {
        return;
    }
    // Held for the whole call; no early return below.
    refreshingContainers_ = true;
    const Machine& machine = *machinePtr;

    QHash<quint64, QVector<quint64>> childrenByParent;
    QHash<quint64, quint64> parentById;
    for (const State& state : machine.states) {
        parentById.insert(state.id, state.parentId);
        if (state.parentId != 0) {
            childrenByParent[state.parentId].push_back(state.id);
        }
    }
    // Depth 0 = root level. maxHops guards against a corrupt parent cycle.
    const int maxHops = machine.states.size();
    auto depthOf = [&](quint64 id) {
        int depth = 0;
        quint64 current = parentById.value(id, 0);
        while (current != 0 && depth < maxHops) {
            ++depth;
            current = parentById.value(current, 0);
        }
        return depth;
    };

    // Deepest first, by real parent-chain depth: a compound's hull reads its
    // child compounds' already-updated rects.
    QVector<quint64> compounds;
    compounds.reserve(childrenByParent.size());
    for (auto it = childrenByParent.constBegin(); it != childrenByParent.constEnd(); ++it) {
        compounds.push_back(it.key());
    }
    std::stable_sort(compounds.begin(), compounds.end(),
                     [&](quint64 a, quint64 b) { return depthOf(a) > depthOf(b); });

    for (quint64 stateId : compounds) {
        StateItem* item = items_.stateItems().value(stateId, nullptr);
        if (item == nullptr) {
            continue;
        }
        QRectF content;
        const QVector<quint64> childVec = childrenByParent.value(stateId);
        QSet<quint64> childSet(childVec.begin(), childVec.end());
        for (quint64 childId : childVec) {
            StateItem* child = items_.stateItems().value(childId, nullptr);
            if (child == nullptr) {
                continue;
            }
            content = content.isNull() ? child->sceneRect() : content.united(child->sceneRect());
        }
        if (content.isNull()) {
            continue;  // every listed child is missing its item -- keep the last rect
        }
        // Pills of wires between direct children grow the hull too.
        for (const Transition& transition : machine.transitions) {
            if (childSet.contains(transition.from) && childSet.contains(transition.to)) {
                if (TransitionLabelItem* label = items_.transitionLabels().value(transition.id, nullptr)) {
                    content = content.united(label->sceneRect());
                }
            }
        }
        // The header grows with the state's own name and entry/exit lines.
        QRectF rect = content.adjusted(-kContainerPadding, -(kContainerPadding + item->containerHeaderHeight()),
                                       kContainerPadding, kContainerPadding);
        // Widen rightward rather than clip a long name.
        const qreal minWidth = item->containerMinHeaderWidth();
        if (rect.width() < minWidth) {
            rect.setWidth(minWidth);
        }
        item->setContainerGeometry(rect);
        item->setZValue(std::min(-5.0 + 0.5 * depthOf(stateId), -1.0));
    }

    // A container that lost its last child reverts to a leaf at State.pos.
    // Leaves are left alone so an in-flight drag is never fought.
    for (const State& state : machine.states) {
        if (childrenByParent.contains(state.id)) {
            continue;  // handled above
        }
        StateItem* item = items_.stateItems().value(state.id, nullptr);
        if (item != nullptr && item->isContainerMode()) {
            item->setPos(state.pos);
            item->setLeafMode();
        }
    }

    refreshCompoundInitialMarkers();
    refreshingContainers_ = false;
}

void RoutingDelegate::refreshCompoundInitialMarkers() {
    for (auto* dot : compoundInitialDots_) {
        delete dot;  // not removeItem() first
    }
    compoundInitialDots_.clear();
    for (auto* arrow : compoundInitialArrows_) {
        delete arrow;
    }
    compoundInitialArrows_.clear();
    const Machine* machine = machine_();
    if (machine == nullptr) {
        return;
    }

    const QColor color = initialMarkerColor();
    for (const State& state : machine->states) {
        if (state.initialChildId == 0) {
            continue;
        }
        StateItem* container = items_.stateItems().value(state.id, nullptr);
        StateItem* child = items_.stateItems().value(state.initialChildId, nullptr);
        // initialChildId can be stale for one refresh (mid cascade delete).
        if (container == nullptr || !container->isContainerMode() || child == nullptr) {
            continue;
        }

        // Anchored to the initial child's top-left, not the container's.
        const QPointF childTopLeft = child->sceneRect().topLeft();
        const QPointF dotCenter = childTopLeft - QPointF(kCompoundInitialGap, kCompoundInitialGap);
        const QPointF delta = childTopLeft - dotCenter;
        const qreal len = std::hypot(delta.x(), delta.y());
        const QPointF travelDir = len > 0.0 ? delta / len : QPointF(1.0, 0.0);

        auto* dot = scene_->addEllipse(dotCenter.x() - kCompoundInitialDotRadius, dotCenter.y() - kCompoundInitialDotRadius,
                                      kCompoundInitialDotRadius * 2.0, kCompoundInitialDotRadius * 2.0,
                                      QPen(Qt::NoPen), QBrush(color));
        dot->setZValue(11.0);  // initial-marker band
        compoundInitialDots_.insert(state.id, dot);

        const QPointF lineStop = childTopLeft - travelDir * kCompoundInitialArrowLength;
        const QPolygonF arrow = buildArrowhead(lineStop, travelDir, kCompoundInitialArrowLength, kCompoundInitialArrowHalfWidth);
        auto* arrowItem = scene_->addPolygon(arrow, QPen(Qt::NoPen), QBrush(color));
        arrowItem->setZValue(11.0);
        compoundInitialArrows_.insert(state.id, arrowItem);
    }
}

void RoutingDelegate::forgetSceneItems() {
    initialDot_ = nullptr;
    initialStub_ = nullptr;
    initialArrow_ = nullptr;
    initialMarkerStateId_ = 0;
    compoundInitialDots_.clear();
    compoundInitialArrows_.clear();
    labelBaseAnchors_.clear();
    autoLabelBaseAnchors_.clear();
    detachedLabelTransitions_.clear();
}

}  // namespace app
