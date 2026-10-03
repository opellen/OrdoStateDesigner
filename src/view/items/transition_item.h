#pragma once

#include <functional>

#include <QColor>
#include <QFont>
#include <QGraphicsItem>
#include <QLineF>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QtGlobal>

#include "model/machine.h"  // ElementColor
#include "view/geometry/edge_router.h"

class QGraphicsSceneMouseEvent;
class QGraphicsSceneHoverEvent;

namespace app {

// Draws one transition's routed path halves and arrowhead at zValue 0, under
// node boxes (10), labels (20) and the interaction overlay (100). Local
// coordinates equal scene coordinates: the item stays at pos() (0,0) and the
// presenter replaces the whole route via setRoute(). Selectable via shape()'s
// fat hit path; selected, it paints solid white, two 8px cyan port-anchor dots
// and the skeleton handles (cyan bend squares, white segment-midpoint squares).
class TransitionItem : public QGraphicsItem {
public:
    enum { Type = QGraphicsItem::UserType + 2 };
    int type() const override { return Type; }

    explicit TransitionItem(quint64 id);

    quint64 id() const { return id_; }

    // Called whenever this transition (or either endpoint's port/side) is rerouted.
    void setRoute(const RoutedEdge& routed);
    // Probe hook: the route as currently drawn.
    const RoutedEdge& debugRoute() const { return routed_; }

    // Fired on ItemSelectedHasChanged so the presenter can repaint the label
    // pill, whose selection ring follows this item's isSelected().
    void setOnSelectedChanged(std::function<void(bool selected)> cb);

    // While selected, pressing within ~14px of the target-end arrowhead starts a
    // reconnect drag; the presenter commits RetargetTransitionRequested on
    // release. A targetless stub has no arrowhead and no handle.
    void setOnReconnectDrag(std::function<void(QPointF scenePos)> started,
                             std::function<void(QPointF scenePos)> moved,
                             std::function<void(QPointF scenePos)> finished);

    // Interactive orthogonal segment drag.
    void setOnSegmentDrag(std::function<void(int segmentIndex, bool isSourceHalf, bool isHorizontal, QPointF scenePos)> started,
                          std::function<void(QPointF scenePos)> moved,
                          std::function<void(QPointF scenePos)> finished);

    WireSegment hitSegmentAt(QPointF scenePos) const;

    // Live segment-drag highlight: the presenter pushes the dragged segment's
    // exact line on every move (index matching against the live route breaks
    // once the router slides an anchor) and clears it on release.
    void setActiveDragLine(QLineF line);
    void clearActiveDragLine();

    // Simulate-mode presentation, driven by CanvasPresenter:
    //   setDimmed(true)  -- opacity 0.3 for a transition outside the active set.
    //   setPulseColor(c) -- paint() strokes with `c` instead of the resting or
    //                       selected color while the presenter's animation runs.
    //   clearPulse()     -- ends the override; also called when the presenter
    //                       force-stops every sim animation.
    void setDimmed(bool dimmed);
    void setPulseColor(const QColor& color);
    void clearPulse();
    void setHighlighted(bool highlighted);
    bool isHighlighted() const { return highlighted_; }
    // Tints the resting stroke; selection and pulse take precedence.
    void setColor(ElementColor color);

    // The dashed leader line between baseAnchor and labelAnchor is shown only
    // during an active pill drag, and hidden on release.
    void setTransientLabelAnchor(QPointF anchor);
    void setTransientLeaderLine(bool visible);
    bool isTransientLeaderLineVisible() const { return transientLeaderVisible_; }

    QRectF boundingRect() const override;
    // Both halves widened to ~14px; the hit-test and selection surface.
    QPainterPath shape() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    quint64 id_;
    RoutedEdge routed_;
    ElementColor color_ = ElementColor::Default;
    std::function<void(bool)> onSelectedChanged_;
    std::function<void(QPointF)> onReconnectStarted_;
    std::function<void(QPointF)> onReconnectMoved_;
    std::function<void(QPointF)> onReconnectFinished_;
    std::function<void(int, bool, bool, QPointF)> onSegmentDragStarted_;
    std::function<void(QPointF)> onSegmentDragMoved_;
    std::function<void(QPointF)> onSegmentDragFinished_;
    bool reconnecting_ = false;
    bool segmentDragging_ = false;
    WireSegment activeSegment_;
    WireSegment hoveredSegment_;
    QLineF activeDragLine_;
    bool hasActiveDragLine_ = false;
    QPointF dragStartPos_;
    QColor pulseColor_;
    bool pulsing_ = false;
    bool highlighted_ = false;
    bool transientLeaderVisible_ = false;
    void updateContinuousPath();
    QPainterPath continuousPath_;
};

// A pill's event text: "always" for an eventless transition, the event name
// when there is one, "after/every N ms" for a pure timer, blank otherwise.
// Display only; the fireable click and inline editor keep the raw
// Transition::event. Shared by the canvas pill and auto-layout's pill measurement.
QString transitionPillEventText(const Transition& transition);

// The event-name pill at a routed edge's label anchor; a blank event renders a
// dimmer "." placeholder. A top-level scene item, not a child of TransitionItem
// (zValue 0): a child could not draw above StateItem (10), so the pill takes
// zValue 20 as a sibling. The presenter owns both (one id -> one edge + one
// label). Clicking the pill selects its edge (setEdgeItem); it draws a blue ring
// while that edge is selected.
class TransitionLabelItem : public QGraphicsItem {
public:
    enum { Type = QGraphicsItem::UserType + 3 };
    int type() const override { return Type; }

    TransitionLabelItem();

    void setEvent(const QString& event);
    // Updates event, guard and action together.
    void setLabelData(const QString& event, const QString& guard, const QString& action, bool always = false);
    bool isAlways() const { return always_; }
    // Centers the pill on `sceneAnchor` (the routed edge's labelAnchor).
    void setAnchor(QPointF sceneAnchor);

    // The pill body in scene coordinates, which PillPortResolver treats as a
    // node on the edge. The `center` overload answers for a pill about to be
    // placed there, for a live drag before the item has moved.
    QRectF sceneRectAt(QPointF center) const;
    QRectF sceneRect() const;

    // Non-owning; set once by the presenter right after both items are created.
    void setEdgeItem(TransitionItem* edge);

    // A press-and-move past the drag threshold moves the pill live (view-local);
    // release fires this with the pill's new center scene position and the
    // presenter commits one MoveTransitionLabelRequested. A press released
    // below the threshold is a click (select, then click again to edit).
    void setOnDragFinished(std::function<void(QPointF centerScenePos)> cb);
    // Fired on every drag move past the threshold with the cursor's raw pill
    // target, so the presenter can re-bend the edge and pin the pill back onto
    // the bent route. Each move recomputes from the cursor (scenePos -
    // grabOffset_), not from the pill's overridden position. The document is
    // touched only by onDragFinished.
    void setOnDragMoved(std::function<void(QPointF centerScenePos)> cb);
    // While false (Simulate lockdown), clicking the pill does nothing.
    void setInteractive(bool interactive);

    // Simulate-mode presentation, driven by updateSimPresentation(). fireable and
    // waitingAccent are never both true, and both are false outside Simulate+running.
    // setFireable(true) makes a clickable blue button whose click calls
    // onFireClicked() instead of selecting the edge, even if interactive_ is true.
    // setWaitingAccent(true) marks an armed delayed transition (blank event,
    // delayMs > 0) with a dim cyan ring, not clickable. setDimmed: opacity 0.3.
    void setFireable(bool fireable);
    void setWaitingAccent(bool waiting);
    void setDimmed(bool dimmed);
    void setHighlighted(bool highlighted);
    bool isHighlighted() const { return highlighted_; }
    // Invoked from mousePressEvent only while fireable_ is true.
    void setOnFireClicked(std::function<void()> cb);

    // Fired on a click while this pill's edge is already selected; a first,
    // selecting click never fires it.
    void setOnEditRequested(std::function<void()> cb);

    QRectF boundingRect() const override;
    void paint(QPainter* painter, const QStyleOptionGraphicsItem* option, QWidget* widget) override;

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override;
    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override;
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent* event) override;

private:
    void recomputeGeometry();
    // The deferred click action (select or open the editor) that a drag suppresses.
    void performClickAction(QGraphicsSceneMouseEvent* event);

    QString event_;
    QString guard_;
    QString action_;
    bool always_ = false;
    QFont font_;
    QRectF rect_;  // local, centered at (0,0) -- pos() is the pill's center
    TransitionItem* edge_ = nullptr;
    bool interactive_ = true;
    bool fireable_ = false;
    bool waitingAccent_ = false;
    bool highlighted_ = false;
    bool dragging_ = false;
    QPointF grabOffset_;  // pressScenePos - pos() at press time
    std::function<void()> onFireClicked_;
    std::function<void()> onEditRequested_;
    std::function<void(QPointF)> onDragFinished_;
    std::function<void(QPointF)> onDragMoved_;
};

}  // namespace app
