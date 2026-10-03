#include "infra/node_code_projector.h"

#include <QSet>
#include <QStringList>

#include "infra/code_generator_internal.h"
#include "infra/expression.h"

namespace app {

namespace {

Machine normalizeMachine(const Machine& machineIn) {
    Machine machine = machineIn;
    for (Transition& transition : machine.transitions) {
        if (transition.from == 0 && transition.machineSelf && machine.initialStateId != 0) {
            transition.to = machine.initialStateId;
            transition.machineSelf = false;
        }
    }
    return machine;
}

}  // namespace

NodeCodeProjection projectStateCode(const Machine& machineIn, quint64 stateId, const QString& rootNamespace) {
    const Machine machine = normalizeMachine(machineIn);
    const State* state = findStateById(machine, stateId);
    if (state == nullptr) {
        return {};
    }

    const GenModel model = buildModel(machine, rootNamespace);
    const QString sIdent = stateIdent(model, stateId);
    const QString stateEnumName = model.machinePascal + QStringLiteral("State");
    const QString scopedState = QStringLiteral("%1::%2").arg(stateEnumName, sIdent);

    // 1. State Enum Snippet
    QString enumSnippet;
    enumSnippet += QStringLiteral("// 1. State Enum (%1_state.h)\n").arg(model.machineNs);
    enumSnippet += scopedState;
    if (!state->description.trimmed().isEmpty()) {
        enumSnippet += QStringLiteral("  // %1").arg(state->description.trimmed());
    }

    // 2. Hooks Snippet
    QSet<QString> addedSignatures;
    QStringList hookList;
    const QString actionParam = model.hasContext ? QStringLiteral("Context& ctx") : QString();
    const QString guardParam = model.hasContext ? QStringLiteral("const Context& ctx") : QString();

    // Entry actions
    for (const QString& action : state->entryActions) {
        if (actionIsHook(model, action, false)) {
            const QString id = model.actions.idOf(action.trimmed());
            if (!id.isEmpty()) {
                const QString sig = QStringLiteral("virtual void %1(%2) = 0;").arg(id, actionParam);
                if (!addedSignatures.contains(sig)) {
                    addedSignatures.insert(sig);
                    hookList.push_back(sig);
                }
            }
        }
    }

    // Exit actions
    for (const QString& action : state->exitActions) {
        if (actionIsHook(model, action, false)) {
            const QString id = model.actions.idOf(action.trimmed());
            if (!id.isEmpty()) {
                const QString sig = QStringLiteral("virtual void %1(%2) = 0;").arg(id, actionParam);
                if (!addedSignatures.contains(sig)) {
                    addedSignatures.insert(sig);
                    hookList.push_back(sig);
                }
            }
        }
    }

    // Outgoing transitions (guards & actions)
    for (const Transition& transition : machine.transitions) {
        if (transition.from == stateId) {
            if (guardIsHook(model, transition.guard, false)) {
                const QString id = model.guards.idOf(transition.guard.trimmed());
                if (!id.isEmpty()) {
                    const QString sig = QStringLiteral("virtual bool %1(%2) = 0;").arg(id, guardParam);
                    if (!addedSignatures.contains(sig)) {
                        addedSignatures.insert(sig);
                        hookList.push_back(sig);
                    }
                }
            }
            if (actionIsHook(model, transition.action, false)) {
                const QString id = model.actions.idOf(transition.action.trimmed());
                if (!id.isEmpty()) {
                    const QString sig = QStringLiteral("virtual void %1(%2) = 0;").arg(id, actionParam);
                    if (!addedSignatures.contains(sig)) {
                        addedSignatures.insert(sig);
                        hookList.push_back(sig);
                    }
                }
            }
        }
    }

    // Invocations
    if (model.hasInvoke && model.invokeEffectiveIdsByStateId.contains(stateId)) {
        const QStringList effectiveIds = model.invokeEffectiveIdsByStateId.value(stateId);
        for (const QString& effectiveId : effectiveIds) {
            const QString methodName = invokeHookMethodName(effectiveId);
            const ContextType outType = model.invokeOutputTypeByEffectiveId.value(effectiveId, ContextType::Int);
            const QString outTypeStr = contextTypeSpelling(outType);
            const QString sig = QStringLiteral(
                                    "virtual void %1(std::stop_token stopToken, std::function<void(%2)> onDone, "
                                    "std::function<void(const std::string&)> onError) = 0;")
                                    .arg(methodName, outTypeStr);
            if (!addedSignatures.contains(sig)) {
                addedSignatures.insert(sig);
                hookList.push_back(sig);
            }
        }
    }

    // Scheduler
    if (model.hasScheduler && model.delayedBySource.contains(stateId)) {
        const QString sig = QStringLiteral("virtual void scheduleAfter(int delayMs, std::function<void()> fire) = 0;");
        if (!addedSignatures.contains(sig)) {
            addedSignatures.insert(sig);
            hookList.push_back(sig);
        }
    }

    QString hooksSnippet;
    hooksSnippet += QStringLiteral("// 2. Hook Interfaces (%1_hooks.h)\n").arg(model.machineNs);
    if (hookList.isEmpty()) {
        hooksSnippet += QStringLiteral("// (No hooks declared for this state)");
    } else {
        hooksSnippet += hookList.join(QStringLiteral("\n"));
    }

    // 3. Core Handler Snippet
    QString coreSnippet;
    coreSnippet += QStringLiteral("// 3. Transition Logic (%1_core.h)\n").arg(model.machineNs);
    coreSnippet += QStringLiteral("case %1State::%2: {\n").arg(model.machinePascal, sIdent);

    // Entry actions note
    if (!state->entryActions.isEmpty()) {
        coreSnippet += QStringLiteral("    // Entry Actions:\n");
        for (const QString& entryAct : state->entryActions) {
            if (!entryAct.trimmed().isEmpty()) {
                appendAction(coreSnippet, model, QStringLiteral("    "), entryAct.trimmed());
            }
        }
    }

    bool hasOutgoing = false;
    for (const Transition& transition : machine.transitions) {
        if (transition.from == stateId) {
            hasOutgoing = true;
            const QString ev = transition.event.trimmed();
            QString evLabel = ev.isEmpty()
                                  ? (transition.delayMs > 0 ? QStringLiteral("[after %1ms]").arg(transition.delayMs)
                                                            : QStringLiteral("[always]"))
                                  : QStringLiteral("Event::%1").arg(transition.event);
            coreSnippet += QStringLiteral("    // On %1\n").arg(evLabel);

            const QString guard = transition.guard.trimmed();
            const bool hasGuard = !guard.isEmpty();
            QString indent = QStringLiteral("    ");
            if (hasGuard) {
                coreSnippet += indent + QStringLiteral("if (%1) {\n").arg(guardCondition(model, guard, false));
                indent += QStringLiteral("    ");
            }

            // Exit actions on source
            for (const QString& exitAct : state->exitActions) {
                if (!exitAct.trimmed().isEmpty()) {
                    appendAction(coreSnippet, model, indent, exitAct.trimmed());
                }
            }

            // Transition action
            const QString act = transition.action.trimmed();
            if (!act.isEmpty()) {
                appendAction(coreSnippet, model, indent, act);
            }

            // Destination
            if (transition.to != 0) {
                coreSnippet += indent + QStringLiteral("transitionTo(%1State::%2);\n")
                                          .arg(model.machinePascal, stateIdent(model, transition.to));
                if (const State* dstState = findStateById(machine, transition.to)) {
                    for (const QString& entryAct : dstState->entryActions) {
                        if (!entryAct.trimmed().isEmpty()) {
                            appendAction(coreSnippet, model, indent, entryAct.trimmed());
                        }
                    }
                }
            }
            coreSnippet += indent + QStringLiteral("return true;\n");
            if (hasGuard) {
                coreSnippet += QStringLiteral("    }\n");
            }
        }
    }

    if (!hasOutgoing) {
        coreSnippet += QStringLiteral("    // (No outgoing transitions)\n");
    }
    coreSnippet += QStringLiteral("    break;\n");
    coreSnippet += QStringLiteral("}");

    // Combined Full Snippet
    const QString fullSnippet = enumSnippet + QStringLiteral("\n\n") + hooksSnippet + QStringLiteral("\n\n") + coreSnippet;

    return NodeCodeProjection{
        .stateEnumSnippet = enumSnippet,
        .hooksSnippet = hooksSnippet,
        .coreHandlerSnippet = coreSnippet,
        .fullSnippet = fullSnippet,
    };
}

NodeCodeProjection projectTransitionCode(const Machine& machineIn, quint64 transitionId, const QString& rootNamespace) {
    const Machine machine = normalizeMachine(machineIn);
    const Transition* targetTr = nullptr;
    for (const Transition& t : machine.transitions) {
        if (t.id == transitionId) {
            targetTr = &t;
            break;
        }
    }
    if (targetTr == nullptr) {
        return {};
    }

    const GenModel model = buildModel(machine, rootNamespace);
    const QString sourceName = targetTr->from == 0 ? QStringLiteral("Root") : stateIdent(model, targetTr->from);
    const QString targetName = targetTr->to == 0 ? QStringLiteral("Targetless") : stateIdent(model, targetTr->to);

    QString evDesc = targetTr->event.trimmed();
    if (evDesc.isEmpty()) {
        evDesc = targetTr->delayMs > 0 ? QStringLiteral("[after %1ms]").arg(targetTr->delayMs) : QStringLiteral("[always]");
    }

    // 1. State Enum / Routing Header Snippet
    QString enumSnippet;
    enumSnippet += QStringLiteral("// 1. Transition Header\n");
    enumSnippet += QStringLiteral("// Event: %1\n").arg(evDesc);
    enumSnippet += QStringLiteral("// Source: %1State::%2 -> Target: %1State::%3")
                       .arg(model.machinePascal, sourceName, targetName);

    // 2. Hooks Snippet
    QStringList hookList;
    const QString guardParam = model.hasContext ? QStringLiteral("const Context& ctx") : QString();
    const QString actionParam = model.hasContext ? QStringLiteral("Context& ctx") : QString();

    if (guardIsHook(model, targetTr->guard, false)) {
        const QString id = model.guards.idOf(targetTr->guard.trimmed());
        if (!id.isEmpty()) {
            hookList.push_back(QStringLiteral("virtual bool %1(%2) = 0;").arg(id, guardParam));
        }
    }
    if (actionIsHook(model, targetTr->action, false)) {
        const QString id = model.actions.idOf(targetTr->action.trimmed());
        if (!id.isEmpty()) {
            hookList.push_back(QStringLiteral("virtual void %1(%2) = 0;").arg(id, actionParam));
        }
    }

    QString hooksSnippet;
    hooksSnippet += QStringLiteral("// 2. Hook Interfaces (%1_hooks.h)\n").arg(model.machineNs);
    if (hookList.isEmpty()) {
        hooksSnippet += QStringLiteral("// (No hooks declared for this transition)");
    } else {
        hooksSnippet += hookList.join(QStringLiteral("\n"));
    }

    // 3. Core Handler Snippet
    QString coreSnippet;
    coreSnippet += QStringLiteral("// 3. Dispatch & Execution Fragment (%1_core.h)\n").arg(model.machineNs);

    const bool hasSource = (targetTr->from != 0);
    if (hasSource) {
        coreSnippet += QStringLiteral("if (state_ == %1State::%2) {\n").arg(model.machinePascal, sourceName);
    } else {
        coreSnippet += QStringLiteral("// Root transition:\n");
    }

    const QString indent = hasSource ? QStringLiteral("    ") : QStringLiteral("");
    const QString guard = targetTr->guard.trimmed();
    const bool hasGuard = !guard.isEmpty();
    QString bodyIndent = indent;
    if (hasGuard) {
        coreSnippet += indent + QStringLiteral("if (%1) {\n").arg(guardCondition(model, guard, false));
        bodyIndent += QStringLiteral("    ");
    }

    // Source exit actions
    if (const State* srcState = findStateById(machine, targetTr->from)) {
        for (const QString& exitAct : srcState->exitActions) {
            if (!exitAct.trimmed().isEmpty()) {
                appendAction(coreSnippet, model, bodyIndent, exitAct.trimmed());
            }
        }
    }

    // Transition action
    if (!targetTr->action.trimmed().isEmpty()) {
        appendAction(coreSnippet, model, bodyIndent, targetTr->action.trimmed());
    }

    // Target transition
    if (targetTr->to != 0) {
        coreSnippet += bodyIndent + QStringLiteral("transitionTo(%1State::%2);\n").arg(model.machinePascal, targetName);
        if (const State* dstState = findStateById(machine, targetTr->to)) {
            for (const QString& entryAct : dstState->entryActions) {
                if (!entryAct.trimmed().isEmpty()) {
                    appendAction(coreSnippet, model, bodyIndent, entryAct.trimmed());
                }
            }
        }
    }
    coreSnippet += bodyIndent + QStringLiteral("return true;\n");

    if (hasGuard) {
        coreSnippet += indent + QStringLiteral("}\n");
    }
    if (hasSource) {
        coreSnippet += QStringLiteral("}\n");
    }

    const QString fullSnippet = enumSnippet + QStringLiteral("\n\n") + hooksSnippet + QStringLiteral("\n\n") + coreSnippet;

    return NodeCodeProjection{
        .stateEnumSnippet = enumSnippet,
        .hooksSnippet = hooksSnippet,
        .coreHandlerSnippet = coreSnippet,
        .fullSnippet = fullSnippet,
    };
}

}  // namespace app
