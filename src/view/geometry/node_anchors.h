#pragma once

#include <functional>

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <QtGlobal>

#include "view/geometry/edge_router.h"

namespace app {

struct Machine;

// A transition endpoint's anchor point in scene coordinates and the side of the
// node rect it sits on.
struct EndpointAnchor {
    QPointF anchor;
    PortSide side;
};

// One transition touching the node whose anchors are being resolved.
// `counterpartRect` is the other endpoint's scene rect, resolved by the caller;
// self-loops and targetless stubs are never included.
struct TouchingTransition {
    quint64 transitionId = 0;
    QRectF counterpartRect;
    quint64 counterpartId = 0;
    bool isIncoming = false;
};

// Which side of `fromCenter`'s rect faces `towardCenter`: the larger axis delta
// wins, ties go to the horizontal sides.
PortSide sideFor(QPointF fromCenter, QPointF towardCenter);

// Resolves each touching transition's anchor and side on a node with scene rect
// `nodeRect`. Transitions are grouped by side (sideFor), each projects onto its
// counterpart's center, transitions sharing a counterpart corridor are spread
// across the side span (edge_router::assignPortOffsets), and adjacent anchors
// are finally nudged apart to a 16px minimum gap. Pure geometry; the caller
// resolves all rects first.
QHash<quint64, EndpointAnchor> computeNodeAnchors(const QRectF& nodeRect, const QVector<TouchingTransition>& touching);

// Every Normal transition touching `stateId`, excluding self-loops, targetless
// stubs and root transitions, each paired with its counterpart's rect from
// `rectOf`; a counterpart with a null rect (not on the scene yet) is skipped.
// Shared by RoutingDelegate::nodeAnchorsFor and the initial auto-layout so the
// two cannot drift.
QVector<TouchingTransition> touchingTransitionsFor(const Machine& machine, quint64 stateId,
                                                   const std::function<QRectF(quint64)>& rectOf);

}  // namespace app
