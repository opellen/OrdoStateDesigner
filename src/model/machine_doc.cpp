#include "model/machine_doc.h"

#include <utility>

#include "model/machine_events.h"
#include "model/machine_journal.h"

namespace app {

namespace {

// The first child that remains under `parentId`, in document order, once `excludeId`
// is reparented away or deleted; 0 if none.
quint64 firstRemainingChild(const QVector<State>& states, quint64 parentId, quint64 excludeId) {
    for (const State& state : states) {
        if (state.id != excludeId && state.parentId == parentId) {
            return state.id;
        }
    }
    return 0;
}

}  // namespace

quint64 MachineDocAgent::addState(QPointF pos, quint64 parentId) {
    const quint64 id = machine_.nextId++;
    State state{
        .id = id,
        .name = QStringLiteral("State%1").arg(id),
        .kind = StateKind::Normal,
        .pos = pos,
        .entryActions = {},
        .exitActions = {},
        .parentId = parentId,
    };
    machine_.states.push_back(state);
    if (journal_) {
        journal_->stateAdded(state);
    }
    context().send(events::StateAdded{.state = state});
    // A childless new parent adopts the new state as its first child, as its own
    // journaled op right after StateAddOp.
    if (parentId != 0) {
        const State* parent = findState(parentId);
        if (parent != nullptr && parent->initialChildId == 0) {
            setInitialChild(parentId, id);
        }
    }
    return id;
}

void MachineDocAgent::renameState(quint64 id, const QString& name) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const QString before = state.name;
            state.name = name;
            if (journal_) {
                journal_->stateRenamed(id, before, name);
            }
            break;
        }
    }
    context().send(events::StateRenamed{.id = id, .name = name});
}

void MachineDocAgent::setStateKind(quint64 id, StateKind kind) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const StateKind before = state.kind;
            state.kind = kind;
            if (journal_) {
                journal_->stateKindChanged(id, before, kind);
            }
            break;
        }
    }
    context().send(events::StateKindChanged{.id = id, .kind = kind});
}

void MachineDocAgent::moveState(quint64 id, QPointF pos) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const QPointF before = state.pos;
            state.pos = pos;
            if (journal_) {
                journal_->stateMoved(id, before, pos);
            }
            break;
        }
    }
    context().send(events::StateMoved{.id = id, .pos = pos});
}

void MachineDocAgent::setEntryActions(quint64 id, QStringList entryActions) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const QStringList before = state.entryActions;
            state.entryActions = entryActions;
            if (journal_) {
                journal_->entryActionsChanged(id, before, entryActions);
            }
            break;
        }
    }
    context().send(events::EntryActionsChanged{.stateId = id, .entryActions = std::move(entryActions)});
}

void MachineDocAgent::setExitActions(quint64 id, QStringList exitActions) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const QStringList before = state.exitActions;
            state.exitActions = exitActions;
            if (journal_) {
                journal_->exitActionsChanged(id, before, exitActions);
            }
            break;
        }
    }
    context().send(events::ExitActionsChanged{.stateId = id, .exitActions = std::move(exitActions)});
}

void MachineDocAgent::setDescription(quint64 id, const QString& description) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const QString before = state.description;
            state.description = description;
            if (journal_) {
                journal_->descriptionChanged(id, before, description);
            }
            break;
        }
    }
    context().send(events::DescriptionChanged{.stateId = id, .description = description});
}

void MachineDocAgent::setTags(quint64 id, QStringList tags) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const QStringList before = state.tags;
            state.tags = tags;
            if (journal_) {
                journal_->tagsChanged(id, before, tags);
            }
            break;
        }
    }
    context().send(events::TagsChanged{.stateId = id, .tags = std::move(tags)});
}

void MachineDocAgent::setHistoryDeep(quint64 id, bool deep) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const bool before = state.historyDeep;
            state.historyDeep = deep;
            if (journal_) {
                journal_->historyDeepChanged(id, before, deep);
            }
            break;
        }
    }
    context().send(events::HistoryDeepChanged{.stateId = id, .deep = deep});
}

void MachineDocAgent::setInvokeSrc(quint64 id, const QString& src) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const QString before = state.invokeSrc;
            state.invokeSrc = src;
            if (!state.invocations.isEmpty()) {
                state.invocations[0].src = src;
            }
            if (journal_) {
                journal_->invokeSrcChanged(id, before, src);
            }
            break;
        }
    }
    context().send(events::InvokeSrcChanged{.stateId = id, .src = src});
}

void MachineDocAgent::setInvokeId(quint64 id, const QString& invokeId) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const QString before = state.invokeId;
            state.invokeId = invokeId;
            if (!state.invocations.isEmpty()) {
                state.invocations[0].id = invokeId;
            }
            if (journal_) {
                journal_->invokeIdChanged(id, before, invokeId);
            }
            break;
        }
    }
    context().send(events::InvokeIdChanged{.stateId = id, .invokeId = invokeId});
}

void MachineDocAgent::setInvokeOutputType(quint64 id, ContextType type) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const ContextType before = state.invokeOutputType;
            state.invokeOutputType = type;
            if (!state.invocations.isEmpty()) {
                state.invocations[0].outputType = type;
            }
            if (journal_) {
                journal_->invokeOutputTypeChanged(id, before, type);
            }
            break;
        }
    }
    context().send(events::InvokeOutputTypeChanged{.stateId = id, .type = type});
}

void MachineDocAgent::setInvocations(quint64 id, const QVector<Invocation>& invocations) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const QVector<Invocation> before = state.invocations;
            state.invocations = invocations;
            if (!invocations.isEmpty()) {
                state.invokeSrc = invocations.first().src;
                state.invokeId = invocations.first().id;
                state.invokeOutputType = invocations.first().outputType;
            } else {
                state.invokeSrc.clear();
                state.invokeId.clear();
                state.invokeOutputType = ContextType::Int;
            }
            if (journal_) {
                journal_->invocationsChanged(id, before, invocations);
            }
            break;
        }
    }
    context().send(events::InvocationsChanged{.stateId = id, .invocations = invocations});
}

void MachineDocAgent::setStateColor(quint64 id, ElementColor color) {
    for (auto& state : machine_.states) {
        if (state.id == id) {
            const ElementColor before = state.color;
            state.color = color;
            if (journal_) {
                journal_->stateColorChanged(id, before, color);
            }
            break;
        }
    }
    context().send(events::StateColorChanged{.id = id, .color = color});
}

void MachineDocAgent::setInitialChild(quint64 stateId, quint64 childId) {
    for (auto& state : machine_.states) {
        if (state.id == stateId) {
            const quint64 before = state.initialChildId;
            state.initialChildId = childId;
            if (journal_) {
                journal_->initialChildChanged(stateId, before, childId);
            }
            break;
        }
    }
    context().send(events::InitialChildChanged{.stateId = stateId, .initialChildId = childId});
}

void MachineDocAgent::reparentState(quint64 id, quint64 newParentId) {
    const State* moving = findState(id);
    const quint64 oldParentId = moving ? moving->parentId : 0;

    // If the old parent's initialChildId pointed at `id`, move it to the first remaining
    // sibling (or clear it), as its own journaled op BEFORE the reparent itself.
    if (oldParentId != 0) {
        const State* oldParent = findState(oldParentId);
        if (oldParent && oldParent->initialChildId == id) {
            setInitialChild(oldParentId, firstRemainingChild(machine_.states, oldParentId, id));
        }
    }

    for (auto& state : machine_.states) {
        if (state.id == id) {
            const quint64 before = state.parentId;
            state.parentId = newParentId;
            if (journal_) {
                journal_->stateReparented(id, before, newParentId);
            }
            break;
        }
    }
    context().send(events::StateReparented{.id = id, .parentId = newParentId});

    // A childless new parent adopts `id` as its first child.
    if (newParentId != 0) {
        const State* newParent = findState(newParentId);
        if (newParent && newParent->initialChildId == 0) {
            setInitialChild(newParentId, id);
        }
    }
}

void MachineDocAgent::setMachineName(const QString& name) {
    const QString before = machine_.name;
    machine_.name = name;
    if (journal_) {
        journal_->machineRenamed(before, name);
    }
    context().send(events::MachineNameChanged{.name = name});
}

void MachineDocAgent::setInitialState(quint64 id) {
    const quint64 before = machine_.initialStateId;
    machine_.initialStateId = id;
    if (journal_) {
        journal_->initialStateChanged(before, id);
    }
    context().send(events::InitialStateChanged{.id = id});
}

quint64 MachineDocAgent::addTransition(quint64 from, quint64 to) {
    const quint64 id = machine_.nextId++;
    Transition transition{
        .id = id,
        .from = from,
        .to = to,
        .event = QString(),
        .guard = QString(),
        .action = QString(),
        .delayMs = 0,
    };
    machine_.transitions.push_back(transition);
    if (journal_) {
        journal_->transitionAdded(transition);
    }
    context().send(events::TransitionAdded{.transition = transition});
    return id;
}

void MachineDocAgent::setTransitionEvent(quint64 id, const QString& event) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const QString before = transition.event;
            transition.event = event;
            if (journal_) {
                journal_->transitionEventChanged(id, before, event);
            }
            break;
        }
    }
    context().send(events::TransitionEventChanged{.id = id, .event = event});
}

void MachineDocAgent::setTransitionGuard(quint64 id, const QString& guard) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const QString before = transition.guard;
            transition.guard = guard;
            if (journal_) {
                journal_->transitionGuardChanged(id, before, guard);
            }
            break;
        }
    }
    context().send(events::TransitionGuardChanged{.id = id, .guard = guard});
}

void MachineDocAgent::setTransitionAction(quint64 id, const QString& action) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const QString before = transition.action;
            transition.action = action;
            if (journal_) {
                journal_->transitionActionChanged(id, before, action);
            }
            break;
        }
    }
    context().send(events::TransitionActionChanged{.id = id, .action = action});
}

void MachineDocAgent::setTransitionDelay(quint64 id, int delayMs) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const int before = transition.delayMs;
            transition.delayMs = delayMs;
            if (journal_) {
                journal_->transitionDelayChanged(id, before, delayMs);
            }
            break;
        }
    }
    context().send(events::TransitionDelayChanged{.id = id, .delayMs = delayMs});
}

void MachineDocAgent::retargetTransition(quint64 id, quint64 from, quint64 to, const QList<quint64>& targets) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const quint64 fromBefore = transition.from;
            const quint64 toBefore = transition.to;
            const QList<quint64> targetsBefore = transition.targets;
            const quint64 resolvedTo = targets.size() > 1 ? targets.first() : (targets.size() == 1 ? targets.first() : to);
            const QList<quint64> storedTargets = targets.size() > 1 ? targets : QList<quint64>{};
            transition.from = from;
            transition.to = resolvedTo;
            transition.targets = storedTargets;
            if (journal_) {
                journal_->transitionRetargeted(id, fromBefore, toBefore, from, resolvedTo, targetsBefore, storedTargets);
            }
            context().send(events::TransitionRetargeted{
                .id = id, .from = from, .to = resolvedTo, .targets = transition.effectiveTargets()});
            break;
        }
    }
}

void MachineDocAgent::setTransitionLabelOffset(quint64 id, QPointF offset) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const QPointF before = transition.labelOffset;
            transition.labelOffset = offset;
            if (journal_) {
                journal_->transitionLabelMoved(id, before, offset);
            }
            break;
        }
    }
    context().send(events::TransitionLabelMoved{.id = id, .offset = offset});
}

// Returns early, before recording a before-image, notifying the journal or
// publishing a fact, when the stored value already equals `ratio`, so an
// unchanged re-layout captures no op.
void MachineDocAgent::setTransitionLabelRatio(quint64 id, std::optional<qreal> ratio) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            if (transition.labelRatio == ratio) {
                return;
            }
            const std::optional<qreal> before = transition.labelRatio;
            transition.labelRatio = ratio;
            if (journal_) {
                journal_->transitionLabelRatioChanged(id, before, ratio);
            }
            context().send(events::TransitionLabelRatioChanged{.id = id, .ratio = ratio});
            break;
        }
    }
}

void MachineDocAgent::setTransitionBendpoints(quint64 id, const QVector<QPointF>& bendpoints) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const QVector<QPointF> before = transition.manualBendpoints;
            transition.manualBendpoints = bendpoints;
            if (journal_) {
                journal_->transitionBendpointsChanged(id, before, bendpoints);
            }
            break;
        }
    }
    context().send(events::TransitionBendpointsChanged{.id = id, .bendpoints = bendpoints});
}

void MachineDocAgent::setTransitionColor(quint64 id, ElementColor color) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const ElementColor before = transition.color;
            transition.color = color;
            if (journal_) {
                journal_->transitionColorChanged(id, before, color);
            }
            break;
        }
    }
    context().send(events::TransitionColorChanged{.id = id, .color = color});
}

void MachineDocAgent::setMachineSelf(quint64 id, bool machineSelf) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const bool before = transition.machineSelf;
            transition.machineSelf = machineSelf;
            if (journal_) {
                journal_->machineSelfChanged(id, before, machineSelf);
            }
            break;
        }
    }
    context().send(events::MachineSelfChanged{.id = id, .machineSelf = machineSelf});
}

void MachineDocAgent::setTransitionReenter(quint64 id, bool reenter) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const bool before = transition.reenter;
            transition.reenter = reenter;
            if (journal_) {
                journal_->transitionReenterChanged(id, before, reenter);
            }
            break;
        }
    }
    context().send(events::TransitionReenterChanged{.id = id, .reenter = reenter});
}

void MachineDocAgent::setTransitionAlways(quint64 id, bool always) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const bool before = transition.always;
            transition.always = always;
            if (journal_) {
                journal_->transitionAlwaysChanged(id, before, always);
            }
            break;
        }
    }
    context().send(events::TransitionAlwaysChanged{.id = id, .always = always});
}

void MachineDocAgent::setTransitionPayloadType(quint64 id, const QString& payloadType) {
    for (auto& transition : machine_.transitions) {
        if (transition.id == id) {
            const QString before = transition.payloadType;
            transition.payloadType = payloadType;
            if (journal_) {
                journal_->transitionPayloadTypeChanged(id, before, payloadType);
            }
            break;
        }
    }
    context().send(events::TransitionPayloadTypeChanged{.id = id, .payloadType = payloadType});
}

void MachineDocAgent::deleteState(quint64 id) {
    // Children first (document order, through this same method), before this state's
    // own transition cascade; undo's reverse iteration then restores the subtree.
    QVector<quint64> directChildren;
    for (const auto& state : machine_.states) {
        if (state.parentId == id) {
            directChildren.push_back(state.id);
        }
    }
    for (quint64 childId : directChildren) {
        deleteState(childId);
    }

    QVector<quint64> cascaded;
    for (const auto& transition : machine_.transitions) {
        if (transition.from == id || transition.to == id) {
            cascaded.push_back(transition.id);
        }
    }
    // Each cascaded removal goes through deleteTransition and so notifies the journal
    // BEFORE this method's own state-deleted notification.
    for (quint64 transitionId : cascaded) {
        deleteTransition(transitionId);
    }

    // Clear Machine::initialStateId BEFORE the state's own removal, so undo's reverse
    // replay restores the state before re-pointing initialStateId at it.
    if (machine_.initialStateId == id) {
        setInitialState(0);
    }

    // Deleting this state may leave its parent's initialChildId dangling; apply the
    // same first-remaining-sibling rule as reparentState.
    {
        const State* existingBeforeRemoval = findState(id);
        const quint64 parentId = existingBeforeRemoval ? existingBeforeRemoval->parentId : 0;
        if (parentId != 0) {
            const State* parent = findState(parentId);
            if (parent && parent->initialChildId == id) {
                setInitialChild(parentId, firstRemainingChild(machine_.states, parentId, id));
            }
        }
    }

    // Erase by index: the journal records where the state sat so undo re-inserts it there.
    int index = -1;
    for (int i = 0; i < machine_.states.size(); ++i) {
        if (machine_.states[i].id == id) {
            index = i;
            break;
        }
    }
    const bool found = index >= 0;
    const State before = found ? machine_.states[index] : State{};
    if (found) {
        machine_.states.removeAt(index);
    }
    if (journal_ && found) {
        journal_->stateDeleted(before, index);
    }
    context().send(events::StateDeleted{.id = id, .cascadedTransitionIds = cascaded});
}

void MachineDocAgent::deleteTransition(quint64 id) {
    // Index-recording erase, as in deleteState.
    int index = -1;
    for (int i = 0; i < machine_.transitions.size(); ++i) {
        if (machine_.transitions[i].id == id) {
            index = i;
            break;
        }
    }
    const bool found = index >= 0;
    const Transition before = found ? machine_.transitions[index] : Transition{};
    if (found) {
        machine_.transitions.removeAt(index);
    }
    if (journal_ && found) {
        journal_->transitionDeleted(before, index);
    }
    context().send(events::TransitionDeleted{.id = id});
}

quint64 MachineDocAgent::addNote(QPointF pos) {
    const quint64 id = machine_.nextId++;
    Note note{.id = id, .pos = pos, .text = QString()};
    machine_.notes.push_back(note);
    if (journal_) {
        journal_->noteAdded(note);
    }
    context().send(events::NoteAdded{.note = note});
    return id;
}

void MachineDocAgent::moveNote(quint64 id, QPointF pos) {
    for (auto& note : machine_.notes) {
        if (note.id == id) {
            const QPointF before = note.pos;
            note.pos = pos;
            if (journal_) {
                journal_->noteMoved(id, before, pos);
            }
            break;
        }
    }
    context().send(events::NoteMoved{.id = id, .pos = pos});
}

void MachineDocAgent::setNoteText(quint64 id, const QString& text) {
    for (auto& note : machine_.notes) {
        if (note.id == id) {
            const QString before = note.text;
            note.text = text;
            if (journal_) {
                journal_->noteTextChanged(id, before, text);
            }
            break;
        }
    }
    context().send(events::NoteTextChanged{.id = id, .text = text});
}

void MachineDocAgent::setNoteColor(quint64 id, ElementColor color) {
    for (auto& note : machine_.notes) {
        if (note.id == id) {
            const ElementColor before = note.color;
            note.color = color;
            if (journal_) {
                journal_->noteColorChanged(id, before, color);
            }
            break;
        }
    }
    context().send(events::NoteColorChanged{.id = id, .color = color});
}

void MachineDocAgent::deleteNote(quint64 id) {
    // Index-recording erase, as in deleteState.
    int index = -1;
    for (int i = 0; i < machine_.notes.size(); ++i) {
        if (machine_.notes[i].id == id) {
            index = i;
            break;
        }
    }
    const bool found = index >= 0;
    const Note before = found ? machine_.notes[index] : Note{};
    if (found) {
        machine_.notes.removeAt(index);
    }
    if (journal_ && found) {
        journal_->noteDeleted(before, index);
    }
    context().send(events::NoteDeleted{.id = id});
}

quint64 MachineDocAgent::addContextVariable() {
    const quint64 id = machine_.nextId++;
    ContextVariable variable{
        .id = id,
        .name = QStringLiteral("var%1").arg(id),
        .type = ContextType::Int,
        .initialValue = QStringLiteral("0"),
    };
    machine_.context.push_back(variable);
    if (journal_) {
        journal_->contextVariableAdded(variable);
    }
    context().send(events::ContextVariableAdded{.variable = variable});
    return id;
}

void MachineDocAgent::renameContextVariable(quint64 id, const QString& name) {
    for (auto& variable : machine_.context) {
        if (variable.id == id) {
            const QString before = variable.name;
            variable.name = name;
            if (journal_) {
                journal_->contextVariableRenamed(id, before, name);
            }
            break;
        }
    }
    context().send(events::ContextVariableRenamed{.id = id, .name = name});
}

void MachineDocAgent::setContextType(quint64 id, ContextType type) {
    for (auto& variable : machine_.context) {
        if (variable.id == id) {
            const ContextType before = variable.type;
            variable.type = type;
            if (journal_) {
                journal_->contextTypeChanged(id, before, type);
            }
            break;
        }
    }
    context().send(events::ContextTypeChanged{.id = id, .type = type});
}

void MachineDocAgent::setContextInitialValue(quint64 id, const QString& initialValue) {
    for (auto& variable : machine_.context) {
        if (variable.id == id) {
            const QString before = variable.initialValue;
            variable.initialValue = initialValue;
            if (journal_) {
                journal_->contextInitialValueChanged(id, before, initialValue);
            }
            break;
        }
    }
    context().send(events::ContextInitialValueChanged{.id = id, .initialValue = initialValue});
}

void MachineDocAgent::deleteContextVariable(quint64 id) {
    // Index-recording erase, as in deleteState.
    int index = -1;
    for (int i = 0; i < machine_.context.size(); ++i) {
        if (machine_.context[i].id == id) {
            index = i;
            break;
        }
    }
    const bool found = index >= 0;
    const ContextVariable before = found ? machine_.context[index] : ContextVariable{};
    if (found) {
        machine_.context.removeAt(index);
    }
    if (journal_ && found) {
        journal_->contextVariableDeleted(before, index);
    }
    context().send(events::ContextVariableDeleted{.id = id});
}

void MachineDocAgent::setContextCustomTypeName(quint64 id, const QString& customTypeName) {
    for (auto& variable : machine_.context) {
        if (variable.id == id) {
            const QString before = variable.customTypeName;
            variable.customTypeName = customTypeName;
            if (journal_) {
                journal_->contextCustomTypeNameChanged(id, before, customTypeName);
            }
            break;
        }
    }
    context().send(events::ContextCustomTypeNameChanged{.id = id, .customTypeName = customTypeName});
}

void MachineDocAgent::setExternalHeaders(const QStringList& headers) {
    const QStringList before = machine_.externalHeaders;
    machine_.externalHeaders = headers;
    if (journal_) {
        journal_->externalHeadersChanged(before, headers);
    }
    context().send(events::ExternalHeadersChanged{.headers = headers});
}

quint64 MachineDocAgent::addStructDefinition(StructDefinition def) {
    if (def.id == 0) {
        def.id = machine_.nextId++;
    }
    if (def.name.isEmpty()) {
        def.name = QStringLiteral("Struct%1").arg(def.id);
    }
    machine_.types.push_back(def);
    if (journal_) {
        journal_->structDefinitionAdded(def);
    }
    context().send(events::StructDefinitionAdded{.definition = def});
    return def.id;
}

void MachineDocAgent::setStructDefinition(quint64 id, const StructDefinition& def) {
    for (auto& t : machine_.types) {
        if (t.id == id) {
            const StructDefinition before = t;
            t = def;
            t.id = id;
            if (journal_) {
                journal_->structDefinitionChanged(before, t);
            }
            break;
        }
    }
    context().send(events::StructDefinitionChanged{.id = id, .definition = def});
}

void MachineDocAgent::deleteStructDefinition(quint64 id) {
    int index = -1;
    for (int i = 0; i < machine_.types.size(); ++i) {
        if (machine_.types[i].id == id) {
            index = i;
            break;
        }
    }
    const bool found = index >= 0;
    const StructDefinition before = found ? machine_.types[index] : StructDefinition{};
    if (found) {
        machine_.types.removeAt(index);
    }
    if (journal_ && found) {
        journal_->structDefinitionDeleted(before, index);
    }
    context().send(events::StructDefinitionDeleted{.id = id});
}

void MachineDocAgent::restoreNote(Note note, int index) {
    const int at = (index < 0 || index > machine_.notes.size()) ? machine_.notes.size() : index;
    machine_.notes.insert(at, note);
    if (journal_) {
        journal_->noteAdded(note);
    }
    context().send(events::NoteAdded{.note = note});
}

void MachineDocAgent::restoreState(State state, int index) {
    // Re-insert at the delete op's recorded position (-1/out of range = append).
    const int at = (index < 0 || index > machine_.states.size()) ? machine_.states.size() : index;
    machine_.states.insert(at, state);
    if (journal_) {
        journal_->stateAdded(state);
    }
    context().send(events::StateAdded{.state = state});
}

void MachineDocAgent::restoreTransition(Transition transition, int index) {
    const int at = (index < 0 || index > machine_.transitions.size()) ? machine_.transitions.size() : index;
    machine_.transitions.insert(at, transition);
    if (journal_) {
        journal_->transitionAdded(transition);
    }
    context().send(events::TransitionAdded{.transition = transition});
}

void MachineDocAgent::restoreContextVariable(ContextVariable variable, int index) {
    // Re-insert at the delete op's recorded position, as in restoreNote.
    const int at = (index < 0 || index > machine_.context.size()) ? machine_.context.size() : index;
    machine_.context.insert(at, variable);
    if (journal_) {
        journal_->contextVariableAdded(variable);
    }
    context().send(events::ContextVariableAdded{.variable = variable});
}

void MachineDocAgent::restoreStructDefinition(StructDefinition def, int index) {
    const int at = (index < 0 || index > machine_.types.size()) ? machine_.types.size() : index;
    machine_.types.insert(at, def);
    if (journal_) {
        journal_->structDefinitionAdded(def);
    }
    context().send(events::StructDefinitionAdded{.definition = def});
}

void MachineDocAgent::restore(Machine machine) {
    // No journal notification: a wholesale restore is not undoable.
    machine_ = std::move(machine);
    context().send(events::MachineSnapshotPublished{.machine = machine_});
}

const State* MachineDocAgent::findState(quint64 id) const {
    for (const auto& state : machine_.states) {
        if (state.id == id) {
            return &state;
        }
    }
    return nullptr;
}

const Transition* MachineDocAgent::findTransition(quint64 id) const {
    for (const auto& transition : machine_.transitions) {
        if (transition.id == id) {
            return &transition;
        }
    }
    return nullptr;
}

const Note* MachineDocAgent::findNote(quint64 id) const {
    for (const auto& note : machine_.notes) {
        if (note.id == id) {
            return &note;
        }
    }
    return nullptr;
}

const ContextVariable* MachineDocAgent::findContextVariable(quint64 id) const {
    for (const auto& variable : machine_.context) {
        if (variable.id == id) {
            return &variable;
        }
    }
    return nullptr;
}

const StructDefinition* MachineDocAgent::findStructDefinition(quint64 id) const {
    for (const auto& def : machine_.types) {
        if (def.id == id) {
            return &def;
        }
    }
    return nullptr;
}

}  // namespace app
