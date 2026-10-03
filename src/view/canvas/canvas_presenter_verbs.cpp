// CanvasPresenter verbs: add verbs, context menus, the action box, the inline
// edit flow, and the code lens.
#include "view/canvas/canvas_presenter.h"
#include "infra/expression.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <QAction>
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
#include <QIcon>
#include <QLineF>
#include <QMenu>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QSet>
#include <QVector>

#include "model/undo_events.h"
#include "view/canvas/canvas_view.h"
#include "view/geometry/element_colors.h"  // elementAccent, for the color swatch menu
#include "view/items/machine_frame_item.h"
#include "view/items/note_item.h"
#include "view/items/state_item.h"
#include "view/items/transition_item.h"
#include "infra/node_code_projector.h"
#include "infra/settings_store.h"
#include "view/items/code_lens_item.h"

#include "view/canvas/canvas_presenter_detail.h"

namespace app {

namespace {

using InteractionState = CanvasInteractionFsm::State;

using presenter_detail::ScopedUndoBatch;

// Must match state_item.cpp's drag snap and the drawn grid.
constexpr qreal kGridStep = 24.0;

// "Event N" with the smallest unused N. Machine-wide unique, so it never
// duplicates an event on the same source.
QString firstFreeDefaultEventName(const Machine& machine) {
    for (int n = 1;; ++n) {
        const QString candidate = QStringLiteral("Event %1").arg(n);
        bool taken = false;
        for (const Transition& transition : machine.transitions) {
            if (transition.event.trimmed() == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return candidate;
        }
    }
}

}  // namespace

void CanvasPresenter::beginInlineEdit(SelectionKind kind, quint64 id) {
    if (doc_ == nullptr) {
        return;
    }

    QString initialText;
    QRectF editorRect;
    if (kind == SelectionKind::State) {
        StateItem* item = itemRegistry_.stateItems().value(id, nullptr);
        const State* state = doc_->findState(id);
        if (item == nullptr || state == nullptr) {
            return;
        }
        initialText = state->name;
        editorRect = InlineEditController::editorRectForState(item->sceneRect());
    } else if (kind == SelectionKind::Transition) {
        TransitionLabelItem* label = itemRegistry_.transitionLabels().value(id, nullptr);
        const Transition* transition = doc_->findTransition(id);
        if (label == nullptr || transition == nullptr) {
            return;
        }
        if (transition->isAlways()) {
            initialText = QStringLiteral("always");
        } else if (!transition->event.trimmed().isEmpty()) {
            initialText = transition->event;
        } else if (transition->delayMs > 0) {
            initialText = transition->periodic
                ? QStringLiteral("every %1ms").arg(transition->delayMs)
                : QStringLiteral("after %1ms").arg(transition->delayMs);
        }
        if (!transition->guard.trimmed().isEmpty()) {
            initialText += QStringLiteral(" [%1]").arg(transition->guard);
        }
        if (!transition->action.trimmed().isEmpty()) {
            initialText += QStringLiteral(" / %1").arg(transition->action);
        }
        editorRect = InlineEditController::editorRectForTransition(label->pos(), label->boundingRect().width());
    } else if (kind == SelectionKind::Note) {
        NoteItem* item = itemRegistry_.noteItems().value(id, nullptr);
        const Note* note = doc_->findNote(id);
        if (item == nullptr || note == nullptr) {
            return;
        }
        initialText = note->text;
        // Only the top-left is used; the editor autosizes.
        editorRect = item->sceneRect();
    } else {
        return;
    }

    // The FSM refuses the entry in Simulate mode or mid-session.
    fsm_.editOpened();
    if (fsm_.state() != InteractionState::InlineEdit) {
        return;
    }

    fsm_.inlineEdit() = CanvasInteractionFsm::InlineEdit{kind, id};
    inlineEdit_.begin(kind == SelectionKind::Note, initialText, editorRect);
}

void CanvasPresenter::finishInlineEdit() {
    // The target check matters too: commitInlineEdit's hide() re-enters here
    // while the FSM is still in InlineEdit, with the target already cleared.
    if (fsm_.state() != InteractionState::InlineEdit ||
        fsm_.inlineEdit().kind == SelectionKind::None) {
        return;
    }
    fsm_.sessionCommitted();
}

void CanvasPresenter::commitInlineEdit() {
    if (fsm_.inlineEdit().kind == SelectionKind::None) {
        return;
    }
    // Read while the editor is still open. trimmed() keeps a note's inner newlines.
    const QString text = inlineEdit_.currentText().trimmed();
    // Clear the target before hide(): hiding re-fires the commit signal, and
    // the cleared target turns that re-entry into a no-op.
    const Selection target{fsm_.inlineEdit().kind, fsm_.inlineEdit().id};
    fsm_.inlineEdit() = CanvasInteractionFsm::InlineEdit{};
    inlineEdit_.hide();

    if (doc_ == nullptr) {
        return;
    }
    if (target.kind == SelectionKind::State) {
        // A blank rename is dropped; blank names need the Inspector.
        const State* state = doc_->findState(target.id);
        if (state != nullptr && !text.isEmpty() && state->name != text) {
            context().send(events::RenameStateRequested{.id = target.id, .name = text});
        }
    } else if (target.kind == SelectionKind::Transition) {
        const Transition* transition = doc_->findTransition(target.id);
        if (transition != nullptr && !text.isEmpty()) {
            const expr::TransitionLabelForm parsed = expr::parseTransitionLabel(text);
            if (parsed.always != transition->isAlways()) {
                context().send(events::SetTransitionAlwaysRequested{.id = target.id, .always = parsed.always});
            }
            const QString newEvent = parsed.always ? QString() : parsed.event;
            if (transition->event != newEvent) {
                context().send(events::SetTransitionEventRequested{.id = target.id, .event = newEvent});
            }
            if (parsed.timeTrigger.ok) {
                if (transition->delayMs != parsed.timeTrigger.durationMs) {
                    context().send(events::SetTransitionDelayRequested{.id = target.id, .delayMs = parsed.timeTrigger.durationMs});
                }
            }
            if (transition->guard != parsed.guard) {
                context().send(events::SetTransitionGuardRequested{.id = target.id, .guard = parsed.guard});
            }
            if (transition->action != parsed.action) {
                context().send(events::SetTransitionActionRequested{.id = target.id, .action = parsed.action});
            }
        }
    } else if (target.kind == SelectionKind::Note) {
        // Unlike a name, an empty note text is committed.
        const Note* note = doc_->findNote(target.id);
        if (note != nullptr && note->text != text) {
            context().send(events::SetNoteTextRequested{.id = target.id, .text = text});
        }
    }
}

void CanvasPresenter::clearInlineEditState() {
    fsm_.inlineEdit() = CanvasInteractionFsm::InlineEdit{};
    inlineEdit_.hide();  // re-fires editingFinished/focus-out; the cleared target no-ops it
}

void CanvasPresenter::cancelInlineEdit() {
    clearInlineEditState();
    fsm_.sessionAborted();
}

void CanvasPresenter::onCanvasDoubleClicked(QPointF scenePos) {
    if (currentMode_ != events::Mode::Design) {
        return;  // AddStateRequested would be rejected anyway
    }
    // Container interiors are click-through, so an "empty" double-click may
    // be inside one; the new state goes into the deepest container there.
    const quint64 parentId = dragSession_.containerAtPoint(scenePos);
    const QPointF snapped(std::round(scenePos.x() / kGridStep) * kGridStep,
                           std::round(scenePos.y() / kGridStep) * kGridStep);
    context().send(events::AddStateRequested{.pos = snapped, .parentId = parentId});
}

void CanvasPresenter::drainPendingInlineEdit() {
    const Selection target = pendingInlineEditTarget_;
    pendingInlineEditTarget_ = Selection{};
    if (target.kind != SelectionKind::None) {
        beginInlineEdit(target.kind, target.id);
    }
}

void CanvasPresenter::addTransitionInDirection(quint64 sourceId, PortSide side) {
    if (doc_ == nullptr || currentMode_ != events::Mode::Design) {
        return;
    }
    StateItem* source = itemRegistry_.stateItems().value(sourceId, nullptr);
    if (source == nullptr) {
        return;
    }
    const QRectF box = source->sceneRect();

    // pos is the new box's top-left, so left/up offsets are larger.
    QPointF newPos;
    switch (side) {
        case PortSide::Right:
            newPos = QPointF(box.right() + 140.0, box.top());
            break;
        case PortSide::Left:
            newPos = QPointF(box.left() - 240.0, box.top());
            break;
        case PortSide::Bottom:
            newPos = QPointF(box.left(), box.bottom() + 100.0);
            break;
        case PortSide::Top:
            newPos = QPointF(box.left(), box.top() - 160.0);
            break;
    }
    const QPointF snapped(std::round(newPos.x() / kGridStep) * kGridStep,
                           std::round(newPos.y() / kGridStep) * kGridStep);

    // send() is synchronous, so nextId read just before a send is the id it
    // mints; a rejected command leaves nextId unused, hence the re-checks.
    // State, transition, and default name form one undo step; a name typed
    // over the default is a second one.
    const quint64 newStateId = doc_->machine().nextId;
    quint64 newTransitionId = 0;
    {
        ScopedUndoBatch batch(context());
        // The new state is a sibling of the source, inside the same parent.
        const State* sourceState = doc_->findState(sourceId);
        context().send(events::AddStateRequested{
            .pos = snapped, .parentId = sourceState != nullptr ? sourceState->parentId : 0});
        if (doc_->findState(newStateId) == nullptr) {
            return;  // the scope's dtor closes the (empty) batch
        }
        newTransitionId = doc_->machine().nextId;
        context().send(events::AddTransitionRequested{.from = sourceId, .to = newStateId});
        if (doc_->findTransition(newTransitionId) != nullptr) {
            context().send(events::SetTransitionEventRequested{
                .id = newTransitionId, .event = firstFreeDefaultEventName(doc_->machine())});
        }
    }
    if (doc_->findTransition(newTransitionId) == nullptr) {
        return;
    }

    // Record, don't open: this can run inside commitWire. The caller drains it.
    selectTransition(newTransitionId);
    pendingInlineEditTarget_ = Selection{SelectionKind::Transition, newTransitionId};
}

void CanvasPresenter::addAttachedTransition(quint64 sourceId, quint64 targetId, bool machineSelf,
                                            std::optional<QPointF> seedPillCenter) {
    // sourceId 0 is the machine root. Root-only extras, in the same batch:
    // `machineSelf` makes the event target the machine itself, and
    // `seedPillCenter` places the pill (the base anchor already exists,
    // since the add fact reroutes synchronously).
    if (doc_ == nullptr || currentMode_ != events::Mode::Design ||
        (sourceId != 0 && doc_->findState(sourceId) == nullptr)) {
        return;
    }
    quint64 newTransitionId = 0;
    {
        ScopedUndoBatch batch(context());
        newTransitionId = doc_->machine().nextId;
        context().send(events::AddTransitionRequested{.from = sourceId, .to = targetId});
        if (doc_->findTransition(newTransitionId) != nullptr) {
            if (machineSelf && sourceId == 0 && targetId == 0) {
                context().send(events::SetMachineSelfRequested{.id = newTransitionId, .machineSelf = true});
            }
            context().send(events::SetTransitionEventRequested{
                .id = newTransitionId, .event = firstFreeDefaultEventName(doc_->machine())});
            if (seedPillCenter.has_value() && routing_.hasLabelBaseAnchor(newTransitionId)) {
                context().send(events::MoveTransitionLabelRequested{
                    .id = newTransitionId,
                    .offset = seedPillCenter.value() - routing_.labelBaseAnchor(newTransitionId)});
            }
        }
    }
    if (doc_->findTransition(newTransitionId) == nullptr) {
        return;
    }
    selectTransition(newTransitionId);
    pendingInlineEditTarget_ = Selection{SelectionKind::Transition, newTransitionId};  // drained by the caller
}

void CanvasPresenter::addNoteAt(QPointF pos) {
    if (doc_ == nullptr || currentMode_ != events::Mode::Design) {
        return;
    }
    const quint64 newNoteId = doc_->machine().nextId;
    context().send(events::AddNoteRequested{.pos = pos});
    if (doc_->findNote(newNoteId) == nullptr) {
        return;  // the intent was rejected
    }
    // Outside any FSM session, so the editor opens directly.
    scene_->clearSelection();
    if (NoteItem* item = itemRegistry_.noteItems().value(newNoteId, nullptr)) {
        item->setSelected(true);
    }
    beginInlineEdit(SelectionKind::Note, newNoteId);
}

QMenu* CanvasPresenter::buildStateContextMenu(quint64 stateId) {
    if (doc_ == nullptr || doc_->findState(stateId) == nullptr) {
        return nullptr;
    }
    const State* state = doc_->findState(stateId);

    StateMenuContext menuCtx;
    menuCtx.currentKind = state->kind;
    menuCtx.isInitial = doc_->machine().initialStateId == stateId;
    menuCtx.canHaveChildren = state->kind == StateKind::Normal || state->kind == StateKind::Parallel;
    menuCtx.hasParent = state->parentId != 0;
    menuCtx.hasBreakpoint = sim_ != nullptr && sim_->hasBreakpoint(stateId);

    StateMenuCallbacks cb;
    cb.onZoomToSelection = [this] { zoomToSelection(); };
    cb.onRename = [this, stateId] {
        selectState(stateId);
        beginInlineEdit(SelectionKind::State, stateId);
    };
    cb.onSetKind = [this, stateId](StateKind kind) {
        context().send(events::SetStateKindRequested{.id = stateId, .kind = kind});
    };
    cb.onSetInitial = [this, stateId](bool checked) {
        context().send(events::SetInitialStateRequested{.id = checked ? stateId : 0});
    };
    cb.onToggleBreakpoint = [this, stateId] {
        context().send(events::ToggleBreakpointRequested{.stateId = stateId});
    };
    cb.onAddTransition = [this, stateId] {
        addTransitionInDirection(stateId, PortSide::Right);
        drainPendingInlineEdit();
    };
    cb.onAddSelfTransition = [this, stateId] {
        addAttachedTransition(stateId, stateId);
        drainPendingInlineEdit();
    };
    cb.onAddTargetless = [this, stateId] {
        addAttachedTransition(stateId, 0);
        drainPendingInlineEdit();
    };
    // The child lands at the parent box's snapped center.
    cb.onAddChildState = [this, stateId] {
        StateItem* item = itemRegistry_.stateItems().value(stateId, nullptr);
        const QPointF center = item != nullptr ? item->sceneRect().center() : QPointF();
        const QPointF snapped(std::round(center.x() / kGridStep) * kGridStep,
                               std::round(center.y() / kGridStep) * kGridStep);
        context().send(events::AddStateRequested{.pos = snapped, .parentId = stateId});
    };
    // Reparent to the grandparent (0 = root).
    cb.onMoveOutOneLevel = [this, stateId] {
        quint64 grandparentId = 0;
        if (doc_ != nullptr) {
            if (const State* st = doc_->findState(stateId)) {
                if (const State* parent = doc_->findState(st->parentId)) {
                    grandparentId = parent->parentId;
                }
            }
        }
        context().send(events::ReparentStateRequested{.id = stateId, .parentId = grandparentId});
    };
    cb.onFocusField = [this, stateId](InspectorField field) {
        selectState(stateId);  // the Inspector shows this state's tab first
        emit inspectorFieldFocusRequested(field);
    };
    cb.onDelete = [this, stateId] { context().send(events::DeleteStateRequested{.id = stateId}); };

    // Qualified: the member function of the same name hides the free one.
    return app::buildStateContextMenu(view_, menuCtx, cb);
}

// Caller owns the returned menu.
QMenu* CanvasPresenter::buildTransitionContextMenu(quint64 transitionId) {
    if (doc_ == nullptr) {
        return nullptr;
    }
    const Transition* transition = doc_->findTransition(transitionId);
    if (transition == nullptr) {
        return nullptr;
    }

    TransitionMenuContext menuCtx;
    menuCtx.hasTarget = transition->to != 0;
    menuCtx.hasBendpoints = !transition->manualBendpoints.isEmpty();
    menuCtx.hasLabelOffset = !transition->labelOffset.isNull();

    TransitionMenuCallbacks cb;
    cb.onZoomToSelection = [this] { zoomToSelection(); };
    cb.onSetEvent = [this, transitionId] {
        selectTransition(transitionId);
        beginInlineEdit(SelectionKind::Transition, transitionId);
    };
    cb.onFocusField = [this, transitionId](InspectorField field) {
        selectTransition(transitionId);
        emit inspectorFieldFocusRequested(field);
    };
    // Only offered when there is a target (ctx.hasTarget).
    const quint64 from = transition->from;
    const quint64 to = transition->to;
    cb.onReverseDirection = [this, transitionId, from, to] {
        context().send(events::RetargetTransitionRequested{.id = transitionId, .from = to, .to = from});
    };
    cb.onResetRoute = [this, transitionId] {
        context().send(events::SetTransitionBendpointsRequested{.id = transitionId, .bendpoints = {}});
    };
    cb.onResetLabelPosition = [this, transitionId] {
        routing_.setLabelDetached(transitionId, false);
        context().send(events::MoveTransitionLabelRequested{
            .id = transitionId,
            .offset = QPointF(),
            .resetBendpoints = false,
        });
    };
    cb.onDelete = [this, transitionId] { context().send(events::DeleteTransitionRequested{.id = transitionId}); };

    return app::buildTransitionContextMenu(view_, menuCtx, cb);
}

void CanvasPresenter::showQuickAddMenu(quint64 stateId) {
    if (doc_ == nullptr || currentMode_ != events::Mode::Design || doc_->findState(stateId) == nullptr) {
        return;
    }
    AddTransitionCallbacks cb;
    cb.onAddTransition = [this, stateId] {
        addTransitionInDirection(stateId, PortSide::Right);
        drainPendingInlineEdit();
    };
    cb.onAddSelfTransition = [this, stateId] {
        addAttachedTransition(stateId, stateId);
        drainPendingInlineEdit();
    };
    cb.onAddTargetless = [this, stateId] {
        addAttachedTransition(stateId, 0);
        drainPendingInlineEdit();
    };
    execQuickAddMenu(view_, cb);
}

void CanvasPresenter::ensureActionBox() {
    if (actionBox_ == nullptr) {
        actionBox_ = new ActionBoxItem();
        actionBox_->setOnVerb([this](ActionBoxVerb verb) { onActionBoxVerb(verb); });
        scene_->addItem(actionBox_);
    }
}

// Visible iff Design mode, the FSM is Idle, and the selection has a variant.
void CanvasPresenter::updateActionBoxVisibility() {
    if (doc_ == nullptr) {
        return;
    }
    const SelectionDelegate::ActionBoxInputs inputs = selection_.actionBoxInputsFor(selection_.last());

    const bool visible =
        inputs.actionable && currentMode_ == events::Mode::Design && fsm_.state() == InteractionState::Idle;
    if (!visible) {
        if (actionBox_ != nullptr) {
            actionBox_->hideBox();
        }
        return;
    }
    ensureActionBox();
    actionBox_->showFor(selection_.last().kind, inputs.anchorRect, inputs.reverseEnabled);
}

// Every verb maps to an existing verb or intent. The box hides first, like a
// menu closing; a verb that changes the selection may bring it back.
void CanvasPresenter::onActionBoxVerb(ActionBoxVerb verb) {
    if (actionBox_ != nullptr) {
        actionBox_->hideBox();
    }
    if (doc_ == nullptr) {
        return;
    }
    const Selection target = selection_.last();
    switch (verb) {
        case ActionBoxVerb::ZoomToSelection:
            if (target.kind == SelectionKind::Multi) {
                zoomToSelection();
            }
            break;
        case ActionBoxVerb::DeleteSelection:
            // Same as the Delete key.
            if (target.kind == SelectionKind::Multi) {
                onDeleteRequested();
            }
            break;
        case ActionBoxVerb::MultiColor: {
            if (target.kind != SelectionKind::Multi) {
                break;
            }
            QMenu menu(view_);
            const std::array<std::pair<ElementColor, QString>, 9> palette{{
                {ElementColor::Default, QStringLiteral("No color")},
                {ElementColor::Red, QStringLiteral("Red")},
                {ElementColor::Orange, QStringLiteral("Orange")},
                {ElementColor::Yellow, QStringLiteral("Yellow")},
                {ElementColor::Green, QStringLiteral("Green")},
                {ElementColor::Blue, QStringLiteral("Blue")},
                {ElementColor::Purple, QStringLiteral("Purple")},
                {ElementColor::Pink, QStringLiteral("Pink")},
                {ElementColor::Gray, QStringLiteral("Gray")},
            }};
            for (const auto& [color, label] : palette) {
                QPixmap swatch(14, 14);
                swatch.fill(color == ElementColor::Default ? Qt::transparent : elementAccent(color));
                QAction* action = menu.addAction(QIcon(swatch), label);
                action->setData(static_cast<int>(color));
            }
            QAction* picked = menu.exec(QCursor::pos());
            if (picked != nullptr) {
                applyColorToSelection(static_cast<ElementColor>(picked->data().toInt()));
            }
            break;
        }
        case ActionBoxVerb::AddMachineEvent:
            if (target.kind == SelectionKind::Frame) {
                addAttachedTransition(0, 0);
                drainPendingInlineEdit();
            }
            break;
        case ActionBoxVerb::AddMachineSelf:
            // A root event that targets the machine itself (a restart).
            if (target.kind == SelectionKind::Frame) {
                addAttachedTransition(0, 0, /*machineSelf=*/true);
                drainPendingInlineEdit();
            }
            break;
        case ActionBoxVerb::FrameMore: {
            // The empty-canvas menu; positional verbs use the spot just below
            // the frame's bottom-left, like AddState.
            if (target.kind != SelectionKind::Frame || frameItem_ == nullptr || !frameItem_->isVisible()) {
                break;
            }
            const QRectF frame = frameItem_->sceneFrameRect();
            const QPointF raw(frame.left() + 40.0, frame.bottom() + 40.0);
            const QPointF spot(std::round(raw.x() / kGridStep) * kGridStep,
                                std::round(raw.y() / kGridStep) * kGridStep);
            EmptyCanvasCallbacks cb;
            cb.onAddStateHere = [this, spot] { onCanvasDoubleClicked(spot); };
            cb.onAddMachineEvent = [this] { context().send(events::AddTransitionRequested{.from = 0, .to = 0}); };
            cb.onAddNote = [this, spot] { addNoteAt(spot); };
            cb.onAutoLayout = [this] { emit autoLayoutRequested(); };
            cb.onZoomToFit = [this] { zoomToFit(); };
            cb.onResetView = [this] { view_->resetView(scene_->itemsBoundingRect()); };
            execEmptyCanvasMenu(view_, QCursor::pos(), /*design=*/true, cb);
            break;
        }
        case ActionBoxVerb::AddState: {
            // No click position: use a fixed spot below the frame's bottom-left.
            if (frameItem_ == nullptr || !frameItem_->isVisible()) {
                return;
            }
            const QRectF frame = frameItem_->sceneFrameRect();
            const QPointF raw(frame.left() + 40.0, frame.bottom() + 40.0);
            context().send(events::AddStateRequested{
                .pos = QPointF(std::round(raw.x() / kGridStep) * kGridStep,
                                std::round(raw.y() / kGridStep) * kGridStep)});
            break;
        }
        case ActionBoxVerb::AddNote: {
            if (frameItem_ == nullptr || !frameItem_->isVisible()) {
                return;
            }
            const QRectF frame = frameItem_->sceneFrameRect();
            const QPointF raw(frame.left() + 220.0, frame.bottom() + 40.0);
            addNoteAt(QPointF(std::round(raw.x() / kGridStep) * kGridStep,
                               std::round(raw.y() / kGridStep) * kGridStep));
            break;
        }
        case ActionBoxVerb::AddTransition:
            // The state variant's "+" adds a targetless transition.
            if (target.kind == SelectionKind::State) {
                addAttachedTransition(target.id, 0);
                drainPendingInlineEdit();
            }
            break;
        case ActionBoxVerb::AddSelfTransition:
            if (target.kind == SelectionKind::State) {
                addAttachedTransition(target.id, target.id);
                drainPendingInlineEdit();
            }
            break;
        case ActionBoxVerb::StateMore:
            if (target.kind == SelectionKind::State) {
                if (QMenu* menu = buildStateContextMenu(target.id)) {
                    menu->exec(QCursor::pos());
                    menu->deleteLater();
                }
            }
            break;
        case ActionBoxVerb::SetGuard:
        case ActionBoxVerb::SetAction:
            if (target.kind == SelectionKind::Transition) {
                selectTransition(target.id);
                emit inspectorFieldFocusRequested(verb == ActionBoxVerb::SetGuard ? InspectorField::TransitionGuard
                                                                                   : InspectorField::TransitionAction);
            }
            break;
        case ActionBoxVerb::ReverseDirection:
            // Disabled for a targetless transition.
            if (target.kind == SelectionKind::Transition) {
                if (const Transition* transition = doc_->findTransition(target.id);
                    transition != nullptr && transition->to != 0) {
                    context().send(events::RetargetTransitionRequested{
                        .id = target.id, .from = transition->to, .to = transition->from});
                }
            }
            break;
        case ActionBoxVerb::TransitionMore:
            if (target.kind == SelectionKind::Transition) {
                if (QMenu* menu = buildTransitionContextMenu(target.id)) {
                    menu->exec(QCursor::pos());
                    menu->deleteLater();
                }
            }
            break;
    }
}

void CanvasPresenter::applyColorToSelection(ElementColor color) {
    if (doc_ == nullptr || currentMode_ != events::Mode::Design) {
        return;
    }
    // One undo batch; the frame has no color and is skipped.
    ScopedUndoBatch batch(context());
    for (QGraphicsItem* item : scene_->selectedItems()) {
        if (item->type() == StateItem::Type) {
            context().send(events::SetStateColorRequested{.id = static_cast<StateItem*>(item)->id(), .color = color});
        } else if (item->type() == TransitionItem::Type) {
            context().send(
                events::SetTransitionColorRequested{.id = static_cast<TransitionItem*>(item)->id(), .color = color});
        } else if (item->type() == NoteItem::Type) {
            context().send(events::SetNoteColorRequested{.id = static_cast<NoteItem*>(item)->id(), .color = color});
        }
    }
}

void CanvasPresenter::showContextMenu(QPointF scenePos, QPoint globalPos) {
    if (doc_ == nullptr || scene_ == nullptr) {
        return;
    }

    // Topmost hit wins; a pill or edge beats an overlapping state container,
    // and the frame only wins when nothing else is hit. Simulate mode uses
    // the hit too, as its Zoom to Selection target.
    quint64 stateId = 0;
    quint64 transitionId = 0;
    bool frameHit = false;
    for (QGraphicsItem* item : scene_->items(scenePos)) {
        if (qgraphicsitem_cast<TransitionLabelItem*>(item) != nullptr) {
            for (auto it = itemRegistry_.transitionLabels().constBegin(); it != itemRegistry_.transitionLabels().constEnd(); ++it) {
                if (it.value() == item) {
                    transitionId = it.key();
                    break;
                }
            }
            if (transitionId != 0) {
                break;
            }
        }
        if (auto* edgeItem = qgraphicsitem_cast<TransitionItem*>(item)) {
            transitionId = edgeItem->id();
            break;
        }
        if (auto* stateItem = qgraphicsitem_cast<StateItem*>(item)) {
            stateId = stateItem->id();
            break;
        }
        if (item == frameItem_) {
            frameHit = true;
            break;
        }
    }

    // Simulate mode offers only view verbs. Nothing is selectable there, so
    // the right-clicked element is the Zoom to Selection target.
    if (currentMode_ != events::Mode::Design) {
        EmptyCanvasCallbacks cb;
        if (stateId != 0 || transitionId != 0) {
            cb.onZoomToSelection = [this, stateId, transitionId] {
                zoomToElements(stateId != 0 ? QList<quint64>{stateId} : QList<quint64>{},
                               transitionId != 0 ? QList<quint64>{transitionId} : QList<quint64>{});
            };
        }
        cb.onZoomToFit = [this] { zoomToFit(); };
        cb.onResetView = [this] { view_->resetView(scene_->itemsBoundingRect()); };
        execEmptyCanvasMenu(view_, globalPos, /*design=*/false, cb);
        return;
    }

    // Right-clicking inside the selection keeps it (multi-selection too);
    // otherwise the clicked item becomes the only selection.
    if (transitionId != 0) {
        const TransitionItem* edge = itemRegistry_.transitionItems().value(transitionId, nullptr);
        if (edge == nullptr || !edge->isSelected()) {
            selectTransition(transitionId);
        }
        if (QMenu* menu = buildTransitionContextMenu(transitionId)) {
            menu->exec(globalPos);
            menu->deleteLater();
        }
        return;
    }

    if (stateId != 0) {
        const StateItem* state = itemRegistry_.stateItems().value(stateId, nullptr);
        if (state == nullptr || !state->isSelected()) {
            selectState(stateId);
        }
        if (QMenu* menu = buildStateContextMenu(stateId)) {
            menu->exec(globalPos);
            menu->deleteLater();
        }
        return;
    }

    EmptyCanvasCallbacks cb;
    cb.onAddStateHere = [this, scenePos] { onCanvasDoubleClicked(scenePos); };
    cb.onAddMachineEvent = [this] { context().send(events::AddTransitionRequested{.from = 0, .to = 0}); };
    cb.onAddNote = [this, scenePos] { addNoteAt(scenePos); };
    cb.onAutoLayout = [this] { emit autoLayoutRequested(); };
    if (frameHit) {
        // The frame's right press selected it (or kept a selection it was in).
        cb.onZoomToSelection = [this] { zoomToSelection(); };
    }
    cb.onZoomToFit = [this] { zoomToFit(); };
    cb.onResetView = [this] { view_->resetView(scene_->itemsBoundingRect()); };
    execEmptyCanvasMenu(view_, globalPos, /*design=*/true, cb);
}

void CanvasPresenter::debugCommitInlineEditText(const QString& text) {
    inlineEdit_.debugSetText(text);
    finishInlineEdit();
}

QTextEdit* CanvasPresenter::debugInlineEditAreaWidget() const { return inlineEdit_.debugNoteEditorWidget(); }

void CanvasPresenter::toggleCodeLens() {
    if (codeLens_ != nullptr && codeLens_->isLensVisible()) {
        codeLens_->hideLens();
        return;
    }
    showCodeLensForSelection();
}

void CanvasPresenter::showCodeLensForSelection() {
    if (scene_ == nullptr || doc_ == nullptr) {
        return;
    }
    const CanvasSelection sel = selection_.last();
    QString title;
    QString code;
    QRectF anchorRect;
    if (sel.kind == SelectionKind::State) {
        const State* s = doc_->findState(sel.id);
        if (s == nullptr) {
            return;
        }
        title = s->name;
        code = projectStateCode(doc_->machine(), sel.id).fullSnippet;
        if (StateItem* item = itemRegistry_.stateItems().value(sel.id, nullptr)) {
            anchorRect = item->sceneRect();
        }
    } else if (sel.kind == SelectionKind::Transition) {
        const Transition* t = doc_->findTransition(sel.id);
        if (t == nullptr) {
            return;
        }
        title = t->event.isEmpty() ? QStringLiteral("Transition") : t->event;
        code = projectTransitionCode(doc_->machine(), sel.id).fullSnippet;
        if (TransitionLabelItem* label = itemRegistry_.transitionLabels().value(sel.id, nullptr)) {
            anchorRect = label->sceneRect();
        } else if (TransitionItem* item = itemRegistry_.transitionItems().value(sel.id, nullptr)) {
            anchorRect = item->sceneBoundingRect();
        }
    } else {
        return;
    }

    auto* store = SettingsStore::activeStore();
    if (store && !store->get(QStringLiteral("devmode.codeLensEnabled"), true).toBool()) {
        return;
    }

    if (codeLens_ == nullptr) {
        codeLens_ = new CodeLensItem();
        scene_->addItem(codeLens_);
    }
    codeLens_->showLens(title, code, anchorRect);
}

void CanvasPresenter::hideCodeLens() {
    if (codeLens_ != nullptr) {
        codeLens_->hideLens();
    }
}

bool CanvasPresenter::debugCodeLensVisible() const {
    return codeLens_ != nullptr && codeLens_->isLensVisible();
}

QString CanvasPresenter::debugCodeLensTitle() const {
    return codeLens_ != nullptr ? codeLens_->debugTitle() : QString();
}

QString CanvasPresenter::debugCodeLensCode() const {
    return codeLens_ != nullptr ? codeLens_->debugCode() : QString();
}

QRectF CanvasPresenter::debugCodeLensRect() const {
    return codeLens_ != nullptr ? codeLens_->sceneBoundingRect() : QRectF();
}

}  // namespace app
