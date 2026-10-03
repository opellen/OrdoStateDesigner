// CanvasPresenter core: construction, fact handlers, and scene lifecycle.
// Gestures are in canvas_presenter_gestures.cpp, verbs in canvas_presenter_verbs.cpp.

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
#include <QLineF>
#include <QMenu>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QSet>
#include <QVector>

#include "constants/design_tokens.h"
#include "infra/expression.h"
#include "model/undo_events.h"
#include "view/canvas/canvas_view.h"
#include "view/items/machine_frame_item.h"
#include "view/items/note_item.h"
#include "view/items/state_item.h"
#include "view/items/transition_item.h"

namespace app {

namespace {

using InteractionState = CanvasInteractionFsm::State;

}  // namespace

CanvasPresenter::CanvasPresenter(QGraphicsScene* scene, CanvasView* view)
    : Presenter(QStringLiteral("CanvasPresenter"), scene),
      scene_(scene),
      view_(view),
      simPresentation_(this),
      itemRegistry_(
          scene_, [this](StateItem* item) { configureStateItem(item); },
          [this](TransitionItem* edgeItem, TransitionLabelItem* labelItem) {
              configureTransitionVisual(edgeItem, labelItem);
          },
          [this](NoteItem* item) { configureNoteItem(item); }),
      routing_(
          scene_, itemRegistry_, [this]() -> const Machine* { return doc_ != nullptr ? &doc_->machine() : nullptr; },
          [this]() -> MachineFrameItem* { return frameItem_; }, [this] { return ensureFrameItem(); },
          [this] {
              updateOnboardingHint();
              // Re-anchor the action box when the selected item's rect changes.
              updateActionBoxVisibility();
          }),
      selection_(
          scene_, itemRegistry_, [this]() -> const Machine* { return doc_ != nullptr ? &doc_->machine() : nullptr; },
          [this]() -> MachineFrameItem* { return frameItem_; }),
      dragSession_(scene_, itemRegistry_,
                   [this]() -> const Machine* { return doc_ != nullptr ? &doc_->machine() : nullptr; }),
      wireDrag_(scene_, itemRegistry_),
      fsm_(makeInteractionHooks()),
      inlineEdit_(scene) {
    // Any session start or end re-derives the action box's visibility.
    fsm_.setStateObserver([this](InteractionState, InteractionState) { updateActionBoxVisibility(); });

    inlineEdit_.onCommitRequested = [this] { finishInlineEdit(); };
    inlineEdit_.onAbortRequested = [this] { cancelInlineEdit(); };
    inlineEdit_.onUndoRedoKey = [this](bool redo) {
        cancelInlineEdit();
        if (redo) {
            context().send(events::RedoRequested{});
        } else {
            context().send(events::UndoRequested{});
        }
    };
}

// Called from the ctor's initializer list -- it only forms lambdas over
// `this`, touching no member, so it is safe there.
CanvasInteractionFsm::Hooks CanvasPresenter::makeInteractionHooks() {
    CanvasInteractionFsm::Hooks hooks;
    hooks.commitNodeMove = [this] { commitNodeMove(); };
    hooks.commitNoteMove = [this] { commitNoteMove(); };
    hooks.commitPillOffset = [this] { commitPillOffset(); };
    hooks.commitFrameMove = [this] { commitFrameMove(); };
    hooks.commitWire = [this] { commitWire(); };
    hooks.commitReconnect = [this] { commitReconnect(); };
    hooks.commitInlineEdit = [this] { commitInlineEdit(); };
    hooks.commitBandSelection = [this] { commitBandSelection(); };
    return hooks;
}

void CanvasPresenter::onRegister() {
    doc_ = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    sim_ = context().agentAs<SimulationAgent>(SimulationAgent::kName);
    // Seed both mode mirrors before the snapshot builds any item: this view
    // may attach while the kernel is already simulating.
    currentMode_ = sim_ != nullptr ? sim_->mode() : events::Mode::Design;
    fsm_.modeChanged(currentMode_ == events::Mode::Design);

    subscribe<events::StateAdded>(&CanvasPresenter::onStateAdded);
    subscribe<events::StateRenamed>(&CanvasPresenter::onStateRenamed);
    subscribe<events::StateKindChanged>(&CanvasPresenter::onStateKindChanged);
    subscribe<events::InitialStateChanged>(&CanvasPresenter::onInitialStateChanged);
    subscribe<events::StateMoved>(&CanvasPresenter::onStateMoved);
    subscribe<events::EntryActionsChanged>(&CanvasPresenter::onEntryActionsChanged);
    subscribe<events::ExitActionsChanged>(&CanvasPresenter::onExitActionsChanged);
    subscribe<events::TransitionAdded>(&CanvasPresenter::onTransitionAdded);
    subscribe<events::TransitionEventChanged>(&CanvasPresenter::onTransitionEventChanged);
    subscribe<events::TransitionGuardChanged>(&CanvasPresenter::onTransitionGuardChanged);
    subscribe<events::TransitionActionChanged>(&CanvasPresenter::onTransitionActionChanged);
    subscribe<events::TransitionDelayChanged>(&CanvasPresenter::onTransitionDelayChanged);
    subscribe<events::TransitionAlwaysChanged>(&CanvasPresenter::onTransitionAlwaysChanged);
    subscribe<events::TransitionRetargeted>(&CanvasPresenter::onTransitionRetargeted);
    subscribe<events::TransitionLabelMoved>(&CanvasPresenter::onTransitionLabelMoved);
    subscribe<events::TransitionLabelRatioChanged>(&CanvasPresenter::onTransitionLabelRatioChanged);
    subscribe<events::TransitionBendpointsChanged>(&CanvasPresenter::onTransitionBendpointsChanged);
    subscribe<events::StateDeleted>(&CanvasPresenter::onStateDeleted);
    subscribe<events::TransitionDeleted>(&CanvasPresenter::onTransitionDeleted);
    subscribe<events::StateReparented>(&CanvasPresenter::onStateReparented);
    subscribe<events::InitialChildChanged>(&CanvasPresenter::onInitialChildChanged);
    subscribe<events::NoteAdded>(&CanvasPresenter::onNoteAdded);
    subscribe<events::NoteMoved>(&CanvasPresenter::onNoteMoved);
    subscribe<events::NoteTextChanged>(&CanvasPresenter::onNoteTextChanged);
    subscribe<events::NoteDeleted>(&CanvasPresenter::onNoteDeleted);
    subscribe<events::StateColorChanged>(&CanvasPresenter::onStateColorChanged);
    subscribe<events::TransitionColorChanged>(&CanvasPresenter::onTransitionColorChanged);
    subscribe<events::NoteColorChanged>(&CanvasPresenter::onNoteColorChanged);
    subscribe<events::MachineNameChanged>(&CanvasPresenter::onMachineNameChanged);
    subscribe<events::MachineSnapshotPublished>(&CanvasPresenter::onSnapshotPublished);
    subscribe<events::ModeChanged>(&CanvasPresenter::onModeChanged);

    // GuardResultChanged is deliberately not subscribed.
    subscribe<events::SimulationStarted>(&CanvasPresenter::onSimulationStarted);
    subscribe<events::SimulationPaused>(&CanvasPresenter::onSimulationPaused);
    subscribe<events::SimulationReset>(&CanvasPresenter::onSimulationReset);
    subscribe<events::ActiveStateChanged>(&CanvasPresenter::onActiveStateChanged);
    // The glow reads this fact, not ActiveStateChanged.
    subscribe<events::ConfigurationChanged>(&CanvasPresenter::onConfigurationChanged);
    subscribe<events::TransitionFired>(&CanvasPresenter::onTransitionFired);
    subscribe<events::BreakpointToggled>(&CanvasPresenter::onBreakpointToggled);
    subscribe<events::BreakpointHit>(&CanvasPresenter::onBreakpointHit);

    // `this` as context: Qt disconnects these when the presenter dies.
    connect(view_, &CanvasView::emptyCanvasDoubleClicked, this,
            [this](QPointF scenePos) { onCanvasDoubleClicked(scenePos); });
    connect(view_, &CanvasView::deleteRequested, this, [this] { onDeleteRequested(); });
    connect(view_, &CanvasView::escapePressed, this, [this] { onEscapePressed(); });
    connect(view_, &CanvasView::spacePressed, this, [this] { toggleCodeLens(); });
    // Never mid-gesture: re-zooming would move the scene under the drag.
    connect(view_, &CanvasView::zoomToSelectionRequested, this, [this] {
        if (fsm_.state() == InteractionState::Idle) {
            zoomToSelection();
        }
    });
    connect(view_, &CanvasView::contextMenuRequested, this,
            [this](QPointF scenePos, QPoint globalPos) { showContextMenu(scenePos, globalPos); });
    connect(view_, &CanvasView::rubberBandStarted, this, [this] { onRubberBandStarted(); });
    connect(view_, &CanvasView::rubberBandFinished, this, [this] { onRubberBandFinished(); });

    connect(scene_, &QGraphicsScene::selectionChanged, this, [this] { emitSelectionChanged(); });

    // Repopulate from whatever the document already holds.
    context().send(events::MachineSnapshotRequested{});
}

CanvasPresenter::Selection CanvasPresenter::currentSelection() const {
    return selection_.derive();
}

void CanvasPresenter::selectState(quint64 id) {
    StateItem* item = itemRegistry_.stateItems().value(id, nullptr);
    if (item == nullptr) {
        return;
    }
    scene_->clearSelection();
    item->setSelected(true);
}

void CanvasPresenter::selectTransition(quint64 id) {
    TransitionItem* item = itemRegistry_.transitionItems().value(id, nullptr);
    if (item == nullptr) {
        return;
    }
    scene_->clearSelection();
    item->setSelected(true);
}

QRectF CanvasPresenter::elementsRect(const QList<quint64>& stateIds, const QList<quint64>& transitionIds) const {
    QRectF rect;
    for (quint64 id : stateIds) {
        if (const StateItem* state = itemRegistry_.stateItems().value(id, nullptr)) {
            rect |= state->sceneBoundingRect();
        }
    }
    for (quint64 id : transitionIds) {
        if (const TransitionItem* edge = itemRegistry_.transitionItems().value(id, nullptr)) {
            rect |= edge->sceneBoundingRect();
        }
        if (const TransitionLabelItem* pill = itemRegistry_.transitionLabels().value(id, nullptr)) {
            rect |= pill->sceneBoundingRect();
        }
    }
    return rect;
}

void CanvasPresenter::zoomToElements(const QList<quint64>& stateIds, const QList<quint64>& transitionIds) {
    const QRectF target = elementsRect(stateIds, transitionIds);
    if (target.isEmpty()) {
        zoomToFit();
        return;
    }
    view_->centerAndFit(target);
}

void CanvasPresenter::zoomToSelection() {
    QList<quint64> stateIds;
    QList<quint64> transitionIds;
    // Notes and the frame join by rect. The frame's rect is the machine's
    // extent, unlike itemsBoundingRect(), which also counts items outside it.
    QRectF extra;
    for (QGraphicsItem* item : scene_->selectedItems()) {
        switch (item->type()) {
            case StateItem::Type:
                stateIds.push_back(static_cast<StateItem*>(item)->id());
                break;
            case TransitionItem::Type:
                transitionIds.push_back(static_cast<TransitionItem*>(item)->id());
                break;
            case NoteItem::Type:
            case MachineFrameItem::Type:
                extra |= item->sceneBoundingRect();
                break;
            default:
                break;
        }
    }
    const QRectF target = elementsRect(stateIds, transitionIds) | extra;
    if (target.isEmpty()) {
        zoomToFit();
        return;
    }
    view_->centerAndFit(target);
}

void CanvasPresenter::zoomToFit() { view_->centerAndFit(scene_->itemsBoundingRect()); }

void CanvasPresenter::revealElements(const QList<quint64>& stateIds, const QList<quint64>& transitionIds) {
    view_->revealRect(elementsRect(stateIds, transitionIds));
}

void CanvasPresenter::highlightTransitionsForVariable(const QString& varName) {
    if (varName.isEmpty()) {
        clearVariableHighlight();
        return;
    }
    activeHighlightedVariable_ = varName;

    QSet<quint64> matchingTransitions;
    if (doc_ != nullptr) {
        for (const Transition& t : doc_->machine().transitions) {
            if (expr::textReferencesIdentifier(t.guard, varName) ||
                expr::textReferencesIdentifier(t.action, varName)) {
                matchingTransitions.insert(t.id);
            }
        }
    }

    constexpr qreal kDimmedOpacity = 0.25;

    // Transitions and their label pills
    for (auto it = itemRegistry_.transitionItems().begin(); it != itemRegistry_.transitionItems().end(); ++it) {
        const quint64 tid = it.key();
        TransitionItem* edge = it.value();
        TransitionLabelItem* label = itemRegistry_.transitionLabels().value(tid, nullptr);
        const bool match = matchingTransitions.contains(tid);

        if (edge != nullptr) {
            edge->setHighlighted(match);
            edge->setOpacity(match ? 1.0 : kDimmedOpacity);
        }
        if (label != nullptr) {
            label->setHighlighted(match);
            label->setOpacity(match ? 1.0 : kDimmedOpacity);
        }
    }

    // Dim states
    for (auto it = itemRegistry_.stateItems().begin(); it != itemRegistry_.stateItems().end(); ++it) {
        StateItem* state = it.value();
        if (state != nullptr) {
            state->setOpacity(kDimmedOpacity);
        }
    }

    emit variableHighlightChanged(activeHighlightedVariable_);
}

void CanvasPresenter::clearVariableHighlight() {
    if (activeHighlightedVariable_.isEmpty()) {
        return;
    }
    activeHighlightedVariable_.clear();

    for (auto it = itemRegistry_.transitionItems().begin(); it != itemRegistry_.transitionItems().end(); ++it) {
        TransitionItem* edge = it.value();
        if (edge != nullptr) {
            edge->setHighlighted(false);
            edge->setOpacity(1.0);
        }
    }
    for (auto it = itemRegistry_.transitionLabels().begin(); it != itemRegistry_.transitionLabels().end(); ++it) {
        TransitionLabelItem* label = it.value();
        if (label != nullptr) {
            label->setHighlighted(false);
            label->setOpacity(1.0);
        }
    }
    for (auto it = itemRegistry_.stateItems().begin(); it != itemRegistry_.stateItems().end(); ++it) {
        StateItem* state = it.value();
        if (state != nullptr) {
            state->setOpacity(1.0);
        }
    }

    if (currentMode_ == events::Mode::Simulate) {
        updateSimPresentation();
    }

    emit variableHighlightChanged(QString());
}

bool CanvasPresenter::debugIsTransitionHighlighted(quint64 id) const {
    TransitionItem* edge = itemRegistry_.transitionItems().value(id, nullptr);
    return edge != nullptr && edge->isHighlighted();
}

bool CanvasPresenter::debugIsTransitionDimmed(quint64 id) const {
    TransitionItem* edge = itemRegistry_.transitionItems().value(id, nullptr);
    return edge != nullptr && edge->opacity() < 0.9;
}

bool CanvasPresenter::debugIsStateDimmed(quint64 id) const {
    StateItem* state = itemRegistry_.stateItems().value(id, nullptr);
    return state != nullptr && state->opacity() < 0.9;
}

bool CanvasPresenter::debugIsTransitionAlways(quint64 id) const {
    const TransitionLabelItem* label = itemRegistry_.transitionLabels().value(id, nullptr);
    return label != nullptr && label->isAlways();
}

void CanvasPresenter::emitSelectionChanged() {
    hideCodeLens();
    if (!selection_.refreshLast()) {
        return;
    }
    const Selection selection = selection_.last();
    if (selection.kind == SelectionKind::None && !activeHighlightedVariable_.isEmpty()) {
        clearVariableHighlight();
    }
    emit selectionChanged(selection.kind, selection.id);
    updateActionBoxVisibility();
}

void CanvasPresenter::onStateAdded(const events::StateAdded& fact) {
    createStateItem(fact.state);
    // The new state may be a child of an existing compound. Refresh
    // containers before rerouteTouching, whose frame refresh fits against them.
    routing_.refreshContainers();
    // Currently a no-op (transitions are always created after their states),
    // kept so this does not depend on that ordering.
    routing_.rerouteTouching(fact.state.id);
    // Undo can re-create the initial state; the marker needs its item.
    if (doc_ != nullptr && fact.state.id == doc_->machine().initialStateId) {
        routing_.refreshInitialMarker();
    }
}

void CanvasPresenter::onStateRenamed(const events::StateRenamed& fact) {
    StateItem* item = itemRegistry_.stateItems().value(fact.id, nullptr);
    if (item == nullptr) {
        return;
    }
    item->setName(fact.name);
    // The name sizes a leaf box and a container's minimum header width.
    routing_.refreshContainers();
    routing_.rerouteTouching(fact.id);
}

void CanvasPresenter::onStateKindChanged(const events::StateKindChanged& fact) {
    StateItem* item = itemRegistry_.stateItems().value(fact.id, nullptr);
    if (item == nullptr) {
        return;
    }
    item->setKind(fact.kind);
    routing_.rerouteTouching(fact.id);
}

void CanvasPresenter::onInitialStateChanged(const events::InitialStateChanged&) {
    routing_.refreshInitialMarker();
}

void CanvasPresenter::onStateMoved(const events::StateMoved& fact) {
    StateItem* item = itemRegistry_.stateItems().value(fact.id, nullptr);
    if (item == nullptr) {
        return;
    }
    // After a drag this echo equals the item's snapped position, so setPos()
    // is a no-op and cannot loop; it matters for undo/redo.
    item->setPos(fact.pos);
    // Before rerouteTouching: an ancestor's hull is now stale.
    routing_.refreshContainers();
    routing_.rerouteTouching(fact.id);
    routing_.refreshInitialMarker();  // the marker's own position tracks its state's box
}

void CanvasPresenter::onEntryActionsChanged(const events::EntryActionsChanged& fact) {
    StateItem* item = itemRegistry_.stateItems().value(fact.stateId, nullptr);
    if (item == nullptr) {
        return;
    }
    item->setEntryActions(fact.entryActions);
    // Entry lines size a leaf box and a container's header.
    routing_.refreshContainers();
    routing_.rerouteTouching(fact.stateId);
}

void CanvasPresenter::onExitActionsChanged(const events::ExitActionsChanged& fact) {
    StateItem* item = itemRegistry_.stateItems().value(fact.stateId, nullptr);
    if (item == nullptr) {
        return;
    }
    item->setExitActions(fact.exitActions);
    // Exit lines only show in a container's header.
    routing_.refreshContainers();
    routing_.rerouteTouching(fact.stateId);
}

void CanvasPresenter::onTransitionAdded(const events::TransitionAdded& fact) { createTransitionVisual(fact.transition); }

void CanvasPresenter::onTransitionEventChanged(const events::TransitionEventChanged& fact) {
    refreshTransitionLabelText(fact.id);
    // Label text resizes the pill, and the pill's rect is a routing input.
    routing_.rerouteTransition(fact.id);
}

void CanvasPresenter::onTransitionGuardChanged(const events::TransitionGuardChanged& fact) {
    refreshTransitionLabelText(fact.id);
    routing_.rerouteTransition(fact.id);
}

void CanvasPresenter::onTransitionActionChanged(const events::TransitionActionChanged& fact) {
    refreshTransitionLabelText(fact.id);
    routing_.rerouteTransition(fact.id);
}

void CanvasPresenter::onTransitionDelayChanged(const events::TransitionDelayChanged& fact) {
    refreshTransitionLabelText(fact.id);
    routing_.rerouteTransition(fact.id);
}

void CanvasPresenter::onTransitionAlwaysChanged(const events::TransitionAlwaysChanged& fact) {
    refreshTransitionLabelText(fact.id);
    routing_.rerouteTransition(fact.id);
}

void CanvasPresenter::onTransitionLabelMoved(const events::TransitionLabelMoved& fact) {
    routing_.rerouteTransition(fact.id);
    // After the reroute: the frame fits against the pill's current rect.
    routing_.refreshFrame();
}

void CanvasPresenter::onTransitionLabelRatioChanged(const events::TransitionLabelRatioChanged& fact) {
    routing_.rerouteTransition(fact.id);
    routing_.refreshFrame();
}

void CanvasPresenter::onTransitionBendpointsChanged(const events::TransitionBendpointsChanged& fact) {
    routing_.rerouteTransition(fact.id);
    // Bendpoints are frame content.
    routing_.refreshFrame();
}

// Display only; the fire click and the inline editor use the raw event.
void CanvasPresenter::refreshTransitionLabelText(quint64 transitionId) {
    TransitionLabelItem* label = itemRegistry_.transitionLabels().value(transitionId, nullptr);
    const Transition* transition = doc_ != nullptr ? doc_->findTransition(transitionId) : nullptr;
    if (label == nullptr || transition == nullptr) {
        return;
    }
    label->setLabelData(transitionPillEventText(*transition), transition->guard, transition->action,
                        transition->isAlways());
}

void CanvasPresenter::onTransitionRetargeted(const events::TransitionRetargeted& fact) {
    // Known gap: siblings at the OLD endpoint do not re-compact, since the
    // fact carries no before-image.
    routing_.rerouteTransition(fact.id);
}

void CanvasPresenter::onStateDeleted(const events::StateDeleted& fact) {
    // Cascaded transitions already arrived as TransitionDeleted facts.
    removeStateItem(fact.id);
    // Before refreshFrame(): the parent may have stopped being a container.
    routing_.refreshContainers();
    routing_.refreshFrame();
    routing_.refreshInitialMarker();
}

void CanvasPresenter::onStateReparented(const events::StateReparented& fact) {
    // Leaf/container mode may have flipped for the state and both parents.
    routing_.refreshContainers();
    routing_.rerouteTouching(fact.id);
    routing_.refreshInitialMarker();
}

void CanvasPresenter::onInitialChildChanged(const events::InitialChildChanged&) {
    // Only the per-compound initial marker changes; refreshContainers() rebuilds it.
    routing_.refreshContainers();
}

void CanvasPresenter::onTransitionDeleted(const events::TransitionDeleted& fact) { removeTransitionVisual(fact.id); }

void CanvasPresenter::onNoteAdded(const events::NoteAdded& fact) { createNoteItem(fact.note); }

void CanvasPresenter::onNoteMoved(const events::NoteMoved& fact) {
    NoteItem* item = itemRegistry_.noteItems().value(fact.id, nullptr);
    if (item == nullptr) {
        return;
    }
    // A no-op echo after a drag (see onStateMoved); matters for undo/redo.
    item->setPos(fact.pos);
}

void CanvasPresenter::onNoteTextChanged(const events::NoteTextChanged& fact) {
    NoteItem* item = itemRegistry_.noteItems().value(fact.id, nullptr);
    if (item == nullptr) {
        return;
    }
    item->setText(fact.text);
}

void CanvasPresenter::onNoteDeleted(const events::NoteDeleted& fact) { removeNoteItem(fact.id); }

// Color is paint-only: no reroute or refresh.
void CanvasPresenter::onStateColorChanged(const events::StateColorChanged& fact) {
    if (StateItem* item = itemRegistry_.stateItems().value(fact.id, nullptr)) {
        item->setColor(fact.color);
    }
}

void CanvasPresenter::onTransitionColorChanged(const events::TransitionColorChanged& fact) {
    if (TransitionItem* item = itemRegistry_.transitionItems().value(fact.id, nullptr)) {
        item->setColor(fact.color);
    }
}

void CanvasPresenter::onNoteColorChanged(const events::NoteColorChanged& fact) {
    if (NoteItem* item = itemRegistry_.noteItems().value(fact.id, nullptr)) {
        item->setColor(fact.color);
    }
}

void CanvasPresenter::onSnapshotPublished(const events::MachineSnapshotPublished& fact) { rebuildAll(fact.machine); }

void CanvasPresenter::onModeChanged(const events::ModeChanged& fact) {
    currentMode_ = fact.mode;
    const bool design = currentMode_ == events::Mode::Design;
    updateOnboardingHint();

    // Edits are rejected while simulating anyway; this stops the canvas
    // from offering them. Design restores every flag below.
    if (!design) {
        scene_->clearSelection();
        clearWireDragState();
        clearInlineEditState();
        if (fsm_.state() == InteractionState::FrameDrag) {
            dragSession_.cancelFramePreview();  // nothing was sent yet, so restore the preview
        } else if (fsm_.state() == InteractionState::NodeDrag) {
            dragSession_.cancelSubtreeDrag();  // a no-op for a plain leaf drag
            clearNodeDropHighlight();
            dragSession_.clearAlignmentGuides();
            dragSession_.clearSessionState();
        }
        // Simulate leaves no session behind.
        fsm_.sessionAborted();
    }
    // After the abort, so the cleanup above still sees the session it ends.
    fsm_.modeChanged(design);
    for (auto it = itemRegistry_.stateItems().begin(); it != itemRegistry_.stateItems().end(); ++it) {
        it.value()->setFlag(QGraphicsItem::ItemIsMovable, design);
        it.value()->setFlag(QGraphicsItem::ItemIsSelectable, design);
    }
    for (auto it = itemRegistry_.transitionItems().begin(); it != itemRegistry_.transitionItems().end(); ++it) {
        it.value()->setFlag(QGraphicsItem::ItemIsSelectable, design);
    }
    for (auto it = itemRegistry_.transitionLabels().begin(); it != itemRegistry_.transitionLabels().end(); ++it) {
        it.value()->setInteractive(design);
    }
    for (auto it = itemRegistry_.noteItems().begin(); it != itemRegistry_.noteItems().end(); ++it) {
        it.value()->setFlag(QGraphicsItem::ItemIsMovable, design);
        it.value()->setFlag(QGraphicsItem::ItemIsSelectable, design);
    }
    if (frameItem_ != nullptr) {
        frameItem_->setFlag(QGraphicsItem::ItemIsSelectable, design);
    }

    // A mode change is a clean animation boundary in both directions.
    stopAllSimAnimations();
    updateSimPresentation();

    // clearSelection() above only re-derives the box if something was selected.
    updateActionBoxVisibility();
}

void CanvasPresenter::onSimulationStarted(const events::SimulationStarted&) {
    for (StateItem* item : itemRegistry_.stateItems()) {
        if (item != nullptr && item->isBreakpointHit()) {
            item->setBreakpointHit(false);
        }
    }
    updateSimPresentation();
}

void CanvasPresenter::onSimulationPaused(const events::SimulationPaused&) { updateSimPresentation(); }

void CanvasPresenter::onSimulationReset(const events::SimulationReset&) {
    // Animations from firings before the reset are stale.
    stopAllSimAnimations();
    for (StateItem* item : itemRegistry_.stateItems()) {
        if (item != nullptr && item->isBreakpointHit()) {
            item->setBreakpointHit(false);
        }
    }
    updateSimPresentation();
}

void CanvasPresenter::onActiveStateChanged(const events::ActiveStateChanged& fact) {
    // Presentation follows ConfigurationChanged, which arrives once per
    // macrostep; this fact fires per microstep. Only pop on a real firing,
    // not on Run/Reset's jump to the initial state.
    if (fact.viaTransitionId != 0 && fact.toId != 0) {
        playStatePop(fact.toId);
    }
}

void CanvasPresenter::onConfigurationChanged(const events::ConfigurationChanged& fact) {
    simPresentation_.setConfiguration(fact.activeIds);
    updateSimPresentation();
}

void CanvasPresenter::onTransitionFired(const events::TransitionFired& fact) { pulseTransition(fact.transitionId); }

void CanvasPresenter::onBreakpointToggled(const events::BreakpointToggled& fact) {
    if (StateItem* item = itemRegistry_.stateItems().value(fact.stateId, nullptr)) {
        item->setBreakpoint(fact.enabled);
        if (!fact.enabled && item->isBreakpointHit()) {
            item->setBreakpointHit(false);
        }
    }
}

void CanvasPresenter::onBreakpointHit(const events::BreakpointHit& fact) {
    if (StateItem* item = itemRegistry_.stateItems().value(fact.stateId, nullptr)) {
        item->setBreakpointHit(true);
    }
}

bool CanvasPresenter::debugHasBreakpoint(quint64 stateId) const {
    if (const StateItem* item = itemRegistry_.stateItems().value(stateId, nullptr)) {
        return item->hasBreakpoint();
    }
    return false;
}

bool CanvasPresenter::debugIsBreakpointHit(quint64 stateId) const {
    if (const StateItem* item = itemRegistry_.stateItems().value(stateId, nullptr)) {
        return item->isBreakpointHit();
    }
    return false;
}

void CanvasPresenter::rebuildAll(const Machine& machine) {
    clearAll();
    for (const State& state : machine.states) {
        createStateItem(state);
    }
    // Containers before the frame's first fit, which unions every state box.
    routing_.refreshContainers();
    // Root transitions anchor to the frame, so it must exist first.
    routing_.refreshFrame();
    for (const Transition& transition : machine.transitions) {
        createTransitionVisual(transition);
    }
    // Refit now that pills exist; an offset pill may stick out.
    routing_.refreshContainers();
    routing_.refreshFrame();
    for (const Note& note : machine.notes) {
        createNoteItem(note);
    }
    routing_.refreshInitialMarker();
    // A scene rebuilt mid-simulation must show the live look immediately.
    updateSimPresentation();
}

void CanvasPresenter::clearAll() {
    // Stop animations before their targets are deleted.
    stopAllSimAnimations();
    scene_->clear();  // deletes every item the scene owns
    // Every owner below drops pointers to items scene_->clear() just deleted.
    itemRegistry_.clearAllItems();
    routing_.forgetSceneItems();
    wireDrag_.forgetSceneItems();
    dragSession_.forgetSceneItems();
    frameItem_ = nullptr;
    actionBox_ = nullptr;
    codeLens_ = nullptr;
    onboardingHint_ = nullptr;
    inlineEdit_.forgetSceneOwnedWidgets();
    // Session payloads point at deleted items; reset them and end any session.
    fsm_.wireDrag() = CanvasInteractionFsm::WireDrag{};
    fsm_.reconnectDrag() = CanvasInteractionFsm::ReconnectDrag{};
    fsm_.nodeDrag() = CanvasInteractionFsm::NodeDrag{};
    fsm_.noteDrag() = CanvasInteractionFsm::NoteDrag{};
    fsm_.pillDrag() = CanvasInteractionFsm::PillDrag{};
    fsm_.frameDrag() = CanvasInteractionFsm::FrameDrag{};
    fsm_.inlineEdit() = CanvasInteractionFsm::InlineEdit{};
    pendingInlineEditTarget_ = Selection{};
    if (fsm_.state() != InteractionState::Idle) {
        fsm_.sessionAborted();
    }
    // scene_->clear() may not have fired Qt's selectionChanged().
    emitSelectionChanged();
}

StateItem* CanvasPresenter::createStateItem(const State& state) {
    return itemRegistry_.createStateItem(state, currentMode_ == events::Mode::Design);
}

void CanvasPresenter::configureStateItem(StateItem* item) {
    const quint64 id = item->id();
    item->setOnMoved([this, id] {
        // A move caused by refreshContainers() is derived geometry, not a
        // gesture: treating it as one re-enters the refresh and blows up per
        // nesting level (stack overflow or a hang). Only wires follow it.
        if (routing_.refreshingContainers()) {
            routing_.rerouteTouching(id);
            return;
        }
        // Move descendants first, or the hull would be derived from children
        // that have not moved yet and snap the drag back.
        if (dragSession_.subtreeRootId() == id) {
            dragSession_.applySubtreeDragDelta();
        }
        // Only for the dragged root; descendants must not overwrite its verdict.
        if (fsm_.state() == InteractionState::NodeDrag && fsm_.nodeDrag().stateId == id) {
            updateDropCandidate();
        }
        routing_.rerouteTouching(id);
        // Ancestor hulls stretch live; no kernel traffic until release.
        routing_.refreshContainers();
        routing_.refreshInitialMarker();
    });
    // Installed on every item; resolveAlignmentAdjuster() gates itself.
    item->setPositionAdjuster([this, id](QPointF proposed) { return resolveAlignmentAdjuster(id, proposed); });
    item->setOnDragStarted([this, id] { onNodeDragStarted(id); });
    item->setOnDragFinished([this, id](QPointF pos) { onNodeDragFinished(id, pos); });
    item->setOnDragReverted([this, id] { onNodeDragReverted(id); });
    item->setOnWireDragStarted([this](quint64 stateId, PortSide side) { onWireDragStarted(stateId, side); });
    item->setOnWireDragMoved([this](QPointF scenePos) { onWireDragMoved(scenePos); });
    item->setOnWireDragFinished([this](QPointF scenePos) { onWireDragFinished(scenePos); });
    item->setOnRenameRequested([this, id] { beginInlineEdit(SelectionKind::State, id); });
    item->setOnQuickAddClicked([this](quint64 stateId) { showQuickAddMenu(stateId); });
    item->setBreakpoint(sim_ != nullptr && sim_->hasBreakpoint(id));
    item->setOnBreakpointToggled([this](quint64 stateId) {
        context().send(events::ToggleBreakpointRequested{.stateId = stateId});
    });
}

void CanvasPresenter::removeStateItem(quint64 id) {
    simPresentation_.stopStatePop(id);
    if (fsm_.inlineEdit().kind == SelectionKind::State && fsm_.inlineEdit().id == id) {
        cancelInlineEdit();  // the editor's target is going away
    }
    if (itemRegistry_.removeStateItem(id)) {
        if (fsm_.wireDrag().snapTargetId == id) {
            // Its drop-highlight dies with the item; nothing left to un-highlight.
            fsm_.wireDrag().snapTargetId = 0;
        }
    }
    emitSelectionChanged();
}

void CanvasPresenter::createTransitionVisual(const Transition& transition) {
    itemRegistry_.createTransitionVisual(transition, currentMode_ == events::Mode::Design);
    refreshTransitionLabelText(transition.id);
    routing_.rerouteTransition(transition.id);
}

void CanvasPresenter::configureTransitionVisual(TransitionItem* edgeItem, TransitionLabelItem* labelItem) {
    // Click-to-fire reads the event by id at click time, never a captured copy.
    const quint64 transitionId = edgeItem->id();
    labelItem->setOnFireClicked([this, transitionId] {
        if (doc_ == nullptr) {
            return;
        }
        const Transition* live = doc_->findTransition(transitionId);
        if (live == nullptr) {
            return;
        }
        context().send(events::SendEventRequested{.name = live->event});
    });
    labelItem->setOnEditRequested([this, transitionId] {
        beginInlineEdit(SelectionKind::Transition, transitionId);
    });
    labelItem->setOnDragFinished(
        [this, transitionId](QPointF centerScenePos) { onLabelDragFinished(transitionId, centerScenePos); });
    labelItem->setOnDragMoved(
        [this, transitionId](QPointF centerScenePos) { onLabelDragMoved(transitionId, centerScenePos); });
    edgeItem->setOnReconnectDrag(
        [this, transitionId](QPointF scenePos) { onReconnectDragStarted(transitionId, scenePos); },
        [this](QPointF scenePos) { onWireDragMoved(scenePos); },
        [this](QPointF scenePos) { onReconnectDragFinished(scenePos); });
    edgeItem->setOnSegmentDrag(
        [this, transitionId](int segmentIndex, bool isSourceHalf, bool isHorizontal, QPointF scenePos) {
            onSegmentDragStarted(transitionId, segmentIndex, isSourceHalf, isHorizontal, scenePos);
        },
        [this, transitionId](QPointF scenePos) {
            onSegmentDragMoved(transitionId, scenePos);
        },
        [this, transitionId](QPointF scenePos) {
            onSegmentDragFinished(transitionId, scenePos);
        });
}

void CanvasPresenter::removeTransitionVisual(quint64 id) {
    simPresentation_.stopTransitionPulse(id);
    if (fsm_.inlineEdit().kind == SelectionKind::Transition && fsm_.inlineEdit().id == id) {
        cancelInlineEdit();  // the editor's target is going away
    }
    itemRegistry_.removeTransitionVisual(id);
    routing_.forgetLabelBase(id);
    emitSelectionChanged();
}

NoteItem* CanvasPresenter::createNoteItem(const Note& note) {
    return itemRegistry_.createNoteItem(note, currentMode_ == events::Mode::Design);
}

void CanvasPresenter::configureNoteItem(NoteItem* item) {
    const quint64 id = item->id();
    item->setOnDragStarted([this, id] { onNoteDragStarted(id); });
    item->setOnDragFinished([this, id](QPointF pos) { onNoteDragFinished(id, pos); });
    item->setOnDragReverted([this, id] { onNoteDragReverted(id); });
    item->setOnEditRequested([this, id] { beginInlineEdit(SelectionKind::Note, id); });
}

void CanvasPresenter::removeNoteItem(quint64 id) {
    if (fsm_.inlineEdit().kind == SelectionKind::Note && fsm_.inlineEdit().id == id) {
        cancelInlineEdit();  // the editor's target is going away
    }
    itemRegistry_.removeNoteItem(id);
    emitSelectionChanged();
}

QPointF CanvasPresenter::debugLabelCenter(quint64 transitionId) const {
    const TransitionLabelItem* label = itemRegistry_.transitionLabels().value(transitionId, nullptr);
    return label != nullptr ? label->pos() : QPointF();
}

QRectF CanvasPresenter::debugPillRect(quint64 transitionId) const {
    const TransitionLabelItem* label = itemRegistry_.transitionLabels().value(transitionId, nullptr);
    return label != nullptr ? label->sceneRect() : QRectF();
}

QRectF CanvasPresenter::debugNoteRect(quint64 id) const {
    const NoteItem* item = itemRegistry_.noteItems().value(id, nullptr);
    return item != nullptr ? item->sceneRect() : QRectF();
}

MachineFrameItem* CanvasPresenter::ensureFrameItem() {
    if (frameItem_ == nullptr) {
        frameItem_ = new MachineFrameItem();
        frameItem_->setFlag(QGraphicsItem::ItemIsSelectable, currentMode_ == events::Mode::Design);
        frameItem_->setOnDragStarted([this](QPointF grabScenePos) { onFrameDragStarted(grabScenePos); });
        frameItem_->setOnDragMoved([this](QPointF scenePos) { onFrameDragMoved(scenePos); });
        frameItem_->setOnDragFinished([this](QPointF scenePos) { onFrameDragFinished(scenePos); });
        frameItem_->setOnDragReverted([this] { onFrameDragReverted(); });
        // Frame handles start a WireDrag with the machine root (sourceId 0).
        frameItem_->setOnWireDragStarted([this](PortSide side) { onFrameWireDragStarted(side); });
        frameItem_->setOnWireDragMoved([this](QPointF scenePos) { onWireDragMoved(scenePos); });
        frameItem_->setOnWireDragFinished([this](QPointF scenePos) { onWireDragFinished(scenePos); });
        scene_->addItem(frameItem_);
    }
    return frameItem_;
}

// Mouse-transparent: it sits where the double-click it advertises must land.
void CanvasPresenter::updateOnboardingHint() {
    const bool visible = doc_ != nullptr && itemRegistry_.stateItems().isEmpty() && currentMode_ == events::Mode::Design;
    if (!visible) {
        if (onboardingHint_ != nullptr) {
            onboardingHint_->setVisible(false);
        }
        return;
    }
    if (onboardingHint_ == nullptr) {
        onboardingHint_ = new QGraphicsSimpleTextItem(QStringLiteral("Double-click to add a state"));
        QFont hintFont = onboardingHint_->font();
        hintFont.setPointSizeF(11.0);
        onboardingHint_->setFont(hintFont);
        onboardingHint_->setBrush(design::color(design::kOnboardingHint));  // faint against the dark canvas
        onboardingHint_->setAcceptedMouseButtons(Qt::NoButton);
        const QRectF hintRect = onboardingHint_->boundingRect();
        onboardingHint_->setPos(-hintRect.width() / 2.0, -hintRect.height() / 2.0);
        scene_->addItem(onboardingHint_);
    }
    onboardingHint_->setVisible(true);
}

bool CanvasPresenter::debugOnboardingHintVisible() const {
    return onboardingHint_ != nullptr && onboardingHint_->isVisible();
}

void CanvasPresenter::onMachineNameChanged(const events::MachineNameChanged&) { routing_.refreshFrame(); }

void CanvasPresenter::updateSimPresentation() {
    // Design mode and a simulation that is not running look the same: neutral.
    const bool simulateRunning = currentMode_ == events::Mode::Simulate && sim_ != nullptr && sim_->running();
    simPresentation_.apply(SimPresentation::Scene{
        .stateItems = itemRegistry_.stateItems(),
        .transitionItems = itemRegistry_.transitionItems(),
        .transitionLabels = itemRegistry_.transitionLabels(),
        .doc = doc_.get(),
        .simulateRunning = simulateRunning,
        .initialDot = routing_.initialDot(),
        .initialStub = routing_.initialStub(),
        .initialArrow = routing_.initialArrow(),
    });
}

void CanvasPresenter::playStatePop(quint64 stateId) { simPresentation_.playStatePop(stateId, itemRegistry_.stateItems()); }

void CanvasPresenter::pulseTransition(quint64 transitionId) {
    simPresentation_.pulseTransition(transitionId, itemRegistry_.transitionItems());
}

void CanvasPresenter::stopAllSimAnimations() { simPresentation_.stopAll(itemRegistry_.stateItems(), itemRegistry_.transitionItems()); }

}  // namespace app
