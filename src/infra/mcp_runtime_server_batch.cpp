#include "infra/mcp_runtime_server.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QStringList>

#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "view/shell/document_session.h"
#include "view/shell/main_window.h"

namespace app {

QJsonObject McpRuntimeServer::handleBatchCreateMachine(const QJsonObject& params) {
    if (!window_) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active window.");
        return err;
    }

    const QString machineName = params.value(QStringLiteral("name")).toString(QStringLiteral("Untitled"));
    DocumentSession* session = window_->createNewMachine(machineName);
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32000;
        err[QStringLiteral("message")] = QStringLiteral("Failed to create new machine.");
        return err;
    }

    ensureDesignMode(session);

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("MachineDocAgent not found.");
        return err;
    }

    // 1. Context variables
    if (params.contains(QStringLiteral("context")) && params.value(QStringLiteral("context")).isArray()) {
        const QJsonArray ctxArr = params.value(QStringLiteral("context")).toArray();
        for (const QJsonValue& cvVal : ctxArr) {
            if (!cvVal.isObject()) continue;
            const QJsonObject cvObj = cvVal.toObject();
            const QString varName = cvObj.value(QStringLiteral("name")).toString().trimmed();
            if (varName.isEmpty()) continue;

            const QString typeStr = cvObj.value(QStringLiteral("type")).toString(QStringLiteral("int")).toLower();
            ContextType cType = ContextType::Int;
            if (typeStr == QStringLiteral("bool")) {
                cType = ContextType::Bool;
            } else if (typeStr == QStringLiteral("double") || typeStr == QStringLiteral("float")) {
                cType = ContextType::Double;
            } else if (typeStr == QStringLiteral("string")) {
                cType = ContextType::String;
            } else if (typeStr == QStringLiteral("object")) {
                cType = ContextType::Object;
            }

            QString initialVal = cvObj.value(QStringLiteral("initialValue")).toString();
            if (initialVal.isEmpty() && cvObj.contains(QStringLiteral("initialValue"))) {
                const QJsonValue iv = cvObj.value(QStringLiteral("initialValue"));
                if (iv.isBool()) {
                    initialVal = iv.toBool() ? QStringLiteral("true") : QStringLiteral("false");
                } else if (iv.isDouble()) {
                    initialVal = QString::number(iv.toDouble());
                }
            }

            session->kernel().send(events::AddContextVariableRequested{});
            if (!doc->machine().context.empty()) {
                const quint64 varId = doc->machine().context.back().id;
                session->kernel().send(events::RenameContextVariableRequested{.id = varId, .name = varName});
                if (cType != ContextType::Int) {
                    session->kernel().send(events::SetContextTypeRequested{.id = varId, .type = cType});
                }
                if (!initialVal.isEmpty() && initialVal != QStringLiteral("0")) {
                    session->kernel().send(events::SetContextInitialValueRequested{.id = varId, .initialValue = initialVal});
                }
            }
        }
    }

    // 2. States
    QHash<QString, quint64> nameToId;
    struct PendingStateInfo {
        quint64 stateId;
        QJsonValue parentVal;
    };
    QList<PendingStateInfo> pendingParents;

    if (params.contains(QStringLiteral("states")) && params.value(QStringLiteral("states")).isArray()) {
        const QJsonArray statesArr = params.value(QStringLiteral("states")).toArray();
        for (const QJsonValue& sVal : statesArr) {
            if (!sVal.isObject()) continue;
            const QJsonObject sObj = sVal.toObject();
            const QString sName = sObj.value(QStringLiteral("name")).toString().trimmed();
            if (sName.isEmpty()) continue;

            const double x = sObj.value(QStringLiteral("x")).toDouble(120.0);
            const double y = sObj.value(QStringLiteral("y")).toDouble(120.0);
            const QString kindStr = sObj.value(QStringLiteral("kind")).toString(QStringLiteral("normal")).toLower();

            StateKind kind = StateKind::Normal;
            bool historyDeep = false;
            if (kindStr == QStringLiteral("parallel")) {
                kind = StateKind::Parallel;
            } else if (kindStr == QStringLiteral("final")) {
                kind = StateKind::Final;
            } else if (kindStr == QStringLiteral("history") || kindStr == QStringLiteral("historyshallow")) {
                kind = StateKind::History;
                historyDeep = false;
            } else if (kindStr == QStringLiteral("historydeep")) {
                kind = StateKind::History;
                historyDeep = true;
            }

            session->kernel().send(events::AddStateRequested{
                .pos = QPointF(x, y),
                .parentId = 0
            });

            if (doc->machine().states.empty()) continue;
            const quint64 newId = doc->machine().states.back().id;
            nameToId.insert(sName, newId);

            session->kernel().send(events::RenameStateRequested{.id = newId, .name = sName});
            if (kind != StateKind::Normal) {
                session->kernel().send(events::SetStateKindRequested{.id = newId, .kind = kind});
            }
            if (kind == StateKind::History && historyDeep) {
                session->kernel().send(events::SetHistoryDeepRequested{.stateId = newId, .deep = true});
            }

            if (sObj.contains(QStringLiteral("entryActions")) && sObj.value(QStringLiteral("entryActions")).isArray()) {
                QStringList entries;
                for (const QJsonValue& a : sObj.value(QStringLiteral("entryActions")).toArray()) {
                    if (a.isString()) entries.append(a.toString());
                }
                session->kernel().send(events::SetEntryActionsRequested{.id = newId, .entryActions = entries});
            }
            if (sObj.contains(QStringLiteral("exitActions")) && sObj.value(QStringLiteral("exitActions")).isArray()) {
                QStringList exits;
                for (const QJsonValue& a : sObj.value(QStringLiteral("exitActions")).toArray()) {
                    if (a.isString()) exits.append(a.toString());
                }
                session->kernel().send(events::SetExitActionsRequested{.id = newId, .exitActions = exits});
            }

            if (sObj.contains(QStringLiteral("invocations")) && sObj.value(QStringLiteral("invocations")).isArray()) {
                QVector<Invocation> invocations;
                for (const QJsonValue& iv : sObj.value(QStringLiteral("invocations")).toArray()) {
                    if (!iv.isObject()) continue;
                    Invocation inv = invocationFromJson(iv.toObject());
                    if (inv.src.isEmpty()) continue;
                    invocations.append(inv);
                }
                if (!invocations.isEmpty()) {
                    session->kernel().send(events::SetInvocationsRequested{.stateId = newId, .invocations = invocations});
                }
            }

            QJsonValue pVal = sObj.value(QStringLiteral("parentId"));
            if (pVal.isUndefined() || pVal.isNull()) {
                pVal = sObj.value(QStringLiteral("parent"));
            }
            if (!pVal.isUndefined() && !pVal.isNull()) {
                pendingParents.append({newId, pVal});
            }
        }

        for (const auto& pending : pendingParents) {
            quint64 pId = 0;
            if (pending.parentVal.isString()) {
                pId = nameToId.value(pending.parentVal.toString(), 0);
            } else {
                pId = static_cast<quint64>(pending.parentVal.toInteger(0));
            }
            if (pId != 0) {
                session->kernel().send(events::ReparentStateRequested{.id = pending.stateId, .parentId = pId});
            }
        }
    }

    // 3. Initial State
    if (params.contains(QStringLiteral("initialState"))) {
        const QJsonValue initVal = params.value(QStringLiteral("initialState"));
        quint64 initId = 0;
        if (initVal.isString()) {
            initId = nameToId.value(initVal.toString(), 0);
        } else {
            initId = static_cast<quint64>(initVal.toInteger(0));
        }
        if (initId != 0) {
            session->kernel().send(events::SetInitialStateRequested{.id = initId});
        }
    }

    // 4. Transitions
    if (params.contains(QStringLiteral("transitions")) && params.value(QStringLiteral("transitions")).isArray()) {
        const QJsonArray transArr = params.value(QStringLiteral("transitions")).toArray();
        for (const QJsonValue& tVal : transArr) {
            if (!tVal.isObject()) continue;
            const QJsonObject tObj = tVal.toObject();

            QJsonValue srcVal = tObj.value(QStringLiteral("source"));
            if (srcVal.isUndefined() || srcVal.isNull()) srcVal = tObj.value(QStringLiteral("sourceId"));
            if (srcVal.isUndefined() || srcVal.isNull()) srcVal = tObj.value(QStringLiteral("from"));

            QJsonValue tgtVal = tObj.value(QStringLiteral("target"));
            if (tgtVal.isUndefined() || tgtVal.isNull()) tgtVal = tObj.value(QStringLiteral("targetId"));
            if (tgtVal.isUndefined() || tgtVal.isNull()) tgtVal = tObj.value(QStringLiteral("to"));

            quint64 fromId = 0;
            if (srcVal.isString()) {
                fromId = nameToId.value(srcVal.toString(), 0);
            } else {
                fromId = static_cast<quint64>(srcVal.toInteger(0));
            }

            quint64 toId = 0;
            if (tgtVal.isString()) {
                toId = nameToId.value(tgtVal.toString(), 0);
            } else {
                toId = static_cast<quint64>(tgtVal.toInteger(0));
            }

            if (fromId == 0 || toId == 0) continue;

            session->kernel().send(events::AddTransitionRequested{.from = fromId, .to = toId});
            if (doc->machine().transitions.empty()) continue;
            const quint64 newTId = doc->machine().transitions.back().id;

            const QString event = tObj.value(QStringLiteral("event")).toString();
            const QString guard = tObj.value(QStringLiteral("guard")).toString();
            const QString action = tObj.value(QStringLiteral("action")).toString();
            bool isAlways = tObj.value(QStringLiteral("isAlways")).toBool(false);
            if (!isAlways && tObj.contains(QStringLiteral("always"))) {
                isAlways = tObj.value(QStringLiteral("always")).toBool(false);
            }
            const bool reenter = tObj.value(QStringLiteral("reenter")).toBool(false);

            if (!event.isEmpty()) {
                session->kernel().send(events::SetTransitionEventRequested{.id = newTId, .event = event});
            }
            if (!guard.isEmpty()) {
                session->kernel().send(events::SetTransitionGuardRequested{.id = newTId, .guard = guard});
            }
            if (!action.isEmpty()) {
                session->kernel().send(events::SetTransitionActionRequested{.id = newTId, .action = action});
            }
            if (isAlways) {
                session->kernel().send(events::SetTransitionAlwaysRequested{.id = newTId, .always = true});
            }
            if (reenter) {
                session->kernel().send(events::SetTransitionReenterRequested{.id = newTId, .reenter = true});
            }
        }
    }

    qint64 invocationTotal = 0;
    for (const auto& s : doc->machine().states) {
        invocationTotal += s.effectiveInvocations().size();
    }

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("machineName")] = session->machineName();
    res[QStringLiteral("stateCount")] = static_cast<qint64>(doc->machine().states.size());
    res[QStringLiteral("transitionCount")] = static_cast<qint64>(doc->machine().transitions.size());
    res[QStringLiteral("contextCount")] = static_cast<qint64>(doc->machine().context.size());
    res[QStringLiteral("invocationCount")] = invocationTotal;
    return res;
}

}  // namespace app
