#include "infra/xstate_v5_io.h"

#include <QFile>
#include <QHash>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

#include <cmath>
#include <utility>

#include "infra/expression.h"

namespace app {

namespace {

// Expressions on the wire: export renders the Ast parsed from a guard/assign string, import
// parses it back and prints with expr::print(); crossing the Ast keeps round trips idempotent. `and`/`or`/`not` (operands under `guards`) are
// v5's guard combinators; comparisons (`left`/`right`) and unary minus (`value`) have no
// standard v5 JSON form and use operator symbols, which cannot collide with hook names.
// A literal is a bare JSON primitive; a context reference is `{"type":"context","name":..}`;
// `output`/`error` are `{"type":"event","name":..}`. Bare-identifier guards stay plain name strings.
QString structuralOperator(expr::NodeKind kind) {
    switch (kind) {
        case expr::NodeKind::And:
            return QStringLiteral("and");
        case expr::NodeKind::Or:
            return QStringLiteral("or");
        case expr::NodeKind::Not:
            return QStringLiteral("not");
        case expr::NodeKind::Negate:
            return QStringLiteral("-");
        case expr::NodeKind::Equal:
            return QStringLiteral("==");
        case expr::NodeKind::NotEqual:
            return QStringLiteral("!=");
        case expr::NodeKind::Less:
            return QStringLiteral("<");
        case expr::NodeKind::LessEqual:
            return QStringLiteral("<=");
        case expr::NodeKind::Greater:
            return QStringLiteral(">");
        case expr::NodeKind::GreaterEqual:
            return QStringLiteral(">=");
        case expr::NodeKind::Add:
            return QStringLiteral("+");
        case expr::NodeKind::Subtract:
            return QStringLiteral("-");
        case expr::NodeKind::Multiply:
            return QStringLiteral("*");
        case expr::NodeKind::Divide:
            return QStringLiteral("/");
        case expr::NodeKind::Modulo:
            return QStringLiteral("%");
        default:
            return QString();  // a literal or an identifier: no operator object
    }
}

// One Ast node (and subtree) as JSON; total over the node set, so any expression that
// parses crosses. `payloadName` is "output"/"error" when the enclosing transition is the
// matching onDone/onError row, blank otherwise; a matching Identifier crosses as
// `{"type": "event", "name": ...}` instead of a context reference.
QJsonValue expressionToJson(const expr::Ast& ast, int index, const QString& payloadName) {
    const expr::Node& node = ast.nodes.at(static_cast<size_t>(index));
    switch (node.kind) {
        case expr::NodeKind::BoolLiteral:
            return QJsonValue(node.boolValue);
        case expr::NodeKind::IntLiteral:
            // An integral JSON number, not a double with .0: import reads
            // integralness back as Int.
            return QJsonValue(node.intValue);
        case expr::NodeKind::DoubleLiteral:
            return QJsonValue(std::isfinite(node.doubleValue) ? node.doubleValue : 0.0);
        case expr::NodeKind::StringLiteral:
            return QJsonValue(node.text);
        case expr::NodeKind::Identifier: {
            QJsonObject reference;
            if (!payloadName.isEmpty() && node.text == payloadName) {
                reference[QStringLiteral("type")] = QStringLiteral("event");
            } else {
                reference[QStringLiteral("type")] = QStringLiteral("context");
            }
            reference[QStringLiteral("name")] = node.text;
            return reference;
        }
        case expr::NodeKind::Not: {
            QJsonObject object;
            object[QStringLiteral("type")] = structuralOperator(node.kind);
            object[QStringLiteral("guards")] = QJsonArray{expressionToJson(ast, node.lhs, payloadName)};
            return object;
        }
        case expr::NodeKind::Negate: {
            QJsonObject object;
            object[QStringLiteral("type")] = structuralOperator(node.kind);
            object[QStringLiteral("value")] = expressionToJson(ast, node.lhs, payloadName);
            return object;
        }
        case expr::NodeKind::And:
        case expr::NodeKind::Or: {
            // Always two operands, never a flattened chain: `a && (b && c)` and
            // `(a && b) && c` are different trees. Import still accepts a longer
            // array by left-folding it.
            QJsonObject object;
            object[QStringLiteral("type")] = structuralOperator(node.kind);
            object[QStringLiteral("guards")] =
                QJsonArray{expressionToJson(ast, node.lhs, payloadName), expressionToJson(ast, node.rhs, payloadName)};
            return object;
        }
        case expr::NodeKind::MemberAccess: {
            QJsonObject object;
            object[QStringLiteral("type")] = QStringLiteral("member");
            object[QStringLiteral("object")] = expressionToJson(ast, node.lhs, payloadName);
            object[QStringLiteral("property")] = node.text;
            return object;
        }
        case expr::NodeKind::Call: {
            QJsonObject object;
            object[QStringLiteral("type")] = QStringLiteral("call");
            object[QStringLiteral("callee")] = node.text;
            QJsonArray args;
            if (node.lhs != -1) args.append(expressionToJson(ast, node.lhs, payloadName));
            if (node.rhs != -1) args.append(expressionToJson(ast, node.rhs, payloadName));
            for (int extra : node.extraArgs) {
                args.append(expressionToJson(ast, extra, payloadName));
            }
            object[QStringLiteral("arguments")] = args;
            return object;
        }
        default: {
            QJsonObject object;
            object[QStringLiteral("type")] = structuralOperator(node.kind);
            object[QStringLiteral("left")] = expressionToJson(ast, node.lhs, payloadName);
            object[QStringLiteral("right")] = expressionToJson(ast, node.rhs, payloadName);
            return object;
        }
    }
}

// A guard as v5 sees it: a plain name string for a hook, the structural object for an
// expression. Unparseable text is exported as a plain string so it is not lost, with a
// diagnostic.
QJsonValue guardToJson(const QString& guard, const QString& label, QStringList* diagnostics, const QString& payloadName) {
    const QString trimmed = guard.trimmed();
    if (expr::isBareIdentifier(trimmed)) {
        return QJsonValue(trimmed);
    }
    const expr::ParseResult parsed = expr::parse(trimmed);
    if (!parsed.ok || parsed.ast.root < 0) {
        diagnostics->push_back(QStringLiteral("%1: guard '%2' does not parse (%3) -- written as a plain string, which "
                                               "XState will read as a guard NAME")
                                    .arg(label, trimmed, parsed.message));
        return QJsonValue(trimmed);
    }
    return expressionToJson(parsed.ast, parsed.ast.root, payloadName);
}

// An action as v5 sees it: a plain name string for a hook, v5's action objects for
// sendTo/sendParent/raise/assign. The assign value is an expression in the structural
// form above, since v5 spells it as JavaScript, which JSON cannot carry.
QJsonValue actionToJson(const QString& action, const QString& label, QStringList* diagnostics,
                        const QString& payloadName) {
    const QString trimmed = action.trimmed();
    const expr::SendToForm sendTo = expr::parseSendToForm(trimmed);
    if (sendTo.ok) {
        QJsonObject object;
        object[QStringLiteral("type")] = QStringLiteral("xstate.sendTo");
        QJsonObject eventObj;
        eventObj[QStringLiteral("type")] = sendTo.event;
        QJsonObject params;
        params[QStringLiteral("to")] = sendTo.target;
        params[QStringLiteral("event")] = eventObj;
        object[QStringLiteral("params")] = params;
        return object;
    } else if (!sendTo.message.isEmpty()) {
        diagnostics->push_back(
            QStringLiteral("%1: action '%2' looks like a sendTo but is not one (%3) -- written as a plain "
                           "string, which XState will read as an action NAME")
                .arg(label, trimmed, sendTo.message));
        return QJsonValue(trimmed);
    }

    const expr::SendParentForm sendParent = expr::parseSendParentForm(trimmed);
    if (sendParent.ok) {
        QJsonObject object;
        object[QStringLiteral("type")] = QStringLiteral("xstate.sendParent");
        QJsonObject eventObj;
        eventObj[QStringLiteral("type")] = sendParent.event;
        QJsonObject params;
        params[QStringLiteral("event")] = eventObj;
        object[QStringLiteral("params")] = params;
        return object;
    } else if (!sendParent.message.isEmpty()) {
        diagnostics->push_back(
            QStringLiteral("%1: action '%2' looks like a sendParent but is not one (%3) -- written as a plain "
                           "string, which XState will read as an action NAME")
                .arg(label, trimmed, sendParent.message));
        return QJsonValue(trimmed);
    }

    const expr::RaiseForm raise = expr::parseRaiseForm(trimmed);
    if (raise.ok) {
        QJsonObject object;
        object[QStringLiteral("type")] = QStringLiteral("xstate.raise");
        QJsonObject eventObj;
        eventObj[QStringLiteral("type")] = raise.event;
        QJsonObject params;
        params[QStringLiteral("event")] = eventObj;
        object[QStringLiteral("params")] = params;
        return object;
    } else if (!raise.message.isEmpty()) {
        diagnostics->push_back(
            QStringLiteral("%1: action '%2' looks like a raise but is not one (%3) -- written as a plain "
                           "string, which XState will read as an action NAME")
                .arg(label, trimmed, raise.message));
        return QJsonValue(trimmed);
    }

    const expr::AssignForm form = expr::parseAssignForm(trimmed);
    if (!form.ok) {
        if (!form.message.isEmpty()) {
            // Looked like an assign but is not valid: crosses as a plain name, with a diagnostic.
            diagnostics->push_back(
                QStringLiteral("%1: action '%2' looks like an assign but is not one (%3) -- written as a plain "
                                "string, which XState will read as an action NAME")
                    .arg(label, trimmed, form.message));
        }
        return QJsonValue(trimmed);
    }
    const expr::ParseResult value = expr::parse(form.valueSource);
    if (!value.ok || value.ast.root < 0) {
        diagnostics->push_back(QStringLiteral("%1: assign '%2' has an unparseable value (%3) -- written as a plain "
                                               "string, which XState will read as an action NAME")
                                    .arg(label, trimmed, value.message));
        return QJsonValue(trimmed);
    }
    QJsonObject assignment;
    assignment[form.target] = expressionToJson(value.ast, value.ast.root, payloadName);
    QJsonObject object;
    object[QStringLiteral("type")] = QStringLiteral("xstate.assign");
    object[QStringLiteral("assignment")] = assignment;
    return object;
}

// Entry/exit lists: the per-action mapping in document order. Always payload-free: an
// entry/exit action belongs to a state, so it is never an onDone/onError completion.
QJsonArray actionsToJson(const QStringList& actions, const QString& label, QStringList* diagnostics) {
    QJsonArray array;
    for (int i = 0; i < actions.size(); ++i) {
        array.append(actionToJson(actions.at(i), QStringLiteral("%1[%2]").arg(label).arg(i), diagnostics, QString()));
    }
    return array;
}

// Same type names project_io.cpp writes into `.sdm`; unknown or absent degrades to Int.
QString contextTypeName(ContextType type) {
    switch (type) {
        case ContextType::Bool:
            return QStringLiteral("Bool");
        case ContextType::Double:
            return QStringLiteral("Double");
        case ContextType::String:
            return QStringLiteral("String");
        case ContextType::Object:
            return QStringLiteral("Object");
        case ContextType::Int:
        default:
            return QStringLiteral("Int");
    }
}

ContextType contextTypeFromName(const QString& text) {
    if (text == QStringLiteral("Bool")) return ContextType::Bool;
    if (text == QStringLiteral("Double")) return ContextType::Double;
    if (text == QStringLiteral("String")) return ContextType::String;
    if (text == QStringLiteral("Object")) return ContextType::Object;
    return ContextType::Int;
}

// One context variable's initial value as typed JSON. The literal rules match
// model/sim_agent.cpp's seedContext(); a malformed initialValue degrades to the same
// 0/false rather than refusing the export.
QJsonValue contextValueToJson(const ContextVariable& variable) {
    const QString trimmed = variable.initialValue.trimmed();
    switch (variable.type) {
        case ContextType::Bool:
            return QJsonValue(trimmed.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
        case ContextType::Int:
            return QJsonValue(static_cast<qint64>(trimmed.toLongLong()));  // integral number, never 3.0
        case ContextType::Double: {
            const double value = trimmed.toDouble();
            return QJsonValue(std::isfinite(value) ? value : 0.0);  // JSON has no inf/nan spelling
        }
        case ContextType::Object: {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
            if (!doc.isNull() && doc.isObject()) {
                return doc.object();
            }
            return QJsonObject();
        }
        case ContextType::String:
        default:
            // Not trimmed: leading/trailing spaces are part of the value.
            return QJsonValue(variable.initialValue);
    }
}

// v5's top-level `context` object plus the `meta.ordo.context` schema, built in one pass.
// The object alone loses the authored order (QJsonObject keys are alphabetical) and the
// declared type (Int 0 and Double 0 are the same JSON), so the schema is
// `[{ name, type }, ...]` in document order; values live only in `context`.
QJsonObject contextToJson(const Machine& machine, QJsonArray* schema, QStringList* diagnostics) {
    QJsonObject values;
    QSet<QString> seen;
    for (const ContextVariable& variable : machine.context) {
        const QString name = variable.name.trimmed();
        if (name.isEmpty()) {
            diagnostics->push_back(
                QStringLiteral("context variable %1 has a blank name -- XState keys context by name, dropped")
                    .arg(variable.id));
            continue;
        }
        if (seen.contains(name)) {
            // A validator Error in-app; the export cannot emit the key twice.
            diagnostics->push_back(
                QStringLiteral("two context variables share the name '%1' -- only the first crosses, dropped")
                    .arg(name));
            continue;
        }
        seen.insert(name);
        values[name] = contextValueToJson(variable);
        QJsonObject entry;
        entry[QStringLiteral("name")] = name;
        entry[QStringLiteral("type")] = contextTypeName(variable.type);
        schema->append(entry);
    }
    return values;
}

// Same structural test as the validator's payloadBindingForTransition(): from == the
// invoking state and event == "done.invoke.<id>" / "error.platform.<id>" off
// effectiveInvokeId(). Returns "output"/"error", or blank for no payload in scope.
QString payloadNameForTransition(const Machine& machine, const Transition& transition) {
    if (transition.from == 0) {
        return QString();
    }
    for (const State& state : machine.states) {
        if (state.id != transition.from) {
            continue;
        }
        const QString effectiveId = effectiveInvokeId(state);
        if (effectiveId.isEmpty()) {
            return QString();
        }
        if (transition.event == QStringLiteral("done.invoke.") + effectiveId) {
            return QStringLiteral("output");
        }
        if (transition.event == QStringLiteral("error.platform.") + effectiveId) {
            return QStringLiteral("error");
        }
        return QString();
    }
    return QString();  // dangling `from`; the validator flags it
}

// One transition's XState object, shared by the `on` and `after` emitters. `target` is
// omitted for a targetless or dangling `to`. The single action becomes a one-element
// `actions` array. `label` names the transition in diagnostics; `payloadName` (computed
// once by the caller) is passed to both guard and action. `isInvokeCompletion` rows type
// their payload via the state's invokeOutputType, so meta.ordo.payloadType is withheld.
QJsonObject transitionToJson(const Transition& transition, const QHash<quint64, QString>& nameById,
                             const QString& machineName, const QString& label, QStringList* diagnostics,
                             const QString& payloadName, bool isInvokeCompletion = false) {
    QJsonObject object;
    if (transition.from == 0 && transition.machineSelf) {
        // XState's root self-target: the machine's own id.
        object[QStringLiteral("target")] = QStringLiteral("#") + machineName;
    } else if (transition.isMultiTarget()) {
        QJsonArray targetArray;
        for (quint64 tid : transition.targets) {
            if (nameById.contains(tid)) {
                targetArray.append(nameById.value(tid));
            }
        }
        if (targetArray.size() > 1) {
            object[QStringLiteral("target")] = targetArray;
        } else if (targetArray.size() == 1) {
            object[QStringLiteral("target")] = targetArray.first().toString();
        }
    } else if (transition.to != 0 && nameById.contains(transition.to)) {
        object[QStringLiteral("target")] = nameById.value(transition.to);
    }
    if (transition.reenter) {
        object[QStringLiteral("reenter")] = true;
    }
    if (!transition.guard.trimmed().isEmpty()) {
        object[QStringLiteral("guard")] = guardToJson(transition.guard, label, diagnostics, payloadName);
    }
    if (!transition.action.trimmed().isEmpty()) {
        object[QStringLiteral("actions")] = QJsonArray{actionToJson(transition.action, label, diagnostics, payloadName)};
    }
    // meta.ordo (label offset, labelRatio, payloadType) is emitted only when one of
    // them is set. payloadType is withheld for an onDone/onError row.
    const bool writePayloadType = !isInvokeCompletion && !transition.payloadType.trimmed().isEmpty();
    if (!transition.labelOffset.isNull() || transition.labelRatio.has_value() || writePayloadType) {
        QJsonObject ordo;
        if (!transition.labelOffset.isNull()) {
            ordo[QStringLiteral("labelOffsetX")] = transition.labelOffset.x();
            ordo[QStringLiteral("labelOffsetY")] = transition.labelOffset.y();
        }
        if (transition.labelRatio.has_value()) {
            ordo[QStringLiteral("labelRatio")] = *transition.labelRatio;
        }
        if (writePayloadType) {
            ordo[QStringLiteral("payloadType")] = transition.payloadType;
        }
        QJsonObject meta;
        meta[QStringLiteral("ordo")] = ordo;
        object[QStringLiteral("meta")] = meta;
    }
    return object;
}

// One source's transitions as v5 `on`/`after`/`always` members written into `into`, for
// states and the machine level (`sourceId` 0 = root). `sourceLabel` names the source in
// diagnostics. `excludeInvokeCompletions` is true for state-level calls: onDone/onError
// rows belong to `invoke` and emitting both would round-trip into duplicates.
void emitTransitionGroups(const Machine& machine, quint64 sourceId, const QString& sourceLabel,
                           const QHash<quint64, QString>& nameById, QJsonObject* into,
                           QStringList* diagnostics, bool excludeInvokeCompletions = false) {
    QHash<QString, QJsonArray> onByEvent;
    QStringList onEventOrder;
    QHash<int, QJsonArray> afterByDelay;
    QList<int> afterDelayOrder;
    QJsonArray alwaysTransitions;
    for (const Transition& transition : machine.transitions) {
        if (transition.from != sourceId) {
            continue;
        }
        if (excludeInvokeCompletions && invokePayloadKindForTransition(machine, transition) != InvokePayloadKind::None) {
            continue;  // routed to invoke.onDone/onError instead
        }
        const QString payloadName = payloadNameForTransition(machine, transition);
        const QString event = transition.event.trimmed();
        if (transition.isAlways()) {
            alwaysTransitions.append(
                transitionToJson(transition, nameById, machine.name,
                                 QStringLiteral("always transition from %1").arg(sourceLabel),
                                 diagnostics, payloadName));
        } else if (!event.isEmpty()) {
            if (transition.delayMs > 0) {
                diagnostics->push_back(
                    QStringLiteral("transition '%1' from %2: delayMs %3 on an eventful transition is "
                                    "decorative in Ordo and has no XState slot -- dropped")
                        .arg(event, sourceLabel)
                        .arg(transition.delayMs));
            }
            if (!onByEvent.contains(event)) {
                onEventOrder.push_back(event);
            }
            onByEvent[event].append(transitionToJson(transition, nameById, machine.name,
                                                      QStringLiteral("transition '%1' from %2").arg(event, sourceLabel),
                                                      diagnostics, payloadName));
        } else if (transition.delayMs > 0) {
            if (!afterByDelay.contains(transition.delayMs)) {
                afterDelayOrder.push_back(transition.delayMs);
            }
            afterByDelay[transition.delayMs].append(
                transitionToJson(transition, nameById, machine.name,
                                 QStringLiteral("delayed transition (%1 ms) from %2")
                                     .arg(QString::number(transition.delayMs), sourceLabel),
                                 diagnostics, payloadName));
        } else {
            // Blank event AND no delay AND not always: the validator already calls this a
            // dead transition; XState has no slot for it either.
            diagnostics->push_back(
                QStringLiteral("transition %1 from %2 has a blank event and no delay -- unmappable, dropped")
                    .arg(transition.id)
                    .arg(sourceLabel));
        }
    }
    if (!alwaysTransitions.isEmpty()) {
        (*into)[QStringLiteral("always")] = alwaysTransitions;
    }
    if (!onEventOrder.isEmpty()) {
        QJsonObject on;
        for (const QString& event : onEventOrder) {
            const QJsonArray& candidates = onByEvent.value(event);
            on[event] = candidates.size() == 1 ? candidates.first() : QJsonValue(candidates);
        }
        (*into)[QStringLiteral("on")] = on;
    }
    if (!afterDelayOrder.isEmpty()) {
        QJsonObject after;
        for (int delayMs : afterDelayOrder) {
            // Always array-valued: two delayed transitions may share one delay.
            after[QString::number(delayMs)] = afterByDelay.value(delayMs);
        }
        (*into)[QStringLiteral("after")] = after;
    }
}

// One invoke completion (onDone/onError) as v5's nested slot value: a single object or a
// candidate array, like `on` groups. Returns the Undefined sentinel when nothing matches
// so the caller omits the slot. These rows never reach emitTransitionGroups(), so the
// decorative-delayMs diagnostic is raised here.
QJsonValue emitSingleInvokeCompletion(const Machine& machine, const State& state, const QString& effId,
                                      InvokePayloadKind wantKind, const QHash<quint64, QString>& nameById,
                                      const QString& label, QStringList* diagnostics) {
    QJsonArray candidates;
    const QString expectedEvent = (wantKind == InvokePayloadKind::Output ? QStringLiteral("done.invoke.") : QStringLiteral("error.platform.")) + effId;
    for (const Transition& transition : machine.transitions) {
        if (transition.from != state.id || (effId.isEmpty() ? (invokePayloadKindForTransition(machine, transition) != wantKind) : (transition.event != expectedEvent))) {
            continue;
        }
        if (transition.delayMs > 0) {
            diagnostics->push_back(
                QStringLiteral("%1: delayMs %2 on an invoke completion is decorative in Ordo and has no XState "
                                "slot -- dropped")
                    .arg(label)
                    .arg(transition.delayMs));
        }
        const QString payloadName = payloadNameForTransition(machine, transition);
        candidates.append(transitionToJson(transition, nameById, machine.name, label, diagnostics, payloadName,
                                            /*isInvokeCompletion=*/true));
    }
    if (candidates.isEmpty()) {
        return QJsonValue(QJsonValue::Undefined);
    }
    return candidates.size() == 1 ? candidates.first() : QJsonValue(candidates);
}

QJsonValue emitInvokeCompletion(const Machine& machine, const State& state, InvokePayloadKind wantKind,
                                const QHash<quint64, QString>& nameById, const QString& label,
                                QStringList* diagnostics) {
    return emitSingleInvokeCompletion(machine, state, effectiveInvokeId(state), wantKind, nameById, label, diagnostics);
}

QJsonObject singleInvokeToJson(const Machine& machine, const State& state, const Invocation& inv,
                               const QHash<quint64, QString>& nameById, QStringList* diagnostics) {
    QJsonObject invoke;
    invoke[QStringLiteral("src")] = inv.src;
    if (!inv.id.trimmed().isEmpty()) {
        invoke[QStringLiteral("id")] = inv.id;
    }
    const QString effId = effectiveInvocationId(inv);
    const QString label = QStringLiteral("invoke on '%1'").arg(nameById.value(state.id));
    const QJsonValue onDone = emitSingleInvokeCompletion(machine, state, effId, InvokePayloadKind::Output, nameById,
                                                         label + QStringLiteral(" onDone"), diagnostics);
    if (!onDone.isUndefined()) {
        invoke[QStringLiteral("onDone")] = onDone;
    }
    const QJsonValue onError = emitSingleInvokeCompletion(machine, state, effId, InvokePayloadKind::Error, nameById,
                                                          label + QStringLiteral(" onError"), diagnostics);
    if (!onError.isUndefined()) {
        invoke[QStringLiteral("onError")] = onError;
    }
    return invoke;
}

QJsonObject invokeToJson(const Machine& machine, const State& state, const QHash<quint64, QString>& nameById,
                         QStringList* diagnostics) {
    return singleInvokeToJson(machine, state, Invocation{.src = state.invokeSrc, .id = state.invokeId, .outputType = state.invokeOutputType},
                              nameById, diagnostics);
}

// One state's whole JSON body (kind, entry/exit, `on`/`after`, nested `states`/`initial`,
// meta.ordo), the same shape at every tree level. `childrenByParent` is a document-order
// index built once by the caller. A Parallel state's children become its regions through
// the ordinary nested `states`.
QJsonObject stateToJson(const Machine& machine, const State& state, const QHash<quint64, QString>& nameById,
                        const QHash<quint64, QVector<const State*>>& childrenByParent, QStringList* diagnostics) {
    QJsonObject stateObject;
    switch (state.kind) {
        case StateKind::Parallel:
            stateObject[QStringLiteral("type")] = QStringLiteral("parallel");
            break;
        case StateKind::Final:
            stateObject[QStringLiteral("type")] = QStringLiteral("final");
            break;
        case StateKind::History:
            stateObject[QStringLiteral("type")] = QStringLiteral("history");
            if (state.historyDeep) {
                // v5 defaults to shallow when the key is absent: emit only "deep".
                // Import treats "shallow" and absent identically.
                stateObject[QStringLiteral("history")] = QStringLiteral("deep");
            }
            break;
        case StateKind::Normal:
            break;  // atomic: v5's default, omitted
    }
    if (!state.entryActions.isEmpty()) {
        stateObject[QStringLiteral("entry")] =
            actionsToJson(state.entryActions, QStringLiteral("state '%1' entry").arg(state.name), diagnostics);
    }
    if (!state.exitActions.isEmpty()) {
        stateObject[QStringLiteral("exit")] =
            actionsToJson(state.exitActions, QStringLiteral("state '%1' exit").arg(state.name), diagnostics);
    }
    if (!state.description.isEmpty()) {
        stateObject[QStringLiteral("description")] = state.description;
    }
    if (!state.tags.isEmpty()) {
        stateObject[QStringLiteral("tags")] = QJsonArray::fromStringList(state.tags);
    }

    // `on`: event -> one transition object, or a candidate array when several
    // transitions share a (source, event) pair (v5's guarded-fallback form).
    // onDone/onError rows are excluded; they belong to `invoke` below.
    emitTransitionGroups(machine, state.id, QStringLiteral("'%1'").arg(nameById.value(state.id)), nameById,
                          &stateObject, diagnostics, /*excludeInvokeCompletions=*/true);

    // `invoke`: emitted only when this state declares one.
    const auto effectiveInvs = state.effectiveInvocations();
    if (effectiveInvs.size() > 1) {
        QJsonArray invArr;
        for (const Invocation& inv : effectiveInvs) {
            if (!inv.src.trimmed().isEmpty()) {
                invArr.append(singleInvokeToJson(machine, state, inv, nameById, diagnostics));
            }
        }
        if (!invArr.isEmpty()) {
            stateObject[QStringLiteral("invoke")] = invArr;
        }
    } else if (effectiveInvs.size() == 1 && !effectiveInvs.first().src.trimmed().isEmpty()) {
        stateObject[QStringLiteral("invoke")] = singleInvokeToJson(machine, state, effectiveInvs.first(), nameById, diagnostics);
    }

    const QVector<const State*> children = childrenByParent.value(state.id);
    if (!children.isEmpty()) {
        QJsonObject nested;
        for (const State* child : children) {
            nested[nameById.value(child->id)] = stateToJson(machine, *child, nameById, childrenByParent, diagnostics);
        }
        stateObject[QStringLiteral("states")] = nested;
        if (state.initialChildId != 0 && nameById.contains(state.initialChildId)) {
            stateObject[QStringLiteral("initial")] = nameById.value(state.initialChildId);
        }
    }

    QJsonObject ordo;
    ordo[QStringLiteral("x")] = state.pos.x();
    ordo[QStringLiteral("y")] = state.pos.y();
    // invokeOutputType has no v5 counterpart (a TypeScript type), so it rides meta.ordo;
    // emitted only for an invoking state and only when not the default (Int).
    if (!state.invokeSrc.trimmed().isEmpty() && state.invokeOutputType != ContextType::Int) {
        ordo[QStringLiteral("invokeOutputType")] = contextTypeName(state.invokeOutputType);
    }
    QJsonObject meta;
    meta[QStringLiteral("ordo")] = ordo;
    stateObject[QStringLiteral("meta")] = meta;

    return stateObject;
}

}  // namespace

XStateExportResult machineToXStateJson(const Machine& machine) {
    XStateExportResult result;

    // Names key everything on the XState side: refuse a blank or duplicate name
    // rather than emit a file that imports as a different machine.
    QHash<quint64, QString> nameById;
    QSet<QString> seenNames;
    for (const State& state : machine.states) {
        const QString name = state.name.trimmed();
        if (name.isEmpty()) {
            result.ok = false;
            result.error = QStringLiteral("state %1 has a blank name -- XState keys states by name").arg(state.id);
            return result;
        }
        if (seenNames.contains(name)) {
            result.ok = false;
            result.error =
                QStringLiteral("two states share the name '%1' -- XState keys states by name, rename one").arg(name);
            return result;
        }
        seenNames.insert(name);
        nameById.insert(state.id, name);
    }

    result.json[QStringLiteral("id")] = machine.name;
    if (machine.initialStateId != 0 && nameById.contains(machine.initialStateId)) {
        result.json[QStringLiteral("initial")] = nameById.value(machine.initialStateId);
    }

    // `context` plus its meta schema, emitted only when the machine has variables so
    // a context-free machine exports byte-identically.
    QJsonArray contextSchema;
    const QJsonObject contextValues = contextToJson(machine, &contextSchema, &result.diagnostics);
    if (!contextValues.isEmpty()) {
        result.json[QStringLiteral("context")] = contextValues;
    }

    // parentId -> direct children in document order; a child is filed under its
    // parent's own `states`, never at the machine root.
    QHash<quint64, QVector<const State*>> childrenByParent;
    for (const State& state : machine.states) {
        if (state.parentId != 0) {
            childrenByParent[state.parentId].push_back(&state);
        }
    }

    QJsonObject states;
    for (const State& state : machine.states) {
        if (state.parentId != 0) {
            continue;  // nested under its parent's own `states` below
        }
        states[nameById.value(state.id)] = stateToJson(machine, state, nameById, childrenByParent, &result.diagnostics);
    }
    result.json[QStringLiteral("states")] = states;

    // Machine-level (root, from == 0) transitions become v5's root `on`/`after`.
    emitTransitionGroups(machine, 0, QStringLiteral("the machine"), nameById, &result.json, &result.diagnostics);

    QJsonObject machineOrdo;
    // Stays 2: the context schema is additive, and a bump would change the bytes of
    // every context-free export.
    machineOrdo[QStringLiteral("formatVersion")] = 2;
    if (!contextSchema.isEmpty()) {
        machineOrdo[QStringLiteral("context")] = contextSchema;
    }
    QJsonObject machineMeta;
    machineMeta[QStringLiteral("ordo")] = machineOrdo;
    result.json[QStringLiteral("meta")] = machineMeta;

    return result;
}

namespace {

// ---- import helpers ---------------------------------------------------------

// The `type` values reserved for structural expressions (the inverse of
// expressionToJson()): an object-form guard with one of these types is an
// expression, any other is a named hook. A plain-string guard such as "and" still
// names a hook.
bool isStructuralExpressionType(const QString& type) {
    static const QStringList reserved{
        QStringLiteral("context"), QStringLiteral("event"), QStringLiteral("and"), QStringLiteral("or"),
        QStringLiteral("not"),     QStringLiteral("-"),     QStringLiteral("=="),  QStringLiteral("!="),
        QStringLiteral("<"),       QStringLiteral("<="),    QStringLiteral(">"),   QStringLiteral(">="),
        QStringLiteral("+"),       QStringLiteral("*"),     QStringLiteral("/"),   QStringLiteral("%"),
        QStringLiteral("call"),    QStringLiteral("member")};
    return reserved.contains(type);
}

expr::NodeKind comparisonKind(const QString& type, bool* ok) {
    *ok = true;
    if (type == QStringLiteral("==")) return expr::NodeKind::Equal;
    if (type == QStringLiteral("!=")) return expr::NodeKind::NotEqual;
    if (type == QStringLiteral("<")) return expr::NodeKind::Less;
    if (type == QStringLiteral("<=")) return expr::NodeKind::LessEqual;
    if (type == QStringLiteral(">")) return expr::NodeKind::Greater;
    if (type == QStringLiteral(">=")) return expr::NodeKind::GreaterEqual;
    if (type == QStringLiteral("+")) return expr::NodeKind::Add;
    if (type == QStringLiteral("-")) return expr::NodeKind::Subtract;
    if (type == QStringLiteral("*")) return expr::NodeKind::Multiply;
    if (type == QStringLiteral("/")) return expr::NodeKind::Divide;
    if (type == QStringLiteral("%")) return expr::NodeKind::Modulo;
    *ok = false;
    return expr::NodeKind::Equal;
}

int appendNode(expr::Ast* ast, const expr::Node& node) {
    ast->nodes.push_back(node);
    return static_cast<int>(ast->nodes.size()) - 1;
}

// One JSON value as an Ast subtree; returns the node's index in the flat arena (-1 =
// diagnosed failure). Builds the same tree the infix parser would, so the printed
// result re-exports byte-identically. sourceOffset/sourceLength stay 0: there is no
// source text, and print() ignores them.
int expressionFromJson(const QJsonValue& value, const QString& path, expr::Ast* ast, QStringList* diagnostics) {
    if (value.isBool()) {
        expr::Node node;
        node.kind = expr::NodeKind::BoolLiteral;
        node.boolValue = value.toBool();
        return appendNode(ast, node);
    }
    if (value.isDouble()) {
        // JSON has one number type: an integral number reads as an Int literal, anything
        // else as a Double. Harmless, as Int and Double promote freely; a declared
        // Double `3.0` is the one thing this cannot recover.
        const double number = value.toDouble();
        const bool integral = std::isfinite(number) && number == std::trunc(number) && std::fabs(number) < 9.2e18;
        expr::Node node;
        node.kind = integral ? expr::NodeKind::IntLiteral : expr::NodeKind::DoubleLiteral;
        node.intValue = integral ? static_cast<qint64>(number) : 0;
        node.doubleValue = integral ? 0.0 : number;
        return appendNode(ast, node);
    }
    if (value.isString()) {
        expr::Node node;
        node.kind = expr::NodeKind::StringLiteral;
        node.text = value.toString();
        return appendNode(ast, node);
    }
    if (!value.isObject()) {
        diagnostics->push_back(
            QStringLiteral("%1: neither a literal nor an operator object -- expression dropped").arg(path));
        return -1;
    }
    const QJsonObject object = value.toObject();
    const QString type = object.value(QStringLiteral("type")).toString().trimmed();

    if (type == QStringLiteral("context")) {
        const QString name = object.value(QStringLiteral("name")).toString().trimmed();
        if (!expr::isBareIdentifier(name)) {
            diagnostics->push_back(
                QStringLiteral("%1.name: '%2' is not an identifier -- expression dropped").arg(path, name));
            return -1;
        }
        expr::Node node;
        node.kind = expr::NodeKind::Identifier;
        node.text = name;
        return appendNode(ast, node);
    }
    // `event.output`/`event.error`: the inverse of expressionToJson()'s event reference.
    // Unconditional (the JSON already says which meaning was intended); any other
    // name is diagnosed and dropped.
    if (type == QStringLiteral("event")) {
        const QString name = object.value(QStringLiteral("name")).toString().trimmed();
        if (name != QStringLiteral("output") && name != QStringLiteral("error")) {
            diagnostics->push_back(
                QStringLiteral("%1.name: '%2' is not a known invoke-payload identifier -- expression dropped")
                    .arg(path, name));
            return -1;
        }
        expr::Node node;
        node.kind = expr::NodeKind::Identifier;
        node.text = name;
        return appendNode(ast, node);
    }
    if (type == QStringLiteral("and") || type == QStringLiteral("or") || type == QStringLiteral("not")) {
        const QJsonValue guardsValue = object.value(QStringLiteral("guards"));
        if (!guardsValue.isArray()) {
            diagnostics->push_back(
                QStringLiteral("%1.guards: '%2' needs a `guards` array -- expression dropped").arg(path, type));
            return -1;
        }
        const QJsonArray guards = guardsValue.toArray();
        if (type == QStringLiteral("not")) {
            if (guards.size() != 1) {
                diagnostics->push_back(QStringLiteral("%1.guards: `not` takes exactly one operand (%2 given) -- "
                                                       "expression dropped")
                                            .arg(path)
                                            .arg(guards.size()));
                return -1;
            }
            const int operand =
                expressionFromJson(guards.at(0), path + QStringLiteral(".guards[0]"), ast, diagnostics);
            if (operand < 0) {
                return -1;
            }
            expr::Node node;
            node.kind = expr::NodeKind::Not;
            node.lhs = operand;
            return appendNode(ast, node);
        }
        if (guards.size() < 2) {
            diagnostics->push_back(QStringLiteral("%1.guards: `%2` needs at least two operands (%3 given) -- "
                                                   "expression dropped")
                                        .arg(path, type)
                                        .arg(guards.size()));
            return -1;
        }
        // Left-fold: our exports have two operands, so this is the identity; a
        // hand-written `and([a, b, c])` folds to `(a && b) && c`.
        const expr::NodeKind kind = type == QStringLiteral("and") ? expr::NodeKind::And : expr::NodeKind::Or;
        int folded = expressionFromJson(guards.at(0), path + QStringLiteral(".guards[0]"), ast, diagnostics);
        if (folded < 0) {
            return -1;
        }
        for (int i = 1; i < guards.size(); ++i) {
            const int right =
                expressionFromJson(guards.at(i), QStringLiteral("%1.guards[%2]").arg(path).arg(i), ast, diagnostics);
            if (right < 0) {
                return -1;
            }
            expr::Node node;
            node.kind = kind;
            node.lhs = folded;
            node.rhs = right;
            folded = appendNode(ast, node);
        }
        return folded;
    }
    if (type == QStringLiteral("member")) {
        const QString property = object.value(QStringLiteral("property")).toString().trimmed();
        const int obj = expressionFromJson(object.value(QStringLiteral("object")), path + QStringLiteral(".object"), ast, diagnostics);
        if (obj < 0) return -1;
        expr::Node node;
        node.kind = expr::NodeKind::MemberAccess;
        node.lhs = obj;
        node.text = property;
        return appendNode(ast, node);
    }
    if (type == QStringLiteral("call")) {
        const QString callee = object.value(QStringLiteral("callee")).toString().trimmed();
        expr::Node node;
        node.kind = expr::NodeKind::Call;
        node.text = callee;
        const QJsonArray args = object.value(QStringLiteral("arguments")).toArray();
        if (!args.isEmpty()) {
            node.lhs = expressionFromJson(args.at(0), path + QStringLiteral(".arguments[0]"), ast, diagnostics);
            if (node.lhs < 0) return -1;
        }
        if (args.size() > 1) {
            node.rhs = expressionFromJson(args.at(1), path + QStringLiteral(".arguments[1]"), ast, diagnostics);
            if (node.rhs < 0) return -1;
        }
        for (int a = 2; a < args.size(); ++a) {
            const int extra = expressionFromJson(args.at(a), path + QStringLiteral(".arguments[%1]").arg(a), ast, diagnostics);
            if (extra < 0) return -1;
            node.extraArgs.push_back(extra);
        }
        return appendNode(ast, node);
    }
    if (type == QStringLiteral("-") && object.contains(QStringLiteral("value"))) {
        const int operand =
            expressionFromJson(object.value(QStringLiteral("value")), path + QStringLiteral(".value"), ast, diagnostics);
        if (operand < 0) {
            return -1;
        }
        expr::Node node;
        node.kind = expr::NodeKind::Negate;
        node.lhs = operand;
        return appendNode(ast, node);
    }
    bool comparison = false;
    const expr::NodeKind kind = comparisonKind(type, &comparison);
    if (!comparison) {
        diagnostics->push_back(
            QStringLiteral("%1.type: '%2' is not an expression operator -- expression dropped").arg(path, type));
        return -1;
    }
    const int left =
        expressionFromJson(object.value(QStringLiteral("left")), path + QStringLiteral(".left"), ast, diagnostics);
    if (left < 0) {
        return -1;
    }
    const int right =
        expressionFromJson(object.value(QStringLiteral("right")), path + QStringLiteral(".right"), ast, diagnostics);
    if (right < 0) {
        return -1;
    }
    expr::Node node;
    node.kind = kind;
    node.lhs = left;
    node.rhs = right;
    return appendNode(ast, node);
}

// The structural form as infix text via expr::print(), which re-parses to the same
// tree, so re-exporting reproduces the JSON. Empty return = diagnosed failure.
QString expressionSourceFromJson(const QJsonValue& value, const QString& path, QStringList* diagnostics) {
    expr::Ast ast;
    const int root = expressionFromJson(value, path, &ast, diagnostics);
    if (root < 0) {
        return QString();
    }
    ast.root = root;
    return expr::print(ast);
}

// v5's xstate.raise action -> this model's `raise(<event>)` action string.
QString raiseFromJson(const QJsonObject& object, const QString& path, QStringList* diagnostics) {
    for (const QString& key : object.keys()) {
        if (key != QStringLiteral("type") && key != QStringLiteral("params") && key != QStringLiteral("event")) {
            diagnostics->push_back(
                QStringLiteral("%1.%2: unrecognized key on xstate.raise action -- dropped").arg(path, key));
        }
    }
    QString eventName;
    if (object.contains(QStringLiteral("params")) && object.value(QStringLiteral("params")).isObject()) {
        const QJsonObject params = object.value(QStringLiteral("params")).toObject();
        if (params.contains(QStringLiteral("event"))) {
            const QJsonValue evVal = params.value(QStringLiteral("event"));
            if (evVal.isObject()) {
                eventName = evVal.toObject().value(QStringLiteral("type")).toString().trimmed();
            } else if (evVal.isString()) {
                eventName = evVal.toString().trimmed();
            }
        }
    }
    if (eventName.isEmpty() && object.contains(QStringLiteral("event"))) {
        const QJsonValue evVal = object.value(QStringLiteral("event"));
        if (evVal.isObject()) {
            eventName = evVal.toObject().value(QStringLiteral("type")).toString().trimmed();
        } else if (evVal.isString()) {
            eventName = evVal.toString().trimmed();
        }
    }

    if (eventName.isEmpty()) {
        diagnostics->push_back(QStringLiteral("%1: xstate.raise carries no valid event type -- dropped").arg(path));
        return QString();
    }

    const QString candidate = QStringLiteral("raise(%1)").arg(eventName);
    const expr::RaiseForm parsed = expr::parseRaiseForm(candidate);
    if (!parsed.ok) {
        diagnostics->push_back(QStringLiteral("%1: '%2' is not a valid raise in this editor (%3) -- dropped")
                                   .arg(path, candidate, parsed.message));
        return QString();
    }
    return candidate;
}

// v5's assign action -> `<name> = <expr>` (the inverse of actionToJson()). A multi-key
// `assignment` maps its first key and notes the rest. The built string is re-read with
// expr::parseAssignForm; if it would not classify as an assign (e.g. a non-identifier
// target) it is dropped rather than imported as a hook with '=' in its name.
QString assignFromJson(const QJsonObject& object, const QString& path, QStringList* diagnostics) {
    for (const QString& key : object.keys()) {
        if (key != QStringLiteral("type") && key != QStringLiteral("assignment")) {
            diagnostics->push_back(
                QStringLiteral("%1.%2: only `assignment` maps from an xstate.assign action -- dropped")
                    .arg(path, key));
        }
    }
    const QJsonValue assignmentValue = object.value(QStringLiteral("assignment"));
    if (!assignmentValue.isObject()) {
        diagnostics->push_back(QStringLiteral("%1.assignment: not an object -- assign dropped").arg(path));
        return QString();
    }
    const QJsonObject assignment = assignmentValue.toObject();
    const QStringList targets = assignment.keys();
    if (targets.isEmpty()) {
        diagnostics->push_back(QStringLiteral("%1.assignment: assigns nothing -- dropped").arg(path));
        return QString();
    }
    for (int i = 1; i < targets.size(); ++i) {
        diagnostics->push_back(QStringLiteral("%1.assignment.%2: only the first assignment maps to this model's "
                                               "single action slot -- dropped")
                                    .arg(path, targets.at(i)));
    }
    const QString target = targets.first();
    const QString valuePath = QStringLiteral("%1.assignment.%2").arg(path, target);
    const QString valueSource = expressionSourceFromJson(assignment.value(target), valuePath, diagnostics);
    if (valueSource.isEmpty()) {
        return QString();
    }
    const QString source = target.trimmed() + QStringLiteral(" = ") + valueSource;
    if (!expr::parseAssignForm(source).ok) {
        diagnostics->push_back(
            QStringLiteral("%1: '%2' is not a valid assign in this editor -- dropped").arg(valuePath, source));
        return QString();
    }
    return source;
}

// v5's xstate.sendTo action -> this model's `sendTo(<target>, <event>)` action string.
QString sendToJson(const QJsonObject& object, const QString& path, QStringList* diagnostics) {
    for (const QString& key : object.keys()) {
        if (key != QStringLiteral("type") && key != QStringLiteral("params") && key != QStringLiteral("to") && key != QStringLiteral("event")) {
            diagnostics->push_back(
                QStringLiteral("%1.%2: unrecognized key on xstate.sendTo action -- dropped").arg(path, key));
        }
    }
    QString target;
    QString eventName;
    if (object.contains(QStringLiteral("params")) && object.value(QStringLiteral("params")).isObject()) {
        const QJsonObject params = object.value(QStringLiteral("params")).toObject();
        if (params.contains(QStringLiteral("to"))) {
            target = params.value(QStringLiteral("to")).toString().trimmed();
        }
        if (params.contains(QStringLiteral("event"))) {
            const QJsonValue evVal = params.value(QStringLiteral("event"));
            if (evVal.isObject()) {
                eventName = evVal.toObject().value(QStringLiteral("type")).toString().trimmed();
            } else if (evVal.isString()) {
                eventName = evVal.toString().trimmed();
            }
        }
    }
    if (target.isEmpty() && object.contains(QStringLiteral("to"))) {
        target = object.value(QStringLiteral("to")).toString().trimmed();
    }
    if (eventName.isEmpty() && object.contains(QStringLiteral("event"))) {
        const QJsonValue evVal = object.value(QStringLiteral("event"));
        if (evVal.isObject()) {
            eventName = evVal.toObject().value(QStringLiteral("type")).toString().trimmed();
        } else if (evVal.isString()) {
            eventName = evVal.toString().trimmed();
        }
    }

    if (target.isEmpty() || eventName.isEmpty()) {
        diagnostics->push_back(QStringLiteral("%1: xstate.sendTo requires both valid `to` and `event` -- dropped").arg(path));
        return QString();
    }

    const QString candidate = QStringLiteral("sendTo(%1, %2)").arg(target, eventName);
    const expr::SendToForm parsed = expr::parseSendToForm(candidate);
    if (!parsed.ok) {
        diagnostics->push_back(QStringLiteral("%1: '%2' is not a valid sendTo in this editor (%3) -- dropped")
                                   .arg(path, candidate, parsed.message));
        return QString();
    }
    return candidate;
}

// v5's xstate.sendParent action -> this model's `sendParent(<event>)` action string.
QString sendParentFromJson(const QJsonObject& object, const QString& path, QStringList* diagnostics) {
    for (const QString& key : object.keys()) {
        if (key != QStringLiteral("type") && key != QStringLiteral("params") && key != QStringLiteral("event")) {
            diagnostics->push_back(
                QStringLiteral("%1.%2: unrecognized key on xstate.sendParent action -- dropped").arg(path, key));
        }
    }
    QString eventName;
    if (object.contains(QStringLiteral("params")) && object.value(QStringLiteral("params")).isObject()) {
        const QJsonObject params = object.value(QStringLiteral("params")).toObject();
        if (params.contains(QStringLiteral("event"))) {
            const QJsonValue evVal = params.value(QStringLiteral("event"));
            if (evVal.isObject()) {
                eventName = evVal.toObject().value(QStringLiteral("type")).toString().trimmed();
            } else if (evVal.isString()) {
                eventName = evVal.toString().trimmed();
            }
        }
    }
    if (eventName.isEmpty() && object.contains(QStringLiteral("event"))) {
        const QJsonValue evVal = object.value(QStringLiteral("event"));
        if (evVal.isObject()) {
            eventName = evVal.toObject().value(QStringLiteral("type")).toString().trimmed();
        } else if (evVal.isString()) {
            eventName = evVal.toString().trimmed();
        }
    }

    if (eventName.isEmpty()) {
        diagnostics->push_back(QStringLiteral("%1: xstate.sendParent carries no valid event type -- dropped").arg(path));
        return QString();
    }

    const QString candidate = QStringLiteral("sendParent(%1)").arg(eventName);
    const expr::SendParentForm parsed = expr::parseSendParentForm(candidate);
    if (!parsed.ok) {
        diagnostics->push_back(QStringLiteral("%1: '%2' is not a valid sendParent in this editor (%3) -- dropped")
                                   .arg(path, candidate, parsed.message));
        return QString();
    }
    return candidate;
}

// An action name as v5 spells it: a plain string for a hook name, or a
// { type } object.
QString actionName(const QJsonValue& value, const QString& path, QStringList* diagnostics) {
    if (value.isString()) {
        return value.toString().trimmed();
    }
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        const QString name = object.value(QStringLiteral("type")).toString().trimmed();
        if (name.isEmpty()) {
            diagnostics->push_back(QStringLiteral("%1: object form carries no string `type` -- dropped").arg(path));
            return QString();
        }
        if (name == QStringLiteral("xstate.assign")) {
            // Checked before the "only `type` maps" sweep, which would lose `assignment`.
            return assignFromJson(object, path, diagnostics);
        }
        if (name == QStringLiteral("xstate.raise")) {
            // Becomes the action string `raise(<event>)`.
            return raiseFromJson(object, path, diagnostics);
        }
        if (name == QStringLiteral("xstate.sendTo")) {
            // Becomes the action string `sendTo(<target>, <event>)`.
            return sendToJson(object, path, diagnostics);
        }
        if (name == QStringLiteral("xstate.sendParent")) {
            // Becomes the action string `sendParent(<event>)`.
            return sendParentFromJson(object, path, diagnostics);
        }
        for (const QString& key : object.keys()) {
            if (key != QStringLiteral("type")) {
                diagnostics->push_back(
                    QStringLiteral("%1.%2: only `type` maps from the object form -- dropped").arg(path, key));
            }
        }
        return name;
    }
    diagnostics->push_back(QStringLiteral("%1: neither a string nor a { type } object -- dropped").arg(path));
    return QString();
}

// entry/exit/tags: v5 accepts a single string or an array of them.
QStringList nameList(const QJsonValue& value, const QString& path, QStringList* diagnostics) {
    QStringList result;
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (int i = 0; i < array.size(); ++i) {
            const QString name = actionName(array.at(i), QStringLiteral("%1[%2]").arg(path).arg(i), diagnostics);
            if (!name.isEmpty()) {
                result.push_back(name);
            }
        }
    } else {
        const QString name = actionName(value, path, diagnostics);
        if (!name.isEmpty()) {
            result.push_back(name);
        }
    }
    return result;
}

// One transition's `guard`: a structural object becomes infix text; a plain string
// (a hook name, or hand-written infix text) and a `{ "type": "name" }` object are read as before.
QString guardFromJson(const QJsonValue& value, const QString& path, QStringList* diagnostics) {
    if (!value.isObject() ||
        !isStructuralExpressionType(value.toObject().value(QStringLiteral("type")).toString().trimmed())) {
        return actionName(value, path, diagnostics);
    }
    const QString source = expressionSourceFromJson(value, path, diagnostics);
    if (source.isEmpty()) {
        return QString();  // expressionFromJson already diagnosed it
    }
    if (expr::isBareIdentifier(source)) {
        // A lone context reference as a whole guard has no spelling here: a bare
        // identifier in a guard string is a hook name, so importing it would turn a
        // context test into a hook reference. Dropped, with the workaround named.
        diagnostics->push_back(QStringLiteral("%1: a bare context reference cannot be a whole guard here (a lone "
                                               "identifier names a hook) -- write `%2 == true`; dropped")
                                    .arg(path, source));
        return QString();
    }
    return source;
}

// `ordo` is this app's meta channel; other meta keys are foreign, dropped with a
// diagnostic. Returns the ordo object (empty when absent).
QJsonObject takeOrdoMeta(const QJsonValue& value, const QString& path, QStringList* diagnostics) {
    if (!value.isObject()) {
        return QJsonObject();
    }
    const QJsonObject meta = value.toObject();
    for (const QString& key : meta.keys()) {
        if (key != QStringLiteral("ordo")) {
            diagnostics->push_back(
                QStringLiteral("%1.%2: foreign meta has no model field -- dropped").arg(path, key));
        }
    }
    return meta.value(QStringLiteral("ordo")).toObject();
}

// Resolves a target string against the full name index, in order: (a) an exact sibling
// of the source state (root scope for a machine-level transition) wins even if the
// name exists elsewhere; (b) a `#` absolute path, either `#machineId.Parent.Child` or
// a bare `#RootState`; (c) a name that is unique across the whole tree; (d) anything
// else, including an ambiguous name, is unresolved (returns 0 with a diagnostic).
// `machineSelfOut` is set, returning 0 without one, for the machine's own id.
quint64 resolveTarget(const QString& target, quint64 sourceParentId, const QString& machineName,
                     const QHash<quint64, QHash<QString, quint64>>& siblingsByParent,
                     const QHash<QString, QVector<quint64>>& idsByName, const QString& path,
                     QStringList* diagnostics, bool* machineSelfOut) {
    // (a) sibling scope of the source state.
    const auto siblingScope = siblingsByParent.constFind(sourceParentId);
    if (siblingScope != siblingsByParent.constEnd()) {
        const auto hit = siblingScope->constFind(target);
        if (hit != siblingScope->constEnd()) {
            return hit.value();
        }
    }
    // (b) '#'-prefixed absolute path.
    if (target.startsWith(QLatin1Char('#'))) {
        const QStringList segments = target.mid(1).split(QLatin1Char('.'));
        if (segments.size() == 1) {
            // A bare "#RootState" -- a root-level name, no machine-id prefix.
            const auto rootScope = siblingsByParent.constFind(0);
            if (rootScope != siblingsByParent.constEnd()) {
                const auto hit = rootScope->constFind(segments.first());
                if (hit != rootScope->constEnd()) {
                    return hit.value();
                }
            }
            // "#machineId" alone: XState's root self-target.
            if (machineSelfOut != nullptr && segments.first().trimmed() == machineName.trimmed()) {
                *machineSelfOut = true;
                return 0;
            }
        } else if (segments.first().trimmed() == machineName.trimmed()) {
            // "#machineId.Parent.Child...": walk the remaining segments
            // root to leaf, one nesting level per segment.
            quint64 scopeParent = 0;
            quint64 resolved = 0;
            bool ok = true;
            for (int i = 1; i < segments.size(); ++i) {
                const auto scope = siblingsByParent.constFind(scopeParent);
                if (scope == siblingsByParent.constEnd()) {
                    ok = false;
                    break;
                }
                const auto hit = scope->constFind(segments.at(i));
                if (hit == scope->constEnd()) {
                    ok = false;
                    break;
                }
                resolved = hit.value();
                scopeParent = resolved;
            }
            if (ok) {
                return resolved;
            }
        }
        diagnostics->push_back(
            QStringLiteral("%1.target: no state resolves the '#' path '%2' -- transition dropped").arg(path, target));
        return 0;
    }
    // (c) a name that is globally unique across the whole tree.
    const auto matches = idsByName.constFind(target);
    if (matches != idsByName.constEnd()) {
        if (matches->size() == 1) {
            return matches->first();
        }
        diagnostics->push_back(
            QStringLiteral("%1.target: '%2' names %3 different states -- ambiguous, transition dropped")
                .arg(path, target)
                .arg(matches->size()));
        return 0;
    }
    // (d) nothing resolved at all.
    diagnostics->push_back(
        QStringLiteral("%1.target: no state is named '%2' -- transition dropped").arg(path, target));
    return 0;
}

// Parses one transition value (string shorthand or full object) into `machine`, minting
// the next id. A transition whose target does not resolve is dropped whole with a
// diagnostic; importing it targetless would silently change semantics.
void parseTransition(const QJsonValue& value, const QString& path, quint64 fromId, quint64 sourceParentId,
                     const QString& event, int delayMs, const QString& machineName,
                     const QHash<quint64, QHash<QString, quint64>>& siblingsByParent,
                     const QHash<QString, QVector<quint64>>& idsByName, Machine* machine,
                     QStringList* diagnostics, bool isAlways = false) {
    QJsonObject object;
    if (value.isString()) {
        object[QStringLiteral("target")] = value.toString();  // v5 shorthand: "EVENT": "TargetName"
    } else if (value.isArray()) {
        object[QStringLiteral("target")] = value.toArray();   // v5 shorthand: "EVENT": ["Target1", "Target2"]
    } else if (value.isObject()) {
        object = value.toObject();
    } else {
        diagnostics->push_back(
            QStringLiteral("%1: neither a target string nor a transition object -- dropped").arg(path));
        return;
    }

    Transition transition;
    transition.from = fromId;
    transition.event = isAlways ? QString() : event;
    transition.delayMs = isAlways ? 0 : delayMs;
    transition.always = isAlways;

    const QJsonValue targetValue = object.value(QStringLiteral("target"));
    if (!targetValue.isUndefined() && !targetValue.isNull()) {  // absent/null = targetless, `to` stays 0
        if (targetValue.isArray()) {
            const QJsonArray targetArray = targetValue.toArray();
            if (targetArray.isEmpty()) {
                transition.to = 0;
                transition.targets.clear();
            } else {
                QList<quint64> resolvedList;
                bool allResolved = true;
                for (int i = 0; i < targetArray.size(); ++i) {
                    const QString elemTarget = targetArray.at(i).toString();
                    bool elemMachineSelf = false;
                    const quint64 resolvedElem = resolveTarget(elemTarget, sourceParentId, machineName,
                                                               siblingsByParent, idsByName,
                                                               QStringLiteral("%1.target[%2]").arg(path).arg(i),
                                                               diagnostics, &elemMachineSelf);
                    if (elemMachineSelf) {
                        diagnostics->push_back(
                            QStringLiteral("%1.target[%2]: machine self-target cannot be part of an array target -- dropped")
                                .arg(path).arg(i));
                        allResolved = false;
                        break;
                    }
                    if (resolvedElem == 0) {
                        allResolved = false;
                        break;  // resolveTarget already added diagnostic
                    }
                    resolvedList.append(resolvedElem);
                }
                if (!allResolved) {
                    return;  // drop transition if any target in array failed to resolve
                }
                if (resolvedList.size() == 1) {
                    transition.to = resolvedList.first();
                    transition.targets.clear();
                } else {
                    transition.targets = resolvedList;
                    transition.to = resolvedList.first();
                }
            }
        } else {
            const QString target = targetValue.toString();
            bool machineSelf = false;
            const quint64 resolved = resolveTarget(target, sourceParentId, machineName, siblingsByParent, idsByName,
                                                   path, diagnostics, &machineSelf);
            if (machineSelf) {
                // The root self-target is valid only on a machine-level handler; from
                // a state it has no slot here and is diagnosed.
                if (fromId != 0) {
                    diagnostics->push_back(
                        QStringLiteral("%1.target: '%2' (the machine itself) from a state has no Ordo slot -- dropped")
                            .arg(path, target));
                    return;
                }
                transition.machineSelf = true;  // to stays 0
            } else {
                if (resolved == 0) {
                    return;  // resolveTarget() already pushed the diagnostic
                }
                transition.to = resolved;
            }
        }
    }

    for (const QString& key : object.keys()) {
        const QString keyPath = path + QLatin1Char('.') + key;
        if (key == QStringLiteral("target")) {
            continue;  // handled above
        }
        if (key == QStringLiteral("reenter")) {
            transition.reenter = object.value(key).toBool(false);
        } else if (key == QStringLiteral("guard")) {
            transition.guard = guardFromJson(object.value(key), keyPath, diagnostics);
        } else if (key == QStringLiteral("cond")) {
            diagnostics->push_back(
                QStringLiteral("%1: 'cond' is XState v4 -- v5 (and this importer) spell it 'guard' -- dropped")
                    .arg(keyPath));
        } else if (key == QStringLiteral("actions")) {
            const QJsonValue actionsValue = object.value(key);
            if (actionsValue.isArray()) {
                const QJsonArray array = actionsValue.toArray();
                if (!array.isEmpty()) {
                    transition.action = actionName(array.at(0), keyPath + QStringLiteral("[0]"), diagnostics);
                }
                for (int i = 1; i < array.size(); ++i) {
                    diagnostics->push_back(QStringLiteral("%1[%2]: only the first action maps to this model's "
                                                           "single action slot -- dropped")
                                                .arg(keyPath)
                                                .arg(i));
                }
            } else {
                transition.action = actionName(actionsValue, keyPath, diagnostics);
            }
        } else if (key == QStringLiteral("meta")) {
            const QJsonObject ordo = takeOrdoMeta(object.value(key), keyPath, diagnostics);
            transition.labelOffset = QPointF(ordo.value(QStringLiteral("labelOffsetX")).toDouble(),
                                              ordo.value(QStringLiteral("labelOffsetY")).toDouble());
            // Absent labelRatio means nullopt (router default).
            if (ordo.contains(QStringLiteral("labelRatio"))) {
                transition.labelRatio = ordo.value(QStringLiteral("labelRatio")).toDouble();
            }
            // Absent payloadType imports the event untyped, without a diagnostic.
            if (ordo.contains(QStringLiteral("payloadType"))) {
                transition.payloadType = ordo.value(QStringLiteral("payloadType")).toString();
            }
        } else {
            diagnostics->push_back(QStringLiteral("%1: no Phase-1 mapping -- dropped").arg(keyPath));
        }
    }

    transition.id = machine->nextId++;
    machine->transitions.push_back(transition);
}

// One `on.<event>` / `after.<ms>` value: a single transition or v5's
// guarded-candidate array of them.
void parseTransitions(const QJsonValue& value, const QString& path, quint64 fromId, quint64 sourceParentId,
                      const QString& event, int delayMs, const QString& machineName,
                      const QHash<quint64, QHash<QString, quint64>>& siblingsByParent,
                      const QHash<QString, QVector<quint64>>& idsByName, Machine* machine,
                      QStringList* diagnostics, bool isAlways = false) {
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (int i = 0; i < array.size(); ++i) {
            parseTransition(array.at(i), QStringLiteral("%1[%2]").arg(path).arg(i), fromId, sourceParentId, event,
                            delayMs, machineName, siblingsByParent, idsByName, machine, diagnostics, isAlways);
        }
    } else {
        parseTransition(value, path, fromId, sourceParentId, event, delayMs, machineName, siblingsByParent,
                        idsByName, machine, diagnostics, isAlways);
    }
}

// Recursively mints every state of one level's `"states"` object as a child of
// `parentId` (0 = machine root), indexing it for resolveTarget(). Level order: siblings
// mint before their children, so a flat machine gets ids 1..N in Qt's alphabetical key
// order and descendants land at the end of machine->states. Property parsing is a
// separate later pass, since a transition may target a state nested several levels away.
void mintStates(const QJsonObject& statesJson, quint64 parentId, Machine* machine,
                QHash<quint64, QHash<QString, quint64>>* siblingsByParent,
                QHash<QString, QVector<quint64>>* idsByName) {
    QVector<std::pair<quint64, QJsonObject>> withChildren;
    for (auto it = statesJson.begin(); it != statesJson.end(); ++it) {
        State state;
        state.id = machine->nextId++;
        state.name = it.key();
        state.parentId = parentId;
        machine->states.push_back(state);
        (*siblingsByParent)[parentId].insert(it.key(), state.id);
        (*idsByName)[it.key()].push_back(state.id);
        if (it.value().isObject()) {
            const QJsonValue nested = it.value().toObject().value(QStringLiteral("states"));
            if (nested.isObject()) {
                withChildren.push_back({state.id, nested.toObject()});
            }
        }
    }
    for (const auto& pending : withChildren) {
        mintStates(pending.second, pending.first, machine, siblingsByParent, idsByName);
    }
}

// The per-state property sweep (type/entry/exit/description/tags/on/after/meta), recursing
// into nested `states` and resolving `initial` in the state's own child scope. Mirrors
// mintStates()'s recursion so each JSON object finds the state it was minted as.
void parseStateProperties(const QJsonObject& statesJson, quint64 parentId, const QString& pathPrefix,
                          const QString& machineName,
                          const QHash<quint64, QHash<QString, quint64>>& siblingsByParent,
                          const QHash<QString, QVector<quint64>>& idsByName,
                          const QHash<quint64, int>& indexById, Machine* machine, QSet<quint64>* statesWithGeometry,
                          QStringList* diagnostics) {
    const QHash<QString, quint64> scope = siblingsByParent.value(parentId);
    for (auto it = statesJson.begin(); it != statesJson.end(); ++it) {
        const quint64 stateId = scope.value(it.key());  // minted by mintStates() above
        State& state = machine->states[indexById.value(stateId)];
        const QString statePath = pathPrefix + QLatin1Char('.') + it.key();
        if (!it.value().isObject()) {
            diagnostics->push_back(statePath + QStringLiteral(": not an object -- imported as an empty state"));
            continue;
        }
        const QJsonObject stateObject = it.value().toObject();
        // Set by the `invoke` branch when it claims a completion via the nested
        // onDone/onError slot. The `on` branch runs later in this loop (Qt's
        // alphabetical key order puts "invoke" before "on") and drops a flat
        // spelling of the same completion instead of minting a duplicate.
        QString invokeDoneEvent;
        QString invokeErrorEvent;
        for (const QString& key : stateObject.keys()) {
            const QString keyPath = statePath + QLatin1Char('.') + key;
            const QJsonValue keyValue = stateObject.value(key);
            if (key == QStringLiteral("type")) {
                const QString type = keyValue.toString();
                if (type == QStringLiteral("final")) {
                    state.kind = StateKind::Final;
                } else if (type == QStringLiteral("parallel")) {
                    state.kind = StateKind::Parallel;
                } else if (type == QStringLiteral("history")) {
                    state.kind = StateKind::History;
                } else if (type != QStringLiteral("atomic") && type != QStringLiteral("compound")) {
                    // "compound" is accepted silently: a Normal state with children is one.
                    diagnostics->push_back(
                        QStringLiteral("%1: unknown type '%2' -- treated as Normal").arg(keyPath, type));
                }
            } else if (key == QStringLiteral("history")) {
                // History depth: read unconditionally, but only meaningful on a History
                // state. "shallow" and an absent key both mean false.
                const QString history = keyValue.toString();
                state.historyDeep = history == QStringLiteral("deep");
                if (history != QStringLiteral("deep") && history != QStringLiteral("shallow")) {
                    diagnostics->push_back(
                        QStringLiteral("%1: unknown history value '%2' -- treated as shallow").arg(keyPath, history));
                }
            } else if (key == QStringLiteral("entry")) {
                state.entryActions = nameList(keyValue, keyPath, diagnostics);
            } else if (key == QStringLiteral("exit")) {
                state.exitActions = nameList(keyValue, keyPath, diagnostics);
            } else if (key == QStringLiteral("description")) {
                state.description = keyValue.toString();
            } else if (key == QStringLiteral("tags")) {
                state.tags = nameList(keyValue, keyPath, diagnostics);
            } else if (key == QStringLiteral("meta")) {
                const QJsonObject ordo = takeOrdoMeta(keyValue, keyPath, diagnostics);
                if (ordo.contains(QStringLiteral("x")) || ordo.contains(QStringLiteral("y"))) {
                    state.pos = QPointF(ordo.value(QStringLiteral("x")).toDouble(),
                                         ordo.value(QStringLiteral("y")).toDouble());
                    statesWithGeometry->insert(state.id);
                }
                if (ordo.contains(QStringLiteral("invokeOutputType"))) {
                    // Absent = the default type (Int).
                    state.invokeOutputType =
                        contextTypeFromName(ordo.value(QStringLiteral("invokeOutputType")).toString());
                }
            } else if (key == QStringLiteral("on")) {
                if (!keyValue.isObject()) {
                    diagnostics->push_back(keyPath + QStringLiteral(": not an object -- dropped"));
                    continue;
                }
                const QJsonObject on = keyValue.toObject();
                for (const QString& event : on.keys()) {
                    if (event.trimmed().isEmpty()) {
                        diagnostics->push_back(QStringLiteral("%1.'': blank event key -- dropped").arg(keyPath));
                        continue;
                    }
                    if (!invokeDoneEvent.isEmpty() && event == invokeDoneEvent) {
                        // Both forms present: the nested invoke.onDone wins.
                        diagnostics->push_back(QStringLiteral("%1.%2: invoke.onDone (nested) already describes this "
                                                               "completion -- the nested form wins, this flat `on` "
                                                               "entry is dropped")
                                                    .arg(keyPath, event));
                        continue;
                    }
                    if (!invokeErrorEvent.isEmpty() && event == invokeErrorEvent) {
                        diagnostics->push_back(QStringLiteral("%1.%2: invoke.onError (nested) already describes this "
                                                               "completion -- the nested form wins, this flat `on` "
                                                               "entry is dropped")
                                                    .arg(keyPath, event));
                        continue;
                    }
                    parseTransitions(on.value(event), keyPath + QLatin1Char('.') + event, state.id, state.parentId,
                                     event, 0, machineName, siblingsByParent, idsByName, machine, diagnostics);
                }
            } else if (key == QStringLiteral("after")) {
                if (!keyValue.isObject()) {
                    diagnostics->push_back(keyPath + QStringLiteral(": not an object -- dropped"));
                    continue;
                }
                const QJsonObject after = keyValue.toObject();
                for (const QString& delayKey : after.keys()) {
                    bool numeric = false;
                    const int delayMs = delayKey.toInt(&numeric);
                    if (!numeric || delayMs <= 0) {
                        diagnostics->push_back(
                            QStringLiteral("%1.%2: only positive millisecond delays map in Phase 1 (named "
                                            "delays are deferred) -- dropped")
                                .arg(keyPath, delayKey));
                        continue;
                    }
                    parseTransitions(after.value(delayKey), keyPath + QLatin1Char('.') + delayKey, state.id,
                                     state.parentId, QString(), delayMs, machineName, siblingsByParent, idsByName,
                                     machine, diagnostics);
                }
            } else if (key == QStringLiteral("states")) {
                // Nested states were minted by mintStates(); recurse for their properties.
                if (keyValue.isObject()) {
                    parseStateProperties(keyValue.toObject(), state.id, statePath + QStringLiteral(".states"),
                                         machineName, siblingsByParent, idsByName, indexById, machine,
                                         statesWithGeometry, diagnostics);
                }
            } else if (key == QStringLiteral("initial")) {
                // Resolves in this state's own child scope: a direct child only.
                const QString childName = keyValue.toString();
                const auto childScope = siblingsByParent.constFind(state.id);
                if (childScope != siblingsByParent.constEnd() && childScope->contains(childName)) {
                    state.initialChildId = childScope->value(childName);
                } else {
                    diagnostics->push_back(
                        QStringLiteral("%1: no direct child is named '%2' -- left unset").arg(keyPath, childName));
                }
            } else if (key == QStringLiteral("invoke")) {
                // `invoke` is one object or an array; each entry becomes an Invocation.
                QJsonArray invokes;
                if (keyValue.isArray()) {
                    invokes = keyValue.toArray();
                } else if (keyValue.isObject()) {
                    invokes.append(keyValue);
                } else {
                    diagnostics->push_back(keyPath + QStringLiteral(": neither an invoke object nor an array -- "
                                                                     "dropped"));
                    continue;
                }
                if (invokes.isEmpty()) {
                    diagnostics->push_back(keyPath + QStringLiteral(": empty invoke array -- dropped"));
                } else {
                    for (int i = 0; i < invokes.size(); ++i) {
                        if (!invokes.at(i).isObject()) {
                            diagnostics->push_back(QStringLiteral("%1[%2]: not an object -- dropped").arg(keyPath).arg(i));
                            continue;
                        }
                        const QJsonObject invokeObject = invokes.at(i).toObject();
                        const QString src = actionName(invokeObject.value(QStringLiteral("src")),
                                                       QStringLiteral("%1[%2].src").arg(keyPath).arg(i), diagnostics);
                        QString id;
                        if (invokeObject.contains(QStringLiteral("id"))) {
                            id = invokeObject.value(QStringLiteral("id")).toString().trimmed();
                        }
                        Invocation inv{.src = src, .id = id, .outputType = ContextType::Int};
                        state.invocations.push_back(inv);

                        const QString effectiveId = effectiveInvocationId(inv);
                        if (!effectiveId.isEmpty()) {
                            if (invokeObject.contains(QStringLiteral("onDone"))) {
                                const QString doneEvt = QStringLiteral("done.invoke.") + effectiveId;
                                invokeDoneEvent = doneEvt;
                                parseTransitions(invokeObject.value(QStringLiteral("onDone")),
                                                 QStringLiteral("%1[%2].onDone").arg(keyPath).arg(i), state.id, state.parentId,
                                                 doneEvt, 0, machineName, siblingsByParent, idsByName, machine,
                                                 diagnostics);
                            }
                            if (invokeObject.contains(QStringLiteral("onError"))) {
                                const QString errEvt = QStringLiteral("error.platform.") + effectiveId;
                                invokeErrorEvent = errEvt;
                                parseTransitions(invokeObject.value(QStringLiteral("onError")),
                                                 QStringLiteral("%1[%2].onError").arg(keyPath).arg(i), state.id, state.parentId,
                                                 errEvt, 0, machineName, siblingsByParent, idsByName, machine,
                                                 diagnostics);
                            }
                        } else if (invokeObject.contains(QStringLiteral("onDone")) ||
                                   invokeObject.contains(QStringLiteral("onError"))) {
                            diagnostics->push_back(QStringLiteral("%1[%2]: onDone/onError given but src/id leave no "
                                                                  "effective invoke id -- dropped").arg(keyPath).arg(i));
                        }
                        for (const QString& subKey : invokeObject.keys()) {
                            if (subKey == QStringLiteral("src") || subKey == QStringLiteral("id") ||
                                subKey == QStringLiteral("onDone") || subKey == QStringLiteral("onError")) {
                                continue;
                            }
                            diagnostics->push_back(
                                QStringLiteral("%1[%2].%3: no Phase-1 mapping -- dropped").arg(keyPath).arg(i).arg(subKey));
                        }
                    }
                    if (!state.invocations.isEmpty()) {
                        state.invokeSrc = state.invocations.first().src;
                        state.invokeId = state.invocations.first().id;
                        state.invokeOutputType = state.invocations.first().outputType;
                    }
                }
            } else if (key == QStringLiteral("always")) {
                parseTransitions(keyValue, keyPath, state.id, state.parentId, QString(), 0, machineName,
                                 siblingsByParent, idsByName, machine, diagnostics, /*isAlways=*/true);
            } else {
                diagnostics->push_back(QStringLiteral("%1: no Phase-1 mapping -- dropped").arg(keyPath));
            }
        }
    }
}

// The shortest text that re-parses to exactly the same double (printDouble()'s search
// without the ".0" suffix; an initialValue is read back by type, not re-lexed).
QString doubleInitialValue(double value) {
    for (int precision = 1; precision < 17; ++precision) {
        const QString candidate = QString::number(value, 'g', precision);
        if (candidate.toDouble() == value) {
            return candidate;
        }
    }
    return QString::number(value, 'g', 17);
}

// v5's top-level `context` -> Machine::context (the inverse of contextToJson()). Runs
// last so context ids come off the tail of the id counter. `meta.ordo.context` supplies
// declared type and order; without a schema entry the type is inferred (bool, string,
// integral -> Int, other number -> Double) and order is alphabetical, so a foreign
// Double 0 and Int 0 are indistinguishable.
void parseContext(const QJsonValue& value, const QJsonObject& rootOrdo, Machine* machine, QStringList* diagnostics) {
    if (!value.isObject()) {
        diagnostics->push_back(QStringLiteral("context: not an object -- dropped"));
        return;
    }
    const QJsonObject contextObject = value.toObject();
    if (contextObject.isEmpty()) {
        return;  // `"context": {}` is an empty schema, not a construct that failed to map
    }

    QHash<QString, ContextType> declaredType;
    QStringList order;
    const QJsonArray schema = rootOrdo.value(QStringLiteral("context")).toArray();
    for (int i = 0; i < schema.size(); ++i) {
        const QJsonObject entry = schema.at(i).toObject();
        const QString name = entry.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }
        if (!contextObject.contains(name)) {
            diagnostics->push_back(
                QStringLiteral("meta.ordo.context[%1]: '%2' has no value in `context` -- dropped").arg(i).arg(name));
            continue;
        }
        if (!order.contains(name)) {
            order.push_back(name);
        }
        declaredType.insert(name, contextTypeFromName(entry.value(QStringLiteral("type")).toString()));
    }
    if (schema.isEmpty()) {
        diagnostics->push_back(
            QStringLiteral("context: no meta.ordo.context schema -- variable types inferred from the values (an "
                            "integral number reads as Int, never Double) and ordered by key"));
    }
    for (const QString& key : contextObject.keys()) {
        if (!order.contains(key)) {
            order.push_back(key);
        }
    }

    for (const QString& name : order) {
        const QJsonValue entryValue = contextObject.value(name);
        ContextVariable variable;
        variable.name = name;
        if (entryValue.isBool()) {
            variable.type = ContextType::Bool;
            variable.initialValue =
                entryValue.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        } else if (entryValue.isDouble()) {
            const double number = entryValue.toDouble();
            const bool integral = std::isfinite(number) && number == std::trunc(number) && std::fabs(number) < 9.2e18;
            variable.type = integral ? ContextType::Int : ContextType::Double;
            variable.initialValue =
                integral ? QString::number(static_cast<qint64>(number)) : doubleInitialValue(number);
        } else if (entryValue.isString()) {
            variable.type = ContextType::String;
            variable.initialValue = entryValue.toString();
        } else if (entryValue.isObject()) {
            variable.type = ContextType::Object;
            variable.initialValue = QString::fromUtf8(QJsonDocument(entryValue.toObject()).toJson(QJsonDocument::Compact));
        } else {
            // Arrays are unsupported.
            diagnostics->push_back(QStringLiteral("context.%1: only Bool/Int/Double/String/Object values map (this editor's "
                                                   "context has no array type) -- dropped")
                                        .arg(name));
            continue;
        }
        const auto declared = declaredType.constFind(name);
        if (declared != declaredType.constEnd() && declared.value() != variable.type) {
            if ((declared.value() == ContextType::Int || declared.value() == ContextType::Double) &&
                (variable.type == ContextType::Int || variable.type == ContextType::Double)) {
                // Int 0 and Double 0 are one JSON number: the declared type wins.
                variable.type = declared.value();
            } else {
                // Hand-edited disagreement: the value is what XState runs with, so it wins.
                diagnostics->push_back(QStringLiteral("meta.ordo.context: '%1' is declared %2 but its value is %3 -- "
                                                       "the value wins")
                                            .arg(name, contextTypeName(declared.value()),
                                                 contextTypeName(variable.type)));
            }
        }
        variable.id = machine->nextId++;
        machine->context.push_back(variable);
    }
}

}  // namespace

XStateImportResult machineFromXStateJson(const QJsonObject& json) {
    XStateImportResult result;

    const QJsonValue statesValue = json.value(QStringLiteral("states"));
    if (!statesValue.isObject()) {
        result.ok = false;
        result.error = QStringLiteral("no `states` object -- not an XState machine config");
        return result;
    }
    const QJsonObject statesObject = statesValue.toObject();
    Machine& machine = result.machine;

    machine.name = json.value(QStringLiteral("id")).toString();
    if (machine.name.isEmpty()) {
        machine.name = QStringLiteral("Imported");
        result.diagnostics.push_back(QStringLiteral("id: missing -- machine named 'Imported'"));
    }

    // Mint every state in the tree (root and nested) before any transition resolves.
    // `siblingsByParent` (parentId -> {name -> id}, root keyed 0) serves resolveTarget()'s
    // rule (a); `idsByName` (name -> all ids sharing it) serves rule (c).
    QHash<quint64, QHash<QString, quint64>> siblingsByParent;
    QHash<QString, QVector<quint64>> idsByName;
    mintStates(statesObject, /*parentId=*/0, &machine, &siblingsByParent, &idsByName);
    QHash<quint64, int> indexById;
    for (int i = 0; i < machine.states.size(); ++i) {
        indexById.insert(machine.states.at(i).id, i);
    }

    // The machine-level `initial` names a root-scope child, not a nested descendant,
    // so it resolves against root scope rather than resolveTarget()'s wider ladder.
    const QString initialName = json.value(QStringLiteral("initial")).toString();
    if (!initialName.isEmpty()) {
        const QHash<QString, quint64> rootScope = siblingsByParent.value(0);
        if (rootScope.contains(initialName)) {
            machine.initialStateId = rootScope.value(initialName);
        } else {
            result.diagnostics.push_back(
                QStringLiteral("initial: no state is named '%1' -- left unset (a validator Error)").arg(initialName));
        }
    }

    // Root sweep: `id`/`initial`/`states`/`meta` are handled elsewhere. Root `on`/`after`/
    // `always` become from == 0 transitions; other keys are diagnosed. `context` is held
    // for parseContext(); the flag records the key's presence, because a default
    // QJsonValue is Null, not Undefined.
    QJsonValue contextValue;
    bool hasContextKey = false;
    for (const QString& key : json.keys()) {
        if (key == QStringLiteral("id") || key == QStringLiteral("initial") || key == QStringLiteral("states") ||
            key == QStringLiteral("meta")) {
            continue;
        }
        const QJsonValue keyValue = json.value(key);
        if (key == QStringLiteral("on")) {
            if (!keyValue.isObject()) {
                result.diagnostics.push_back(QStringLiteral("on: not an object -- dropped"));
                continue;
            }
            const QJsonObject on = keyValue.toObject();
            for (const QString& event : on.keys()) {
                if (event.trimmed().isEmpty()) {
                    result.diagnostics.push_back(QStringLiteral("on.'': blank event key -- dropped"));
                    continue;
                }
                parseTransitions(on.value(event), QStringLiteral("on.") + event, 0, /*sourceParentId=*/0, event, 0,
                                 machine.name, siblingsByParent, idsByName, &machine, &result.diagnostics);
            }
        } else if (key == QStringLiteral("after")) {
            if (!keyValue.isObject()) {
                result.diagnostics.push_back(QStringLiteral("after: not an object -- dropped"));
                continue;
            }
            const QJsonObject after = keyValue.toObject();
            for (const QString& delayKey : after.keys()) {
                bool numeric = false;
                const int delayMs = delayKey.toInt(&numeric);
                if (!numeric || delayMs <= 0) {
                    result.diagnostics.push_back(
                        QStringLiteral("after.%1: only positive millisecond delays map in Phase 1 (named "
                                        "delays are deferred) -- dropped")
                            .arg(delayKey));
                    continue;
                }
                parseTransitions(after.value(delayKey), QStringLiteral("after.") + delayKey, 0,
                                 /*sourceParentId=*/0, QString(), delayMs, machine.name, siblingsByParent, idsByName,
                                 &machine, &result.diagnostics);
            }
        } else if (key == QStringLiteral("always")) {
            parseTransitions(keyValue, QStringLiteral("always"), 0, /*sourceParentId=*/0, QString(), 0,
                             machine.name, siblingsByParent, idsByName, &machine, &result.diagnostics,
                             /*isAlways=*/true);
        } else if (key == QStringLiteral("context")) {
            // Held: its schema is in root `meta.ordo`, and its ids must mint after all others.
            contextValue = keyValue;
            hasContextKey = true;
        } else {
            result.diagnostics.push_back(QStringLiteral("%1: no Phase-1 mapping -- dropped").arg(key));
        }
    }
    // Root meta: ours carries the formatVersion stamp (ignored on read) and the
    // `context` schema for parseContext(); foreign keys are diagnosed in takeOrdoMeta.
    const QJsonObject rootOrdo =
        takeOrdoMeta(json.value(QStringLiteral("meta")), QStringLiteral("meta"), &result.diagnostics);

    // Per-state properties, recursing into nested `states`.
    QSet<quint64> statesWithGeometry;
    parseStateProperties(statesObject, /*parentId=*/0, QStringLiteral("states"), machine.name, siblingsByParent,
                         idsByName, indexById, &machine, &statesWithGeometry, &result.diagnostics);

    // Context last: its ids come off the tail of the counter, shifting nothing above.
    if (hasContextKey) {
        parseContext(contextValue, rootOrdo, &machine, &result.diagnostics);
    }

    // Importers never guess geometry: a file with no meta.ordo leaves every State::pos at
    // the (0,0) origin for DocumentSession to lay out. A mixed file keeps what it has, and
    // each gap is diagnosed.
    if (!statesWithGeometry.isEmpty() && statesWithGeometry.size() < machine.states.size()) {
        for (const State& state : machine.states) {
            if (!statesWithGeometry.contains(state.id)) {
                result.diagnostics.push_back(
                    QStringLiteral("states.%1.meta.ordo: missing while other states carry geometry -- placed "
                                    "at (0,0)")
                        .arg(state.name));
            }
        }
    }

    return result;
}

XStateImportResult importXStateFile(const QString& path) {
    XStateImportResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.ok = false;
        result.error = file.errorString();
        return result;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        result.ok = false;
        result.error = parseError.errorString();
        return result;
    }
    if (!document.isObject()) {
        result.ok = false;
        result.error = QStringLiteral("root is not a JSON object");
        return result;
    }
    return machineFromXStateJson(document.object());
}

bool exportXStateFile(const Machine& machine, const QString& path, QStringList* diagnostics, QString* error) {
    const XStateExportResult result = machineToXStateJson(machine);
    if (diagnostics != nullptr) {
        *diagnostics = result.diagnostics;
    }
    if (!result.ok) {
        if (error != nullptr) {
            *error = result.error;
        }
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    const QJsonDocument document(result.json);
    if (file.write(document.toJson(QJsonDocument::Indented)) < 0) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

}  // namespace app
