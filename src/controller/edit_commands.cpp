#include "controller/edit_commands.h"

#include <QPair>
#include <QSet>
#include <QVector>

#include "infra/expression.h"
#include "model/machine_doc.h"
#include "model/sim_agent.h"

namespace app {

namespace detail {

// Every edit intent is rejected while simulating. The null check is defensive only.
bool simulating(ordo::core::CommandContext& context) {
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    return sim && sim->mode() == events::Mode::Simulate;
}

// A (from, event) group may hold several transitions as long as at most one is
// unguarded. True iff event (trimmed) is non-blank, guardOfChanged (trimmed) is blank, and
// another transition (excluding excludeTransitionId) already shares from + that event with
// a blank guard, i.e. the pending change would leave a second unguarded member.
bool wouldCreateSecondUnguarded(const Machine& machine, quint64 from, const QString& event,
                                 const QString& guardOfChanged, quint64 excludeTransitionId) {
    const QString trimmedEvent = event.trimmed();
    if (trimmedEvent.isEmpty() || !guardOfChanged.trimmed().isEmpty()) {
        return false;
    }
    for (const auto& transition : machine.transitions) {
        if (transition.id != excludeTransitionId && transition.from == from &&
            transition.event.trimmed() == trimmedEvent && transition.guard.trimmed().isEmpty()) {
            return true;
        }
    }
    return false;
}

// Rewrites every "done.invoke.<before>" / "error.platform.<before>" event leaving
// `stateId` to name `after`, via setTransitionEvent directly (bypassing
// SetTransitionEventCommand's policy). No-op if `before` is empty or unchanged.
// Collected first, applied after, because setTransitionEvent mutates the vector walked.
void cascadeInvokeIdRename(MachineDocAgent& doc, quint64 stateId, const QString& before, const QString& after) {
    if (before.isEmpty() || before == after) {
        return;
    }
    const QString doneBefore = QStringLiteral("done.invoke.") + before;
    const QString errorBefore = QStringLiteral("error.platform.") + before;
    QVector<QPair<quint64, QString>> rewrites;
    for (const Transition& transition : doc.machine().transitions) {
        if (transition.from != stateId) {
            continue;
        }
        if (transition.event == doneBefore) {
            rewrites.push_back({transition.id, QStringLiteral("done.invoke.") + after});
        } else if (transition.event == errorBefore) {
            rewrites.push_back({transition.id, QStringLiteral("error.platform.") + after});
        }
    }
    for (const auto& [transitionId, newEvent] : rewrites) {
        doc.setTransitionEvent(transitionId, newEvent);
    }
}

}  // namespace detail

void AddStateCommand::execute(const events::AddStateRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    if (event.parentId != 0) {
        const State* parent = doc->findState(event.parentId);
        if (!parent) {
            qWarning("AddStateCommand: rejected -- parent id %llu does not exist",
                     static_cast<unsigned long long>(event.parentId));
            return;
        }
        if (parent->kind == StateKind::Final || parent->kind == StateKind::History) {
            qWarning("AddStateCommand: rejected -- parent %llu is Final/History and cannot contain children",
                     static_cast<unsigned long long>(event.parentId));
            return;
        }
    }
    doc->addState(event.pos, event.parentId);
}

void RenameStateCommand::execute(const events::RenameStateRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->renameState(event.id, event.name);
}

void SetStateKindCommand::execute(const events::SetStateKindRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->setStateKind(event.id, event.kind);
}

void MoveStateCommand::execute(const events::MoveStateRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->moveState(event.id, event.pos);
}

void SetEntryActionsCommand::execute(const events::SetEntryActionsRequested& event,
                                      ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->setEntryActions(event.id, event.entryActions);
}

void SetExitActionsCommand::execute(const events::SetExitActionsRequested& event,
                                     ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->setExitActions(event.id, event.exitActions);
}

void SetDescriptionCommand::execute(const events::SetDescriptionRequested& event,
                                     ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->setDescription(event.id, event.description);
}

void SetTagsCommand::execute(const events::SetTagsRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->setTags(event.id, event.tags);
}

void SetHistoryDeepCommand::execute(const events::SetHistoryDeepRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    const State* state = doc->findState(event.stateId);
    if (!state || state->kind != StateKind::History) {
        qWarning("SetHistoryDeepCommand: rejected -- state %llu is not History-kind",
                 static_cast<unsigned long long>(event.stateId));
        return;
    }
    doc->setHistoryDeep(event.stateId, event.deep);
}

void SetInvokeSrcCommand::execute(const events::SetInvokeSrcRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    const State* state = doc ? doc->findState(event.stateId) : nullptr;
    if (!state) {
        return;
    }
    const QString before = effectiveInvokeId(*state);
    doc->setInvokeSrc(event.stateId, event.src);
    const QString after = effectiveInvokeId(*doc->findState(event.stateId));
    detail::cascadeInvokeIdRename(*doc, event.stateId, before, after);
}

void SetInvokeIdCommand::execute(const events::SetInvokeIdRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    const State* state = doc ? doc->findState(event.stateId) : nullptr;
    if (!state) {
        return;
    }
    const QString before = effectiveInvokeId(*state);
    doc->setInvokeId(event.stateId, event.invokeId);
    const QString after = effectiveInvokeId(*doc->findState(event.stateId));
    detail::cascadeInvokeIdRename(*doc, event.stateId, before, after);
}

void SetInvokeOutputTypeCommand::execute(const events::SetInvokeOutputTypeRequested& event,
                                          ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findState(event.stateId) == nullptr) {
        return;
    }
    doc->setInvokeOutputType(event.stateId, event.type);
}

void SetInvocationsCommand::execute(const events::SetInvocationsRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    const State* state = doc ? doc->findState(event.stateId) : nullptr;
    if (!state) {
        return;
    }
    const auto beforeInvs = state->effectiveInvocations();
    doc->setInvocations(event.stateId, event.invocations);
    const State* updated = doc->findState(event.stateId);
    if (!updated) {
        return;
    }
    const auto afterInvs = updated->effectiveInvocations();
    const int count = std::min(beforeInvs.size(), afterInvs.size());
    for (int i = 0; i < count; ++i) {
        const QString beforeId = effectiveInvocationId(beforeInvs[i]);
        const QString afterId = effectiveInvocationId(afterInvs[i]);
        if (beforeId != afterId) {
            detail::cascadeInvokeIdRename(*doc, event.stateId, beforeId, afterId);
        }
    }
}

void SetStateColorCommand::execute(const events::SetStateColorRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findState(event.id) == nullptr) {
        return;
    }
    doc->setStateColor(event.id, event.color);
}

void SetTransitionColorCommand::execute(const events::SetTransitionColorRequested& event,
                                        ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findTransition(event.id) == nullptr) {
        return;
    }
    doc->setTransitionColor(event.id, event.color);
}

void SetNoteColorCommand::execute(const events::SetNoteColorRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findNote(event.id) == nullptr) {
        return;
    }
    doc->setNoteColor(event.id, event.color);
}

void SetMachineSelfCommand::execute(const events::SetMachineSelfRequested& event,
                                    ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    const Transition* transition = doc->findTransition(event.id);
    if (transition == nullptr) {
        return;
    }
    if (event.machineSelf && (transition->from != 0 || transition->to != 0)) {
        qWarning("SetMachineSelfCommand: rejected -- transition %llu is not a root targetless transition",
                 static_cast<unsigned long long>(event.id));
        return;
    }
    doc->setMachineSelf(event.id, event.machineSelf);
}

void ReparentStateCommand::execute(const events::ReparentStateRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        qWarning("ReparentStateCommand: rejected -- machine is simulating");
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    const State* parent = nullptr;
    if (event.parentId != 0) {
        parent = doc->findState(event.parentId);
        if (!parent) {
            qWarning("ReparentStateCommand: rejected -- parent id %llu does not exist",
                     static_cast<unsigned long long>(event.parentId));
            return;
        }
    }
    if (event.parentId == event.id) {
        qWarning("ReparentStateCommand: rejected -- state %llu cannot be its own parent",
                 static_cast<unsigned long long>(event.id));
        return;
    }
    if (parent) {
        // The new parent must not lie inside id's own subtree: walk up from it, and reaching
        // id means a cycle. The visited guard stops a pre-existing cycle from looping forever.
        QSet<quint64> visited;
        quint64 current = event.parentId;
        while (current != 0 && !visited.contains(current)) {
            if (current == event.id) {
                qWarning("ReparentStateCommand: rejected -- parent %llu lies inside state %llu's own subtree",
                         static_cast<unsigned long long>(event.parentId), static_cast<unsigned long long>(event.id));
                return;
            }
            visited.insert(current);
            const State* ancestor = doc->findState(current);
            current = ancestor ? ancestor->parentId : 0;
        }
        if (parent->kind == StateKind::Final || parent->kind == StateKind::History) {
            qWarning("ReparentStateCommand: rejected -- parent %llu is Final/History and cannot contain children",
                     static_cast<unsigned long long>(event.parentId));
            return;
        }
    }
    doc->reparentState(event.id, event.parentId);
}

void SetInitialChildCommand::execute(const events::SetInitialChildRequested& event,
                                      ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    if (event.initialChildId != 0) {
        const State* child = doc->findState(event.initialChildId);
        if (!child || child->parentId != event.stateId) {
            return;  // policy: initialChildId must name an actual direct child of stateId
        }
    }
    doc->setInitialChild(event.stateId, event.initialChildId);
}

void SetMachineNameCommand::execute(const events::SetMachineNameRequested& event,
                                     ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->setMachineName(event.name);
}

void SetInitialStateCommand::execute(const events::SetInitialStateRequested& event,
                                      ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    if (event.id != 0 && doc->findState(event.id) == nullptr) {
        return;  // policy: never point initialStateId at a state that does not exist
    }
    doc->setInitialState(event.id);
}

void AddTransitionCommand::execute(const events::AddTransitionRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    // Vacuous today: a blank event always makes wouldCreateSecondUnguarded() false.
    if (detail::wouldCreateSecondUnguarded(doc->machine(), event.from, QString(), QString(), 0)) {
        return;
    }
    doc->addTransition(event.from, event.to);
}

void SetTransitionEventCommand::execute(const events::SetTransitionEventRequested& event,
                                         ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    const Transition* transition = doc->findTransition(event.id);
    if (!transition) {
        return;
    }
    // Refuse only when this transition is unguarded and the new event would join a group that
    // already has an unguarded member.
    if (detail::wouldCreateSecondUnguarded(doc->machine(), transition->from, event.event, transition->guard,
                                            event.id)) {
        return;
    }
    if (transition->always && !event.event.isEmpty()) {
        doc->setTransitionAlways(event.id, false);
    }
    doc->setTransitionEvent(event.id, event.event);
}

void SetTransitionGuardCommand::execute(const events::SetTransitionGuardRequested& event,
                                         ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    const Transition* transition = doc->findTransition(event.id);
    if (!transition) {
        return;
    }
    // Refuse only a clear that would leave a second unguarded member in this transition's
    // (from, event) group; setting a non-blank guard is always allowed.
    if (detail::wouldCreateSecondUnguarded(doc->machine(), transition->from, transition->event, event.guard,
                                            event.id)) {
        return;
    }
    doc->setTransitionGuard(event.id, event.guard);
}

void SetTransitionActionCommand::execute(const events::SetTransitionActionRequested& event,
                                          ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->setTransitionAction(event.id, event.action);
}

void RenameGuardCommand::execute(const events::RenameGuardRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    const QString before = event.before.trimmed();
    const QString after = event.after.trimmed();
    if (before.isEmpty() || before == after) {
        return;  // nothing to rename, or renaming to itself
    }
    if (after.isEmpty()) {
        qWarning("RenameGuardCommand: rejected -- blank `after` would rename guard hook \"%s\" to nothing, which is "
                 "a delete (risking a second unguarded (from, event) member), not a rename",
                 qUtf8Printable(before));
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    // Collected first, applied after: setTransitionGuard mutates the vector walked here.
    // Only a whole-string (trimmed) hook matches; an expression merely containing `before` is untouched.
    QVector<quint64> rewrites;
    for (const Transition& transition : doc->machine().transitions) {
        if (expr::isBareIdentifier(transition.guard) && transition.guard.trimmed() == before) {
            rewrites.push_back(transition.id);
        }
    }
    if (rewrites.isEmpty()) {
        return;  // `before` names no hook guard: silent no-op, no fact, no undo entry
    }
    for (quint64 transitionId : rewrites) {
        doc->setTransitionGuard(transitionId, after);
    }
    context.send(events::GuardRenamed{.before = before, .after = after});
}

void RenameActionCommand::execute(const events::RenameActionRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    const QString before = event.before.trimmed();
    const QString after = event.after.trimmed();
    if (before.isEmpty() || before == after) {
        return;
    }
    if (after.isEmpty()) {
        qWarning("RenameActionCommand: rejected -- blank `after` would rename action hook \"%s\" to nothing, which "
                 "is a delete, not a rename",
                 qUtf8Printable(before));
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    // Actions share one vocabulary, so a rename walks three fields: transition actions, then
    // state entry actions, then exit actions. Collected first, applied after, as in RenameGuardCommand.
    QVector<quint64> transitionRewrites;
    for (const Transition& transition : doc->machine().transitions) {
        if (expr::isBareIdentifier(transition.action) && transition.action.trimmed() == before) {
            transitionRewrites.push_back(transition.id);
        }
    }
    QVector<quint64> entryRewrites;
    QVector<quint64> exitRewrites;
    for (const State& state : doc->machine().states) {
        for (const QString& entryAction : state.entryActions) {
            if (expr::isBareIdentifier(entryAction) && entryAction.trimmed() == before) {
                entryRewrites.push_back(state.id);
                break;  // one match suffices -- setEntryActions below rewrites the WHOLE list
            }
        }
        for (const QString& exitAction : state.exitActions) {
            if (expr::isBareIdentifier(exitAction) && exitAction.trimmed() == before) {
                exitRewrites.push_back(state.id);
                break;
            }
        }
    }
    if (transitionRewrites.isEmpty() && entryRewrites.isEmpty() && exitRewrites.isEmpty()) {
        return;  // `before` names no hook action: silent no-op, no fact, no undo entry
    }
    for (quint64 transitionId : transitionRewrites) {
        doc->setTransitionAction(transitionId, after);
    }
    for (quint64 stateId : entryRewrites) {
        const State* state = doc->findState(stateId);
        if (!state) {
            continue;
        }
        QStringList rewritten = state->entryActions;
        for (QString& entryAction : rewritten) {
            if (expr::isBareIdentifier(entryAction) && entryAction.trimmed() == before) {
                entryAction = after;
            }
        }
        doc->setEntryActions(stateId, rewritten);
    }
    for (quint64 stateId : exitRewrites) {
        const State* state = doc->findState(stateId);
        if (!state) {
            continue;
        }
        QStringList rewritten = state->exitActions;
        for (QString& exitAction : rewritten) {
            if (expr::isBareIdentifier(exitAction) && exitAction.trimmed() == before) {
                exitAction = after;
            }
        }
        doc->setExitActions(stateId, rewritten);
    }
    context.send(events::ActionRenamed{.before = before, .after = after});
}

void SetTransitionDelayCommand::execute(const events::SetTransitionDelayRequested& event,
                                         ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    const Transition* transition = doc->findTransition(event.id);
    if (!transition) {
        return;
    }
    if (transition->always && event.delayMs != 0) {
        doc->setTransitionAlways(event.id, false);
    }
    doc->setTransitionDelay(event.id, event.delayMs);
}

void SetTransitionReenterCommand::execute(const events::SetTransitionReenterRequested& event,
                                          ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findTransition(event.id) == nullptr) {
        return;
    }
    doc->setTransitionReenter(event.id, event.reenter);
}

void SetTransitionAlwaysCommand::execute(const events::SetTransitionAlwaysRequested& event,
                                         ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    const Transition* transition = doc->findTransition(event.id);
    if (!transition) {
        return;
    }
    doc->setTransitionAlways(event.id, event.always);
    if (event.always) {
        if (!transition->event.isEmpty()) {
            doc->setTransitionEvent(event.id, QString());
        }
        if (transition->delayMs != 0) {
            doc->setTransitionDelay(event.id, 0);
        }
    }
}

void SetTransitionPayloadTypeCommand::execute(const events::SetTransitionPayloadTypeRequested& event,
                                               ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findTransition(event.id) == nullptr) {
        return;
    }
    doc->setTransitionPayloadType(event.id, event.payloadType);
}

void RetargetTransitionCommand::execute(const events::RetargetTransitionRequested& event,
                                         ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    const Transition* transition = doc->findTransition(event.id);
    if (!transition) {
        return;
    }
    // A root transition's machine-side end is not rewireable; only the target moves.
    const quint64 from = transition->from == 0 ? 0 : event.from;
    // Refuse a from-change that would join an unguarded transition into a group (keyed on
    // the new source) that already has an unguarded member.
    if (detail::wouldCreateSecondUnguarded(doc->machine(), from, transition->event, transition->guard, event.id)) {
        return;
    }
    // A machineSelf pill reconnected onto a real state stops being a self-target: clear the
    // flag first, in the same execution, so both ops journal as one undo step.
    if (transition->machineSelf && event.to != 0) {
        doc->setMachineSelf(event.id, false);
    }
    doc->retargetTransition(event.id, from, event.to, event.targets);
}

void MoveTransitionLabelCommand::execute(const events::MoveTransitionLabelRequested& event,
                                          ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    const Transition* transition = doc->findTransition(event.id);
    if (event.resetBendpoints && transition != nullptr && !transition->manualBendpoints.isEmpty()) {
        doc->setTransitionBendpoints(event.id, {});
    }
    doc->setTransitionLabelOffset(event.id, event.offset);
}

void ApplyLayoutPlanCommand::execute(const events::ApplyLayoutPlanRequested& event,
                                     ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    // Compare x/y exactly, since QPointF's operator== is fuzzy.
    for (const StatePlacement& placement : event.states) {
        const State* state = doc->findState(placement.id);
        if (state == nullptr
            || (state->pos.x() == placement.pos.x() && state->pos.y() == placement.pos.y())) {
            continue;
        }
        doc->moveState(placement.id, placement.pos);
    }
    for (const LabelPlacement& placement : event.labels) {
        const Transition* transition = doc->findTransition(placement.id);
        if (transition == nullptr) {
            continue;
        }
        // The layout owns a placed label: a leftover drag offset would be added to the new
        // base and miss the wire. Clearing it is journaled, so one undo brings it back.
        if (!transition->labelOffset.isNull()) {
            doc->setTransitionLabelOffset(placement.id, QPointF());
        }
        if (transition->labelRatio != placement.ratio) {
            doc->setTransitionLabelRatio(placement.id, placement.ratio);
        }
    }
}

void SetTransitionBendpointsCommand::execute(const events::SetTransitionBendpointsRequested& event,
                                             ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->setTransitionBendpoints(event.id, event.bendpoints);
}

void DeleteStateCommand::execute(const events::DeleteStateRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->deleteState(event.id);
}

void DeleteTransitionCommand::execute(const events::DeleteTransitionRequested& event,
                                       ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->deleteTransition(event.id);
}

void MachineSnapshotCommand::execute(const events::MachineSnapshotRequested&, ordo::core::CommandContext& context) {
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    context.send(events::MachineSnapshotPublished{.machine = doc->machine()});
}

void AddNoteCommand::execute(const events::AddNoteRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->addNote(event.pos);
}

void MoveNoteCommand::execute(const events::MoveNoteRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->moveNote(event.id, event.pos);
}

void SetNoteTextCommand::execute(const events::SetNoteTextRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->setNoteText(event.id, event.text);
}

void DeleteNoteCommand::execute(const events::DeleteNoteRequested& event, ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->deleteNote(event.id);
}

void AddContextVariableCommand::execute(const events::AddContextVariableRequested&,
                                         ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->addContextVariable();
}

void RenameContextVariableCommand::execute(const events::RenameContextVariableRequested& event,
                                            ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    const ContextVariable* variable = doc ? doc->findContextVariable(event.id) : nullptr;
    if (!variable) {
        return;
    }
    // Capture the old name first: the rename below overwrites it in place.
    const QString before = variable->name;
    doc->renameContextVariable(event.id, event.name);
    if (before == event.name) {
        return;  // no-op rename (same name, or blank/unchanged): no guard referenced anything that changed
    }
    // Cascade: every transition guard referencing the old name is rewritten through
    // setTransitionGuard, inside the same Transaction, so one undo restores the name and all
    // guards. renameIdentifierInSource already skips hooks, string literals, unparseable
    // guards and partial matches (`count` vs `counter`).
    // Collected first, applied after: setTransitionGuard mutates the vector being walked.
    QVector<QPair<quint64, QString>> rewrites;
    for (const Transition& transition : doc->machine().transitions) {
        const QString rewritten = expr::renameIdentifierInSource(transition.guard, before, event.name);
        if (rewritten != transition.guard) {
            rewrites.push_back({transition.id, rewritten});
        }
    }
    for (const auto& [transitionId, guard] : rewrites) {
        doc->setTransitionGuard(transitionId, guard);
    }
}

void SetContextTypeCommand::execute(const events::SetContextTypeRequested& event,
                                     ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findContextVariable(event.id) == nullptr) {
        return;
    }
    doc->setContextType(event.id, event.type);
}

void SetContextInitialValueCommand::execute(const events::SetContextInitialValueRequested& event,
                                             ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findContextVariable(event.id) == nullptr) {
        return;
    }
    doc->setContextInitialValue(event.id, event.initialValue);
}

void DeleteContextVariableCommand::execute(const events::DeleteContextVariableRequested& event,
                                            ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findContextVariable(event.id) == nullptr) {
        return;
    }
    doc->deleteContextVariable(event.id);
}

void SetContextCustomTypeNameCommand::execute(const events::SetContextCustomTypeNameRequested& event,
                                               ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findContextVariable(event.id) == nullptr) {
        return;
    }
    doc->setContextCustomTypeName(event.id, event.customTypeName);
}

void SetExternalHeadersCommand::execute(const events::SetExternalHeadersRequested& event,
                                         ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->setExternalHeaders(event.headers);
}

void AddStructDefinitionCommand::execute(const events::AddStructDefinitionRequested& event,
                                          ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    doc->addStructDefinition(event.definition);
}

void SetStructDefinitionCommand::execute(const events::SetStructDefinitionRequested& event,
                                          ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findStructDefinition(event.id) == nullptr) {
        return;
    }
    doc->setStructDefinition(event.id, event.definition);
}

void DeleteStructDefinitionCommand::execute(const events::DeleteStructDefinitionRequested& event,
                                             ordo::core::CommandContext& context) {
    if (detail::simulating(context)) {
        return;
    }
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->findStructDefinition(event.id) == nullptr) {
        return;
    }
    doc->deleteStructDefinition(event.id);
}

}  // namespace app
