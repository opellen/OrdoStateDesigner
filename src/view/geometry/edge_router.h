#pragma once

#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QVector>
#include <QtGlobal>

namespace app {

// Pure geometry: orthogonal edge routing with rounded (filleted) corners over
// N-waypoint polylines. No QGraphicsItem/QGraphicsScene dependency; the caller
// turns the results into scene items.

// Which side of a node's rect a routed edge's port sits on.
enum class PortSide { Top, Right, Bottom, Left };

// Edge style, a build-time default plus a setter (View > Edge Style). Every
// style consumes the same waypoint lists; only the path builder differs.
enum class EdgeStyle {
    SmallFillet,  // 10px fillet corners
    LargeFillet,  // ~40px clamped fillets
    Bezier,       // Catmull-Rom cubics through the same waypoints
};
void setEdgeStyle(EdgeStyle style);
EdgeStyle edgeStyle();

// Builds the path for the current EdgeStyle from waypoints.
QPainterPath buildStyledPath(const QVector<QPointF>& points, qreal filletRadius = 10.0);

// Secondary target branch for multi-target transitions.
struct TargetBranch {
    quint64 targetId = 0;
    QPainterPath path;
    QPolygonF arrowhead;
    bool operator==(const TargetBranch&) const = default;
};

// One orthogonal segment of a wire, for interactive segment dragging.
struct WireSegment {
    enum class Orientation { Horizontal, Vertical };
    Orientation orientation = Orientation::Horizontal;
    QLineF line;
    int segmentIndex = -1;
    bool isSourceHalf = true;
    bool operator==(const WireSegment&) const = default;
};

// One fully routed edge, in scene coordinates throughout.
struct RoutedEdge {
    QPainterPath sourceHalf;  // the two drawn halves; a routeThroughPorts route ends them AT the pill's ports
    QPainterPath targetHalf;  // stops short of the target node; the arrowhead fills the approach
    QPointF labelAnchor;      // where the event-label pill goes (the pill's center for a port route)
    QPolygonF arrowhead;      // ~10px triangle centroid-anchored to targetHalf's end, along the approach tangent
    QVector<TargetBranch> secondaryBranches = {};
    QVector<QPointF> sourceWaypoints = {};
    QVector<QPointF> targetWaypoints = {};
    QVector<QPointF> waypoints = {};
    // The offset-free pill anchor (longest segment's midpoint) that
    // routeEdgeFromWaypoints-family routes place the label against; the persisted
    // labelOffset diffs against it. Null on every other route builder.
    QPointF labelBase;
};

// The outward-facing unit normal for a rect side (Top -> up, etc.).
QPointF outwardNormal(PortSide side);

// The point on `rect`'s boundary for a port on `side`, `offset` px from that
// side's center along the side (positive = toward Right/Bottom).
QPointF sidePortAnchor(const QRectF& rect, PortSide side, qreal offset = 0.0);

// Given `edgeCount` edges wanting the same side of one node, returns that many
// offsets (see sidePortAnchor) centered on the side: {0} for one edge, evenly
// spread by `spacing` for more.
QVector<qreal> assignPortOffsets(int edgeCount, qreal spacing = 16.0);

// A ~10px arrowhead triangle, centroid-anchored at `lineStopPoint` (so the point
// stays fixed whatever the size) and pointing along the unit vector
// `travelDirection`. Shared with the initial-state marker.
QPolygonF buildArrowhead(QPointF lineStopPoint, QPointF travelDirection, qreal length = 10.0, qreal halfWidth = 5.0);

// Routes one edge from a source port to a target port, each a point already ON
// its node rect's boundary (e.g. from sidePortAnchor) plus the side it sits on.
// A 5px stub gap leaves each node; the target half stops ~10px short of its stub
// so the arrowhead supplies the approach; the default is a mid-axis two-corner
// route (straight when the ends align); fillet radius is min(filletRadius, half
// the shorter adjoining segment).
RoutedEdge routeEdge(QPointF sourceAnchor, PortSide sourceSide, QPointF targetAnchor, PortSide targetSide,
                      qreal filletRadius = 10.0, qreal fraction = 0.5);

// Routes one continuous Manhattan polyline from sourceAnchor to targetAnchor:
// at most 2 corners (one Z-channel) for parallel runs, 1 for perpendicular L-routes.
// labelAnchor is baseAnchor + labelOffset; the path is unbroken, the pill masks it.
// `labelRatio`, when set, wins over `fraction`: labelBase lands at exactly that
// fraction of the route length, skipping the L-route longer-leg rule and the
// 0.15-0.85 clamp. Set by auto-layout via Transition::labelRatio, never by a pill drag.
RoutedEdge routeUnifiedEdge(QPointF sourceAnchor, PortSide sourceSide,
                            QPointF targetAnchor, PortSide targetSide,
                            qreal filletRadius = 10.0,
                            QPointF labelOffset = QPointF(),
                            qreal fraction = 0.5,
                            std::optional<QPointF> entryPoint = std::nullopt,
                            std::optional<QPointF> exitPoint = std::nullopt,
                            std::optional<qreal> labelRatio = std::nullopt);

// Removes adjacent collinear points from a polyline.
QVector<QPointF> collapseCollinear(const QVector<QPointF>& points);

// The canonical, fully collapsed polyline of a routed edge.
QVector<QPointF> canonicalWaypoints(const RoutedEdge& route);

// Splits an orthogonal waypoint polyline into horizontal and vertical segments.
QVector<WireSegment> extractWireSegments(const QVector<QPointF>& waypoints, bool isSourceHalf = true);

// Translates the segment at `segmentIndex` by `delta` perpendicular to itself,
// adjusting neighbours to keep strict 90-degree connections.
QVector<QPointF> translateOrthogonalSegment(const QVector<QPointF>& waypoints, int segmentIndex, qreal delta);

// Routes an edge along an explicit polyline of orthogonal waypoints.
// The pill sits on the wire: the desired center (longest-segment midpoint +
// labelOffset) projects onto the nearest polyline point and clamps along that
// segment, and the label gap is cut around that point sized by `pillSize`
// (fixed extents when there is no pill), so pill and gap never separate.
RoutedEdge routeEdgeFromWaypoints(const QVector<QPointF>& waypoints,
                                  qreal filletRadius = 10.0,
                                  QPointF labelOffset = QPointF(),
                                  QSizeF pillSize = QSizeF());

// Routes an edge through manual orthogonal bendpoints.
RoutedEdge routeEdgeWithBendpoints(QPointF sourceAnchor, PortSide sourceSide,
                                   QPointF targetAnchor, PortSide targetSide,
                                   const QVector<QPointF>& bendpoints,
                                   qreal filletRadius = 10.0,
                                   QPointF labelOffset = QPointF(),
                                   QSizeF pillSize = QSizeF());

// The committed manual-route entry point: derives each node-side anchor from the
// authored course (the side of `sourceRect`/`targetRect` facing the first/last
// bendpoint, anchored at that bendpoint's clamped coordinate so the stub leaves
// straight when geometry allows), then routes via routeEdgeWithBendpoints. The
// pill-port resolver plays no part: re-routing an authored course through an
// offset pill detaches pills and flips endpoints.
RoutedEdge routeManualEdge(const QRectF& sourceRect, const QRectF& targetRect,
                           const QVector<QPointF>& bendpoints,
                           qreal filletRadius = 10.0,
                           QPointF labelOffset = QPointF(),
                           QSizeF pillSize = QSizeF());

// True iff the skeleton crosses neither endpoint node's body and the target
// half's last real chord (>= 4px, so a micro-tail cannot vouch for a diagonal
// body) feeds the arrow parallel. A stored course a node move has made
// degenerate fails; the caller then falls back to the auto route.
bool manualRouteWellFormed(const RoutedEdge& routed, const QRectF& sourceRect, const QRectF& targetRect);

// ---- Port-based through-pill assembly ------
// Every half terminates AT a pill side-middle port, perpendicular to that side;
// the pill's body is the gap between the halves.

// The drawn source half's start point: `anchor` pushed kStubGap off the node.
QPointF sourceHalfStart(QPointF anchor, PortSide side);
// The drawn target half's stop point: kStubGap + kArrowApproach off the node,
// so the arrowhead anchored there supplies the final approach.
QPointF targetHalfStop(QPointF anchor, PortSide side);

// One half's orthogonal run: leaves `from` along `fromDir` and arrives at `to`
// along `toDir` (both unit axis vectors). Straight when both ends align on one
// axis, an L when the directions are perpendicular and the corner lies ahead of
// both, a two-corner Z otherwise; a geometry that fits none of those (a port
// whose counterpart sits behind it) falls back to a three-corner form rather
// than violating either end direction.
QVector<QPointF> orthogonalPortRun(QPointF from, QPointF fromDir, QPointF to, QPointF toDir, qreal minRun = 12.0);

// Defined in pill_port_resolver.h; passed by reference so this header stays the
// lower layer.
struct PortAssignment;

// Assembles a resolved pill route: the source half runs `sourceAnchor ->
// entryPoint` (arriving perpendicular to entrySide) and the target half
// `exitPoint -> target anchor`, with the usual stub-gap, arrow-approach and
// arrowhead conventions at the node end. labelAnchor is the pill center. A half
// whose port the assignment marks absent (Targetless has no exit, Root no entry)
// is not built.
RoutedEdge routeThroughPorts(const PortAssignment& ports, qreal filletRadius = 10.0);

// Routes a filleted orthogonal branch from the pill to an additional target
// state of a multi-target transition.
TargetBranch routeTargetBranch(const QRectF& pillRect, const QRectF& targetRect, quint64 targetId,
                               qreal filletRadius = 10.0);

// Self-transition loop (from == to): leaves the node's Top side, rises, runs
// right past the top-right corner and drops into the Right side; arrowhead on
// the return, pill centered on the top run. `slot` (0-based, per node) pushes
// further loops outward so they nest.
RoutedEdge routeSelfLoop(const QRectF& nodeRect, int slot = 0, qreal filletRadius = 10.0,
                         QSizeF pillSize = QSizeF());

// Root-event stub: a machine-level (from == 0) transition's pill sits outside the
// machine frame with a short run into the frame edge; arrowhead into the frame
// for a targeted root transition, bare stub for a targetless one.
// `frameEdgePoint` lies on the frame boundary; `side` is the frame side it is on,
// and the stub runs outward along that side's normal.
RoutedEdge routeRootStub(QPointF frameEdgePoint, bool arrowIntoFrame, PortSide side = PortSide::Right);

// Targetless-transition stub (to == 0): a short straight drop off the node's
// Bottom side ending AT the label anchor, with no target half or arrowhead.
// `slot` (0-based, per node) spreads several stubs along the bottom side.
RoutedEdge routeTargetlessStub(const QRectF& nodeRect, int slot = 0);

// The wire-drag rubber preview: one continuous filleted mid-axis path from the
// grabbed anchor to the free end, without routeEdge's committed-edge extras
// (stub gaps, label split, arrowhead). A drag under 0.01 collapses to a point
// path. Uses a 10px fillet.
QPainterPath buildWireDragPreviewPath(QPointF from, PortSide fromSide, QPointF to);

}  // namespace app
