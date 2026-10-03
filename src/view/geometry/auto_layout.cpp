#include "view/geometry/auto_layout.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>

#include <QHash>
#include <QPair>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QVector>

#include "model/machine.h"
#include "view/geometry/edge_router.h"
#include "view/geometry/node_anchors.h"
#include "view/geometry/pill_port_resolver.h"  // kPillNodeKeepout
#include "view/geometry/routing_logger.h"      // RoutingLogSilencer

namespace app {

namespace {

constexpr qreal kGridStep = 24.0;       // the canvas grid; corners snap onto it in (p, s)
constexpr qreal kPillClearance = 40.0;  // the pill's node keepout (kPillNodeKeepout), on both sides
constexpr qreal kLabelGap = 24.0;       // one grid step between two pills
constexpr qreal kScanStep = 2.0;        // arc-length step of the along-wire placement scan
constexpr qreal kAxisTolerance = 0.01;  // a segment this close to an axis runs along it
constexpr int kBarycenterRounds = 4;    // down+up sweeps before the crossing reduction gives up

using EdgeKind = AutoLayoutResult::EdgeKind;
using RectOf = std::function<QRectF(quint64)>;
using AnchorCache = QHash<quint64, QHash<quint64, EndpointAnchor>>;  // by state id; lookup only

qreal snapUp(qreal value) { return std::ceil(value / kGridStep) * kGridStep; }

// A transition the router draws node to node: both ends set, not a self
// loop. Only these get a channel, a placed label and a labelRatio.
bool isNormal(const Transition& transition) {
    return transition.from != 0 && transition.to != 0 && transition.from != transition.to;
}

QRectF pillRectAt(QPointF center, QSizeF size) {
    return QRectF(center - QPointF(size.width() / 2.0, size.height() / 2.0), size);
}

bool runsHorizontally(QPointF a, QPointF b) {
    return std::abs(a.y() - b.y()) < kAxisTolerance && std::abs(a.x() - b.x()) >= kAxisTolerance;
}

// A closed interval along one axis.
struct Span {
    qreal min = 0.0;
    qreal max = 0.0;
};

// The flow's two axes: primary p along the flow (rank progression, layer gap,
// channel strips), secondary s across it (order within a rank, node gap, pill
// pitch on one bar). LR: p = +x, s = y; RL: p = -x, s = y; TB: p = +y, s = x;
// BT: p = -y, s = x. Each mapping is an exact swap or negation, so LR is the
// identity. A (p, s) pair travels as QPointF(p, s).
class Axes {
public:
    explicit Axes(LayoutDirection direction)
        : transposed_(direction == LayoutDirection::TopToBottom || direction == LayoutDirection::BottomToTop),
          reversed_(direction == LayoutDirection::RightToLeft || direction == LayoutDirection::BottomToTop) {}

    QPointF toPS(QPointF scene) const {
        const qreal along = transposed_ ? scene.y() : scene.x();
        return QPointF(reversed_ ? -along : along, transposed_ ? scene.x() : scene.y());
    }
    QPointF fromPS(qreal p, qreal s) const {
        const qreal along = reversed_ ? -p : p;
        return transposed_ ? QPointF(s, along) : QPointF(along, s);
    }
    Span pExtent(const QRectF& rect) const {
        const qreal low = transposed_ ? rect.top() : rect.left();
        const qreal high = transposed_ ? rect.bottom() : rect.right();
        return reversed_ ? Span{-high, -low} : Span{low, high};
    }
    Span sExtent(const QRectF& rect) const {
        return transposed_ ? Span{rect.left(), rect.right()} : Span{rect.top(), rect.bottom()};
    }
    qreal pSize(QSizeF size) const { return transposed_ ? size.height() : size.width(); }
    qreal sSize(QSizeF size) const { return transposed_ ? size.width() : size.height(); }

    // The scene top-left of a box of scene size `size` whose low (p, s)
    // corner is `low`: on a reversed axis the box's scene-low edge is its
    // high-p edge.
    QPointF topLeft(QPointF low, QSizeF size) const {
        return fromPS(reversed_ ? low.x() + pSize(size) : low.x(), low.y());
    }
    // A primary-axis span back in scene coordinates -- x for LR/RL, y for
    // TB/BT -- low to high.
    Span sceneSpan(Span p) const { return reversed_ ? Span{-p.max, -p.min} : p; }

    // A segment that runs along the secondary axis (a bar across the flow),
    // or along the primary one (a leg with the flow).
    bool runsAlongS(QPointF a, QPointF b) const {
        const QPointF d = toPS(b) - toPS(a);
        return std::abs(d.x()) < kAxisTolerance && std::abs(d.y()) >= kAxisTolerance;
    }
    bool runsAlongP(QPointF a, QPointF b) const {
        const QPointF d = toPS(b) - toPS(a);
        return std::abs(d.y()) < kAxisTolerance && std::abs(d.x()) >= kAxisTolerance;
    }

private:
    bool transposed_ = false;  // TB/BT: p runs along scene y
    bool reversed_ = false;    // RL/BT: p grows against the scene axis
};

// The router's polyline for one transition, with cumulative arc lengths.
struct Wire {
    QVector<QPointF> points;
    QVector<qreal> cumulative;

    bool isValid() const { return points.size() >= 2; }
    qreal total() const { return cumulative.isEmpty() ? 0.0 : cumulative.back(); }

    // The point at arc length `arc`; walks like edge_router's pointAtLength, so a
    // ratio written from this arc lands labelBase on this exact point.
    QPointF pointAt(qreal arc) const {
        arc = std::clamp(arc, 0.0, total());
        int i = 1;
        while (i < cumulative.size() - 1 && cumulative.at(i) < arc) {
            ++i;
        }
        const qreal length = cumulative.at(i) - cumulative.at(i - 1);
        const qreal t = length > 1e-9 ? (arc - cumulative.at(i - 1)) / length : 0.0;
        return points.at(i - 1) + (points.at(i) - points.at(i - 1)) * t;
    }
};

Wire wireThrough(const QVector<QPointF>& points) {
    Wire wire;
    wire.points = points;
    wire.cumulative.fill(0.0, points.size());
    for (int i = 1; i < points.size(); ++i) {
        const QPointF step = points.at(i) - points.at(i - 1);
        wire.cumulative[i] = wire.cumulative.at(i - 1) + std::hypot(step.x(), step.y());
    }
    return wire;
}

// The arc length where the wire's first leg along the primary axis spanning
// primary coordinate `p` passes it; nullopt when no such leg does.
std::optional<qreal> arcAtP(const Wire& wire, qreal p, const Axes& axes) {
    for (int i = 0; i + 1 < wire.points.size(); ++i) {
        const QPointF a = wire.points.at(i);
        const QPointF b = wire.points.at(i + 1);
        const qreal pa = axes.toPS(a).x();
        const qreal pb = axes.toPS(b).x();
        if (axes.runsAlongP(a, b) && p >= std::min(pa, pb) && p <= std::max(pa, pb)) {
            return wire.cumulative.at(i) + std::abs(p - pa);
        }
    }
    return std::nullopt;
}

// A channel label's preferred arc: the midpoint of the first segment along the
// secondary axis inside the strip (the Z bar of an adjacent-rank edge), else
// where a primary-axis leg crosses the strip's center, else the wire's midpoint.
qreal preferredArc(const Wire& wire, Span strip, const Axes& axes) {
    for (int i = 0; i + 1 < wire.points.size(); ++i) {
        const QPointF a = wire.points.at(i);
        const qreal pa = axes.toPS(a).x();
        if (axes.runsAlongS(a, wire.points.at(i + 1)) && pa >= strip.min && pa <= strip.max) {
            return (wire.cumulative.at(i) + wire.cumulative.at(i + 1)) / 2.0;
        }
    }
    return arcAtP(wire, (strip.min + strip.max) / 2.0, axes).value_or(wire.total() / 2.0);
}

// One label of a channel: its wire, its preferred point, its pill size.
struct ChannelLabel {
    quint64 transitionId = 0;
    Wire wire;
    qreal arc = 0.0;
    QPointF point;
    QSizeF size;
};

// The channel's collinear clusters: labels whose preferred points lie within
// (e_i + e_j)/2 + kLabelGap of each other along s (e = a pill's secondary
// extent), closed transitively; such pills must sit side by side. `labels` is
// in (s, id) order; each cluster keeps it, clusters ordered by first label.
QVector<QVector<int>> collinearClusters(const QVector<ChannelLabel>& labels, const Axes& axes) {
    QVector<int> root(labels.size());
    for (int i = 0; i < root.size(); ++i) {
        root[i] = i;
    }
    const std::function<int(int)> find = [&](int i) { return root.at(i) == i ? i : root[i] = find(root.at(i)); };
    for (int i = 0; i < labels.size(); ++i) {
        for (int j = i + 1; j < labels.size(); ++j) {
            const qreal reach = (axes.sSize(labels.at(i).size) + axes.sSize(labels.at(j).size)) / 2.0 + kLabelGap;
            if (std::abs(axes.toPS(labels.at(i).point).y() - axes.toPS(labels.at(j).point).y()) < reach) {
                const int a = find(i);
                const int b = find(j);
                root[std::max(a, b)] = std::min(a, b);
            }
        }
    }
    QVector<QVector<int>> clusters;
    QHash<int, int> clusterOfRoot;
    for (int i = 0; i < labels.size(); ++i) {
        const int r = find(i);
        if (!clusterOfRoot.contains(r)) {
            clusterOfRoot.insert(r, clusters.size());
            clusters.push_back({});
        }
        clusters[clusterOfRoot.value(r)].push_back(i);
    }
    return clusters;
}

// The pills of one cluster side by side along the primary axis, kLabelGap
// apart.
qreal clusterRowWidth(const QVector<ChannelLabel>& labels, const QVector<int>& cluster, const Axes& axes) {
    qreal width = kLabelGap * (cluster.size() - 1);
    for (int i : cluster) {
        width += axes.pSize(labels.at(i).size);
    }
    return width;
}

// A collinear cluster's pills can only separate along the primary axis, so
// each gets its own slot: in (s, id) order along p, the row centered in the
// strip the channel reserved for it, and the label's preferred point moves
// to where its wire crosses its slot's center.
void slotCollinearClusters(QVector<ChannelLabel>* labels, Span strip, const Axes& axes) {
    for (const QVector<int>& cluster : collinearClusters(*labels, axes)) {
        if (cluster.size() < 2) {
            continue;
        }
        qreal p = (strip.min + strip.max - clusterRowWidth(*labels, cluster, axes)) / 2.0;
        for (int i : cluster) {
            ChannelLabel& label = (*labels)[i];
            const std::optional<qreal> arc = arcAtP(label.wire, p + axes.pSize(label.size) / 2.0, axes);
            p += axes.pSize(label.size) + kLabelGap;
            if (arc.has_value()) {
                label.arc = *arc;
                label.point = label.wire.pointAt(*arc);
            }
        }
    }
}

// How far along the wire, from its end `endPoint` (whose next point is
// `next`), a pill's center must sit before the pill grown by
// kPillNodeKeepout clears the end's own node `rect` -- the scan's bound.
qreal endKeepoutArc(QPointF endPoint, QPointF next, const QRectF& rect, QSizeF pill) {
    if (runsHorizontally(endPoint, next)) {
        const qreal offRect = std::min(std::abs(endPoint.x() - rect.left()), std::abs(endPoint.x() - rect.right()));
        return std::max(0.0, pill.width() / 2.0 + kPillNodeKeepout - offRect);
    }
    const qreal offRect = std::min(std::abs(endPoint.y() - rect.top()), std::abs(endPoint.y() - rect.bottom()));
    return std::max(0.0, pill.height() / 2.0 + kPillNodeKeepout - offRect);
}

// Barycenter crossing reduction over one scope's columns. `adjacent` holds the
// adjacent-column node pairs, each once. A node with no neighbour in the fixed
// column keeps its slot; the rest re-sort into the remaining slots by
// barycenter, ties by previous position.
void orderColumns(QVector<QVector<quint64>>* columns, const QVector<QPair<quint64, quint64>>& adjacent) {
    if (columns->size() < 2 || adjacent.isEmpty()) {
        return;
    }
    const auto sweep = [&](int column, int fixed) {
        QHash<quint64, int> fixedIndex;
        for (int i = 0; i < columns->at(fixed).size(); ++i) {
            fixedIndex.insert(columns->at(fixed).at(i), i);
        }
        struct Entry {
            quint64 id = 0;
            int previous = 0;
            qreal barycenter = 0.0;
        };
        QVector<int> movableSlots;
        QVector<Entry> movable;
        const QVector<quint64>& order = columns->at(column);
        for (int i = 0; i < order.size(); ++i) {
            qreal sum = 0.0;
            int count = 0;
            for (const auto& pair : adjacent) {
                const quint64 other = pair.first == order.at(i) ? pair.second
                                      : pair.second == order.at(i) ? pair.first
                                                                   : 0;
                if (other != 0 && fixedIndex.contains(other)) {
                    sum += fixedIndex.value(other);
                    ++count;
                }
            }
            if (count > 0) {
                movableSlots.push_back(i);
                movable.push_back(Entry{order.at(i), i, sum / count});
            }
        }
        std::sort(movable.begin(), movable.end(), [](const Entry& a, const Entry& b) {
            if (a.barycenter != b.barycenter) {
                return a.barycenter < b.barycenter;
            }
            return a.previous != b.previous ? a.previous < b.previous : a.id < b.id;
        });
        for (int k = 0; k < movableSlots.size(); ++k) {
            (*columns)[column][movableSlots.at(k)] = movable.at(k).id;
        }
    };
    QVector<QVector<QVector<quint64>>> seen{*columns};
    for (int round = 0; round < kBarycenterRounds; ++round) {
        for (int c = 1; c < columns->size(); ++c) {
            sweep(c, c - 1);
        }
        for (int c = columns->size() - 2; c >= 0; --c) {
            sweep(c, c + 1);
        }
        if (seen.contains(*columns)) {
            return;  // a repeated order: no further sweep can change anything new
        }
        seen.push_back(*columns);
    }
}

class Layout {
public:
    Layout(const Machine& machine, const AutoLayoutMetrics& metrics, const AutoLayoutOptions& options)
        : machine_(machine), metrics_(metrics), options_(options), axes_(options.direction) {
        for (int i = 0; i < machine_.states.size(); ++i) {
            const State& state = machine_.states.at(i);
            indexById_.insert(state.id, i);
            childrenOf_[state.parentId].push_back(state.id);  // document order
        }
        for (int i = 0; i < machine_.transitions.size(); ++i) {
            transitionIndexById_.insert(machine_.transitions.at(i).id, i);
        }
    }

    AutoLayoutPlan run() {
        layoutScope(0);
        place(0, QPointF(0.0, 0.0));
        AutoLayoutPlan plan;
        for (const Transition& transition : machine_.transitions) {
            if (edgeOf_.contains(transition.id)) {
                plan.result.edges.push_back(edgeOf_.value(transition.id));
            }
        }
        const QHash<quint64, qreal> ratioOf = placeLabels(&plan.result);
        for (const State& state : machine_.states) {
            const auto rect = sceneRect_.constFind(state.id);
            if (rect != sceneRect_.constEnd()) {  // a state whose parent is gone is not laid out
                plan.states.push_back(StatePlacement{state.id, rect->topLeft()});
            }
        }
        for (const Transition& transition : machine_.transitions) {
            if (isNormal(transition)) {
                const auto ratio = ratioOf.constFind(transition.id);
                plan.labels.push_back(LabelPlacement{
                    transition.id, ratio != ratioOf.constEnd() ? std::optional<qreal>(*ratio) : std::nullopt});
            }
        }
        return plan;
    }

private:
    // On the placed scene: route every Normal transition as the canvas will,
    // give each label a preferred point, then slide it along its wire to the
    // nearest spot clear of placed pills and node keepouts. That spot becomes
    // its labelRatio (returned by transition id).
    QHash<quint64, qreal> placeLabels(AutoLayoutResult* result) {
        QHash<quint64, qreal> ratioOf;
        const RectOf rectOf = [this](quint64 id) { return sceneRect_.value(id); };
        AnchorCache anchors;
        const auto rankOf = [this](quint64 scope) { return scope == 0 ? -1 : indexById_.value(scope); };
        QVector<quint64> scopesByRank = scopeOrder_;
        std::sort(scopesByRank.begin(), scopesByRank.end(),
                  [&](quint64 a, quint64 b) { return rankOf(a) < rankOf(b); });

        // Placement order: channels along the flow (by low primary edge, ties by
        // scope rank then index), labels within a channel by preferred s then id;
        // then same-column labels per scope and column by id; then transitions no
        // scope owns (a compound and its own descendant) by id.
        struct Pending {
            quint64 transitionId = 0;
            Wire wire;
            qreal arc = 0.0;
        };
        struct ChannelRun {
            qreal low = 0.0;
            int scopeRank = 0;
            int index = 0;
            QVector<ChannelLabel> labels;
        };
        QVector<ChannelRun> channels;
        for (quint64 scope : scopesByRank) {
            const ScopeInfo info = scopes_.value(scope);
            for (int c = 0; c < info.channelWidth.size(); ++c) {
                const Span strip = stripOf(info, c, rectOf);
                QVector<ChannelLabel> labels = channelLabels(scope, c, info, rectOf, &anchors);
                slotCollinearClusters(&labels, strip, axes_);
                channels.push_back(ChannelRun{strip.min, rankOf(scope), c, labels});
                const Span scene = axes_.sceneSpan(strip);
                result->channels.push_back(
                    AutoLayoutResult::Channel{scope, c, info.channelWidth.at(c), scene.min, scene.max});
            }
        }
        std::stable_sort(channels.begin(), channels.end(), [](const ChannelRun& a, const ChannelRun& b) {
            if (a.low != b.low) {
                return a.low < b.low;
            }
            return a.scopeRank != b.scopeRank ? a.scopeRank < b.scopeRank : a.index < b.index;
        });
        QVector<Pending> pending;
        for (const ChannelRun& channel : channels) {
            for (const ChannelLabel& label : channel.labels) {
                pending.push_back(Pending{label.transitionId, label.wire, label.arc});
            }
        }
        // One run of midpoint-preferred labels, in ascending transition id.
        const auto pushByIdAtMidpoint = [&](QVector<quint64> ids) {
            std::sort(ids.begin(), ids.end());
            for (quint64 id : ids) {
                const Wire wire = routeWire(transition(id), rectOf, &anchors);
                if (wire.isValid()) {
                    pending.push_back(Pending{id, wire, wire.total() / 2.0});
                }
            }
        };
        for (quint64 scope : scopesByRank) {
            for (int c = 0; c < scopes_.value(scope).columns.size(); ++c) {
                QVector<quint64> ids;
                for (const Transition& transition : machine_.transitions) {
                    const auto edge = edgeOf_.constFind(transition.id);
                    if (edge != edgeOf_.constEnd() && edge->scope == scope && edge->kind == EdgeKind::SameColumn &&
                        edge->fromLayer == c) {
                        ids.push_back(transition.id);
                    }
                }
                pushByIdAtMidpoint(ids);
            }
        }
        QVector<quint64> scopeless;
        for (const Transition& transition : machine_.transitions) {
            if (isNormal(transition) && !edgeOf_.contains(transition.id) && sceneRect_.contains(transition.from) &&
                sceneRect_.contains(transition.to)) {
                scopeless.push_back(transition.id);
            }
        }
        pushByIdAtMidpoint(scopeless);

        QVector<QPair<quint64, QRectF>> nodes;  // document order
        for (const State& node : machine_.states) {
            nodes.push_back({node.id, sceneRect_.value(node.id)});
        }
        QVector<QRectF> placed;  // placed pills, each grown by kLabelGap / 2
        const qreal halfGap = kLabelGap / 2.0;
        for (const Pending& label : pending) {
            const Transition& transition = this->transition(label.transitionId);
            const QSizeF size = labelSize(label.transitionId);
            // A compound around an endpoint is where the wire lives: its pill
            // may sit inside that hull.
            QSet<quint64> exempt;  // lookup only
            for (quint64 end : {transition.from, transition.to}) {
                for (quint64 id = state(end).parentId; id != 0 && indexById_.contains(id); id = state(id).parentId) {
                    exempt.insert(id);
                }
            }
            const auto overlapAt = [&](qreal arc) {
                const QRectF pill = pillRectAt(label.wire.pointAt(arc), size);
                const QRectF spaced = pill.adjusted(-halfGap, -halfGap, halfGap, halfGap);
                const QRectF keepout =
                    pill.adjusted(-kPillNodeKeepout, -kPillNodeKeepout, kPillNodeKeepout, kPillNodeKeepout);
                qreal overlap = 0.0;
                for (const QRectF& other : placed) {
                    const QRectF shared = spaced.intersected(other);
                    overlap += shared.width() * shared.height();
                }
                for (const auto& node : nodes) {
                    if (!exempt.contains(node.first)) {
                        const QRectF shared = keepout.intersected(node.second);
                        overlap += shared.width() * shared.height();
                    }
                }
                return overlap;
            };
            const auto clearAt = [&](qreal arc) {
                const QRectF pill = pillRectAt(label.wire.pointAt(arc), size);
                const QRectF spaced = pill.adjusted(-halfGap, -halfGap, halfGap, halfGap);
                for (const QRectF& other : placed) {
                    if (spaced.intersects(other)) {
                        return false;
                    }
                }
                const QRectF keepout =
                    pill.adjusted(-kPillNodeKeepout, -kPillNodeKeepout, kPillNodeKeepout, kPillNodeKeepout);
                for (const auto& node : nodes) {
                    if (!exempt.contains(node.first) && keepout.intersects(node.second)) {
                        return false;
                    }
                }
                return true;
            };
            // Scan outward from the preferred point, kScanStep at a time,
            // alternating sides, over the wire minus both ends' keepouts.
            const QVector<QPointF>& points = label.wire.points;
            const qreal lo = exempt.contains(transition.from)
                                 ? 0.0
                                 : endKeepoutArc(points.first(), points.at(1), rectOf(transition.from), size);
            const qreal hi = label.wire.total() - (exempt.contains(transition.to)
                                                       ? 0.0
                                                       : endKeepoutArc(points.last(), points.at(points.size() - 2),
                                                                       rectOf(transition.to), size));
            std::optional<qreal> chosen;
            for (int k = 0;; ++k) {
                const qreal later = label.arc + k * kScanStep;
                const qreal earlier = label.arc - k * kScanStep;
                if (later >= lo && later <= hi && clearAt(later)) {
                    chosen = later;
                    break;
                }
                if (k > 0 && earlier >= lo && earlier <= hi && clearAt(earlier)) {
                    chosen = earlier;
                    break;
                }
                if (later > hi && earlier < lo) {
                    break;
                }
            }
            if (!chosen.has_value()) {
                result->residuals.push_back(AutoLayoutResult::Residual{label.transitionId, overlapAt(label.arc)});
            }
            const qreal arc = chosen.value_or(label.arc);
            const QPointF center = label.wire.pointAt(arc);
            placed.push_back(pillRectAt(center, size).adjusted(-halfGap, -halfGap, halfGap, halfGap));
            const qreal total = label.wire.total();
            const qreal ratio = total > 0.0 ? std::clamp(arc / total, 0.0, 1.0) : 0.5;
            ratioOf.insert(label.transitionId, ratio);
            result->labels.push_back(AutoLayoutResult::Label{label.transitionId, center, ratio});
        }
        return ratioOf;
    }

    const State& state(quint64 id) const { return machine_.states.at(indexById_.value(id)); }
    const Transition& transition(quint64 id) const {
        return machine_.transitions.at(transitionIndexById_.value(id));
    }
    bool isCompound(quint64 id) const { return !childrenOf_.value(id).isEmpty(); }

    // The member of `scope` that is `id` or one of its ancestors; 0 when `id`
    // lies outside the scope (or is the 0 sentinel).
    quint64 memberOf(quint64 id, quint64 scope) const {
        while (id != 0 && indexById_.contains(id)) {
            const quint64 parent = state(id).parentId;
            if (parent == scope) {
                return id;
            }
            id = parent;
        }
        return 0;
    }

    // Lays out `scope`'s members relative to the scope's content origin and
    // returns the content extent. Compound members are laid out first, so
    // their box size is known when this scope stacks them.
    QSizeF layoutScope(quint64 scope) {
        const QVector<quint64> members = childrenOf_.value(scope);
        if (members.isEmpty()) {
            return QSizeF();
        }
        for (quint64 member : members) {
            const State& memberState = state(member);
            const QSizeF leaf = metrics_.leafSize(memberState);
            if (isCompound(member)) {
                const QSizeF inner = layoutScope(member);
                const qreal header = metrics_.containerHeaderHeight(memberState);
                headerOf_.insert(member, header);
                sizeOf_.insert(member, QSizeF(std::max(inner.width() + 2.0 * kContainerHullPadding, leaf.width()),
                                              inner.height() + 2.0 * kContainerHullPadding + header));
            } else {
                sizeOf_.insert(member, leaf);
            }
        }

        // Scope graph: Normal transitions between two distinct members (one into
        // or out of a compound member's subtree counts as that member's).
        QVector<quint64> scopeEdges;  // transition ids, document order
        QHash<quint64, QVector<quint64>> successors;
        QSet<QPair<quint64, quint64>> seen;
        for (const Transition& transition : machine_.transitions) {
            const quint64 from = memberOf(transition.from, scope);
            const quint64 to = memberOf(transition.to, scope);
            if (!isNormal(transition) || from == 0 || to == 0 || to == from) {
                continue;
            }
            scopeEdges.push_back(transition.id);
            if (!seen.contains({from, to})) {
                seen.insert({from, to});
                successors[from].push_back(to);
            }
        }

        // BFS layers from the scope's initial member; unreached members seed
        // further BFS runs in trailing columns, in document order.
        const quint64 initial = scope == 0 ? machine_.initialStateId : state(scope).initialChildId;
        QHash<quint64, int> layerOf;
        int maxLayer = -1;
        auto bfsFrom = [&](quint64 root, int rootLayer) {
            QVector<quint64> queue{root};
            layerOf.insert(root, rootLayer);
            maxLayer = std::max(maxLayer, rootLayer);
            for (int head = 0; head < queue.size(); ++head) {
                const quint64 current = queue.at(head);
                for (quint64 next : successors.value(current)) {
                    if (!layerOf.contains(next)) {
                        layerOf.insert(next, layerOf.value(current) + 1);
                        maxLayer = std::max(maxLayer, layerOf.value(next));
                        queue.push_back(next);
                    }
                }
            }
        };
        const quint64 start = memberOf(initial, scope);
        bfsFrom(start != 0 ? start : members.front(), 0);
        for (quint64 member : members) {
            if (!layerOf.contains(member)) {
                bfsFrom(member, maxLayer + 1);
            }
        }

        // Channel c is the gap after column c along the flow. A forward edge labels
        // the channel after its source column, a backward one the channel before
        // it (the first gap its wire enters), a same-column edge none.
        QVector<QPair<quint64, quint64>> adjacent;  // |delta| == 1 node pairs, each once, lower layer first
        for (quint64 id : scopeEdges) {
            const Transition& edge = transition(id);
            const quint64 from = memberOf(edge.from, scope);
            const quint64 to = memberOf(edge.to, scope);
            const int fromLayer = layerOf.value(from);
            const int toLayer = layerOf.value(to);
            const int delta = toLayer - fromLayer;
            Q_ASSERT(delta <= 1);  // BFS reaches `to` through `from` at the latest
            const EdgeKind kind = delta > 0 ? EdgeKind::Forward : delta < 0 ? EdgeKind::Backward : EdgeKind::SameColumn;
            const int channel = kind == EdgeKind::Forward ? fromLayer : kind == EdgeKind::Backward ? fromLayer - 1 : -1;
            edgeOf_.insert(id, AutoLayoutResult::Edge{id, scope, fromLayer, toLayer, kind, channel});
            if (std::abs(delta) == 1) {
                const QPair<quint64, quint64> pair = delta > 0 ? qMakePair(from, to) : qMakePair(to, from);
                if (!adjacent.contains(pair)) {
                    adjacent.push_back(pair);
                }
            }
        }

        // Crossing reduction: document order within a column, then barycenter sweeps.
        QVector<QVector<quint64>> columns(maxLayer + 1);
        for (quint64 member : members) {
            columns[layerOf.value(member)].push_back(member);
        }
        orderColumns(&columns, adjacent);

        // Rows stack along s in the sweep order, each column centered against the
        // longest. The gap a same-column edge leaves through must hold its pill
        // clear of both boxes (several such pills stacked kLabelGap apart); the
        // node gap alone is shorter. Until the projection everything is in (p, s).
        const qreal nodeGap = options_.nodeGap;
        QVector<QVector<qreal>> gapBelow(columns.size());  // gapBelow[c][r]: between rows r and r + 1
        QVector<QVector<qreal>> gapPills(columns.size());
        QVector<QVector<int>> gapPillCount(columns.size());
        for (int c = 0; c < columns.size(); ++c) {
            const int gaps = std::max(0, static_cast<int>(columns.at(c).size()) - 1);
            gapBelow[c].fill(nodeGap, gaps);
            gapPills[c].fill(0.0, gaps);
            gapPillCount[c].fill(0, gaps);
        }
        for (quint64 id : scopeEdges) {
            const AutoLayoutResult::Edge edge = edgeOf_.value(id);
            if (edge.kind != EdgeKind::SameColumn) {
                continue;
            }
            const QVector<quint64>& column = columns.at(edge.fromLayer);
            const int fromRow = column.indexOf(memberOf(transition(id).from, scope));
            const int toRow = column.indexOf(memberOf(transition(id).to, scope));
            const int gap = fromRow < toRow ? fromRow : fromRow - 1;
            gapPills[edge.fromLayer][gap] += axes_.sSize(labelSize(id));
            ++gapPillCount[edge.fromLayer][gap];
        }
        QVector<qreal> columnLength(columns.size(), 0.0);  // along s
        qreal longest = 0.0;
        for (int c = 0; c < columns.size(); ++c) {
            for (int r = 0; r < columns.at(c).size(); ++r) {
                columnLength[c] += axes_.sSize(sizeOf_.value(columns.at(c).at(r)));
                if (r + 1 < columns.at(c).size()) {
                    if (gapPillCount.at(c).at(r) > 0) {
                        gapBelow[c][r] = std::max(nodeGap, gapPills.at(c).at(r) +
                                                               kLabelGap * (gapPillCount.at(c).at(r) - 1) +
                                                               2.0 * kPillNodeKeepout);
                    }
                    columnLength[c] += gapBelow.at(c).at(r);
                }
            }
            longest = std::max(longest, columnLength.at(c));
        }
        // Corners snap up to the grid, so snapping only widens a gap.
        QHash<quint64, qreal> rowS;
        QVector<qreal> columnWidth(columns.size(), 0.0);  // along p
        for (int c = 0; c < columns.size(); ++c) {
            qreal s = snapUp((longest - columnLength.at(c)) / 2.0);
            for (int r = 0; r < columns.at(c).size(); ++r) {
                const QSizeF size = sizeOf_.value(columns.at(c).at(r));
                rowS.insert(columns.at(c).at(r), s);
                columnWidth[c] = std::max(columnWidth.at(c), axes_.pSize(size));
                if (r + 1 < columns.at(c).size()) {
                    s = snapUp(s + axes_.sSize(size) + gapBelow.at(c).at(r));
                }
            }
        }

        // Channel widths along p. The floor is the layer gap, or the longest
        // primary pill extent plus kPillClearance, counting the channel's labels
        // and the self-loop/targetless pills of the preceding column, which may
        // reach into the gap.
        QVector<qreal> channelWidth(std::max(0, static_cast<int>(columns.size()) - 1), options_.layerGap);
        for (quint64 id : scopeEdges) {
            const int channel = edgeOf_.value(id).channel;
            if (channel >= 0) {
                channelWidth[channel] =
                    std::max(channelWidth.at(channel), axes_.pSize(labelSize(id)) + kPillClearance);
            }
        }
        for (const Transition& transition : machine_.transitions) {
            const bool offNode = transition.from != 0 && (transition.to == 0 || transition.to == transition.from);
            if (offNode && memberOf(transition.from, scope) == transition.from) {
                const int column = layerOf.value(transition.from);
                if (column < channelWidth.size()) {
                    channelWidth[column] =
                        std::max(channelWidth.at(column), axes_.pSize(labelSize(transition.id)) + kPillClearance);
                }
            }
        }
        // Columns advance along p, then one projection to scene: each box's low
        // (p, s) corner becomes its scene top-left and the scope is translated so
        // its min x and min y are 0. In LR and TB that translation is zero, so
        // leading edges stay on the grid; in RL and BT trailing edges align.
        const auto placeColumns = [&]() {
            QHash<quint64, QPointF> sceneTopLeft;
            qreal p = 0.0;
            for (int c = 0; c < columns.size(); ++c) {
                p = snapUp(p);
                for (quint64 member : columns.at(c)) {
                    sceneTopLeft.insert(member,
                                        axes_.topLeft(QPointF(p, rowS.value(member)), sizeOf_.value(member)));
                }
                p += columnWidth.at(c) + (c < channelWidth.size() ? channelWidth.at(c) : 0.0);
            }
            qreal minX = std::numeric_limits<qreal>::max();
            qreal minY = std::numeric_limits<qreal>::max();
            for (quint64 member : members) {
                minX = std::min(minX, sceneTopLeft.value(member).x());
                minY = std::min(minY, sceneTopLeft.value(member).y());
            }
            for (quint64 member : members) {
                localPos_.insert(member, sceneTopLeft.value(member) - QPointF(minX, minY));
            }
        };
        // Provisional p at the floor, then this scope's wires as the router will
        // draw them (this subtree's rects only), and each collinear cluster widens
        // its channel to hold its pills side by side. Then the final p.
        placeColumns();
        if (!channelWidth.isEmpty()) {
            QHash<quint64, QRectF> localRects;
            for (quint64 member : members) {
                collectRects(member, localPos_.value(member), &localRects);
            }
            const RectOf rectOf = [&localRects](quint64 id) { return localRects.value(id); };
            AnchorCache anchors;
            ScopeInfo provisional{columns, channelWidth};
            for (int c = 0; c < channelWidth.size(); ++c) {
                const QVector<ChannelLabel> labels = channelLabels(scope, c, provisional, rectOf, &anchors);
                for (const QVector<int>& cluster : collinearClusters(labels, axes_)) {
                    channelWidth[c] =
                        std::max(channelWidth.at(c), clusterRowWidth(labels, cluster, axes_) + kPillClearance);
                }
            }
            placeColumns();
        }
        scopes_.insert(scope, ScopeInfo{columns, channelWidth});
        scopeOrder_.push_back(scope);

        QRectF extent;
        for (quint64 member : members) {
            extent |= QRectF(localPos_.value(member), sizeOf_.value(member));
        }
        return QSizeF(extent.right(), extent.bottom());
    }

    // `member`'s box and, for a compound, every descendant's, with `member` at
    // `topLeft`.
    void collectRects(quint64 member, QPointF topLeft, QHash<quint64, QRectF>* rects) const {
        rects->insert(member, QRectF(topLeft, sizeOf_.value(member)));
        const QPointF contentOrigin =
            topLeft + QPointF(kContainerHullPadding, kContainerHullPadding + headerOf_.value(member));
        for (quint64 child : childrenOf_.value(member)) {
            collectRects(child, contentOrigin + localPos_.value(child), rects);
        }
    }

    // Converts the scope-relative layout to scene coordinates: a compound
    // member's children start inside its hull, below its header.
    void place(quint64 scope, QPointF origin) {
        for (quint64 member : childrenOf_.value(scope)) {
            const QPointF topLeft = origin + localPos_.value(member);
            sceneRect_.insert(member, QRectF(topLeft, sizeOf_.value(member)));
            if (isCompound(member)) {
                place(member, topLeft + QPointF(kContainerHullPadding, kContainerHullPadding + headerOf_.value(member)));
            }
        }
    }

    QSizeF labelSize(quint64 transitionId) {
        if (!labelSizeOf_.contains(transitionId)) {
            labelSizeOf_.insert(transitionId, metrics_.labelSize(transition(transitionId)));
        }
        return labelSizeOf_.value(transitionId);
    }

    // The wire the canvas will draw for `transition` over the rects `rectOf`
    // hands out, using RoutingDelegate's own gathering, anchoring and routing
    // calls. Invalid when an end has no rect.
    Wire routeWire(const Transition& transition, const RectOf& rectOf, AnchorCache* anchors) const {
        const auto anchorsOf = [&](quint64 stateId) {
            if (!anchors->contains(stateId)) {
                const QRectF rect = rectOf(stateId);
                anchors->insert(stateId, rect.isNull() ? QHash<quint64, EndpointAnchor>()
                                                       : computeNodeAnchors(rect, touchingTransitionsFor(
                                                                                      machine_, stateId, rectOf)));
            }
            return anchors->value(stateId);
        };
        const QHash<quint64, EndpointAnchor> fromAnchors = anchorsOf(transition.from);
        const QHash<quint64, EndpointAnchor> toAnchors = anchorsOf(transition.to);
        if (!fromAnchors.contains(transition.id) || !toAnchors.contains(transition.id)) {
            return {};
        }
        const EndpointAnchor source = fromAnchors.value(transition.id);
        const EndpointAnchor target = toAnchors.value(transition.id);
        return wireThrough(routeUnifiedEdge(source.anchor, source.side, target.anchor, target.side).waypoints);
    }

    struct ScopeInfo {
        QVector<QVector<quint64>> columns;  // low s to high s
        QVector<qreal> channelWidth;        // reserved width (along p) of the gap after column c
    };

    // Channel `channel`'s strip in primary coordinates of the rects `rectOf`
    // hands out: from the furthest high-p edge of its own column's boxes to
    // the nearest low-p edge of the next column's.
    Span stripOf(const ScopeInfo& info, int channel, const RectOf& rectOf) const {
        Span strip{-std::numeric_limits<qreal>::max(), std::numeric_limits<qreal>::max()};
        for (quint64 member : info.columns.at(channel)) {
            strip.min = std::max(strip.min, axes_.pExtent(rectOf(member)).max);
        }
        for (quint64 member : info.columns.at(channel + 1)) {
            strip.max = std::min(strip.max, axes_.pExtent(rectOf(member)).min);
        }
        return strip;
    }

    // The labels of `scope`'s channel `channel` with their wires and
    // preferred points, in (preferred s, id) order.
    QVector<ChannelLabel> channelLabels(quint64 scope, int channel, const ScopeInfo& info, const RectOf& rectOf,
                                        AnchorCache* anchors) {
        const Span strip = stripOf(info, channel, rectOf);
        QVector<ChannelLabel> labels;
        for (const Transition& transition : machine_.transitions) {
            const auto edge = edgeOf_.constFind(transition.id);
            if (edge == edgeOf_.constEnd() || edge->scope != scope || edge->channel != channel) {
                continue;
            }
            ChannelLabel label;
            label.transitionId = transition.id;
            label.wire = routeWire(transition, rectOf, anchors);
            if (!label.wire.isValid()) {
                continue;
            }
            label.arc = preferredArc(label.wire, strip, axes_);
            label.point = label.wire.pointAt(label.arc);
            label.size = labelSize(transition.id);
            labels.push_back(label);
        }
        std::sort(labels.begin(), labels.end(), [this](const ChannelLabel& a, const ChannelLabel& b) {
            const qreal sa = axes_.toPS(a.point).y();
            const qreal sb = axes_.toPS(b.point).y();
            return sa != sb ? sa < sb : a.transitionId < b.transitionId;
        });
        return labels;
    }

    const Machine& machine_;
    const AutoLayoutMetrics& metrics_;
    const AutoLayoutOptions options_;
    const Axes axes_;
    QHash<quint64, int> indexById_;
    QHash<quint64, int> transitionIndexById_;
    QHash<quint64, QVector<quint64>> childrenOf_;
    QHash<quint64, QSizeF> sizeOf_;
    QHash<quint64, qreal> headerOf_;  // compounds only
    QHash<quint64, QPointF> localPos_;
    QHash<quint64, QRectF> sceneRect_;
    QHash<quint64, QSizeF> labelSizeOf_;
    QHash<quint64, AutoLayoutResult::Edge> edgeOf_;  // by transition id; lookup only
    QHash<quint64, ScopeInfo> scopes_;               // lookup only; scopeOrder_ is the walk order
    QVector<quint64> scopeOrder_;                    // children before parents, document order
};

}  // namespace

bool machineHasNoGeometry(const Machine& machine) {
    if (machine.states.isEmpty()) {
        return false;
    }
    return std::all_of(machine.states.begin(), machine.states.end(),
                       [](const State& state) { return state.pos.isNull(); });
}

AutoLayoutPlan computeAutoLayoutPlan(const Machine& machine, const AutoLayoutMetrics& metrics,
                                     const AutoLayoutOptions& options) {
    if (machine.states.isEmpty()) {
        return {};
    }
    // The provisional wires routed here are for measurement only and must not
    // reach the routing trace or qDebug(). Every route of a layout happens in
    // here; applyAutoLayout's write routes nothing.
    const RoutingLogSilencer quiet;
    return Layout(machine, metrics, options).run();
}

AutoLayoutResult applyAutoLayout(Machine* machine, const AutoLayoutMetrics& metrics,
                                 const AutoLayoutOptions& options) {
    if (machine == nullptr) {
        return {};
    }
    AutoLayoutPlan plan = computeAutoLayoutPlan(*machine, metrics, options);
    QHash<quint64, int> stateIndex;  // lookup only
    for (int i = 0; i < machine->states.size(); ++i) {
        stateIndex.insert(machine->states.at(i).id, i);
    }
    QHash<quint64, int> transitionIndex;  // lookup only
    for (int i = 0; i < machine->transitions.size(); ++i) {
        transitionIndex.insert(machine->transitions.at(i).id, i);
    }
    for (const StatePlacement& placement : plan.states) {
        machine->states[stateIndex.value(placement.id)].pos = placement.pos;
    }
    for (const LabelPlacement& placement : plan.labels) {
        machine->transitions[transitionIndex.value(placement.id)].labelRatio = placement.ratio;
    }
    return std::move(plan.result);
}

}  // namespace app
