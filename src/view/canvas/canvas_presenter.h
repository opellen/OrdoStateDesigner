#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <QHash>
#include <QLineF>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QtGlobal>

#include <ordo/qt/presenter.h>

#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/sim_agent.h"
#include "model/sim_events.h"
#include "view/items/action_box.h"
#include "view/canvas/canvas_interaction_fsm.h"
#include "view/canvas/canvas_menus.h"
#include "view/canvas/drag_session_delegate.h"
#include "view/geometry/edge_router.h"
#include "view/canvas/inline_edit_controller.h"
#include "view/canvas/item_registry_delegate.h"
#include "view/geometry/node_anchors.h"
#include "view/geometry/pill_port_resolver.h"
#include "view/canvas/routing_delegate.h"
#include "view/canvas/selection_delegate.h"
#include "view/canvas/sim_presentation.h"
#include "view/canvas/wire_drag_delegate.h"

class QGraphicsScene;
class QGraphicsSimpleTextItem;
class QTextEdit;

namespace app {

class CanvasView;
class StateItem;
class MachineFrameItem;
class TransitionItem;
class TransitionLabelItem;
class NoteItem;
class CodeLensItem;
}  // namespace app

class QMenu;

namespace app {

// Presenter over the canvas scene: mirrors the document's facts into scene
// items and turns view input into intents. onRegister() requests a snapshot,
// so a new presenter always repopulates. Every commit* method is an FSM hook
// and must follow CanvasInteractionFsm's reentrancy rule.
class CanvasPresenter : public ordo::qt::Presenter {
    Q_OBJECT

public:
    // Takes ownership of neither; both must outlive the presenter.
    CanvasPresenter(QGraphicsScene* scene, CanvasView* view);

    void onRegister() override;

    using Selection = CanvasSelection;

    // Re-derived on every call. selectionChanged() only fires on the next
    // change, so a newly attached listener calls this once.
    Selection currentSelection() const;

    // Selects exactly this item through the same scene signal path a click
    // uses. No-op if `id` has no item, and in Simulate mode (items are not
    // selectable there).
    void selectState(quint64 id);
    void selectTransition(quint64 id);

    // View verbs, never edits. Fits the elements' union (a transition counts
    // its edge plus its pill); ids without an item are skipped, and an empty
    // result is Zoom to Fit.
    void zoomToElements(const QList<quint64>& stateIds, const QList<quint64>& transitionIds);
    // The union of everything selected, frame included; empty is Zoom to Fit.
    void zoomToSelection();
    void zoomToFit();
    // Scrolls the elements into view at the current zoom; no-op if visible.
    void revealElements(const QList<quint64>& stateIds, const QList<quint64>& transitionIds);

    // Highlights transitions whose guard or action references `varName` and
    // dims everything else.
    void highlightTransitionsForVariable(const QString& varName);
    void clearVariableHighlight();
    QString activeHighlightedVariable() const { return activeHighlightedVariable_; }

    bool debugIsTransitionHighlighted(quint64 id) const;
    bool debugIsTransitionDimmed(quint64 id) const;
    bool debugIsStateDimmed(quint64 id) const;
    bool debugIsTransitionAlways(quint64 id) const;

public:

    // ---- probe hooks ----
    void debugSelectState(quint64 id) { selectState(id); }
    void debugSelectTransition(quint64 id) { selectTransition(id); }

    // 0 when no initial marker is drawn.
    quint64 debugInitialMarkerStateId() const { return routing_.initialMarkerStateId(); }

    bool debugHasCompoundInitialMarker(quint64 stateId) const { return routing_.hasCompoundInitialMarker(stateId); }
    // A feedback loop yields correct geometry many times over, so probes
    // assert the pass count.
    int debugContainerRefreshPasses() const { return routing_.debugRefreshPassCount(); }
    void debugResetContainerRefreshPasses() { routing_.debugResetRefreshPassCount(); }

    bool debugStateGlowing(quint64 stateId) const {
        return currentMode_ == events::Mode::Simulate && sim_ != nullptr && sim_->running() &&
               simPresentation_.configuration().contains(stateId);
    }

    // Read from the machine's own context, not the presenter's mirror.
    bool debugFsmDesignMode() const { return fsm_.context().designMode; }
    quint64 debugFsmDropTargetId() const { return fsm_.dropTargetId(); }
    CanvasInteractionFsm::State debugFsmState() const { return fsm_.state(); }

    bool debugHasTransitionVisual(quint64 transitionId) const {
        return itemRegistry_.transitionItems().contains(transitionId) &&
               itemRegistry_.transitionLabels().contains(transitionId);
    }
    // Null/origin when absent.
    const TransitionItem* debugTransitionItem(quint64 transitionId) const {
        return itemRegistry_.transitionItems().value(transitionId, nullptr);
    }
    QPointF debugLabelCenter(quint64 transitionId) const;
    StateItem* debugStateItem(quint64 stateId) const { return itemRegistry_.stateItems().value(stateId, nullptr); }
    // Null when absent.
    QRectF debugPillRect(quint64 transitionId) const;

    void debugSegmentDragStarted(quint64 transitionId, int segmentIndex, bool isSourceHalf, bool isHorizontal, QPointF scenePos) {
        onSegmentDragStarted(transitionId, segmentIndex, isSourceHalf, isHorizontal, scenePos);
    }
    void debugSegmentDragMoved(quint64 transitionId, QPointF scenePos) {
        onSegmentDragMoved(transitionId, scenePos);
    }
    void debugSegmentDragFinished(quint64 transitionId, QPointF scenePos) {
        onSegmentDragFinished(transitionId, scenePos);
    }
    void debugLabelDragMoved(quint64 transitionId, QPointF scenePos, std::optional<bool> altOverride = std::nullopt) {
        onLabelDragMoved(transitionId, scenePos, altOverride);
    }
    void debugLabelDragFinished(quint64 transitionId, QPointF scenePos, std::optional<bool> altOverride = std::nullopt) {
        onLabelDragFinished(transitionId, scenePos, altOverride);
    }

    // Context-menu verbs without exec(); the returned menu is caller-owned.
    QMenu* debugBuildStateContextMenu(quint64 stateId) { return buildStateContextMenu(stateId); }
    QMenu* debugBuildTransitionContextMenu(quint64 transitionId) { return buildTransitionContextMenu(transitionId); }
    // Add verbs must be followed by drainPendingInlineEdit().
    void debugAddSelfTransition(quint64 stateId) {
        addAttachedTransition(stateId, stateId);
        drainPendingInlineEdit();
    }
    void debugAddTargetlessTransition(quint64 stateId) {
        addAttachedTransition(stateId, 0);
        drainPendingInlineEdit();
    }
    void debugToggleBreakpoint(quint64 stateId) {
        context().send(events::ToggleBreakpointRequested{.stateId = stateId});
    }
    bool debugHasBreakpoint(quint64 stateId) const;
    bool debugIsBreakpointHit(quint64 stateId) const;

    // Gesture levers call the same handlers the items' own callbacks reach.
    void debugWireDragStarted(quint64 stateId, PortSide side) { onWireDragStarted(stateId, side); }
    void debugWireDragMoved(QPointF scenePos) { onWireDragMoved(scenePos); }
    void debugWireDragFinished(QPointF scenePos) { onWireDragFinished(scenePos); }
    void debugFrameWireDragStarted(PortSide side) { onFrameWireDragStarted(side); }
    void debugAddTransitionInDirection(quint64 stateId, PortSide side) {
        addTransitionInDirection(stateId, side);
        drainPendingInlineEdit();
    }

    void debugFrameDragStarted(QPointF grabScenePos) { onFrameDragStarted(grabScenePos); }
    void debugFrameDragMoved(QPointF scenePos) { onFrameDragMoved(scenePos); }
    void debugFrameDragFinished(QPointF scenePos) { onFrameDragFinished(scenePos); }
    void debugDeleteSelection() { onDeleteRequested(); }

    bool debugVerticalAlignGuideVisible() const;
    bool debugHorizontalAlignGuideVisible() const;

    void debugAddNote(QPointF pos) { addNoteAt(pos); }

    void debugApplySelectionColor(ElementColor color) { applyColorToSelection(color); }

    void debugAddMachineSelfTransition() {
        addAttachedTransition(0, 0, /*machineSelf=*/true);
        drainPendingInlineEdit();
    }

    void debugCanvasDoubleClicked(QPointF scenePos) { onCanvasDoubleClicked(scenePos); }

    // debugFireActionBoxVerb calls the same handler a segment click does.
    bool debugActionBoxVisible() const { return actionBox_ != nullptr && actionBox_->isVisible(); }
    QRectF debugActionBoxRect() const { return actionBox_ != nullptr ? actionBox_->sceneBoundingRect() : QRectF(); }
    std::vector<QString> debugActionBoxLabels() const {
        return actionBox_ != nullptr ? actionBox_->debugSegmentLabels() : std::vector<QString>();
    }
    std::vector<QString> debugActionBoxIconSlugs() const {
        return actionBox_ != nullptr ? actionBox_->debugSegmentIconSlugs() : std::vector<QString>();
    }
    qreal debugActionBoxIconRenderRatio() const {
        return actionBox_ != nullptr ? actionBox_->debugLastIconRenderRatio() : 0.0;
    }
    std::vector<QString> debugActionBoxTooltips() const {
        return actionBox_ != nullptr ? actionBox_->debugSegmentTooltips() : std::vector<QString>();
    }
    void debugFireActionBoxVerb(ActionBoxVerb verb) { onActionBoxVerb(verb); }
    void toggleCodeLens();
    void showCodeLensForSelection();
    void hideCodeLens();
    bool debugCodeLensVisible() const;
    QString debugCodeLensTitle() const;
    QString debugCodeLensCode() const;
    QRectF debugCodeLensRect() const;
    void debugToggleCodeLens() { toggleCodeLens(); }
    bool debugOnboardingHintVisible() const;
    // Null when `id` has no item.
    QRectF debugNoteRect(quint64 id) const;
    // Sets the open inline editor's text and commits it the way a real
    // editor would. No-op if none is open.
    void debugCommitInlineEditText(const QString& text);
    // The open note editor, for sending it real key events; nullptr unless a
    // note is being edited.
    QTextEdit* debugInlineEditAreaWidget() const;

signals:
    // Never emitted twice in a row for the same (kind, id). View-local, not
    // a kernel fact.
    void selectionChanged(app::SelectionKind kind, quint64 id);

    // A menu verb asks the Inspector to focus a field; the target is
    // already selected.
    void inspectorFieldFocusRequested(app::InspectorField field);

    // The shell owns the dialog and runs the layout on this view's session.
    void autoLayoutRequested();

    // Empty `varName` means the highlight was cleared.
    void variableHighlightChanged(const QString& varName);

private:
    // Empty when no id has an item.
    QRectF elementsRect(const QList<quint64>& stateIds, const QList<quint64>& transitionIds) const;

    // Emits selectionChanged() if the selection differs from the last one
    // emitted. Also called after removals, since it dedupes.
    void emitSelectionChanged();

    void onStateAdded(const events::StateAdded& fact);
    void onStateRenamed(const events::StateRenamed& fact);
    void onStateKindChanged(const events::StateKindChanged& fact);
    void onInitialStateChanged(const events::InitialStateChanged& fact);
    void onStateMoved(const events::StateMoved& fact);
    void onEntryActionsChanged(const events::EntryActionsChanged& fact);
    void onExitActionsChanged(const events::ExitActionsChanged& fact);
    void onTransitionAdded(const events::TransitionAdded& fact);
    void onTransitionEventChanged(const events::TransitionEventChanged& fact);
    void onTransitionGuardChanged(const events::TransitionGuardChanged& fact);
    void onTransitionActionChanged(const events::TransitionActionChanged& fact);
    void onTransitionDelayChanged(const events::TransitionDelayChanged& fact);
    void onTransitionAlwaysChanged(const events::TransitionAlwaysChanged& fact);
    void onTransitionRetargeted(const events::TransitionRetargeted& fact);
    void onTransitionLabelMoved(const events::TransitionLabelMoved& fact);
    void onTransitionLabelRatioChanged(const events::TransitionLabelRatioChanged& fact);
    void onTransitionBendpointsChanged(const events::TransitionBendpointsChanged& fact);
    void onStateDeleted(const events::StateDeleted& fact);
    void onTransitionDeleted(const events::TransitionDeleted& fact);
    void onStateReparented(const events::StateReparented& fact);
    void onInitialChildChanged(const events::InitialChildChanged& fact);
    void onNoteAdded(const events::NoteAdded& fact);
    void onNoteMoved(const events::NoteMoved& fact);
    void onNoteTextChanged(const events::NoteTextChanged& fact);
    void onNoteDeleted(const events::NoteDeleted& fact);
    // Paint-only; no reroute.
    void onStateColorChanged(const events::StateColorChanged& fact);
    void onTransitionColorChanged(const events::TransitionColorChanged& fact);
    void onNoteColorChanged(const events::NoteColorChanged& fact);
    void onSnapshotPublished(const events::MachineSnapshotPublished& fact);
    void onModeChanged(const events::ModeChanged& fact);

    // Guard results are not subscribed: the simulation look derives from
    // topology and the active configuration alone.
    void onSimulationStarted(const events::SimulationStarted& fact);
    void onSimulationPaused(const events::SimulationPaused& fact);
    void onSimulationReset(const events::SimulationReset& fact);
    void onActiveStateChanged(const events::ActiveStateChanged& fact);
    void onConfigurationChanged(const events::ConfigurationChanged& fact);
    void onTransitionFired(const events::TransitionFired& fact);
    void onBreakpointToggled(const events::BreakpointToggled& fact);
    void onBreakpointHit(const events::BreakpointHit& fact);

    void onCanvasDoubleClicked(QPointF scenePos);
    void onDeleteRequested();
    void onEscapePressed();

    // Drag sessions open on the first real move and commit on release; a
    // drag that returns to its press point reverts with nothing to commit.
    void onNodeDragStarted(quint64 stateId);
    void onNodeDragFinished(quint64 stateId, QPointF pos);
    void onNodeDragReverted(quint64 stateId);

    void onNoteDragStarted(quint64 noteId);
    void onNoteDragFinished(quint64 noteId, QPointF pos);
    void onNoteDragReverted(quint64 noteId);

    // Moves every state by the same delta from its pre-drag position; release
    // commits all the moves as one undo batch.
    void onFrameDragStarted(QPointF grabScenePos);
    void onFrameDragMoved(QPointF scenePos);
    void onFrameDragFinished(QPointF scenePos);
    void onFrameDragReverted();

    // Called on every NodeDrag move; tells the FSM only when the drop
    // verdict changes, and moves the drop highlight with it.
    void updateDropCandidate();
    // Clears the drop highlight on every NodeDrag end, commit or abort.
    void clearNodeDropHighlight();

    // Position adjuster installed on every StateItem. A no-op (returns
    // `proposed`) unless a NodeDrag is live, `draggedId` is its root, and no
    // container refresh is running. The math is dragSession_.resolveAlignment().
    QPointF resolveAlignmentAdjuster(quint64 draggedId, QPointF proposed);

    // Qt's RubberBandDrag drives the band itself; there are no move reports.
    void onRubberBandStarted();
    void onRubberBandFinished();

    // The label item's drag threshold stands in for the first real move.
    void onLabelDragMoved(quint64 transitionId, QPointF centerScenePos, std::optional<bool> altOverride = std::nullopt);
    void onLabelDragFinished(quint64 transitionId, QPointF centerScenePos, std::optional<bool> altOverride = std::nullopt);

    void onWireDragStarted(quint64 stateId, PortSide side);
    void onWireDragMoved(QPointF scenePos);
    void onWireDragFinished(QPointF scenePos);
    // The frame handle's wire drag (sourceId 0): a click makes a targetless
    // machine event, a drop on a state a targeted root transition.
    void onFrameWireDragStarted(PortSide side);
    // Stationary click on a side handle: adds a state in `side`'s direction
    // plus the transition to it and selects that transition. Runs inside the
    // commitWire hook, so it only RECORDS the inline editor; the caller must
    // call drainPendingInlineEdit() after the commit returns.
    void addTransitionInDirection(quint64 sourceId, PortSide side);

    // Self (targetId == sourceId) or targetless (targetId == 0) transition,
    // named "Event N" in one undo batch. Same drain contract as above.
    void addAttachedTransition(quint64 sourceId, quint64 targetId, bool machineSelf = false,
                               std::optional<QPointF> seedPillCenter = std::nullopt);

    // Runs outside any FSM session, so it opens the note's editor directly.
    void addNoteAt(QPointF pos);

    // Opens the recorded inline-edit follow-up, if any. Call from Idle only,
    // never from inside a commit hook.
    void drainPendingInlineEdit();

    // Picks the state/transition/empty-canvas menu by hit test; Simulate mode
    // gets only the view verbs.
    void showContextMenu(QPointF scenePos, QPoint globalPos);
    QMenu* buildStateContextMenu(quint64 stateId);            // caller owns; nullptr for an unknown id
    QMenu* buildTransitionContextMenu(quint64 transitionId);  // caller owns; nullptr for an unknown id
    void showQuickAddMenu(quint64 stateId);

    // The action box is derived, never an FSM state. It shows only in Design
    // mode, in Idle, with a selection that has a variant; the check runs on
    // selection change, FSM state change, and mode change.
    void ensureActionBox();
    void updateActionBoxVisibility();
    void onActionBoxVerb(ActionBoxVerb verb);
    // Colors every selected element in one undo batch.
    void applyColorToSelection(ElementColor color);

    // Resets the wire/reconnect payloads, hides the overlay, and clears the
    // snap highlight. Never touches the FSM, so commit hooks may call it.
    void clearWireDragState();

    // One inline edit at a time, Design mode only. Commit sends the rename or
    // event change; Esc cancels.
    void beginInlineEdit(SelectionKind kind, quint64 id);
    // The editor's commit callback: checks a session is live, then commits.
    void finishInlineEdit();
    // Runs inside sessionCommitted(); never touches the FSM. hide() re-fires
    // the editor's commit signal, which the cleared target turns into a no-op.
    void commitInlineEdit();
    // Hides the editor and forgets its target without ending the session.
    void clearInlineEditState();
    void cancelInlineEdit();  // clearInlineEditState() + sessionAborted()

    void rebuildAll(const Machine& machine);
    void clearAll();

    // Event name, "(after N ms)" for a pure delay, blank otherwise.
    void refreshTransitionLabelText(quint64 transitionId);

    // Drags a selected edge's target end through the wire-drag overlay and
    // snap. Dropping on the source makes a self-transition; on empty canvas, cancels.
    void onReconnectDragStarted(quint64 transitionId, QPointF scenePos);
    void onReconnectDragFinished(QPointF scenePos);

    struct SegmentDragState {
        quint64 transitionId = 0;
        int segmentIndex = -1;
        bool isSourceHalf = true;
        bool isHorizontal = false;
        QPointF pressPos;
        QVector<QPointF> initialWaypoints;
        // The skeleton as last drawn (the router may adjust the raw
        // translate); release commits exactly this.
        QVector<QPointF> lastDrawnWaypoints;
    };
    SegmentDragState segmentDrag_;

    // Shared by live move and release so they never disagree. Within the
    // snap radius a segment holds collinear with a parallel sibling, and the
    // commit merges the pair.
    struct SegmentDragResolution {
        qreal delta = 0.0;
        bool snapped = false;
        bool guideVertical = false;  // vertical guide <=> a vertical segment dragged along X
        qreal guideCoord = 0.0;
        QLineF draggedLine;  // the dragged segment at its resolved position
        QLineF partnerLine;  // the sibling the snap aligned against (snapped only)
    };
    SegmentDragResolution resolveSegmentDrag(QPointF scenePos) const;

    void onSegmentDragStarted(quint64 transitionId, int segmentIndex, bool isSourceHalf, bool isHorizontal, QPointF scenePos);
    void onSegmentDragMoved(quint64 transitionId, QPointF scenePos);
    void onSegmentDragFinished(quint64 transitionId, QPointF scenePos);

    // Registry wrappers that add the presenter-side steps: label text and
    // routing after create; animation stop, edit cancel, and cache cleanup
    // before remove.
    StateItem* createStateItem(const State& state);
    void removeStateItem(quint64 id);
    void createTransitionVisual(const Transition& transition);
    void removeTransitionVisual(quint64 id);
    NoteItem* createNoteItem(const Note& note);
    void removeNoteItem(quint64 id);

    // Gesture wiring for a freshly built item, called by itemRegistry_.
    void configureStateItem(StateItem* item);
    void configureTransitionVisual(TransitionItem* edgeItem, TransitionLabelItem* labelItem);
    void configureNoteItem(NoteItem* item);

    // Re-routes the edge to the dragged pill on every move; the document
    // commits once, on release.
    void previewLabelDrag(quint64 transitionId, QPointF pillCenter);
    void onMachineNameChanged(const events::MachineNameChanged& fact);
    // Visible iff the machine has no states and the mode is Design.
    void updateOnboardingHint();
    // Creates the frame on first use and installs its gesture callbacks.
    MachineFrameItem* ensureFrameItem();

    // Safe to call whenever mode, running state, or configuration may have
    // changed; a re-attached view gets the right look immediately. Only a
    // RUNNING simulation dims anything. Active states glow; transitions out
    // of them (or root ones) are fireable (named event) or waiting (delay
    // only); everything else dims. Pure topology reads, no guard evaluation.
    void updateSimPresentation();

    // Pop only when a state is entered via a fired transition; pulse on every
    // firing. A repeat call for the same id restarts the animation.
    void playStatePop(quint64 stateId);
    void pulseTransition(quint64 transitionId);

    // On mode change, reset, and clearAll(); deliberately not on pause.
    void stopAllSimAnimations();

    // The FSM's commit hooks. Each runs inside sessionCommitted() and must
    // not call an fsm_ method.
    CanvasInteractionFsm::Hooks makeInteractionHooks();
    void commitNodeMove();
    void commitNoteMove();
    void commitPillOffset();
    void commitFrameMove();
    void commitWire();
    void commitReconnect();
    void commitBandSelection();

    QGraphicsScene* scene_;
    CanvasView* view_;
    std::shared_ptr<MachineDocAgent> doc_;
    // Read-only: only running() and mode(). The active configuration comes
    // from simPresentation_'s cache, fed by ConfigurationChanged.
    std::shared_ptr<SimulationAgent> sim_;

    SimPresentation simPresentation_;

    ItemRegistryDelegate itemRegistry_;
    // The delegates below hold a reference to itemRegistry_, so they must be
    // declared after it.
    RoutingDelegate routing_;
    SelectionDelegate selection_;
    DragSessionDelegate dragSession_;
    WireDragDelegate wireDrag_;

    // Scene-owned items below are created lazily, hidden rather than
    // deleted, and nulled by clearAll() after scene_->clear() deletes them.
    MachineFrameItem* frameItem_ = nullptr;
    ActionBoxItem* actionBox_ = nullptr;
    CodeLensItem* codeLens_ = nullptr;
    QGraphicsSimpleTextItem* onboardingHint_ = nullptr;

    // Local mirror of the mode fact so edit verbs can no-op immediately. The
    // FSM keeps its own `designMode` mirror for its entry guard.
    events::Mode currentMode_ = events::Mode::Design;

    // One per presenter, so each split pane has its own sessions.
    CanvasInteractionFsm fsm_;

    // Recorded by a commit hook, opened by drainPendingInlineEdit() from Idle.
    Selection pendingInlineEditTarget_;

    // The edit target lives in fsm_.inlineEdit().
    InlineEditController inlineEdit_;

    // Empty when no variable highlight is active.
    QString activeHighlightedVariable_;
};

}  // namespace app
