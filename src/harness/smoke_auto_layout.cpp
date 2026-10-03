#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHashFunctions>
#include <QIODevice>
#include <QJsonObject>
#include <QPair>
#include <QPointF>
#include <QString>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <optional>

#include "infra/project_io.h"
#include "infra/xstate_v5_io.h"
#include "model/machine.h"
#include "view/geometry/auto_layout.h"
#include "view/geometry/edge_router.h"
#include "view/geometry/node_anchors.h"
#include "view/geometry/pill_port_resolver.h"
#include "view/geometry/routing_logger.h"

#include "harness/harness.h"

namespace {

// The layered layout's fixed smoke metrics: leaf 160x64, pill
// 18px tall and 8 + 7px per event character wide, container header 28 --
// no fonts, so every number below is reproducible by hand.
app::AutoLayoutMetrics smokeLayoutMetrics() {
    app::AutoLayoutMetrics metrics;
    metrics.leafSize = [](const app::State&) { return QSizeF(160.0, 64.0); };
    metrics.containerHeaderHeight = [](const app::State&) { return 28.0; };
    metrics.labelSize = [](const app::Transition& transition) {
        return QSizeF(8.0 + 7.0 * transition.event.size(), 18.0);
    };
    return metrics;
}

struct StateSpec {
    quint64 id = 0;
    const char* name = "";
    quint64 parent = 0;
    quint64 initialChild = 0;
};
struct EdgeSpec {
    quint64 id = 0;
    quint64 from = 0;
    quint64 to = 0;
    const char* event = "";
};

// A geometry-free machine (every state at the origin) in the given document
// order.
app::Machine layoutFixture(const QVector<StateSpec>& states, const QVector<EdgeSpec>& edges, quint64 initial) {
    app::Machine machine;
    machine.name = QStringLiteral("AutoLayoutFixture");
    quint64 maxId = 0;
    for (const StateSpec& spec : states) {
        app::State state;
        state.id = spec.id;
        state.name = QString::fromLatin1(spec.name);
        state.parentId = spec.parent;
        state.initialChildId = spec.initialChild;
        machine.states.push_back(state);
        maxId = std::max(maxId, spec.id);
    }
    for (const EdgeSpec& spec : edges) {
        app::Transition transition;
        transition.id = spec.id;
        transition.from = spec.from;
        transition.to = spec.to;
        transition.event = QString::fromLatin1(spec.event);
        machine.transitions.push_back(transition);
        maxId = std::max(maxId, spec.id);
    }
    machine.initialStateId = initial;
    machine.nextId = maxId + 1;
    return machine;
}

const app::AutoLayoutResult::Edge* layoutEdge(const app::AutoLayoutResult& result, quint64 transitionId) {
    for (const app::AutoLayoutResult::Edge& edge : result.edges) {
        if (edge.transitionId == transitionId) {
            return &edge;
        }
    }
    return nullptr;
}

QPointF statePos(const app::Machine& machine, quint64 id) {
    for (const app::State& state : machine.states) {
        if (state.id == id) {
            return state.pos;
        }
    }
    return QPointF();
}

const char* edgeKindName(app::AutoLayoutResult::EdgeKind kind) {
    switch (kind) {
        case app::AutoLayoutResult::EdgeKind::Forward:
            return "forward";
        case app::AutoLayoutResult::EdgeKind::Backward:
            return "backward";
        case app::AutoLayoutResult::EdgeKind::SameColumn:
            return "same-column";
    }
    return "?";
}

const app::AutoLayoutResult::Channel* layoutChannel(const app::AutoLayoutResult& result, quint64 scope, int index) {
    for (const app::AutoLayoutResult::Channel& channel : result.channels) {
        if (channel.scope == scope && channel.index == index) {
            return &channel;
        }
    }
    return nullptr;
}

// Every state's scene rect as the canvas derives it, rebuilt here from the
// positions alone: a leaf is its pos plus the smoke's leaf size, a compound
// the hull around its children (kContainerHullPadding on every side, the
// children already sitting below the header), never narrower than a leaf.
QHash<quint64, QRectF> smokeSceneRects(const app::Machine& machine) {
    QHash<quint64, QRectF> rects;
    const std::function<QRectF(const app::State&)> rectOf = [&](const app::State& state) {
        if (rects.contains(state.id)) {
            return rects.value(state.id);
        }
        bool compound = false;
        qreal right = 0.0;
        qreal bottom = 0.0;
        for (const app::State& child : machine.states) {
            if (child.parentId == state.id) {
                const QRectF childRect = rectOf(child);
                right = compound ? std::max(right, childRect.right()) : childRect.right();
                bottom = compound ? std::max(bottom, childRect.bottom()) : childRect.bottom();
                compound = true;
            }
        }
        const QRectF rect =
            compound ? QRectF(state.pos, QSizeF(std::max(right + app::kContainerHullPadding - state.pos.x(), 160.0),
                                                bottom + app::kContainerHullPadding - state.pos.y()))
                     : QRectF(state.pos, QSizeF(160.0, 64.0));
        rects.insert(state.id, rect);
        return rect;
    };
    for (const app::State& state : machine.states) {
        rectOf(state);
    }
    return rects;
}

// One Normal transition's pill as the canvas will draw it from the laid-out
// machine: RoutingDelegate's gathering, anchoring and routing calls over
// the rects above, with the transition's own labelRatio.
struct SmokePill {
    quint64 transitionId = 0;
    QPointF base;
    QRectF rect;
};

QVector<SmokePill> smokeRoutedPills(const app::Machine& machine, const QHash<quint64, QRectF>& rects,
                                    const app::AutoLayoutMetrics& metrics) {
    const std::function<QRectF(quint64)> rectOf = [&rects](quint64 id) { return rects.value(id); };
    QVector<SmokePill> pills;
    for (const app::Transition& transition : machine.transitions) {
        if (transition.from == 0 || transition.to == 0 || transition.from == transition.to) {
            continue;
        }
        const auto fromAnchors = app::computeNodeAnchors(rects.value(transition.from),
                                                         app::touchingTransitionsFor(machine, transition.from, rectOf));
        const auto toAnchors = app::computeNodeAnchors(rects.value(transition.to),
                                                       app::touchingTransitionsFor(machine, transition.to, rectOf));
        const app::EndpointAnchor source = fromAnchors.value(transition.id);
        const app::EndpointAnchor target = toAnchors.value(transition.id);
        const app::RoutedEdge routed =
            app::routeUnifiedEdge(source.anchor, source.side, target.anchor, target.side, 10.0, QPointF(), 0.5,
                                  std::nullopt, std::nullopt, transition.labelRatio);
        const QSizeF size = metrics.labelSize(transition);
        pills.push_back(SmokePill{
            transition.id, routed.labelBase,
            QRectF(routed.labelBase - QPointF(size.width() / 2.0, size.height() / 2.0), size)});
    }
    return pills;
}

bool isAncestorState(const app::Machine& machine, quint64 ancestor, quint64 id) {
    for (;;) {
        quint64 parent = 0;
        for (const app::State& state : machine.states) {
            if (state.id == id) {
                parent = state.parentId;
            }
        }
        if (parent == 0) {
            return false;
        }
        if (parent == ancestor) {
            return true;
        }
        id = parent;
    }
}

QString eventOf(const app::Machine& machine, quint64 transitionId) {
    for (const app::Transition& transition : machine.transitions) {
        if (transition.id == transitionId) {
            return transition.event;
        }
    }
    return QString();
}

// Item 5's three assertions on a laid-out machine, as the canvas will draw
// it: no two pill rects intersect, every pill rect stays kPillNodeKeepout
// clear of every state box except its endpoints' compound ancestors, and
// the layout reported no residual. `focus` names the pills a FAIL is most
// likely about, so the message says where to look.
bool checkLabelPlacement(const char* fixture, const char* focus, const app::Machine& machine,
                         const app::AutoLayoutResult& result, const app::AutoLayoutMetrics& metrics) {
    const QHash<quint64, QRectF> rects = smokeSceneRects(machine);
    const QVector<SmokePill> pills = smokeRoutedPills(machine, rects, metrics);
    bool ok = true;
    for (int i = 0; i < pills.size(); ++i) {
        for (int j = i + 1; j < pills.size(); ++j) {
            if (pills.at(i).rect.intersects(pills.at(j).rect)) {
                std::fprintf(stderr, "FAIL: auto-layout smoke: %s: pills %llu '%s' and %llu '%s' intersect (%s)\n",
                             fixture, static_cast<unsigned long long>(pills.at(i).transitionId),
                             qUtf8Printable(eventOf(machine, pills.at(i).transitionId)),
                             static_cast<unsigned long long>(pills.at(j).transitionId),
                             qUtf8Printable(eventOf(machine, pills.at(j).transitionId)), focus);
                ok = false;
            }
        }
    }
    for (const SmokePill& pill : pills) {
        const QRectF keepout = pill.rect.adjusted(-app::kPillNodeKeepout, -app::kPillNodeKeepout,
                                                  app::kPillNodeKeepout, app::kPillNodeKeepout);
        quint64 from = 0;
        quint64 to = 0;
        for (const app::Transition& transition : machine.transitions) {
            if (transition.id == pill.transitionId) {
                from = transition.from;
                to = transition.to;
            }
        }
        for (const app::State& state : machine.states) {
            if (isAncestorState(machine, state.id, from) || isAncestorState(machine, state.id, to)) {
                continue;
            }
            if (keepout.intersects(rects.value(state.id))) {
                std::fprintf(stderr,
                             "FAIL: auto-layout smoke: %s: pill %llu '%s' is inside the %gpx keepout of '%s' (%s)\n",
                             fixture, static_cast<unsigned long long>(pill.transitionId),
                             qUtf8Printable(eventOf(machine, pill.transitionId)), app::kPillNodeKeepout,
                             qUtf8Printable(state.name), focus);
                ok = false;
            }
        }
    }
    for (const app::AutoLayoutResult::Residual& residual : result.residuals) {
        std::fprintf(stderr, "FAIL: auto-layout smoke: %s: residual on %llu '%s', overlap %.1f (%s)\n", fixture,
                     static_cast<unsigned long long>(residual.transitionId),
                     qUtf8Printable(eventOf(machine, residual.transitionId)), residual.overlap, focus);
        ok = false;
    }
    return ok;
}

// Checks one edge's recorded layers, class and channel; prints the FAIL.
bool expectEdge(const app::AutoLayoutResult& result, const char* fixture, quint64 id, int fromLayer, int toLayer,
                app::AutoLayoutResult::EdgeKind kind, int channel) {
    const app::AutoLayoutResult::Edge* edge = layoutEdge(result, id);
    if (edge == nullptr) {
        std::fprintf(stderr, "FAIL: auto-layout smoke: %s: transition %llu has no scope edge\n", fixture,
                     static_cast<unsigned long long>(id));
        return false;
    }
    if (edge->fromLayer != fromLayer || edge->toLayer != toLayer || edge->kind != kind || edge->channel != channel) {
        std::fprintf(stderr,
                     "FAIL: auto-layout smoke: %s: transition %llu is layers %d->%d %s channel %d, want %d->%d %s "
                     "channel %d\n",
                     fixture, static_cast<unsigned long long>(id), edge->fromLayer, edge->toLayer,
                     edgeKindName(edge->kind), edge->channel, fromLayer, toLayer, edgeKindName(kind), channel);
        return false;
    }
    return true;
}

const char* directionName(app::LayoutDirection direction) {
    switch (direction) {
        case app::LayoutDirection::LeftToRight:
            return "LR";
        case app::LayoutDirection::RightToLeft:
            return "RL";
        case app::LayoutDirection::TopToBottom:
            return "TB";
        case app::LayoutDirection::BottomToTop:
            return "BT";
    }
    return "?";
}

// A scene point's coordinate along `direction`'s flow: ranks must grow in it.
qreal primaryCoordinate(app::LayoutDirection direction, QPointF point) {
    switch (direction) {
        case app::LayoutDirection::LeftToRight:
            return point.x();
        case app::LayoutDirection::RightToLeft:
            return -point.x();
        case app::LayoutDirection::TopToBottom:
            return point.y();
        case app::LayoutDirection::BottomToTop:
            return -point.y();
    }
    return 0.0;
}

// The geometric half of a directional layout's contract, on the rects the
// canvas will derive: no two boxes overlap unless one encloses the other,
// every child sits inside its container's content area (below the header,
// kContainerHullPadding in from every side), and each of `rankChains` -- a
// chain of state ids along the flow -- advances along the primary axis, box
// center by box center.
bool checkDirectionalBoxes(const QString& label, app::LayoutDirection direction, const app::Machine& machine,
                           const QVector<QVector<quint64>>& rankChains) {
    const QHash<quint64, QRectF> rects = smokeSceneRects(machine);
    constexpr qreal kSmokeHeader = 28.0;  // smokeLayoutMetrics' container header
    bool ok = true;
    for (int i = 0; i < machine.states.size(); ++i) {
        const app::State& a = machine.states.at(i);
        for (int j = i + 1; j < machine.states.size(); ++j) {
            const app::State& b = machine.states.at(j);
            if (isAncestorState(machine, a.id, b.id) || isAncestorState(machine, b.id, a.id)) {
                continue;
            }
            if (rects.value(a.id).intersects(rects.value(b.id))) {
                std::fprintf(stderr, "FAIL: auto-layout smoke: %s: boxes '%s' and '%s' overlap\n",
                             qUtf8Printable(label), qUtf8Printable(a.name), qUtf8Printable(b.name));
                ok = false;
            }
        }
        if (a.parentId != 0) {
            const QRectF content = rects.value(a.parentId).adjusted(
                app::kContainerHullPadding, app::kContainerHullPadding + kSmokeHeader, -app::kContainerHullPadding,
                -app::kContainerHullPadding);
            if (!content.contains(rects.value(a.id))) {
                std::fprintf(stderr, "FAIL: auto-layout smoke: %s: child '%s' is not inside its container's content\n",
                             qUtf8Printable(label), qUtf8Printable(a.name));
                ok = false;
            }
        }
    }
    for (const QVector<quint64>& ranks : rankChains) {
        for (int i = 1; i < ranks.size(); ++i) {
            const qreal before = primaryCoordinate(direction, rects.value(ranks.at(i - 1)).center());
            const qreal after = primaryCoordinate(direction, rects.value(ranks.at(i)).center());
            if (!(after > before)) {
                std::fprintf(stderr,
                             "FAIL: auto-layout smoke: %s: state %llu does not advance along %s past state %llu "
                             "(%.2f after %.2f)\n",
                             qUtf8Printable(label), static_cast<unsigned long long>(ranks.at(i)),
                             directionName(direction), static_cast<unsigned long long>(ranks.at(i - 1)), after,
                             before);
                ok = false;
            }
        }
    }
    return ok;
}

// What a mirror pair is held to. By default: the whole layout mirrors, and
// every labelRatio is equal (the wires mirror, so the arc fraction does).
// Each weakening names its cause, and checkMirror prints it as a NOTE on
// every run, so it can never pass silently.
struct MirrorExpectation {
    // Compare each scope's children within their own extent instead of the
    // whole layout: a container keeps its header on top in every direction,
    // so a y-mirrored container's children sit one header lower than a
    // whole-layout mirror would put them.
    bool perScope = false;
    QVector<quint64> ratioExempt;  // transitions whose labelRatio the mirror cannot reproduce
    const char* why = "";
};

// `mirrored` is `base` reflected across the flow: along x (RL of LR) or y
// (BT of TB), each extent re-normalized to the origin -- every box's low
// edge lands where `base`'s high edge was, measured from the far side of the
// extent (the whole layout's, or with `perScope` its siblings'), and its
// cross coordinate is unchanged; every labelRatio outside `ratioExempt` is
// equal.
bool checkMirror(const QString& label, const app::Machine& base, const app::Machine& mirrored, bool alongX,
                 const MirrorExpectation& expectation) {
    constexpr qreal kPositionTolerance = 0.01;
    constexpr qreal kRatioTolerance = 1e-9;
    const QHash<quint64, QRectF> baseRects = smokeSceneRects(base);
    const QHash<quint64, QRectF> mirroredRects = smokeSceneRects(mirrored);
    // The extent a state is mirrored within: its siblings' union with
    // `perScope`, else the whole layout's.
    const auto extentOf = [&](const app::Machine& machine, const QHash<quint64, QRectF>& rects, quint64 parent) {
        QRectF extent;
        for (const app::State& state : machine.states) {
            if (!expectation.perScope || state.parentId == parent) {
                extent |= rects.value(state.id);
            }
        }
        return extent;
    };
    bool ok = true;
    if (expectation.perScope || !expectation.ratioExempt.isEmpty()) {
        std::printf("NOTE: auto-layout smoke: %s: mirror checked %s%s -- %s\n", qUtf8Printable(label),
                    expectation.perScope ? "per scope" : "over the whole layout",
                    expectation.ratioExempt.isEmpty() ? "" : ", some labelRatios exempt", expectation.why);
    }
    for (const app::State& state : base.states) {
        const QRectF rect = baseRects.value(state.id);
        const QRectF baseExtent = extentOf(base, baseRects, state.parentId);
        const QRectF mirroredExtent = extentOf(mirrored, mirroredRects, state.parentId);
        const QPointF want =
            alongX ? QPointF(mirroredExtent.left() + baseExtent.right() - rect.right(),
                             mirroredExtent.top() + rect.top() - baseExtent.top())
                   : QPointF(mirroredExtent.left() + rect.left() - baseExtent.left(),
                             mirroredExtent.top() + baseExtent.bottom() - rect.bottom());
        const QPointF got = statePos(mirrored, state.id);
        if (std::abs(got.x() - want.x()) > kPositionTolerance || std::abs(got.y() - want.y()) > kPositionTolerance) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: %s: '%s' at (%.3f, %.3f), the mirror wants (%.3f, %.3f)\n",
                         qUtf8Printable(label), qUtf8Printable(state.name), got.x(), got.y(), want.x(), want.y());
            ok = false;
        }
    }
    for (const app::Transition& transition : base.transitions) {
        const app::Transition* other = nullptr;
        for (const app::Transition& candidate : mirrored.transitions) {
            if (candidate.id == transition.id) {
                other = &candidate;
            }
        }
        const bool same = other != nullptr && transition.labelRatio.has_value() == other->labelRatio.has_value() &&
                          (!transition.labelRatio.has_value() ||
                           std::abs(*transition.labelRatio - *other->labelRatio) <= kRatioTolerance);
        const bool exempt = expectation.ratioExempt.contains(transition.id);
        if (exempt || !same) {
            std::fprintf(exempt ? stdout : stderr,
                         "%s: auto-layout smoke: %s: transition %llu '%s' labelRatio %.17g, the mirror has %.17g%s\n",
                         exempt ? "NOTE" : "FAIL", qUtf8Printable(label),
                         static_cast<unsigned long long>(transition.id), qUtf8Printable(transition.event),
                         transition.labelRatio.value_or(-1.0),
                         other != nullptr ? other->labelRatio.value_or(-1.0) : -1.0,
                         exempt && same ? " (equal now -- drop the exemption)" : exempt ? " (exempt)" : "");
            ok = ok && exempt;
        }
    }
    return ok;
}

// Total length of a waypoint polyline (edge_router.cpp's cumulativeLengths()
// helper is private to the router).
qreal polylineTotalLength(const QVector<QPointF>& waypoints) {
    qreal total = 0.0;
    for (int i = 0; i + 1 < waypoints.size(); ++i) {
        total += std::hypot(waypoints[i + 1].x() - waypoints[i].x(), waypoints[i + 1].y() - waypoints[i].y());
    }
    return total;
}

// Arc length from waypoints.front() to `point`, assuming `point` lies ON the
// polyline, which holds for routed.labelBase by construction (the router
// places it with its own pointAtLength()).
qreal arcLengthAlong(const QVector<QPointF>& waypoints, QPointF point) {
    qreal cumulative = 0.0;
    for (int i = 0; i + 1 < waypoints.size(); ++i) {
        const QPointF a = waypoints[i];
        const QPointF b = waypoints[i + 1];
        const QPointF v = b - a;
        const qreal segLen = std::hypot(v.x(), v.y());
        if (segLen > 1e-9) {
            const QPointF dir = v / segLen;
            const qreal t = std::clamp(QPointF::dotProduct(point - a, dir), 0.0, segLen);
            const QPointF proj = a + dir * t;
            if (std::hypot(point.x() - proj.x(), point.y() - proj.y()) <= 0.5) {
                return cumulative + t;
            }
        }
        cumulative += segLen;
    }
    return cumulative;  // point not found within tolerance -- caller compares against total
}

}  // namespace

// Transition::labelRatio at every boundary it crosses: the .sdm and XState v5
// round trips (7a/7b) and the pure router honoring an explicit ratio over the
// fraction/L-route defaults (7c). Existing call sites pass std::nullopt, so
// this also proves the field changed nothing else.
int runAutoLayoutSmoke() {
    using app::Machine;
    using app::PortSide;
    using app::RoutedEdge;
    using app::State;
    using app::StateKind;
    using app::Transition;

    // ---- 7a: `.sdm` round trip (labelRatio set + unset) ------------------------
    {
        Machine machine;
        machine.name = QStringLiteral("AutoLayoutRatio");
        machine.states.push_back(
            State{.id = 1, .name = QStringLiteral("A"), .kind = StateKind::Normal, .pos = QPointF(0, 0)});
        machine.states.push_back(
            State{.id = 2, .name = QStringLiteral("B"), .kind = StateKind::Normal, .pos = QPointF(200, 0)});
        machine.states.push_back(
            State{.id = 3, .name = QStringLiteral("C"), .kind = StateKind::Normal, .pos = QPointF(400, 0)});
        // One transition with a ratio the auto-layout would have projected,
        // one left at the router's own default (nullopt) -- both must
        // survive the round trip, including the unset one staying unset.
        machine.transitions.push_back(Transition{
            .id = 4, .from = 1, .to = 2, .event = QStringLiteral("GO"), .labelRatio = 0.35});
        machine.transitions.push_back(Transition{.id = 5, .from = 2, .to = 3, .event = QStringLiteral("NEXT")});
        machine.nextId = 6;
        machine.initialStateId = 1;

        const QString dir = QStringLiteral("temp/code/auto-layout-smoke");
        QDir().mkpath(dir);
        const QString path = dir + QStringLiteral("/round-trip.sdm");

        QString ioError;
        if (!app::saveMachine(machine, path, &ioError)) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: saveMachine failed: %s\n", qUtf8Printable(ioError));
            return 1;
        }

        QFile savedFile(path);
        if (!savedFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: could not reopen the saved .sdm for the labelRatio "
                                  "substring check\n");
            return 1;
        }
        const QString jsonText = QString::fromUtf8(savedFile.readAll());
        savedFile.close();
        const qsizetype occurrences = jsonText.count(QStringLiteral("labelRatio"));
        if (occurrences != 1) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: expected exactly one \"labelRatio\" key in the saved .sdm, "
                         "found %lld\n",
                         static_cast<long long>(occurrences));
            return 1;
        }

        Machine loaded;
        if (!app::loadMachine(path, &loaded, &ioError)) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: loadMachine failed: %s\n", qUtf8Printable(ioError));
            return 1;
        }
        if (!(loaded == machine)) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: .sdm round trip did not reproduce the original machine\n");
            return 1;
        }
        const Transition* unsetTransition = nullptr;
        for (const Transition& t : loaded.transitions) {
            if (t.id == 5) {
                unsetTransition = &t;
            }
        }
        if (unsetTransition == nullptr || unsetTransition->labelRatio.has_value()) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: unset transition's labelRatio should be nullopt after .sdm "
                         "load\n");
            return 1;
        }
    }

    // ---- 7b: XState v5 round trip (meta.ordo.labelRatio) -----------------------
    {
        Machine machine;
        machine.name = QStringLiteral("AutoLayoutXState");
        machine.states.push_back(
            State{.id = 1, .name = QStringLiteral("Idle"), .kind = StateKind::Normal, .pos = QPointF(0, 0)});
        machine.states.push_back(
            State{.id = 2, .name = QStringLiteral("Busy"), .kind = StateKind::Normal, .pos = QPointF(200, 0)});
        // A ratio-only transition -- labelOffset stays the (0,0) default, the
        // case: meta.ordo must appear even though
        // labelOffsetX/Y do not.
        machine.transitions.push_back(Transition{
            .id = 3, .from = 1, .to = 2, .event = QStringLiteral("GO"), .labelRatio = 0.7});
        machine.nextId = 4;
        machine.initialStateId = 1;

        const app::XStateExportResult exported = app::machineToXStateJson(machine);
        if (!exported.ok) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: xstate export refused a well-formed machine: %s\n",
                         qUtf8Printable(exported.error));
            return 1;
        }
        const QJsonObject idleOn = exported.json.value(QStringLiteral("states"))
                                       .toObject()
                                       .value(QStringLiteral("Idle"))
                                       .toObject()
                                       .value(QStringLiteral("on"))
                                       .toObject();
        const QJsonObject goTransition = idleOn.value(QStringLiteral("GO")).toObject();
        const QJsonObject ordo =
            goTransition.value(QStringLiteral("meta")).toObject().value(QStringLiteral("ordo")).toObject();
        if (!ordo.contains(QStringLiteral("labelRatio")) ||
            ordo.value(QStringLiteral("labelRatio")).toDouble() != 0.7) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: xstate export did not carry labelRatio in meta.ordo\n");
            return 1;
        }
        if (ordo.contains(QStringLiteral("labelOffsetX")) || ordo.contains(QStringLiteral("labelOffsetY"))) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: a ratio-only transition should not emit labelOffsetX/Y\n");
            return 1;
        }

        const app::XStateImportResult imported = app::machineFromXStateJson(exported.json);
        const Transition* importedGo = nullptr;
        for (const Transition& t : imported.machine.transitions) {
            if (t.event == QStringLiteral("GO")) {
                importedGo = &t;
            }
        }
        if (importedGo == nullptr || !importedGo->labelRatio.has_value() || *importedGo->labelRatio != 0.7) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: xstate import did not recover labelRatio\n");
            return 1;
        }

        // export(import(export(m))) == export(m); re-minted ids make Machine
        // deep-equality the wrong bar.
        const app::XStateExportResult reexported = app::machineToXStateJson(imported.machine);
        if (reexported.json != exported.json) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: export(import(export(m))) != export(m) for a labelRatio "
                         "transition\n");
            return 1;
        }
    }

    // ---- 7c: pure router -- labelRatio wins over fraction/L-route defaults ----
    {
        // (i) Z geometry: parallel-facing ports, ratio 0.25.
        const RoutedEdge zRoute = app::routeUnifiedEdge(QPointF(0, 0), PortSide::Right, QPointF(200, 100),
                                                         PortSide::Left, 10.0, QPointF(), 0.5, std::nullopt,
                                                         std::nullopt, 0.25);
        const qreal zTotal = polylineTotalLength(zRoute.waypoints);
        const qreal zArc = arcLengthAlong(zRoute.waypoints, zRoute.labelBase);
        if (std::abs(zArc - zTotal * 0.25) > 0.5) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: Z-route ratio 0.25 landed at arc %.3f, want %.3f (total %.3f)\n",
                         zArc, zTotal * 0.25, zTotal);
            return 1;
        }

        // (ii) L geometry: perpendicular ports (Right -> Top), a single-corner
        // L-route. No ratio must keep the longer-leg-midpoint rule; ratio 0.9
        // must override it, and the two must land at different points.
        const QPointF lSource(0, 0);
        const QPointF lTarget(200, 150);
        const RoutedEdge lBaseline = app::routeUnifiedEdge(lSource, PortSide::Right, lTarget, PortSide::Top, 10.0);
        if (lBaseline.waypoints.size() != 3) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: expected a single-corner L-route (3 waypoints), got %d\n",
                         static_cast<int>(lBaseline.waypoints.size()));
            return 1;
        }
        const qreal lSeg1 = std::hypot(lBaseline.waypoints[1].x() - lBaseline.waypoints[0].x(),
                                        lBaseline.waypoints[1].y() - lBaseline.waypoints[0].y());
        const qreal lSeg2 = std::hypot(lBaseline.waypoints[2].x() - lBaseline.waypoints[1].x(),
                                        lBaseline.waypoints[2].y() - lBaseline.waypoints[1].y());
        const qreal lExpectedBaselineArc = lSeg1 >= lSeg2 ? lSeg1 * 0.5 : lSeg1 + lSeg2 * 0.5;
        const qreal lBaselineArc = arcLengthAlong(lBaseline.waypoints, lBaseline.labelBase);
        if (std::abs(lBaselineArc - lExpectedBaselineArc) > 0.5) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: no-ratio L-route did not land on the longer leg's midpoint "
                         "(arc %.3f, want %.3f)\n",
                         lBaselineArc, lExpectedBaselineArc);
            return 1;
        }

        const RoutedEdge lWithRatio = app::routeUnifiedEdge(lSource, PortSide::Right, lTarget, PortSide::Top, 10.0,
                                                             QPointF(), 0.5, std::nullopt, std::nullopt, 0.9);
        const qreal lTotal = polylineTotalLength(lWithRatio.waypoints);
        const qreal lRatioArc = arcLengthAlong(lWithRatio.waypoints, lWithRatio.labelBase);
        if (std::abs(lRatioArc - lTotal * 0.9) > 0.5) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: L-route ratio 0.9 landed at arc %.3f, want %.3f (total %.3f)\n",
                         lRatioArc, lTotal * 0.9, lTotal);
            return 1;
        }
        if ((lWithRatio.labelBase - lBaseline.labelBase).manhattanLength() < 1.0) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: ratio 0.9 and the no-ratio longer-leg midpoint should differ\n");
            return 1;
        }

        // (iii) Omitting the trailing labelRatio argument must be
        // byte-identical to passing std::nullopt explicitly.
        const QPointF sourceAnchor(440.0, 192.0);
        const QPointF targetAnchor(560.0, 184.6);
        const RoutedEdge omitted = app::routeUnifiedEdge(sourceAnchor, PortSide::Right, targetAnchor, PortSide::Left,
                                                          10.0, QPointF(0.0, 0.0), 0.5);
        const RoutedEdge explicitNullopt =
            app::routeUnifiedEdge(sourceAnchor, PortSide::Right, targetAnchor, PortSide::Left, 10.0,
                                   QPointF(0.0, 0.0), 0.5, std::nullopt, std::nullopt, std::nullopt);
        if (omitted.waypoints != explicitNullopt.waypoints || omitted.labelBase != explicitNullopt.labelBase ||
            omitted.labelAnchor != explicitNullopt.labelAnchor) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: omitting the trailing labelRatio differs from passing "
                         "std::nullopt explicitly\n");
            return 1;
        }
    }

    // ---- Layered layout ----
    using EdgeKind = app::AutoLayoutResult::EdgeKind;
    const app::AutoLayoutMetrics metrics = smokeLayoutMetrics();

    // ---- 1: BFS layering -- a cycle closes backward, nothing skips a column ----
    {
        Machine cycle = layoutFixture({{1, "A"}, {2, "B"}, {3, "C"}}, {{4, 1, 2, "AB"}, {5, 2, 3, "BC"}, {6, 3, 1, "CA"}},
                                      1);
        const app::AutoLayoutResult result = app::applyAutoLayout(&cycle, metrics);
        if (!expectEdge(result, "3-cycle", 4, 0, 1, EdgeKind::Forward, 0) ||
            !expectEdge(result, "3-cycle", 5, 1, 2, EdgeKind::Forward, 1) ||
            !expectEdge(result, "3-cycle", 6, 2, 0, EdgeKind::Backward, 1)) {
            return 1;
        }
        if (!(statePos(cycle, 1).x() < statePos(cycle, 2).x() && statePos(cycle, 2).x() < statePos(cycle, 3).x())) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: 3-cycle columns are not A < B < C left to right\n");
            return 1;
        }

        // R -> {A, B}; A -> B joins two already-placed nodes of one column,
        // C -> A and C -> R lead back into placed nodes.
        Machine revisits = layoutFixture({{1, "R"}, {2, "A"}, {3, "B"}, {4, "C"}},
                                         {{10, 1, 2, "RA"},
                                          {11, 1, 3, "RB"},
                                          {12, 2, 3, "AB"},
                                          {13, 3, 4, "BC"},
                                          {14, 4, 2, "CA"},
                                          {15, 4, 1, "CR"},
                                          {16, 2, 4, "AC"}},
                                         1);
        const app::AutoLayoutResult revisitResult = app::applyAutoLayout(&revisits, metrics);
        for (const app::AutoLayoutResult::Edge& edge : revisitResult.edges) {
            if (edge.toLayer - edge.fromLayer > 1) {
                std::fprintf(stderr, "FAIL: auto-layout smoke: transition %llu skips a column (layers %d->%d)\n",
                             static_cast<unsigned long long>(edge.transitionId), edge.fromLayer, edge.toLayer);
                return 1;
            }
        }
        if (revisitResult.edges.size() != 7 ||
            !expectEdge(revisitResult, "revisits", 12, 1, 1, EdgeKind::SameColumn, -1) ||
            !expectEdge(revisitResult, "revisits", 14, 2, 1, EdgeKind::Backward, 1) ||
            !expectEdge(revisitResult, "revisits", 15, 2, 0, EdgeKind::Backward, 1) ||
            !expectEdge(revisitResult, "revisits", 16, 1, 2, EdgeKind::Forward, 1)) {
            return 1;
        }
    }

    // ---- 2: each edge class lands in its channel (the real flat fixture) -------
    {
        Machine flat;
        QString ioError;
        const QString flatPath = QStringLiteral("src/machines/fixtures/auto-layout-no-coordinates.sdm");
        if (!app::loadMachine(flatPath, &flat, &ioError)) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: could not load %s: %s\n", qUtf8Printable(flatPath),
                         qUtf8Printable(ioError));
            return 1;
        }
        const app::AutoLayoutResult result = app::applyAutoLayout(&flat, metrics);
        // Idle 0, Fetching 1, Analyzing and Failed 2, Report and Review 3.
        if (!expectEdge(result, "flat", 10, 0, 1, EdgeKind::Forward, 0) ||
            !expectEdge(result, "flat", 11, 1, 2, EdgeKind::Forward, 1) ||
            !expectEdge(result, "flat", 12, 1, 2, EdgeKind::Forward, 1) ||
            !expectEdge(result, "flat", 13, 2, 3, EdgeKind::Forward, 2) ||
            !expectEdge(result, "flat", 14, 2, 3, EdgeKind::Forward, 2) ||
            !expectEdge(result, "flat", 15, 2, 2, EdgeKind::SameColumn, -1) ||
            !expectEdge(result, "flat", 16, 3, 0, EdgeKind::Backward, 2) ||
            !expectEdge(result, "flat", 17, 3, 0, EdgeKind::Backward, 2) ||
            !expectEdge(result, "flat", 18, 2, 0, EdgeKind::Backward, 1)) {
            return 1;
        }
    }

    // ---- 3: barycenter uncrosses a two-pair crossing, keeps an uncrossed one ----
    {
        // R -> {A, B}, A -> D, B -> C with C before D in document order:
        // document order alone stacks C above D and the two wires cross.
        Machine crossed = layoutFixture({{1, "R"}, {2, "A"}, {3, "B"}, {4, "C"}, {5, "D"}},
                                        {{6, 1, 2, "RA"}, {7, 1, 3, "RB"}, {8, 2, 5, "AD"}, {9, 3, 4, "BC"}}, 1);
        app::applyAutoLayout(&crossed, metrics);
        const bool aAboveB = statePos(crossed, 2).y() < statePos(crossed, 3).y();
        const bool dAboveC = statePos(crossed, 5).y() < statePos(crossed, 4).y();
        if (!aAboveB || !dAboveC) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: crossing fixture still crossed (A above B: %d, D above C: %d)\n",
                         aAboveB, dAboveC);
            return 1;
        }
        // The same graph with D before C in document order is already
        // uncrossed: the order must survive the sweeps untouched.
        Machine uncrossed = layoutFixture({{1, "R"}, {2, "A"}, {3, "B"}, {5, "D"}, {4, "C"}},
                                          {{6, 1, 2, "RA"}, {7, 1, 3, "RB"}, {8, 2, 5, "AD"}, {9, 3, 4, "BC"}}, 1);
        app::applyAutoLayout(&uncrossed, metrics);
        if (!(statePos(uncrossed, 2).y() < statePos(uncrossed, 3).y() &&
              statePos(uncrossed, 5).y() < statePos(uncrossed, 4).y())) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: barycenter reordered an already uncrossed fixture\n");
            return 1;
        }
    }

    // The hierarchy case (the probe's own fixture): A -> Processing{Collect ->
    // Validate} -> B, where DONE leaves the child and ABORT the container,
    // both into B -- two nearly collinear wires through one root channel.
    const auto hierarchyFixture = [] {
        Machine machine = layoutFixture({{1, "A"}, {2, "Processing", 0, 3}, {3, "Collect", 2}, {4, "Validate", 2}, {5, "B"}},
                             {{6, 1, 2, "GO"}, {7, 3, 4, "NEXT"}, {8, 4, 5, "DONE"}, {9, 2, 5, "ABORT"}}, 1);
        machine.name = QStringLiteral("hierarchy");
        return machine;
    };
    // The flat case's shape: S -> X, X -> three stacked targets, and one
    // target leading back into X.
    const auto fanOutFixture = [] {
        Machine machine = layoutFixture({{1, "S"}, {2, "X"}, {3, "T1"}, {4, "T2"}, {5, "T3"}},
                             {{6, 1, 2, "GO"}, {7, 2, 3, "UP"}, {8, 2, 4, "ACROSS"}, {9, 2, 5, "DOWN"},
                              {10, 4, 2, "BACK"}},
                             1);
        machine.name = QStringLiteral("fan-out");
        return machine;
    };
    const auto loadFlat = [](Machine* flat) {
        QString ioError;
        if (!app::loadMachine(QStringLiteral("src/machines/fixtures/auto-layout-no-coordinates.sdm"), flat,
                              &ioError)) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: could not load the flat fixture: %s\n",
                         qUtf8Printable(ioError));
            return false;
        }
        return true;
    };
    const char* const flatFocus =
        "Fetching's 11 done.invoke.fetch / 12 error.platform.fetch, Analyzing's 13 / 14 done.invoke.analyze / 15 "
        "error.platform.analyze";

    // ---- 4: channel width -- collinear labels widen it, spread ones do not -----
    {
        Machine hierarchy = hierarchyFixture();
        const app::AutoLayoutResult result = app::applyAutoLayout(&hierarchy, metrics);
        const app::AutoLayoutResult::Channel* channel = layoutChannel(result, 0, 1);
        const qreal collinear = (8.0 + 7.0 * 4) + (8.0 + 7.0 * 5) + 24.0 + 40.0;  // DONE + ABORT + kLabelGap + clearance
        if (channel == nullptr || std::abs(channel->reservedWidth - collinear) > 1e-9 ||
            channel->high - channel->low < collinear) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: hierarchy root channel 1 (DONE + ABORT) reserved %.1f over a %.1f "
                         "strip, want %.1f\n",
                         channel != nullptr ? channel->reservedWidth : -1.0,
                         channel != nullptr ? channel->high - channel->low : -1.0, collinear);
            return 1;
        }

        // One compound source, two targets far apart in the next column: the
        // two labels sit on wires 100px apart and pass each other freely.
        Machine spread = layoutFixture({{1, "X", 0, 2}, {2, "C0", 1}, {3, "C1", 1}, {4, "C2", 1}, {5, "T1"}, {6, "T2"}},
                                       {{7, 2, 3, "A"},
                                        {8, 2, 4, "B"},
                                        {9, 1, 5, "FIRST_LONG_EVENT"},
                                        {10, 1, 6, "SECOND_EVENT"}},
                                       1);
        const app::AutoLayoutResult spreadResult = app::applyAutoLayout(&spread, metrics);
        const app::AutoLayoutResult::Channel* spreadChannel = layoutChannel(spreadResult, 0, 0);
        const qreal widestPillFloor = (8.0 + 7.0 * 16) + 40.0;  // FIRST_LONG_EVENT + clearance
        if (spreadChannel == nullptr || std::abs(spreadChannel->reservedWidth - widestPillFloor) > 1e-9) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: spread fan-out channel reserved %.1f, want the widest-pill floor "
                         "%.1f\n",
                         spreadChannel != nullptr ? spreadChannel->reservedWidth : -1.0, widestPillFloor);
            return 1;
        }
    }

    // ---- 5 / 5b: placement -- disjoint pills, clear of nodes, no residual ------
    {
        Machine fanOut = fanOutFixture();
        const app::AutoLayoutResult fanOutResult = app::applyAutoLayout(&fanOut, metrics);
        Machine hierarchy = hierarchyFixture();
        const app::AutoLayoutResult hierarchyResult = app::applyAutoLayout(&hierarchy, metrics);
        Machine flat;
        if (!loadFlat(&flat)) {
            return 1;
        }
        const app::AutoLayoutResult flatResult = app::applyAutoLayout(&flat, metrics);
        if (!checkLabelPlacement("fan-out", "X's UP / ACROSS / DOWN and T2's BACK", fanOut, fanOutResult, metrics) ||
            !checkLabelPlacement("hierarchy", "ABORT / DONE", hierarchy, hierarchyResult, metrics) ||
            !checkLabelPlacement("flat fixture", flatFocus, flat, flatResult, metrics)) {
            return 1;
        }
    }

    // ---- 6: labelRatio written, and the router reproduces the chosen point -----
    {
        Machine fanOut = fanOutFixture();
        Machine hierarchy = hierarchyFixture();
        Machine flat;
        if (!loadFlat(&flat)) {
            return 1;
        }
        for (Machine* machine : {&fanOut, &hierarchy, &flat}) {
            const app::AutoLayoutResult result = app::applyAutoLayout(machine, metrics);
            const QVector<SmokePill> pills = smokeRoutedPills(*machine, smokeSceneRects(*machine), metrics);
            for (const SmokePill& pill : pills) {
                const app::Transition* transition = nullptr;
                for (const app::Transition& candidate : machine->transitions) {
                    if (candidate.id == pill.transitionId) {
                        transition = &candidate;
                    }
                }
                const app::AutoLayoutResult::Label* label = nullptr;
                for (const app::AutoLayoutResult::Label& candidate : result.labels) {
                    if (candidate.transitionId == pill.transitionId) {
                        label = &candidate;
                    }
                }
                if (transition == nullptr || !transition->labelRatio.has_value() || *transition->labelRatio < 0.0 ||
                    *transition->labelRatio > 1.0 || label == nullptr) {
                    std::fprintf(stderr,
                                 "FAIL: auto-layout smoke: %s: Normal transition %llu has no labelRatio in [0,1] or "
                                 "no placed label\n",
                                 qUtf8Printable(machine->name), static_cast<unsigned long long>(pill.transitionId));
                    return 1;
                }
                const QPointF miss = pill.base - label->center;
                if (std::hypot(miss.x(), miss.y()) > 0.5) {
                    std::fprintf(stderr,
                                 "FAIL: auto-layout smoke: %s: transition %llu ratio %.6f routes its label to "
                                 "(%.2f, %.2f), the layout chose (%.2f, %.2f)\n",
                                 qUtf8Printable(machine->name), static_cast<unsigned long long>(pill.transitionId),
                                 *transition->labelRatio, pill.base.x(), pill.base.y(), label->center.x(),
                                 label->center.y());
                    return 1;
                }
            }
        }
    }

    // ---- 7: determinism -- the same input lays out the same, whatever the seed --
    {
        // Three runs under three qHash seeds (the process's random one, the
        // deterministic zero seed, a fresh random one): any order-sensitive
        // walk over a QHash/QSet would show up as a difference.
        Machine flat;
        if (!loadFlat(&flat)) {
            return 1;
        }
        const Machine sources[] = {flat, hierarchyFixture(), fanOutFixture()};
        for (const Machine& source : sources) {
            Machine runs[3] = {source, source, source};
            QVector<app::AutoLayoutResult::Label> labels[3];
            labels[0] = app::applyAutoLayout(&runs[0], metrics).labels;
            QHashSeed::setDeterministicGlobalSeed();
            labels[1] = app::applyAutoLayout(&runs[1], metrics).labels;
            QHashSeed::resetRandomGlobalSeed();
            labels[2] = app::applyAutoLayout(&runs[2], metrics).labels;
            for (int run = 1; run < 3; ++run) {
                bool sameLabels = labels[run].size() == labels[0].size();
                for (int i = 0; sameLabels && i < labels[0].size(); ++i) {
                    sameLabels = labels[run].at(i).transitionId == labels[0].at(i).transitionId &&
                                 labels[run].at(i).center == labels[0].at(i).center &&
                                 labels[run].at(i).ratio == labels[0].at(i).ratio;
                }
                if (!(runs[run] == runs[0]) || !sameLabels) {
                    std::fprintf(stderr,
                                 "FAIL: auto-layout smoke: %s: run %d under another qHash seed laid out differently\n",
                                 qUtf8Printable(source.name), run + 1);
                    return 1;
                }
            }
        }
    }

    // ---- 8: the layout run itself is silent ----
    // A silenced applyAutoLayout run must not grow the routing trace log, even
    // though it routes every Normal transition's wire twice (provisional, then
    // final). The unguarded control call below proves logging is on at all.
    {
        const QString logPath = app::routingTraceLogPath();
        const qint64 sizeBefore = QFileInfo(logPath).size();  // 0 when the file does not exist yet

        Machine flat;
        if (!loadFlat(&flat)) {
            return 1;
        }
        app::applyAutoLayout(&flat, metrics);
        const qint64 sizeAfterLayout = QFileInfo(logPath).size();
        if (sizeAfterLayout != sizeBefore) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: %s grew from %lld to %lld bytes during a "
                         "silenced applyAutoLayout run\n",
                         qUtf8Printable(logPath), static_cast<long long>(sizeBefore),
                         static_cast<long long>(sizeAfterLayout));
            return 1;
        }

#if !defined(SD_ENABLE_LOGGING) || (SD_ENABLE_LOGGING != 0)
        // Control: the same router entry point, called with no
        // RoutingLogSilencer on the stack, must still write to the log.
        app::routeUnifiedEdge(QPointF(0, 0), PortSide::Right, QPointF(200, 100), PortSide::Left, 10.0);
        const qint64 sizeAfterControl = QFileInfo(logPath).size();
        if (sizeAfterControl <= sizeAfterLayout) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: an unguarded routeUnifiedEdge call did not grow "
                         "%s (%lld -> %lld bytes)\n",
                         qUtf8Printable(logPath), static_cast<long long>(sizeAfterLayout),
                         static_cast<long long>(sizeAfterControl));
            return 1;
        }
#endif
    }

    // ---- 9: golden LR -- the flat fixture's output, pinned --------------------
    // The left-to-right default is pinned: every state's pos exactly, every
    // labelRatio to 1e-9, under the smoke metrics.
    {
        Machine flat;
        if (!loadFlat(&flat)) {
            return 1;
        }
        app::applyAutoLayout(&flat, metrics);
        struct GoldenState {
            quint64 id;
            const char* name;
            QPointF pos;
        };
        const GoldenState goldenStates[] = {
            {4, "Idle", QPointF(0, 72)},        {5, "Fetching", QPointF(264, 72)}, {6, "Analyzing", QPointF(840, 0)},
            {7, "Report", QPointF(1416, 24)},   {8, "Review", QPointF(1416, 144)}, {9, "Failed", QPointF(840, 144)},
        };
        const QPair<quint64, qreal> goldenRatios[] = {
            {10, 0.55952380952380953}, {11, 0.19509345794392524}, {12, 0.57242990654205606},
            {13, 0.2196969696969697},  {14, 0.504},               {15, 0.5},
            {16, 0.030546623794212219}, {17, 0.15809968847352024}, {18, 0.059971098265895952},
        };
        constexpr qreal kRatioTolerance = 1e-9;
        if (flat.states.size() != static_cast<qsizetype>(std::size(goldenStates))) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: golden LR: flat fixture has %lld states, want %zu\n",
                         static_cast<long long>(flat.states.size()), std::size(goldenStates));
            return 1;
        }
        for (const GoldenState& golden : goldenStates) {
            const QPointF pos = statePos(flat, golden.id);
            if (pos != golden.pos) {
                std::fprintf(stderr, "FAIL: auto-layout smoke: golden LR: %s at (%.17g, %.17g), want (%g, %g)\n",
                             golden.name, pos.x(), pos.y(), golden.pos.x(), golden.pos.y());
                return 1;
            }
        }
        int normalCount = 0;
        for (const Transition& transition : flat.transitions) {
            if (transition.from != 0 && transition.to != 0 && transition.from != transition.to) {
                ++normalCount;
            }
        }
        if (normalCount != static_cast<int>(std::size(goldenRatios))) {
            std::fprintf(stderr, "FAIL: auto-layout smoke: golden LR: %d Normal transitions, want %zu\n", normalCount,
                         std::size(goldenRatios));
            return 1;
        }
        for (const auto& golden : goldenRatios) {
            const Transition* transition = nullptr;
            for (const Transition& candidate : flat.transitions) {
                if (candidate.id == golden.first) {
                    transition = &candidate;
                }
            }
            if (transition == nullptr || !transition->labelRatio.has_value() ||
                std::abs(*transition->labelRatio - golden.second) > kRatioTolerance) {
                std::fprintf(stderr, "FAIL: auto-layout smoke: golden LR: transition %llu labelRatio %.17g, want %.17g\n",
                             static_cast<unsigned long long>(golden.first),
                             transition != nullptr ? transition->labelRatio.value_or(-1.0) : -1.0, golden.second);
                return 1;
            }
        }
    }

    // ---- 10: directions -- RL / TB / BT on the flat and hierarchy fixtures ----
    // Each direction keeps the layout contract (disjoint boxes, children inside
    // containers, keepout-clear pills, no residual, ranks advancing along the
    // primary axis), is deterministic, and its pure plan equals what
    // applyAutoLayout writes. RL mirrors LR along x and BT mirrors TB along y
    // unless a fixture's MirrorExpectation names a cause (printed as a NOTE).
    {
        using app::LayoutDirection;
        Machine flat;
        if (!loadFlat(&flat)) {
            return 1;
        }
        struct DirectionFixture {
            Machine source;
            const char* focus;
            QVector<QVector<quint64>> rankChains;  // each chain's states must advance along the flow
            MirrorExpectation rlOfLr;
            MirrorExpectation btOfTb;
        };
        const DirectionFixture fixtures[] = {
            {flat,
             flatFocus,
             {{4, 5, 6, 7}},  // Idle -> Fetching -> Analyzing -> Report
             // RESET and RETRY both enter Idle's Bottom side clamped to one
             // slot; the id tie-break nudges toward +x in either direction,
             // so RL swaps their anchors instead of mirroring them.
             {false, {17, 18},
              "computeNodeAnchors' slot tie-break (along, then id; nudged toward +along) is not mirror-symmetric"},
             {}},
            {hierarchyFixture(),
             "ABORT / DONE",
             {{1, 2, 5}, {3, 4}},  // A -> Processing -> B; Collect -> Validate
             {},
             {true, {8},
              "Processing's header stays on top, so BT's children sit one header lower than a y-mirror and DONE's "
              "wire from Validate is not TB's mirrored"}},
        };
        const LayoutDirection directions[] = {LayoutDirection::LeftToRight, LayoutDirection::RightToLeft,
                                              LayoutDirection::TopToBottom, LayoutDirection::BottomToTop};
        for (const DirectionFixture& fixture : fixtures) {
            Machine laidOut[4];
            for (int d = 0; d < 4; ++d) {
                const app::AutoLayoutOptions options{directions[d]};
                const QString label = fixture.source.name + QStringLiteral(" ") +
                                      QString::fromLatin1(directionName(directions[d]));
                laidOut[d] = fixture.source;
                const app::AutoLayoutResult result = app::applyAutoLayout(&laidOut[d], metrics, options);
                const bool boxesOk = checkDirectionalBoxes(label, directions[d], laidOut[d], fixture.rankChains);
                bool ok = checkLabelPlacement(qUtf8Printable(label), fixture.focus, laidOut[d], result, metrics) &&
                          boxesOk;

                // Deterministic, and the plan is the write.
                Machine again = fixture.source;
                const app::AutoLayoutResult againResult = app::applyAutoLayout(&again, metrics, options);
                bool sameLabels = againResult.labels.size() == result.labels.size();
                for (int i = 0; sameLabels && i < result.labels.size(); ++i) {
                    sameLabels = againResult.labels.at(i).transitionId == result.labels.at(i).transitionId &&
                                 againResult.labels.at(i).center == result.labels.at(i).center &&
                                 againResult.labels.at(i).ratio == result.labels.at(i).ratio;
                }
                if (!(again == laidOut[d]) || !sameLabels) {
                    std::fprintf(stderr, "FAIL: auto-layout smoke: %s: a second run laid out differently\n",
                                 qUtf8Printable(label));
                    ok = false;
                }
                const Machine untouched = fixture.source;
                const app::AutoLayoutPlan plan = app::computeAutoLayoutPlan(untouched, metrics, options);
                Machine fromPlan = fixture.source;
                for (const app::StatePlacement& placement : plan.states) {
                    for (State& state : fromPlan.states) {
                        if (state.id == placement.id) {
                            state.pos = placement.pos;
                        }
                    }
                }
                for (const app::LabelPlacement& placement : plan.labels) {
                    for (Transition& transition : fromPlan.transitions) {
                        if (transition.id == placement.id) {
                            transition.labelRatio = placement.ratio;
                        }
                    }
                }
                if (!(untouched == fixture.source) || !(fromPlan == laidOut[d]) ||
                    plan.states.size() != fixture.source.states.size()) {
                    std::fprintf(stderr,
                                 "FAIL: auto-layout smoke: %s: computeAutoLayoutPlan touched its input or planned "
                                 "something applyAutoLayout did not write\n",
                                 qUtf8Printable(label));
                    ok = false;
                }
                if (!ok) {
                    return 1;
                }
            }
            const bool rlMirrorsLr = checkMirror(fixture.source.name + QStringLiteral(" RL vs LR"), laidOut[0],
                                                 laidOut[1], true, fixture.rlOfLr);
            const bool btMirrorsTb = checkMirror(fixture.source.name + QStringLiteral(" BT vs TB"), laidOut[2],
                                                 laidOut[3], false, fixture.btOfTb);
            if (!rlMirrorsLr || !btMirrorsTb) {
                return 1;
            }
        }
    }

    // ---- 11: idempotence -- default options on a laid-out machine plan nothing new ----
    // Auto Layout with default options on an already laid-out machine must plan
    // every state and label exactly where it is (bitwise), since
    // ApplyLayoutPlanCommand skips only exactly-equal entries; that is what
    // makes such a run record zero undo ops.
    {
        Machine flat;
        if (!loadFlat(&flat)) {
            return 1;
        }
        app::applyAutoLayout(&flat, metrics);
        const app::AutoLayoutPlan plan = app::computeAutoLayoutPlan(flat, metrics);
        int differing = 0;
        for (const app::StatePlacement& placement : plan.states) {
            const auto it = std::find_if(flat.states.cbegin(), flat.states.cend(),
                                         [&placement](const State& state) { return state.id == placement.id; });
            if (it == flat.states.cend() || it->pos.x() != placement.pos.x() || it->pos.y() != placement.pos.y()) {
                ++differing;
            }
        }
        for (const app::LabelPlacement& placement : plan.labels) {
            const auto it = std::find_if(flat.transitions.cbegin(), flat.transitions.cend(),
                                         [&placement](const Transition& transition) {
                                             return transition.id == placement.id;
                                         });
            if (it == flat.transitions.cend() || it->labelRatio != placement.ratio) {
                ++differing;
            }
        }
        if (differing != 0 || plan.states.size() != flat.states.size() || plan.labels.isEmpty()) {
            std::fprintf(stderr,
                         "FAIL: auto-layout smoke: default options on the laid-out flat fixture planned %d "
                         "change(s) (want 0; %d state and %d label placements)\n",
                         differing, static_cast<int>(plan.states.size()), static_cast<int>(plan.labels.size()));
            return 1;
        }
    }

    std::printf(
        "PASS: state-designer auto-layout labelRatio smoke (.sdm round trip + XState v5 round trip + pure "
        "router ratio precedence over fraction/L-route defaults; layered layout: BFS layers, edge classes "
        "and channels, barycenter uncrossing, collinear channel widths, along-wire label placement with no "
        "overlap/keepout/residual on the fan-out, hierarchy and real flat fixtures, labelRatio reproduced by the "
        "router, seed-independent determinism; routing_trace-<version>.log does not grow during a layout run, "
        "unguarded control call does; golden LR positions and labelRatios of the flat fixture pinned; RL / TB / BT "
        "on the flat and hierarchy fixtures: disjoint boxes, children inside containers, disjoint keepout-clear "
        "pills, no residual, ranks monotone along the primary axis, two runs identical, pure plan == applied "
        "write, RL an x-mirror of LR and BT a y-mirror of TB; default options on the laid-out flat fixture plan "
        "zero changes)\n");
    return 0;
}
