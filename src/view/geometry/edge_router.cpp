#include "view/geometry/edge_router.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "view/geometry/pill_port_resolver.h"
#include "view/geometry/routing_logger.h"

namespace app {

namespace {

constexpr qreal kStubGap = 5.0;         // the line starts/ends ~5px off the rect
constexpr qreal kArrowApproach = 10.0;  // the target half stops ~10px short of its stub
constexpr qreal kLabelGapHalf = 20.0;   // ~40px gap around the label, split evenly
// Minimum straight run a routed half keeps at each end: a channel closer than
// this to an end's approach coordinate erases the final segment (stranding the
// arrowhead) or reverses the approach. Same value as orthogonalPortRun's minRun.
constexpr qreal kMinEndRun = 12.0;
constexpr qreal kArrowLength = 10.0;
constexpr qreal kArrowHalfWidth = 5.0;

qreal distance(QPointF a, QPointF b) { return std::hypot(b.x() - a.x(), b.y() - a.y()); }

// Drops consecutive near-duplicate points; a zero-length segment would divide
// by zero in the fillet math below.
QVector<QPointF> dedupAdjacent(const QVector<QPointF>& points) {
    QVector<QPointF> out;
    for (const QPointF& p : points) {
        if (out.isEmpty() || distance(out.back(), p) > 0.01) {
            out.push_back(p);
        }
    }
    return out;
}

// Mid-axis two-corner default waypoints from `from` (leaving via `fromSide`) to
// `to`: horizontal-vertical-horizontal through the midpoint x when the exit is
// Left/Right, vertical-horizontal-vertical through the midpoint y otherwise.
QVector<QPointF> midAxisWaypoints(QPointF from, PortSide fromSide, QPointF to) {
    QVector<QPointF> points;
    points.push_back(from);
    if (fromSide == PortSide::Left || fromSide == PortSide::Right) {
        if (std::abs(from.y() - to.y()) > 0.01) {
            const qreal midX = (from.x() + to.x()) / 2.0;
            points.push_back(QPointF(midX, from.y()));
            points.push_back(QPointF(midX, to.y()));
        }
    } else {
        if (std::abs(from.x() - to.x()) > 0.01) {
            const qreal midY = (from.y() + to.y()) / 2.0;
            points.push_back(QPointF(from.x(), midY));
            points.push_back(QPointF(to.x(), midY));
        }
    }
    points.push_back(to);
    return collapseCollinear(dedupAdjacent(points));
}

QVector<qreal> cumulativeLengths(const QVector<QPointF>& points) {
    QVector<qreal> cumulative(points.size(), 0.0);
    for (int i = 1; i < points.size(); ++i) {
        cumulative[i] = cumulative[i - 1] + distance(points[i - 1], points[i]);
    }
    return cumulative;
}

// The point at arc length `len` along the polyline (clamped to its length), plus
// the index of its segment (segment i spans points[i]..points[i+1]).
QPointF pointAtLength(const QVector<QPointF>& points, const QVector<qreal>& cumulative, qreal len,
                      int* segmentIndexOut) {
    const qreal total = cumulative.back();
    len = std::clamp(len, 0.0, total);
    int i = 1;
    while (i < cumulative.size() - 1 && cumulative[i] < len) {
        ++i;
    }
    if (segmentIndexOut != nullptr) {
        *segmentIndexOut = i - 1;
    }
    const qreal segmentLength = cumulative[i] - cumulative[i - 1];
    const qreal t = segmentLength > 1e-9 ? (len - cumulative[i - 1]) / segmentLength : 0.0;
    return points[i - 1] + (points[i] - points[i - 1]) * t;
}

// The sub-polyline spanning arc length [from, to]: the two interpolated endpoints
// plus any original waypoint strictly between them, so a corner inside a half
// keeps its fillet and one at the cut gets a shrunk one.
QVector<QPointF> slicePolyline(const QVector<QPointF>& points, const QVector<qreal>& cumulative, qreal from,
                               qreal to) {
    int fromSegment = 0;
    int toSegment = 0;
    const QPointF start = pointAtLength(points, cumulative, from, &fromSegment);
    const QPointF end = pointAtLength(points, cumulative, to, &toSegment);
    QVector<QPointF> out;
    out.push_back(start);
    for (int i = fromSegment + 1; i <= toSegment; ++i) {
        out.push_back(points[i]);
    }
    out.push_back(end);
    return dedupAdjacent(out);
}

// Straight runs between fillets over an N-point list; both quadTo control points
// collapse onto the corner vertex, radius min(filletRadius, half the shorter
// adjoining segment). Fewer than 3 points need no fillet.
QPainterPath buildFilletedPath(const QVector<QPointF>& points, qreal filletRadius) {
    QPainterPath path;
    if (points.isEmpty()) {
        return path;
    }
    path.moveTo(points.front());
    if (points.size() < 3) {
        if (points.size() == 2) {
            path.lineTo(points.back());
        }
        return path;
    }
    for (int i = 1; i < points.size() - 1; ++i) {
        const QPointF prev = points[i - 1];
        const QPointF curr = points[i];
        const QPointF next = points[i + 1];
        const qreal len1 = distance(prev, curr);
        const qreal len2 = distance(curr, next);
        const qreal r = std::min({filletRadius, len1 / 2.0, len2 / 2.0});
        const QPointF unit1 = (curr - prev) / len1;
        const QPointF unit2 = (next - curr) / len2;
        path.lineTo(curr - unit1 * r);
        path.quadTo(curr, curr + unit2 * r);
    }
    path.lineTo(points.back());
    return path;
}

// Read by buildStyledPath at every (re)route.
EdgeStyle currentEdgeStyle = EdgeStyle::SmallFillet;

// Catmull-Rom through the waypoints, each span converted to one cubic. A 2-point
// list stays a line.
QPainterPath buildBezierPath(const QVector<QPointF>& points) {
    QPainterPath path;
    if (points.isEmpty()) {
        return path;
    }
    path.moveTo(points.front());
    if (points.size() == 2) {
        path.lineTo(points.back());
        return path;
    }
    for (int i = 0; i + 1 < points.size(); ++i) {
        const QPointF p0 = points[std::max(i - 1, 0)];
        const QPointF p1 = points[i];
        const QPointF p2 = points[i + 1];
        const QPointF p3 = points[std::min(i + 2, static_cast<int>(points.size()) - 1)];
        path.cubicTo(p1 + (p2 - p0) / 6.0, p2 - (p3 - p1) / 6.0, p2);
    }
    return path;
}

}  // namespace

// All route path construction goes through here, so the styles differ only in
// rendering, not geometry.
QPainterPath buildStyledPath(const QVector<QPointF>& points, qreal filletRadius) {
    switch (currentEdgeStyle) {
        case EdgeStyle::LargeFillet:
            return buildFilletedPath(points, 40.0);
        case EdgeStyle::Bezier:
            return buildBezierPath(points);
        case EdgeStyle::SmallFillet:
            break;
    }
    return buildFilletedPath(points, filletRadius);
}

void setEdgeStyle(EdgeStyle style) { currentEdgeStyle = style; }
EdgeStyle edgeStyle() { return currentEdgeStyle; }

QPointF outwardNormal(PortSide side) {
    switch (side) {
        case PortSide::Top:
            return QPointF(0.0, -1.0);
        case PortSide::Bottom:
            return QPointF(0.0, 1.0);
        case PortSide::Left:
            return QPointF(-1.0, 0.0);
        case PortSide::Right:
            return QPointF(1.0, 0.0);
    }
    return QPointF(0.0, 0.0);
}

QPointF sidePortAnchor(const QRectF& rect, PortSide side, qreal offset) {
    switch (side) {
        case PortSide::Top:
            return QPointF(rect.center().x() + offset, rect.top());
        case PortSide::Bottom:
            return QPointF(rect.center().x() + offset, rect.bottom());
        case PortSide::Left:
            return QPointF(rect.left(), rect.center().y() + offset);
        case PortSide::Right:
            return QPointF(rect.right(), rect.center().y() + offset);
    }
    return rect.center();
}

QVector<qreal> assignPortOffsets(int edgeCount, qreal spacing) {
    QVector<qreal> offsets(std::max(edgeCount, 0));
    for (int i = 0; i < edgeCount; ++i) {
        offsets[i] = (static_cast<qreal>(i) - (static_cast<qreal>(edgeCount) - 1.0) / 2.0) * spacing;
    }
    return offsets;
}

QPolygonF buildArrowhead(QPointF lineStopPoint, QPointF travelDirection, qreal length, qreal halfWidth) {
    // The centroid sits 1/3 of the height from the base, so pinning it at
    // `lineStopPoint` keeps that point fixed however `length`/`halfWidth` change.
    const QPointF apex = lineStopPoint + travelDirection * (length * 2.0 / 3.0);
    const QPointF baseCenter = lineStopPoint - travelDirection * (length / 3.0);
    const QPointF perpendicular(-travelDirection.y(), travelDirection.x());
    return QPolygonF({apex, baseCenter + perpendicular * halfWidth, baseCenter - perpendicular * halfWidth});
}

RoutedEdge routeEdge(QPointF sourceAnchor, PortSide sourceSide, QPointF targetAnchor, PortSide targetSide,
                      qreal filletRadius, qreal fraction) {
    const QPointF sourceOut = outwardNormal(sourceSide);
    const QPointF targetOut = outwardNormal(targetSide);

    const QPointF sourceStub = sourceAnchor + sourceOut * kStubGap;
    // The drawn line ends kArrowApproach short of the target's stub; the
    // arrowhead fills the gap.
    const QPointF lineStop = targetAnchor + targetOut * (kStubGap + kArrowApproach);

    QVector<QPointF> waypoints;
    const bool sourceHoriz = sourceSide == PortSide::Left || sourceSide == PortSide::Right;
    const bool targetHoriz = targetSide == PortSide::Left || targetSide == PortSide::Right;
    if (sourceHoriz != targetHoriz) {
        const QPointF corner = sourceHoriz ? QPointF(lineStop.x(), sourceStub.y())
                                           : QPointF(sourceStub.x(), lineStop.y());
        const auto ahead = [](QPointF a, QPointF b, QPointF dir) {
            return QPointF::dotProduct(b - a, dir) > 0.01;
        };
        if (ahead(sourceStub, corner, sourceOut) && ahead(corner, lineStop, -targetOut)) {
            waypoints = dedupAdjacent(QVector<QPointF>{sourceStub, corner, lineStop});
        }
    }
    if (waypoints.isEmpty()) {
        waypoints = midAxisWaypoints(sourceStub, sourceSide, lineStop);
    }
    waypoints = collapseCollinear(dedupAdjacent(waypoints));
    if (waypoints.size() < 2) {
        // Degenerate (stubs coincide); avoids the divide-by-zero paths below.
        waypoints.push_back(waypoints.isEmpty() ? sourceStub : waypoints.back());
    }
    const QVector<qreal> cumulative = cumulativeLengths(waypoints);
    const qreal total = cumulative.back();
    qreal labelLen = 0.0;
    const bool isLRoute = waypoints.size() == 3 &&
                          std::abs(waypoints[0].x() - waypoints[2].x()) > 0.01 &&
                          std::abs(waypoints[0].y() - waypoints[2].y()) > 0.01;
    if (isLRoute) {
        // Pin the label to the middle of the L-route's longer leg.
        const qreal seg1 = cumulative[1] - cumulative[0];
        const qreal seg2 = cumulative[2] - cumulative[1];
        if (seg1 >= seg2) {
            labelLen = cumulative[0] + seg1 * 0.5;
        } else {
            labelLen = cumulative[1] + seg2 * 0.5;
        }
    } else {
        const qreal clampedFraction = std::clamp(fraction, 0.15, 0.85);
        labelLen = total * clampedFraction;
    }
    const qreal gapHalf = std::min(kLabelGapHalf, std::min(labelLen, total - labelLen));

    RoutedEdge routed;
    routed.labelAnchor = pointAtLength(waypoints, cumulative, labelLen, nullptr);
    routed.waypoints = waypoints;
    routed.sourceWaypoints = slicePolyline(waypoints, cumulative, 0.0, labelLen - gapHalf);
    routed.targetWaypoints = slicePolyline(waypoints, cumulative, labelLen + gapHalf, total);
    routed.sourceHalf = buildStyledPath(routed.sourceWaypoints, filletRadius);
    routed.targetHalf = buildStyledPath(routed.targetWaypoints, filletRadius);

    // The final segment always runs along -targetOut into lineStop, so this is
    // the end tangent.
    const QPointF travelDir = -targetOut;
    routed.arrowhead = buildArrowhead(lineStop, travelDir, kArrowLength, kArrowHalfWidth);

    return routed;
}

static std::optional<qreal> projectPointToPolylineLength(const QVector<QPointF>& waypoints,
                                                         const QVector<qreal>& cumulative,
                                                         QPointF pt) {
    if (waypoints.size() < 2 || cumulative.size() != waypoints.size()) {
        return std::nullopt;
    }
    for (int i = 0; i < waypoints.size() - 1; ++i) {
        const QPointF a = waypoints[i];
        const QPointF b = waypoints[i + 1];
        const QPointF v = b - a;
        const qreal segLen = cumulative[i + 1] - cumulative[i];
        if (segLen < 1e-4) {
            continue;
        }
        const QPointF dir = v / segLen;
        const qreal t = QPointF::dotProduct(pt - a, dir);
        if (t >= -0.1 && t <= segLen + 0.1) {
            const QPointF proj = a + dir * std::clamp(t, 0.0, segLen);
            if (std::hypot(pt.x() - proj.x(), pt.y() - proj.y()) <= 1.0) {
                return cumulative[i] + std::clamp(t, 0.0, segLen);
            }
        }
    }
    return std::nullopt;
}

RoutedEdge routeUnifiedEdge(QPointF sourceAnchor, PortSide sourceSide,
                            QPointF targetAnchor, PortSide targetSide,
                            qreal filletRadius, QPointF labelOffset, qreal fraction,
                            std::optional<QPointF> entryPoint,
                            std::optional<QPointF> exitPoint,
                            std::optional<qreal> labelRatio) {
    const QPointF sourceOut = outwardNormal(sourceSide);
    const QPointF targetOut = outwardNormal(targetSide);

    // Short corridor: a facing pair closer than the standard offsets (5px stub +
    // 15px arrow stop = 20px) would put lineStop behind sourceStub and force
    // orthogonalPortRun's multi-corner wrap (a container's ~16px hull padding, or
    // two states dragged together). Shrink the offsets to fit, in the same 1:3
    // split, keeping a sliver of drawn run; 20px or more is untouched.
    qreal stubGap = kStubGap;
    qreal stopOffset = kStubGap + kArrowApproach;
    if (sourceOut == -targetOut) {
        const qreal available = QPointF::dotProduct(targetAnchor - sourceAnchor, sourceOut);
        if (available > 0.0 && available < stubGap + stopOffset) {
            const qreal ceremony = std::max(available - 2.0, 2.0);
            stubGap = ceremony * 0.25;
            stopOffset = ceremony * 0.75;
        }
    }
    const QPointF sourceStub = sourceAnchor + sourceOut * stubGap;
    const QPointF lineStop = targetAnchor + targetOut * stopOffset;

    QVector<QPointF> waypoints = orthogonalPortRun(sourceStub, sourceOut, lineStop, -targetOut);
    waypoints = collapseCollinear(dedupAdjacent(waypoints));
    if (waypoints.size() < 2) {
        waypoints = {sourceStub, lineStop};
    }

    const QVector<qreal> cumulative = cumulativeLengths(waypoints);
    const qreal total = cumulative.back();
    qreal labelLen = 0.0;
    if (labelRatio.has_value()) {
        // The caller's ratio wins verbatim: neither the L-route longer-leg rule
        // nor the 0.15-0.85 clamp below applies.
        labelLen = total * std::clamp(*labelRatio, 0.0, 1.0);
    } else {
        const bool isLRoute = waypoints.size() == 3 &&
                              std::abs(waypoints[0].x() - waypoints[2].x()) > 0.01 &&
                              std::abs(waypoints[0].y() - waypoints[2].y()) > 0.01;
        if (isLRoute) {
            const qreal seg1 = cumulative[1] - cumulative[0];
            const qreal seg2 = cumulative[2] - cumulative[1];
            if (seg1 >= seg2) {
                labelLen = cumulative[0] + seg1 * 0.5;
            } else {
                labelLen = cumulative[1] + seg2 * 0.5;
            }
        } else {
            const qreal clampedFraction = std::clamp(fraction, 0.15, 0.85);
            labelLen = total * clampedFraction;
        }
    }

    qreal entryLen = labelLen;
    qreal exitLen = labelLen;
    if (entryPoint.has_value() && exitPoint.has_value() && labelOffset.manhattanLength() <= 0.01) {
        const auto eLen = projectPointToPolylineLength(waypoints, cumulative, *entryPoint);
        const auto xLen = projectPointToPolylineLength(waypoints, cumulative, *exitPoint);
        if (eLen.has_value() && xLen.has_value() && *eLen < *xLen && *eLen > 0.0 && *xLen < total) {
            entryLen = *eLen;
            exitLen = *xLen;
        }
    }

    RoutedEdge routed;
    const QPointF baseAnchor = pointAtLength(waypoints, cumulative, labelLen, nullptr);
    routed.labelBase = baseAnchor;
    routed.labelAnchor = baseAnchor + labelOffset;
    routed.waypoints = waypoints;
    routed.sourceWaypoints = slicePolyline(waypoints, cumulative, 0.0, entryLen);
    routed.targetWaypoints = slicePolyline(waypoints, cumulative, exitLen, total);
    routed.sourceHalf = buildStyledPath(routed.sourceWaypoints, filletRadius);
    routed.targetHalf = buildStyledPath(routed.targetWaypoints, filletRadius);

    const QPointF travelDir = -targetOut;
    routed.arrowhead = buildArrowhead(lineStop, travelDir, kArrowLength, kArrowHalfWidth);

    return routed;
}

QVector<WireSegment> extractWireSegments(const QVector<QPointF>& waypoints, bool isSourceHalf) {
    QVector<WireSegment> segments;
    if (waypoints.size() < 2) {
        return segments;
    }
    for (int i = 0; i < waypoints.size() - 1; ++i) {
        const QPointF p1 = waypoints[i];
        const QPointF p2 = waypoints[i + 1];
        const qreal dx = std::abs(p1.x() - p2.x());
        const qreal dy = std::abs(p1.y() - p2.y());
        if (dx * dx + dy * dy < 9.0) {
            continue;
        }
        WireSegment seg;
        seg.line = QLineF(p1, p2);
        seg.segmentIndex = i;
        seg.isSourceHalf = isSourceHalf;
        seg.orientation = (dy <= dx) ? WireSegment::Orientation::Horizontal : WireSegment::Orientation::Vertical;
        segments.push_back(seg);
    }
    return segments;
}

QVector<QPointF> collapseCollinear(const QVector<QPointF>& points) {
    if (points.size() < 3) {
        return points;
    }
    QVector<QPointF> out;
    out.push_back(points[0]);
    for (int i = 1; i < points.size() - 1; ++i) {
        const QPointF prev = out.back();
        const QPointF curr = points[i];
        const QPointF next = points[i + 1];
        const bool horiz = std::abs(prev.y() - curr.y()) < 0.01 && std::abs(curr.y() - next.y()) < 0.01;
        const bool vert = std::abs(prev.x() - curr.x()) < 0.01 && std::abs(curr.x() - next.x()) < 0.01;
        if (!horiz && !vert) {
            out.push_back(curr);
        }
    }
    out.push_back(points.back());
    return out;
}

QVector<QPointF> canonicalWaypoints(const RoutedEdge& route) {
    if (route.waypoints.size() >= 2) {
        return collapseCollinear(dedupAdjacent(route.waypoints));
    }
    QVector<QPointF> combined = route.sourceWaypoints;
    if (!route.sourceWaypoints.isEmpty() && !route.targetWaypoints.isEmpty()) {
        const QPointF pEnd = route.sourceWaypoints.back();
        const QPointF pStart = route.targetWaypoints.front();
        if (std::abs(pEnd.x() - pStart.x()) > 0.01 && std::abs(pEnd.y() - pStart.y()) > 0.01) {
            const QPointF mid = !route.labelBase.isNull() ? route.labelBase : route.labelAnchor;
            if (!mid.isNull()) {
                combined.push_back(mid);
            }
        }
    }
    for (const auto& pt : route.targetWaypoints) {
        combined.push_back(pt);
    }
    return collapseCollinear(dedupAdjacent(combined));
}

QVector<QPointF> translateOrthogonalSegment(const QVector<QPointF>& waypointsIn, int segmentIndex, qreal delta) {
    const QVector<QPointF> waypoints = collapseCollinear(dedupAdjacent(waypointsIn));
    if (waypoints.size() < 2 || segmentIndex < 0 || segmentIndex >= waypoints.size() - 1) {
        return waypoints;
    }
    if (std::abs(delta) < 0.01) {
        return waypoints;
    }

    const QPointF p1 = waypoints[segmentIndex];
    const QPointF p2 = waypoints[segmentIndex + 1];
    const bool isHorizontal = std::abs(p1.y() - p2.y()) <= std::abs(p1.x() - p2.x());
    const QPointF d = isHorizontal ? QPointF(0.0, delta) : QPointF(delta, 0.0);

    QVector<QPointF> result;
    if (waypoints.size() == 2) {
        // Straight line: a parallel drag makes a 3-segment jog that keeps the end anchors.
        result = {waypoints[0], waypoints[0] + d, waypoints[1] + d, waypoints[1]};
    } else if (segmentIndex == 0) {
        // First segment: keep P0 and add an orthogonal stub to the translated segment.
        result.push_back(waypoints[0]);
        result.push_back(waypoints[0] + d);
        result.push_back(waypoints[1] + d);
        for (int i = 2; i < waypoints.size(); ++i) {
            result.push_back(waypoints[i]);
        }
    } else if (segmentIndex == waypoints.size() - 2) {
        // Final segment: translate it and add an orthogonal stub to the last point.
        for (int i = 0; i < segmentIndex; ++i) {
            result.push_back(waypoints[i]);
        }
        result.push_back(waypoints[segmentIndex] + d);
        result.push_back(waypoints.back() + d);
        result.push_back(waypoints.back());
    } else {
        // Interior segment: slide adjacent perpendicular segments
        result = waypoints;
        result[segmentIndex] = result[segmentIndex] + d;
        result[segmentIndex + 1] = result[segmentIndex + 1] + d;
    }

    result = collapseCollinear(dedupAdjacent(result));

    // Every adjacent segment must be strictly orthogonal (dx <= 0.01 || dy <= 0.01).
    for (int i = 0; i < result.size() - 1; ++i) {
        const qreal dx = std::abs(result[i].x() - result[i + 1].x());
        const qreal dy = std::abs(result[i].y() - result[i + 1].y());
        if (dx > 0.01 && dy > 0.01) {
            if (dx >= dy) {
                result[i + 1].setY(result[i].y());
            } else {
                result[i + 1].setX(result[i].x());
            }
        }
    }

    return collapseCollinear(dedupAdjacent(result));
}

RoutedEdge routeEdgeFromWaypoints(const QVector<QPointF>& waypointsIn,
                                  qreal filletRadius,
                                  QPointF labelOffset,
                                  QSizeF pillSize) {
    QVector<QPointF> baseWaypoints = collapseCollinear(dedupAdjacent(waypointsIn));
    if (baseWaypoints.size() < 2) {
        return RoutedEdge{};
    }

    // Every adjacent segment must be strictly orthogonal (no diagonals).
    QVector<QPointF> orthogonalized;
    orthogonalized.push_back(baseWaypoints[0]);
    for (int i = 0; i < baseWaypoints.size() - 1; ++i) {
        const QPointF p1 = orthogonalized.back();
        const QPointF p2 = baseWaypoints[i + 1];
        const qreal dx = std::abs(p1.x() - p2.x());
        const qreal dy = std::abs(p1.y() - p2.y());
        if (dx > 0.01 && dy > 0.01) {
            if (dx >= dy) {
                orthogonalized.push_back(QPointF(p2.x(), p1.y()));
            } else {
                orthogonalized.push_back(QPointF(p1.x(), p2.y()));
            }
        }
        orthogonalized.push_back(p2);
    }
    const QVector<QPointF> waypoints = collapseCollinear(dedupAdjacent(orthogonalized));
    if (waypoints.size() < 2) {
        return RoutedEdge{};
    }

    const QVector<qreal> cumulative = cumulativeLengths(waypoints);
    const qreal total = cumulative.back();

    // The offset-free base anchor (RoutedEdge::labelBase), origin of the persisted
    // labelOffset: the longest segment's midpoint, off any fillet.
    qreal maxSegLen = 0.0;
    int longestSeg = 0;
    for (int i = 0; i < waypoints.size() - 1; ++i) {
        const qreal segLen = cumulative[i + 1] - cumulative[i];
        if (segLen > maxSegLen) {
            maxSegLen = segLen;
            longestSeg = i;
        }
    }
    const QPointF baseAnchor =
        pointAtLength(waypoints, cumulative, cumulative[longestSeg] + maxSegLen * 0.5, nullptr);

    // On an authored course the pill rides the wire: the desired center (base +
    // offset) projects onto the nearest polyline point, so the gap is cut where
    // the pill sits and the two never separate, whatever the offset.
    const QPointF desired = baseAnchor + labelOffset;
    qreal bestDistSq = 1e18;
    int bestSeg = longestSeg;
    qreal bestT = 0.5;
    for (int i = 0; i < waypoints.size() - 1; ++i) {
        const QPointF a = waypoints[i];
        const QPointF v = waypoints[i + 1] - a;
        const qreal lenSq = QPointF::dotProduct(v, v);
        if (lenSq < 1e-6) {
            continue;
        }
        const qreal t = std::clamp(QPointF::dotProduct(desired - a, v) / lenSq, 0.0, 1.0);
        const QPointF proj = a + v * t;
        const qreal distSq = QPointF::dotProduct(desired - proj, desired - proj);
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestSeg = i;
            bestT = t;
        }
    }

    // Gap half-extent along the pinned segment: the pill's body half plus 6px when
    // supplied, fixed extents otherwise; at most ~45% of the segment, and the
    // pinned point clamps inward by the same amount so the cut never crosses a corner.
    const qreal segStart = cumulative[bestSeg];
    const qreal segLen = cumulative[bestSeg + 1] - segStart;
    const QPointF pA = waypoints[bestSeg];
    const QPointF pB = waypoints[bestSeg + 1];
    const bool isHoriz = std::abs(pA.y() - pB.y()) <= std::abs(pA.x() - pB.x());
    qreal halfExtent = !pillSize.isEmpty()
                           ? (isHoriz ? pillSize.width() / 2.0 + 6.0 : pillSize.height() / 2.0 + 6.0)
                           : (isHoriz ? 28.0 : 16.0);
    halfExtent = std::min(halfExtent, segLen * 0.45);
    const qreal labelLen =
        std::clamp(segStart + bestT * segLen, segStart + halfExtent, segStart + segLen - halfExtent);

    RoutedEdge routed;
    routed.labelBase = baseAnchor;
    routed.labelAnchor = baseAnchor + labelOffset;
    routed.waypoints = waypoints;
    routed.sourceWaypoints = slicePolyline(waypoints, cumulative, 0.0, labelLen);
    routed.targetWaypoints = slicePolyline(waypoints, cumulative, labelLen, total);
    routed.sourceHalf = buildStyledPath(routed.sourceWaypoints, filletRadius);
    routed.targetHalf = buildStyledPath(routed.targetWaypoints, filletRadius);

    // Arrow direction: the last segment long enough (>= 4px) to be a real
    // approach, so a micro-jog at the tail cannot turn the arrow sideways.
    QPointF travelDir(1.0, 0.0);
    for (int i = waypoints.size() - 1; i >= 1; --i) {
        const qreal segLen = distance(waypoints[i - 1], waypoints[i]);
        if (segLen >= 4.0) {
            travelDir = (waypoints[i] - waypoints[i - 1]) / segLen;
            break;
        }
    }
    routed.arrowhead = buildArrowhead(waypoints.back(), travelDir, kArrowLength, kArrowHalfWidth);
    return routed;
}

RoutedEdge routeEdgeWithBendpoints(QPointF sourceAnchor, PortSide sourceSide,
                                   QPointF targetAnchor, PortSide targetSide,
                                   const QVector<QPointF>& bendpoints,
                                   qreal filletRadius,
                                   QPointF labelOffset,
                                   QSizeF pillSize) {
    const QPointF sourceOut = outwardNormal(sourceSide);
    const QPointF targetOut = outwardNormal(targetSide);
    const QPointF sourceStub = sourceAnchor + sourceOut * kStubGap;
    const QPointF lineStop = targetAnchor + targetOut * (kStubGap + kArrowApproach);

    QVector<QPointF> raw;
    raw.push_back(sourceStub);

    if (!bendpoints.isEmpty()) {
        // Bridge sourceStub to the first bendpoint orthogonally, respecting the departure normal.
        const QPointF firstBp = bendpoints.front();
        const bool srcHoriz = std::abs(sourceOut.x()) > 0.5;
        const bool firstOrthogonal = std::abs(sourceStub.x() - firstBp.x()) < 0.01 ||
                                     std::abs(sourceStub.y() - firstBp.y()) < 0.01;
        if (!firstOrthogonal) {
            const QPointF corner = srcHoriz ? QPointF(firstBp.x(), sourceStub.y())
                                            : QPointF(sourceStub.x(), firstBp.y());
            raw.push_back(corner);
        }

        for (const QPointF& pt : bendpoints) {
            raw.push_back(pt);
        }

        // Bridge the last bendpoint to lineStop orthogonally, respecting the approach normal.
        const QPointF lastBp = bendpoints.back();
        const bool tgtHoriz = std::abs(targetOut.x()) > 0.5;
        const bool lastOrthogonal = std::abs(lastBp.x() - lineStop.x()) < 0.01 ||
                                    std::abs(lastBp.y() - lineStop.y()) < 0.01;
        if (!lastOrthogonal) {
            const QPointF corner = tgtHoriz ? QPointF(lastBp.x(), lineStop.y())
                                            : QPointF(lineStop.x(), lastBp.y());
            raw.push_back(corner);
        }
    }

    raw.push_back(lineStop);

    RoutedEdge routed = routeEdgeFromWaypoints(raw, filletRadius, labelOffset, pillSize);
    // The arrow aims along the target side's inward normal (-targetOut), as in
    // routeEdge, not along the course's last segment, which can run along the
    // approach height and point the arrow sideways.
    routed.arrowhead = buildArrowhead(lineStop, -targetOut, kArrowLength, kArrowHalfWidth);
    return routed;
}

namespace {

// The side of `rect` facing `p`: the largest outside overhang wins; a point inside
// falls back to the dominant axis from the center, normalized by the half-extents
// so wide-flat nodes don't always answer Left/Right.
PortSide sideFacing(const QRectF& rect, QPointF p) {
    const qreal dxL = rect.left() - p.x();
    const qreal dxR = p.x() - rect.right();
    const qreal dyT = rect.top() - p.y();
    const qreal dyB = p.y() - rect.bottom();
    const qreal overhang = std::max({dxL, dxR, dyT, dyB});
    if (overhang <= 0.0) {
        const QPointF c = rect.center();
        const qreal nx = (p.x() - c.x()) / std::max(1.0, rect.width() / 2.0);
        const qreal ny = (p.y() - c.y()) / std::max(1.0, rect.height() / 2.0);
        return std::abs(nx) >= std::abs(ny) ? (nx >= 0.0 ? PortSide::Right : PortSide::Left)
                                            : (ny >= 0.0 ? PortSide::Bottom : PortSide::Top);
    }
    if (overhang == dxL) return PortSide::Left;
    if (overhang == dxR) return PortSide::Right;
    if (overhang == dyT) return PortSide::Top;
    return PortSide::Bottom;
}

// Usable span inset inside a side's corners; shared by the side pickers and
// sideAnchorToward so "fits the span" and "where the anchor clamps" agree.
constexpr qreal kSideCornerInset = 12.0;

// Minimum depth a perpendicular attach needs beyond its side: stub gap + arrow
// approach + a visible straight run that survives the fillet. Inside this band
// the approach would drown in the fillet, so the attach flips to another port.
constexpr qreal kApproachKeepout = kStubGap + kArrowApproach + kMinEndRun;

// Travel-aware side pick for the target end, in three tiers: glide (the node lies
// ahead and the run's perpendicular coordinate fits the facing side's span, so
// the run feeds the arrow); perpendicular (the course passes over/under/beside the
// node with >= kApproachKeepout of depth, dropping into the facing side); keepout
// flip (inside that band, attach through the flank the run crosses, and
// routeManualEdge snaps the run onto the port line).
std::optional<PortSide> sideFromArrival(const QRectF& rect, QPointF prev, QPointF last) {
    const QPointF travel = last - prev;
    if (std::abs(travel.x()) < 0.01 && std::abs(travel.y()) < 0.01) {
        return std::nullopt;
    }
    if (std::abs(travel.x()) >= std::abs(travel.y())) {
        const bool inYSpan =
            last.y() >= rect.top() + kSideCornerInset && last.y() <= rect.bottom() - kSideCornerInset;
        if (travel.x() > 0.0 && rect.left() >= last.x() && inYSpan) return PortSide::Left;
        if (travel.x() < 0.0 && rect.right() <= last.x() && inYSpan) return PortSide::Right;
        // Sticky: before crossing the side's boundary plane, stay Top / Bottom.
        if (last.y() < rect.top()) return PortSide::Top;
        if (last.y() > rect.bottom()) return PortSide::Bottom;
        // Flip only when the run genuinely reaches that flank (its start at or
        // beyond the flank's plane); otherwise the node is behind the run and the
        // wire would be drawn backward through it. No flip: positional fallback.
        if (travel.x() > 0.0 && prev.x() <= rect.left()) return PortSide::Left;
        if (travel.x() < 0.0 && prev.x() >= rect.right()) return PortSide::Right;
        return std::nullopt;
    }
    const bool inXSpan =
        last.x() >= rect.left() + kSideCornerInset && last.x() <= rect.right() - kSideCornerInset;
    if (travel.y() > 0.0 && rect.top() >= last.y() && inXSpan) return PortSide::Top;
    if (travel.y() < 0.0 && rect.bottom() <= last.y() && inXSpan) return PortSide::Bottom;
    // Sticky: before crossing the side's boundary plane, stay Left / Right.
    if (last.x() < rect.left()) return PortSide::Left;
    if (last.x() > rect.right()) return PortSide::Right;
    if (travel.y() > 0.0 && prev.y() <= rect.top()) return PortSide::Top;
    if (travel.y() < 0.0 && prev.y() >= rect.bottom()) return PortSide::Bottom;
    return std::nullopt;
}

// The source-end twin: the stub reaches the first bendpoint perpendicular to the
// course's first segment when the bendpoint's aligned coordinate fits that side's
// span; a course running beside the node departs from the flank it is on.
std::optional<PortSide> sideFromDeparture(const QRectF& rect, QPointF first, QPointF second) {
    const QPointF travel = second - first;
    if (std::abs(travel.x()) < 0.01 && std::abs(travel.y()) < 0.01) {
        return std::nullopt;
    }
    if (std::abs(travel.x()) >= std::abs(travel.y())) {
        // First authored segment runs horizontally.
        const bool inYSpan =
            first.y() >= rect.top() + kSideCornerInset && first.y() <= rect.bottom() - kSideCornerInset;
        if (travel.x() < 0.0 && rect.left() >= first.x() && inYSpan) return PortSide::Left;
        if (travel.x() > 0.0 && rect.right() <= first.x() && inYSpan) return PortSide::Right;

        // Near the node's lateral bounds (within keepout), depart perpendicularly
        // from Top / Bottom rather than cling to a Left / Right corner.
        const bool nearLeft = first.x() >= rect.left() - kApproachKeepout && first.x() <= rect.right();
        const bool nearRight = first.x() <= rect.right() + kApproachKeepout && first.x() >= rect.left();
        if (nearLeft || nearRight) {
            if (first.y() < rect.top()) return PortSide::Top;
            if (first.y() > rect.bottom()) return PortSide::Bottom;
        }

        // Far beside the node.
        if (first.x() <= rect.left()) return PortSide::Left;
        if (first.x() >= rect.right()) return PortSide::Right;
        if (first.y() < rect.top()) return PortSide::Top;
        if (first.y() > rect.bottom()) return PortSide::Bottom;
        return std::nullopt;
    }
    // First authored segment runs vertically.
    const bool inXSpan =
        first.x() >= rect.left() + kSideCornerInset && first.x() <= rect.right() - kSideCornerInset;
    if (travel.y() < 0.0 && rect.top() >= first.y() && inXSpan) return PortSide::Top;
    if (travel.y() > 0.0 && rect.bottom() <= first.y() && inXSpan) return PortSide::Bottom;

    const bool nearTop = first.y() >= rect.top() - kApproachKeepout && first.y() <= rect.bottom();
    const bool nearBottom = first.y() <= rect.bottom() + kApproachKeepout && first.y() >= rect.top();
    if (nearTop || nearBottom) {
        if (first.x() < rect.left()) return PortSide::Left;
        if (first.x() > rect.right()) return PortSide::Right;
    }

    if (first.y() <= rect.top()) return PortSide::Top;
    if (first.y() >= rect.bottom()) return PortSide::Bottom;
    if (first.x() < rect.left()) return PortSide::Left;
    if (first.x() > rect.right()) return PortSide::Right;
    return std::nullopt;
}

// The anchor on `side` of `rect` aligned with `p`'s perpendicular coordinate
// (clamped 12px inside the corners), so the stub toward the first/last bendpoint
// leaves straight when geometry allows.
QPointF sideAnchorToward(const QRectF& rect, PortSide side, QPointF p) {
    switch (side) {
        case PortSide::Left:
            return QPointF(rect.left(),
                           std::clamp(p.y(), rect.top() + kSideCornerInset, rect.bottom() - kSideCornerInset));
        case PortSide::Right:
            return QPointF(rect.right(),
                           std::clamp(p.y(), rect.top() + kSideCornerInset, rect.bottom() - kSideCornerInset));
        case PortSide::Top:
            return QPointF(std::clamp(p.x(), rect.left() + kSideCornerInset, rect.right() - kSideCornerInset),
                           rect.top());
        case PortSide::Bottom:
            return QPointF(std::clamp(p.x(), rect.left() + kSideCornerInset, rect.right() - kSideCornerInset),
                           rect.bottom());
    }
    return rect.center();
}

}  // namespace

RoutedEdge routeManualEdge(const QRectF& sourceRect, const QRectF& targetRect,
                           const QVector<QPointF>& bendpoints,
                           qreal filletRadius,
                           QPointF labelOffset,
                           QSizeF pillSize) {
    // Each node's anchor derives from the bendpoint nearest that end, never from
    // the pill's position, which may sit far off the course.
    const QPointF firstRef = bendpoints.isEmpty() ? targetRect.center() : bendpoints.front();
    const QPointF lastRef = bendpoints.isEmpty() ? sourceRect.center() : bendpoints.back();
    PortSide targetSide = sideFacing(targetRect, lastRef);
    if (bendpoints.size() >= 2) {
        if (const auto side =
                sideFromArrival(targetRect, bendpoints[bendpoints.size() - 2], bendpoints.back())) {
            targetSide = *side;
        }
    }

    // On a glide-class attach (the target side's normal runs along the final run)
    // the run must lie on the anchor's line to feed the arrow. Snap the final
    // run's perpendicular coordinate to the clamped anchor; overshoot past the
    // port self-clips in collapseCollinear. Without this the bridge would put a
    // corner inside the node when the stored run crosses over it.
    QVector<QPointF> course = bendpoints;
    if (course.size() >= 2) {
        const QPointF runTravel = course.back() - course[course.size() - 2];
        const bool runHoriz = std::abs(runTravel.x()) >= std::abs(runTravel.y());
        const bool sideNormalHoriz = targetSide == PortSide::Left || targetSide == PortSide::Right;
        if (runHoriz && sideNormalHoriz) {
            const qreal anchorY = std::clamp(course.back().y(), targetRect.top() + kSideCornerInset,
                                             targetRect.bottom() - kSideCornerInset);
            if (std::abs(course.back().y() - anchorY) > 0.01) {
                course.back().setY(anchorY);
                course[course.size() - 2].setY(anchorY);
            }
        } else if (!runHoriz && !sideNormalHoriz) {
            const qreal anchorX = std::clamp(course.back().x(), targetRect.left() + kSideCornerInset,
                                             targetRect.right() - kSideCornerInset);
            if (std::abs(course.back().x() - anchorX) > 0.01) {
                course.back().setX(anchorX);
                course[course.size() - 2].setX(anchorX);
            }
        } else if (runHoriz && !sideNormalHoriz) {
            // Perpendicular attach: hold the horizontal run at minimum approach
            // depth when it falls inside the keepout band.
            if (targetSide == PortSide::Top) {
                const qreal clampY = targetRect.top() - kApproachKeepout;
                if (course.back().y() > clampY) {
                    course.back().setY(clampY);
                    course[course.size() - 2].setY(clampY);
                }
            } else if (targetSide == PortSide::Bottom) {
                const qreal clampY = targetRect.bottom() + kApproachKeepout;
                if (course.back().y() < clampY) {
                    course.back().setY(clampY);
                    course[course.size() - 2].setY(clampY);
                }
            }
        } else if (!runHoriz && sideNormalHoriz) {
            // As above for a vertical run.
            if (targetSide == PortSide::Left) {
                const qreal clampX = targetRect.left() - kApproachKeepout;
                if (course.back().x() > clampX) {
                    course.back().setX(clampX);
                    course[course.size() - 2].setX(clampX);
                }
            } else if (targetSide == PortSide::Right) {
                const qreal clampX = targetRect.right() + kApproachKeepout;
                if (course.back().x() < clampX) {
                    course.back().setX(clampX);
                    course[course.size() - 2].setX(clampX);
                }
            }
        }
    }

    // The source side derives from the snapped course: on a 2-bendpoint course the
    // snap above rewrites course.front(), so a side picked before it would aim the
    // stub at a line that has moved.
    const QPointF sourceRef = course.isEmpty() ? firstRef : course.front();
    PortSide sourceSide = sideFacing(sourceRect, sourceRef);
    if (course.size() >= 2) {
        if (const auto side = sideFromDeparture(sourceRect, course.front(), course[1])) {
            sourceSide = *side;
        }
    }

    // Source-end clamp, symmetrical to the target end.
    if (course.size() >= 2) {
        const QPointF firstRunTravel = course[1] - course.front();
        const bool runHoriz = std::abs(firstRunTravel.x()) >= std::abs(firstRunTravel.y());
        const bool sideNormalHoriz = sourceSide == PortSide::Left || sourceSide == PortSide::Right;
        if (runHoriz && !sideNormalHoriz) {
            // Perpendicular departure: hold the horizontal run at minimum depth.
            if (sourceSide == PortSide::Top) {
                const qreal clampY = sourceRect.top() - kApproachKeepout;
                if (course.front().y() > clampY) {
                    course.front().setY(clampY);
                    course[1].setY(clampY);
                }
            } else if (sourceSide == PortSide::Bottom) {
                const qreal clampY = sourceRect.bottom() + kApproachKeepout;
                if (course.front().y() < clampY) {
                    course.front().setY(clampY);
                    course[1].setY(clampY);
                }
            }
        } else if (!runHoriz && sideNormalHoriz) {
            // As above for a vertical run.
            if (sourceSide == PortSide::Left) {
                const qreal clampX = sourceRect.left() - kApproachKeepout;
                if (course.front().x() > clampX) {
                    course.front().setX(clampX);
                    course[1].setX(clampX);
                }
            } else if (sourceSide == PortSide::Right) {
                const qreal clampX = sourceRect.right() + kApproachKeepout;
                if (course.front().x() < clampX) {
                    course.front().setX(clampX);
                    course[1].setX(clampX);
                }
            }
        }
    }

    const QPointF sourceAnchor = sideAnchorToward(sourceRect, sourceSide, course.isEmpty() ? firstRef : course.front());
    const QPointF targetAnchor = sideAnchorToward(targetRect, targetSide, course.isEmpty() ? lastRef : course.back());
    return routeEdgeWithBendpoints(sourceAnchor, sourceSide, targetAnchor, targetSide, course,
                                   filletRadius, labelOffset, pillSize);
}

bool manualRouteWellFormed(const RoutedEdge& routed, const QRectF& sourceRect, const QRectF& targetRect) {
    // 1) The skeleton may not pass through either endpoint node's body. Only a
    //    degenerate attach (stale bendpoints after a node move) crosses the
    //    interior. Segments are axis-aligned, so bbox overlap is intersection.
    const auto crossesInterior = [](const QVector<QPointF>& pts, const QRectF& rect) {
        const QRectF inner = rect.adjusted(1.0, 1.0, -1.0, -1.0);
        if (!inner.isValid()) {
            return false;
        }
        for (int i = 0; i < pts.size() - 1; ++i) {
            const qreal minX = std::min(pts[i].x(), pts[i + 1].x());
            const qreal maxX = std::max(pts[i].x(), pts[i + 1].x());
            const qreal minY = std::min(pts[i].y(), pts[i + 1].y());
            const qreal maxY = std::max(pts[i].y(), pts[i + 1].y());
            if (minX < inner.right() && maxX > inner.left() && minY < inner.bottom() && maxY > inner.top()) {
                return true;
            }
        }
        return false;
    };
    if (crossesInterior(routed.waypoints, sourceRect) || crossesInterior(routed.waypoints, targetRect)) {
        return false;
    }

    // 2) The approach must feed the arrow: the target half's last real chord must
    //    run parallel to the arrow direction.
    if (routed.arrowhead.size() >= 3 && routed.targetHalf.elementCount() >= 2) {
        const QPointF apex = routed.arrowhead.at(0);
        const QPointF baseCenter = (routed.arrowhead.at(1) + routed.arrowhead.at(2)) * 0.5;
        QPointF arrowDir = apex - baseCenter;
        const qreal arrowLen = std::sqrt(QPointF::dotProduct(arrowDir, arrowDir));
        if (arrowLen > 0.01) {
            arrowDir /= arrowLen;
            for (int i = routed.targetHalf.elementCount() - 1; i >= 1; --i) {
                const auto e1 = routed.targetHalf.elementAt(i - 1);
                const auto e2 = routed.targetHalf.elementAt(i);
                QPointF chord(e2.x - e1.x, e2.y - e1.y);
                const qreal chordLen = std::sqrt(QPointF::dotProduct(chord, chord));
                if (chordLen < 4.0) {
                    continue;
                }
                if (QPointF::dotProduct(chord / chordLen, arrowDir) < 0.9) {
                    return false;
                }
                break;
            }
        }
    }
    return true;
}

QPointF sourceHalfStart(QPointF anchor, PortSide side) { return anchor + outwardNormal(side) * kStubGap; }

QPointF targetHalfStop(QPointF anchor, PortSide side) {
    return anchor + outwardNormal(side) * (kStubGap + kArrowApproach);
}

QVector<QPointF> orthogonalPortRun(QPointF from, QPointF fromDir, QPointF to, QPointF toDir, qreal minRun) {
    const bool fromHorizontal = std::abs(fromDir.x()) > 0.5;
    const bool toHorizontal = std::abs(toDir.x()) > 0.5;
    // Guaranteed straight runs: `out` off the departure port, `in` before the arrival port.
    const QPointF out = from + fromDir * minRun;
    const QPointF in = to - toDir * minRun;
    const auto ahead = [](QPointF a, QPointF b, QPointF dir) { return QPointF::dotProduct(b - a, dir) > 0.01; };
    const auto perpOf = [](QPointF p, bool horizontal) { return horizontal ? p.y() : p.x(); };

    QVector<QPointF> result;
    const char* branchName = "Unknown";

    if (fromHorizontal != toHorizontal) {
        // Perpendicular directions: the L's single corner is where the two axes
        // meet, valid only when it lies ahead of both end directions.
        const QPointF corner = fromHorizontal ? QPointF(to.x(), from.y()) : QPointF(from.x(), to.y());
        if (ahead(from, corner, fromDir) && ahead(corner, to, toDir)) {
            branchName = "L-shape";
            result = dedupAdjacent(QVector<QPointF>{from, corner, to});
        }
    } else if (QPointF::dotProduct(fromDir, toDir) > 0.0) {
        // Same direction: straight when the ends share the perpendicular
        // coordinate, else a two-corner Z.
        if (std::abs(perpOf(from, fromHorizontal) - perpOf(to, fromHorizontal)) < 0.01 && ahead(from, to, fromDir)) {
            branchName = "SameDir-Straight";
            result = QVector<QPointF>{from, to};
        } else if (ahead(from, to, fromDir)) {
            branchName = "SameDir-Z-Channel";
            const qreal mid = fromHorizontal ? (from.x() + to.x()) / 2.0 : (from.y() + to.y()) / 2.0;
            const QPointF first = fromHorizontal ? QPointF(mid, from.y()) : QPointF(from.x(), mid);
            const QPointF second = fromHorizontal ? QPointF(mid, to.y()) : QPointF(to.x(), mid);
            result = dedupAdjacent(QVector<QPointF>{from, first, second, to});
        }
    } else if (std::abs(perpOf(from, fromHorizontal) - perpOf(to, fromHorizontal)) > 0.01) {
        // Opposite directions: one channel beyond both ends along the departure
        // direction, so each end still leaves/arrives its own way (a two-corner Z).
        branchName = "OppositeDir-Channel-Z";
        const qreal alongOut = fromHorizontal ? out.x() : out.y();
        const qreal alongIn = fromHorizontal ? in.x() : in.y();
        const bool positive = (fromHorizontal ? fromDir.x() : fromDir.y()) > 0.0;
        const qreal channel = positive ? std::max(alongOut, alongIn) : std::min(alongOut, alongIn);
        const QPointF first = fromHorizontal ? QPointF(channel, from.y()) : QPointF(from.x(), channel);
        const QPointF second = fromHorizontal ? QPointF(channel, to.y()) : QPointF(to.x(), channel);
        result = dedupAdjacent(QVector<QPointF>{from, first, second, to});
    }

    // Fallback for geometries the forms above reject (a counterpart behind its
    // port, or collinear ports facing the same way): out along fromDir, across a
    // perpendicular channel, in along toDir. Never violates either end direction.
    if (result.isEmpty()) {
        if (fromHorizontal != toHorizontal) {
            branchName = "Fallback-Wrap-Perpendicular-5pt";
            const QPointF cross = fromHorizontal ? QPointF(out.x(), in.y()) : QPointF(in.x(), out.y());
            result = dedupAdjacent(QVector<QPointF>{from, out, cross, in, to});
        } else {
            branchName = "Fallback-Wrap-Collinear-6pt";
            const qreal fromPerp = perpOf(from, fromHorizontal);
            const qreal toPerp = perpOf(to, fromHorizontal);
            const qreal midPerp = (fromPerp + toPerp) / 2.0;
            // Collinear ends need a jog, or the "channel" would fold onto the run.
            const qreal cross = std::abs(midPerp - fromPerp) < 0.01 ? fromPerp + minRun : midPerp;
            const QPointF first = fromHorizontal ? QPointF(out.x(), cross) : QPointF(cross, out.y());
            const QPointF second = fromHorizontal ? QPointF(in.x(), cross) : QPointF(cross, in.y());
            result = dedupAdjacent(QVector<QPointF>{from, out, first, second, in, to});
        }
    }

    logPortRun(QStringLiteral("orthogonalPortRun"), from, fromDir, to, toDir, QLatin1String(branchName), result);
    return result;
}

RoutedEdge routeThroughPorts(const PortAssignment& ports, qreal filletRadius) {
    RoutedEdge routed;
    // The pill's body is the gap, so its center is the label anchor.
    routed.labelBase = ports.pillCenter;
    routed.labelAnchor = ports.pillCenter;
    logRoutingTrace(
        QStringLiteral("ROUTE_THROUGH_PORTS"),
        QStringLiteral("entrySide=%1 exitSide=%2 sAnchorSide=%3 tAnchorSide=%4 pillCenter=(%5, %6)")
            .arg(static_cast<int>(ports.entrySide))
            .arg(static_cast<int>(ports.exitSide))
            .arg(static_cast<int>(ports.sourceAnchorSide))
            .arg(static_cast<int>(ports.targetAnchorSide))
            .arg(ports.pillCenter.x(), 0, 'f', 1)
            .arg(ports.pillCenter.y(), 0, 'f', 1));
    if (ports.hasEntry) {
        routed.sourceWaypoints = orthogonalPortRun(sourceHalfStart(ports.sourceAnchor, ports.sourceAnchorSide),
                                                  outwardNormal(ports.sourceAnchorSide), ports.entryPoint,
                                                  -outwardNormal(ports.entrySide), kMinEndRun);
        routed.sourceHalf = buildStyledPath(routed.sourceWaypoints, filletRadius);
    }
    if (ports.hasExit) {
        // The approach tangent is the target side's inward normal.
        const QPointF approach = -outwardNormal(ports.targetAnchorSide);
        const QPointF stop = ports.arrowAtTarget
                                  ? targetHalfStop(ports.targetAnchor, ports.targetAnchorSide)
                                  : ports.targetAnchor + outwardNormal(ports.targetAnchorSide) * kStubGap;
        routed.targetWaypoints = orthogonalPortRun(ports.exitPoint, outwardNormal(ports.exitSide), stop,
                                                   approach, kMinEndRun);
        routed.targetHalf = buildStyledPath(routed.targetWaypoints, filletRadius);
        if (ports.arrowAtTarget) {
            routed.arrowhead = buildArrowhead(stop, approach, kArrowLength, kArrowHalfWidth);
        }
    }
    routed.waypoints = routed.sourceWaypoints;
    if (ports.hasEntry && ports.hasExit) {
        // If entry and exit are on perpendicular pill sides, route through
        // pillCenter so no segment jumps diagonally across the pill.
        const bool entryExitOrthogonal = std::abs(ports.entryPoint.x() - ports.exitPoint.x()) < 0.01 ||
                                         std::abs(ports.entryPoint.y() - ports.exitPoint.y()) < 0.01;
        if (!entryExitOrthogonal) {
            routed.waypoints.push_back(ports.pillCenter);
        }
    }
    for (const QPointF& pt : routed.targetWaypoints) {
        routed.waypoints.push_back(pt);
    }
    return routed;
}

RoutedEdge routeSelfLoop(const QRectF& nodeRect, int slot, qreal filletRadius, QSizeF pillSize) {
    // Rise from the measured pill height: the pill's bottom clears the node top by >= 10px.
    const qreal baseRise = pillSize.height() > 0.0
                               ? std::max(26.0, (pillSize.height() / 2.0) + 10.0)
                               : 26.0;
    const qreal rise = baseRise + 14.0 * static_cast<qreal>(slot);

    // Lateral extent from the measured pill width: a pill wider than the node widens
    // the loop rightward so the pill's left half doesn't hang past the node's left side.
    qreal baseExtent = 26.0;
    if (pillSize.width() > 0.0) {
        const qreal exitX = nodeRect.center().x() + nodeRect.width() * 0.25;
        const qreal neededExtent = (2.0 * nodeRect.left() + pillSize.width() - exitX - nodeRect.right()) + 16.0;
        if (neededExtent > baseExtent) {
            baseExtent = neededExtent;
        }
    }
    const qreal extent = baseExtent + 14.0 * static_cast<qreal>(slot);

    // Out of the Top side right of center, back into the Right side above center.
    const QPointF exitAnchor = sidePortAnchor(nodeRect, PortSide::Top, nodeRect.width() * 0.25);
    const QPointF entryAnchor = sidePortAnchor(nodeRect, PortSide::Right, -nodeRect.height() * 0.25);

    const QPointF exitStub = exitAnchor + outwardNormal(PortSide::Top) * kStubGap;
    const QPointF lineStop = entryAnchor + outwardNormal(PortSide::Right) * (kStubGap + kArrowApproach);
    const qreal topY = nodeRect.top() - rise;
    const qreal loopX = nodeRect.right() + extent;

    const QVector<QPointF> waypoints = dedupAdjacent(QVector<QPointF>{
        exitStub,
        QPointF(exitStub.x(), topY),
        QPointF(loopX, topY),
        QPointF(loopX, lineStop.y()),
        lineStop,
    });
    const QVector<qreal> cumulative = cumulativeLengths(waypoints);

    // The label pins to the middle of the top run (waypoint 1 -> 2), the loop's
    // only long node-free stretch, so the pill never sits on a fillet.
    const qreal labelLen = cumulative[1] + (cumulative[2] - cumulative[1]) / 2.0;
    const qreal gapHalf = std::min(kLabelGapHalf, (cumulative[2] - cumulative[1]) / 2.0);

    RoutedEdge routed;
    routed.labelAnchor = pointAtLength(waypoints, cumulative, labelLen, nullptr);
    routed.labelBase = routed.labelAnchor;
    routed.sourceHalf = buildStyledPath(slicePolyline(waypoints, cumulative, 0.0, labelLen - gapHalf), filletRadius);
    routed.targetHalf =
        buildStyledPath(slicePolyline(waypoints, cumulative, labelLen + gapHalf, cumulative.back()), filletRadius);
    // The final segment travels left (-x) into the Right side's lineStop.
    routed.arrowhead = buildArrowhead(lineStop, -outwardNormal(PortSide::Right), kArrowLength, kArrowHalfWidth);
    return routed;
}

RoutedEdge routeRootStub(QPointF frameEdgePoint, bool arrowIntoFrame, PortSide side) {
    const QPointF out = outwardNormal(side);    RoutedEdge routed;
    routed.labelAnchor = frameEdgePoint + out * 52.0;
    routed.labelBase = routed.labelAnchor;
    const QPointF runStart = frameEdgePoint + out * 34.0;  // just off the pill's body
    QPainterPath path(runStart);
    if (arrowIntoFrame) {
        const QPointF lineStop = frameEdgePoint + out * (kStubGap + kArrowApproach);
        path.lineTo(lineStop);
        routed.arrowhead = buildArrowhead(lineStop, -out, kArrowLength, kArrowHalfWidth);
    } else {
        path.lineTo(frameEdgePoint + out * kStubGap);
    }
    routed.sourceHalf = path;
    return routed;
}

RoutedEdge routeTargetlessStub(const QRectF& nodeRect, int slot) {
    const qreal offset = 22.0 * static_cast<qreal>(slot);
    const QPointF anchor = sidePortAnchor(nodeRect, PortSide::Bottom, offset);
    const QPointF down = outwardNormal(PortSide::Bottom);
    const QPointF stubStart = anchor + down * kStubGap;
    const QPointF stubEnd = stubStart + down * 22.0;

    RoutedEdge routed;
    QPainterPath stub(stubStart);
    stub.lineTo(stubEnd);
    routed.sourceHalf = stub;
    // No target half or arrowhead; the pill (just past the stub's end) terminates the edge.
    routed.labelAnchor = stubEnd + down * 12.0;
    routed.labelBase = routed.labelAnchor;
    return routed;
}

QPainterPath buildWireDragPreviewPath(QPointF from, PortSide fromSide, QPointF to) {
    if (distance(from, to) < 0.01) {
        return QPainterPath(from);
    }
    // midAxisWaypoints already dedupes adjacent sub-0.01 points.
    return buildFilletedPath(midAxisWaypoints(from, fromSide, to), 10.0);
}

TargetBranch routeTargetBranch(const QRectF& pillRect, const QRectF& targetRect, quint64 targetId,
                               qreal filletRadius) {
    TargetBranch branch;
    branch.targetId = targetId;

    const PortSide targetSide = preferredPillSide(targetRect, pillRect.center());
    const QPointF targetAnchor = counterpartAnchor(targetRect, pillRect.center());
    const PortSide exitSide = preferredPillSide(pillRect, targetAnchor);
    const QPointF exitPoint = sidePortAnchor(pillRect, exitSide);

    const QPointF approach = -outwardNormal(targetSide);
    const QPointF stop = targetHalfStop(targetAnchor, targetSide);

    branch.path = buildStyledPath(
        orthogonalPortRun(exitPoint, outwardNormal(exitSide), stop, approach, kMinEndRun), filletRadius);
    branch.arrowhead = buildArrowhead(stop, approach, kArrowLength, kArrowHalfWidth);
    return branch;
}

}  // namespace app
