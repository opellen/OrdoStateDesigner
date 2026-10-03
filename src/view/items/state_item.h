#pragma once

#include <array>
#include <functional>

#include <QFont>
#include <QGraphicsItem>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "model/machine.h"
#include "view/geometry/edge_router.h"

class QGraphicsSceneHoverEvent;
class QGraphicsSceneMouseEvent;

namespace app {

class HandleItem;

// Renders one State as a content-sized rounded box: a name header and up to a
// few dimmer "entry: x()" lines. Dragging snaps to the 24px grid in itemChange and
// re-routes touching transitions live (setOnMoved), all view-local; the presenter
// hears of a drag only on release, and only if the position changed. Four
// HandleItem children, visible while selected, are the wire-drag sources.
// pos() is the top-left corner (State::pos); boundingRect() is local, origin (0,0).
class StateItem : public QGraphicsItem {
public:
    enum { Type = QGraphicsItem::UserType + 1 };
    int type() const override { return Type; }

    StateItem(quint64 id, QString name, StateKind kind, QStringList entryActions, QStringList exitActions);

    quint64 id() const { return id_; }

    // Incremental updates from the presenter's fact handlers; name/entryActions
    // change the box size, kind only needs a repaint.
    void setName(const QString& name);
    void setKind(StateKind kind);
    void setEntryActions(const QStringList& entryActions);
    // Paint-only input (container header block); never sizes the leaf rect_.
    // Container geometry after a change is refreshContainers()'s job.
    void setExitActions(const QStringList& exitActions);
    // Tints the resting/hover border; selection, glow and drop-highlight take precedence.
    void setColor(ElementColor color);

    StateKind kind() const { return kind_; }

    // This item's rect in scene coordinates; in container mode, the derived box.
    QRectF sceneRect() const;

    // A state with >=1 child renders as a box wrapping its children. Only
    // refreshContainers() calls this, deepest-first from the children's drawn
    // rects; State::pos is not read or written while a state has children.
    // `sceneRect` includes padding and the header band, sized by the caller via
    // containerHeaderHeight()/containerMinHeaderWidth(). setLeafMode() reverts to
    // State::pos rendering (no-op if already a leaf). Neither touches the document.
    void setContainerGeometry(const QRectF& sceneRect);
    void setLeafMode();
    bool isContainerMode() const { return containerMode_; }

    // Header sizing for the container band; valid in leaf or container mode.
    //   containerHeaderHeight()   -- name line plus the entry/exit action lines
    //                                when present.
    //   containerMinHeaderWidth() -- name width plus padding only; action lines
    //                                elide rather than widen.
    qreal containerHeaderHeight() const;
    qreal containerMinHeaderWidth() const;

    // Callbacks the presenter installs after construction. onMoved fires on every
    // live position change and on the echo of a presenter setPos (visual re-route
    // only). onDragStarted fires once per gesture on the first position change of
    // a held left button, never for a presenter setPos. On release, exactly one of
    // onDragFinished (pos() differs from the press position; the one place a drag
    // becomes MoveStateRequested) or onDragReverted follows every Started.
    void setOnMoved(std::function<void()> cb);
    void setOnDragStarted(std::function<void()> cb);
    void setOnDragFinished(std::function<void(QPointF pos)> cb);
    void setOnDragReverted(std::function<void()> cb);
    // The press-time position. Inside onDragStarted_, pos() already reflects the
    // first move, so use this as the pre-drag baseline.
    QPointF dragStartScenePos() const { return dragStartPos_; }
    // Fired on release after a stationary left click on an already-selected
    // item; never after a drag or a first selecting click.
    void setOnRenameRequested(std::function<void()> cb);
    void setOnWireDragStarted(std::function<void(quint64 stateId, PortSide side)> cb);
    void setOnWireDragMoved(std::function<void(QPointF scenePos)> cb);
    void setOnWireDragFinished(std::function<void(QPointF scenePos)> cb);
    // The "+" quick handle at the selected node's top-right corner.
    void setOnQuickAddClicked(std::function<void(quint64 stateId)> cb);

    // Alignment hook, consulted in itemChange's ItemPositionChange before the
    // grid snap. Per axis, if the adjuster changes the proposed value that axis
    // skips the grid snap; an untouched axis still snaps. The presenter installs
    // one on every item and returns `proposed` unchanged outside a live drag, so
    // undo/redo and programmatic setPos are unaffected. nullptr means grid snap only.
    void setPositionAdjuster(std::function<QPointF(QPointF proposed)> adjuster);

    // Cyan focus-hint border on the current magnetic-snap candidate during a wire
    // drag from another node; takes precedence over hover and selected.
    void setDropHighlighted(bool highlighted);
    bool isDropHighlighted() const { return dropHighlighted_; }

    // Probe accessors: whether all four side handles are shown, and one handle's
    // scene position (QPointF() if the side has no handle).
    bool debugSideHandlesVisible() const;
    QPointF debugHandleScenePos(PortSide side) const;

    // Simulate-mode presentation, driven by updateSimPresentation(); both default
    // false and the presenter restores that on clearAll(), mode change and sim
    // reset, since a rebuild can recreate items mid-run. setDimmed(true) sets
    // opacity 0.3 on non-active states. setActiveGlow(true) draws a 2px #3B82F6
    // border (wins over selected/hover/drop highlight) plus a glow; false clears it.
    void setDimmed(bool dimmed);
    void setActiveGlow(bool active);

    void setBreakpoint(bool hasBp);
    bool hasBreakpoint() const { return hasBreakpoint_; }
    void setBreakpointHit(bool hit);
    bool isBreakpointHit() const { return breakpointHit_; }
    void setOnBreakpointToggled(std::function<void(quint64)> cb);
    QPointF lastMouseScenePos() const { return lastMouseScenePos_; }

    QRectF boundingRect() const override;
    // Container mode: border ring + header band only, so the interior stays
    // click-through. Leaf mode: the plain boundingRect().
    QPainterPath shape() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;

private:
    void recomputeGeometry();
    void repositionHandles();
    void updateHandleVisibility();
    void paintContainer(QPainter* painter) const;  // container mode's paint() branch

    quint64 id_;
    QString name_;
    StateKind kind_;
    ElementColor color_ = ElementColor::Default;
    QStringList entryActions_;
    QStringList exitActions_;  // paint()-only (container header block)

    QFont nameFont_;
    QFont entryFont_;
    QRectF rect_;  // leaf mode's local rect; (0,0) at top-left

    // containerRect_ is rect_'s container-mode counterpart: local, (0,0) at
    // top-left, sized by CanvasPresenter::refreshContainers().
    bool containerMode_ = false;
    QRectF containerRect_;

    bool hovered_ = false;
    bool dropHighlighted_ = false;
    bool dimmed_ = false;
    bool activeGlow_ = false;
    bool hasBreakpoint_ = false;
    bool breakpointHit_ = false;
    bool wasSelectedAtPress_ = false;  // selection state sampled at press, for the rename gesture
    bool leftPressed_ = false;         // between press and release: what makes a position change a DRAG
    bool dragReported_ = false;        // onDragStarted_ already fired for this gesture
    QPointF dragStartPos_;
    QPointF lastMouseScenePos_;
    std::array<HandleItem*, 4> handles_{};  // Top, Right, Bottom, Left
    QGraphicsItem* quickAddHandle_ = nullptr;  // the "+" corner button
    std::function<void(quint64)> onQuickAddClicked_;
    std::function<QPointF(QPointF)> positionAdjuster_;

    std::function<void()> onMoved_;
    std::function<void()> onDragStarted_;
    std::function<void(QPointF)> onDragFinished_;
    std::function<void()> onDragReverted_;
    std::function<void()> onRenameRequested_;
    std::function<void(quint64, PortSide)> onWireDragStarted_;
    std::function<void(QPointF)> onWireDragMoved_;
    std::function<void(QPointF)> onWireDragFinished_;
    std::function<void(quint64)> onBreakpointToggled_;
};

}  // namespace app
