// --gui-probe canvas scenarios: root-level machine events, edge-style captures,
// soft-snap movement, lateral pill freedom, pill-port resolution, the machine
// frame hull, and rubber-band selection.

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHash>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPair>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointF>
#include <QRect>
#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <ordo/core/kernel.h>
#include <ordo/qt/view_host.h>

#include "controller/edit_commands.h"
#include "controller/sim_commands.h"
#include "controller/undo_capture.h"
#include "controller/undo_commands.h"
#include "infra/code_generator.h"
#include "infra/machine_validator.h"
#include "infra/project_io.h"
#include "infra/sim_clock.h"
#include "infra/xstate_v5_io.h"
#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/sim_agent.h"
#include "model/sim_events.h"
#include "model/undo_events.h"
#include "model/undo_store.h"
#include "view/canvas/canvas_presenter.h"
#include "view/canvas/canvas_view.h"
#include "view/generated/canvas_interaction_core.h"  // drives the machine directly
#include "view/shell/document_session.h"
#include "view/shell/editor_view.h"
#include "view/shell/inspector_panel.h"
#include "view/shell/main_window.h"
#include "view/items/machine_frame_item.h"
#include "view/shell/minimap_view.h"
#include "view/items/note_item.h"
#include "view/geometry/node_anchors.h"       // sideFor: the RULE a unified edge's node ends obey
#include "view/geometry/pill_port_resolver.h"  // asserts the rule table 1:1
#include "view/items/state_item.h"
#include "view/shell/theme.h"
#include "view/shell/trace_panel.h"
#include "view/items/transition_item.h"

#include "harness/harness.h"
#include "harness/probe_scenarios.h"

// Unified-wire invariants: a Normal transition is one Manhattan polyline from
// node anchor to node anchor (canonicalWaypoints(debugRoute())), the pill a
// pure annotation. 1) axis-aligned, no zero-length segment; 2) at most 4
// points and no U-turn wrap; 3) the run leaves and arrives along the node
// sides' outward normals, anchors on the node borders; 4) labelAnchor ==
// labelBase + labelOffset, labelBase on the wire.
namespace {

// edge_router.cpp's offsets, restated because they are file-local there: a
// wire starts kProbeStubGap off the source rect and stops kProbeStubGap +
// kProbeArrowApproach off the target's, the arrowhead supplying the rest.
constexpr qreal kProbeStubGap = 5.0;
constexpr qreal kProbeArrowApproach = 10.0;

// What one Normal edge's canonical polyline says about itself.
struct UnifiedWire {
    QVector<QPointF> points;
    QPointF departure;     // unit tangent leaving the source node
    QPointF arrival;       // unit tangent entering the target node
    QPointF sourceAnchor;  // where the wire meets the source node's border
    QPointF targetAnchor;  // where its arrowhead lands on the target's
};

QPointF unitStep(QPointF from, QPointF to) {
    const QPointF d = to - from;
    const qreal norm = std::hypot(d.x(), d.y());
    return norm > 1e-6 ? d / norm : QPointF();
}

// The rect side whose outward normal is `normal` (an axis unit vector once
// the orthogonality check has passed).
app::PortSide sideOfOutwardNormal(QPointF normal) {
    if (normal.x() > 0.5) {
        return app::PortSide::Right;
    }
    if (normal.x() < -0.5) {
        return app::PortSide::Left;
    }
    if (normal.y() > 0.5) {
        return app::PortSide::Bottom;
    }
    return app::PortSide::Top;
}

bool pointOnRectSide(const QRectF& rect, app::PortSide side, QPointF p) {
    switch (side) {
        case app::PortSide::Top:
            return std::abs(p.y() - rect.top()) < 0.01 && p.x() >= rect.left() - 0.01 &&
                   p.x() <= rect.right() + 0.01;
        case app::PortSide::Bottom:
            return std::abs(p.y() - rect.bottom()) < 0.01 && p.x() >= rect.left() - 0.01 &&
                   p.x() <= rect.right() + 0.01;
        case app::PortSide::Left:
            return std::abs(p.x() - rect.left()) < 0.01 && p.y() >= rect.top() - 0.01 &&
                   p.y() <= rect.bottom() + 0.01;
        case app::PortSide::Right:
            return std::abs(p.x() - rect.right()) < 0.01 && p.y() >= rect.top() - 0.01 &&
                   p.y() <= rect.bottom() + 0.01;
    }
    return false;
}

void dumpWaypoints(const QVector<QPointF>& points) {
    for (const QPointF& pt : points) {
        std::fprintf(stderr, "  wp (%.1f, %.1f)\n", pt.x(), pt.y());
    }
}

qreal distanceToPolyline(const QVector<QPointF>& points, QPointF p) {
    qreal best = 1.0e9;
    for (int i = 0; i + 1 < points.size(); ++i) {
        const QPointF a = points[i];
        const QPointF ab = points[i + 1] - a;
        const qreal len2 = QPointF::dotProduct(ab, ab);
        const qreal t = len2 > 1e-9 ? std::clamp(QPointF::dotProduct(p - a, ab) / len2, 0.0, 1.0) : 0.0;
        const QPointF proj = a + ab * t;
        best = std::min(best, std::hypot(p.x() - proj.x(), p.y() - proj.y()));
    }
    return best;
}

// Invariants 1 and 2, on the skeleton alone. Fills `out` with the ends the node
// checks below need, derived from the drawn geometry.
bool unifiedWireHolds(const app::RoutedEdge& route, const char* what, UnifiedWire* out) {
    const QVector<QPointF> points = app::canonicalWaypoints(route);
    if (points.size() < 2) {
        std::fprintf(stderr, "FAIL: %s has no unified wire (%d waypoint(s))\n", what,
                     static_cast<int>(points.size()));
        return false;
    }
    if (points.size() > 4) {
        std::fprintf(stderr, "FAIL: %s bends more than twice (%d waypoints, want <= 4 -- straight, L or Z)\n", what,
                     static_cast<int>(points.size()));
        dumpWaypoints(points);
        return false;
    }
    QVector<QPointF> directions;
    for (int i = 0; i + 1 < points.size(); ++i) {
        const qreal dx = points[i + 1].x() - points[i].x();
        const qreal dy = points[i + 1].y() - points[i].y();
        if (std::abs(dx) > 0.01 && std::abs(dy) > 0.01) {
            std::fprintf(stderr, "FAIL: %s segment %d is diagonal (dx=%.2f dy=%.2f)\n", what, i, dx, dy);
            dumpWaypoints(points);
            return false;
        }
        if (std::hypot(dx, dy) <= 0.01) {
            std::fprintf(stderr, "FAIL: %s kept a zero-length segment at %d through the collapse\n", what, i);
            dumpWaypoints(points);
            return false;
        }
        directions.push_back(unitStep(points[i], points[i + 1]));
    }
    for (int i = 1; i < directions.size(); ++i) {
        if (QPointF::dotProduct(directions[i - 1], directions[i]) < -0.99) {
            std::fprintf(stderr, "FAIL: %s wraps back on itself at segment %d (a U-turn, not a corner)\n", what, i);
            dumpWaypoints(points);
            return false;
        }
    }
    out->points = points;
    out->departure = directions.front();
    out->arrival = directions.back();
    out->sourceAnchor = points.front() - directions.front() * kProbeStubGap;
    out->targetAnchor = points.back() + directions.back() * (kProbeStubGap + kProbeArrowApproach);
    return true;
}

// Invariant 3: both ends belong to their own node. The skeleton is already
// orthogonal, so an anchor on the side its tangent names proves perpendicular
// contact at that end.
bool unifiedWireMeetsNodes(const UnifiedWire& wire, const QRectF& sourceRect, const QRectF& targetRect,
                           const char* what) {
    const app::PortSide sourceSide = sideOfOutwardNormal(wire.departure);
    const app::PortSide targetSide = sideOfOutwardNormal(-wire.arrival);
    if (!pointOnRectSide(sourceRect, sourceSide, wire.sourceAnchor)) {
        std::fprintf(stderr, "FAIL: %s does not leave the source node's own border (anchor %.1f, %.1f side %d)\n",
                     what, wire.sourceAnchor.x(), wire.sourceAnchor.y(), static_cast<int>(sourceSide));
        return false;
    }
    if (!pointOnRectSide(targetRect, targetSide, wire.targetAnchor)) {
        std::fprintf(stderr, "FAIL: %s does not arrive on the target node's own border (anchor %.1f, %.1f side %d)\n",
                     what, wire.targetAnchor.x(), wire.targetAnchor.y(), static_cast<int>(targetSide));
        return false;
    }
    return true;
}

// Invariant 4: the pill sits on the wire, displaced by exactly the persisted
// offset. Auto-routed edges only: an authored (bendpointed) course pins its
// label onto the skeleton instead.
bool unifiedLabelLawHolds(const app::RoutedEdge& route, QPointF labelOffset, const char* what) {
    const QPointF want = route.labelBase + labelOffset;
    if (std::hypot(route.labelAnchor.x() - want.x(), route.labelAnchor.y() - want.y()) > 0.01) {
        std::fprintf(stderr, "FAIL: %s pill is not its wire's own base plus the persisted offset (%.1f, %.1f vs %.1f, %.1f)\n",
                     what, route.labelAnchor.x(), route.labelAnchor.y(), want.x(), want.y());
        return false;
    }
    const QVector<QPointF> points = app::canonicalWaypoints(route);
    const qreal off = distanceToPolyline(points, route.labelBase);
    if (off > 0.5) {
        std::fprintf(stderr, "FAIL: %s label base is not on its own wire (%.2f px off)\n", what, off);
        dumpWaypoints(points);
        return false;
    }
    return true;
}

}  // namespace

// Scenario "root-events": the machine frame hull plus a machine-level event end
// to end: add (as the context menu's "Add Machine Event" does), name it, assert
// the document and the frame-anchored pill, prove the Simulate fallback fires
// it, then undo.
int runRootEventsScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    if (doc == nullptr || sim == nullptr || presenter == nullptr) {
        std::fprintf(stderr, "FAIL: root-events scenario preconditions (doc/sim/presenter) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);

    bool frameFound = false;
    for (QGraphicsItem* item : loginPane->canvasView()->scene()->items()) {
        frameFound = frameFound || (item->type() == app::MachineFrameItem::Type && item->isVisible());
    }
    if (!frameFound) {
        std::fprintf(stderr, "FAIL: no visible MachineFrameItem in the login-flow scene\n");
        return 1;
    }

    kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 0});
    QApplication::processEvents();
    if (doc->machine().transitions.isEmpty() || doc->machine().transitions.last().from != 0) {
        std::fprintf(stderr, "FAIL: Add Machine Event's intent did not append a root transition\n");
        return 1;
    }
    const quint64 rootId = doc->machine().transitions.last().id;
    kernel.send(app::events::SetTransitionEventRequested{.id = rootId, .event = QStringLiteral("PANIC")});
    QApplication::processEvents();
    if (doc->findTransition(rootId)->event != QStringLiteral("PANIC") ||
        !presenter->debugHasTransitionVisual(rootId)) {
        std::fprintf(stderr, "FAIL: the root event did not land in the document + scene\n");
        return 1;
    }
    if (!saveSceneCapture(loginPane, "probe-root-events")) {
        return 1;
    }

    // Simulate: the machine-level fallback catches PANIC from the initial
    // state (no login-flow state handles it).
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("PANIC")});
    QApplication::processEvents();
    if (sim->firedTransitionIds().empty() || sim->firedTransitionIds().back() != rootId) {
        std::fprintf(stderr, "FAIL: Simulate did not fire the root PANIC handler\n");
        return 1;
    }
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();

    kernel.send(app::events::UndoRequested{});  // the event name
    kernel.send(app::events::UndoRequested{});  // the add
    QApplication::processEvents();
    if (doc->findTransition(rootId) != nullptr || presenter->debugHasTransitionVisual(rootId)) {
        std::fprintf(stderr, "FAIL: undo x2 did not remove the root event\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario root-events (frame hull, add/name, Simulate fallback, undo)\n");
    return 0;
}

// Scenario "edge-style-gate": renders the login-flow machine once per edge
// style and captures each for visual review; no geometric assertion. Restores
// the default style before leaving.
int runEdgeStyleGateScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    window.debugFocusView(loginPane);
    struct Candidate {
        app::EdgeStyle style;
        const char* fileName;
    };
    const Candidate candidates[] = {
        {app::EdgeStyle::SmallFillet, "probe-edge-style-1-small-fillet"},
        {app::EdgeStyle::LargeFillet, "probe-edge-style-2-large-fillet"},
        {app::EdgeStyle::Bezier, "probe-edge-style-3-bezier"},
    };
    int exitCode = 0;
    for (const Candidate& candidate : candidates) {
        app::setEdgeStyle(candidate.style);
        kernel.send(app::events::MachineSnapshotRequested{});  // full rebuild -> every edge re-renders
        QApplication::processEvents();
        if (!saveSceneCapture(loginPane, candidate.fileName)) {
            exitCode = 1;
        }
    }
    app::setEdgeStyle(app::EdgeStyle::SmallFillet);
    kernel.send(app::events::MachineSnapshotRequested{});
    QApplication::processEvents();
    if (exitCode == 0) {
        std::printf("PASS: gui-probe scenario edge-style-gate (three candidates captured for the wow-gate)\n");
    }
    return exitCode;
}
// Scenario "soft-snap": node movement is free with a near-grid magnet; an
// off-grid coordinate survives while a near-grid one snaps.
int runSoftSnapScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    window.debugFocusView(loginPane);
    app::StateItem* item = findStateItemById(loginPane->canvasView()->scene(), 1);
    if (item == nullptr) {
        std::fprintf(stderr, "FAIL: soft-snap scenario found no StateItem 1\n");
        return 1;
    }
    app::CanvasView* view = loginPane->canvasView();
    const QPointF original = item->pos();
    // Must be a real drag: a programmatic setPos does not snap (see
    // StateItem::itemChange). Target: x 5.7px off the 96 line (stays free), y
    // 3.2px off (snaps).
    const QPointF target(101.7, 99.2);
    view->debugMousePress(item->sceneRect().center());
    view->debugMouseMove(item->sceneRect().center() + (target - original));
    view->debugMouseRelease(item->sceneRect().center() + (target - original));
    QApplication::processEvents();
    // y lands exactly on the grid line (the magnet); x only has to stay off it
    // and near the target: the synthetic move rounds through integer viewport
    // pixels, so do not assert byte-equality against the requested delta.
    if (item->pos().y() != 96.0) {
        std::fprintf(stderr, "FAIL: soft snap did not magnetise y (pos %.2f, %.2f; want y 96.0)\n", item->pos().x(),
                     item->pos().y());
        return 1;
    }
    const qreal nearestGridX = std::round(item->pos().x() / 24.0) * 24.0;
    if (std::abs(item->pos().x() - nearestGridX) <= 4.0 || std::abs(item->pos().x() - target.x()) > 1.5) {
        std::fprintf(stderr, "FAIL: soft snap did not leave x free (pos %.2f, want ~%.1f and off-grid)\n",
                     item->pos().x(), target.x());
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (item->pos() != original) {
        std::fprintf(stderr, "FAIL: one undo did not restore the soft-snap scenario's node\n");
        return 1;
    }
    std::printf("PASS: gui-probe scenario soft-snap (off-grid survives, near-grid snaps, one undo restores)\n");
    return 0;
}

// Scenario "edge-freedom": pills move freely. The near-vertical Failure edge
// takes a lateral offset and lands exactly there; a fresh targetless stub
// follows its pill into an L; a fresh self-loop re-hangs around its dragged
// pill; undo restores it all.
int runEdgeFreedomScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    if (doc == nullptr || presenter == nullptr) {
        std::fprintf(stderr, "FAIL: edge-freedom scenario preconditions (doc/presenter) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);

    // ---- lateral freedom on the near-vertical Failure edge (id 7) ------------
    const QPointF before = presenter->debugLabelCenter(7);
    kernel.send(app::events::MoveTransitionLabelRequested{.id = 7, .offset = QPointF(90.0, 10.0)});
    QApplication::processEvents();
    const QPointF pill = presenter->debugLabelCenter(7);
    const QPointF want = before + QPointF(90.0, 10.0);
    if (std::hypot(pill.x() - want.x(), pill.y() - want.y()) > 0.5) {
        std::fprintf(stderr, "FAIL: lateral pill offset did not land where dragged (%.1f, %.1f vs %.1f, %.1f)\n",
                     pill.x(), pill.y(), want.x(), want.y());
        return 1;
    }
    const app::TransitionItem* failureEdge = presenter->debugTransitionItem(7);
    if (failureEdge == nullptr || pill != failureEdge->debugRoute().labelAnchor ||
        failureEdge->debugRoute().sourceHalf.isEmpty() || failureEdge->debugRoute().targetHalf.isEmpty()) {
        std::fprintf(stderr, "FAIL: the laterally dragged pill is not pinned on a live route\n");
        return 1;
    }

    // ---- a fresh targetless stub follows its pill anywhere --------------------
    kernel.send(app::events::AddTransitionRequested{.from = 4, .to = 0});
    QApplication::processEvents();
    const quint64 stubId = doc->machine().transitions.last().id;
    const QPointF stubBase = presenter->debugLabelCenter(stubId);
    kernel.send(app::events::SetTransitionEventRequested{.id = stubId, .event = QStringLiteral("NOTE")});
    kernel.send(app::events::MoveTransitionLabelRequested{.id = stubId, .offset = QPointF(150.0, -30.0)});
    QApplication::processEvents();
    if (presenter->debugLabelCenter(stubId) != stubBase + QPointF(150.0, -30.0)) {
        std::fprintf(stderr, "FAIL: the targetless pill did not follow its drag (want base + 150,-30)\n");
        return 1;
    }

    // ---- a fresh self-loop re-hangs around its dragged pill -------------------
    kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 3});
    QApplication::processEvents();
    const quint64 selfId = doc->machine().transitions.last().id;
    const QPointF selfBase = presenter->debugLabelCenter(selfId);
    kernel.send(app::events::SetTransitionEventRequested{.id = selfId, .event = QStringLiteral("RETRY")});
    // Generous offset: near the boundary the route is so short that the
    // label-gap end clamps legitimately dominate the pin.
    kernel.send(app::events::MoveTransitionLabelRequested{.id = selfId, .offset = QPointF(0.0, 160.0)});
    QApplication::processEvents();
    const QPointF selfPill = presenter->debugLabelCenter(selfId);
    const QPointF selfWant = selfBase + QPointF(0.0, 160.0);
    const app::TransitionItem* selfEdge = presenter->debugTransitionItem(selfId);
    if (std::hypot(selfPill.x() - selfWant.x(), selfPill.y() - selfWant.y()) > 0.5 || selfEdge == nullptr ||
        selfEdge->debugRoute().arrowhead.isEmpty()) {
        std::fprintf(stderr, "FAIL: the self-loop did not re-hang around its dragged pill\n");
        return 1;
    }
    if (!saveSceneCapture(loginPane, "probe-edge-freedom")) {
        return 1;
    }

    // ---- undo x7 restores the pristine machine --------------------------------
    for (int i = 0; i < 7; ++i) {
        kernel.send(app::events::UndoRequested{});
    }
    QApplication::processEvents();
    if (doc->findTransition(stubId) != nullptr || doc->findTransition(selfId) != nullptr ||
        doc->findTransition(7)->labelOffset != QPointF()) {
        std::fprintf(stderr, "FAIL: undo x7 did not restore the pre-scenario machine\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario edge-freedom (lateral pill, targetless L, self re-hang, undo)\n");
    return 0;
}

// Scenario "pill-ports": edge geometry under adversarial pill positions on the
// real canvas, with expectations re-derived from public geometry rather than
// read back from the presenter. Self/Targetless/Root route through the pill
// (sections (d)-(f), via resolvePillPorts + checkAttachment); a Normal
// transition has no pill ports and is held to the unified invariants above.
int runPillPortsScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    QGraphicsScene* scene = loginPane->canvasView() != nullptr ? loginPane->canvasView()->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: pill-ports scenario preconditions (doc/presenter/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);

    const auto rectOf = [&](quint64 stateId) {
        app::StateItem* item = findStateItemById(scene, stateId);
        return item != nullptr ? item->sceneRect() : QRectF();
    };
    // Puts a pill at an absolute scene point by offsetting from its current
    // base anchor, so it survives the base moving with a node.
    const auto placePill = [&](quint64 id, QPointF wanted) {
        const app::Transition* transition = doc->findTransition(id);
        if (transition == nullptr) {
            return;
        }
        const QPointF base = presenter->debugLabelCenter(id) - transition->labelOffset;
        kernel.send(app::events::MoveTransitionLabelRequested{.id = id, .offset = wanted - base});
        QApplication::processEvents();
    };
    // The drawn path's tangent at an end, sampled 2px in (the last stretch
    // before a port is never filleted).
    const auto tangentAt = [](const QPainterPath& path, bool atEnd) {
        const qreal length = path.length();
        if (length < 3.0) {
            return QPointF();
        }
        const qreal step = 2.0 / length;
        const QPointF a = path.pointAtPercent(atEnd ? 1.0 - step : 0.0);
        const QPointF b = path.pointAtPercent(atEnd ? 1.0 : step);
        const QPointF d = b - a;
        const qreal norm = std::hypot(d.x(), d.y());
        return norm > 1e-6 ? d / norm : QPointF();
    };

    // The pill-port invariant on one live transition: re-resolve, then hold the
    // drawn route to it. Sections (d)-(f) only; a Normal edge has no ports and
    // goes through checkUnifiedEdge below.
    const auto checkAttachment = [&](quint64 id, app::EdgeClass cls, const QRectF& sourceRect,
                                      const QRectF& targetRect, const char* what) -> std::optional<app::PortAssignment> {
        const app::TransitionItem* edge = presenter->debugTransitionItem(id);
        const QRectF pillRect = presenter->debugPillRect(id);
        if (edge == nullptr || pillRect.isNull()) {
            std::fprintf(stderr, "FAIL: %s has no edge/pill visual\n", what);
            return std::nullopt;
        }
        const app::PortAssignment ports =
            app::resolvePillPorts(cls, sourceRect, targetRect, pillRect, std::nullopt);
        if (!portAssignmentHoldsInvariant(ports, pillRect, what)) {
            return std::nullopt;
        }
        const app::RoutedEdge& route = edge->debugRoute();
        if (route.labelAnchor != pillRect.center()) {
            std::fprintf(stderr, "FAIL: %s pill is not pinned on its own route anchor\n", what);
            return std::nullopt;
        }
        const auto near = [](QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()) <= 0.01; };
        if (ports.hasEntry) {
            if (route.sourceHalf.isEmpty() || !near(route.sourceHalf.currentPosition(), ports.entryPoint)) {
                std::fprintf(stderr, "FAIL: %s source half does not end AT the pill's entry port\n", what);
                return std::nullopt;
            }
            const QPointF want = -app::outwardNormal(ports.entrySide);
            if (QPointF::dotProduct(tangentAt(route.sourceHalf, /*atEnd=*/true), want) < 0.99) {
                std::fprintf(stderr, "FAIL: %s source half does not arrive perpendicular to its entry side\n", what);
                return std::nullopt;
            }
        } else if (!route.sourceHalf.isEmpty()) {
            std::fprintf(stderr, "FAIL: %s drew a source half it has no entry port for\n", what);
            return std::nullopt;
        }
        if (ports.hasExit) {
            const QPointF drawnStart(route.targetHalf.elementAt(0).x, route.targetHalf.elementAt(0).y);
            if (route.targetHalf.isEmpty() || !near(drawnStart, ports.exitPoint)) {
                std::fprintf(stderr, "FAIL: %s target half does not start AT the pill's exit port\n", what);
                return std::nullopt;
            }
            const QPointF want = app::outwardNormal(ports.exitSide);
            if (QPointF::dotProduct(tangentAt(route.targetHalf, /*atEnd=*/false), want) < 0.99) {
                std::fprintf(stderr, "FAIL: %s target half does not leave perpendicular to its exit side\n", what);
                return std::nullopt;
            }
        } else if (!route.targetHalf.isEmpty() || !route.arrowhead.isEmpty()) {
            std::fprintf(stderr, "FAIL: %s drew a target half/arrowhead it has no exit port for\n", what);
            return std::nullopt;
        }
        return ports;
    };
    const auto routeRegion = [&](quint64 id) {
        const app::RoutedEdge& route = presenter->debugTransitionItem(id)->debugRoute();
        QRectF region = presenter->debugPillRect(id);
        if (!route.sourceHalf.isEmpty()) {
            region = region.united(route.sourceHalf.boundingRect());
        }
        if (!route.targetHalf.isEmpty()) {
            region = region.united(route.targetHalf.boundingRect());
        }
        return region.adjusted(-14.0, -14.0, 14.0, 14.0);
    };

    const QPointF state2Origin = doc->findState(2) != nullptr ? doc->findState(2)->pos : QPointF();
    const QPointF state4Origin = doc->findState(4) != nullptr ? doc->findState(4)->pos : QPointF();
    const QPointF offset6 = doc->findTransition(6)->labelOffset;
    const QPointF offset7 = doc->findTransition(7)->labelOffset;

    // The unified wire of one live Normal transition, held to the skeleton law,
    // both ends on their node's border and, unless waived for an authored
    // course, the label law against the persisted offset.
    const auto checkUnifiedEdge = [&](quint64 id, const QRectF& sourceRect, const QRectF& targetRect,
                                       const char* what) -> std::optional<UnifiedWire> {
        const app::TransitionItem* edge = presenter->debugTransitionItem(id);
        const app::Transition* transition = doc->findTransition(id);
        if (edge == nullptr || transition == nullptr) {
            std::fprintf(stderr, "FAIL: %s has no edge visual/document row\n", what);
            return std::nullopt;
        }
        UnifiedWire wire;
        if (!unifiedWireHolds(edge->debugRoute(), what, &wire) ||
            !unifiedWireMeetsNodes(wire, sourceRect, targetRect, what) ||
            !unifiedLabelLawHolds(edge->debugRoute(), transition->labelOffset, what)) {
            return std::nullopt;
        }
        return wire;
    };

    // ---- (a) the offset-free pill sits ON its wire ---------------------------
    // Authenticating -Failure-> Error with no persisted offset: one Manhattan
    // run node border to node border, and a label anchored exactly on it.
    kernel.send(app::events::MoveTransitionLabelRequested{.id = 7, .offset = QPointF()});
    QApplication::processEvents();
    {
        const auto wire = checkUnifiedEdge(7, rectOf(2), rectOf(4), "the offset-free Failure edge");
        if (!wire.has_value()) {
            return 1;
        }
        const app::RoutedEdge& route = presenter->debugTransitionItem(7)->debugRoute();
        if (std::hypot(route.labelAnchor.x() - route.labelBase.x(),
                        route.labelAnchor.y() - route.labelBase.y()) > 0.01) {
            std::fprintf(stderr, "FAIL: an offset-free Failure pill does not sit on its own wire base\n");
            return 1;
        }
        if (!saveSceneRegionCapture(loginPane, routeRegion(7), "probe-pill-ports-vertical")) {
            return 1;
        }
    }

    // ---- (b) a lateral pill move moves the LABEL and nothing else ------------
    // The pill is a pure annotation: the skeleton must come back
    // byte-identical while the anchor tracks the offset exactly.
    {
        const app::TransitionItem* edge = presenter->debugTransitionItem(7);
        const QVector<QPointF> before = app::canonicalWaypoints(edge->debugRoute());
        const QPointF baseBefore = edge->debugRoute().labelBase;
        placePill(7, presenter->debugLabelCenter(7) + QPointF(150.0, 0.0));
        const auto wire = checkUnifiedEdge(7, rectOf(2), rectOf(4), "the laterally moved Failure pill");
        if (!wire.has_value()) {
            return 1;
        }
        if (wire->points != before) {
            std::fprintf(stderr, "FAIL: moving the Failure pill 150px sideways moved its wire too\n");
            dumpWaypoints(before);
            dumpWaypoints(wire->points);
            return 1;
        }
        const app::Transition* moved = doc->findTransition(7);
        const QPointF anchor = presenter->debugTransitionItem(7)->debugRoute().labelAnchor;
        if (moved == nullptr || std::hypot(anchor.x() - (baseBefore.x() + moved->labelOffset.x()),
                                            anchor.y() - (baseBefore.y() + moved->labelOffset.y())) > 0.01) {
            std::fprintf(stderr, "FAIL: the Failure pill did not track its own persisted offset off a fixed base\n");
            return 1;
        }
    }

    // ---- (c) a node move DOES reroute: the wire follows its endpoints --------
    // Converse of (b). Authenticating goes straight up, keeping every gap to
    // its neighbours over the 20px a stub plus arrow approach needs; closer
    // than that orthogonalPortRun wraps, which is section (g)'s territory.
    {
        const app::TransitionItem* edge = presenter->debugTransitionItem(6);
        const QVector<QPointF> before = app::canonicalWaypoints(edge->debugRoute());
        kernel.send(app::events::MoveStateRequested{.id = 2, .pos = QPointF(320.0, 90.0)});
        QApplication::processEvents();
        const auto wire = checkUnifiedEdge(6, rectOf(2), rectOf(3), "the Success edge after its source moved");
        if (!wire.has_value()) {
            return 1;
        }
        if (wire->points == before) {
            std::fprintf(stderr, "FAIL: moving Authenticating did not reroute the Success wire\n");
            return 1;
        }
        if (!saveSceneRegionCapture(loginPane, routeRegion(6), "probe-pill-ports-node-moved")) {
            return 1;
        }
    }
    kernel.send(app::events::MoveStateRequested{.id = 2, .pos = state2Origin});
    QApplication::processEvents();
    {
        const auto wire = checkUnifiedEdge(6, rectOf(2), rectOf(3), "the Success edge back at its origin");
        if (!wire.has_value()) {
            return 1;
        }
        if (!saveSceneRegionCapture(loginPane, routeRegion(6), "probe-pill-ports-node-restored")) {
            return 1;
        }
    }

    // ---- (d) a self transition's straddled pair ------------------------------
    kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 3});
    QApplication::processEvents();
    const quint64 selfId = doc->machine().transitions.last().id;
    kernel.send(app::events::SetTransitionEventRequested{.id = selfId, .event = QStringLiteral("PING")});
    QApplication::processEvents();
    placePill(selfId, QPointF(rectOf(3).right() + 220.0, rectOf(3).center().y()));
    {
        const auto ports = checkAttachment(selfId, app::EdgeClass::Self, rectOf(3), rectOf(3), "the self pair");
        if (!ports.has_value()) {
            return 1;
        }
        const QPointF middle = app::sidePortAnchor(presenter->debugPillRect(selfId), ports->entrySide);
        if (ports->entrySide != ports->exitSide || (ports->entryPoint + ports->exitPoint) / 2.0 != middle) {
            std::fprintf(stderr, "FAIL: the self pair does not straddle one side's middle\n");
            return 1;
        }
        if (!saveSceneRegionCapture(loginPane, routeRegion(selfId), "probe-pill-ports-self")) {
            return 1;
        }
    }

    // ---- (e) a targetless stub: the pill terminates the edge -----------------
    kernel.send(app::events::AddTransitionRequested{.from = 4, .to = 0});
    QApplication::processEvents();
    const quint64 stubId = doc->machine().transitions.last().id;
    kernel.send(app::events::SetTransitionEventRequested{.id = stubId, .event = QStringLiteral("AUDIT")});
    QApplication::processEvents();
    placePill(stubId, QPointF(rectOf(4).left() - 150.0, rectOf(4).center().y() - 20.0));
    {
        const auto ports = checkAttachment(stubId, app::EdgeClass::Targetless, rectOf(4), QRectF(), "the bent stub");
        if (!ports.has_value()) {
            return 1;
        }
        if (ports->hasExit || ports->entrySide != app::PortSide::Right ||
            ports->sourceShape == app::HalfShape::Straight) {
            std::fprintf(stderr, "FAIL: the targetless stub did not bend into the pill's facing side port\n");
            return 1;
        }
    }

    // ---- (f) the root form keeps its frame-right anchoring -------------------
    kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 0});
    QApplication::processEvents();
    const quint64 rootId = doc->machine().transitions.last().id;
    kernel.send(app::events::SetTransitionEventRequested{.id = rootId, .event = QStringLiteral("PANIC")});
    QApplication::processEvents();
    {
        QRectF frame;
        for (QGraphicsItem* item : scene->items()) {
            if (item->type() == app::MachineFrameItem::Type) {
                frame = static_cast<app::MachineFrameItem*>(item)->sceneFrameRect();
            }
        }
        const QPointF edgePoint(frame.right(), frame.top() + 64.0);
        const auto ports = checkAttachment(rootId, app::EdgeClass::Root, QRectF(),
                                            QRectF(edgePoint, QSizeF(0.0, 0.0)), "the root pill");
        if (!ports.has_value()) {
            return 1;
        }
        if (ports->hasEntry || ports->exitSide != app::PortSide::Left) {
            std::fprintf(stderr, "FAIL: the root pill does not exit toward the frame edge\n");
            return 1;
        }
    }

    // ---- (g) endpoints dragged close together: the corner bound holds --------
    // Pins against the 6-point 'ㄹ' wrap: drag Error up against Authenticating
    // and hold the wire to the corner bound and the no-U-turn rule, first
    // beside the source and then tucked diagonally behind its departure side.
    {
        const QRectF authRect = rectOf(2);
        kernel.send(app::events::MoveStateRequested{
            .id = 4, .pos = QPointF(authRect.right() + 26.0, authRect.bottom() + 26.0)});
        QApplication::processEvents();
        if (!checkUnifiedEdge(7, rectOf(2), rectOf(4), "the close-proximity Failure edge").has_value()) {
            return 1;
        }
        if (!saveSceneRegionCapture(loginPane, routeRegion(7), "probe-pill-close-proximity")) {
            return 1;
        }

        kernel.send(app::events::MoveStateRequested{
            .id = 4, .pos = QPointF(authRect.left() - rectOf(4).width() - 26.0, authRect.top() - 26.0)});
        QApplication::processEvents();
        if (!checkUnifiedEdge(7, rectOf(2), rectOf(4), "the behind-the-source Failure edge").has_value()) {
            return 1;
        }

        kernel.send(app::events::MoveStateRequested{.id = 4, .pos = state4Origin});
        QApplication::processEvents();
    }

    // ---- (h) a pill parked beside its source cannot curl the wire ------------
    // Parking the pill anywhere leaves the skeleton untouched, so the wire
    // cannot curl.
    {
        const app::TransitionItem* successEdge = presenter->debugTransitionItem(6);
        if (successEdge == nullptr) {
            std::fprintf(stderr, "FAIL: curl-avoidance fixture missing the Success edge\n");
            return 1;
        }
        const QVector<QPointF> parked = app::canonicalWaypoints(successEdge->debugRoute());
        const QRectF authRect = rectOf(2);
        const QPointF curlSpots[] = {
            QPointF(authRect.left() - 45.0, authRect.center().y() - 20.0),  // beside the source's left
            QPointF(authRect.left() - 40.0, authRect.top() - 16.0),         // off its top-left corner
        };
        for (const QPointF& spot : curlSpots) {
            placePill(6, spot);
            const auto wire = checkUnifiedEdge(6, rectOf(2), rectOf(3), "the Success edge under a parked pill");
            if (!wire.has_value()) {
                return 1;
            }
            if (wire->points != parked) {
                std::fprintf(stderr, "FAIL: parking the Success pill at (%.0f, %.0f) moved its wire\n", spot.x(),
                             spot.y());
                dumpWaypoints(parked);
                dumpWaypoints(wire->points);
                return 1;
            }
        }
        if (!saveSceneRegionCapture(loginPane, routeRegion(6), "probe-pill-parked-beside-node")) {
            return 1;
        }
    }

    // ---- (i) the NODE pipeline picks the ends, not the pill ------------------
    // A unified wire's ends come from node_anchors.h's sideFor (center-to-center
    // dominance between the two nodes), so parking the pill far left of the
    // Authenticating<->Error corridor must leave both ends where that rule puts them.
    {
        const QRectF authRect = rectOf(2);
        const QRectF errRect = rectOf(4);
        if (authRect.isNull() || errRect.isNull()) {
            std::fprintf(stderr, "FAIL: node-anchor-rule fixture missing Authenticating/Error\n");
            return 1;
        }
        placePill(7, QPointF(authRect.left() - 70.0, (authRect.bottom() + errRect.top()) / 2.0));
        const auto wire = checkUnifiedEdge(7, rectOf(2), rectOf(4), "the far-left-parked Failure edge");
        if (!wire.has_value()) {
            return 1;
        }
        const app::PortSide wantSource = app::sideFor(authRect.center(), errRect.center());
        const app::PortSide wantTarget = app::sideFor(errRect.center(), authRect.center());
        if (sideOfOutwardNormal(wire->departure) != wantSource ||
            sideOfOutwardNormal(-wire->arrival) != wantTarget) {
            std::fprintf(stderr,
                         "FAIL: the Failure wire's ends followed the pill, not sideFor (got %d/%d, want %d/%d)\n",
                         static_cast<int>(sideOfOutwardNormal(wire->departure)),
                         static_cast<int>(sideOfOutwardNormal(-wire->arrival)), static_cast<int>(wantSource),
                         static_cast<int>(wantTarget));
            return 1;
        }
        if (!saveSceneRegionCapture(loginPane, routeRegion(7), "probe-pill-node-pipeline-ends")) {
            return 1;
        }
    }

    // ---- (j) close-proximity Z-runs without backtracking snake ---------------
    // Under 2*minRun (24px) of counterpart distance, orthogonalPortRun must
    // divide the available clearance into a monotonic 4-point Z, not fall back
    // to a 6-point backtracking snake.
    {
        const QVector<QPointF> horizRun = app::orthogonalPortRun(
            QPointF(100.0, 100.0), QPointF(1.0, 0.0), QPointF(115.0, 120.0), QPointF(1.0, 0.0), 12.0);
        if (horizRun.size() != 4) {
            std::fprintf(stderr, "FAIL: close-proximity horiz run produced %d points (expected 4-point Z)\n",
                         static_cast<int>(horizRun.size()));
            return 1;
        }
        for (int i = 0; i < horizRun.size() - 1; ++i) {
            if (horizRun[i].x() > horizRun[i + 1].x() + 0.01) {
                std::fprintf(stderr, "FAIL: close-proximity horiz run backtracks: [%.1f -> %.1f]\n",
                             horizRun[i].x(), horizRun[i + 1].x());
                return 1;
            }
        }

        const QVector<QPointF> vertRun = app::orthogonalPortRun(
            QPointF(200.0, 100.0), QPointF(0.0, 1.0), QPointF(220.0, 115.0), QPointF(0.0, 1.0), 12.0);
        if (vertRun.size() != 4) {
            std::fprintf(stderr, "FAIL: close-proximity vert run produced %d points (expected 4-point Z)\n",
                         static_cast<int>(vertRun.size()));
            return 1;
        }
        for (int i = 0; i < vertRun.size() - 1; ++i) {
            if (vertRun[i].y() > vertRun[i + 1].y() + 0.01) {
                std::fprintf(stderr, "FAIL: close-proximity vert run backtracks: [%.1f -> %.1f]\n",
                             vertRun[i].y(), vertRun[i + 1].y());
                return 1;
            }
        }
    }

    // ---- (k) the pill-to-node keepout is a DRAG clamp now --------------------
    // The gesture-side clamp (clampPillDragPosition) holds the pill
    // kPillNodeKeepout clear of a connected node; the wire must sit perfectly
    // still while it acts.
    {
        const QRectF liRect = rectOf(3);  // LoggedIn
        if (liRect.isNull() || presenter->debugTransitionItem(6) == nullptr) {
            std::fprintf(stderr, "FAIL: keepout-clamping fixture missing Success/LoggedIn\n");
            return 1;
        }
        const QVector<QPointF> beforeClamp =
            app::canonicalWaypoints(presenter->debugTransitionItem(6)->debugRoute());
        // Drag the Success pill straight above LoggedIn, one px INSIDE the
        // 20px keepout (y = liRect.top() - 19.0).
        const QPointF encroachingSpot(liRect.center().x(), liRect.top() - 19.0);
        presenter->debugLabelDragMoved(6, encroachingSpot);
        QApplication::processEvents();
        const QPointF actualCenter = presenter->debugLabelCenter(6);
        const qreal expectedMaxY = liRect.top() - app::kPillNodeKeepout - presenter->debugPillRect(6).height() / 2.0;
        if (actualCenter.y() > expectedMaxY + 0.5) {
            std::fprintf(stderr, "FAIL: pill-to-node keepout clamping failed (actual y=%.1f, expected max y=%.1f)\n",
                         actualCenter.y(), expectedMaxY);
            return 1;
        }
        const QVector<QPointF> duringClamp =
            app::canonicalWaypoints(presenter->debugTransitionItem(6)->debugRoute());
        if (duringClamp != beforeClamp) {
            std::fprintf(stderr, "FAIL: the clamped pill drag moved the Success wire mid-gesture\n");
            dumpWaypoints(beforeClamp);
            dumpWaypoints(duringClamp);
            return 1;
        }
        presenter->debugLabelDragFinished(6, encroachingSpot);
        QApplication::processEvents();
        if (!checkUnifiedEdge(6, rectOf(2), rectOf(3), "the Success edge after a clamped pill drop").has_value()) {
            return 1;
        }
        if (!saveSceneRegionCapture(loginPane, routeRegion(6), "probe-pill-keepout-clamp")) {
            return 1;
        }
    }

    // ---- (l) pill port-line snap ---------------------------------------------
    // Dragging a pill within kPillPortSnapThreshold (10.0px) of a connected
    // node's port line snaps the pill onto that line and shows the alignment
    // guide; the wire skeleton must not move at all.
    {
        const QRectF sRect = rectOf(2);  // Authenticating
        const QRectF tRect = rectOf(3);  // LoggedIn
        if (presenter->debugTransitionItem(6) == nullptr || sRect.isNull() || tRect.isNull()) {
            std::fprintf(stderr, "FAIL: port-line snap fixture missing Success/nodes\n");
            return 1;
        }
        const QVector<QPointF> beforeSnap =
            app::canonicalWaypoints(presenter->debugTransitionItem(6)->debugRoute());

        // Midpoint X between Authenticating and LoggedIn, Y near LoggedIn's center + 5.0px (within 10px snap)
        const qreal midX = (sRect.right() + tRect.left()) / 2.0;
        const qreal nearY = tRect.center().y() + 5.0;

        presenter->debugLabelDragMoved(6, QPointF(midX, nearY));
        QApplication::processEvents();

        if (!presenter->debugHorizontalAlignGuideVisible()) {
            std::fprintf(stderr, "FAIL: horizontal align guide not visible when pill dragged 5px off port line\n");
            return 1;
        }

        const QPointF snappedCenter = presenter->debugLabelCenter(6);
        if (std::abs(snappedCenter.y() - tRect.center().y()) > 0.5) {
            std::fprintf(stderr, "FAIL: pill did not snap to port line Y (actual y=%.1f, expected %.1f)\n",
                         snappedCenter.y(), tRect.center().y());
            return 1;
        }

        const QVector<QPointF> snappedWire =
            app::canonicalWaypoints(presenter->debugTransitionItem(6)->debugRoute());
        if (snappedWire != beforeSnap) {
            std::fprintf(stderr, "FAIL: the port-line snap moved the Success wire\n");
            dumpWaypoints(beforeSnap);
            dumpWaypoints(snappedWire);
            return 1;
        }

        if (!saveSceneRegionCapture(loginPane, routeRegion(6), "probe-pill-port-snap")) {
            return 1;
        }

        // Drag 25px away (beyond 10px snap threshold): guide should disappear
        presenter->debugLabelDragMoved(6, QPointF(midX, tRect.center().y() + 25.0));
        QApplication::processEvents();
        if (presenter->debugHorizontalAlignGuideVisible()) {
            std::fprintf(stderr, "FAIL: horizontal align guide still visible when pill dragged 25px off port line\n");
            return 1;
        }

        // Finish drag at snapped position: guides should clear
        presenter->debugLabelDragFinished(6, QPointF(midX, nearY));
        QApplication::processEvents();
        if (presenter->debugHorizontalAlignGuideVisible()) {
            std::fprintf(stderr, "FAIL: horizontal align guide did not clear on pill drag finish\n");
            return 1;
        }
    }

    // ---- restore the pre-scenario machine ------------------------------------
    kernel.send(app::events::DeleteTransitionRequested{.id = rootId});
    kernel.send(app::events::DeleteTransitionRequested{.id = stubId});
    kernel.send(app::events::DeleteTransitionRequested{.id = selfId});
    kernel.send(app::events::MoveTransitionLabelRequested{.id = 6, .offset = offset6});
    kernel.send(app::events::MoveTransitionLabelRequested{.id = 7, .offset = offset7});
    QApplication::processEvents();
    if (doc->findTransition(rootId) != nullptr || doc->findTransition(stubId) != nullptr ||
        doc->findTransition(selfId) != nullptr || doc->findTransition(6)->labelOffset != offset6 ||
        doc->findTransition(7)->labelOffset != offset7 || doc->findState(2)->pos != state2Origin ||
        doc->findState(4)->pos != state4Origin) {
        std::fprintf(stderr, "FAIL: the pill-ports scenario did not restore the machine it borrowed\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario pill-ports (offset-free pill on its wire, a lateral pill move that moves "
                "nothing else, node move reroutes, self straddle, bent stub, root form, corner bound + no U-turn on "
                "endpoints dragged close, parked pills leave the skeleton alone, node-pipeline ends, keepout drag "
                "clamp, port-line snap)\n");
    return 0;
}

// Scenario "min-bend-routing": a diagonal transition (Transition 8: Reset, Error
// (id 4) to LoggedOut (id 1)) attaches through perpendicular node sides
// (Error.Left -> LoggedOut.Bottom) and draws as an unobstructed 1-corner L.
// The min-bend rule lives in node_anchors.cpp's disjoint-on-both-axes branch;
// assertions are made against the drawn wire.
int runMinBendRoutingScenario(app::MainWindow& window, app::EditorView* loginPane) {
    if (loginPane == nullptr || loginPane->presenter() == nullptr) {
        std::fprintf(stderr, "FAIL: min-bend-routing scenario preconditions (pane/presenter) missing\n");
        return 1;
    }
    app::CanvasPresenter* presenter = loginPane->presenter();
    auto doc = window.loginFlowSession()->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr) {
        std::fprintf(stderr, "FAIL: min-bend-routing scenario cannot find MachineDocAgent\n");
        return 1;
    }

    constexpr quint64 kResetTransitionId = 8;
    const app::Transition* resetTransition = doc->findTransition(kResetTransitionId);
    if (resetTransition == nullptr || resetTransition->from != 4 || resetTransition->to != 1) {
        std::fprintf(stderr, "FAIL: min-bend-routing scenario cannot find Reset transition (id=8, from=4, to=1)\n");
        return 1;
    }

    QGraphicsScene* scene = loginPane->canvasView() != nullptr ? loginPane->canvasView()->scene() : nullptr;
    if (scene == nullptr) {
        std::fprintf(stderr, "FAIL: min-bend-routing scenario cannot find scene\n");
        return 1;
    }
    const app::StateItem* sourceItem = findStateItemById(scene, 4);
    const app::StateItem* targetItem = findStateItemById(scene, 1);
    const app::TransitionItem* edgeItem = presenter->debugTransitionItem(kResetTransitionId);
    if (sourceItem == nullptr || targetItem == nullptr || edgeItem == nullptr) {
        std::fprintf(stderr, "FAIL: min-bend-routing scenario cannot find source/target/edge items for Reset\n");
        return 1;
    }

    const QRectF sourceRect = sourceItem->sceneRect();
    const QRectF targetRect = targetItem->sceneRect();
    const app::RoutedEdge& route = edgeItem->debugRoute();

    UnifiedWire wire;
    if (!unifiedWireHolds(route, "the Reset wire", &wire) ||
        !unifiedWireMeetsNodes(wire, sourceRect, targetRect, "the Reset wire") ||
        !unifiedLabelLawHolds(route, resetTransition->labelOffset, "the Reset pill")) {
        return 1;
    }

    // Perpendicular node sides: out of Error's Left (West), into LoggedOut's
    // Bottom (South) -- read off the wire's own end tangents.
    const app::PortSide sourceSide = sideOfOutwardNormal(wire.departure);
    const app::PortSide targetSide = sideOfOutwardNormal(-wire.arrival);
    if (sourceSide != app::PortSide::Left || targetSide != app::PortSide::Bottom) {
        std::fprintf(stderr,
                      "FAIL: Reset transition node sides are source=%d, target=%d (expected source=Left, target=Bottom)\n",
                      static_cast<int>(sourceSide), static_cast<int>(targetSide));
        return 1;
    }

    // One corner: perpendicular sides make the run an L, not a Z.
    if (wire.points.size() != 3) {
        std::fprintf(stderr, "FAIL: Reset transition is not a 1-corner L (%d waypoints, want 3)\n",
                      static_cast<int>(wire.points.size()));
        for (const QPointF& pt : wire.points) {
            std::fprintf(stderr, "  wp (%.1f, %.1f)\n", pt.x(), pt.y());
        }
        return 1;
    }

    // Capture the route region for visual verification
    QRectF region = presenter->debugPillRect(kResetTransitionId);
    if (!route.sourceHalf.isEmpty()) {
        region = region.united(route.sourceHalf.boundingRect());
    }
    if (!route.targetHalf.isEmpty()) {
        region = region.united(route.targetHalf.boundingRect());
    }
    region = region.united(sourceRect);
    region = region.united(targetRect);
    region = region.adjusted(-20.0, -20.0, 20.0, 20.0);

    if (!saveSceneRegionCapture(loginPane, region, "probe-min-bend-routing")) {
        return 1;
    }

    std::printf("PASS: gui-probe scenario min-bend-routing (Error.Left -> LoggedOut.Bottom perpendicular node "
                "sides, one unified 1-corner L, pill on its own wire base)\n");
    return 0;
}

// Scenario "port-snap-alignment": dragging LoggedIn near Authenticating's port
// centerline engages magnetic port-to-port snap (dy == 0), shows a magenta
// dashed guide, and yields a straight horizontal wire.
int runPortSnapAlignmentScenario(app::MainWindow& window, app::EditorView* loginPane) {
    if (loginPane == nullptr || loginPane->presenter() == nullptr || loginPane->canvasView() == nullptr) {
        std::fprintf(stderr, "FAIL: port-snap-alignment scenario preconditions (pane/presenter/view) missing\n");
        return 1;
    }
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view->scene();
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: port-snap-alignment scenario cannot find doc/scene\n");
        return 1;
    }

    // Login Flow fixture:
    // State 2 = Authenticating (pos = 320, 180, entryAction beginLogin(), taller box)
    // State 3 = LoggedIn (pos = 560, 120, leaf final state, shorter box)
    // Transition 6: Authenticating -> LoggedIn ("Success")
    app::StateItem* authItem = findStateItemById(scene, 2);
    app::StateItem* loggedInItem = findStateItemById(scene, 3);
    if (authItem == nullptr || loggedInItem == nullptr) {
        std::fprintf(stderr, "FAIL: port-snap-alignment scenario cannot find Authenticating / LoggedIn items\n");
        return 1;
    }

    const qreal authCenterY = authItem->sceneRect().center().y();
    const qreal origLoggedInCenterY = loggedInItem->sceneRect().center().y();
    const QPointF origLoggedInPos = loggedInItem->pos();

    // Authenticating and LoggedIn initial centers are intentionally misaligned (180 vs 120)
    if (std::abs(origLoggedInCenterY - authCenterY) < 10.0) {
        std::fprintf(stderr, "FAIL: port-snap-alignment initial centers already aligned\n");
        return 1;
    }

    // Drag LoggedIn toward Authenticating's port centerline (Y-axis)
    const QPointF pressPoint = loggedInItem->sceneRect().center();
    view->debugMousePress(pressPoint);

    // Step 1: initial move 25px away arms NodeDrag session without premature snap:
    view->debugMouseMove(QPointF(pressPoint.x(), authCenterY - 25.0));
    if (presenter->debugHorizontalAlignGuideVisible()) {
        view->debugMouseRelease(QPointF(pressPoint.x(), authCenterY - 25.0));
        std::fprintf(stderr, "FAIL: port-snap-alignment guide showed prematurely 25px away\n");
        return 1;
    }

    // Step 2: drag to within 3px of Authenticating's center Y (within 5px threshold):
    const QPointF dragTarget(pressPoint.x(), authCenterY - 3.0);
    view->debugMouseMove(dragTarget);

    auto abortOnFail = [&](const char* msg) -> int {
        view->debugMouseRelease(dragTarget);
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();
        std::fprintf(stderr, "%s\n", msg);
        return 1;
    };

    // Verify port snap engaged:
    // 1. LoggedIn's center Y snapped exactly to Authenticating's center Y:
    const qreal snappedCenterY = loggedInItem->sceneRect().center().y();
    if (std::abs(snappedCenterY - authCenterY) > 0.001) {
        return abortOnFail(qPrintable(
            QStringLiteral(
                "FAIL: port-snap-alignment did not snap LoggedIn center Y to Authenticating center Y (got %1, want %2)")
                .arg(snappedCenterY)
                .arg(authCenterY)));
    }

    // 2. Horizontal alignment guide is visible:
    if (!presenter->debugHorizontalAlignGuideVisible()) {
        return abortOnFail("FAIL: port-snap-alignment did not show horizontal alignment guide");
    }

    // 3. Transition 6 (Success) is now a straight horizontal line (dy == 0 across port anchors)
    const app::TransitionItem* edgeItem = presenter->debugTransitionItem(6);
    if (edgeItem != nullptr) {
        const app::RoutedEdge& route = edgeItem->debugRoute();
        if (!route.sourceHalf.isEmpty() && !route.targetHalf.isEmpty()) {
            const QPointF sourceStart = route.sourceHalf.pointAtPercent(0.0);
            const QPointF targetEnd = route.targetHalf.pointAtPercent(1.0);
            if (std::abs(sourceStart.y() - targetEnd.y()) > 0.5) {
                const QPointF pillCenter = presenter->debugLabelCenter(6);
                std::fprintf(stderr, "  pill center (%.1f, %.1f), authCenterY %.1f\n", pillCenter.x(),
                             pillCenter.y(), authCenterY);
                for (const QPointF& pt : route.sourceWaypoints) {
                    std::fprintf(stderr, "  src (%.1f, %.1f)\n", pt.x(), pt.y());
                }
                for (const QPointF& pt : route.targetWaypoints) {
                    std::fprintf(stderr, "  tgt (%.1f, %.1f)\n", pt.x(), pt.y());
                }
                return abortOnFail(
                    qPrintable(QStringLiteral("FAIL: port-snap-alignment wire is not horizontal (sourceStart.y=%1, "
                                              "targetEnd.y=%2)")
                                   .arg(sourceStart.y())
                                   .arg(targetEnd.y())));
            }
        }
    }

    // Capture visual proof of the aligned port guide and straight transition wire:
    const QRectF captureRegion =
        authItem->sceneRect().united(loggedInItem->sceneRect()).adjusted(-40.0, -40.0, 40.0, 40.0);
    if (!saveSceneRegionCapture(loginPane, captureRegion, "probe-port-snap-alignment")) {
        return abortOnFail("FAIL: port-snap-alignment failed to save capture");
    }

    // Release and commit:
    view->debugMouseRelease(dragTarget);
    QApplication::processEvents();

    if (presenter->debugHorizontalAlignGuideVisible()) {
        std::fprintf(stderr, "FAIL: port-snap-alignment guide did not hide after mouse release\n");
        return 1;
    }

    // Verify committed position maintains center alignment:
    const app::State* committedState = doc->findState(3);
    if (committedState == nullptr) {
        std::fprintf(stderr, "FAIL: port-snap-alignment cannot find committed LoggedIn state\n");
        return 1;
    }

    // Undo to restore original machine position:
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();

    if (loggedInItem->pos() != origLoggedInPos) {
        std::fprintf(stderr, "FAIL: port-snap-alignment undo did not restore original position\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario port-snap-alignment (LoggedIn snaps to Authenticating port centerline dy=0, "
                "horizontal magenta guide displayed, straight horizontal Success wire, undo restored)\n");
    return 0;
}

// Scenario "segment-dragging": hovering an orthogonal wire segment shows the
// move cursor and midpoint grab handles; dragging translates the segment in
// parallel, keeping every waypoint orthogonal, recorded in
// Transition::manualBendpoints with undo/redo.
int runSegmentDraggingScenario(app::MainWindow& window, app::EditorView* loginPane) {
    if (loginPane == nullptr || loginPane->presenter() == nullptr || loginPane->canvasView() == nullptr) {
        std::fprintf(stderr, "FAIL: segment-dragging scenario preconditions (pane/presenter/view) missing\n");
        return 1;
    }
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view->scene();
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: segment-dragging scenario cannot find doc/scene\n");
        return 1;
    }

    constexpr quint64 kResetTransitionId = 8;
    const app::Transition* resetTransition = doc->findTransition(kResetTransitionId);
    if (resetTransition == nullptr || resetTransition->from != 4 || resetTransition->to != 1) {
        std::fprintf(stderr, "FAIL: segment-dragging scenario cannot find Reset transition (id=8)\n");
        return 1;
    }

    const app::TransitionItem* edgeItem = presenter->debugTransitionItem(kResetTransitionId);
    if (edgeItem == nullptr) {
        std::fprintf(stderr, "FAIL: segment-dragging scenario cannot find TransitionItem for Reset\n");
        return 1;
    }

    // 1. Verify canonical waypoints: Reset is a 1-corner L-route (sourceStub -> corner -> lineStop)
    const QVector<QPointF> origWaypoints = app::canonicalWaypoints(edgeItem->debugRoute());
    if (origWaypoints.size() < 3) {
        std::fprintf(stderr, "FAIL: segment-dragging scenario Reset transition has fewer than 3 waypoints (got %d)\n",
                      static_cast<int>(origWaypoints.size()));
        return 1;
    }

    // Find the horizontal segment (segment 0: sourceStub -> corner):
    const QPointF testHitPoint = (origWaypoints[0] + origWaypoints[1]) * 0.5;
    const app::WireSegment hit = edgeItem->hitSegmentAt(testHitPoint);
    if (hit.segmentIndex < 0) {
        std::fprintf(stderr, "FAIL: segment-dragging scenario hitSegmentAt failed on Reset horizontal segment\n");
        return 1;
    }
    if (hit.orientation != app::WireSegment::Orientation::Horizontal) {
        std::fprintf(stderr, "FAIL: segment-dragging expected horizontal orientation for segment %d\n", hit.segmentIndex);
        return 1;
    }

    // 2. Select edge item to reveal grab handles:
    const_cast<app::TransitionItem*>(edgeItem)->setSelected(true);
    QApplication::processEvents();

    // Visual proof of the selected handle language: port dots, bend-vertex
    // squares and midpoint handles on the resting selected edge.
    const QRectF selectedRegion = edgeItem->sceneBoundingRect().adjusted(-30.0, -30.0, 30.0, 30.0);
    if (!saveSceneRegionCapture(loginPane, selectedRegion, "probe-segment-handles-selected")) {
        return 1;
    }

    // 3. Perform interactive segment drag: move horizontal segment parallel by deltaY = +40.0
    presenter->debugSegmentDragStarted(kResetTransitionId, hit.segmentIndex, hit.isSourceHalf, true, testHitPoint);
    const QPointF dragPos(testHitPoint.x(), testHitPoint.y() + 40.0);
    presenter->debugSegmentDragMoved(kResetTransitionId, dragPos);
    QApplication::processEvents();

    // Mid-drag proof: the bend-vertex squares track the moved skeleton. The
    // presenter debug path bypasses TransitionItem's mouse handlers, so the
    // active-segment highlight is not in this capture.
    const QRectF liveRegion = edgeItem->sceneBoundingRect().adjusted(-30.0, -30.0, 30.0, 30.0);
    if (!saveSceneRegionCapture(loginPane, liveRegion, "probe-segment-drag-live")) {
        return 1;
    }

    presenter->debugSegmentDragFinished(kResetTransitionId, dragPos);
    QApplication::processEvents();

    // 4. Verify manual bendpoints persisted in doc model:
    const app::Transition* modifiedTransition = doc->findTransition(kResetTransitionId);
    if (modifiedTransition == nullptr || modifiedTransition->manualBendpoints.isEmpty()) {
        std::fprintf(stderr, "FAIL: segment-dragging did not populate manualBendpoints on transition\n");
        return 1;
    }
    // The bendpoint transaction must not touch any other field:
    if (modifiedTransition->event != QStringLiteral("Reset")) {
        std::fprintf(stderr, "FAIL: segment-dragging commit changed the transition event (now '%s')\n",
                     qUtf8Printable(modifiedTransition->event));
        return 1;
    }

    // Pill-on-wire invariant: on a manual route the anchor sits on the
    // skeleton, the pill item at the anchor, and both drawn halves stop at
    // the pill's body.
    const auto distToPolyline = [](QPointF p, const QVector<QPointF>& pts) -> qreal {
        qreal best = 1e18;
        for (int i = 0; i < pts.size() - 1; ++i) {
            const QPointF a = pts[i];
            const QPointF v = pts[i + 1] - a;
            const qreal lenSq = QPointF::dotProduct(v, v);
            if (lenSq < 1e-6) continue;
            const qreal t = std::clamp(QPointF::dotProduct(p - a, v) / lenSq, 0.0, 1.0);
            const QPointF proj = a + v * t;
            best = std::min(best, std::sqrt(QPointF::dotProduct(p - proj, p - proj)));
        }
        return best;
    };
    const auto assertPillOnWire = [&](const char* stage) -> bool {
        const app::RoutedEdge& route = edgeItem->debugRoute();
        const QVector<QPointF> pts = app::canonicalWaypoints(route);
        const qreal anchorDist = distToPolyline(route.labelAnchor, pts);
        if (anchorDist > 0.75) {
            std::fprintf(stderr, "FAIL: segment-dragging %s pill anchor left the wire (dist=%.2f)\n", stage,
                         anchorDist);
            return false;
        }
        const QPointF pillCenter = presenter->debugLabelCenter(kResetTransitionId);
        if ((pillCenter - route.labelAnchor).manhattanLength() > 0.5) {
            std::fprintf(stderr, "FAIL: segment-dragging %s pill item detached from the anchor\n", stage);
            return false;
        }
        const QRectF nearPill = presenter->debugPillRect(kResetTransitionId).adjusted(-10.0, -10.0, 10.0, 10.0);
        if (route.sourceHalf.elementCount() > 0 && !nearPill.contains(route.sourceHalf.currentPosition())) {
            std::fprintf(stderr, "FAIL: segment-dragging %s source half does not stop at the pill\n", stage);
            return false;
        }
        if (route.targetHalf.elementCount() > 0) {
            const auto firstEl = route.targetHalf.elementAt(0);
            if (!nearPill.contains(QPointF(firstEl.x, firstEl.y))) {
                std::fprintf(stderr, "FAIL: segment-dragging %s target half does not start at the pill\n", stage);
                return false;
            }
        }
        return true;
    };
    // Arrow-approach invariant: the arrowhead is axis-aligned and points into
    // its target node, never sideways along a jog, mid-drag included.
    const auto assertArrowIntoRect = [](const app::TransitionItem* item, const QRectF& nodeRect,
                                        const char* stage) -> bool {
        const QPolygonF& arrow = item->debugRoute().arrowhead;
        if (arrow.size() < 3) {
            std::fprintf(stderr, "FAIL: segment-dragging %s arrowhead missing\n", stage);
            return false;
        }
        const QPointF apex = arrow.at(0);
        const QPointF baseCenter = (arrow.at(1) + arrow.at(2)) * 0.5;
        QPointF dir = apex - baseCenter;
        const qreal len = std::sqrt(QPointF::dotProduct(dir, dir));
        if (len < 0.01) {
            std::fprintf(stderr, "FAIL: segment-dragging %s arrowhead degenerate\n", stage);
            return false;
        }
        dir /= len;
        if (std::min(std::abs(dir.x()), std::abs(dir.y())) > 0.1) {
            std::fprintf(stderr, "FAIL: segment-dragging %s arrowhead not axis-aligned (%.2f, %.2f)\n", stage,
                         dir.x(), dir.y());
            return false;
        }
        const QRectF target = nodeRect.adjusted(-2.0, -2.0, 2.0, 2.0);
        bool pointsIn = false;
        for (qreal t = 0.0; t <= 40.0; t += 4.0) {
            if (target.contains(apex + dir * t)) {
                pointsIn = true;
                break;
            }
        }
        if (!pointsIn) {
            std::fprintf(stderr, "FAIL: segment-dragging %s arrowhead points away from its target node\n",
                         stage);
            return false;
        }
        // The target half's end tangent must run parallel to the arrow. It is
        // taken from the last chord of real length (>= 4px): a sub-pixel
        // axis-parallel tail must not vouch for a diagonal fillet body.
        const QPainterPath& half = item->debugRoute().targetHalf;
        for (int i = half.elementCount() - 1; i >= 1; --i) {
            const auto e1 = half.elementAt(i - 1);
            const auto e2 = half.elementAt(i);
            QPointF tangent(e2.x - e1.x, e2.y - e1.y);
            const qreal tangentLen = std::sqrt(QPointF::dotProduct(tangent, tangent));
            if (tangentLen < 4.0) {
                continue;
            }
            tangent /= tangentLen;
            if (QPointF::dotProduct(tangent, dir) < 0.95) {
                std::fprintf(stderr,
                             "FAIL: segment-dragging %s approach does not feed the arrow "
                             "(tangent %.2f, %.2f vs arrow %.2f, %.2f)\n",
                             stage, tangent.x(), tangent.y(), dir.x(), dir.y());
                return false;
            }
            break;
        }
        return true;
    };
    app::StateItem* loggedOutItem = findStateItemById(scene, 1);
    app::StateItem* errorStateItem = findStateItemById(scene, 4);
    if (loggedOutItem == nullptr || errorStateItem == nullptr) {
        std::fprintf(stderr, "FAIL: segment-dragging cannot find LoggedOut/Error StateItems\n");
        return 1;
    }

    // No manual route may pass through its own endpoint nodes' bodies
    // (routeManual falls back to the auto route for degenerate courses).
    const auto assertNoNodeCrossing = [&](const app::TransitionItem* item, const char* stage) -> bool {
        const QVector<QPointF> pts = app::canonicalWaypoints(item->debugRoute());
        const auto crosses = [&pts](const QRectF& rect) {
            const QRectF inner = rect.adjusted(1.0, 1.0, -1.0, -1.0);
            for (int i = 0; i < pts.size() - 1; ++i) {
                const qreal minX = std::min(pts[i].x(), pts[i + 1].x());
                const qreal maxX = std::max(pts[i].x(), pts[i + 1].x());
                const qreal minY = std::min(pts[i].y(), pts[i + 1].y());
                const qreal maxY = std::max(pts[i].y(), pts[i + 1].y());
                if (minX < inner.right() && maxX > inner.left() && minY < inner.bottom() &&
                    maxY > inner.top()) {
                    return true;
                }
            }
            return false;
        };
        if (crosses(errorStateItem->sceneRect()) || crosses(loggedOutItem->sceneRect())) {
            std::fprintf(stderr, "FAIL: segment-dragging %s wire crosses an endpoint node's body\n", stage);
            return false;
        }
        return true;
    };

    if (!assertPillOnWire("post-commit")) {
        return 1;
    }
    if (!assertArrowIntoRect(edgeItem, loggedOutItem->sceneRect(), "post-commit")) {
        return 1;
    }
    if (!assertNoNodeCrossing(edgeItem, "post-commit")) {
        return 1;
    }

    // 5. Verify strict 90-degree orthogonality across all waypoints:
    const QVector<QPointF>& newWaypoints = edgeItem->debugRoute().waypoints;
    if (newWaypoints.size() < 3) {
        std::fprintf(stderr, "FAIL: segment-dragging newWaypoints has fewer than 3 points\n");
        return 1;
    }
    for (int i = 0; i < newWaypoints.size() - 1; ++i) {
        const qreal dx = std::abs(newWaypoints[i].x() - newWaypoints[i + 1].x());
        const qreal dy = std::abs(newWaypoints[i].y() - newWaypoints[i + 1].y());
        if (dx > 0.01 && dy > 0.01) {
            std::fprintf(stderr, "FAIL: segment-dragging waypoints[%d]->waypoints[%d] is not orthogonal (dx=%.2f, dy=%.2f)\n",
                         i, i + 1, dx, dy);
            return 1;
        }
    }

    const auto assertPathHasNoDiagonals = [](const QPainterPath& path, const char* name) -> bool {
        for (int i = 0; i < path.elementCount() - 1; ++i) {
            const auto e1 = path.elementAt(i);
            const auto e2 = path.elementAt(i + 1);
            if (e2.isLineTo()) {
                const qreal dx = std::abs(e1.x - e2.x);
                const qreal dy = std::abs(e1.y - e2.y);
                if (dx > 0.5 && dy > 0.5) {
                    std::fprintf(stderr, "FAIL: %s contains diagonal LineTo segment from (%.1f, %.1f) to (%.1f, %.1f)\n",
                                 name, e1.x, e1.y, e2.x, e2.y);
                    return false;
                }
            }
        }
        return true;
    };
    if (!assertPathHasNoDiagonals(edgeItem->debugRoute().sourceHalf, "reset-sourceHalf")) return 1;
    if (!assertPathHasNoDiagonals(edgeItem->debugRoute().targetHalf, "reset-targetHalf")) return 1;

    // Save capture for visual proof:
    const QRectF captureRegion = edgeItem->sceneBoundingRect().adjusted(-30.0, -30.0, 30.0, 30.0);
    if (!saveSceneRegionCapture(loginPane, captureRegion, "probe-segment-dragging")) {
        return 1;
    }

    // 6. Test Undo:
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();

    const app::Transition* revertedTransition = doc->findTransition(kResetTransitionId);
    if (revertedTransition == nullptr || !revertedTransition->manualBendpoints.isEmpty()) {
        std::fprintf(stderr, "FAIL: segment-dragging undo did not clear manualBendpoints\n");
        return 1;
    }
    if (revertedTransition->event != QStringLiteral("Reset")) {
        std::fprintf(stderr, "FAIL: segment-dragging undo changed the transition event (now '%s')\n",
                     qUtf8Printable(revertedTransition->event));
        return 1;
    }

    // 7. Test Redo:
    kernel.send(app::events::RedoRequested{});
    QApplication::processEvents();

    const app::Transition* redoneTransition = doc->findTransition(kResetTransitionId);
    if (redoneTransition == nullptr || redoneTransition->manualBendpoints.isEmpty()) {
        std::fprintf(stderr, "FAIL: segment-dragging redo did not restore manualBendpoints\n");
        return 1;
    }
    if (redoneTransition->event != QStringLiteral("Reset")) {
        std::fprintf(stderr, "FAIL: segment-dragging redo changed the transition event (now '%s')\n",
                     qUtf8Printable(redoneTransition->event));
        return 1;
    }
    if (!assertPillOnWire("post-redo")) {
        return 1;
    }

    // Revert back so machine remains untouched for subsequent scenarios:
    kernel.send(app::events::UndoRequested{});
    const_cast<app::TransitionItem*>(edgeItem)->setSelected(false);
    QApplication::processEvents();

    // 7b. Frame growth under an extreme drag: push the horizontal segment far
    // below the frame; mid-drag and post-commit the frame must contain the
    // whole skeleton and the pill still ride the wire; undo shrinks the frame back.
    {
        const auto findFrameRect = [&]() -> QRectF {
            for (QGraphicsItem* item : scene->items()) {
                if (item->type() == app::MachineFrameItem::Type) {
                    return static_cast<app::MachineFrameItem*>(item)->sceneFrameRect();
                }
            }
            return QRectF();
        };
        const auto frameContainsSkeleton = [&](const char* stage) -> bool {
            const QRectF frameRect = findFrameRect().adjusted(-1.0, -1.0, 1.0, 1.0);
            if (frameRect.isNull()) {
                std::fprintf(stderr, "FAIL: segment-dragging %s machine frame missing\n", stage);
                return false;
            }
            const QVector<QPointF> pts = app::canonicalWaypoints(edgeItem->debugRoute());
            for (const QPointF& pt : pts) {
                if (!frameRect.contains(pt)) {
                    std::fprintf(stderr,
                                 "FAIL: segment-dragging %s wire escaped the frame (point %.1f,%.1f vs frame "
                                 "bottom %.1f)\n",
                                 stage, pt.x(), pt.y(), frameRect.bottom());
                    return false;
                }
            }
            if (!frameRect.contains(presenter->debugPillRect(kResetTransitionId))) {
                std::fprintf(stderr, "FAIL: segment-dragging %s pill escaped the frame\n", stage);
                return false;
            }
            return true;
        };

        const QRectF frameBefore = findFrameRect();
        const QVector<QPointF> growWaypoints = app::canonicalWaypoints(edgeItem->debugRoute());
        if (growWaypoints.size() < 2) {
            std::fprintf(stderr, "FAIL: segment-dragging frame-growth setup has no waypoints\n");
            return 1;
        }
        const QPointF growHitPoint = (growWaypoints[0] + growWaypoints[1]) * 0.5;
        const app::WireSegment growHit = edgeItem->hitSegmentAt(growHitPoint);
        if (growHit.segmentIndex < 0) {
            std::fprintf(stderr, "FAIL: segment-dragging frame-growth hitSegmentAt missed\n");
            return 1;
        }
        presenter->debugSegmentDragStarted(kResetTransitionId, growHit.segmentIndex, growHit.isSourceHalf,
                                           growHit.orientation == app::WireSegment::Orientation::Horizontal,
                                           growHitPoint);
        const QPointF growDragPos =
            growHit.orientation == app::WireSegment::Orientation::Horizontal
                ? QPointF(growHitPoint.x(), growHitPoint.y() + 150.0)
                : QPointF(growHitPoint.x() + 150.0, growHitPoint.y());
        presenter->debugSegmentDragMoved(kResetTransitionId, growDragPos);
        QApplication::processEvents();

        if (!frameContainsSkeleton("mid-drag")) {
            return 1;
        }
        if (!assertPillOnWire("mid-drag")) {
            return 1;
        }
        if (!assertArrowIntoRect(edgeItem, loggedOutItem->sceneRect(), "frame-growth-mid-drag")) {
            return 1;
        }
        if (!saveSceneRegionCapture(loginPane, findFrameRect().adjusted(-16.0, -16.0, 16.0, 16.0),
                                    "probe-segment-drag-frame-growth")) {
            return 1;
        }

        presenter->debugSegmentDragFinished(kResetTransitionId, growDragPos);
        QApplication::processEvents();

        if (!frameContainsSkeleton("post-commit")) {
            return 1;
        }
        if (!assertPillOnWire("frame-growth-commit")) {
            return 1;
        }
        if (!assertArrowIntoRect(edgeItem, loggedOutItem->sceneRect(), "frame-growth-commit")) {
            return 1;
        }
        if (!assertNoNodeCrossing(edgeItem, "frame-growth-commit")) {
            return 1;
        }

        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();
        const app::Transition* grownReverted = doc->findTransition(kResetTransitionId);
        if (grownReverted == nullptr || !grownReverted->manualBendpoints.isEmpty()) {
            std::fprintf(stderr, "FAIL: segment-dragging frame-growth undo did not clear manualBendpoints\n");
            return 1;
        }
        const QRectF frameAfterUndo = findFrameRect();
        if (frameAfterUndo.bottom() > frameBefore.bottom() + 1.0) {
            std::fprintf(stderr,
                         "FAIL: segment-dragging frame did not shrink back after undo (bottom %.1f vs %.1f)\n",
                         frameAfterUndo.bottom(), frameBefore.bottom());
            return 1;
        }
    }

    // 7c. Merge snap: re-bend the wire, then drag one of two parallel runs to
    // within the snap radius of the other; the #FF0066 guide must show mid-drag
    // and the release must land the pair exactly collinear so the commit
    // merges them into fewer segments.
    {
        const QVector<QPointF> setupWaypoints = app::canonicalWaypoints(edgeItem->debugRoute());
        if (setupWaypoints.size() < 2) {
            std::fprintf(stderr, "FAIL: segment-dragging merge-snap setup has no waypoints\n");
            return 1;
        }
        const QPointF setupHit = (setupWaypoints[0] + setupWaypoints[1]) * 0.5;
        const app::WireSegment setupSeg = edgeItem->hitSegmentAt(setupHit);
        if (setupSeg.segmentIndex < 0) {
            std::fprintf(stderr, "FAIL: segment-dragging merge-snap setup hitSegmentAt missed\n");
            return 1;
        }
        const bool setupHoriz = setupSeg.orientation == app::WireSegment::Orientation::Horizontal;
        presenter->debugSegmentDragStarted(kResetTransitionId, setupSeg.segmentIndex, setupSeg.isSourceHalf,
                                           setupHoriz, setupHit);
        const QPointF setupDragPos = setupHoriz ? QPointF(setupHit.x(), setupHit.y() + 40.0)
                                                : QPointF(setupHit.x() + 40.0, setupHit.y());
        presenter->debugSegmentDragMoved(kResetTransitionId, setupDragPos);
        QApplication::processEvents();
        presenter->debugSegmentDragFinished(kResetTransitionId, setupDragPos);
        QApplication::processEvents();

        // A parallel pair with real separation, so the snap has a gap to close.
        const QVector<QPointF> bent = app::canonicalWaypoints(edgeItem->debugRoute());
        const QVector<app::WireSegment> bentSegs = app::extractWireSegments(bent, true);
        const auto segCoord = [](const app::WireSegment& seg) {
            return seg.orientation == app::WireSegment::Orientation::Horizontal ? seg.line.p1().y()
                                                                                : seg.line.p1().x();
        };
        int dragIdx = -1;
        qreal dragCoord = 0.0;
        qreal partnerCoord = 0.0;
        bool dragHoriz = false;
        QPointF dragPress;
        qreal bestSeparation = 0.0;
        for (int i = 0; i < bentSegs.size(); ++i) {
            for (int j = 0; j < bentSegs.size(); ++j) {
                if (i == j || bentSegs[i].orientation != bentSegs[j].orientation) {
                    continue;
                }
                // Mergeable pairs only: exactly one perpendicular connector
                // between them, or a far-apart pair makes the probe fail wrongly.
                if (std::abs(bentSegs[i].segmentIndex - bentSegs[j].segmentIndex) != 2) {
                    continue;
                }
                const qreal separation = std::abs(segCoord(bentSegs[i]) - segCoord(bentSegs[j]));
                if (separation < 12.0) {
                    continue;
                }
                const bool interior = bentSegs[i].segmentIndex > 0 &&
                                      bentSegs[i].segmentIndex < bent.size() - 2;
                const qreal score = separation + (interior ? 1000.0 : 0.0);
                if (score > bestSeparation) {
                    bestSeparation = score;
                    dragIdx = bentSegs[i].segmentIndex;
                    dragCoord = segCoord(bentSegs[i]);
                    partnerCoord = segCoord(bentSegs[j]);
                    dragHoriz = bentSegs[i].orientation == app::WireSegment::Orientation::Horizontal;
                    dragPress = bentSegs[i].line.center();
                }
            }
        }
        if (dragIdx < 0) {
            std::fprintf(stderr, "FAIL: segment-dragging merge-snap found no parallel segment pair\n");
            return 1;
        }

        // Stop 3px short of the partner: inside the 6px snap radius, so
        // only the snap can close the gap to exact collinearity.
        const qreal fullDelta = partnerCoord - dragCoord;
        const qreal shortDelta = fullDelta > 0.0 ? fullDelta - 3.0 : fullDelta + 3.0;
        const QPointF mergeDragPos = dragHoriz ? QPointF(dragPress.x(), dragPress.y() + shortDelta)
                                               : QPointF(dragPress.x() + shortDelta, dragPress.y());
        presenter->debugSegmentDragStarted(kResetTransitionId, dragIdx, true, dragHoriz, dragPress);
        presenter->debugSegmentDragMoved(kResetTransitionId, mergeDragPos);
        QApplication::processEvents();

        const bool guideVisible = dragHoriz ? presenter->debugHorizontalAlignGuideVisible()
                                            : presenter->debugVerticalAlignGuideVisible();
        if (!guideVisible) {
            std::fprintf(stderr, "FAIL: segment-dragging merge-snap guide did not show inside the radius\n");
            return 1;
        }
        const QRectF mergeRegion = edgeItem->sceneBoundingRect().adjusted(-30.0, -30.0, 30.0, 30.0);
        if (!saveSceneRegionCapture(loginPane, mergeRegion, "probe-segment-merge-snap")) {
            return 1;
        }

        presenter->debugSegmentDragFinished(kResetTransitionId, mergeDragPos);
        QApplication::processEvents();

        if (presenter->debugHorizontalAlignGuideVisible() || presenter->debugVerticalAlignGuideVisible()) {
            std::fprintf(stderr, "FAIL: segment-dragging merge-snap guide survived the release\n");
            return 1;
        }
        const QVector<app::WireSegment> mergedSegs =
            app::extractWireSegments(app::canonicalWaypoints(edgeItem->debugRoute()), true);
        if (mergedSegs.size() >= bentSegs.size()) {
            std::fprintf(stderr,
                         "FAIL: segment-dragging merge-snap did not reduce segments (%d -> %d)\n",
                         static_cast<int>(bentSegs.size()), static_cast<int>(mergedSegs.size()));
            return 1;
        }

        // Unwind both commits (merge, then the setup bend) back to baseline:
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();
        const app::Transition* mergeReverted = doc->findTransition(kResetTransitionId);
        if (mergeReverted == nullptr || !mergeReverted->manualBendpoints.isEmpty()) {
            std::fprintf(stderr, "FAIL: segment-dragging merge-snap unwind did not restore the baseline\n");
            return 1;
        }
    }

    // 7d. Flank attach: a course running beside the node must enter through
    // the flank it is on, never a clamped Bottom/Top anchor whose short
    // approach drowns in the fillet. The rule is direction-symmetric, so one
    // left and one top case pin it.
    {
        const QRectF loRect = loggedOutItem->sceneRect();
        const auto arrowDirOf = [&]() -> QPointF {
            const QPolygonF& arrow = edgeItem->debugRoute().arrowhead;
            if (arrow.size() < 3) {
                return QPointF();
            }
            const QPointF apex = arrow.at(0);
            const QPointF baseCenter = (arrow.at(1) + arrow.at(2)) * 0.5;
            const QPointF d = apex - baseCenter;
            const qreal len = std::sqrt(QPointF::dotProduct(d, d));
            return len > 0.01 ? d / len : QPointF();
        };
        const auto arrowApexOf = [&]() -> QPointF {
            const QPolygonF& arrow = edgeItem->debugRoute().arrowhead;
            return arrow.isEmpty() ? QPointF() : arrow.at(0);
        };

        // Left flank: the course rises past the node 60px left of it.
        const qreal leftX = loRect.left() - 60.0;
        kernel.send(app::events::SetTransitionBendpointsRequested{
            .id = kResetTransitionId,
            .bendpoints = {QPointF(leftX, loRect.bottom() + 120.0), QPointF(leftX, loRect.center().y())},
        });
        QApplication::processEvents();
        QPointF flankDir = arrowDirOf();
        QPointF flankApex = arrowApexOf();
        if (flankDir.x() < 0.9) {
            std::fprintf(stderr,
                         "FAIL: segment-dragging left-flank course did not enter the Left port (dir %.2f, %.2f)\n",
                         flankDir.x(), flankDir.y());
            return 1;
        }
        if (flankApex.x() > loRect.left() + 2.0 || flankApex.x() < loRect.left() - 25.0 ||
            flankApex.y() < loRect.top() || flankApex.y() > loRect.bottom()) {
            std::fprintf(stderr, "FAIL: segment-dragging left-flank arrow is not at the node's left side\n");
            return 1;
        }
        if (!assertPillOnWire("left-flank")) {
            return 1;
        }
        if (!assertNoNodeCrossing(edgeItem, "left-flank")) {
            return 1;
        }
        const QRectF flankRegion = edgeItem->sceneBoundingRect().adjusted(-30.0, -30.0, 30.0, 30.0);
        if (!saveSceneRegionCapture(loginPane, flankRegion, "probe-segment-flank-attach")) {
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();

        // Top flank: the course runs above the node rightward to over its center.
        kernel.send(app::events::SetTransitionBendpointsRequested{
            .id = kResetTransitionId,
            .bendpoints = {QPointF(loRect.left() - 80.0, loRect.top() - 40.0),
                           QPointF(loRect.center().x(), loRect.top() - 40.0)},
        });
        QApplication::processEvents();
        flankDir = arrowDirOf();
        flankApex = arrowApexOf();
        if (flankDir.y() < 0.9) {
            std::fprintf(stderr,
                         "FAIL: segment-dragging top-flank course did not enter the Top port (dir %.2f, %.2f)\n",
                         flankDir.x(), flankDir.y());
            return 1;
        }
        if (flankApex.y() > loRect.top() + 2.0 || flankApex.y() < loRect.top() - 25.0 ||
            flankApex.x() < loRect.left() || flankApex.x() > loRect.right()) {
            std::fprintf(stderr, "FAIL: segment-dragging top-flank arrow is not at the node's top side\n");
            return 1;
        }
        if (!assertNoNodeCrossing(edgeItem, "top-flank")) {
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();

        // Keepout band with sticky clamp. Stage A: a course crossing above the
        // node closer than the minimum approach depth (e.g. top - 18px) stays
        // attached to the Top port, its horizontal run clamped at top - 27px,
        // arrow pointing down into Top.
        kernel.send(app::events::SetTransitionBendpointsRequested{
            .id = kResetTransitionId,
            .bendpoints = {QPointF(loRect.left() - 80.0, loRect.top() - 18.0),
                           QPointF(loRect.center().x(), loRect.top() - 18.0)},
        });
        QApplication::processEvents();
        flankDir = arrowDirOf();
        flankApex = arrowApexOf();
        if (flankDir.y() < 0.9) {
            std::fprintf(stderr,
                         "FAIL: segment-dragging keepout-clamp course did not stay on Top port (dir %.2f, %.2f)\n",
                         flankDir.x(), flankDir.y());
            return 1;
        }
        if (flankApex.y() > loRect.top() + 2.0 || flankApex.y() < loRect.top() - 25.0 ||
            flankApex.x() < loRect.left() || flankApex.x() > loRect.right()) {
            std::fprintf(stderr, "FAIL: segment-dragging keepout-clamp arrow is not at node top side (apex %.1f, %.1f)\n",
                         flankApex.x(), flankApex.y());
            return 1;
        }
        // Verify the horizontal run is clamped at top - 27.0 (kApproachKeepout = 27):
        {
            const QVector<QPointF> pts = app::canonicalWaypoints(edgeItem->debugRoute());
            bool hasClampedRun = false;
            for (int i = 0; i < pts.size() - 1; ++i) {
                if (std::abs(pts[i].y() - pts[i + 1].y()) < 0.01 &&
                    std::abs(pts[i].y() - (loRect.top() - 27.0)) < 0.5) {
                    hasClampedRun = true;
                    break;
                }
            }
            if (!hasClampedRun) {
                std::fprintf(stderr, "FAIL: segment-dragging keepout-clamp run did not clamp at top - 27px\n");
                return 1;
            }
        }
        if (!assertArrowIntoRect(edgeItem, loRect, "keepout-clamp")) {
            return 1;
        }
        if (!assertNoNodeCrossing(edgeItem, "keepout-clamp")) {
            return 1;
        }
        const QRectF clampRegion = edgeItem->sceneBoundingRect().adjusted(-30.0, -30.0, 30.0, 30.0);
        if (!saveSceneRegionCapture(loginPane, clampRegion, "probe-segment-keepout-clamp")) {
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();

        // Stage B: once the course crosses the node's top boundary plane (e.g.
        // top + 2px) it flips to the flank it crosses (Left port) and the run
        // relocates onto the port line (top + 12px).
        kernel.send(app::events::SetTransitionBendpointsRequested{
            .id = kResetTransitionId,
            .bendpoints = {QPointF(loRect.left() - 80.0, loRect.top() + 2.0),
                           QPointF(loRect.center().x(), loRect.top() + 2.0)},
        });
        QApplication::processEvents();
        flankDir = arrowDirOf();
        flankApex = arrowApexOf();
        if (flankDir.x() < 0.9) {
            std::fprintf(stderr,
                         "FAIL: segment-dragging keepout-flip course did not flip to the Left port (dir %.2f, %.2f)\n",
                         flankDir.x(), flankDir.y());
            return 1;
        }
        if (flankApex.x() > loRect.left() + 2.0 || flankApex.x() < loRect.left() - 25.0 ||
            flankApex.y() < loRect.top() || flankApex.y() > loRect.top() + 30.0) {
            std::fprintf(stderr,
                         "FAIL: segment-dragging keepout-flip arrow is not on the left port line (apex %.1f, %.1f)\n",
                         flankApex.x(), flankApex.y());
            return 1;
        }
        if (!assertArrowIntoRect(edgeItem, loRect, "keepout-flip")) {
            return 1;
        }
        if (!assertNoNodeCrossing(edgeItem, "keepout-flip")) {
            return 1;
        }
        const QRectF keepoutRegion = edgeItem->sceneBoundingRect().adjusted(-30.0, -30.0, 30.0, 30.0);
        if (!saveSceneRegionCapture(loginPane, keepoutRegion, "probe-segment-keepout-flip")) {
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();

        // Node-behind, resolvable: a course wholly beyond the node's right that
        // still reaches back; the overshoot self-clips and the wire backs into
        // the Right port, arrow pointing left.
        kernel.send(app::events::SetTransitionBendpointsRequested{
            .id = kResetTransitionId,
            .bendpoints = {QPointF(loRect.right() + 40.0, loRect.center().y()),
                           QPointF(loRect.right() + 100.0, loRect.center().y())},
        });
        QApplication::processEvents();
        flankDir = arrowDirOf();
        flankApex = arrowApexOf();
        if (flankDir.x() > -0.9) {
            std::fprintf(stderr,
                         "FAIL: segment-dragging beyond-right course did not back into the Right port "
                         "(dir %.2f, %.2f)\n",
                         flankDir.x(), flankDir.y());
            return 1;
        }
        if (!assertNoNodeCrossing(edgeItem, "beyond-right")) {
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();

        // Node-behind, degenerate: the course starts inside the approach zone
        // and travels away, so routeManual must fall back to the auto route:
        // no body crossing, arrow still aimed into the node, pill attached.
        kernel.send(app::events::SetTransitionBendpointsRequested{
            .id = kResetTransitionId,
            .bendpoints = {QPointF(loRect.right() + 5.0, loRect.center().y()),
                           QPointF(loRect.right() + 80.0, loRect.center().y())},
        });
        QApplication::processEvents();
        if (!assertNoNodeCrossing(edgeItem, "node-behind-fallback")) {
            return 1;
        }
        if (!assertArrowIntoRect(edgeItem, loRect, "node-behind-fallback")) {
            return 1;
        }
        if (!assertPillOnWire("node-behind-fallback")) {
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();

        const app::Transition* flankReverted = doc->findTransition(kResetTransitionId);
        if (flankReverted == nullptr || !flankReverted->manualBendpoints.isEmpty()) {
            std::fprintf(stderr, "FAIL: segment-dragging flank-attach unwind did not restore the baseline\n");
            return 1;
        }
    }

    // 8. Test dragging a straight vertical line sideways (Failure:
    // Authenticating -> Error, id 7). Every precondition hard-FAILs, so
    // fixture drift cannot silently skip the block.
    quint64 failureId = 0;
    for (const app::Transition& t : doc->machine().transitions) {
        if (t.from == 2 && t.to == 4) {
            failureId = t.id;
            break;
        }
    }
    if (failureId == 0) {
        std::fprintf(stderr, "FAIL: segment-dragging fixture lost the Failure transition (from=2, to=4)\n");
        return 1;
    }
    const app::TransitionItem* failureItem = presenter->debugTransitionItem(failureId);
    if (failureItem == nullptr) {
        std::fprintf(stderr, "FAIL: segment-dragging cannot find the Failure TransitionItem\n");
        return 1;
    }
    const QVector<QPointF> failureOrig = app::canonicalWaypoints(failureItem->debugRoute());
    if (failureOrig.size() < 2) {
        std::fprintf(stderr, "FAIL: segment-dragging Failure wire has no waypoints\n");
        return 1;
    }
    const QPointF failHitPoint = (failureOrig[0] + failureOrig[1]) * 0.5;
    const app::WireSegment failHit = failureItem->hitSegmentAt(failHitPoint);
    if (failHit.segmentIndex < 0) {
        std::fprintf(stderr, "FAIL: segment-dragging hitSegmentAt missed the Failure wire\n");
        return 1;
    }
    // Needs a vertical segment: a horizontal first Failure segment would turn
    // this into a parallel-drag test.
    if (failHit.orientation != app::WireSegment::Orientation::Vertical) {
        std::fprintf(stderr, "FAIL: segment-dragging Failure first segment is not vertical (fixture drift)\n");
        return 1;
    }
    presenter->debugSegmentDragStarted(failureId, failHit.segmentIndex, failHit.isSourceHalf,
                                       /*isHorizontal=*/false, failHitPoint);
    const QPointF failDragPos(failHitPoint.x() + 60.0, failHitPoint.y());
    presenter->debugSegmentDragMoved(failureId, failDragPos);
    QApplication::processEvents();

    // A straight vertical wire dragged sideways must keep its arrow aimed into
    // Error, not along the translate jog, mid-drag.
    app::StateItem* failureErrorItem = findStateItemById(scene, 4);
    if (failureErrorItem == nullptr) {
        std::fprintf(stderr, "FAIL: segment-dragging cannot find the Error StateItem for the sideways repro\n");
        return 1;
    }
    if (!assertArrowIntoRect(failureItem, failureErrorItem->sceneRect(), "failure-mid-drag")) {
        return 1;
    }

    presenter->debugSegmentDragFinished(failureId, failDragPos);
    QApplication::processEvents();

    const QVector<QPointF>& failNew = failureItem->debugRoute().waypoints;
    for (int i = 0; i < failNew.size() - 1; ++i) {
        const qreal dx = std::abs(failNew[i].x() - failNew[i + 1].x());
        const qreal dy = std::abs(failNew[i].y() - failNew[i + 1].y());
        if (dx > 0.01 && dy > 0.01) {
            std::fprintf(stderr, "FAIL: straight line drag waypoints not orthogonal\n");
            return 1;
        }
    }
    if (!assertPathHasNoDiagonals(failureItem->debugRoute().sourceHalf, "failure-sourceHalf")) return 1;
    if (!assertPathHasNoDiagonals(failureItem->debugRoute().targetHalf, "failure-targetHalf")) return 1;

    // 7f. A plain pill drag over an authored wire is offset-only: the
    // bendpoints survive, the skeleton never moves (mid-gesture or on commit),
    // and the single undo entry carries the labelOffset alone.
    const app::Transition* failureBendpointed = doc->findTransition(failureId);
    if (failureBendpointed == nullptr || failureBendpointed->manualBendpoints.isEmpty()) {
        std::fprintf(stderr, "FAIL: failure transition has no manualBendpoints before pill drag\n");
        return 1;
    }
    const QVector<QPointF> authoredBendpoints = failureBendpointed->manualBendpoints;
    const QPointF preDragOffset = failureBendpointed->labelOffset;
    const QVector<QPointF> authoredWire =
        app::canonicalWaypoints(presenter->debugTransitionItem(failureId)->debugRoute());
    const QPointF prePillPos = presenter->debugLabelCenter(failureId);
    // Drag pill away in 2D (e.g. +80px horizontal, -40px vertical):
    presenter->debugLabelDragMoved(failureId, QPointF(prePillPos.x() + 80.0, prePillPos.y() - 40.0));
    QApplication::processEvents();
    // Live preview must allow 2D departure (not clamped onto the line):
    const QPointF midPillPos = presenter->debugLabelCenter(failureId);
    if (std::abs(midPillPos.x() - (prePillPos.x() + 80.0)) > 5.0 ||
        std::abs(midPillPos.y() - (prePillPos.y() - 40.0)) > 5.0) {
        std::fprintf(stderr, "FAIL: pill drag live preview was constrained to wire line (expected %.1f,%.1f got %.1f,%.1f)\n",
                     prePillPos.x() + 80.0, prePillPos.y() - 40.0, midPillPos.x(), midPillPos.y());
        return 1;
    }
    if (app::canonicalWaypoints(presenter->debugTransitionItem(failureId)->debugRoute()) != authoredWire) {
        std::fprintf(stderr, "FAIL: the authored Failure course moved under a mid-flight pill drag\n");
        return 1;
    }
    // A plain drag shows the dashed leader from the wire's base anchor to the
    // displaced pill while it lasts ...
    if (!presenter->debugTransitionItem(failureId)->isTransientLeaderLineVisible()) {
        std::fprintf(stderr, "FAIL: a plain pill drag showed no leader line to the wire mid-drag\n");
        return 1;
    }
    {
        const QRectF leaderArea = QRectF(prePillPos, midPillPos).normalized().adjusted(-90.0, -60.0, 90.0, 60.0);
        if (!saveSceneRegionCapture(loginPane, leaderArea, "probe-pill-drag-leader")) {
            return 1;
        }
    }
    presenter->debugLabelDragFinished(failureId, midPillPos);
    QApplication::processEvents();
    // ... and hides it on release.
    if (presenter->debugTransitionItem(failureId)->isTransientLeaderLineVisible()) {
        std::fprintf(stderr, "FAIL: the plain pill drag's leader line stayed visible after release\n");
        return 1;
    }

    // The authored course survives the commit; only the offset changed:
    const app::Transition* failurePillMoved = doc->findTransition(failureId);
    if (failurePillMoved == nullptr || failurePillMoved->manualBendpoints != authoredBendpoints) {
        std::fprintf(stderr, "FAIL: a plain pill drag threw the authored Failure bendpoints away\n");
        return 1;
    }
    const QPointF committedOffset = failurePillMoved->labelOffset;
    if (committedOffset == preDragOffset) {
        std::fprintf(stderr, "FAIL: a plain pill drag committed no labelOffset\n");
        return 1;
    }
    if (app::canonicalWaypoints(presenter->debugTransitionItem(failureId)->debugRoute()) != authoredWire) {
        std::fprintf(stderr, "FAIL: committing the pill drag moved the authored Failure course\n");
        return 1;
    }

    // ONE undo, and it is the OFFSET that comes back -- never the course:
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    const app::Transition* failurePillUndone = doc->findTransition(failureId);
    if (failurePillUndone == nullptr || failurePillUndone->labelOffset != preDragOffset ||
        failurePillUndone->manualBendpoints != authoredBendpoints) {
        std::fprintf(stderr, "FAIL: undoing the pill drag did not restore the labelOffset alone\n");
        return 1;
    }

    // Redo re-applies that same offset, course still untouched:
    kernel.send(app::events::RedoRequested{});
    QApplication::processEvents();
    const app::Transition* failurePillRedone = doc->findTransition(failureId);
    if (failurePillRedone == nullptr || failurePillRedone->labelOffset != committedOffset ||
        failurePillRedone->manualBendpoints != authoredBendpoints) {
        std::fprintf(stderr, "FAIL: redoing the pill drag did not re-apply the labelOffset alone\n");
        return 1;
    }

    // Undo pill drag to return to the segment-dragged state:
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();

    // Revert the failure transition via Undo and verify it, so a broken
    // commit/undo pairing cannot leak a bent wire into later scenarios:
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    const app::Transition* failureReverted = doc->findTransition(failureId);
    if (failureReverted == nullptr || !failureReverted->manualBendpoints.isEmpty()) {
        std::fprintf(stderr, "FAIL: segment-dragging Failure undo did not clear manualBendpoints\n");
        return 1;
    }

    // 7g. Alt+drag on an event pill: both drags are offset-only (7f); Alt shows
    // the dashed leader line back to the wire only while dragging.
    const QVector<QPointF> preAltWaypoints = app::canonicalWaypoints(failureItem->debugRoute());
    const QPointF preAltPos = presenter->debugLabelCenter(failureId);

    // Start Alt-drag (+50px X, +30px Y):
    const QPointF altTargetPos = preAltPos + QPointF(50.0, 30.0);
    presenter->debugLabelDragMoved(failureId, altTargetPos, /*altOverride=*/true);
    QApplication::processEvents();

    if (!failureItem->isTransientLeaderLineVisible()) {
        std::fprintf(stderr, "FAIL: transient leader line not visible during active Alt-drag\n");
        return 1;
    }
    const QVector<QPointF> midAltWaypoints = app::canonicalWaypoints(failureItem->debugRoute());
    if (midAltWaypoints != preAltWaypoints) {
        std::fprintf(stderr, "FAIL: wire geometry deformed during Alt-drag (expected stationary)\n");
        return 1;
    }
    const QPointF midAltPos = presenter->debugLabelCenter(failureId);
    if (std::abs(midAltPos.x() - altTargetPos.x()) > 2.0 || std::abs(midAltPos.y() - altTargetPos.y()) > 2.0) {
        std::fprintf(stderr, "FAIL: pill position did not follow Alt-drag cursor\n");
        return 1;
    }

    // Release Alt-drag: leader line must disappear immediately, offset committed, wire stationary
    presenter->debugLabelDragFinished(failureId, altTargetPos, /*altOverride=*/true);
    QApplication::processEvents();

    if (failureItem->isTransientLeaderLineVisible()) {
        std::fprintf(stderr, "FAIL: transient leader line still visible after Alt-drag release\n");
        return 1;
    }
    const QVector<QPointF> postAltWaypoints = app::canonicalWaypoints(failureItem->debugRoute());
    if (postAltWaypoints != preAltWaypoints) {
        std::fprintf(stderr, "FAIL: wire geometry deformed after Alt-drag release (expected stationary)\n");
        return 1;
    }
    const app::Transition* failureAltCommitted = doc->findTransition(failureId);
    if (failureAltCommitted == nullptr || failureAltCommitted->labelOffset.isNull()) {
        std::fprintf(stderr, "FAIL: Alt-drag did not commit labelOffset\n");
        return 1;
    }

    // Context menu: "Reset Label Position" must be present when labelOffset is non-null
    QMenu* menuWithOffset = presenter->debugBuildTransitionContextMenu(failureId);
    if (menuWithOffset == nullptr) {
        std::fprintf(stderr, "FAIL: could not build transition context menu for Alt-dragged transition\n");
        return 1;
    }
    QAction* resetAction = nullptr;
    for (QAction* action : menuWithOffset->actions()) {
        if (action->text() == QStringLiteral("Reset Label Position")) {
            resetAction = action;
            break;
        }
    }
    if (resetAction == nullptr) {
        std::fprintf(stderr, "FAIL: 'Reset Label Position' action not found in context menu\n");
        delete menuWithOffset;
        return 1;
    }
    // Trigger reset:
    resetAction->trigger();
    delete menuWithOffset;
    QApplication::processEvents();

    const app::Transition* failureReset = doc->findTransition(failureId);
    if (failureReset == nullptr || !failureReset->labelOffset.isNull()) {
        std::fprintf(stderr, "FAIL: 'Reset Label Position' did not reset labelOffset to (0,0)\n");
        return 1;
    }
    const QPointF postResetPos = presenter->debugLabelCenter(failureId);
    if (std::abs(postResetPos.x() - preAltPos.x()) > 2.0 || std::abs(postResetPos.y() - preAltPos.y()) > 2.0) {
        std::fprintf(stderr, "FAIL: pill did not snap back to base position after reset\n");
        return 1;
    }

    // Context menu after reset: "Reset Label Position" must NOT be present
    QMenu* menuAfterReset = presenter->debugBuildTransitionContextMenu(failureId);
    if (menuAfterReset != nullptr) {
        for (QAction* action : menuAfterReset->actions()) {
            if (action->text() == QStringLiteral("Reset Label Position")) {
                std::fprintf(stderr, "FAIL: 'Reset Label Position' still present in menu after reset\n");
                delete menuAfterReset;
                return 1;
            }
        }
        delete menuAfterReset;
    }

    std::printf("PASS: gui-probe scenario segment-dragging (horizontal segment hit, parallel deltaY=+40 drag, "
                "straight vertical line Failure sideways deltaX=+60 drag, strict 90-deg orthogonality preserved, "
                "zero diagonal path elements, manualBendpoints saved, undo/redo verified, pill rides the wire "
                "mid-drag and post-commit, event name survives the bendpoint transaction, frame grows around an "
                "extreme drag and shrinks back on undo, arrowhead stays aimed into its target node mid-drag, "
                "parallel-segment merge snap shows the guide and merges on release, "
                "2D pill drag over an authored wire commits the offset alone and leaves the course, "
                "Option 2 Alt-drag transient leader line and stationary wire with Reset Label Position)\n");
    return 0;
}

// Scenario "machine-frame": the frame is selectable via its border/header only
// (interior stays click-through), a no-op delete target, draggable as a
// whole-machine move committed as one undo batch, and grown by an outward pill
// drag.
int runMachineFrameScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    QGraphicsScene* scene = loginPane->canvasView() != nullptr ? loginPane->canvasView()->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: machine-frame scenario preconditions (doc/presenter/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);

    app::MachineFrameItem* frame = findFrameItem(scene);
    if (frame == nullptr || !frame->isVisible()) {
        std::fprintf(stderr, "FAIL: no visible MachineFrameItem in the login-flow scene\n");
        return 1;
    }

    // ---- border hit surface selects; interior stays click-through -----------
    const QRectF frameRectBefore = frame->sceneFrameRect();
    const QPointF borderPoint(frameRectBefore.left() + 2.0, frameRectBefore.center().y());
    const QPointF interiorPoint = frameRectBefore.center();
    if (scene->itemAt(borderPoint, QTransform()) != frame) {
        std::fprintf(stderr, "FAIL: the frame's border band did not hit-test at (%.1f, %.1f)\n", borderPoint.x(),
                     borderPoint.y());
        return 1;
    }
    if (scene->itemAt(interiorPoint, QTransform()) == frame) {
        std::fprintf(stderr, "FAIL: the frame's interior hit-tests as the frame (should be click-through) at "
                              "(%.1f, %.1f)\n",
                     interiorPoint.x(), interiorPoint.y());
        return 1;
    }
    scene->clearSelection();
    frame->setSelected(true);
    QApplication::processEvents();
    if (presenter->currentSelection().kind != app::SelectionKind::Frame || presenter->currentSelection().id != 0) {
        std::fprintf(stderr, "FAIL: selecting the frame did not report (Frame, 0)\n");
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, frameRectBefore.adjusted(-16.0, -16.0, 16.0, 16.0),
                                 "probe-machine-frame-selected")) {
        return 1;
    }

    // ---- Delete with the frame selected is a no-op ---------------------------
    const int stateCountBeforeDelete = doc->machine().states.size();
    presenter->debugDeleteSelection();
    QApplication::processEvents();
    if (doc->machine().states.size() != stateCountBeforeDelete ||
        presenter->currentSelection().kind != app::SelectionKind::Frame) {
        std::fprintf(stderr, "FAIL: Delete with the frame selected mutated the machine or cleared selection\n");
        return 1;
    }

    // ---- whole-machine drag: every state moves by the SAME delta -------------
    QHash<quint64, QPointF> before;
    for (const app::State& state : doc->machine().states) {
        before.insert(state.id, state.pos);
    }
    const QPointF grabPoint = borderPoint;
    const QPointF delta(72.0, -48.0);  // both multiples of the 24px grid -- no soft-snap surprises
    presenter->debugFrameDragStarted(grabPoint);
    presenter->debugFrameDragMoved(grabPoint + delta);
    QApplication::processEvents();
    for (auto it = before.begin(); it != before.end(); ++it) {
        app::StateItem* item = findStateItemById(scene, it.key());
        if (item == nullptr || item->pos() != it.value() + delta) {
            std::fprintf(stderr, "FAIL: state %llu did not preview-move by the drag delta\n",
                         static_cast<unsigned long long>(it.key()));
            return 1;
        }
    }
    presenter->debugFrameDragFinished(grabPoint + delta);
    QApplication::processEvents();
    for (auto it = before.begin(); it != before.end(); ++it) {
        const app::State* state = doc->findState(it.key());
        if (state == nullptr || state->pos != it.value() + delta) {
            std::fprintf(stderr, "FAIL: state %llu's committed pos is not pre-drag pos + delta\n",
                         static_cast<unsigned long long>(it.key()));
            return 1;
        }
    }
    if (!saveSceneRegionCapture(loginPane, frame->sceneFrameRect().adjusted(-16.0, -16.0, 16.0, 16.0),
                                 "probe-machine-frame-dragged")) {
        return 1;
    }

    // ---- ONE undo restores EVERY state's pre-drag position --------------------
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    for (auto it = before.begin(); it != before.end(); ++it) {
        const app::State* state = doc->findState(it.key());
        if (state == nullptr || state->pos != it.value()) {
            std::fprintf(stderr, "FAIL: one undo did not restore state %llu's pre-drag position\n",
                         static_cast<unsigned long long>(it.key()));
            return 1;
        }
    }

    // ---- an outward pill drag grows the frame to contain it -------------------
    const QRectF frameRectBeforeGrowth = frame->sceneFrameRect();
    kernel.send(app::events::MoveTransitionLabelRequested{.id = 7, .offset = QPointF(500.0, 400.0)});
    QApplication::processEvents();
    const QRectF frameRectAfterGrowth = frame->sceneFrameRect();
    const QRectF pillRect = presenter->debugPillRect(7);
    if (!(frameRectAfterGrowth.width() > frameRectBeforeGrowth.width() ||
          frameRectAfterGrowth.height() > frameRectBeforeGrowth.height())) {
        std::fprintf(stderr, "FAIL: dragging pill 7 outward did not grow the frame\n");
        return 1;
    }
    if (!frameRectAfterGrowth.contains(pillRect)) {
        std::fprintf(stderr, "FAIL: the grown frame does not contain the dragged pill's rect\n");
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, frameRectAfterGrowth.adjusted(-16.0, -16.0, 16.0, 16.0),
                                 "probe-machine-frame-grown")) {
        return 1;
    }

    // ---- frame side ports: selecting the outermost machine shows four handles
    // ---- on its border, and a wire drag from one carries ROOT semantics -------
    scene->clearSelection();
    frame->setSelected(true);
    QApplication::processEvents();
    if (!frame->debugSideHandlesVisible()) {
        std::fprintf(stderr, "FAIL: selecting the machine frame did not show its four side handles\n");
        return 1;
    }
    {
        const QPointF expect = app::sidePortAnchor(frame->sceneFrameRect(), app::PortSide::Right, 0.0);
        const QPointF got = frame->debugHandleScenePos(app::PortSide::Right);
        if (std::hypot(got.x() - expect.x(), got.y() - expect.y()) > 0.5) {
            std::fprintf(stderr, "FAIL: the frame's Right handle does not sit on its border middle\n");
            return 1;
        }
    }
    if (!saveSceneRegionCapture(loginPane, frame->sceneFrameRect().adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-frame-ports")) {
        return 1;
    }
    // Drag onto state 1 -> a targeted root (fallback) transition; ONE undo.
    const quint64 kRootT = doc->machine().nextId;
    presenter->debugFrameWireDragStarted(app::PortSide::Right);
    app::StateItem* rootTarget = findStateItemById(scene, 1);
    if (rootTarget == nullptr) {
        std::fprintf(stderr, "FAIL: frame-wire fixture state 1 missing\n");
        return 1;
    }
    presenter->debugWireDragMoved(rootTarget->sceneRect().center());
    presenter->debugWireDragFinished(rootTarget->sceneRect().center());
    QApplication::processEvents();
    {
        const app::Transition* rootT = doc->findTransition(kRootT);
        if (rootT == nullptr || rootT->from != 0 || rootT->to != 1) {
            std::fprintf(stderr, "FAIL: a wire drag from the frame border did not commit a root transition to state 1\n");
            return 1;
        }
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findTransition(kRootT) != nullptr) {
        std::fprintf(stderr, "FAIL: ONE undo did not remove the frame-wire root transition\n");
        return 1;
    }
    // A stationary CLICK on the handle -> a targetless machine event (the
    // "Add Machine Event" verb with the pill-edit tail); ONE undo removes
    // the whole event+name batch (removal also cancels the opened editor).
    const quint64 kEvT = doc->machine().nextId;
    presenter->debugFrameWireDragStarted(app::PortSide::Right);
    presenter->debugWireDragFinished(app::sidePortAnchor(frame->sceneFrameRect(), app::PortSide::Right, 0.0));
    QApplication::processEvents();
    {
        const app::Transition* evT = doc->findTransition(kEvT);
        if (evT == nullptr || evT->from != 0 || evT->to != 0) {
            std::fprintf(stderr, "FAIL: a stationary click on a frame handle did not commit a machine event\n");
            return 1;
        }
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findTransition(kEvT) != nullptr) {
        std::fprintf(stderr, "FAIL: ONE undo did not remove the frame handle's machine-event batch\n");
        return 1;
    }

    // ---- side-aware handle click: a TOP-handle click births its pill ABOVE the frame
    const quint64 kTopT = doc->machine().nextId;
    presenter->debugFrameWireDragStarted(app::PortSide::Top);
    presenter->debugWireDragFinished(app::sidePortAnchor(frame->sceneFrameRect(), app::PortSide::Top, 0.0));
    QApplication::processEvents();
    {
        const app::Transition* topT = doc->findTransition(kTopT);
        const QRectF pillRect = presenter->debugPillRect(kTopT);
        if (topT == nullptr || topT->from != 0 || pillRect.isNull() ||
            pillRect.center().y() >= frame->sceneFrameRect().top()) {
            std::fprintf(stderr, "FAIL: a TOP frame-handle click did not birth its pill above the frame\n");
            return 1;
        }
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findTransition(kTopT) != nullptr) {
        std::fprintf(stderr, "FAIL: ONE undo did not remove the side-seeded machine-event batch\n");
        return 1;
    }

    // ---- machine self-transition: the frame box's ↺ births a
    // ---- #machine pill (arrow-into-frame), ONE undo removes the batch --------
    const quint64 kSelfT = doc->machine().nextId;
    presenter->debugAddMachineSelfTransition();
    QApplication::processEvents();
    {
        const app::Transition* selfT = doc->findTransition(kSelfT);
        if (selfT == nullptr || selfT->from != 0 || selfT->to != 0 || !selfT->machineSelf) {
            std::fprintf(stderr, "FAIL: the frame box's self verb did not birth a machineSelf root transition\n");
            return 1;
        }
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findTransition(kSelfT) != nullptr) {
        std::fprintf(stderr, "FAIL: ONE undo did not remove the machine-self batch\n");
        return 1;
    }

    // ---- restore the pre-scenario machine --------------------------------------
    kernel.send(app::events::UndoRequested{});  // the pill offset
    QApplication::processEvents();
    scene->clearSelection();
    QApplication::processEvents();
    if (doc->findTransition(7)->labelOffset != QPointF() ||
        presenter->currentSelection().kind != app::SelectionKind::None) {
        std::fprintf(stderr, "FAIL: the machine-frame scenario did not restore the machine it borrowed\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario machine-frame (border select, click-through interior, delete no-op, "
                "whole-machine drag + one-undo, pill growth, frame side ports + root wire drag + machine-event "
                "click, side-aware seeding, machine-self pill)\n");
    return 0;
}

// Scenario "rubber-band": a plain LMB drag over empty canvas band-selects a
// mixed set of states plus their connecting edge as one (Multi, 0) selection,
// and Delete removes the whole selection (cascaded transitions included) as one
// undo batch through the Multi action box's Delete verb. A press directly on an
// item never bands (Qt's QGraphicsItem::mousePressEvent claims it).
int runRubberBandScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || view == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: rubber-band scenario preconditions (doc/presenter/view/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    // ---- a plain LMB drag over EMPTY canvas bands; a band that sweeps nothing
    // ---- leaves no selection --------------------------------------------------
    const QPointF emptyFrom(-2000.0, -2000.0);  // far outside the machine frame -- guaranteed empty
    const QPointF emptyTo(-1500.0, -1500.0);
    view->debugMousePress(emptyFrom);  // no modifier
    if (view->dragMode() != QGraphicsView::RubberBandDrag) {
        std::fprintf(stderr, "FAIL: a plain LMB press over empty canvas did not switch to RubberBandDrag\n");
        return 1;
    }
    view->debugMouseMove(emptyTo);
    view->debugMouseRelease(emptyTo);
    QApplication::processEvents();
    if (view->dragMode() != QGraphicsView::NoDrag || presenter->currentSelection().kind != app::SelectionKind::None) {
        std::fprintf(stderr, "FAIL: an empty band left a selection or a non-NoDrag mode behind\n");
        return 1;
    }

    // ---- a press directly ON an item does not band ---------------------------
    app::StateItem* loggedOut = findStateItemById(scene, 1);        // LoggedOut (80,200)
    app::StateItem* authenticating = findStateItemById(scene, 2);   // Authenticating (320,180)
    app::MachineFrameItem* frame = findFrameItem(scene);
    if (loggedOut == nullptr || authenticating == nullptr || frame == nullptr) {
        std::fprintf(stderr, "FAIL: rubber-band scenario's fixture states/frame are missing from the scene\n");
        return 1;
    }
    view->debugMousePress(loggedOut->sceneRect().center());
    if (view->dragMode() != QGraphicsView::NoDrag) {
        std::fprintf(stderr, "FAIL: LMB directly on an item unexpectedly entered RubberBandDrag\n");
        return 1;
    }
    view->debugMouseRelease(loggedOut->sceneRect().center());
    QApplication::processEvents();
    scene->clearSelection();  // that press's own click-select, undone before the real band below
    QApplication::processEvents();

    // ---- Shift+LMB band-selects a mixed set: 2 states + their connecting edge --
    // Press well outside the machine frame: a press on its 8px border ring
    // counts as an item and refuses to start the band.
    const QRectF fixtureRect = loggedOut->sceneRect() | authenticating->sceneRect();
    const QPointF pressPoint = frame->sceneFrameRect().topLeft() - QPointF(80.0, 80.0);
    const QPointF releasePoint = fixtureRect.bottomRight() + QPointF(24.0, 24.0);
    const int stateCountBefore = doc->machine().states.size();
    const int transitionCountBefore = doc->machine().transitions.size();

    view->debugMousePress(pressPoint);  // plain LMB
    if (view->dragMode() != QGraphicsView::RubberBandDrag) {
        std::fprintf(stderr, "FAIL: a plain LMB press over empty canvas did not switch to RubberBandDrag\n");
        return 1;
    }
    view->debugMouseMove(releasePoint);
    view->debugMouseRelease(releasePoint);
    QApplication::processEvents();

    if (view->dragMode() != QGraphicsView::NoDrag) {
        std::fprintf(stderr, "FAIL: RubberBandDrag was not restored to NoDrag on release\n");
        return 1;
    }
    if (presenter->currentSelection().kind != app::SelectionKind::Multi || presenter->currentSelection().id != 0) {
        std::fprintf(stderr, "FAIL: the band did not report (Multi, 0) -- got kind=%d id=%llu\n",
                     static_cast<int>(presenter->currentSelection().kind),
                     static_cast<unsigned long long>(presenter->currentSelection().id));
        return 1;
    }
    if (!loggedOut->isSelected() || !authenticating->isSelected()) {
        std::fprintf(stderr, "FAIL: the band did not select both fixture states\n");
        return 1;
    }
    const app::TransitionItem* loginEdge = presenter->debugTransitionItem(5);  // LoggedOut -> Authenticating
    if (loginEdge == nullptr || !loginEdge->isSelected()) {
        std::fprintf(stderr, "FAIL: the band did not also select the connecting transition (id 5)\n");
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, fixtureRect.adjusted(-40.0, -40.0, 40.0, 40.0),
                                 "probe-rubber-band-selected")) {
        return 1;
    }

    // ---- the Multi action box: visible under the band's union rect with its
    // ---- verb row -------------------------------------------------------------
    std::vector<QString> multiLabels = presenter->debugActionBoxLabels();
    if (!presenter->debugActionBoxVisible() || multiLabels.size() != 3 ||
        multiLabels[0] != QStringLiteral("Zoom to Selection") || multiLabels[1] != QStringLiteral("Color") ||
        multiLabels[2] != QStringLiteral("Delete")) {
        std::fprintf(stderr, "FAIL: a Multi selection did not show the [Zoom to Selection][Color][Delete] box\n");
        return 1;
    }
    presenter->debugFireActionBoxVerb(app::ActionBoxVerb::ZoomToSelection);
    QApplication::processEvents();
    {
        const QPointF viewCenter = view->mapToScene(view->viewport()->rect().center());
        const QPointF unionCenter = fixtureRect.center();
        if (std::hypot(viewCenter.x() - unionCenter.x(), viewCenter.y() - unionCenter.y()) > 250.0) {
            std::fprintf(stderr, "FAIL: Zoom to Selection did not center the view on the band's union rect\n");
            std::fprintf(stderr, "  diag: view center (%.0f,%.0f), fixture center (%.0f,%.0f), zoom %.3f\n", viewCenter.x(),
                         viewCenter.y(), unionCenter.x(), unionCenter.y(), view->transform().m11());
            for (const QGraphicsItem* item : view->scene()->selectedItems()) {
                const QRectF r = item->sceneBoundingRect();
                std::fprintf(stderr, "  diag:   selected type %d rect (%.0f,%.0f %.0fx%.0f)\n", item->type(), r.x(), r.y(),
                             r.width(), r.height());
            }
            return 1;
        }
    }

    // ---- Color via the box's apply path: every selected element recolors as
    // ---- ONE undo batch --------------------------------------------------------
    presenter->debugApplySelectionColor(app::ElementColor::Green);
    QApplication::processEvents();
    if (doc->findState(1)->color != app::ElementColor::Green ||
        doc->findState(2)->color != app::ElementColor::Green ||
        doc->findTransition(5)->color != app::ElementColor::Green) {
        std::fprintf(stderr, "FAIL: the band-wide Color apply did not recolor both states + the edge\n");
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, fixtureRect.adjusted(-40.0, -40.0, 40.0, 40.0), "probe-multi-color")) {
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findState(1)->color != app::ElementColor::Default ||
        doc->findState(2)->color != app::ElementColor::Default ||
        doc->findTransition(5)->color != app::ElementColor::Default) {
        std::fprintf(stderr, "FAIL: ONE undo did not revert the band-wide recolor\n");
        return 1;
    }

    // ---- Delete VIA THE BOX VERB: the whole selection goes in ONE undo batch,
    // ---- cascades included --
    // Deleting states 1 and 2 cascades every transition (5-8), leaving only
    // LoggedIn and Error. A swept frame is skipped and a re-deleted cascaded
    // transition is a no-op.
    presenter->debugFireActionBoxVerb(app::ActionBoxVerb::DeleteSelection);
    QApplication::processEvents();
    if (doc->machine().states.size() != stateCountBefore - 2 || !doc->machine().transitions.isEmpty() ||
        doc->findState(1) != nullptr || doc->findState(2) != nullptr || doc->findState(3) == nullptr ||
        doc->findState(4) == nullptr) {
        std::fprintf(stderr, "FAIL: batch delete did not remove exactly the selected states + their cascade\n");
        return 1;
    }

    // ---- ONE undo restores every deleted state and transition ----------------
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->machine().states.size() != stateCountBefore || doc->machine().transitions.size() != transitionCountBefore ||
        doc->findState(1) == nullptr || doc->findState(2) == nullptr || doc->findTransition(5) == nullptr ||
        doc->findTransition(6) == nullptr || doc->findTransition(7) == nullptr || doc->findTransition(8) == nullptr) {
        std::fprintf(stderr, "FAIL: one undo did not restore every batch-deleted state/transition\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario rubber-band (plain-LMB band -> Multi, [Zoom to Selection][Delete] box, "
                "batch delete + cascade via the box verb, one undo, on-item presses stay NoDrag)\n");
    return 0;
}
