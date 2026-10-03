#include "view/geometry/pill_port_resolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string_view>

#include <QVector>

#include "view/geometry/routing_logger.h"

namespace app {

namespace {

// The rule table. A new rule is a row, not a branch: resolvePillPorts only looks
// rows up. The eight conflict rows state one priority: the target half keeps the
// contested side and the source half yields to the adjacent side matching its
// counterpart's lateral direction. Only the contested-Top pair was observed; the
// other six rows are that rule mirrored or rotated.
constexpr PillPortRule kRules[] = {
    // Each line prefers the pill side facing its counterpart anchor; with the two
    // preferences distinct, both stand.
    {"normal-facing", EdgeClass::Normal, SideCollision::Distinct, PortSide::Top, Lateral::Irrelevant,
     SideSource::FacingCounterpart, PortSide::Top, SideSource::FacingCounterpart,
     "two distinct facing preferences, both kept"},
    // Both nodes above the pill: target keeps top, source takes right.
    {"normal-yield-top-source-right", EdgeClass::Normal, SideCollision::Same, PortSide::Top, Lateral::Positive,
     SideSource::YieldedSide, PortSide::Right, SideSource::FacingCounterpart,
     "contested top, source node right of the pill"},
    // Same layout with the source node moved across the pill: the source line
    // flips right -> left.
    {"normal-yield-top-source-left", EdgeClass::Normal, SideCollision::Same, PortSide::Top, Lateral::Negative,
     SideSource::YieldedSide, PortSide::Left, SideSource::FacingCounterpart,
     "contested top, source node left of the pill (the crossing flip)"},
    // The two rows above mirrored about the pill's horizontal axis.
    {"normal-yield-bottom-source-right", EdgeClass::Normal, SideCollision::Same, PortSide::Bottom, Lateral::Positive,
     SideSource::YieldedSide, PortSide::Right, SideSource::FacingCounterpart,
     "contested bottom, source node right of the pill"},
    {"normal-yield-bottom-source-left", EdgeClass::Normal, SideCollision::Same, PortSide::Bottom, Lateral::Negative,
     SideSource::YieldedSide, PortSide::Left, SideSource::FacingCounterpart,
     "contested bottom, source node left of the pill"},
    // Rotated a quarter turn: a contested vertical side's lateral axis is y, so
    // the source yields to Bottom/Top.
    {"normal-yield-left-source-below", EdgeClass::Normal, SideCollision::Same, PortSide::Left, Lateral::Positive,
     SideSource::YieldedSide, PortSide::Bottom, SideSource::FacingCounterpart,
     "contested left, source node below the pill"},
    {"normal-yield-left-source-above", EdgeClass::Normal, SideCollision::Same, PortSide::Left, Lateral::Negative,
     SideSource::YieldedSide, PortSide::Top, SideSource::FacingCounterpart,
     "contested left, source node above the pill"},
    {"normal-yield-right-source-below", EdgeClass::Normal, SideCollision::Same, PortSide::Right, Lateral::Positive,
     SideSource::YieldedSide, PortSide::Bottom, SideSource::FacingCounterpart,
     "contested right, source node below the pill"},
    {"normal-yield-right-source-above", EdgeClass::Normal, SideCollision::Same, PortSide::Right, Lateral::Negative,
     SideSource::YieldedSide, PortSide::Top, SideSource::FacingCounterpart,
     "contested right, source node above the pill"},
    // A self pair shares the one side facing the node, as two attach points
    // straddling its middle; the distinct-sides invariant is normal-only.
    {"self-straddle", EdgeClass::Self, SideCollision::Irrelevant, PortSide::Top, Lateral::Irrelevant,
     SideSource::FacingCounterpart, PortSide::Top, SideSource::SharedWithEntry,
     "out-and-back pair straddles the node-facing side's middle"},
    // The pill's body terminates a targetless edge: entry port only.
    {"targetless-entry-only", EdgeClass::Targetless, SideCollision::Irrelevant, PortSide::Top, Lateral::Irrelevant,
     SideSource::FacingCounterpart, PortSide::Top, SideSource::Absent,
     "the pill terminates the edge; entry port only"},
    // Root pills: the frame edge point stands in as the counterpart; one exit
    // port, facing the frame.
    {"root-frame-exit", EdgeClass::Root, SideCollision::Irrelevant, PortSide::Top, Lateral::Irrelevant,
     SideSource::Absent, PortSide::Top, SideSource::FacingCounterpart,
     "frame-right anchoring kept, frame edge as counterpart"},
    // A diagonal quadrant relationship forms a single 1-bend L-shape.
    {"normal-min-bend-l", EdgeClass::Normal, SideCollision::Distinct, PortSide::Top, Lateral::Irrelevant,
     SideSource::FacingCounterpart, PortSide::Top, SideSource::FacingCounterpart,
     "diagonal quadrant relationship forms a 1-bend L-shape"},
};

const PillPortRule& ruleById(const char* id) {
    for (const PillPortRule& rule : kRules) {
        if (std::string_view(rule.id) == std::string_view(id)) {
            return rule;
        }
    }
    return kRules[0];  // unreachable for the ids this file passes
}

// The yield row for a contested side and the source counterpart's lateral sign.
const PillPortRule& yieldRule(PortSide contested, bool positiveLateral) {
    const Lateral want = positiveLateral ? Lateral::Positive : Lateral::Negative;
    for (const PillPortRule& rule : kRules) {
        if (rule.collision == SideCollision::Same && rule.contested == contested && rule.sourceLateral == want) {
            return rule;
        }
    }
    return kRules[0];  // unreachable: the table covers all four sides x both signs
}

// A side's "along" axis is x for Top/Bottom, y for Left/Right.
bool alongIsX(PortSide side) { return side == PortSide::Top || side == PortSide::Bottom; }

PortSide sideFacing(QPointF fromCenter, QPointF toward) {
    const QPointF delta = toward - fromCenter;
    if (std::abs(delta.x()) >= std::abs(delta.y())) {
        return delta.x() >= 0.0 ? PortSide::Right : PortSide::Left;
    }
    return delta.y() >= 0.0 ? PortSide::Bottom : PortSide::Top;
}

// `toward`'s along-side coordinate projected onto `rect`'s `side`, clamped
// kAnchorInset inside the corners. A rect too small to inset collapses to the
// side's middle, keeping the anchor on the rect for a degenerate counterpart
// such as the machine frame's edge point.
QPointF sideProjection(const QRectF& rect, PortSide side, QPointF toward) {
    const bool alongX = alongIsX(side);
    qreal lo = (alongX ? rect.left() : rect.top()) + kAnchorInset;
    qreal hi = (alongX ? rect.right() : rect.bottom()) - kAnchorInset;
    if (lo > hi) {
        lo = hi = (alongX ? rect.left() + rect.right() : rect.top() + rect.bottom()) / 2.0;
    }
    const qreal along = std::clamp(alongX ? toward.x() : toward.y(), lo, hi);
    switch (side) {
        case PortSide::Top:
            return QPointF(along, rect.top());
        case PortSide::Bottom:
            return QPointF(along, rect.bottom());
        case PortSide::Left:
            return QPointF(rect.left(), along);
        case PortSide::Right:
            break;
    }
    return QPointF(rect.right(), along);
}

// How far `counterpart` lies toward `side`, in px; what the facing preference
// maximizes and hysteresis measures.
qreal sideScore(QPointF pillCenter, QPointF counterpart, PortSide side) {
    return QPointF::dotProduct(counterpart - pillCenter, outwardNormal(side));
}

// Keeps `incumbent` until the counterpart lies more than kFlipHysteresis further
// toward `fresh`.
PortSide holdSide(PortSide fresh, PortSide incumbent, QPointF pillCenter, QPointF counterpart) {
    if (fresh == incumbent) {
        return fresh;
    }
    const qreal margin = sideScore(pillCenter, counterpart, fresh) - sideScore(pillCenter, counterpart, incumbent);
    return margin > kFlipHysteresis ? fresh : incumbent;
}

HalfShape classifyRun(const QVector<QPointF>& run) {
    if (run.size() <= 2) {
        return HalfShape::Straight;
    }
    return run.size() == 3 ? HalfShape::L : HalfShape::Z;
}

// Min-bend node anchor side: center-to-center dominance alone can park the
// anchor on a side whose run needs a Z while the other facing side reaches the
// pill's port with a single L. Both facing candidates are scored by their
// actual run's point count; dominance only breaks ties.
PortSide minBendAnchorSide(const QRectF& nodeRect, QPointF pill, QPointF port, PortSide portSide,
                           bool isSource) {
    const PortSide hSide = pill.x() >= nodeRect.center().x() ? PortSide::Right : PortSide::Left;
    const PortSide vSide = pill.y() >= nodeRect.center().y() ? PortSide::Bottom : PortSide::Top;
    const auto runSize = [&](PortSide side) {
        const QPointF anchor = sideProjection(nodeRect, side, port);
        // Score with the alignment the micro-jog nudge applies afterwards: on a
        // matching axis a sub-tolerance residual counts as aligned, or a candidate
        // the nudge would straighten loses to a worse one when the pill parks
        // close enough that the raw Z falls back multi-corner.
        QPointF scoredPort = port;
        if (alongIsX(side) == alongIsX(portSide)) {
            if (!alongIsX(side)) {
                if (std::abs(anchor.y() - port.y()) <= kPortAlignTolerance) {
                    scoredPort.setY(anchor.y());
                }
            } else if (std::abs(anchor.x() - port.x()) <= kPortAlignTolerance) {
                scoredPort.setX(anchor.x());
            }
        }
        if (isSource) {
            return static_cast<int>(orthogonalPortRun(sourceHalfStart(anchor, side), outwardNormal(side),
                                                      scoredPort, -outwardNormal(portSide))
                                        .size());
        }
        return static_cast<int>(orthogonalPortRun(scoredPort, outwardNormal(portSide),
                                                  targetHalfStop(anchor, side), -outwardNormal(side))
                                    .size());
    };
    // Wraps (>= 5 points) are equally worst: when a port sits inside or behind the
    // node both candidates wrap, and preferring the smaller wrap could break an
    // otherwise straight wire, so the tie falls back to dominance.
    const int hSize = std::min(runSize(hSide), 5);
    const int vSize = std::min(runSize(vSide), 5);
    if (hSize == vSize) {
        return sideFacing(nodeRect.center(), pill);
    }
    return hSize < vSize ? hSide : vSide;
}

}  // namespace

const PillPortRule* pillPortRuleTable(int* rowCount) {
    if (rowCount != nullptr) {
        *rowCount = static_cast<int>(std::size(kRules));
    }
    return kRules;
}

PortSide preferredPillSide(const QRectF& pillRect, QPointF counterpart) {
    return sideFacing(pillRect.center(), counterpart);
}

PortSide oppositePortSide(PortSide side) {
    switch (side) {
        case PortSide::Top:
            return PortSide::Bottom;
        case PortSide::Bottom:
            return PortSide::Top;
        case PortSide::Left:
            return PortSide::Right;
        case PortSide::Right:
            return PortSide::Left;
    }
    return PortSide::Top;
}

QPointF counterpartAnchor(const QRectF& rect, QPointF toward) {
    return sideProjection(rect, sideFacing(rect.center(), toward), toward);
}

PortAssignment resolvePillPorts(EdgeClass cls, const QRectF& sourceRect, const QRectF& targetRect,
                                 const QRectF& pillRect, const std::optional<PortAssignment>& incumbent) {
    PortAssignment out;
    out.cls = cls;
    out.pillCenter = pillRect.center();
    const QPointF pill = out.pillCenter;

    // ---- classify, preferred side per half, conflict yield --
    qreal entryStraddle = 0.0;
    qreal exitStraddle = 0.0;

    switch (cls) {
        case EdgeClass::Normal: {
            // A diagonal quadrant relationship between the rects may admit an
            // unobstructed 1-bend L-shape.
            const QRectF S = sourceRect;
            const QRectF T = targetRect;
            const bool disjointX = (T.left() >= S.right()) || (S.left() >= T.right());
            const bool disjointY = (T.top() >= S.bottom()) || (S.top() >= T.bottom());
            if (disjointX && disjointY) {
                const qreal dx = T.center().x() - S.center().x();
                const qreal dy = T.center().y() - S.center().y();
                PortSide sSide, tSide;
                if (std::abs(dx) >= std::abs(dy)) {
                    sSide = (dx > 0.0 ? PortSide::Right : PortSide::Left);
                    tSide = (dy < 0.0 ? PortSide::Bottom : PortSide::Top);
                } else {
                    sSide = (dy > 0.0 ? PortSide::Bottom : PortSide::Top);
                    tSide = (dx < 0.0 ? PortSide::Right : PortSide::Left);
                }
                const bool sHoriz = sSide == PortSide::Left || sSide == PortSide::Right;
                const QPointF sAnchor = sideProjection(sourceRect, sSide, pill);
                const QPointF tAnchor = sideProjection(targetRect, tSide, pill);
                const QPointF sOut = outwardNormal(sSide);
                const QPointF tIn = -outwardNormal(tSide);
                const QPointF corner = sHoriz ? QPointF(tAnchor.x(), sAnchor.y())
                                              : QPointF(sAnchor.x(), tAnchor.y());
                const auto ahead = [](QPointF a, QPointF b, QPointF dir) {
                    return QPointF::dotProduct(b - a, dir) > 0.01;
                };

                bool onLeg1 = false;
                bool onLeg2 = false;
                if (sHoriz) {
                    const qreal minX = std::min(sAnchor.x(), corner.x()) - 20.0;
                    const qreal maxX = std::max(sAnchor.x(), corner.x()) + 20.0;
                    onLeg1 = (std::abs(pill.y() - sAnchor.y()) <= 30.0) && (pill.x() >= minX && pill.x() <= maxX);

                    const qreal minY = std::min(corner.y(), tAnchor.y()) - 20.0;
                    const qreal maxY = std::max(corner.y(), tAnchor.y()) + 20.0;
                    onLeg2 = (std::abs(pill.x() - tAnchor.x()) <= 30.0) && (pill.y() >= minY && pill.y() <= maxY);
                } else {
                    const qreal minY = std::min(sAnchor.y(), corner.y()) - 20.0;
                    const qreal maxY = std::max(sAnchor.y(), corner.y()) + 20.0;
                    onLeg1 = (std::abs(pill.x() - sAnchor.x()) <= 30.0) && (pill.y() >= minY && pill.y() <= maxY);

                    const qreal minX = std::min(corner.x(), tAnchor.x()) - 20.0;
                    const qreal maxX = std::max(corner.x(), tAnchor.x()) + 20.0;
                    onLeg2 = (std::abs(pill.y() - tAnchor.y()) <= 30.0) && (pill.x() >= minX && pill.x() <= maxX);
                }

                if (ahead(sAnchor, corner, sOut) && ahead(corner, tAnchor, tIn) && (onLeg1 || onLeg2)) {
                    out.sourceAnchorSide = sSide;
                    out.targetAnchorSide = tSide;
                    if (onLeg1) {
                        out.entrySide = oppositePortSide(sSide);
                        out.exitSide = sSide;
                    } else {
                        // On leg 2 the wire travels from the corner to tAnchor; if
                        // tSide is Bottom it approaches going up, so it enters
                        // the pill from Bottom and exits Top.
                        out.entrySide = tSide;
                        out.exitSide = oppositePortSide(tSide);
                    }
                    out.ruleId = ruleById("normal-min-bend-l").id;
                    break;
                }
            }

            const QPointF sourceCounterpart = counterpartAnchor(sourceRect, pill);
            const QPointF targetCounterpart = counterpartAnchor(targetRect, pill);
            // The target half settles first: it keeps a contested side.
            PortSide exit = sideFacing(pill, targetCounterpart);
            if (incumbent.has_value()) {
                exit = holdSide(exit, incumbent->exitSide, pill, targetCounterpart);
            }
            PortSide entry = sideFacing(pill, sourceCounterpart);
            const PillPortRule* row = &ruleById("normal-facing");
            if (entry == exit) {
                const bool positive = alongIsX(exit) ? sourceCounterpart.x() > pill.x()
                                                      : sourceCounterpart.y() > pill.y();
                row = &yieldRule(exit, positive);
                entry = row->yieldedSide;
            }
            if (incumbent.has_value()) {
                // Hysteresis must not break the distinct-sides invariant: a held
                // side that collides with the target's is dropped.
                const PortSide held = holdSide(entry, incumbent->entrySide, pill, sourceCounterpart);
                if (held != exit) {
                    entry = held;
                }
            }
            out.entrySide = entry;
            out.exitSide = exit;
            out.ruleId = row->id;
            break;
        }
        case EdgeClass::Self: {
            const QPointF sourceCounterpart = counterpartAnchor(sourceRect, pill);
            PortSide shared = sideFacing(pill, sourceCounterpart);
            if (incumbent.has_value()) {
                shared = holdSide(shared, incumbent->entrySide, pill, sourceCounterpart);
            }
            out.entrySide = shared;
            out.exitSide = shared;
            // The pair straddles the side's middle; entry takes the negative side
            // so the two runs stay parallel.
            entryStraddle = -kSelfStraddle;
            exitStraddle = kSelfStraddle;
            out.ruleId = ruleById("self-straddle").id;
            break;
        }
        case EdgeClass::Targetless: {
            const QPointF sourceCounterpart = counterpartAnchor(sourceRect, pill);
            PortSide entry = sideFacing(pill, sourceCounterpart);
            if (incumbent.has_value()) {
                entry = holdSide(entry, incumbent->entrySide, pill, sourceCounterpart);
            }
            out.entrySide = entry;
            out.exitSide = entry;  // unused; kept defined for a stable struct
            out.hasExit = false;
            out.ruleId = ruleById("targetless-entry-only").id;
            break;
        }
        case EdgeClass::Root: {
            const QPointF targetCounterpart = counterpartAnchor(targetRect, pill);
            PortSide exit = sideFacing(pill, targetCounterpart);
            if (incumbent.has_value()) {
                exit = holdSide(exit, incumbent->exitSide, pill, targetCounterpart);
            }
            out.exitSide = exit;
            out.entrySide = exit;  // unused; kept defined for a stable struct
            out.hasEntry = false;
            out.ruleId = ruleById("root-frame-exit").id;
            break;
        }
    }

    // ---- attach points (side middles; Self: the straddled pair) ---
    out.entryPoint = sidePortAnchor(pillRect, out.entrySide, entryStraddle);
    out.exitPoint = sidePortAnchor(pillRect, out.exitSide, exitStraddle);

    // The node end of each half: the port point projected onto the node side
    // facing the pill. Projecting the port, not the pill center, is what draws an
    // aligned edge straight and keeps a self pair's runs parallel.
    if (out.hasEntry) {
        if (std::strcmp(out.ruleId, "normal-min-bend-l") != 0) {
            out.sourceAnchorSide =
                cls == EdgeClass::Normal
                    ? minBendAnchorSide(sourceRect, pill, out.entryPoint, out.entrySide, /*isSource=*/true)
                    : sideFacing(sourceRect.center(), pill);
        }
        out.sourceAnchor = sideProjection(sourceRect, out.sourceAnchorSide, out.entryPoint);
    }
    if (out.hasExit) {
        if (std::strcmp(out.ruleId, "normal-min-bend-l") != 0) {
            out.targetAnchorSide =
                cls == EdgeClass::Normal
                    ? minBendAnchorSide(targetRect, pill, out.exitPoint, out.exitSide, /*isSource=*/false)
                    : sideFacing(targetRect.center(), pill);
        }
        out.targetAnchor = sideProjection(targetRect, out.targetAnchorSide, out.exitPoint);
    }

    // ---- small-wrap (curl / snake) avoidance ----------------------
    // A yielded or hysteresis-held port can face away from its counterpart, or
    // close placement can make orthogonalPortRun fall back to a 5- or 6-point
    // wrap. Under kWrapFlipThreshold such wraps read as defects, so the half
    // searches other pill sides for a clean run (Straight / L / Z, <= 4 points).
    // Normal class only: Self/Root/Targetless own their side geometry.
    if (cls == EdgeClass::Normal) {
        const auto runLen = [](const QVector<QPointF>& run) {
            qreal length = 0.0;
            for (int i = 0; i < run.size() - 1; ++i) {
                length += std::abs(run[i].x() - run[i + 1].x()) + std::abs(run[i].y() - run[i + 1].y());
            }
            return length;
        };
        const auto isSmallWrap = [&](const QVector<QPointF>& run) {
            return run.size() >= 5 && runLen(run) < kWrapFlipThreshold;
        };
        const auto perpToward = [&](PortSide current, QPointF counterpart) {
            const bool currentVertical = current == PortSide::Top || current == PortSide::Bottom;
            if (currentVertical) {
                return counterpart.x() < pill.x() ? PortSide::Left : PortSide::Right;
            }
            return counterpart.y() < pill.y() ? PortSide::Top : PortSide::Bottom;
        };
        const auto computeAnchorSide = [&](bool exitHalf, PortSide pillSide, const QPointF& port) {
            const QRectF& nodeRect = exitHalf ? targetRect : sourceRect;
            if (std::strcmp(out.ruleId, "normal-min-bend-l") != 0) {
                return minBendAnchorSide(nodeRect, pill, port, pillSide, /*isSource=*/!exitHalf);
            }
            return sideFacing(nodeRect.center(), pill);
        };
        const auto runFor = [&](bool exitHalf, PortSide pillSide) {
            const QRectF& nodeRect = exitHalf ? targetRect : sourceRect;
            const QPointF port = sidePortAnchor(pillRect, pillSide, 0.0);
            const PortSide anchorSide = computeAnchorSide(exitHalf, pillSide, port);
            const QPointF anchor = sideProjection(nodeRect, anchorSide, port);
            if (exitHalf) {
                return orthogonalPortRun(port, outwardNormal(pillSide),
                                         targetHalfStop(anchor, anchorSide), -outwardNormal(anchorSide));
            }
            return orthogonalPortRun(sourceHalfStart(anchor, anchorSide), outwardNormal(anchorSide), port,
                                     -outwardNormal(pillSide));
        };
        const auto adopt = [&](bool exitHalf, PortSide pillSide) {
            const QRectF& nodeRect = exitHalf ? targetRect : sourceRect;
            const QPointF port = sidePortAnchor(pillRect, pillSide, 0.0);
            const PortSide anchorSide = computeAnchorSide(exitHalf, pillSide, port);
            const QPointF anchor = sideProjection(nodeRect, anchorSide, port);
            if (exitHalf) {
                out.exitSide = pillSide;
                out.exitPoint = port;
                out.targetAnchorSide = anchorSide;
                out.targetAnchor = anchor;
            } else {
                out.entrySide = pillSide;
                out.entryPoint = port;
                out.sourceAnchorSide = anchorSide;
                out.sourceAnchor = anchor;
            }
        };

        const bool nodesAlignedH = std::abs(sourceRect.center().y() - targetRect.center().y()) <= kPortAlignTolerance;
        const bool nodesAlignedV = std::abs(sourceRect.center().x() - targetRect.center().x()) <= kPortAlignTolerance;
        const auto breaksCorridor = [&](PortSide current, PortSide candidate) {
            if (nodesAlignedH && !alongIsX(current) && alongIsX(candidate)) {
                return true;
            }
            if (nodesAlignedV && alongIsX(current) && !alongIsX(candidate)) {
                return true;
            }
            return false;
        };

        // Exit first (it owns contested sides), then entry vs the updated exit.
        if (out.hasExit) {
            const auto run = runFor(true, out.exitSide);
            if (isSmallWrap(run)) {
                const PortSide alt1 = perpToward(out.exitSide, counterpartAnchor(targetRect, pill));
                const PortSide alt2 = oppositePortSide(alt1);
                const PortSide alt3 = oppositePortSide(out.exitSide);

                PortSide bestSide = out.exitSide;
                int bestPts = run.size();
                qreal bestLen = runLen(run);

                // Unoccupied candidate sides first.
                for (PortSide c : {alt1, alt2, alt3}) {
                    if (c == out.entrySide || breaksCorridor(out.exitSide, c)) {
                        continue;
                    }
                    const auto cRun = runFor(true, c);
                    const int cPts = cRun.size();
                    const qreal cLen = runLen(cRun);
                    if (cPts <= 4) {
                        if (bestPts > 4 || cPts < bestPts || (cPts == bestPts && cLen < bestLen)) {
                            bestSide = c;
                            bestPts = cPts;
                            bestLen = cLen;
                        }
                    }
                }

                logRoutingTrace(
                    QStringLiteral("STAGE4B_EXIT"),
                    QStringLiteral("exitSide=%1 runPts=%2 runLen=%3 bestSide=%4 bestPts=%5")
                        .arg(static_cast<int>(out.exitSide))
                        .arg(run.size())
                        .arg(runLen(run))
                        .arg(static_cast<int>(bestSide))
                        .arg(bestPts));

                if (bestSide != out.exitSide && bestPts <= 4) {
                    adopt(true, bestSide);
                    logRoutingTrace(QStringLiteral("STAGE4B_EXIT_ADOPT"),
                                    QStringLiteral("Adopted alt=%1 for exitSide").arg(static_cast<int>(bestSide)));
                } else if (bestPts > 4 && out.hasEntry) {
                    // Every unoccupied side wraps: try taking entry's side with a
                    // clean run while entry hands over to another clean side.
                    const PortSide candidate = out.entrySide;
                    const auto candRun = runFor(true, candidate);
                    if (candRun.size() <= 4 && !breaksCorridor(out.exitSide, candidate)) {
                        const PortSide entryAlt1 = perpToward(out.entrySide, counterpartAnchor(sourceRect, pill));
                        const PortSide entryAlt2 = oppositePortSide(entryAlt1);
                        const PortSide entryAlt3 = oppositePortSide(out.entrySide);
                        PortSide bestEntrySide = out.entrySide;
                        int bestEntryPts = 99;
                        qreal bestEntryLen = 1e9;
                        for (PortSide ec : {entryAlt1, entryAlt2, entryAlt3}) {
                            if (ec == candidate || breaksCorridor(out.entrySide, ec)) {
                                continue;
                            }
                            const auto ecRun = runFor(false, ec);
                            if (ecRun.size() <= 4) {
                                if (ecRun.size() < bestEntryPts || (ecRun.size() == bestEntryPts && runLen(ecRun) < bestEntryLen)) {
                                    bestEntrySide = ec;
                                    bestEntryPts = ecRun.size();
                                    bestEntryLen = runLen(ecRun);
                                }
                            }
                        }
                        if (bestEntryPts <= 4) {
                            adopt(false, bestEntrySide);
                            adopt(true, candidate);
                            logRoutingTrace(
                                QStringLiteral("STAGE4B_EXIT_HANDOVER_ADOPT"),
                                QStringLiteral("Exit handover: entry took %1, exit took %2")
                                    .arg(static_cast<int>(bestEntrySide))
                                    .arg(static_cast<int>(candidate)));
                        }
                    }
                }
            }
        }

        if (out.hasEntry) {
            const auto run = runFor(false, out.entrySide);
            if (isSmallWrap(run)) {
                const PortSide alt1 = perpToward(out.entrySide, counterpartAnchor(sourceRect, pill));
                const PortSide alt2 = oppositePortSide(alt1);
                const PortSide alt3 = oppositePortSide(out.entrySide);

                PortSide bestSide = out.entrySide;
                int bestPts = run.size();
                qreal bestLen = runLen(run);

                // Unoccupied candidate sides first.
                for (PortSide c : {alt1, alt2, alt3}) {
                    if (c == out.exitSide || breaksCorridor(out.entrySide, c)) {
                        continue;
                    }
                    const auto cRun = runFor(false, c);
                    const int cPts = cRun.size();
                    const qreal cLen = runLen(cRun);
                    if (cPts <= 4) {
                        if (bestPts > 4 || cPts < bestPts || (cPts == bestPts && cLen < bestLen)) {
                            bestSide = c;
                            bestPts = cPts;
                            bestLen = cLen;
                        }
                    }
                }

                logRoutingTrace(
                    QStringLiteral("STAGE4B_ENTRY"),
                    QStringLiteral("entrySide=%1 runPts=%2 runLen=%3 bestSide=%4 bestPts=%5")
                        .arg(static_cast<int>(out.entrySide))
                        .arg(run.size())
                        .arg(runLen(run))
                        .arg(static_cast<int>(bestSide))
                        .arg(bestPts));

                if (bestSide != out.entrySide && bestPts <= 4) {
                    adopt(false, bestSide);
                    logRoutingTrace(QStringLiteral("STAGE4B_ENTRY_ADOPT"),
                                    QStringLiteral("Adopted alt=%1 for entrySide").arg(static_cast<int>(bestSide)));
                } else if (bestPts > 4 && out.hasExit) {
                    // Handover: entry takes exit's side if clean, exit takes another clean side.
                    const PortSide candidate = out.exitSide;
                    const auto candRun = runFor(false, candidate);
                    if (candRun.size() <= 4 && !breaksCorridor(out.entrySide, candidate)) {
                        const PortSide exitAlt1 = perpToward(out.exitSide, counterpartAnchor(targetRect, pill));
                        const PortSide exitAlt2 = oppositePortSide(exitAlt1);
                        const PortSide exitAlt3 = oppositePortSide(out.exitSide);
                        PortSide bestExitSide = out.exitSide;
                        int bestExitPts = 99;
                        qreal bestExitLen = 1e9;
                        for (PortSide xc : {exitAlt1, exitAlt2, exitAlt3}) {
                            if (xc == candidate || breaksCorridor(out.exitSide, xc)) {
                                continue;
                            }
                            const auto xcRun = runFor(true, xc);
                            if (xcRun.size() <= 4) {
                                if (xcRun.size() < bestExitPts || (xcRun.size() == bestExitPts && runLen(xcRun) < bestExitLen)) {
                                    bestExitSide = xc;
                                    bestExitPts = xcRun.size();
                                    bestExitLen = runLen(xcRun);
                                }
                            }
                        }
                        if (bestExitPts <= 4) {
                            adopt(true, bestExitSide);
                            adopt(false, candidate);
                            logRoutingTrace(
                                QStringLiteral("STAGE4B_ENTRY_HANDOVER_ADOPT"),
                                QStringLiteral("Entry handover: exit took %1, entry took %2")
                                    .arg(static_cast<int>(bestExitSide))
                                    .arg(static_cast<int>(candidate)));
                        }
                    }
                }
            }
        }
    }

    // ---- per-half shape hint -------------------------------------
    if (out.hasEntry) {
        out.sourceShape = classifyRun(orthogonalPortRun(sourceHalfStart(out.sourceAnchor, out.sourceAnchorSide),
                                                         outwardNormal(out.sourceAnchorSide), out.entryPoint,
                                                         -outwardNormal(out.entrySide)));
    }
    if (out.hasExit) {
        out.targetShape = classifyRun(orthogonalPortRun(out.exitPoint, outwardNormal(out.exitSide),
                                                         targetHalfStop(out.targetAnchor, out.targetAnchorSide),
                                                         -outwardNormal(out.targetAnchorSide)));
    }
    return out;
}

static QPointF clampAgainstNode(QPointF center, qreal halfW, qreal halfH, const QRectF& nodeRect) {
    if (!nodeRect.isValid() || nodeRect.isNull()) {
        return center;
    }
    const QRectF expanded = nodeRect.adjusted(-kPillNodeKeepout - halfW,
                                              -kPillNodeKeepout - halfH,
                                              kPillNodeKeepout + halfW,
                                              kPillNodeKeepout + halfH);
    if (!expanded.contains(center)) {
        return center;
    }
    const qreal dLeft = center.x() - expanded.left();
    const qreal dRight = expanded.right() - center.x();
    const qreal dTop = center.y() - expanded.top();
    const qreal dBottom = expanded.bottom() - center.y();

    const qreal minD = std::min({dLeft, dRight, dTop, dBottom});
    if (minD == dTop) {
        center.setY(expanded.top());
    } else if (minD == dBottom) {
        center.setY(expanded.bottom());
    } else if (minD == dLeft) {
        center.setX(expanded.left());
    } else {
        center.setX(expanded.right());
    }
    return center;
}

QPointF clampPillNodeKeepout(QPointF pillCenter, QSizeF pillSize, const QRectF& sourceRect,
                             const QRectF& targetRect) {
    if (pillSize.isEmpty()) {
        pillSize = QSizeF(60.0, 18.0);
    }
    const qreal halfW = pillSize.width() / 2.0;
    const qreal halfH = pillSize.height() / 2.0;

    const bool hasSource = sourceRect.isValid() && !sourceRect.isNull();
    const bool hasTarget = targetRect.isValid() && !targetRect.isNull();

    if (!hasSource && !hasTarget) {
        return pillCenter;
    }
    if (hasSource && !hasTarget) {
        return clampAgainstNode(pillCenter, halfW, halfH, sourceRect);
    }
    if (!hasSource && hasTarget) {
        return clampAgainstNode(pillCenter, halfW, halfH, targetRect);
    }

    // A tight corridor only binds a pill squeezed between the two nodes (its span
    // on the other axis reaches into both keepout bands); a pill clear of them
    // moves freely, or a horizontal drag would stand still then jump.
    auto reachesY = [&](const QRectF& node) {
        return pillCenter.y() + halfH > node.top() - kPillNodeKeepout &&
               pillCenter.y() - halfH < node.bottom() + kPillNodeKeepout;
    };
    auto reachesX = [&](const QRectF& node) {
        return pillCenter.x() + halfW > node.left() - kPillNodeKeepout &&
               pillCenter.x() - halfW < node.right() + kPillNodeKeepout;
    };
    const bool squeezedAlongX = reachesY(sourceRect) && reachesY(targetRect);
    const bool squeezedAlongY = reachesX(sourceRect) && reachesX(targetRect);

    // Tight corridor along X.
    const bool sLeftOfT = sourceRect.right() <= targetRect.left();
    const bool tLeftOfS = targetRect.right() <= sourceRect.left();
    if (!squeezedAlongX) {
        // not between the nodes on this axis: no corridor to center in
    } else if (sLeftOfT) {
        const qreal gapX = targetRect.left() - sourceRect.right();
        if (gapX < pillSize.width() + 2.0 * kPillNodeKeepout) {
            if (pillCenter.x() >= sourceRect.right() && pillCenter.x() <= targetRect.left()) {
                pillCenter.setX((sourceRect.right() + targetRect.left()) / 2.0);
            }
        }
    } else if (tLeftOfS) {
        const qreal gapX = sourceRect.left() - targetRect.right();
        if (gapX < pillSize.width() + 2.0 * kPillNodeKeepout) {
            if (pillCenter.x() >= targetRect.right() && pillCenter.x() <= sourceRect.left()) {
                pillCenter.setX((targetRect.right() + sourceRect.left()) / 2.0);
            }
        }
    }

    // Tight corridor along Y.
    const bool sAboveT = sourceRect.bottom() <= targetRect.top();
    const bool tAboveS = targetRect.bottom() <= sourceRect.top();
    if (!squeezedAlongY) {
        // not between the nodes on this axis: no corridor to center in
    } else if (sAboveT) {
        const qreal gapY = targetRect.top() - sourceRect.bottom();
        if (gapY < pillSize.height() + 2.0 * kPillNodeKeepout) {
            if (pillCenter.y() >= sourceRect.bottom() && pillCenter.y() <= targetRect.top()) {
                pillCenter.setY((sourceRect.bottom() + targetRect.top()) / 2.0);
            }
        }
    } else if (tAboveS) {
        const qreal gapY = sourceRect.top() - targetRect.bottom();
        if (gapY < pillSize.height() + 2.0 * kPillNodeKeepout) {
            if (pillCenter.y() >= targetRect.bottom() && pillCenter.y() <= sourceRect.top()) {
                pillCenter.setY((targetRect.bottom() + sourceRect.top()) / 2.0);
            }
        }
    }

    // Clamp against the closer node first, then the farther one.
    const qreal distS = (pillCenter - sourceRect.center()).manhattanLength();
    const qreal distT = (pillCenter - targetRect.center()).manhattanLength();
    if (distS <= distT) {
        pillCenter = clampAgainstNode(pillCenter, halfW, halfH, sourceRect);
        pillCenter = clampAgainstNode(pillCenter, halfW, halfH, targetRect);
    } else {
        pillCenter = clampAgainstNode(pillCenter, halfW, halfH, targetRect);
        pillCenter = clampAgainstNode(pillCenter, halfW, halfH, sourceRect);
    }

    return pillCenter;
}

}  // namespace app
