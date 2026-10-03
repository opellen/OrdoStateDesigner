// CanvasPresenter gestures: every interaction-session handler and commit hook,
// plus the Delete and Escape keys.
#include "view/canvas/canvas_presenter.h"

#include <algorithm>
#include <cmath>

#include <QBrush>
#include <QColor>
#include <QCursor>
#include <QDebug>
#include <QGraphicsEllipseItem>
#include <QGraphicsItem>
#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGuiApplication>
#include <QLineF>
#include <QMenu>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QSet>
#include <QVector>

#include "model/undo_events.h"
#include "view/canvas/canvas_view.h"
#include "view/items/machine_frame_item.h"
#include "view/items/note_item.h"
#include "view/items/state_item.h"
#include "view/items/transition_item.h"
#include "view/canvas/item_registry_delegate.h"
#include "view/geometry/pill_port_resolver.h"

#include "view/canvas/canvas_presenter_detail.h"

namespace app {

namespace {

using InteractionState = CanvasInteractionFsm::State;

using presenter_detail::ScopedUndoBatch;

// A gesture report without its session in flight is a logged no-op, never a
// commit and never a silent drop.
void logIgnoredGesture(const char* gesture, InteractionState state) {
    qDebug().noquote() << QStringLiteral("[interaction] %1 ignored in %2")
                              .arg(QLatin1String(gesture), CanvasInteractionFsm::stateName(state));
}

QPointF clampPillDragPosition(const MachineDocAgent* doc, const ItemRegistryDelegate& items,
                              quint64 transitionId, QPointF centerScenePos) {
    if (doc == nullptr) {
        return centerScenePos;
    }
    const Transition* transition = doc->findTransition(transitionId);
    if (transition == nullptr) {
        return centerScenePos;
    }
    if (transition->from == 0 || transition->to == 0 || transition->from == transition->to) {
        return centerScenePos;
    }
    QRectF sRect;
    QRectF tRect;
    if (StateItem* sItem = items.stateItems().value(transition->from, nullptr)) {
        sRect = sItem->sceneRect();
    }
    if (StateItem* tItem = items.stateItems().value(transition->to, nullptr)) {
        tRect = tItem->sceneRect();
    }
    TransitionLabelItem* labelItem = items.transitionLabels().value(transitionId, nullptr);
    const QSizeF pillSize = labelItem != nullptr ? labelItem->sceneRect().size() : QSizeF();
    return clampPillNodeKeepout(centerScenePos, pillSize, sRect, tRect);
}

}  // namespace

void CanvasPresenter::onDeleteRequested() {
    if (currentMode_ != events::Mode::Design) {
        return;  // deletes would be rejected anyway
    }

    struct Selected {
        enum class Kind { State, Transition, Note };
        Kind kind;
        quint64 id;
    };
    QVector<Selected> targets;
    for (QGraphicsItem* item : scene_->selectedItems()) {
        if (item->type() == StateItem::Type) {
            targets.push_back({Selected::Kind::State, static_cast<StateItem*>(item)->id()});
        } else if (item->type() == TransitionItem::Type) {
            targets.push_back({Selected::Kind::Transition, static_cast<TransitionItem*>(item)->id()});
        } else if (item->type() == NoteItem::Type) {
            targets.push_back({Selected::Kind::Note, static_cast<NoteItem*>(item)->id()});
        }
        // The frame is the machine itself, never deleted.
    }

    // One undo batch; an empty batch pushes no undo entry.
    ScopedUndoBatch batch(context());

    // Ids were collected while every item was alive; items die during the
    // sends, so each send goes by id and re-checks the registry (a cascade
    // may already have removed it). Transitions go before states.
    for (const Selected& target : targets) {
        if (target.kind == Selected::Kind::Transition && itemRegistry_.transitionItems().contains(target.id)) {
            context().send(events::DeleteTransitionRequested{.id = target.id});
        }
    }
    for (const Selected& target : targets) {
        if (target.kind == Selected::Kind::State && itemRegistry_.stateItems().contains(target.id)) {
            context().send(events::DeleteStateRequested{.id = target.id});
        }
    }
    for (const Selected& target : targets) {
        if (target.kind == Selected::Kind::Note && itemRegistry_.noteItems().contains(target.id)) {
            context().send(events::DeleteNoteRequested{.id = target.id});
        }
    }
}

void CanvasPresenter::onEscapePressed() {
    hideCodeLens();
    clearVariableHighlight();
    clearWireDragState();
    clearInlineEditState();
    if (fsm_.state() == InteractionState::FrameDrag) {
        dragSession_.cancelFramePreview();  // nothing was sent yet, so restore the preview
    } else if (fsm_.state() == InteractionState::NodeDrag) {
        dragSession_.cancelSubtreeDrag();  // a no-op for a plain leaf drag
        clearNodeDropHighlight();
        dragSession_.clearAlignmentGuides();
        dragSession_.clearSessionState();
    } else if (fsm_.state() == InteractionState::PillDrag) {
        dragSession_.clearAlignmentGuides();
        if (TransitionItem* edgeItem = itemRegistry_.transitionItems().value(fsm_.pillDrag().transitionId, nullptr)) {
            edgeItem->setTransientLeaderLine(false);
        }
        routing_.rerouteTransition(fsm_.pillDrag().transitionId);
    }
    fsm_.sessionAborted();  // whatever session was in flight ends at Idle, with no commit
}

void CanvasPresenter::onWireDragStarted(quint64 stateId, PortSide side) {
    StateItem* source = itemRegistry_.stateItems().value(stateId, nullptr);
    if (source == nullptr) {
        return;
    }
    // The FSM refuses the entry in Simulate mode or mid-session.
    fsm_.wireGrabbed();
    if (fsm_.state() != InteractionState::WireDrag) {
        return;
    }
    fsm_.wireDrag() = CanvasInteractionFsm::WireDrag{
        .sourceId = stateId, .side = side, .startAnchor = sidePortAnchor(source->sceneRect(), side, 0.0)};
    wireDrag_.ensureOverlay();
    wireDrag_.updateVisual(fsm_.wireDrag().startAnchor, fsm_.wireDrag().side, fsm_.wireDrag().startAnchor);
}

void CanvasPresenter::onFrameWireDragStarted(PortSide side) {
    if (frameItem_ == nullptr || !frameItem_->isVisible()) {
        return;
    }
    // sourceId 0 (the machine root) selects commitWire's frame branch.
    fsm_.wireGrabbed();
    if (fsm_.state() != InteractionState::WireDrag) {
        return;
    }
    fsm_.wireDrag() = CanvasInteractionFsm::WireDrag{
        .sourceId = 0, .side = side, .startAnchor = sidePortAnchor(frameItem_->sceneFrameRect(), side, 0.0)};
    wireDrag_.ensureOverlay();
    wireDrag_.updateVisual(fsm_.wireDrag().startAnchor, side, fsm_.wireDrag().startAnchor);
}

void CanvasPresenter::onWireDragMoved(QPointF scenePos) {
    // Serves both wire and reconnect drags.
    if (fsm_.state() != InteractionState::WireDrag && fsm_.state() != InteractionState::ReconnectDrag) {
        logIgnoredGesture("wire drag move", fsm_.state());
        return;
    }

    const WireDragDelegate::SnapResult snap = wireDrag_.scan(scenePos);
    if (snap.targetId != fsm_.wireDrag().snapTargetId) {
        if (StateItem* previous = itemRegistry_.stateItems().value(fsm_.wireDrag().snapTargetId, nullptr)) {
            previous->setDropHighlighted(false);
        }
        if (StateItem* next = itemRegistry_.stateItems().value(snap.targetId, nullptr)) {
            next->setDropHighlighted(true);
        }
        fsm_.wireDrag().snapTargetId = snap.targetId;
    }
    wireDrag_.updateVisual(fsm_.wireDrag().startAnchor, fsm_.wireDrag().side, snap.freeEnd);
}

void CanvasPresenter::onWireDragFinished(QPointF scenePos) {
    if (fsm_.state() != InteractionState::WireDrag) {
        logIgnoredGesture("wire drag finish", fsm_.state());
        return;
    }
    fsm_.wireDrag().releasePos = scenePos;
    fsm_.sessionCommitted();  // -> commitWire()
    // Back in Idle: open any inline editor commitWire recorded.
    drainPendingInlineEdit();
}

void CanvasPresenter::commitWire() {
    const quint64 sourceId = fsm_.wireDrag().sourceId;
    const quint64 targetId = fsm_.wireDrag().snapTargetId;
    const PortSide side = fsm_.wireDrag().side;
    // A release near the start anchor is a click on the handle, not a drag.
    const bool stationaryClick = std::hypot(fsm_.wireDrag().releasePos.x() - fsm_.wireDrag().startAnchor.x(),
                                            fsm_.wireDrag().releasePos.y() - fsm_.wireDrag().startAnchor.y()) < 8.0;
    clearWireDragState();
    if (stationaryClick) {
        if (sourceId == 0) {
            // Frame handle click: a targetless machine event whose pill is
            // seeded just outside the clicked side.
            std::optional<QPointF> seed;
            if (frameItem_ != nullptr && frameItem_->isVisible()) {
                const QRectF frameRect = frameItem_->sceneFrameRect();
                seed = sidePortAnchor(frameRect, side, 0.0) + outwardNormal(side) * 52.0;
            }
            addAttachedTransition(0, 0, /*machineSelf=*/false, seed);
        } else {
            addTransitionInDirection(sourceId, side);
        }
        return;
    }
    // The target is the last snap, not the release point; the source itself
    // makes a self-transition. No target cancels: targetless transitions
    // are an explicit verb, never an accidental drop.
    if (targetId != 0) {
        context().send(events::AddTransitionRequested{.from = sourceId, .to = targetId});
    }
}

void CanvasPresenter::onReconnectDragStarted(quint64 transitionId, QPointF scenePos) {
    if (doc_ == nullptr) {
        return;
    }
    const Transition* transition = doc_->findTransition(transitionId);
    StateItem* source = transition != nullptr ? itemRegistry_.stateItems().value(transition->from, nullptr) : nullptr;
    if (source == nullptr) {
        return;
    }
    fsm_.reconnectGrabbed();
    if (fsm_.state() != InteractionState::ReconnectDrag) {
        return;
    }
    // Fills the WireDrag payload too, so onWireDragMoved's snap works unchanged.
    fsm_.reconnectDrag() = CanvasInteractionFsm::ReconnectDrag{.transitionId = transitionId};
    const PortSide side = sideFor(source->sceneRect().center(), scenePos);
    fsm_.wireDrag() = CanvasInteractionFsm::WireDrag{
        .sourceId = transition->from, .side = side, .startAnchor = sidePortAnchor(source->sceneRect(), side, 0.0)};
    wireDrag_.ensureOverlay();
    wireDrag_.updateVisual(fsm_.wireDrag().startAnchor, fsm_.wireDrag().side, scenePos);
}

void CanvasPresenter::onReconnectDragFinished(QPointF scenePos) {
    Q_UNUSED(scenePos);
    if (fsm_.state() != InteractionState::ReconnectDrag) {
        logIgnoredGesture("reconnect drag finish", fsm_.state());
        return;
    }
    fsm_.sessionCommitted();  // -> commitReconnect()
}

void CanvasPresenter::commitReconnect() {
    const quint64 transitionId = fsm_.reconnectDrag().transitionId;
    const quint64 targetId = fsm_.wireDrag().snapTargetId;
    clearWireDragState();
    const Transition* transition = doc_ != nullptr ? doc_->findTransition(transitionId) : nullptr;
    if (transition == nullptr || targetId == 0 || targetId == transition->to) {
        return;  // dropped on empty canvas, or nothing actually changed
    }
    context().send(
        events::RetargetTransitionRequested{.id = transitionId, .from = transition->from, .to = targetId});
}

void CanvasPresenter::onNodeDragStarted(quint64 stateId) {
    // The verdict starts at the current parent, so releasing in place stays
    // put. It rides the entry event, so compute it before the request.
    const State* draggedState = doc_ != nullptr ? doc_->findState(stateId) : nullptr;
    const quint64 startParentId = draggedState != nullptr ? draggedState->parentId : 0;
    fsm_.nodeGrabbed(startParentId);
    if (fsm_.state() != InteractionState::NodeDrag) {
        return;  // Simulate, or another session already owns the canvas
    }
    fsm_.nodeDrag() = CanvasInteractionFsm::NodeDrag{.stateId = stateId};

    dragSession_.armForDrag(stateId);
    // The highlight names who will own the state at release.
    if (StateItem* owner = itemRegistry_.stateItems().value(startParentId, nullptr)) {
        owner->setDropHighlighted(true);
    }
}

void CanvasPresenter::onNodeDragFinished(quint64 stateId, QPointF pos) {
    if (fsm_.state() != InteractionState::NodeDrag || fsm_.nodeDrag().stateId != stateId) {
        logIgnoredGesture("node drag finish", fsm_.state());
        return;
    }
    fsm_.nodeDrag().pos = pos;
    updateDropCandidate();
    fsm_.sessionCommitted();  // -> commitNodeMove()
}

void CanvasPresenter::onNodeDragReverted(quint64 stateId) {
    if (fsm_.state() != InteractionState::NodeDrag || fsm_.nodeDrag().stateId != stateId) {
        return;
    }
    // Back at the press position, descendants included: nothing to restore.
    clearNodeDropHighlight();
    dragSession_.clearAlignmentGuides();
    dragSession_.clearSessionState();
    fsm_.sessionAborted();  // travelled and came back: a session to close, nothing to commit
}

void CanvasPresenter::commitNodeMove() {
    // A subtree drag or a reparent sends several commands, batched so one
    // Ctrl+Z reverts the whole gesture. Order: root, descendants, reparent.
    const quint64 stateId = fsm_.nodeDrag().stateId;
    const DragSessionDelegate::NodeCommitPlan plan =
        dragSession_.planNodeCommit(stateId, fsm_.nodeDrag().pos, fsm_.dropTargetId());

    if (plan.needsBatch) {
        ScopedUndoBatch batch(context());
        for (const DragSessionDelegate::NodeCommitPlan::Move& move : plan.moves) {
            context().send(events::MoveStateRequested{.id = move.id, .pos = move.pos});
        }
        if (plan.reparent) {
            // The command validates; if it refuses, the moves still stand.
            context().send(events::ReparentStateRequested{.id = stateId, .parentId = plan.reparentTo});
        }
    } else {
        for (const DragSessionDelegate::NodeCommitPlan::Move& move : plan.moves) {
            context().send(events::MoveStateRequested{.id = move.id, .pos = move.pos});
        }
    }

    clearNodeDropHighlight();
    dragSession_.clearAlignmentGuides();
    dragSession_.clearSessionState();
}

void CanvasPresenter::updateDropCandidate() {
    const quint64 draggedId = fsm_.nodeDrag().stateId;
    const QPointF mousePos = view_ != nullptr ? view_->lastMouseScenePos() : QPointF();
    const quint64 newTargetId = dragSession_.computeDropTarget(draggedId, mousePos);
    const quint64 currentTargetId = fsm_.dropTargetId();
    if (newTargetId == currentTargetId) {
        return;  // unchanged verdict: no event -- the machine hears boundaries only
    }
    if (StateItem* previous = itemRegistry_.stateItems().value(currentTargetId, nullptr)) {
        previous->setDropHighlighted(false);
    }
    if (StateItem* next = itemRegistry_.stateItems().value(newTargetId, nullptr)) {
        next->setDropHighlighted(true);
    }
    fsm_.dropTargetChanged(newTargetId);
}

void CanvasPresenter::clearNodeDropHighlight() {
    // Leaves the verdict as is; the next nodeGrabbed() re-seeds it.
    if (StateItem* target = itemRegistry_.stateItems().value(fsm_.dropTargetId(), nullptr)) {
        target->setDropHighlighted(false);
    }
    // Session end: settle every hull against the document.
    routing_.refreshContainers();
}

QPointF CanvasPresenter::resolveAlignmentAdjuster(quint64 draggedId, QPointF proposed) {
    // Only the live NodeDrag's own root aligns. The reentrant setPos from
    // refreshContainers() must pass through untouched, or the drag feeds back.
    if (routing_.refreshingContainers() || fsm_.state() != InteractionState::NodeDrag ||
        fsm_.nodeDrag().stateId != draggedId) {
        return proposed;
    }
    return dragSession_.resolveAlignment(draggedId, proposed);
}

bool CanvasPresenter::debugVerticalAlignGuideVisible() const { return dragSession_.verticalGuideVisible(); }

bool CanvasPresenter::debugHorizontalAlignGuideVisible() const { return dragSession_.horizontalGuideVisible(); }

void CanvasPresenter::onNoteDragStarted(quint64 noteId) {
    fsm_.noteGrabbed();
    if (fsm_.state() != InteractionState::NoteDrag) {
        return;  // Simulate, or another session already owns the canvas
    }
    fsm_.noteDrag() = CanvasInteractionFsm::NoteDrag{.noteId = noteId};
}

void CanvasPresenter::onNoteDragFinished(quint64 noteId, QPointF pos) {
    if (fsm_.state() != InteractionState::NoteDrag || fsm_.noteDrag().noteId != noteId) {
        logIgnoredGesture("note drag finish", fsm_.state());
        return;
    }
    fsm_.noteDrag().pos = pos;
    fsm_.sessionCommitted();  // -> commitNoteMove()
}

void CanvasPresenter::onNoteDragReverted(quint64 noteId) {
    if (fsm_.state() != InteractionState::NoteDrag || fsm_.noteDrag().noteId != noteId) {
        return;
    }
    fsm_.sessionAborted();  // travelled and came back: a session to close, nothing to commit
}

void CanvasPresenter::commitNoteMove() {
    context().send(events::MoveNoteRequested{.id = fsm_.noteDrag().noteId, .pos = fsm_.noteDrag().pos});
}

void CanvasPresenter::onFrameDragStarted(QPointF grabScenePos) {
    fsm_.frameGrabbed();
    if (fsm_.state() != InteractionState::FrameDrag) {
        return;  // Simulate, or another session already owns the canvas
    }
    fsm_.frameDrag() = CanvasInteractionFsm::FrameDrag{.grabScenePos = grabScenePos};
}

void CanvasPresenter::onFrameDragMoved(QPointF scenePos) {
    if (fsm_.state() != InteractionState::FrameDrag) {
        logIgnoredGesture("frame drag move", fsm_.state());
        return;
    }
    // Preview only; nothing is sent until release.
    dragSession_.applyFramePreview(scenePos - fsm_.frameDrag().grabScenePos);
}

void CanvasPresenter::onFrameDragFinished(QPointF scenePos) {
    if (fsm_.state() != InteractionState::FrameDrag) {
        logIgnoredGesture("frame drag finish", fsm_.state());
        return;
    }
    onFrameDragMoved(scenePos);  // final live update at the exact release position
    fsm_.sessionCommitted();     // -> commitFrameMove()
}

void CanvasPresenter::onFrameDragReverted() {
    if (fsm_.state() != InteractionState::FrameDrag) {
        return;
    }
    // The last move already applied a zero delta: nothing to restore.
    fsm_.sessionAborted();
}

void CanvasPresenter::commitFrameMove() {
    if (doc_ == nullptr) {
        return;
    }
    // One move per state in one undo batch.
    ScopedUndoBatch batch(context());
    for (auto it = itemRegistry_.stateItems().begin(); it != itemRegistry_.stateItems().end(); ++it) {
        context().send(events::MoveStateRequested{.id = it.key(), .pos = it.value()->pos()});
    }
}

void CanvasPresenter::onRubberBandStarted() {
    // Qt already runs the band; the FSM only decides whether it is legal.
    fsm_.bandStarted();
}

void CanvasPresenter::onRubberBandFinished() {
    if (fsm_.state() != InteractionState::RubberBand) {
        logIgnoredGesture("rubber band finish", fsm_.state());
        return;
    }
    fsm_.sessionCommitted();  // -> commitBandSelection()
}

void CanvasPresenter::commitBandSelection() {
    // Qt already applied the selection; report it if it changed.
    emitSelectionChanged();
}

void CanvasPresenter::onLabelDragMoved(quint64 transitionId, QPointF centerScenePos, std::optional<bool> altOverride) {
    if (fsm_.state() == InteractionState::Idle) {
        // The label reports only past its drag threshold, so this opens the session.
        fsm_.pillGrabbed();
        if (fsm_.state() != InteractionState::PillDrag) {
            return;
        }
        fsm_.pillDrag() = CanvasInteractionFsm::PillDrag{.transitionId = transitionId};
    }
    if (fsm_.state() != InteractionState::PillDrag || fsm_.pillDrag().transitionId != transitionId) {
        logIgnoredGesture("pill drag move", fsm_.state());
        return;
    }

    const bool isAlt = altOverride.value_or((QGuiApplication::keyboardModifiers() & Qt::AltModifier) != 0);
    fsm_.pillDrag().isAltDrag = isAlt;

    TransitionItem* edgeItem = itemRegistry_.transitionItems().value(transitionId, nullptr);
    TransitionLabelItem* labelItem = itemRegistry_.transitionLabels().value(transitionId, nullptr);

    if (isAlt) {
        // Alt+drag detaches the label; the wire stays put and a dashed leader
        // line ties the pill to its base while the drag lasts.
        if (edgeItem != nullptr) {
            edgeItem->setTransientLabelAnchor(centerScenePos);
            edgeItem->setTransientLeaderLine(true);
        }
        if (labelItem != nullptr) {
            labelItem->setAnchor(centerScenePos);
        }
        fsm_.pillDrag().centerScenePos = centerScenePos;
        return;
    }

    // A plain drag moves only the pill offset, clamped clear of the nodes and
    // snapped to port lines. The leader line shows too (empty when the wire
    // follows the pill); every release and abort path hides it.
    centerScenePos = clampPillDragPosition(doc_.get(), itemRegistry_, transitionId, centerScenePos);
    centerScenePos = dragSession_.resolvePillSnap(transitionId, centerScenePos);
    centerScenePos = clampPillDragPosition(doc_.get(), itemRegistry_, transitionId, centerScenePos);
    fsm_.pillDrag().centerScenePos = centerScenePos;
    previewLabelDrag(transitionId, centerScenePos);
    if (edgeItem != nullptr) {
        edgeItem->setTransientLeaderLine(true);
    }
}

void CanvasPresenter::onLabelDragFinished(quint64 transitionId, QPointF centerScenePos, std::optional<bool> altOverride) {
    if (fsm_.state() != InteractionState::PillDrag || fsm_.pillDrag().transitionId != transitionId) {
        logIgnoredGesture("pill drag finish", fsm_.state());
        return;
    }
    if (altOverride.has_value()) {
        fsm_.pillDrag().isAltDrag = *altOverride;
    }
    if (TransitionItem* edgeItem = itemRegistry_.transitionItems().value(transitionId, nullptr)) {
        edgeItem->setTransientLeaderLine(false);
    }

    if (!fsm_.pillDrag().isAltDrag) {
        centerScenePos = clampPillDragPosition(doc_.get(), itemRegistry_, transitionId, centerScenePos);
        centerScenePos = dragSession_.resolvePillSnap(transitionId, centerScenePos);
        centerScenePos = clampPillDragPosition(doc_.get(), itemRegistry_, transitionId, centerScenePos);
    }
    dragSession_.clearAlignmentGuides();
    fsm_.pillDrag().centerScenePos = centerScenePos;
    fsm_.sessionCommitted();  // -> commitPillOffset()
    // Flip memory belongs to the session: reroutes outside a drag have no
    // hysteresis. Aborts need no reset, since a new session starts fresh.
    fsm_.pillDrag().incumbent.reset();
}

void CanvasPresenter::commitPillOffset() {
    // One intent per finished drag: the offset from the pill's base anchor.
    const quint64 transitionId = fsm_.pillDrag().transitionId;
    if (doc_ == nullptr) {
        return;
    }
    if (fsm_.pillDrag().isAltDrag) {
        routing_.setLabelDetached(transitionId, true);
        QPointF base = routing_.labelBaseAnchor(transitionId);
        if (base.isNull() && routing_.hasAutoLabelBaseAnchor(transitionId)) {
            base = routing_.autoLabelBaseAnchor(transitionId);
        }
        if (!base.isNull()) {
            context().send(events::MoveTransitionLabelRequested{
                .id = transitionId,
                .offset = fsm_.pillDrag().centerScenePos - base,
                .resetBendpoints = false});
        }
        return;
    }

    routing_.setLabelDetached(transitionId, false);
    if (!routing_.hasAutoLabelBaseAnchor(transitionId)) {
        return;
    }
    // The wire does not follow the pill, so authored bendpoints are kept.
    context().send(events::MoveTransitionLabelRequested{
        .id = transitionId,
        .offset = fsm_.pillDrag().centerScenePos - routing_.autoLabelBaseAnchor(transitionId),
        .resetBendpoints = false});
}

void CanvasPresenter::clearWireDragState() {
    if (StateItem* target = itemRegistry_.stateItems().value(fsm_.wireDrag().snapTargetId, nullptr)) {
        target->setDropHighlighted(false);
    }
    fsm_.wireDrag() = CanvasInteractionFsm::WireDrag{};
    fsm_.reconnectDrag() = CanvasInteractionFsm::ReconnectDrag{};
    wireDrag_.hideOverlay();
}

void CanvasPresenter::previewLabelDrag(quint64 transitionId, QPointF pillCenter) {
    if (doc_ == nullptr) {
        return;
    }
    const Transition* transition = doc_->findTransition(transitionId);
    TransitionItem* edgeItem = itemRegistry_.transitionItems().value(transitionId, nullptr);
    TransitionLabelItem* labelItem = itemRegistry_.transitionLabels().value(transitionId, nullptr);
    if (transition == nullptr || edgeItem == nullptr || labelItem == nullptr) {
        return;
    }
    pillCenter = clampPillDragPosition(doc_.get(), itemRegistry_, transitionId, pillCenter);
    // An authored course keeps its skeleton; only the label follows the cursor.
    RoutedEdge routed;
    if (routing_.routeManual(*transition, transition->labelOffset, &routed)) {
        routed.labelAnchor = pillCenter;
    } else {
        // Self/targetless/root wires follow the pill's ports; the assignment
        // becomes the next move's flip memory.
        PortAssignment ports;
        if (!routing_.routePillPorts(*transition, pillCenter, fsm_.pillDrag().incumbent, &routed, &ports)) {
            return;
        }
        fsm_.pillDrag().incumbent = ports;
    }
    edgeItem->setRoute(routed);
    labelItem->setAnchor(routed.labelAnchor);
    // The frame grows live as a pill is dragged outward.
    routing_.refreshFrame();
}

void CanvasPresenter::onSegmentDragStarted(quint64 transitionId, int segmentIndex, bool isSourceHalf,
                                           bool isHorizontal, QPointF scenePos) {
    if (fsm_.state() != InteractionState::Idle) {
        return;
    }
    TransitionItem* edgeItem = itemRegistry_.transitionItems().value(transitionId, nullptr);
    if (edgeItem == nullptr) {
        return;
    }
    const QVector<QPointF> waypoints = canonicalWaypoints(edgeItem->debugRoute());
    if (waypoints.size() < 2 || segmentIndex < 0 || segmentIndex >= waypoints.size() - 1) {
        return;
    }
    segmentDrag_ = SegmentDragState{
        .transitionId = transitionId,
        .segmentIndex = segmentIndex,
        .isSourceHalf = isSourceHalf,
        .isHorizontal = isHorizontal,
        .pressPos = scenePos,
        .initialWaypoints = waypoints,
    };
}

CanvasPresenter::SegmentDragResolution CanvasPresenter::resolveSegmentDrag(QPointF scenePos) const {
    SegmentDragResolution resolution;
    const SegmentDragState& drag = segmentDrag_;
    const qreal rawDelta = drag.isHorizontal ? (scenePos.y() - drag.pressPos.y())
                                             : (scenePos.x() - drag.pressPos.x());
    resolution.delta = rawDelta;
    resolution.guideVertical = !drag.isHorizontal;

    const QVector<WireSegment> segs = extractWireSegments(drag.initialWaypoints, true);
    const WireSegment* dragged = nullptr;
    for (const WireSegment& seg : segs) {
        if (seg.segmentIndex == drag.segmentIndex) {
            dragged = &seg;
            break;
        }
    }
    if (dragged == nullptr) {
        return resolution;
    }
    const qreal baseCoord = drag.isHorizontal ? dragged->line.p1().y() : dragged->line.p1().x();

    // The nearest parallel sibling within the radius wins; landing exactly
    // collinear lets the commit merge the two.
    constexpr qreal kMergeSnapRadius = 6.0;
    qreal bestDist = kMergeSnapRadius + 1.0;
    for (const WireSegment& seg : segs) {
        if (seg.segmentIndex == drag.segmentIndex || seg.orientation != dragged->orientation) {
            continue;
        }
        const qreal coord = drag.isHorizontal ? seg.line.p1().y() : seg.line.p1().x();
        const qreal dist = std::abs(coord - (baseCoord + rawDelta));
        if (dist <= kMergeSnapRadius && dist < bestDist) {
            bestDist = dist;
            resolution.snapped = true;
            resolution.guideCoord = coord;
            resolution.delta = coord - baseCoord;
            resolution.partnerLine = seg.line;
        }
    }

    const QPointF shift = drag.isHorizontal ? QPointF(0.0, resolution.delta) : QPointF(resolution.delta, 0.0);
    resolution.draggedLine = QLineF(dragged->line.p1() + shift, dragged->line.p2() + shift);
    return resolution;
}

void CanvasPresenter::onSegmentDragMoved(quint64 transitionId, QPointF scenePos) {
    if (segmentDrag_.transitionId != transitionId || segmentDrag_.initialWaypoints.isEmpty()) {
        return;
    }
    TransitionItem* edgeItem = itemRegistry_.transitionItems().value(transitionId, nullptr);
    if (edgeItem == nullptr) {
        return;
    }
    const SegmentDragResolution resolution = resolveSegmentDrag(scenePos);
    const QVector<QPointF> newWaypoints =
        translateOrthogonalSegment(segmentDrag_.initialWaypoints, segmentDrag_.segmentIndex, resolution.delta);

    const Transition* transition = doc_ != nullptr ? doc_->findTransition(transitionId) : nullptr;
    const QPointF labelOffset = transition != nullptr ? transition->labelOffset : QPointF();
    TransitionLabelItem* labelItem = itemRegistry_.transitionLabels().value(transitionId, nullptr);

    // Route the live skeleton through the same builder the commit uses, so
    // the preview matches the result. Targetless/root wires fall back to the
    // raw polyline.
    QVector<QPointF> liveBendpoints;
    for (int i = 1; i < newWaypoints.size() - 1; ++i) {
        liveBendpoints.push_back(newWaypoints[i]);
    }
    RoutedEdge liveRoute;
    const bool manualLive = transition != nullptr &&
                            routing_.routeManualCourse(*transition, liveBendpoints, labelOffset, &liveRoute);
    if (!manualLive) {
        liveRoute = routeEdgeFromWaypoints(newWaypoints, 10.0, labelOffset,
                                           labelItem != nullptr ? labelItem->sceneRect().size() : QSizeF());
    }
    edgeItem->setRoute(liveRoute);

    // Highlight the drawn segment nearest the resolved line, not the raw
    // cursor line, so highlight, wire, and merge guide coincide.
    QLineF drawnDragLine = resolution.draggedLine;
    const QVector<WireSegment> liveSegs = extractWireSegments(canonicalWaypoints(liveRoute), true);
    qreal bestSegDist = 1e18;
    for (const WireSegment& seg : liveSegs) {
        if ((seg.orientation == WireSegment::Orientation::Horizontal) == segmentDrag_.isHorizontal) {
            const qreal dist = segmentDrag_.isHorizontal
                                   ? std::abs(seg.line.p1().y() - resolution.draggedLine.p1().y())
                                   : std::abs(seg.line.p1().x() - resolution.draggedLine.p1().x());
            if (dist < bestSegDist) {
                bestSegDist = dist;
                drawnDragLine = seg.line;
            }
        }
    }
    edgeItem->setActiveDragLine(drawnDragLine);
    segmentDrag_.lastDrawnWaypoints = canonicalWaypoints(liveRoute);
    if (labelItem != nullptr) {
        labelItem->setAnchor(liveRoute.labelAnchor);
    }

    if (resolution.snapped) {
        dragSession_.showSegmentMergeGuide(
            resolution.guideVertical, resolution.guideCoord,
            QRectF(drawnDragLine.p1(), drawnDragLine.p2()).normalized(),
            QRectF(resolution.partnerLine.p1(), resolution.partnerLine.p2()).normalized());
    } else {
        dragSession_.clearAlignmentGuides();
    }

    // The dragged skeleton grows the frame live.
    const QVector<QPointF>& liveSkeleton = liveRoute.waypoints.size() >= 2 ? liveRoute.waypoints : newWaypoints;
    QRectF liveBounds;
    for (const QPointF& pt : liveSkeleton) {
        liveBounds = liveBounds.united(QRectF(pt - QPointF(1.0, 1.0), QSizeF(2.0, 2.0)));
    }
    routing_.setTransientContentRect(liveBounds);
    routing_.refreshFrame();
}

void CanvasPresenter::onSegmentDragFinished(quint64 transitionId, QPointF scenePos) {
    if (segmentDrag_.transitionId != transitionId) {
        return;
    }
    // From here the frame grows from the committed bendpoints instead.
    routing_.clearTransientContentRect();
    dragSession_.clearAlignmentGuides();
    const SegmentDragResolution resolution = resolveSegmentDrag(scenePos);
    const qreal delta = resolution.delta;
    const bool actuallyMoved = std::abs(delta) >= 3.0;
    TransitionItem* edgeItem = itemRegistry_.transitionItems().value(transitionId, nullptr);
    if (edgeItem != nullptr) {
        edgeItem->clearActiveDragLine();
    }

    if (actuallyMoved && edgeItem != nullptr) {
        // Commit the drawn course, which the router may have adjusted; the raw
        // translate is only for a release that never saw a move.
        const QVector<QPointF> finalWaypoints =
            segmentDrag_.lastDrawnWaypoints.size() >= 2
                ? segmentDrag_.lastDrawnWaypoints
                : translateOrthogonalSegment(segmentDrag_.initialWaypoints, segmentDrag_.segmentIndex, delta);

        // Bendpoints are the interior waypoints.
        QVector<QPointF> bendpoints;
        if (finalWaypoints.size() >= 3) {
            for (int i = 1; i < finalWaypoints.size() - 1; ++i) {
                bendpoints.push_back(finalWaypoints[i]);
            }
        }
        context().send(events::SetTransitionBendpointsRequested{
            .id = transitionId,
            .bendpoints = bendpoints,
        });
    } else if (edgeItem != nullptr) {
        edgeItem->setSelected(true);
        routing_.rerouteTransition(transitionId);
        routing_.refreshFrame();  // shrink back any live transient growth
    }
    segmentDrag_ = SegmentDragState{};
}

}  // namespace app
