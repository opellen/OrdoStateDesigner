#pragma once

#include <optional>

#include <QPointF>
#include <QRectF>

#include "view/geometry/edge_router.h"  // PortSide

namespace app {

// Pill-port invariant: the event pill is a node ON the edge with one port at the
// middle of each side. Every line ends at a side-middle port perpendicular to
// that side, and a normal transition's two lines occupy different sides. The
// resolver is pure (rects in, an assignment out) and decides both ends together;
// the caller owns the drag session's incumbent assignment and passes it back in.

enum class EdgeClass {
    Normal,      // from and to are two distinct states
    Self,        // from == to: the out-and-back pair shares one pill side
    Targetless,  // to == 0: the pill's body terminates the edge (entry only)
    Root,        // from == 0: the machine frame's edge is the counterpart (exit only)
};

// Corner count of the run between a node anchor and its pill port; selected by
// both end directions.
enum class HalfShape { Straight, L, Z };

// How a rule-table row sources one half's pill side.
enum class SideSource {
    FacingCounterpart,  // the pill side facing that half's counterpart anchor
    YieldedSide,        // the row's literal yielded side (conflict resolution)
    SharedWithEntry,    // Self only: the same side the entry port uses
    Absent,             // the class has no such half
};

// Whether a row applies when the two halves' facing preferences coincide.
enum class SideCollision { Irrelevant, Distinct, Same };

// Where the source counterpart lies along the contested side's lateral axis (x
// for Top/Bottom, y for Left/Right); Positive is toward Right/Bottom.
enum class Lateral { Irrelevant, Positive, Negative };

// One row of the rule table. Rows live in pill_port_resolver.cpp as a static
// table and are exposed here so tests can assert it row for row.
struct PillPortRule {
    const char* id;
    EdgeClass cls;
    SideCollision collision;
    PortSide contested;     // read only when collision == Same
    Lateral sourceLateral;  // read only when collision == Same
    SideSource entryFrom;
    PortSide yieldedSide;  // read only when entryFrom == YieldedSide
    SideSource exitFrom;
    const char* observation;  // the behavior this row encodes
};

const PillPortRule* pillPortRuleTable(int* rowCount);

// Both pill ends, plus the node ends the halves run to.
struct PortAssignment {
    EdgeClass cls = EdgeClass::Normal;
    QPointF pillCenter;

    PortSide entrySide = PortSide::Top;   // the SOURCE half arrives here
    PortSide exitSide = PortSide::Bottom;  // the TARGET half departs here
    QPointF entryPoint;                    // side middle (Self: the pair straddles it)
    QPointF exitPoint;
    bool hasEntry = true;  // false for Root
    bool hasExit = true;   // false for Targetless

    PortSide sourceAnchorSide = PortSide::Right;
    PortSide targetAnchorSide = PortSide::Left;
    QPointF sourceAnchor;
    QPointF targetAnchor;

    HalfShape sourceShape = HalfShape::Straight;
    HalfShape targetShape = HalfShape::Straight;

    // Owned by the caller: a root transition with no target runs to the frame's
    // stub point with no arrowhead. The resolver never clears it.
    bool arrowAtTarget = true;

    // The rule-table row (pillPortRuleTable id) this assignment came from.
    const char* ruleId = "";
};

// Flip hysteresis band, in side-score px: a half keeps its incumbent side until
// the counterpart lies this much further toward the raw preference's side than
// toward the incumbent's. ~24px keeps a pill dragged along a side boundary from
// chattering.
inline constexpr qreal kFlipHysteresis = 24.0;

// Half the distance between a self transition's two attach points, which
// straddle the node-facing side's middle. Bounded above: the pill's short side
// is 18px (cap radius 9px), and the node's anchor band is only
// (height - 2*kAnchorInset), so a wider straddle would clamp on a short node and
// jog the pair out of parallel.
inline constexpr qreal kSelfStraddle = 5.0;

// Node anchors stay this far inside a side's corners; a rect too small to inset
// collapses to the side's middle.
inline constexpr qreal kAnchorInset = 12.0;

// Micro-jog tolerance: when the corner clamp leaves the pill's port line within
// this many px of the anchor line, the drawn pill slides onto the anchor line so
// the run stays straight (a Z shorter than the fillet diameter reads as a
// wrinkle). Equal to kAnchorInset, the largest residual a clamp can produce, and
// below the 16px port spacing. View-level only; labelOffset is untouched.
inline constexpr qreal kPortAlignTolerance = kAnchorInset;

// Curl flip threshold: a port facing away from its counterpart normally takes
// orthogonalPortRun's multi-corner fallback, but when the run's total length is
// below this, the wrapped legs read as a curl and the half flips to the
// perpendicular pill side facing its counterpart. ~5 grid cells.
inline constexpr qreal kWrapFlipThreshold = 120.0;

// The pill's edge must stay at least this far outside a connected node's border,
// leaving room for straight arrival (targetHalfStop = anchor - 15px leaves 5px)
// and departure runs without 180-degree wraps.
inline constexpr qreal kPillNodeKeepout = 20.0;

// Clamps pillCenter so the pill's bounding box stays kPillNodeKeepout clear of
// the connected source and target node edges.
QPointF clampPillNodeKeepout(QPointF pillCenter, QSizeF pillSize, const QRectF& sourceRect,
                             const QRectF& targetRect);

// The pill side facing `counterpart`: the raw per-half preference, before
// conflict yielding and hysteresis.
PortSide preferredPillSide(const QRectF& pillRect, QPointF counterpart);

PortSide oppositePortSide(PortSide side);

// The counterpart anchor a half's preference faces: `toward` projected onto
// the side of `rect` that faces it, clamped kAnchorInset inside the corners.
QPointF counterpartAnchor(const QRectF& rect, QPointF toward);

// Resolves both pill ends together. `sourceRect`/`targetRect` are read per
// class (Self: both are the node; Targetless: target unused; Root: source
// unused and target is the degenerate frame-edge rect). `incumbent` is the
// drag session's previous assignment -- absent means a hysteresis-free
// resolve, which is what every reroute outside a drag does.
PortAssignment resolvePillPorts(EdgeClass cls, const QRectF& sourceRect, const QRectF& targetRect,
                                 const QRectF& pillRect, const std::optional<PortAssignment>& incumbent);

}  // namespace app
