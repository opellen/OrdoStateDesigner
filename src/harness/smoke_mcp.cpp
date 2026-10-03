#include <cstdio>

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include "harness/harness.h"
#include "infra/mcp_runtime_server.h"
#include "view/shell/main_window.h"

int runMcpSmoke() {
    std::printf("[SMOKE] Running McpRuntimeServer JSON-RPC smoke...\n");

    app::McpRuntimeServer server(nullptr);

    // 1. Malformed JSON -> Parse error (-32700)
    {
        QByteArray res = server.processLine("invalid-json{");
        QJsonObject doc = QJsonDocument::fromJson(res).object();
        if (doc.value("error").toObject().value("code").toInt() != -32700) {
            std::fprintf(stderr, "FAIL: expected parse error (-32700) for malformed JSON\n");
            return 1;
        }
    }

    // 2. Missing jsonrpc or method -> Invalid Request (-32600)
    {
        QByteArray res = server.processLine("{\"id\": 1}");
        QJsonObject doc = QJsonDocument::fromJson(res).object();
        if (doc.value("error").toObject().value("code").toInt() != -32600) {
            std::fprintf(stderr, "FAIL: expected invalid request (-32600) for missing jsonrpc/method\n");
            return 1;
        }
    }

    // 3. Unknown method -> Method not found (-32601)
    {
        QByteArray res = server.processLine("{\"jsonrpc\": \"2.0\", \"id\": 2, \"method\": \"unknown_command\"}");
        QJsonObject doc = QJsonDocument::fromJson(res).object();
        if (doc.value("error").toObject().value("code").toInt() != -32601) {
            std::fprintf(stderr, "FAIL: expected method not found (-32601)\n");
            return 1;
        }
    }

    // 4. Methods with null window return clean application error (-32001)
    {
        QByteArray res = server.processLine("{\"jsonrpc\": \"2.0\", \"id\": 3, \"method\": \"get_active_machine\"}");
        QJsonObject doc = QJsonDocument::fromJson(res).object();
        if (doc.value("error").toObject().value("code").toInt() != -32001) {
            std::fprintf(stderr, "FAIL: expected no active session error (-32001) with null window\n");
            return 1;
        }
    }

    // 5. Missing required params for add_state (without window)
    {
        QByteArray res = server.processLine("{\"jsonrpc\": \"2.0\", \"id\": 4, \"method\": \"add_state\", \"params\": {}}");
        QJsonObject doc = QJsonDocument::fromJson(res).object();
        if (!doc.contains("error")) {
            std::fprintf(stderr, "FAIL: expected error response for add_state\n");
            return 1;
        }
    }

    // 6. Verify all new GUI-parity mutation methods are registered and return -32001 without window
    {
        const char* methods[] = {
            "rename_state", "set_state_kind", "delete_state", "reparent_state",
            "set_initial_state", "update_transition", "set_transition_flags",
            "delete_transition", "add_context_variable", "update_context_variable",
            "delete_context_variable", "close_project",
            "set_invocations", "add_invocation", "delete_invocation", "complete_invocation",
            "apply_auto_layout"
        };
        for (const char* m : methods) {
            QByteArray req = QByteArray("{\"jsonrpc\": \"2.0\", \"id\": 10, \"method\": \"") + m + "\", \"params\": {}}";
            QByteArray res = server.processLine(req);
            QJsonObject doc = QJsonDocument::fromJson(res).object();
            int errCode = doc.value("error").toObject().value("code").toInt();
            if (errCode == -32601) {
                std::fprintf(stderr, "FAIL: method %s not found (-32601)\n", m);
                return 1;
            }
            if (errCode != -32001) {
                std::fprintf(stderr, "FAIL: method %s expected -32001 with null window, got %d\n", m, errCode);
                return 1;
            }
        }
    }

    // 7. IPC Server start and stop lifecycle
    {
        bool started = server.start(QStringLiteral("state-designer-smoke-test-pipe"));
        if (!started || !server.isListening()) {
            std::fprintf(stderr, "FAIL: McpRuntimeServer failed to start on smoke pipe\n");
            return 1;
        }
        server.stop();
        if (server.isListening()) {
            std::fprintf(stderr, "FAIL: McpRuntimeServer still listening after stop\n");
            return 1;
        }
    }

    std::printf("[SMOKE] McpRuntimeServer smoke PASS\n");
    return 0;
}

namespace {

QJsonObject mcpRequest(int id, const QString& method, const QJsonObject& params) {
    QJsonObject req;
    req[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    req[QStringLiteral("id")] = id;
    req[QStringLiteral("method")] = method;
    req[QStringLiteral("params")] = params;
    return req;
}

// QStringList::contains is whole-element equality, not substring, so a
// fragment assertion on it never passes; this checks CONTAINS per element.
bool historyContainsFragment(const QJsonArray& history, const QString& fragment) {
    for (const QJsonValue& v : history) {
        if (v.toString().contains(fragment)) {
            return true;
        }
    }
    return false;
}

bool jsonArrayContainsString(const QJsonArray& arr, const QString& value) {
    for (const QJsonValue& v : arr) {
        if (v.toString() == value) {
            return true;
        }
    }
    return false;
}

QJsonObject findStateByName(const QJsonObject& machineResult, const QString& name) {
    for (const QJsonValue& sv : machineResult.value(QStringLiteral("states")).toArray()) {
        const QJsonObject sObj = sv.toObject();
        if (sObj.value(QStringLiteral("name")).toString() == name) {
            return sObj;
        }
    }
    return QJsonObject();
}

QStringList invocationEffectiveIds(const QJsonObject& stateObj) {
    QStringList ids;
    for (const QJsonValue& iv : stateObj.value(QStringLiteral("invocations")).toArray()) {
        ids.append(iv.toObject().value(QStringLiteral("effectiveId")).toString());
    }
    return ids;
}

}  // namespace

// Behavioural companion to runMcpSmoke(), which only proves every method
// dispatches. A bare "done.invoke.<id>" via step_simulation leaves `output`
// unbound ("assign skipped: ..."); only a live run and its trace show the
// difference from a real "assign: sampleCount = 128".
// Needs a real MainWindow/DocumentSession, so it constructs its own
// QApplication, never nested with runShellSmoke()'s.
int runMcpInvokeSmoke() {
    int fakeArgc = 1;
    char fakeArgv0[] = "state-designer-mcp-invoke-smoke";
    char* fakeArgv[] = {fakeArgv0};
    QApplication application(fakeArgc, fakeArgv);

    std::printf("[SMOKE] Running McpRuntimeServer invoke completion smoke...\n");

    app::MainWindow window;
    app::McpRuntimeServer server(&window);
    int nextId = 1;

    // ---- 1. Batch authoring: a state carrying an invocations[] entry --------
    QJsonArray contextArr;
    {
        QJsonObject sampleCountVar;
        sampleCountVar[QStringLiteral("name")] = QStringLiteral("sampleCount");
        sampleCountVar[QStringLiteral("type")] = QStringLiteral("int");
        sampleCountVar[QStringLiteral("initialValue")] = QStringLiteral("0");
        contextArr.append(sampleCountVar);

        QJsonObject lastErrorVar;
        lastErrorVar[QStringLiteral("name")] = QStringLiteral("lastError");
        lastErrorVar[QStringLiteral("type")] = QStringLiteral("string");
        lastErrorVar[QStringLiteral("initialValue")] = QString();
        contextArr.append(lastErrorVar);
    }

    QJsonArray statesArr;
    {
        QJsonObject idle;
        idle[QStringLiteral("name")] = QStringLiteral("Idle");
        idle[QStringLiteral("x")] = 80.0;
        idle[QStringLiteral("y")] = 160.0;
        statesArr.append(idle);

        QJsonObject fetching;
        fetching[QStringLiteral("name")] = QStringLiteral("Fetching");
        fetching[QStringLiteral("x")] = 320.0;
        fetching[QStringLiteral("y")] = 160.0;
        QJsonArray fetchingInvocations;
        QJsonObject fetchInv;
        fetchInv[QStringLiteral("src")] = QStringLiteral("fetchSamples");
        fetchInv[QStringLiteral("id")] = QStringLiteral("fetch");
        fetchInv[QStringLiteral("outputType")] = QStringLiteral("int");
        fetchingInvocations.append(fetchInv);
        fetching[QStringLiteral("invocations")] = fetchingInvocations;
        statesArr.append(fetching);

        QJsonObject doneState;
        doneState[QStringLiteral("name")] = QStringLiteral("Done");
        doneState[QStringLiteral("x")] = 560.0;
        doneState[QStringLiteral("y")] = 100.0;
        statesArr.append(doneState);

        QJsonObject failedState;
        failedState[QStringLiteral("name")] = QStringLiteral("Failed");
        failedState[QStringLiteral("x")] = 560.0;
        failedState[QStringLiteral("y")] = 220.0;
        statesArr.append(failedState);
    }

    QJsonArray transitionsArr;
    {
        QJsonObject startTrans;
        startTrans[QStringLiteral("source")] = QStringLiteral("Idle");
        startTrans[QStringLiteral("target")] = QStringLiteral("Fetching");
        startTrans[QStringLiteral("event")] = QStringLiteral("Start");
        transitionsArr.append(startTrans);

        QJsonObject doneTrans;
        doneTrans[QStringLiteral("source")] = QStringLiteral("Fetching");
        doneTrans[QStringLiteral("target")] = QStringLiteral("Done");
        doneTrans[QStringLiteral("event")] = QStringLiteral("done.invoke.fetch");
        doneTrans[QStringLiteral("action")] = QStringLiteral("sampleCount = output");
        transitionsArr.append(doneTrans);

        QJsonObject errorTrans;
        errorTrans[QStringLiteral("source")] = QStringLiteral("Fetching");
        errorTrans[QStringLiteral("target")] = QStringLiteral("Failed");
        errorTrans[QStringLiteral("event")] = QStringLiteral("error.platform.fetch");
        errorTrans[QStringLiteral("action")] = QStringLiteral("lastError = error");
        transitionsArr.append(errorTrans);
    }

    QJsonObject batchParams;
    batchParams[QStringLiteral("name")] = QStringLiteral("InvokeSmokeMachine");
    batchParams[QStringLiteral("context")] = contextArr;
    batchParams[QStringLiteral("states")] = statesArr;
    batchParams[QStringLiteral("transitions")] = transitionsArr;
    batchParams[QStringLiteral("initialState")] = QStringLiteral("Idle");

    const QJsonObject batchResp = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("batch_create_machine"), batchParams));
    const QJsonObject batchResult = batchResp.value(QStringLiteral("result")).toObject();
    if (!batchResp.contains(QStringLiteral("result")) || !batchResult.value(QStringLiteral("success")).toBool(false)) {
        std::fprintf(stderr, "FAIL: batch_create_machine did not succeed authoring InvokeSmokeMachine\n");
        return 1;
    }
    if (batchResult.value(QStringLiteral("invocationCount")).toInt(-1) != 1) {
        std::fprintf(stderr,
                     "FAIL: batch_create_machine reported invocationCount %d (want 1) -- the Fetching state's "
                     "invocations[] entry was not authored\n",
                     batchResult.value(QStringLiteral("invocationCount")).toInt(-1));
        return 1;
    }

    // ---- 2. Read-back: get_active_machine reports the invocation, and an ----
    // ---- empty (present, not absent) array for a state with none -----------
    QJsonObject activeResult = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("get_active_machine"), QJsonObject()))
                                   .value(QStringLiteral("result"))
                                   .toObject();
    const QJsonObject fetchingState0 = findStateByName(activeResult, QStringLiteral("Fetching"));
    const qint64 fetchingStateId = fetchingState0.value(QStringLiteral("id")).toInteger(-1);
    if (fetchingStateId < 0) {
        std::fprintf(stderr, "FAIL: get_active_machine did not report a Fetching state with a valid id\n");
        return 1;
    }
    const QJsonArray fetchingInvocations0 = fetchingState0.value(QStringLiteral("invocations")).toArray();
    if (fetchingInvocations0.size() != 1) {
        std::fprintf(stderr, "FAIL: get_active_machine reported %d invocation(s) on Fetching (want 1)\n",
                     static_cast<int>(fetchingInvocations0.size()));
        return 1;
    }
    const QJsonObject fetchInvJson = fetchingInvocations0.first().toObject();
    if (fetchInvJson.value(QStringLiteral("src")).toString() != QStringLiteral("fetchSamples") ||
        fetchInvJson.value(QStringLiteral("id")).toString() != QStringLiteral("fetch") ||
        fetchInvJson.value(QStringLiteral("outputType")).toString() != QStringLiteral("int") ||
        fetchInvJson.value(QStringLiteral("effectiveId")).toString() != QStringLiteral("fetch")) {
        std::fprintf(stderr,
                     "FAIL: get_active_machine's Fetching invocation entry does not match "
                     "{src=fetchSamples,id=fetch,outputType=int,effectiveId=fetch}\n");
        return 1;
    }
    const QJsonObject idleState0 = findStateByName(activeResult, QStringLiteral("Idle"));
    if (!idleState0.contains(QStringLiteral("invocations")) || !idleState0.value(QStringLiteral("invocations")).isArray() ||
        !idleState0.value(QStringLiteral("invocations")).toArray().isEmpty()) {
        std::fprintf(stderr,
                     "FAIL: get_active_machine did not report an empty (but PRESENT) invocations array for Idle, "
                     "which declares none\n");
        return 1;
    }

    // ---- 3. add_invocation appends, leaving the first intact ----------------
    QJsonObject addParams;
    addParams[QStringLiteral("name")] = QStringLiteral("Fetching");
    addParams[QStringLiteral("src")] = QStringLiteral("fetchExtra");
    addParams[QStringLiteral("id")] = QStringLiteral("extra");
    addParams[QStringLiteral("outputType")] = QStringLiteral("bool");
    const QJsonObject addResp = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("add_invocation"), addParams));
    const QJsonObject addResult = addResp.value(QStringLiteral("result")).toObject();
    if (!addResp.contains(QStringLiteral("result")) || addResult.value(QStringLiteral("effectiveId")).toString() != QStringLiteral("extra") ||
        addResult.value(QStringLiteral("invocationCount")).toInt(-1) != 2) {
        std::fprintf(stderr, "FAIL: add_invocation did not append a second invocation (effectiveId='extra', invocationCount=2)\n");
        return 1;
    }
    activeResult = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("get_active_machine"), QJsonObject()))
                       .value(QStringLiteral("result"))
                       .toObject();
    const QStringList idsAfterAdd = invocationEffectiveIds(findStateByName(activeResult, QStringLiteral("Fetching")));
    if (idsAfterAdd.size() != 2 || !idsAfterAdd.contains(QStringLiteral("fetch")) || !idsAfterAdd.contains(QStringLiteral("extra"))) {
        std::fprintf(stderr,
                     "FAIL: after add_invocation, Fetching's invocations are [%s] (want BOTH 'fetch' and 'extra' -- "
                     "the append must leave the first entry intact)\n",
                     idsAfterAdd.join(QStringLiteral(", ")).toUtf8().constData());
        return 1;
    }

    // ---- 4. delete_invocation removes the right one --------------------------
    QJsonObject delParams;
    delParams[QStringLiteral("name")] = QStringLiteral("Fetching");
    delParams[QStringLiteral("invokeId")] = QStringLiteral("extra");
    const QJsonObject delResp = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("delete_invocation"), delParams));
    const QJsonObject delResult = delResp.value(QStringLiteral("result")).toObject();
    if (!delResp.contains(QStringLiteral("result")) || delResult.value(QStringLiteral("removedEffectiveId")).toString() != QStringLiteral("extra") ||
        delResult.value(QStringLiteral("invocationCount")).toInt(-1) != 1) {
        std::fprintf(stderr, "FAIL: delete_invocation(invokeId='extra') did not report removedEffectiveId='extra'/invocationCount=1\n");
        return 1;
    }
    activeResult = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("get_active_machine"), QJsonObject()))
                       .value(QStringLiteral("result"))
                       .toObject();
    const QStringList idsAfterDelete = invocationEffectiveIds(findStateByName(activeResult, QStringLiteral("Fetching")));
    if (idsAfterDelete.size() != 1 || idsAfterDelete.first() != QStringLiteral("fetch")) {
        std::fprintf(stderr,
                     "FAIL: after delete_invocation('extra'), Fetching's invocations are [%s] (want exactly "
                     "['fetch'] to survive)\n",
                     idsAfterDelete.join(QStringLiteral(", ")).toUtf8().constData());
        return 1;
    }

    // ---- 5 (deferred): set_invocations([]) clears -- run LAST, after the ----
    // live-run assertions: every invocation-authoring handler calls
    // ensureDesignMode(), which resets any run in progress.

    // ---- Drive a real run into Fetching, arming the 'fetch' invocation ------
    QJsonObject startParams;
    startParams[QStringLiteral("event")] = QStringLiteral("Start");
    const QJsonObject stepResp1 = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("step_simulation"), startParams));
    const QJsonObject stepResult1 = stepResp1.value(QStringLiteral("result")).toObject();
    if (!stepResp1.contains(QStringLiteral("result")) ||
        !jsonArrayContainsString(stepResult1.value(QStringLiteral("activeStateNames")).toArray(), QStringLiteral("Fetching"))) {
        std::fprintf(stderr, "FAIL: step_simulation('Start') did not move the machine from Idle into Fetching\n");
        return 1;
    }

    // ---- 6. Live reporting: get_simulation_status names the live invocation -
    const QJsonObject simStatusResult = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("get_simulation_status"), QJsonObject()))
                                             .value(QStringLiteral("result"))
                                             .toObject();
    bool foundFetchLive = false;
    for (const QJsonValue& liv : simStatusResult.value(QStringLiteral("liveInvocations")).toArray()) {
        const QJsonObject livObj = liv.toObject();
        if (livObj.value(QStringLiteral("effectiveId")).toString() != QStringLiteral("fetch")) {
            continue;
        }
        foundFetchLive = true;
        if (livObj.value(QStringLiteral("stateId")).toInteger(-1) != fetchingStateId ||
            livObj.value(QStringLiteral("stateName")).toString() != QStringLiteral("Fetching") ||
            livObj.value(QStringLiteral("outputType")).toString() != QStringLiteral("int")) {
            std::fprintf(stderr,
                         "FAIL: get_simulation_status's liveInvocations entry for 'fetch' has the wrong "
                         "stateId/stateName/outputType\n");
            return 1;
        }
    }
    if (!foundFetchLive) {
        std::fprintf(stderr,
                     "FAIL: get_simulation_status's liveInvocations does not name the live 'fetch' invocation after "
                     "entering Fetching\n");
        return 1;
    }

    // ---- 7. THE REGRESSION ASSERTION -----------------------------------------
    // A bare step_simulation("done.invoke.fetch") transitions the machine but
    // the trace reads "assign skipped: ..." because `output` is unbound.
    QJsonObject completeParams;
    completeParams[QStringLiteral("invokeId")] = QStringLiteral("fetch");
    completeParams[QStringLiteral("ok")] = true;
    completeParams[QStringLiteral("payload")] = 128;
    const QJsonObject completeResp = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("complete_invocation"), completeParams));
    if (!completeResp.contains(QStringLiteral("result"))) {
        std::fprintf(stderr,
                     "FAIL: complete_invocation({invokeId:'fetch', ok:true, payload:128}) returned an error instead "
                     "of a result: %s\n",
                     QJsonDocument(completeResp.value(QStringLiteral("error")).toObject()).toJson(QJsonDocument::Compact).constData());
        return 1;
    }
    const QJsonObject completeResult = completeResp.value(QStringLiteral("result")).toObject();
    const QJsonArray historyAfterComplete = completeResult.value(QStringLiteral("history")).toArray();
    if (!jsonArrayContainsString(completeResult.value(QStringLiteral("activeStateNames")).toArray(), QStringLiteral("Done"))) {
        std::fprintf(stderr, "FAIL: complete_invocation's success completion did not drive the machine into Done\n");
        return 1;
    }
    if (historyContainsFragment(historyAfterComplete, QStringLiteral("assign skipped"))) {
        std::fprintf(stderr,
                     "FAIL: *** THE REGRESSION *** complete_invocation's trace contains an 'assign skipped' line -- "
                     "this is the exact defect complete_invocation exists to fix (a plain event carries no payload, "
                     "so 'output' stayed unbound)\n");
        return 1;
    }
    if (!historyContainsFragment(historyAfterComplete, QStringLiteral("assign: sampleCount = 128"))) {
        std::fprintf(stderr,
                     "FAIL: *** THE REGRESSION *** complete_invocation's trace does not contain "
                     "'assign: sampleCount = 128' -- the completion payload never reached the 'output' identifier\n");
        return 1;
    }

    // ---- 8. Error completion: re-arm 'fetch', then fail it -------------------
    const QJsonObject resetResp1 = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("reset_simulation"), QJsonObject()));
    if (!resetResp1.contains(QStringLiteral("result"))) {
        std::fprintf(stderr, "FAIL: reset_simulation (re-arming for the error-completion test) returned an error\n");
        return 1;
    }
    const QJsonObject stepResp2 = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("step_simulation"), startParams));
    const QJsonObject stepResult2 = stepResp2.value(QStringLiteral("result")).toObject();
    if (!stepResp2.contains(QStringLiteral("result")) ||
        !jsonArrayContainsString(stepResult2.value(QStringLiteral("activeStateNames")).toArray(), QStringLiteral("Fetching"))) {
        std::fprintf(stderr,
                     "FAIL: restart + 'Start' did not re-enter Fetching to re-arm the 'fetch' invocation for the "
                     "error-completion test\n");
        return 1;
    }

    QJsonObject errorCompleteParams;
    errorCompleteParams[QStringLiteral("invokeId")] = QStringLiteral("fetch");
    errorCompleteParams[QStringLiteral("ok")] = false;
    errorCompleteParams[QStringLiteral("error")] = QStringLiteral("sensor timeout");
    const QJsonObject errorCompleteResp =
        server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("complete_invocation"), errorCompleteParams));
    if (!errorCompleteResp.contains(QStringLiteral("result"))) {
        std::fprintf(stderr, "FAIL: complete_invocation({ok:false, error:'sensor timeout'}) returned an error instead of a result\n");
        return 1;
    }
    const QJsonObject errorCompleteResult = errorCompleteResp.value(QStringLiteral("result")).toObject();
    if (!jsonArrayContainsString(errorCompleteResult.value(QStringLiteral("activeStateNames")).toArray(), QStringLiteral("Failed"))) {
        std::fprintf(stderr, "FAIL: error completion did not fire error.platform.fetch into Failed\n");
        return 1;
    }
    if (!historyContainsFragment(errorCompleteResult.value(QStringLiteral("history")).toArray(),
                                  QStringLiteral("assign: lastError = 'sensor timeout'"))) {
        std::fprintf(stderr,
                     "FAIL: error completion's trace does not contain \"assign: lastError = 'sensor timeout'\" -- "
                     "the error string never reached the 'error' identifier\n");
        return 1;
    }

    // ---- 9. Not-live and not-running are explicit errors, never silence -----
    // 'fetch' just completed above -- it is no longer live, a natural probe
    // for the not-live path (still running() at this point).
    QJsonObject notLiveParams;
    notLiveParams[QStringLiteral("invokeId")] = QStringLiteral("fetch");
    notLiveParams[QStringLiteral("ok")] = true;
    notLiveParams[QStringLiteral("payload")] = 1;
    const QJsonObject notLiveResp = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("complete_invocation"), notLiveParams));
    if (!notLiveResp.contains(QStringLiteral("error")) ||
        notLiveResp.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(0) != -32602) {
        std::fprintf(stderr, "FAIL: completing an already-finished (not live) invokeId 'fetch' did not return -32602\n");
        return 1;
    }

    QJsonObject unknownIdParams;
    unknownIdParams[QStringLiteral("invokeId")] = QStringLiteral("does-not-exist");
    const QJsonObject unknownIdResp = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("complete_invocation"), unknownIdParams));
    if (!unknownIdResp.contains(QStringLiteral("error")) ||
        unknownIdResp.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(0) != -32602) {
        std::fprintf(stderr, "FAIL: completing an unknown invokeId 'does-not-exist' did not return -32602\n");
        return 1;
    }

    const QJsonObject resetResp2 = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("reset_simulation"), QJsonObject()));
    if (!resetResp2.contains(QStringLiteral("result"))) {
        std::fprintf(stderr, "FAIL: reset_simulation (for the not-running probe) returned an error\n");
        return 1;
    }
    QJsonObject notRunningParams;
    notRunningParams[QStringLiteral("invokeId")] = QStringLiteral("fetch");
    notRunningParams[QStringLiteral("ok")] = true;
    notRunningParams[QStringLiteral("payload")] = 1;
    const QJsonObject notRunningResp = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("complete_invocation"), notRunningParams));
    if (!notRunningResp.contains(QStringLiteral("error")) ||
        notRunningResp.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(0) != -32002) {
        std::fprintf(stderr, "FAIL: completing an invocation while the simulation is not running did not return -32002\n");
        return 1;
    }

    // ---- 5. set_invocations([]) clears, and the legacy invokeSrc/invokeId ---
    // fields too: effectiveInvocations() folds a non-empty legacy invokeSrc
    // back in when the invocations vector is empty.
    QJsonObject clearParams;
    clearParams[QStringLiteral("name")] = QStringLiteral("Fetching");
    clearParams[QStringLiteral("invocations")] = QJsonArray();
    const QJsonObject clearResp = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("set_invocations"), clearParams));
    const QJsonObject clearResult = clearResp.value(QStringLiteral("result")).toObject();
    if (!clearResp.contains(QStringLiteral("result")) || clearResult.value(QStringLiteral("invocationCount")).toInt(-1) != 0) {
        std::fprintf(stderr, "FAIL: set_invocations with an empty array did not report invocationCount=0\n");
        return 1;
    }
    activeResult = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("get_active_machine"), QJsonObject()))
                       .value(QStringLiteral("result"))
                       .toObject();
    if (!findStateByName(activeResult, QStringLiteral("Fetching")).value(QStringLiteral("invocations")).toArray().isEmpty()) {
        std::fprintf(stderr,
                     "FAIL: after set_invocations([]), get_active_machine still reports a Fetching invocation -- "
                     "effectiveInvocations() resurrected a legacy invokeSrc/invokeId leftover\n");
        return 1;
    }

    // ---- 10. apply_auto_layout ------------------------------------------------
    // TopToBottom on the hand-placed machine above: the result carries the
    // four fields, states really moved (get_active_machine agrees with
    // movedStates), Fetching sits below Idle. An unknown direction is a
    // parameter error, not a silent LeftToRight.
    {
        const QJsonObject before = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("get_active_machine"), QJsonObject()))
                                       .value(QStringLiteral("result"))
                                       .toObject();
        QJsonObject layoutParams;
        layoutParams[QStringLiteral("direction")] = QStringLiteral("TopToBottom");
        const QJsonObject layoutResp = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("apply_auto_layout"), layoutParams));
        const QJsonObject layoutResult = layoutResp.value(QStringLiteral("result")).toObject();
        if (!layoutResp.contains(QStringLiteral("result")) || !layoutResult.contains(QStringLiteral("movedStates")) ||
            !layoutResult.contains(QStringLiteral("changedLabels")) || !layoutResult.contains(QStringLiteral("residuals")) ||
            layoutResult.value(QStringLiteral("direction")).toString() != QStringLiteral("TopToBottom")) {
            std::fprintf(stderr,
                         "FAIL: apply_auto_layout({direction:'TopToBottom'}) did not return "
                         "{movedStates, changedLabels, residuals, direction:'TopToBottom'}: %s\n",
                         QJsonDocument(layoutResp).toJson(QJsonDocument::Compact).constData());
            return 1;
        }
        const QJsonObject after = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("get_active_machine"), QJsonObject()))
                                      .value(QStringLiteral("result"))
                                      .toObject();
        int changed = 0;
        for (const QJsonValue& sv : after.value(QStringLiteral("states")).toArray()) {
            const QJsonObject rectAfter = sv.toObject().value(QStringLiteral("rect")).toObject();
            const QJsonObject rectBefore =
                findStateByName(before, sv.toObject().value(QStringLiteral("name")).toString()).value(QStringLiteral("rect")).toObject();
            if (rectAfter.value(QStringLiteral("x")).toDouble() != rectBefore.value(QStringLiteral("x")).toDouble() ||
                rectAfter.value(QStringLiteral("y")).toDouble() != rectBefore.value(QStringLiteral("y")).toDouble()) {
                ++changed;
            }
        }
        const int movedStates = layoutResult.value(QStringLiteral("movedStates")).toInt(-1);
        if (changed == 0 || changed != movedStates) {
            std::fprintf(stderr,
                         "FAIL: apply_auto_layout reported movedStates=%d but get_active_machine shows %d state(s) "
                         "moved (want the same, non-zero)\n",
                         movedStates, changed);
            return 1;
        }
        const double idleY =
            findStateByName(after, QStringLiteral("Idle")).value(QStringLiteral("rect")).toObject().value(QStringLiteral("y")).toDouble();
        const double fetchingY = findStateByName(after, QStringLiteral("Fetching"))
                                     .value(QStringLiteral("rect"))
                                     .toObject()
                                     .value(QStringLiteral("y"))
                                     .toDouble();
        if (!(fetchingY > idleY)) {
            std::fprintf(stderr, "FAIL: after apply_auto_layout TopToBottom, Fetching (y %g) is not below Idle (y %g)\n",
                         fetchingY, idleY);
            return 1;
        }

        QJsonObject badParams;
        badParams[QStringLiteral("direction")] = QStringLiteral("Diagonal");
        const QJsonObject badResp = server.handleRpcRequest(mcpRequest(nextId++, QStringLiteral("apply_auto_layout"), badParams));
        if (badResp.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(0) != -32602) {
            std::fprintf(stderr, "FAIL: apply_auto_layout with an unknown direction did not return -32602\n");
            return 1;
        }
        std::printf("PASS: MCP apply_auto_layout -- TopToBottom moved %d state(s) (Fetching below Idle), unknown "
                    "direction -32602\n",
                    movedStates);
    }

    std::printf("[SMOKE] McpRuntimeServer invoke completion smoke PASS\n");
    return 0;
}
