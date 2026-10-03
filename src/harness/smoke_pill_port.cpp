#include <QPointF>
#include <QRectF>
#include <cstdio>

#include "view/geometry/pill_port_resolver.h"
#include "harness/harness.h"

static QRectF probePillRect(QPointF center) { return QRectF(center.x() - 30.0, center.y() - 9.0, 60.0, 18.0); }
// PillPortResolver rule table, asserted 1:1: one table row, one fixture
// geometry, one assertion. The row-count check fails when a rule is added
// without its fixture. QApplication-free; the resolver is pure geometry.
int runPillPortSmoke() {
    using app::EdgeClass;
    using app::HalfShape;
    using app::PortSide;

    struct Fixture {
        const char* ruleId;
        EdgeClass cls;
        QRectF sourceRect;
        QRectF targetRect;
        QPointF pillCenter;
        bool hasEntry;
        bool hasExit;
        PortSide entrySide;
        PortSide exitSide;
        std::optional<HalfShape> sourceShape;
        std::optional<HalfShape> targetShape;
        const char* what;
    };

    // One node box (160x60) and one pill (60x18) throughout, so the only
    // thing a row's fixture varies is the LAYOUT the rule reads.
    const Fixture fixtures[] = {
        // Two distinct facing preferences: a vertically aligned edge passes
        // straight through the pill -- in at the top middle, out at the
        // bottom middle.
        {"normal-facing", EdgeClass::Normal, QRectF(320, 100, 160, 60), QRectF(320, 500, 160, 60), QPointF(400, 300),
         true, true, PortSide::Top, PortSide::Bottom, HalfShape::Straight, HalfShape::Straight,
         "normal-facing (vertical straight pass-through)"},
        // Both nodes above the pill, source to the right: target keeps top,
        // source yields right.
        {"normal-yield-top-source-right", EdgeClass::Normal, QRectF(500, 100, 160, 60), QRectF(320, 100, 160, 60),
         QPointF(400, 400), true, true, PortSide::Right, PortSide::Top, std::nullopt, std::nullopt,
         "normal-yield-top-source-right (target keeps top)"},
        // The same layout with the source node moved ACROSS the pill: the
        // source line flips right -> left (the crossing flip).
        {"normal-yield-top-source-left", EdgeClass::Normal, QRectF(140, 100, 160, 60), QRectF(320, 100, 160, 60),
         QPointF(400, 400), true, true, PortSide::Left, PortSide::Top, std::nullopt, std::nullopt,
         "normal-yield-top-source-left (the crossing flip)"},
        {"normal-yield-bottom-source-right", EdgeClass::Normal, QRectF(500, 700, 160, 60), QRectF(320, 700, 160, 60),
         QPointF(400, 400), true, true, PortSide::Right, PortSide::Bottom, std::nullopt, std::nullopt,
         "normal-yield-bottom-source-right"},
        {"normal-yield-bottom-source-left", EdgeClass::Normal, QRectF(140, 700, 160, 60), QRectF(320, 700, 160, 60),
         QPointF(400, 400), true, true, PortSide::Left, PortSide::Bottom, std::nullopt, std::nullopt,
         "normal-yield-bottom-source-left"},
        {"normal-yield-left-source-below", EdgeClass::Normal, QRectF(100, 500, 160, 60), QRectF(100, 320, 160, 60),
         QPointF(400, 400), true, true, PortSide::Bottom, PortSide::Left, std::nullopt, std::nullopt,
         "normal-yield-left-source-below"},
        {"normal-yield-left-source-above", EdgeClass::Normal, QRectF(100, 240, 160, 60), QRectF(100, 320, 160, 60),
         QPointF(400, 400), true, true, PortSide::Top, PortSide::Left, std::nullopt, std::nullopt,
         "normal-yield-left-source-above"},
        {"normal-yield-right-source-below", EdgeClass::Normal, QRectF(540, 500, 160, 60), QRectF(540, 320, 160, 60),
         QPointF(400, 400), true, true, PortSide::Bottom, PortSide::Right, std::nullopt, std::nullopt,
         "normal-yield-right-source-below"},
        {"normal-yield-right-source-above", EdgeClass::Normal, QRectF(540, 240, 160, 60), QRectF(540, 320, 160, 60),
         QPointF(400, 400), true, true, PortSide::Top, PortSide::Right, std::nullopt, std::nullopt,
         "normal-yield-right-source-above"},
        // The self pair shares the node-facing side as two parallel runs.
        {"self-straddle", EdgeClass::Self, QRectF(320, 300, 160, 60), QRectF(320, 300, 160, 60), QPointF(700, 330),
         true, true, PortSide::Left, PortSide::Left, HalfShape::Straight, HalfShape::Straight,
         "self-straddle (parallel out-and-back pair)"},
        // The pill terminates the edge, and a laterally offset node bends
        // the stub into an elbow.
        {"targetless-entry-only", EdgeClass::Targetless, QRectF(500, 300, 160, 60), QRectF(), QPointF(450, 250), true,
         false, PortSide::Bottom, PortSide::Bottom, HalfShape::L, std::nullopt, "targetless-entry-only (bent stub)"},
        // The frame edge point as a degenerate counterpart rect.
        {"root-frame-exit", EdgeClass::Root, QRectF(), QRectF(QPointF(900, 200), QSizeF(0, 0)), QPointF(952, 200),
         false, true, PortSide::Left, PortSide::Left, std::nullopt, HalfShape::Straight,
         "root-frame-exit (frame-right anchoring)"},
        // Diagonal quadrant relationship forms a single 90-degree 1-bend L-shape
        {"normal-min-bend-l", EdgeClass::Normal, QRectF(320, 360, 120, 60), QRectF(80, 200, 120, 60),
         QPointF(227.5, 390), true, true, PortSide::Right, PortSide::Left, HalfShape::Straight, HalfShape::L,
         "normal-min-bend-l (diagonal loopback 1-bend L-shape)"},
    };

    int ruleCount = 0;
    const app::PillPortRule* rules = app::pillPortRuleTable(&ruleCount);
    const int fixtureCount = static_cast<int>(sizeof(fixtures) / sizeof(fixtures[0]));
    if (ruleCount != fixtureCount) {
        std::fprintf(stderr, "FAIL: the rule table has %d row(s) but phase 8 has %d fixture(s) -- 1:1 broken\n",
                     ruleCount, fixtureCount);
        return 1;
    }
    for (int i = 0; i < ruleCount; ++i) {
        int matches = 0;
        for (const Fixture& fixture : fixtures) {
            matches += std::strcmp(fixture.ruleId, rules[i].id) == 0 ? 1 : 0;
        }
        if (matches != 1) {
            std::fprintf(stderr, "FAIL: rule row '%s' has %d fixture(s), want exactly 1\n", rules[i].id, matches);
            return 1;
        }
    }

    for (const Fixture& fixture : fixtures) {
        const QRectF pillRect = probePillRect(fixture.pillCenter);
        const app::PortAssignment ports =
            app::resolvePillPorts(fixture.cls, fixture.sourceRect, fixture.targetRect, pillRect, std::nullopt);
        if (std::strcmp(ports.ruleId, fixture.ruleId) != 0) {
            std::fprintf(stderr, "FAIL: %s resolved through row '%s'\n", fixture.what, ports.ruleId);
            return 1;
        }
        if (ports.hasEntry != fixture.hasEntry || ports.hasExit != fixture.hasExit) {
            std::fprintf(stderr, "FAIL: %s has the wrong halves (entry=%d exit=%d)\n", fixture.what,
                         static_cast<int>(ports.hasEntry), static_cast<int>(ports.hasExit));
            return 1;
        }
        if ((fixture.hasEntry && ports.entrySide != fixture.entrySide) ||
            (fixture.hasExit && ports.exitSide != fixture.exitSide)) {
            std::fprintf(stderr, "FAIL: %s picked sides entry=%d exit=%d\n", fixture.what,
                         static_cast<int>(ports.entrySide), static_cast<int>(ports.exitSide));
            return 1;
        }
        if ((fixture.sourceShape.has_value() && ports.sourceShape != *fixture.sourceShape) ||
            (fixture.targetShape.has_value() && ports.targetShape != *fixture.targetShape)) {
            std::fprintf(stderr, "FAIL: %s produced the wrong half shape(s)\n", fixture.what);
            return 1;
        }
        if (!portAssignmentHoldsInvariant(ports, pillRect, fixture.what)) {
            return 1;
        }
    }

    // ---- the straight pass-through, numerically ------------------------------
    {
        const QRectF pillRect = probePillRect(QPointF(400, 300));
        const app::PortAssignment ports = app::resolvePillPorts(
            EdgeClass::Normal, QRectF(320, 100, 160, 60), QRectF(320, 500, 160, 60), pillRect, std::nullopt);
        if (ports.entryPoint != QPointF(400, 291) || ports.exitPoint != QPointF(400, 309) ||
            ports.sourceAnchor != QPointF(400, 160) || ports.targetAnchor != QPointF(400, 500)) {
            std::fprintf(stderr, "FAIL: the aligned edge does not pass straight through the pill's middles\n");
            return 1;
        }
    }

    // ---- the self pair straddles ONE side's middle ---------------------------
    {
        const QRectF pillRect = probePillRect(QPointF(700, 330));
        const app::PortAssignment ports = app::resolvePillPorts(EdgeClass::Self, QRectF(320, 300, 160, 60),
                                                                 QRectF(320, 300, 160, 60), pillRect, std::nullopt);
        const QPointF middle = app::sidePortAnchor(pillRect, ports.entrySide);
        const QPointF pairMid = (ports.entryPoint + ports.exitPoint) / 2.0;
        const qreal spacing = std::hypot(ports.exitPoint.x() - ports.entryPoint.x(),
                                          ports.exitPoint.y() - ports.entryPoint.y());
        const qreal anchorSpacing = std::hypot(ports.targetAnchor.x() - ports.sourceAnchor.x(),
                                                ports.targetAnchor.y() - ports.sourceAnchor.y());
        if (ports.entrySide != ports.exitSide || pairMid != middle ||
            std::abs(spacing - 2.0 * app::kSelfStraddle) > 0.01 ||
            std::abs(anchorSpacing - 2.0 * app::kSelfStraddle) > 0.01) {
            std::fprintf(stderr, "FAIL: the self pair does not straddle its side's middle as two parallel runs\n");
            return 1;
        }
    }

    // ---- hysteresis -----------------------------------------------------------
    // Both nodes above the pill, so the source half is the yielded one; the
    // source node's rect places its counterpart anchor a known distance
    // across the pill's centre line, which is the flip discriminant.
    {
        const QRectF pillRect = probePillRect(QPointF(400, 400));
        const QRectF targetRect(320, 100, 160, 60);
        const QRectF sourceRight(500, 100, 160, 60);  // counterpart anchor x = 512 (right of the pill)
        const QRectF insideBand(242, 100, 160, 60);   // counterpart anchor x = 390: 20px of discriminant
        const QRectF beyondBand(232, 100, 160, 60);   // counterpart anchor x = 380: 40px, past kFlipHysteresis
        const app::PortAssignment incumbent =
            app::resolvePillPorts(EdgeClass::Normal, sourceRight, targetRect, pillRect, std::nullopt);
        if (incumbent.entrySide != PortSide::Right) {
            std::fprintf(stderr, "FAIL: the hysteresis incumbent did not start on the right\n");
            return 1;
        }
        const app::PortAssignment held =
            app::resolvePillPorts(EdgeClass::Normal, insideBand, targetRect, pillRect, incumbent);
        if (held.entrySide != PortSide::Right) {
            std::fprintf(stderr, "FAIL: a crossing inside the hysteresis band flipped the side anyway\n");
            return 1;
        }
        const app::PortAssignment flipped =
            app::resolvePillPorts(EdgeClass::Normal, beyondBand, targetRect, pillRect, incumbent);
        if (flipped.entrySide != PortSide::Left) {
            std::fprintf(stderr, "FAIL: a crossing past the hysteresis band did not flip the side\n");
            return 1;
        }
        const app::PortAssignment free =
            app::resolvePillPorts(EdgeClass::Normal, insideBand, targetRect, pillRect, std::nullopt);
        if (free.entrySide != PortSide::Left) {
            std::fprintf(stderr, "FAIL: a resolve with no incumbent applied hysteresis anyway\n");
            return 1;
        }
        if (!portAssignmentHoldsInvariant(held, pillRect, "the held assignment")) {
            return 1;  // hysteresis may never break the distinct-sides invariant
        }
    }

    // ---- cross-hierarchy geometry ---------------------------------------------
    // The resolver takes rects, never a hierarchy, so a parent/child or
    // inside/outside pair is just two more rects. The pill sits outside the
    // container with both counterpart anchors facing it from roughly the same
    // side, so (a)/(b) exercise the same-side yield path.
    {
        const auto onBorder = [](const QRectF& rect, PortSide side, QPointF p) {
            switch (side) {
                case PortSide::Top:
                    return std::abs(p.y() - rect.top()) < 0.01 && p.x() >= rect.left() - 0.01 &&
                           p.x() <= rect.right() + 0.01;
                case PortSide::Bottom:
                    return std::abs(p.y() - rect.bottom()) < 0.01 && p.x() >= rect.left() - 0.01 &&
                           p.x() <= rect.right() + 0.01;
                case PortSide::Left:
                    return std::abs(p.x() - rect.left()) < 0.01 && p.y() >= rect.top() - 0.01 &&
                           p.y() <= rect.bottom() + 0.01;
                case PortSide::Right:
                    return std::abs(p.x() - rect.right()) < 0.01 && p.y() >= rect.top() - 0.01 &&
                           p.y() <= rect.bottom() + 0.01;
            }
            return false;
        };
        const auto finite = [](QPointF p) { return std::isfinite(p.x()) && std::isfinite(p.y()); };
        const auto checkCrossHierarchy = [&](const QRectF& sourceRect, const QRectF& targetRect, QPointF pillCenter,
                                              const char* what) {
            const QRectF pillRect = probePillRect(pillCenter);
            const app::PortAssignment ports =
                app::resolvePillPorts(EdgeClass::Normal, sourceRect, targetRect, pillRect, std::nullopt);
            if (!portAssignmentHoldsInvariant(ports, pillRect, what)) {
                return false;
            }
            if (!onBorder(sourceRect, ports.sourceAnchorSide, ports.sourceAnchor) ||
                !onBorder(targetRect, ports.targetAnchorSide, ports.targetAnchor)) {
                std::fprintf(stderr, "FAIL: %s anchors are not on the actual rect borders\n", what);
                return false;
            }
            if (!finite(ports.sourceAnchor) || !finite(ports.targetAnchor) || !finite(ports.entryPoint) ||
                !finite(ports.exitPoint)) {
                std::fprintf(stderr, "FAIL: %s produced a non-finite port\n", what);
                return false;
            }
            return true;
        };

        const QRectF container(320, 100, 320, 260);  // a compound/parallel container box
        const QRectF child(420, 160, 120, 80);        // fully inside container
        const QRectF outside(900, 400, 160, 60);      // disjoint from both -- an external leaf
        if (!container.contains(child)) {
            std::fprintf(stderr, "FAIL: cross-hierarchy fixture's child rect is not inside its container\n");
            return 1;
        }

        // (a) parent -> own child: source is the CONTAINER rect.
        if (!checkCrossHierarchy(container, child, QPointF(700, 230), "parent-to-child")) {
            return 1;
        }
        // (b) child -> parent: the reverse -- target is the CONTAINER rect.
        if (!checkCrossHierarchy(child, container, QPointF(700, 230), "child-to-parent")) {
            return 1;
        }
        // (c) inside -> outside: the child's own edge to a state outside its
        // container. The line crosses the container's border, but the
        // resolver only sees the two end rects.
        if (!checkCrossHierarchy(child, outside, QPointF(700, 300), "inside-to-outside")) {
            return 1;
        }
    }

    // Segment dragging: combinatorial orthogonality assertions.
    {
        const auto assertStrictlyOrthogonal = [](const QVector<QPointF>& pts, const char* name) -> bool {
            if (pts.size() < 2) {
                std::fprintf(stderr, "FAIL: %s has fewer than 2 points\n", name);
                return false;
            }
            for (int i = 0; i < pts.size() - 1; ++i) {
                const qreal dx = std::abs(pts[i].x() - pts[i + 1].x());
                const qreal dy = std::abs(pts[i].y() - pts[i + 1].y());
                if (dx > 0.001 && dy > 0.001) {
                    std::fprintf(stderr, "FAIL: %s segment %d->%d is not orthogonal (dx=%.3f, dy=%.3f)\n",
                                 name, i, i + 1, dx, dy);
                    return false;
                }
            }
            return true;
        };

        // 1. Straight vertical line:
        const QVector<QPointF> vertLine = { QPointF(100, 100), QPointF(100, 400) };
        for (qreal delta : { -50.0, 50.0, 120.0 }) {
            const auto res = app::translateOrthogonalSegment(vertLine, 0, delta);
            if (res.front() != vertLine.front() || res.back() != vertLine.back()) {
                std::fprintf(stderr, "FAIL: straight vert line drag displaced endpoint anchors\n");
                return 1;
            }
            if (!assertStrictlyOrthogonal(res, "straight-vert-drag")) return 1;
        }

        // 2. Straight horizontal line:
        const QVector<QPointF> horizLine = { QPointF(100, 200), QPointF(500, 200) };
        for (qreal delta : { -40.0, 40.0, 80.0 }) {
            const auto res = app::translateOrthogonalSegment(horizLine, 0, delta);
            if (res.front() != horizLine.front() || res.back() != horizLine.back()) {
                std::fprintf(stderr, "FAIL: straight horiz line drag displaced endpoint anchors\n");
                return 1;
            }
            if (!assertStrictlyOrthogonal(res, "straight-horiz-drag")) return 1;
        }

        // 3. 3-point L-route (horizontal then vertical):
        const QVector<QPointF> lRoute = { QPointF(50, 100), QPointF(300, 100), QPointF(300, 400) };
        for (int seg = 0; seg < 2; ++seg) {
            for (qreal delta : { -30.0, 30.0 }) {
                const auto res = app::translateOrthogonalSegment(lRoute, seg, delta);
                if (res.front() != lRoute.front() || res.back() != lRoute.back()) {
                    std::fprintf(stderr, "FAIL: L-route drag displaced endpoint anchors on seg %d\n", seg);
                    return 1;
                }
                if (!assertStrictlyOrthogonal(res, "L-route-drag")) return 1;
            }
        }

        // 4. 4-point Z-route:
        const QVector<QPointF> zRoute = { QPointF(50, 100), QPointF(200, 100), QPointF(200, 300), QPointF(400, 300) };
        for (int seg = 0; seg < 3; ++seg) {
            for (qreal delta : { -40.0, 40.0 }) {
                const auto res = app::translateOrthogonalSegment(zRoute, seg, delta);
                if (res.front() != zRoute.front() || res.back() != zRoute.back()) {
                    std::fprintf(stderr, "FAIL: Z-route drag displaced endpoint anchors on seg %d\n", seg);
                    return 1;
                }
                if (!assertStrictlyOrthogonal(res, "Z-route-drag")) return 1;
            }
        }

        // 5. routeEdgeWithBendpoints under node movement (unaligned anchors):
        const QVector<QPointF> bps = { QPointF(150, 100), QPointF(150, 300) };
        // Source moved diagonally to (50, 60), target moved diagonally to (350, 340):
        const auto routedWithBps = app::routeEdgeWithBendpoints(
            QPointF(50, 60), PortSide::Right,
            QPointF(350, 340), PortSide::Left,
            bps, 10.0, QPointF());
        if (!assertStrictlyOrthogonal(routedWithBps.waypoints, "routeEdgeWithBendpoints-moved-nodes")) {
            return 1;
        }

        // 6. routeEdgeFromWaypoints with arbitrary diagonal input points:
        const QVector<QPointF> diagonalWaypoints = { QPointF(10, 10), QPointF(100, 150), QPointF(300, 50) };
        const auto routedFromDiag = app::routeEdgeFromWaypoints(diagonalWaypoints, 10.0, QPointF());
        if (!assertStrictlyOrthogonal(routedFromDiag.waypoints, "routeEdgeFromWaypoints-arbitrary-diagonal")) {
            return 1;
        }

        // 7. Pill-to-node keepout clamping:
        const QRectF targetNode(560.0, 120.0, 82.0, 33.0);
        const QSizeF pillSz(60.0, 18.0);
        const qreal halfW = 30.0;
        const qreal halfH = 9.0;

        // (a) Encroaching from above:
        const QPointF encroachingAbove(600.0, 105.0);
        const QPointF clampedAbove = app::clampPillNodeKeepout(encroachingAbove, pillSz, QRectF(), targetNode);
        const qreal expectedMaxY = targetNode.top() - app::kPillNodeKeepout - halfH;  // 120 - 20 - 9 = 91.0
        if (std::abs(clampedAbove.y() - expectedMaxY) > 0.01 || std::abs(clampedAbove.x() - 600.0) > 0.01) {
            std::fprintf(stderr, "FAIL: clampPillNodeKeepout above failed (y=%.1f, want %.1f)\n",
                         clampedAbove.y(), expectedMaxY);
            return 1;
        }

        // (b) Encroaching from below:
        const QPointF encroachingBelow(600.0, 160.0);
        const QPointF clampedBelow = app::clampPillNodeKeepout(encroachingBelow, pillSz, QRectF(), targetNode);
        const qreal expectedMinY = targetNode.bottom() + app::kPillNodeKeepout + halfH;  // 153 + 20 + 9 = 182.0
        if (std::abs(clampedBelow.y() - expectedMinY) > 0.01) {
            std::fprintf(stderr, "FAIL: clampPillNodeKeepout below failed (y=%.1f, want %.1f)\n",
                         clampedBelow.y(), expectedMinY);
            return 1;
        }

        // (c) Encroaching from left:
        const QPointF encroachingLeft(540.0, 135.0);
        const QPointF clampedLeft = app::clampPillNodeKeepout(encroachingLeft, pillSz, QRectF(), targetNode);
        const qreal expectedMaxX = targetNode.left() - app::kPillNodeKeepout - halfW;  // 560 - 20 - 30 = 510.0
        if (std::abs(clampedLeft.x() - expectedMaxX) > 0.01) {
            std::fprintf(stderr, "FAIL: clampPillNodeKeepout left failed (x=%.1f, want %.1f)\n",
                         clampedLeft.x(), expectedMaxX);
            return 1;
        }

        // (d) Encroaching from right:
        const QPointF encroachingRight(660.0, 135.0);
        const QPointF clampedRight = app::clampPillNodeKeepout(encroachingRight, pillSz, QRectF(), targetNode);
        const qreal expectedMinX = targetNode.right() + app::kPillNodeKeepout + halfW;  // 642 + 20 + 30 = 692.0
        if (std::abs(clampedRight.x() - expectedMinX) > 0.01) {
            std::fprintf(stderr, "FAIL: clampPillNodeKeepout right failed (x=%.1f, want %.1f)\n",
                         clampedRight.x(), expectedMinX);
            return 1;
        }

        // (f) The tight-corridor centering applies only to a pill actually
        // SQUEEZED between the two nodes: a wide pill well below both nodes must
        // not have its x pinned to the corridor midpoint, or a horizontal drag
        // stalls and then jumps.
        {
            const QRectF left(0.0, 0.0, 100.0, 50.0);
            const QRectF right(400.0, 0.0, 100.0, 50.0);  // gap 300 < 320 + 2*keepout: a tight corridor
            const QSizeF widePill(320.0, 40.0);
            for (qreal x = 380.0; x >= 120.0; x -= 20.0) {
                const QPointF below(x, 150.0);  // pill top 130 -- clear of both nodes' keepout (50 + 20)
                const QPointF clamped = app::clampPillNodeKeepout(below, widePill, left, right);
                if (std::abs(clamped.x() - x) > 0.01 || std::abs(clamped.y() - 150.0) > 0.01) {
                    std::fprintf(stderr,
                                 "FAIL: clampPillNodeKeepout pinned a pill clear of both nodes (in %.1f,150 -> out "
                                 "%.1f,%.1f)\n",
                                 x, clamped.x(), clamped.y());
                    return 1;
                }
            }
            // A pill genuinely squeezed between them keeps the corridor rule:
            // wherever its center sits in the span, it lands on one position.
            const QPointF squeezedA = app::clampPillNodeKeepout(QPointF(180.0, 25.0), widePill, left, right);
            const QPointF squeezedB = app::clampPillNodeKeepout(QPointF(320.0, 25.0), widePill, left, right);
            if ((squeezedA - squeezedB).manhattanLength() > 0.01) {
                std::fprintf(stderr,
                             "FAIL: clampPillNodeKeepout no longer pins a pill squeezed between the nodes (%.1f,%.1f vs "
                             "%.1f,%.1f)\n",
                             squeezedA.x(), squeezedA.y(), squeezedB.x(), squeezedB.y());
                return 1;
            }
        }

        // (e) Verify that resolving with the clamped pill above target produces a clean straight 2-point vertical run:
        const QRectF sourceNode(280.0, 120.0, 110.0, 33.0);
        const QRectF clampedPillRect(clampedAbove.x() - halfW, clampedAbove.y() - halfH, pillSz.width(), pillSz.height());
        const app::PortAssignment portsAbove = app::resolvePillPorts(
            app::EdgeClass::Normal, sourceNode, targetNode, clampedPillRect, std::nullopt);
        const app::RoutedEdge routeAbove = app::routeThroughPorts(portsAbove);
        if (routeAbove.targetWaypoints.size() != 2) {
            std::fprintf(stderr, "FAIL: clamped pill above node did not route as 2-point straight (size=%d)\n",
                         static_cast<int>(routeAbove.targetWaypoints.size()));
            return 1;
        }
        if (std::abs(routeAbove.targetWaypoints.front().x() - routeAbove.targetWaypoints.back().x()) > 0.01) {
            std::fprintf(stderr, "FAIL: clamped pill above node target half is not strictly vertical\n");
            return 1;
        }
    }

    // ---- 8: Unified Wire Routing (Single Continuous Polyline) -----------------
    // Verifies that routeUnifiedEdge produces a single unbroken polyline between source and target:
    // (a) Parallel ports with height delta generate strictly 4 waypoints (1 Z-step, 2 corners) without micro-Z jogs
    // (b) labelOffset moves labelAnchor relative to canonical labelBase
    // (c) Collinear ports produce strictly 2 waypoints (straight run)
    // (d) Perpendicular diagonal ports produce strictly 3 waypoints (1 corner L-route)
    {
        // (a) Parallel run with vertical height delta (Authenticating Y 192.0 -> LoggedIn Y 184.6):
        const QPointF sourceAnchor(440.0, 192.0);
        const QPointF targetAnchor(560.0, 184.6);
        const app::RoutedEdge routeZ = app::routeUnifiedEdge(
            sourceAnchor, PortSide::Right, targetAnchor, PortSide::Left, 10.0, QPointF(0.0, 0.0));

        if (routeZ.waypoints.size() != 4) {
            std::fprintf(stderr, "FAIL: routeUnifiedEdge parallel height delta expected 4 waypoints, got %d\n",
                         static_cast<int>(routeZ.waypoints.size()));
            return 1;
        }
        if (std::abs(routeZ.waypoints[0].y() - 192.0) > 0.01 || std::abs(routeZ.waypoints[1].y() - 192.0) > 0.01) {
            std::fprintf(stderr, "FAIL: routeUnifiedEdge departure segment is not horizontal at Y=192.0\n");
            return 1;
        }
        if (std::abs(routeZ.waypoints[2].y() - 184.6) > 0.01 || std::abs(routeZ.waypoints[3].y() - 184.6) > 0.01) {
            std::fprintf(stderr, "FAIL: routeUnifiedEdge arrival segment is not horizontal at Y=184.6\n");
            return 1;
        }
        if (std::abs(routeZ.waypoints[1].x() - routeZ.waypoints[2].x()) > 0.01) {
            std::fprintf(stderr, "FAIL: routeUnifiedEdge transversal channel is not strictly vertical\n");
            return 1;
        }
        if (routeZ.sourceHalf.isEmpty() || routeZ.targetHalf.isEmpty()) {
            std::fprintf(stderr, "FAIL: routeUnifiedEdge should render contiguous zero-gap polyline halves\n");
            return 1;
        }
        if (routeZ.labelBase.isNull() || routeZ.labelAnchor != routeZ.labelBase) {
            std::fprintf(stderr, "FAIL: routeUnifiedEdge labelBase not set or labelAnchor != labelBase when offset is zero\n");
            return 1;
        }

        // (b) Parallel run with relative labelOffset:
        const app::RoutedEdge routeWithOffset = app::routeUnifiedEdge(
            sourceAnchor, PortSide::Right, targetAnchor, PortSide::Left, 10.0, QPointF(20.0, -15.0));
        if (routeWithOffset.labelAnchor != routeWithOffset.labelBase + QPointF(20.0, -15.0)) {
            std::fprintf(stderr, "FAIL: routeUnifiedEdge labelAnchor did not match labelBase + labelOffset\n");
            return 1;
        }

        // (c) Collinear parallel run (strictly horizontal):
        const app::RoutedEdge routeStraight = app::routeUnifiedEdge(
            QPointF(440.0, 190.0), PortSide::Right, QPointF(560.0, 190.0), PortSide::Left, 10.0);
        if (routeStraight.waypoints.size() != 2) {
            std::fprintf(stderr, "FAIL: routeUnifiedEdge collinear parallel expected 2 waypoints, got %d\n",
                         static_cast<int>(routeStraight.waypoints.size()));
            return 1;
        }

        // (d) Diagonal perpendicular run (1-corner L-route):
        const app::RoutedEdge routeL = app::routeUnifiedEdge(
            QPointF(320.0, 390.0), PortSide::Left, QPointF(140.0, 260.0), PortSide::Bottom, 10.0);
        if (routeL.waypoints.size() != 3) {
            std::fprintf(stderr, "FAIL: routeUnifiedEdge perpendicular diagonal expected 3 waypoints (1-corner L), got %d\n",
                         static_cast<int>(routeL.waypoints.size()));
            return 1;
        }
    }

    std::printf("PASS: state-designer pill-port resolver smoke (%d rule rows 1:1, perpendicular side-middle ports, "
                "distinct sides, self straddle, flip hysteresis, cross-hierarchy parent/child/outside, "
                "pill-to-node keepout clamping, unified wire routing, orthogonal segment dragging combinatorial invariants)\n",
                ruleCount);
    return 0;
}
