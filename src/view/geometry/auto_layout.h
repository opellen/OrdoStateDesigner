#pragma once

// Layered auto-layout: state positions plus each Normal transition's label position
// along its wire. Pure and deterministic; the caller supplies real box sizes.
// Per scope (root or one compound's children): BFS layers give columns, barycenter
// sweeps order rows, channels are sized for their pills; compounds lay out bottom-up.
// Axes: p = flow (ranks advance along it), s = cross (rows stack along it).
// LR p=+x s=y; RL p=-x; TB p=+y s=x; BT p=-y. Mirrored directions align trailing edges.

#include <functional>
#include <optional>

#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <QtGlobal>

// StatePlacement, LabelPlacement, Machine, State, Transition.
#include "model/machine.h"

namespace app {

// Padding the canvas adds around a compound state's children (RoutingDelegate's
// container hull); shared so the layout reserves exactly the drawn hull.
inline constexpr qreal kContainerHullPadding = 16.0;

// Spacing defaults: four grid steps between ranks, two between boxes of one rank.
inline constexpr qreal kDefaultLayerGap = 96.0;
inline constexpr qreal kDefaultNodeGap = 48.0;

// Which way the ranks advance: the initial state's rank first, then each
// following rank to the right (LeftToRight), left, below or above.
enum class LayoutDirection { LeftToRight, RightToLeft, TopToBottom, BottomToTop };

// The enumerator's own name; the one spelling used by the MCP `direction`
// parameter, the autoLayout.direction setting and the dialog.
inline QString layoutDirectionName(LayoutDirection direction) {
    switch (direction) {
        case LayoutDirection::LeftToRight: return QStringLiteral("LeftToRight");
        case LayoutDirection::RightToLeft: return QStringLiteral("RightToLeft");
        case LayoutDirection::TopToBottom: return QStringLiteral("TopToBottom");
        case LayoutDirection::BottomToTop: return QStringLiteral("BottomToTop");
    }
    return QStringLiteral("LeftToRight");
}

// The inverse; nullopt for anything that is not exactly one of the four names.
inline std::optional<LayoutDirection> layoutDirectionFromName(const QString& name) {
    for (LayoutDirection direction : {LayoutDirection::LeftToRight, LayoutDirection::RightToLeft,
                                      LayoutDirection::TopToBottom, LayoutDirection::BottomToTop}) {
        if (name == layoutDirectionName(direction)) {
            return direction;
        }
    }
    return std::nullopt;
}

struct AutoLayoutOptions {
    LayoutDirection direction = LayoutDirection::LeftToRight;
    qreal layerGap = kDefaultLayerGap;  // the narrowest a channel between two ranks may be
    qreal nodeGap = kDefaultNodeGap;    // between two boxes of one rank, before a pill widens it
};

struct AutoLayoutMetrics {
    std::function<QSizeF(const State&)> leafSize;              // a leaf state's painted box
    std::function<qreal(const State&)> containerHeaderHeight;  // a compound's header band above its children
    std::function<QSizeF(const Transition&)> labelSize;        // a transition's event pill
};

// True when the machine has states and every one of them sits at the origin
// -- i.e. the document carries no geometry to respect.
bool machineHasNoGeometry(const Machine& machine);

// What one layout run decided: the residual verdict plus a record of edges,
// channels and labels for checking the geometry.
struct AutoLayoutResult {
    // A label no along-wire position could clear: it stays at its preferred
    // point, and `overlap` is the area its rects still overlap there.
    struct Residual {
        quint64 transitionId = 0;
        qreal overlap = 0.0;
    };
    QVector<Residual> residuals;

    // A scope edge's class by its layer delta: Forward (+1), Backward (< 0),
    // SameColumn (0). BFS layering makes a delta above +1 impossible.
    enum class EdgeKind { Forward, Backward, SameColumn };
    struct Edge {
        quint64 transitionId = 0;
        quint64 scope = 0;  // 0 = the machine root, else the compound whose children it joins
        int fromLayer = 0;
        int toLayer = 0;
        EdgeKind kind = EdgeKind::Forward;
        int channel = -1;  // the gap after column `channel` along the flow; -1 for SameColumn
    };
    QVector<Edge> edges;  // document order of the transitions

    // The gap between column `index` and `index + 1` of one scope: the width
    // reserved along the flow, and its final strip in scene coordinates along
    // the primary axis (x for LR/RL, y for TB/BT), low to high.
    struct Channel {
        quint64 scope = 0;
        int index = 0;
        qreal reservedWidth = 0.0;
        qreal low = 0.0;
        qreal high = 0.0;
    };
    QVector<Channel> channels;

    // Where each Normal transition's pill landed: its center in scene
    // coordinates, and the Transition::labelRatio written for it.
    struct Label {
        quint64 transitionId = 0;
        QPointF center;
        qreal ratio = 0.0;
    };
    QVector<Label> labels;  // placement order
};

// A layout run's outcome as data; the machine is untouched. Holds every state
// under the root (document order; one whose parent is missing keeps its pos),
// every Normal transition with from and to set and not a self loop (document
// order), and the run's record.
struct AutoLayoutPlan {
    QVector<StatePlacement> states;
    QVector<LabelPlacement> labels;
    AutoLayoutResult result;
};

// Reads `machine`, returns the plan. An empty machine gives an empty plan.
AutoLayoutPlan computeAutoLayoutPlan(const Machine& machine, const AutoLayoutMetrics& metrics,
                                     const AutoLayoutOptions& options = {});

// computeAutoLayoutPlan, then writes every State::pos and planned
// Transition::labelRatio into `machine`. Label offsets, bendpoints and other
// transitions' labelRatio are left alone.
AutoLayoutResult applyAutoLayout(Machine* machine, const AutoLayoutMetrics& metrics,
                                 const AutoLayoutOptions& options = {});

}  // namespace app
