#include "view/geometry/node_anchors.h"

#include <algorithm>
#include <cmath>

#include "model/machine.h"
#include "view/geometry/pill_port_resolver.h"  // oppositePortSide

namespace app {

PortSide sideFor(QPointF fromCenter, QPointF towardCenter) {
    const QPointF delta = towardCenter - fromCenter;
    if (std::abs(delta.x()) >= std::abs(delta.y())) {
        return delta.x() >= 0.0 ? PortSide::Right : PortSide::Left;
    }
    return delta.y() >= 0.0 ? PortSide::Bottom : PortSide::Top;
}

QHash<quint64, EndpointAnchor> computeNodeAnchors(const QRectF& nodeRect, const QVector<TouchingTransition>& touching) {
    QHash<quint64, EndpointAnchor> result;

    // Incoming and outgoing edges on the same side share one occupancy list.
    // `counterpartRectById` carries the caller-resolved counterpart rects to the
    // corridor-spread pass below.
    QHash<int, QVector<quint64>> bySide;
    QHash<quint64, QRectF> counterpartRectById;
    QHash<quint64, quint64> counterpartIdById;
    for (const TouchingTransition& touch : touching) {
        PortSide side = sideFor(nodeRect.center(), touch.counterpartRect.center());
        // Containment pair (one rect encloses the other's center): the container
        // anchors on its border nearest the child, at the child's cross-axis
        // coordinate, and reports the OPPOSITE side (`side` is the departure
        // direction, here inward). The contained end anchors at its facing side's
        // middle, so both ends share one corridor. A mutual overlap keeps the
        // dominance path below. Containment edges skip the bySide spread.
        const bool containsCounterpart = nodeRect.contains(touch.counterpartRect.center());
        const bool counterpartContains = touch.counterpartRect.contains(nodeRect.center());
        if (containsCounterpart != counterpartContains) {
            if (containsCounterpart) {
                const PortSide borderSide = side;  // sideFor already faces the enclosed child
                const bool alongX = borderSide == PortSide::Top || borderSide == PortSide::Bottom;
                const qreal lo = (alongX ? nodeRect.left() : nodeRect.top()) + 12.0;
                const qreal hi = std::max(lo, (alongX ? nodeRect.right() : nodeRect.bottom()) - 12.0);
                const qreal centerCoord = alongX ? nodeRect.center().x() : nodeRect.center().y();
                const qreal along = std::clamp(alongX ? touch.counterpartRect.center().x()
                                                      : touch.counterpartRect.center().y(),
                                               lo, hi);
                result.insert(touch.transitionId,
                              EndpointAnchor{sidePortAnchor(nodeRect, borderSide, along - centerCoord),
                                             oppositePortSide(borderSide)});
            } else {
                const PortSide mySide = sideFor(touch.counterpartRect.center(), nodeRect.center());
                result.insert(touch.transitionId, EndpointAnchor{sidePortAnchor(nodeRect, mySide, 0.0), mySide});
            }
            continue;
        }
        // Diagonal pairs (disjoint in both X and Y) use perpendicular ports so the
        // wire is a single L (e.g. West->South, East->North).
        const QRectF S = touch.isIncoming ? touch.counterpartRect : nodeRect;
        const QRectF T = touch.isIncoming ? nodeRect : touch.counterpartRect;
        const bool disjointX = (T.left() >= S.right()) || (S.left() >= T.right());
        const bool disjointY = (T.top() >= S.bottom()) || (S.top() >= T.bottom());
        if (disjointX && disjointY) {
            const qreal dx = T.center().x() - S.center().x();
            const qreal dy = T.center().y() - S.center().y();
            if (std::abs(dx) >= std::abs(dy)) {
                side = touch.isIncoming ? (dy < 0.0 ? PortSide::Bottom : PortSide::Top)
                                        : (dx > 0.0 ? PortSide::Right : PortSide::Left);
            } else {
                side = touch.isIncoming ? (dx < 0.0 ? PortSide::Right : PortSide::Left)
                                        : (dy > 0.0 ? PortSide::Bottom : PortSide::Top);
            }
        }
        bySide[static_cast<int>(side)].push_back(touch.transitionId);
        counterpartRectById.insert(touch.transitionId, touch.counterpartRect);
        counterpartIdById.insert(touch.transitionId, touch.counterpartId);
    }

    for (auto it = bySide.begin(); it != bySide.end(); ++it) {
        const PortSide side = static_cast<PortSide>(it.key());
        const QVector<quint64>& ids = it.value();
        const bool alongIsX = side == PortSide::Top || side == PortSide::Bottom;
        const qreal lo = (alongIsX ? nodeRect.left() : nodeRect.top()) + 12.0;
        const qreal hi = std::max(lo, (alongIsX ? nodeRect.right() : nodeRect.bottom()) - 12.0);
        const qreal centerCoord = alongIsX ? nodeRect.center().x() : nodeRect.center().y();

        // Transitions sharing a counterpart corridor on this side are spread
        // proportionally across the shared side span instead of crowding at 16px.
        struct AnchorSlot {
            quint64 id = 0;
            qreal along = 0.0;
        };
        QVector<AnchorSlot> slotList;

        // Group by counterpartId.
        QHash<quint64, QVector<quint64>> corridorById;
        for (quint64 id : ids) {
            quint64 otherId = counterpartIdById.value(id, 0);
            if (otherId == 0) {
                const QPointF otherCenter = counterpartRectById.value(id).center();
                otherId = static_cast<quint64>(std::round(otherCenter.x())) * 31337 +
                          static_cast<quint64>(std::round(otherCenter.y()));
            }
            corridorById[otherId].push_back(id);
        }

        for (auto corridorIt = corridorById.begin(); corridorIt != corridorById.end(); ++corridorIt) {
            QVector<quint64> groupIds = corridorIt.value();
            std::sort(groupIds.begin(), groupIds.end());
            const int count = groupIds.size();
            const QRectF counterpartRect = counterpartRectById.value(groupIds.front());
            const QPointF otherCenter = counterpartRect.center();
            const qreal corridorCenter = std::clamp(alongIsX ? otherCenter.x() : otherCenter.y(), lo, hi);

            if (count == 1) {
                slotList.push_back(AnchorSlot{groupIds.front(), corridorCenter});
            } else {
                const qreal selfSpan = std::max(0.0, (alongIsX ? nodeRect.width() : nodeRect.height()) - 24.0);
                const qreal otherSpan = std::max(0.0, (alongIsX ? counterpartRect.width() : counterpartRect.height()) - 24.0);
                const qreal sharedSpan = std::min(selfSpan, otherSpan);
                const qreal targetSpacing = sharedSpan / std::max(static_cast<qreal>(count), 2.0);
                const qreal spacing = std::clamp(targetSpacing, 24.0, 70.0);
                const QVector<qreal> offsets = assignPortOffsets(count, spacing);
                for (int i = 0; i < count; ++i) {
                    const qreal along = std::clamp(corridorCenter + offsets[i], lo, hi);
                    slotList.push_back(AnchorSlot{groupIds[i], along});
                }
            }
        }
        std::sort(slotList.begin(), slotList.end(), [](const AnchorSlot& a, const AnchorSlot& b) {
            return a.along != b.along ? a.along < b.along : a.id < b.id;
        });
        for (int i = 1; i < slotList.size(); ++i) {
            slotList[i].along = std::max(slotList[i].along, slotList[i - 1].along + 16.0);
        }
        if (!slotList.isEmpty() && slotList.back().along > hi) {
            slotList.back().along = hi;
            for (int i = slotList.size() - 2; i >= 0; --i) {
                slotList[i].along = std::min(slotList[i].along, slotList[i + 1].along - 16.0);
            }
        }
        for (const AnchorSlot& anchorSlot : slotList) {
            result.insert(anchorSlot.id,
                          EndpointAnchor{sidePortAnchor(nodeRect, side, anchorSlot.along - centerCoord), side});
        }
    }
    return result;
}

QVector<TouchingTransition> touchingTransitionsFor(const Machine& machine, quint64 stateId,
                                                   const std::function<QRectF(quint64)>& rectOf) {
    QVector<TouchingTransition> touching;
    for (const Transition& transition : machine.transitions) {
        if (transition.from == transition.to || transition.to == 0) {
            continue;
        }
        if (transition.from == 0) {
            continue;
        }
        quint64 otherId = 0;
        bool isIncoming = false;
        if (transition.from == stateId) {
            otherId = transition.to;
            isIncoming = false;
        } else if (transition.to == stateId) {
            otherId = transition.from;
            isIncoming = true;
        } else {
            continue;
        }
        const QRectF otherRect = rectOf(otherId);
        if (otherRect.isNull()) {
            continue;
        }
        touching.push_back(TouchingTransition{transition.id, otherRect, otherId, isIncoming});
    }
    return touching;
}

}  // namespace app
