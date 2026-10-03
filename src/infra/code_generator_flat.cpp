#include "infra/code_generator_internal.h"

namespace app {

namespace {

void appendRuntimeExitSwitch(QString& text, const GenModel& model, const Machine& machine, const QString& indent) {
    bool anyExit = false;
    for (const State& state : machine.states) {
        for (const QString& exitAction : state.exitActions) {
            anyExit = anyExit || !exitAction.trimmed().isEmpty();
        }
    }
    if (!anyExit) {
        return;
    }
    text += indent + QStringLiteral("switch (state_) {  // leaving via the root still exits the current state\n");
    for (const State& state : machine.states) {
        QStringList exits;
        for (const QString& exitAction : state.exitActions) {
            const QString trimmed = exitAction.trimmed();
            if (!trimmed.isEmpty()) {
                exits.push_back(trimmed);
            }
        }
        if (exits.isEmpty()) {
            continue;
        }
        text += indent + QStringLiteral("    case %1State::%2:\n").arg(model.machinePascal, stateIdent(model, state.id));
        for (const QString& exitAction : exits) {
            appendAction(text, model, indent + QStringLiteral("        "), exitAction);
        }
        text += indent + QStringLiteral("        break;\n");
    }
    text += indent + QStringLiteral("    default:\n");
    text += indent + QStringLiteral("        break;\n");
    text += indent + QStringLiteral("}\n");
}

// Firing body for one (from-state, event) group: candidates in document order,
// the first whose guard passes fires and returns; none passing is a no-op.
// Firing order: cancelInvocations, source exit actions, transition action,
// target entry actions, transitionTo, re-arm. `bareIdentifiers` (the payload)
// is in scope for the guard and the transition's own action only, never for
// entry/exit actions.
void appendCoreEventBody(QString& text, const GenModel& model, const Machine& machine,
                          const QVector<const Transition*>& candidates,
                          const QString& baseIndent = QStringLiteral("            "),
                          const QSet<QString>& bareIdentifiers = QSet<QString>()) {
    for (const Transition* transition : candidates) {
        const QString guard = transition->guard.trimmed();
        const bool targetless = transition->to == 0;
        const bool isInternalSelf = (transition->from != 0 && transition->from == transition->to && !transition->reenter);
        const bool internal = targetless || isInternalSelf;
        const bool guarded = !guard.isEmpty();
        const QString bodyIndent = guarded ? baseIndent + QStringLiteral("    ") : baseIndent;

        if (guarded) {
            text += baseIndent +
                    QStringLiteral("if (%1) {\n").arg(guardCondition(model, guard, /*negated=*/false, bareIdentifiers));
        }
        if (isInternalSelf) {
            text += bodyIndent +
                    QStringLiteral("// internal self-transition (reenter == false): action only, skip exit/entry sequences\n");
        }
        // An internal firing never leaves the state: no exit, no cancellation.
        if (!internal) {
            if (model.hasInvoke) {
                text += bodyIndent + QStringLiteral("cancelInvocations();\n");
            }
            for (const State& state : machine.states) {
                if (state.id != transition->from) {
                    continue;
                }
                for (const QString& exitAction : state.exitActions) {
                    const QString trimmedExit = exitAction.trimmed();
                    if (!trimmedExit.isEmpty()) {
                        appendAction(text, model, bodyIndent, trimmedExit);
                    }
                }
                break;
            }
        }
        const QString action = transition->action.trimmed();
        if (!action.isEmpty()) {
            appendAction(text, model, bodyIndent, action, bareIdentifiers);
        }
        if (internal) {
            // Internal: action only -- no entry actions, transitionTo or re-arm.
            if (model.hasAlways || model.hasRaise) {
                text += bodyIndent + QStringLiteral("checkAlwaysTransitions();\n");
            }
            text += bodyIndent + QStringLiteral("return;\n");
            if (guarded) {
                text += baseIndent + QStringLiteral("}\n");
            }
            continue;
        }
        for (const State& state : machine.states) {
            if (state.id != transition->to) {
                continue;
            }
            for (const QString& entryAction : state.entryActions) {
                const QString trimmedEntry = entryAction.trimmed();
                if (trimmedEntry.isEmpty()) {
                    continue;
                }
                appendAction(text, model, bodyIndent, trimmedEntry);
            }
            break;
        }
        text += bodyIndent +
                QStringLiteral("transitionTo(%1State::%2);\n").arg(model.machinePascal, stateIdent(model, transition->to));
        if (model.hasScheduler) {
            text += bodyIndent + QStringLiteral("armDelayedTransitions();\n");
        }
        if (model.hasInvoke) {
            text += bodyIndent + QStringLiteral("armInvocations();\n");
        }
        if (model.hasAlways || model.hasRaise) {
            text += bodyIndent + QStringLiteral("checkAlwaysTransitions();\n");
        }
        text += bodyIndent + QStringLiteral("return;\n");
        if (guarded) {
            text += baseIndent + QStringLiteral("}\n");
        }
    }
}

// Root (machine-level `on`) candidates, tried after the current state's own
// declined. The exited state is only known at run time, hence the switch.
void appendCoreRootFallback(QString& text, const GenModel& model, const Machine& machine,
                             const QVector<const Transition*>& rootCandidates,
                             const QSet<QString>& bareIdentifiers = QSet<QString>()) {
    text += QStringLiteral(
        "        // Root fallback (machine-level `on`): reached only when the current\n"
        "        // state's own candidates above did not fire -- XState v5's event\n"
        "        // bubbling collapsed to one hop, matching the designer's simulator.\n");
    for (const Transition* transition : rootCandidates) {
        const QString guard = transition->guard.trimmed();
        const bool targetless = transition->to == 0;
        const bool guarded = !guard.isEmpty();
        const QString bodyIndent = guarded ? QStringLiteral("            ") : QStringLiteral("        ");
        if (guarded) {
            text += QStringLiteral("        if (%1) {\n").arg(guardCondition(model, guard, /*negated=*/false, bareIdentifiers));
        }
        if (!targetless) {
            // Cancel before exit actions, as in appendCoreEventBody.
            if (model.hasInvoke) {
                text += bodyIndent + QStringLiteral("cancelInvocations();\n");
            }
            appendRuntimeExitSwitch(text, model, machine, bodyIndent);
        }
        const QString action = transition->action.trimmed();
        if (!action.isEmpty()) {
            appendAction(text, model, bodyIndent, action, bareIdentifiers);
        }
        if (!targetless) {
            for (const State& state : machine.states) {
                if (state.id != transition->to) {
                    continue;
                }
                for (const QString& entryAction : state.entryActions) {
                    const QString trimmedEntry = entryAction.trimmed();
                    if (!trimmedEntry.isEmpty()) {
                        appendAction(text, model, bodyIndent, trimmedEntry);
                    }
                }
                break;
            }
            text += bodyIndent + QStringLiteral("transitionTo(%1State::%2);\n")
                                      .arg(model.machinePascal, stateIdent(model, transition->to));
            if (model.hasScheduler) {
                text += bodyIndent + QStringLiteral("armDelayedTransitions();\n");
            }
            if (model.hasInvoke) {
                text += bodyIndent + QStringLiteral("armInvocations();\n");
            }
        }
        if (model.hasAlways || model.hasRaise) {
            text += bodyIndent + QStringLiteral("checkAlwaysTransitions();\n");
        }
        text += bodyIndent + QStringLiteral("return;\n");
        if (guarded) {
            text += QStringLiteral("        }\n");
        }
    }
}

// armDelayedTransitions()/armRootDelayedTransitions(); only when hasScheduler.
void appendCoreArmMethods(QString& text, const GenModel& model, const Machine& machine) {
    text += QStringLiteral(
        "    // Arms every one of the CURRENT state's outgoing pure-delayed transitions\n"
        "    // (blank event, delayMs > 0) -- called once for the initial state at\n"
        "    // construction-site request time and again after every transitionTo() that\n"
        "    // lands on a new state (event-driven or delayed-fire alike) -- the same\n"
        "    // \"always a wholesale re-arm on state entry\" contract as the designer's\n"
        "    // simulator. Rather than cancelling an in-flight timer when the state\n"
        "    // changes before it fires, each fire lambda re-checks \"is the machine still\n"
        "    // in the source state\" itself as its first line -- a stale callback that\n"
        "    // fires after the state already moved on is a silent no-op, behaviorally\n"
        "    // equivalent to cancellation. The guard (if any) is evaluated at FIRE time,\n"
        "    // not arm time, exactly like the simulator. Every fire lambda captures\n"
        "    // `this`, so this core must outlive every callback the scheduler still\n"
        "    // holds.\n");
    text += QStringLiteral("    void armDelayedTransitions() {\n");
    if (!model.delayedBySource.isEmpty()) {
        text += QStringLiteral("        switch (state_) {\n");
        for (const State& state : machine.states) {
            const auto it = model.delayedBySource.constFind(state.id);
            if (it == model.delayedBySource.constEnd()) {
                continue;
            }
            text += QStringLiteral("            case %1State::%2:\n").arg(model.machinePascal, stateIdent(model, state.id));
            for (const Transition* transition : it.value()) {
                const bool targetless = transition->to == 0;
                const bool isInternalSelf = (transition->from != 0 && transition->from == transition->to && !transition->reenter);
                const bool internal = targetless || isInternalSelf;
                const QString guard = transition->guard.trimmed();
                const auto tt = expr::parseTimeTrigger(transition->event);
                const int effectiveDelay = tt.ok ? tt.durationMs : transition->delayMs;
                const bool isPeriodic = transition->periodic || (tt.ok && tt.isEvery());
                text += QStringLiteral("                scheduler_.get().scheduleAfter(%1, [this] {\n")
                            .arg(effectiveDelay);
                text += QStringLiteral("                    if (state_ != %1State::%2) return;  // superseded by "
                                        "another transition already\n")
                            .arg(model.machinePascal, stateIdent(model, state.id));
                if (!guard.isEmpty()) {
                    text += QStringLiteral("                    if (%1) return;\n")
                                .arg(guardCondition(model, guard, /*negated=*/true));
                }
                // Same firing order as appendCoreEventBody.
                if (!internal) {
                    if (model.hasInvoke) {
                        text += QStringLiteral("                    cancelInvocations();\n");
                    }
                    for (const State& state : machine.states) {
                        if (state.id != transition->from) {
                            continue;
                        }
                        for (const QString& exitAction : state.exitActions) {
                            const QString trimmedExit = exitAction.trimmed();
                            if (!trimmedExit.isEmpty()) {
                                appendAction(text, model, QStringLiteral("                    "), trimmedExit);
                            }
                        }
                        break;
                    }
                }
                const QString action = transition->action.trimmed();
                if (!action.isEmpty()) {
                    appendAction(text, model, QStringLiteral("                    "), action);
                }
                if (internal) {
                    // Internal: the state stays; re-armed only if periodic.
                    if (model.hasAlways || model.hasRaise) {
                        text += QStringLiteral("                    checkAlwaysTransitions();\n");
                    }
                    if (isPeriodic) {
                        text += QStringLiteral("                    armDelayedTransitions();\n");
                    }
                    text += QStringLiteral("                });\n");
                    continue;
                }
                for (const State& destState : machine.states) {
                    if (destState.id != transition->to) {
                        continue;
                    }
                    for (const QString& entryAction : destState.entryActions) {
                        const QString trimmedEntry = entryAction.trimmed();
                        if (trimmedEntry.isEmpty()) {
                            continue;
                        }
                        appendAction(text, model, QStringLiteral("                    "), trimmedEntry);
                    }
                    break;
                }
                text += QStringLiteral("                    transitionTo(%1State::%2);\n")
                            .arg(model.machinePascal, stateIdent(model, transition->to));
                text += QStringLiteral("                    armDelayedTransitions();\n");
                if (model.hasInvoke) {
                    text += QStringLiteral("                    armInvocations();\n");
                }
                if (model.hasAlways || model.hasRaise) {
                    text += QStringLiteral("                    checkAlwaysTransitions();\n");
                }
                text += QStringLiteral("                });\n");
            }
            text += QStringLiteral("                break;\n");
        }
        text += QStringLiteral("            default:\n                break;\n        }\n");
    }
    text += QStringLiteral("    }\n\n");

    // Root `after` transitions: armed once by the owner, never on state entry,
    // and valid in any state, so no source-state check.
    const QVector<const Transition*> rootDelayed = model.delayedBySource.value(0);
    if (rootDelayed.isEmpty()) {
        return;
    }
    text += QStringLiteral(
        "    // The machine's own root `after` countdowns: armed ONCE by the owner and\n"
        "    // never re-armed on state entry -- a root countdown is valid in whatever\n"
        "    // state the machine is in, so its fire lambda carries no source-state\n"
        "    // check, only the guard (evaluated at fire time).\n");
    text += QStringLiteral("    void armRootDelayedTransitions() {\n");
    for (const Transition* transition : rootDelayed) {
        const bool targetless = transition->to == 0;
        const QString guard = transition->guard.trimmed();
        const auto tt = expr::parseTimeTrigger(transition->event);
        const int effectiveDelay = tt.ok ? tt.durationMs : transition->delayMs;
        const bool isPeriodic = transition->periodic || (tt.ok && tt.isEvery());
        text += QStringLiteral("        scheduler_.get().scheduleAfter(%1, [this] {\n").arg(effectiveDelay);
        if (!guard.isEmpty()) {
            text += QStringLiteral("            if (%1) return;\n").arg(guardCondition(model, guard, /*negated=*/true));
        }
        if (!targetless) {
            if (model.hasInvoke) {
                text += QStringLiteral("            cancelInvocations();\n");
            }
            appendRuntimeExitSwitch(text, model, machine, QStringLiteral("            "));
        }
        const QString action = transition->action.trimmed();
        if (!action.isEmpty()) {
            appendAction(text, model, QStringLiteral("            "), action);
        }
        if (!targetless) {
            for (const State& destState : machine.states) {
                if (destState.id != transition->to) {
                    continue;
                }
                for (const QString& entryAction : destState.entryActions) {
                    const QString trimmedEntry = entryAction.trimmed();
                    if (!trimmedEntry.isEmpty()) {
                        appendAction(text, model, QStringLiteral("            "), trimmedEntry);
                    }
                }
                break;
            }
            text += QStringLiteral("            transitionTo(%1State::%2);\n")
                        .arg(model.machinePascal, stateIdent(model, transition->to));
            text += QStringLiteral("            armDelayedTransitions();\n");
            if (model.hasInvoke) {
                text += QStringLiteral("            armInvocations();\n");
            }
        }
        if (model.hasAlways || model.hasRaise) {
            text += QStringLiteral("            checkAlwaysTransitions();\n");
        }
        if (isPeriodic) {
            text += QStringLiteral("            armRootDelayedTransitions();\n");
        }
        text += QStringLiteral("        });\n");
    }
    text += QStringLiteral("    }\n\n");
}

// cancelInvocations()/armInvocations(); only when hasInvoke. One switch-based
// method per direction, called from every exit/entry site.
void appendCoreInvokeMethods(QString& text, const GenModel& model, const Machine& machine) {
    text += QStringLiteral(
        "    // Requests cooperative cancellation of the CURRENT state's own invoke,\n"
        "    // if it has one -- called from every route that leaves a state (event-\n"
        "    // driven, delayed, or the root fallback), the same \"one shared\n"
        "    // switch, called from everywhere\" shape a Scheduler's own\n"
        "    // armDelayedTransitions() uses for delayed transitions (see that\n"
        "    // method's own comment when this machine declares one). A no-op for\n"
        "    // a state with no invoke (the default case), and harmless even when\n"
        "    // the invocation already completed on its own (the\n"
        "    // stop_source's token has no more listeners by then) -- see\n"
        "    // %1Invocations's own header comment (%2_hooks.h) for why the\n"
        "    // implementation is never obligated to observe this promptly: the\n"
        "    // fire-time re-check inside armInvocations()'s own completion lambdas\n"
        "    // below is the actual, only reliable backstop.\n")
        .arg(model.machinePascal, model.machineNs);
    text += QStringLiteral("    void cancelInvocations() {\n");
    if (!model.invokeEffectiveIdsByStateId.isEmpty()) {
        text += QStringLiteral("        switch (state_) {\n");
        for (const State& state : machine.states) {
            const auto effIds = model.invokeEffectiveIdsByStateId.value(state.id);
            if (effIds.isEmpty()) {
                continue;
            }
            text += QStringLiteral("            case %1State::%2:\n").arg(model.machinePascal, stateIdent(model, state.id));
            for (const QString& effId : effIds) {
                text += QStringLiteral("                %1.request_stop();\n").arg(invokeStopSourceMember(effId));
            }
            text += QStringLiteral("                break;\n");
        }
        text += QStringLiteral("            default:\n                break;\n        }\n");
    }
    text += QStringLiteral("    }\n\n");

    text += QStringLiteral(
        "    // Starts the CURRENT state's own invoke, if it has one -- called once\n"
        "    // for the initial state at construction-site request time (see\n"
        "    // register%1()'s own comment, %2_bootstrap.h) and again after every\n"
        "    // transitionTo() that lands on a new state, the same \"wholesale re-arm\n"
        "    // on state entry\" contract a Scheduler's own armDelayedTransitions()\n"
        "    // uses (see that method's own comment when this machine declares\n"
        "    // one). A fresh std::stop_source per entry (never reused across\n"
        "    // entries, even into the SAME state) is what makes a stale\n"
        "    // cancellation request from a PREVIOUS entry unable to leak into this\n"
        "    // one. Each completion lambda's FIRST line re-checks \"is the machine\n"
        "    // still in the invoking state\" -- the fire-time re-check that makes a\n"
        "    // late completion (one that arrives after the state has already been\n"
        "    // left some other way) a silent no-op, the second line of defense\n"
        "    // alongside cancelInvocations()'s own cooperative request above.\n"
        "    // `output`/`error` are the reserved payload identifiers bound as this\n"
        "    // lambda's own parameter, in scope for THIS state's onDone/onError\n"
        "    // transition alone -- XState v5 firing order applies exactly as it does for\n"
        "    // an ordinary event (exit(source) -> transition action ->\n"
        "    // entry(target) -> transitionTo(), actions strictly before the state\n"
        "    // change).\n")
        .arg(model.machinePascal, model.machineNs);
    text += QStringLiteral("    void armInvocations() {\n");
    if (!model.invokeEffectiveIdsByStateId.isEmpty()) {
        text += QStringLiteral("        switch (state_) {\n");
        for (const State& state : machine.states) {
            const auto effIds = model.invokeEffectiveIdsByStateId.value(state.id);
            if (effIds.isEmpty()) {
                continue;
            }
            const QString stateEnumLiteral = QStringLiteral("%1State::%2").arg(model.machinePascal, stateIdent(model, state.id));
            text += QStringLiteral("            case %1:\n").arg(stateEnumLiteral);
            for (const QString& effectiveId : effIds) {
                const QString stopMember = invokeStopSourceMember(effectiveId);
                const QString outputSpelling = contextTypeSpelling(model.invokeOutputTypeByEffectiveId.value(effectiveId));
                text += QStringLiteral("                %1 = std::stop_source{};\n").arg(stopMember);
                text += QStringLiteral("                invocations_.get().%1(\n").arg(invokeHookMethodName(effectiveId));
                text += QStringLiteral("                    %1.get_token(),\n").arg(stopMember);
                text += QStringLiteral("                    [this](%1 output) {\n").arg(outputSpelling);
                text += QStringLiteral("                        if (state_ != %1) return;\n").arg(stateEnumLiteral);
                QVector<const Transition*> matchingDone;
                for (const Transition* t : model.onDoneByState.value(state.id)) {
                    if (t->event == QStringLiteral("done.invoke.") + effectiveId) {
                        matchingDone.push_back(t);
                    }
                }
                appendCoreEventBody(text, model, machine, matchingDone,
                                     QStringLiteral("                        "), QSet<QString>{QStringLiteral("output")});
                text += QStringLiteral("                    },\n");
                text += QStringLiteral("                    [this](const std::string& error) {\n");
                text += QStringLiteral("                        if (state_ != %1) return;\n").arg(stateEnumLiteral);
                QVector<const Transition*> matchingError;
                for (const Transition* t : model.onErrorByState.value(state.id)) {
                    if (t->event == QStringLiteral("error.platform.") + effectiveId) {
                        matchingError.push_back(t);
                    }
                }
                appendCoreEventBody(text, model, machine, matchingError,
                                     QStringLiteral("                        "), QSet<QString>{QStringLiteral("error")});
                text += QStringLiteral("                    });\n");
            }
            text += QStringLiteral("                break;\n");
        }
        text += QStringLiteral("            default:\n                break;\n        }\n");
    }
    text += QStringLiteral("    }\n\n");
}

void appendFlatAlwaysMethods(QString& text, const GenModel& model, const Machine& machine) {
    if (!model.hasAlways && !model.hasRaise) {
        return;
    }
    if (model.hasAlways) {
        QVector<quint64> fromOrder;
        QHash<quint64, QVector<const Transition*>> byFrom;
        QVector<const Transition*> rootCandidates;
        for (const Transition& transition : machine.transitions) {
            if (!transition.isAlways()) {
                continue;
            }
            if (transition.from == 0) {
                rootCandidates.push_back(&transition);
                continue;
            }
            if (!byFrom.contains(transition.from)) {
                fromOrder.push_back(transition.from);
            }
            byFrom[transition.from].push_back(&transition);
        }

        text += QStringLiteral(
            "    // Evaluates eventless (always) transitions from the current state (or root fallback).\n"
            "    // Returns true if a transition was fired, false otherwise.\n"
            "    bool stepAlwaysTransitions() {\n");

        if (!fromOrder.isEmpty()) {
            text += QStringLiteral("        switch (state_) {\n");
            for (quint64 fromId : fromOrder) {
                text += QStringLiteral("            case %1State::%2: {\n")
                            .arg(model.machinePascal, stateIdent(model, fromId));
                const QVector<const Transition*>& candidates = byFrom.value(fromId);
                for (const Transition* transition : candidates) {
                    const QString guard = transition->guard.trimmed();
                    const bool targetless = transition->to == 0;
                    const bool isInternalSelf = (transition->from != 0 && transition->from == transition->to && !transition->reenter);
                    const bool internal = targetless || isInternalSelf;
                    const bool guarded = !guard.isEmpty();
                    const QString baseIndent = QStringLiteral("                ");
                    const QString bodyIndent = guarded ? baseIndent + QStringLiteral("    ") : baseIndent;

                    if (guarded) {
                        text += baseIndent + QStringLiteral("if (%1) {\n").arg(guardCondition(model, guard, /*negated=*/false));
                    }
                    if (!internal) {
                        if (model.hasInvoke) {
                            text += bodyIndent + QStringLiteral("cancelInvocations();\n");
                        }
                        for (const State& state : machine.states) {
                            if (state.id != transition->from) {
                                continue;
                            }
                            for (const QString& exitAction : state.exitActions) {
                                const QString trimmedExit = exitAction.trimmed();
                                if (!trimmedExit.isEmpty()) {
                                    appendAction(text, model, bodyIndent, trimmedExit);
                                }
                            }
                            break;
                        }
                    }
                    const QString action = transition->action.trimmed();
                    if (!action.isEmpty()) {
                        appendAction(text, model, bodyIndent, action);
                    }
                    if (internal) {
                        text += bodyIndent + QStringLiteral("return true;\n");
                        if (guarded) {
                            text += baseIndent + QStringLiteral("}\n");
                        }
                        continue;
                    }
                    for (const State& state : machine.states) {
                        if (state.id != transition->to) {
                            continue;
                        }
                        for (const QString& entryAction : state.entryActions) {
                            const QString trimmedEntry = entryAction.trimmed();
                            if (!trimmedEntry.isEmpty()) {
                                appendAction(text, model, bodyIndent, trimmedEntry);
                            }
                        }
                        break;
                    }
                    text += bodyIndent + QStringLiteral("transitionTo(%1State::%2);\n")
                                .arg(model.machinePascal, stateIdent(model, transition->to));
                    if (model.hasScheduler) {
                        text += bodyIndent + QStringLiteral("armDelayedTransitions();\n");
                    }
                    if (model.hasInvoke) {
                        text += bodyIndent + QStringLiteral("armInvocations();\n");
                    }
                    text += bodyIndent + QStringLiteral("return true;\n");
                    if (guarded) {
                        text += baseIndent + QStringLiteral("}\n");
                    }
                }
                text += QStringLiteral("                break;\n            }\n");
            }
            text += QStringLiteral("            default:\n                break;\n        }\n");
        }

        if (!rootCandidates.isEmpty()) {
            text += QStringLiteral("        // Root fallback always transitions\n");
            for (const Transition* transition : rootCandidates) {
                const QString guard = transition->guard.trimmed();
                const bool targetless = transition->to == 0;
                const bool isInternalSelf = (transition->from != 0 && transition->from == transition->to && !transition->reenter);
                const bool internal = targetless || isInternalSelf;
                const bool guarded = !guard.isEmpty();
                const QString baseIndent = QStringLiteral("        ");
                const QString bodyIndent = guarded ? QStringLiteral("            ") : QStringLiteral("        ");

                if (guarded) {
                    text += baseIndent + QStringLiteral("if (%1) {\n").arg(guardCondition(model, guard, /*negated=*/false));
                }
                if (!internal) {
                    if (model.hasInvoke) {
                        text += bodyIndent + QStringLiteral("cancelInvocations();\n");
                    }
                    appendRuntimeExitSwitch(text, model, machine, bodyIndent);
                }
                const QString action = transition->action.trimmed();
                if (!action.isEmpty()) {
                    appendAction(text, model, bodyIndent, action);
                }
                if (internal) {
                    text += bodyIndent + QStringLiteral("return true;\n");
                    if (guarded) {
                        text += baseIndent + QStringLiteral("}\n");
                    }
                    continue;
                }
                for (const State& state : machine.states) {
                    if (state.id != transition->to) {
                        continue;
                    }
                    for (const QString& entryAction : state.entryActions) {
                        const QString trimmedEntry = entryAction.trimmed();
                        if (!trimmedEntry.isEmpty()) {
                            appendAction(text, model, bodyIndent, trimmedEntry);
                        }
                    }
                    break;
                }
                text += bodyIndent + QStringLiteral("transitionTo(%1State::%2);\n")
                            .arg(model.machinePascal, stateIdent(model, transition->to));
                if (model.hasScheduler) {
                    text += bodyIndent + QStringLiteral("armDelayedTransitions();\n");
                }
                if (model.hasInvoke) {
                    text += bodyIndent + QStringLiteral("armInvocations();\n");
                }
                text += bodyIndent + QStringLiteral("return true;\n");
                if (guarded) {
                    text += baseIndent + QStringLiteral("}\n");
                }
            }
        }

        text += QStringLiteral("        return false;\n    }\n\n");
    }

    if (model.hasAlways && !model.hasRaise) {
        text += QStringLiteral(
            "    // Microstep quiescence loop for eventless (always) transitions (up to 100 steps).\n"
            "    void checkAlwaysTransitions() {\n"
            "        constexpr int kMaxAlwaysSteps = 100;\n"
            "        int stepCount = 0;\n"
            "        while (stepCount < kMaxAlwaysSteps) {\n"
            "            if (!stepAlwaysTransitions()) {\n"
            "                break;\n"
            "            }\n"
            "            ++stepCount;\n"
            "        }\n"
            "    }\n\n");
    } else {
        text += QStringLiteral(
            "    // Microstep quiescence loop for %1 (up to 100 steps).\n"
            "    void checkAlwaysTransitions() {\n"
            "        if (processingMicrosteps_) {\n"
            "            return;\n"
            "        }\n"
            "        processingMicrosteps_ = true;\n"
            "        constexpr int kMaxMicrosteps = 100;\n"
            "        int stepCount = 0;\n"
            "        while (stepCount < kMaxMicrosteps) {\n")
            .arg(model.hasAlways ? QStringLiteral("always transitions and internal event queue")
                                 : QStringLiteral("internal event queue"));
        if (model.hasAlways) {
            text += QStringLiteral(
                "            if (stepAlwaysTransitions()) {\n"
                "                ++stepCount;\n"
                "                continue;\n"
                "            }\n");
        }
        if (model.hasRaise) {
            text += QStringLiteral(
                "            if (!internalQueue_.empty()) {\n"
                "                const Event event = internalQueue_.front();\n"
                "                internalQueue_.pop_front();\n"
                "                dispatchInternal(event);\n"
                "                ++stepCount;\n"
                "                continue;\n"
                "            }\n");
        }
        text += QStringLiteral(
            "            break;\n"
            "        }\n"
            "        processingMicrosteps_ = false;\n"
            "    }\n\n");
    }
}



}  // namespace

GeneratedFile buildCoreFile(const Machine& machine, const GenModel& model) {
    QString text = bannerLines(QStringLiteral("%1Core -- the machine's whole transition contract, ordo-free.")
                                    .arg(model.machinePascal));
    // <stop_token> only when hasInvoke.
    QString extraHeaders;
    if (model.hasRaise) {
        extraHeaders += QStringLiteral("#include <deque>\n");
    }
    if (model.hasTypes) {
        extraHeaders += QStringLiteral("#include \"%1_types.h\"\n").arg(model.machineNs);
    }
    text += QStringLiteral("#pragma once\n\n%1#include <functional>\n%2#include <utility>\n\n#include \"%3_hooks.h\"\n"
                            "#include \"%3_state.h\"\n\n")
                .arg(extraHeaders, model.hasInvoke ? QStringLiteral("#include <stop_token>\n") : QString(), model.machineNs);
    text += openNamespace(model.nsJoined) + QStringLiteral("\n");

    text += QStringLiteral(
                "// Standalone transition core: the C++ standard library only -- no ordo, no\n"
                "// Qt -- so a plain C++ owner can embed it with no kernel at all.\n"
                "// %1_agent.h's %2StateAgent is one such owner: it holds a core and\n"
                "// republishes its state changes as events::StateChanged facts.\n"
                "//\n"
                "// Every firing rule lives HERE and nowhere else -- XState v5 order\n"
                "// (exit(source) -> transition action -> entry(target)), first candidate\n"
                "// whose guard passes fires, all guards failing is a silent no-op, a\n"
                "// targetless transition runs its action only (no exit/entry, no state\n"
                "// change).\n"
                "// %1_commands.h's Commands delegate to these methods rather than\n"
                "// repeating them.\n"
                "//\n"
                "// `guards`/`actions`%3 are held by reference and must outlive this\n"
                "// core.\n")
                .arg(model.machineNs, model.machinePascal, extraRefComment(model));
    if (model.hasInvoke) {
        text += QStringLiteral(
            "//\n"
            "// Invoke: cancelInvocations()/armInvocations() are the ONE place every\n"
            "// invoking state's std::stop_source is requested-to-stop / freshly\n"
            "// issued and its service started -- called from every route that\n"
            "// leaves/enters a state, the same \"one shared switch, called from\n"
            "// everywhere\" shape a Scheduler's own armDelayedTransitions() uses for\n"
            "// delayed transitions (see that method's own comment when this machine\n"
            "// declares one). See %1Invocations's own header comment (%2_hooks.h)\n"
            "// for the threading contract onDone/onError must honor.\n")
            .arg(model.machinePascal, model.machineNs);
    }
    if (model.hasContext) {
        text += QStringLiteral(
            "//\n"
            "// Extended state: this core OWNS the one Context instance (declared in\n"
            "// the hooks header) and is the only writer of it -- an assign-form action\n"
            "// compiled to a plain assignment below, and whatever a Context&-taking\n"
            "// action hook chooses to change. Expression guards are compiled inline\n"
            "// against it, so they need no hook at all.\n");
    }
    text += QStringLiteral("class %1Core {\n").arg(model.machinePascal);
    text += QStringLiteral("public:\n");
    text += QStringLiteral("    using StateChangedCallback = std::function<void(%1State previous, %1State next)>;\n\n")
                .arg(model.machinePascal);

    if (model.hasRaise) {
        text += QStringLiteral("    enum class Event {\n");
        for (int i = 0; i < model.eventOrder.size(); ++i) {
            const QString rawEvent = model.eventOrder.at(i);
            const QString eventIdent = model.eventEnumIdentByRaw.value(rawEvent);
            const QString comma = (i + 1 < model.eventOrder.size()) ? QStringLiteral(",") : QString();
            text += QStringLiteral("        %1%2\n").arg(eventIdent, comma);
        }
        text += QStringLiteral("    };\n\n");
        text += QStringLiteral("    void raise(Event event) { internalQueue_.push_back(event); }\n");
        text += QStringLiteral("    const std::deque<Event>& internalQueue() const { return internalQueue_; }\n\n");
        text += QStringLiteral("    void dispatchInternal(Event event) {\n");
        text += QStringLiteral("        switch (event) {\n");
        for (const QString& rawEvent : model.eventOrder) {
            const QString payloadType = model.eventPayloadTypeByRaw.value(rawEvent);
            text += QStringLiteral("            case Event::%1:\n")
                        .arg(model.eventEnumIdentByRaw.value(rawEvent));
            if (!payloadType.isEmpty()) {
                text += QStringLiteral("                %1({});\n")
                            .arg(model.eventMethodByRaw.value(rawEvent));
            } else {
                text += QStringLiteral("                %1();\n")
                            .arg(model.eventMethodByRaw.value(rawEvent));
            }
            text += QStringLiteral("                break;\n");
        }
        text += QStringLiteral("            default:\n");
        text += QStringLiteral("                break;\n");
        text += QStringLiteral("        }\n");
        text += QStringLiteral("    }\n\n");
    }

    text += QStringLiteral("    %1Core(%1Guards& guards, %1Actions& actions%2)\n")
                .arg(model.machinePascal, extraCtorParams(model));
    if (model.hasAlways || model.hasRaise) {
        text += QStringLiteral("        : guards_(guards), actions_(actions)%1 {\n"
                               "        checkAlwaysTransitions();\n"
                               "    }\n\n").arg(extraCtorInit(model));
    } else {
        text += QStringLiteral("        : guards_(guards), actions_(actions)%1 {}\n\n").arg(extraCtorInit(model));
    }

    text += QStringLiteral("    %1State state() const { return state_; }\n\n").arg(model.machinePascal);

    if (model.hasContext) {
        text += contextAccessorLines();
    }

    text += QStringLiteral(
        "    // Invoked on every COMMITTED transition -- never for a guard-rejected\n"
        "    // candidate and never for a targetless firing, which changes no state.\n");
    text += QStringLiteral("    void setOnStateChanged(StateChangedCallback callback) { onStateChanged_ = "
                            "std::move(callback); }\n\n");

    text += QStringLiteral(
        "    // The one place a state change is committed and broadcast; every event\n"
        "    // method below funnels through it. Unguarded on purpose -- an owner that\n"
        "    // needs to force a state (a hard reset, an external authority) calls this\n"
        "    // directly, exactly as the kernel agent's transitionTo() always has.\n");
    text += QStringLiteral("    void transitionTo(%1State next) {\n").arg(model.machinePascal);
    text += QStringLiteral("        const %1State previous = state_;\n").arg(model.machinePascal);
    text += QStringLiteral("        state_ = next;\n");
    text += QStringLiteral("        if (onStateChanged_) {\n");
    text += QStringLiteral("            onStateChanged_(previous, next);\n");
    text += QStringLiteral("        }\n");
    text += QStringLiteral("    }\n\n");

    if (model.hasScheduler) {
        appendCoreArmMethods(text, model, machine);
    }
    if (model.hasInvoke) {
        appendCoreInvokeMethods(text, model, machine);
    }
    if (model.hasAlways || model.hasRaise) {
        appendFlatAlwaysMethods(text, model, machine);
    }

    for (const QString& rawEvent : model.eventOrder) {
        const QString methodName = model.eventMethodByRaw.value(rawEvent);

        struct FlatCandidate {
            const Transition* transition;
            int specificity;
            int originalOrder;
        };

        auto collectMatches = [&](quint64 fromId) {
            std::vector<FlatCandidate> matches;
            int order = 0;
            for (const Transition& transition : machine.transitions) {
                const QString ev = transition.event.trimmed();
                if (transition.from == fromId && !ev.isEmpty() && eventMatches(ev, rawEvent)) {
                    matches.push_back({
                        .transition = &transition,
                        .specificity = eventDescriptorSpecificity(ev),
                        .originalOrder = order++,
                    });
                }
            }
            std::stable_sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
                if (a.specificity != b.specificity) {
                    return a.specificity > b.specificity;
                }
                return a.originalOrder < b.originalOrder;
            });
            QVector<const Transition*> res;
            res.reserve(static_cast<int>(matches.size()));
            for (const auto& m : matches) {
                res.push_back(m.transition);
            }
            return res;
        };

        // One if/else-if branch per source state, in document order.
        QVector<quint64> fromOrder;
        QHash<quint64, QVector<const Transition*>> byFrom;
        for (const State& state : machine.states) {
            QVector<const Transition*> candidates = collectMatches(state.id);
            if (!candidates.isEmpty()) {
                fromOrder.push_back(state.id);
                byFrom.insert(state.id, candidates);
            }
        }
        const QVector<const Transition*> rootCandidates = collectMatches(0);

        const QString payloadType = model.eventPayloadTypeByRaw.value(rawEvent);
        const bool hasPayload = !payloadType.isEmpty();
        const QString cppPayload = hasPayload ? cppPayloadTypeSpelling(payloadType) : QString();
        const QSet<QString> bareIdentifiers = hasPayload ? QSet<QString>{QStringLiteral("event")} : QSet<QString>{};

        text += QStringLiteral("    // \"%1\" -- one method per distinct event on the diagram.\n").arg(rawEvent);
        if (hasPayload) {
            text += QStringLiteral("    void %1(const %2& event) {\n").arg(methodName, cppPayload);
        } else {
            text += QStringLiteral("    void %1() {\n").arg(methodName);
        }
        for (int i = 0; i < fromOrder.size(); ++i) {
            const quint64 fromId = fromOrder.at(i);
            const QString kw = (i == 0) ? QStringLiteral("if") : QStringLiteral("} else if");
            text += QStringLiteral("        %1 (state_ == %2State::%3) {\n")
                        .arg(kw, model.machinePascal, stateIdent(model, fromId));
            appendCoreEventBody(text, model, machine, byFrom.value(fromId),
                                QStringLiteral("            "), bareIdentifiers);
        }
        if (!fromOrder.isEmpty()) {
            text += QStringLiteral("        }\n");
        }
        if (!rootCandidates.isEmpty()) {
            appendCoreRootFallback(text, model, machine, rootCandidates, bareIdentifiers);
        }
        text += QStringLiteral(
                    "        // no transition carries \"%1\" out of any other state, or every\n"
                    "        // candidate transition's guard failed above -- silent return (FSM\n"
                    "        // semantics: an event with no fireable transition is simply\n"
                    "        // ignored, matching the designer's simulator).\n")
                    .arg(rawEvent);
        text += QStringLiteral("    }\n\n");
    }

    text += QStringLiteral("private:\n");
    text += QStringLiteral("    std::reference_wrapper<%1Guards> guards_;\n").arg(model.machinePascal);
    text += QStringLiteral("    std::reference_wrapper<%1Actions> actions_;\n").arg(model.machinePascal);
    if (model.hasScheduler) {
        text += QStringLiteral("    std::reference_wrapper<%1Scheduler> scheduler_;\n").arg(model.machinePascal);
    }
    if (model.hasInvoke) {
        text += QStringLiteral("    std::reference_wrapper<%1Invocations> invocations_;\n").arg(model.machinePascal);
        // One std::stop_source per service (effective invoke id).
        for (const QString& effectiveId : model.invokeIdOrder) {
            text += QStringLiteral("    std::stop_source %1;\n").arg(invokeStopSourceMember(effectiveId));
        }
    }
    if (model.hasContext) {
        text += contextMemberLine();
    }
    if (!model.defaultStateIdent.isEmpty()) {
        text += QStringLiteral("    %1State state_ = %1State::%2;  // the machine's initial state\n")
                    .arg(model.machinePascal, model.defaultStateIdent);
    } else {
        text += QStringLiteral("    %1State state_{};\n").arg(model.machinePascal);
    }
    text += QStringLiteral("    StateChangedCallback onStateChanged_;\n");
    if (model.hasRaise) {
        text += QStringLiteral("    std::deque<Event> internalQueue_;\n");
        text += QStringLiteral("    bool processingMicrosteps_ = false;\n");
    }
    text += QStringLiteral("};\n\n");
    text += closeNamespace(model.nsJoined);

    return GeneratedFile{.relativePath = model.machineNs + QStringLiteral("_core.h"), .content = text};
}

}  // namespace app
