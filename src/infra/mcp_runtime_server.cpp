#include "infra/mcp_runtime_server.h"

#include <QBuffer>
#include <QGraphicsScene>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPainter>
#include <QPixmap>

#include "constants/design_tokens.h"
#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/sim_agent.h"
#include "model/sim_events.h"
#include "view/canvas/canvas_presenter.h"
#include "view/canvas/canvas_view.h"
#include "view/geometry/auto_layout.h"
#include "view/shell/document_session.h"
#include "view/shell/editor_view.h"
#include "view/shell/main_window.h"

namespace app {

Invocation invocationFromJson(const QJsonObject& obj) {
    Invocation inv;
    inv.src = obj.value(QStringLiteral("src")).toString().trimmed();
    inv.id = obj.value(QStringLiteral("id")).toString();

    const QString typeStr = obj.value(QStringLiteral("outputType")).toString(QStringLiteral("int")).toLower();
    if (typeStr == QStringLiteral("bool")) {
        inv.outputType = ContextType::Bool;
    } else if (typeStr == QStringLiteral("double") || typeStr == QStringLiteral("float")) {
        inv.outputType = ContextType::Double;
    } else if (typeStr == QStringLiteral("string")) {
        inv.outputType = ContextType::String;
    } else {
        inv.outputType = ContextType::Int;
    }
    return inv;
}

QJsonObject invocationToJson(const Invocation& inv) {
    QJsonObject obj;
    obj[QStringLiteral("src")] = inv.src;
    obj[QStringLiteral("id")] = inv.id;

    QString typeStr = QStringLiteral("int");
    if (inv.outputType == ContextType::Bool) {
        typeStr = QStringLiteral("bool");
    } else if (inv.outputType == ContextType::Double) {
        typeStr = QStringLiteral("double");
    } else if (inv.outputType == ContextType::String) {
        typeStr = QStringLiteral("string");
    }
    obj[QStringLiteral("outputType")] = typeStr;
    obj[QStringLiteral("effectiveId")] = effectiveInvocationId(inv);
    return obj;
}

McpRuntimeServer::McpRuntimeServer(MainWindow* window, QObject* parent)
    : QObject(parent), window_(window), server_(new QLocalServer(this)) {
    connect(server_, &QLocalServer::newConnection, this, &McpRuntimeServer::onNewConnection);
}

McpRuntimeServer::~McpRuntimeServer() {
    stop();
}

bool McpRuntimeServer::start(const QString& serverName) {
    serverName_ = serverName;
    QLocalServer::removeServer(serverName_);
    return server_->listen(serverName_);
}

void McpRuntimeServer::stop() {
    for (auto* client : clients_) {
        client->disconnect(this);
        client->close();
        client->deleteLater();
    }
    clients_.clear();
    clientBuffers_.clear();

    if (server_ && server_->isListening()) {
        server_->close();
    }
}

bool McpRuntimeServer::isListening() const {
    return server_ != nullptr && server_->isListening();
}

void McpRuntimeServer::onNewConnection() {
    while (server_->hasPendingConnections()) {
        QLocalSocket* socket = server_->nextPendingConnection();
        clients_.append(socket);
        clientBuffers_[socket] = QByteArray();

        connect(socket, &QLocalSocket::readyRead, this, &McpRuntimeServer::onClientReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, &McpRuntimeServer::onClientDisconnected);
    }
}

void McpRuntimeServer::onClientReadyRead() {
    auto* socket = qobject_cast<QLocalSocket*>(sender());
    if (!socket) {
        return;
    }

    clientBuffers_[socket].append(socket->readAll());
    QByteArray& buf = clientBuffers_[socket];

    int newlinePos = -1;
    while ((newlinePos = buf.indexOf('\n')) != -1) {
        QByteArray line = buf.left(newlinePos).trimmed();
        buf.remove(0, newlinePos + 1);

        if (!line.isEmpty()) {
            QByteArray responseBytes = processLine(line);
            if (!responseBytes.isEmpty()) {
                socket->write(responseBytes);
                socket->write("\n");
                socket->flush();
            }
        }
    }
}

void McpRuntimeServer::onClientDisconnected() {
    auto* socket = qobject_cast<QLocalSocket*>(sender());
    if (socket) {
        clients_.removeAll(socket);
        clientBuffers_.remove(socket);
        socket->deleteLater();
    }
}

QByteArray McpRuntimeServer::processLine(const QByteArray& line) {
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(line, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        QJsonObject err = makeError(QJsonValue(), -32700, QStringLiteral("Parse error: invalid JSON"));
        return QJsonDocument(err).toJson(QJsonDocument::Compact);
    }

    QJsonObject response = handleRpcRequest(doc.object());
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

QJsonObject McpRuntimeServer::handleRpcRequest(const QJsonObject& req) {
    QJsonValue id = req.value(QStringLiteral("id"));
    if (req.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0") ||
        !req.contains(QStringLiteral("method")) || !req.value(QStringLiteral("method")).isString()) {
        return makeError(id, -32600, QStringLiteral("Invalid Request: jsonrpc must be '2.0' and method must be a string"));
    }

    const QString method = req.value(QStringLiteral("method")).toString();
    const QJsonObject params = req.value(QStringLiteral("params")).toObject();

    if (method == QStringLiteral("get_active_machine")) {
        return makeResult(id, handleGetActiveMachine(params));
    }
    if (method == QStringLiteral("get_selection")) {
        return makeResult(id, handleGetSelection(params));
    }
    if (method == QStringLiteral("get_simulation_status")) {
        return makeResult(id, handleGetSimulationStatus(params));
    }
    if (method == QStringLiteral("capture_canvas")) {
        return makeResult(id, handleCaptureCanvas(params));
    }
    if (method == QStringLiteral("create_machine")) {
        return makeResult(id, handleCreateMachine(params));
    }
    if (method == QStringLiteral("batch_create_machine")) {
        return makeResult(id, handleBatchCreateMachine(params));
    }
    if (method == QStringLiteral("close_project")) {
        return makeResult(id, handleCloseProject(params));
    }
    if (method == QStringLiteral("add_state")) {
        return makeResult(id, handleAddState(params));
    }
    if (method == QStringLiteral("move_state")) {
        return makeResult(id, handleMoveState(params));
    }
    if (method == QStringLiteral("apply_auto_layout")) {
        return makeResult(id, handleApplyAutoLayout(params));
    }
    if (method == QStringLiteral("rename_state")) {
        return makeResult(id, handleRenameState(params));
    }
    if (method == QStringLiteral("set_state_kind")) {
        return makeResult(id, handleSetStateKind(params));
    }
    if (method == QStringLiteral("delete_state")) {
        return makeResult(id, handleDeleteState(params));
    }
    if (method == QStringLiteral("reparent_state")) {
        return makeResult(id, handleReparentState(params));
    }
    if (method == QStringLiteral("set_initial_state")) {
        return makeResult(id, handleSetInitialState(params));
    }
    if (method == QStringLiteral("add_transition")) {
        return makeResult(id, handleAddTransition(params));
    }
    if (method == QStringLiteral("move_transition_label")) {
        return makeResult(id, handleMoveTransitionLabel(params));
    }
    if (method == QStringLiteral("update_transition")) {
        return makeResult(id, handleUpdateTransition(params));
    }
    if (method == QStringLiteral("set_transition_flags")) {
        return makeResult(id, handleSetTransitionFlags(params));
    }
    if (method == QStringLiteral("delete_transition")) {
        return makeResult(id, handleDeleteTransition(params));
    }
    if (method == QStringLiteral("add_context_variable")) {
        return makeResult(id, handleAddContextVariable(params));
    }
    if (method == QStringLiteral("update_context_variable")) {
        return makeResult(id, handleUpdateContextVariable(params));
    }
    if (method == QStringLiteral("delete_context_variable")) {
        return makeResult(id, handleDeleteContextVariable(params));
    }
    if (method == QStringLiteral("set_invocations")) {
        return makeResult(id, handleSetInvocations(params));
    }
    if (method == QStringLiteral("add_invocation")) {
        return makeResult(id, handleAddInvocation(params));
    }
    if (method == QStringLiteral("delete_invocation")) {
        return makeResult(id, handleDeleteInvocation(params));
    }
    if (method == QStringLiteral("complete_invocation")) {
        return makeResult(id, handleCompleteInvocation(params));
    }
    if (method == QStringLiteral("step_simulation")) {
        return makeResult(id, handleStepSimulation(params));
    }
    if (method == QStringLiteral("reset_simulation")) {
        return makeResult(id, handleResetSimulation(params));
    }

    return makeError(id, -32601, QStringLiteral("Method not found: %1").arg(method));
}

DocumentSession* McpRuntimeServer::activeSession() const {
    if (!window_) {
        return nullptr;
    }
    return window_->ensureActiveSession();
}

EditorView* McpRuntimeServer::activeView() const {
    if (!window_) {
        return nullptr;
    }
    return window_->ensureActiveView();
}

QJsonObject McpRuntimeServer::handleCreateMachine(const QJsonObject& params) {
    if (!window_) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active window.");
        return err;
    }
    const QString name = params.value(QStringLiteral("name")).toString(QStringLiteral("Untitled"));
    DocumentSession* session = window_->createNewMachine(name);
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32000;
        err[QStringLiteral("message")] = QStringLiteral("Failed to create new machine.");
        return err;
    }
    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("machineName")] = session->machineName();
    res[QStringLiteral("relativePath")] = session->relativePath();
    return res;
}

QJsonObject McpRuntimeServer::handleCloseProject(const QJsonObject&) {
    if (!window_) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active window.");
        return err;
    }
    window_->closeProject(/*promptIfDirty=*/false);
    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("message")] = QStringLiteral("Project and all machines closed.");
    return res;
}

QJsonObject McpRuntimeServer::handleGetActiveMachine(const QJsonObject&) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("MachineDocAgent not found in active session.");
        return err;
    }

    const Machine& m = doc->machine();
    QJsonObject res;
    res[QStringLiteral("id")] = m.name.isEmpty() ? session->machineName() : m.name;
    res[QStringLiteral("name")] = m.name.isEmpty() ? session->machineName() : m.name;
    res[QStringLiteral("initialStateId")] = static_cast<qint64>(m.initialStateId);

    QJsonArray statesArr;
    for (const auto& s : m.states) {
        QJsonObject sObj;
        sObj[QStringLiteral("id")] = static_cast<qint64>(s.id);
        sObj[QStringLiteral("name")] = s.name;

        QString kindStr = QStringLiteral("normal");
        if (s.kind == StateKind::Parallel) {
            kindStr = QStringLiteral("parallel");
        } else if (s.kind == StateKind::Final) {
            kindStr = QStringLiteral("final");
        } else if (s.kind == StateKind::History) {
            kindStr = s.historyDeep ? QStringLiteral("historyDeep") : QStringLiteral("historyShallow");
        }
        sObj[QStringLiteral("kind")] = kindStr;
        sObj[QStringLiteral("parentId")] = s.parentId == 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(static_cast<qint64>(s.parentId));

        QJsonObject rectObj;
        rectObj[QStringLiteral("x")] = s.pos.x();
        rectObj[QStringLiteral("y")] = s.pos.y();
        rectObj[QStringLiteral("width")] = 140.0;
        rectObj[QStringLiteral("height")] = 80.0;
        sObj[QStringLiteral("rect")] = rectObj;

        QJsonArray entryArr;
        for (const auto& a : s.entryActions) {
            entryArr.append(a);
        }
        sObj[QStringLiteral("entryActions")] = entryArr;

        QJsonArray exitArr;
        for (const auto& a : s.exitActions) {
            exitArr.append(a);
        }
        sObj[QStringLiteral("exitActions")] = exitArr;

        QJsonArray invocationsArr;
        for (const auto& inv : s.effectiveInvocations()) {
            invocationsArr.append(invocationToJson(inv));
        }
        sObj[QStringLiteral("invocations")] = invocationsArr;

        statesArr.append(sObj);
    }
    res[QStringLiteral("states")] = statesArr;

    QJsonArray transArr;
    for (const auto& t : m.transitions) {
        QJsonObject tObj;
        tObj[QStringLiteral("id")] = static_cast<qint64>(t.id);
        tObj[QStringLiteral("sourceId")] = static_cast<qint64>(t.from);
        tObj[QStringLiteral("targetId")] = static_cast<qint64>(t.to);
        tObj[QStringLiteral("event")] = t.event;
        tObj[QStringLiteral("guard")] = t.guard;
        tObj[QStringLiteral("action")] = t.action;
        tObj[QStringLiteral("isAlways")] = t.always;
        tObj[QStringLiteral("reenter")] = t.reenter;
        transArr.append(tObj);
    }
    res[QStringLiteral("transitions")] = transArr;

    QJsonArray ctxArr;
    for (const auto& cv : m.context) {
        QJsonObject cvObj;
        cvObj[QStringLiteral("id")] = static_cast<qint64>(cv.id);
        cvObj[QStringLiteral("name")] = cv.name;
        cvObj[QStringLiteral("key")] = cv.name;
        cvObj[QStringLiteral("defaultValue")] = cv.initialValue;
        cvObj[QStringLiteral("initialValue")] = cv.initialValue;

        QString typeStr = QStringLiteral("int");
        if (cv.type == ContextType::Bool) {
            typeStr = QStringLiteral("bool");
        } else if (cv.type == ContextType::Double) {
            typeStr = QStringLiteral("double");
        } else if (cv.type == ContextType::String) {
            typeStr = QStringLiteral("string");
        } else if (cv.type == ContextType::Object) {
            typeStr = QStringLiteral("object");
        }
        cvObj[QStringLiteral("type")] = typeStr;
        ctxArr.append(cvObj);
    }
    QJsonObject ctxObj;
    ctxObj[QStringLiteral("schema")] = ctxArr;
    res[QStringLiteral("context")] = ctxObj;

    return res;
}

QJsonObject McpRuntimeServer::handleGetSelection(const QJsonObject&) {
    EditorView* view = activeView();
    if (!view || !view->presenter()) {
        QJsonObject res;
        res[QStringLiteral("kind")] = QStringLiteral("None");
        res[QStringLiteral("selectedIds")] = QJsonArray();
        return res;
    }

    CanvasPresenter* p = view->presenter();
    const auto sel = p->currentSelection();
    QJsonObject res;

    QString kindStr = QStringLiteral("None");
    QJsonArray idsArr;
    if (sel.kind == SelectionKind::State) {
        kindStr = QStringLiteral("State");
        idsArr.append(static_cast<qint64>(sel.id));
    } else if (sel.kind == SelectionKind::Transition) {
        kindStr = QStringLiteral("Transition");
        idsArr.append(static_cast<qint64>(sel.id));
    } else if (sel.kind == SelectionKind::Note) {
        kindStr = QStringLiteral("Note");
        idsArr.append(static_cast<qint64>(sel.id));
    } else if (sel.kind == SelectionKind::Multi) {
        kindStr = QStringLiteral("Multi");
    }

    res[QStringLiteral("kind")] = kindStr;
    res[QStringLiteral("selectedIds")] = idsArr;
    return res;
}

QJsonObject McpRuntimeServer::handleGetSimulationStatus(const QJsonObject&) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    auto sim = session->kernel().agentAs<SimulationAgent>(SimulationAgent::kName);
    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    QJsonObject res;
    res[QStringLiteral("mode")] = (sim && sim->mode() == events::Mode::Simulate) ? QStringLiteral("Simulation") : QStringLiteral("Design");
    res[QStringLiteral("isRunning")] = sim ? sim->running() : false;

    QJsonArray activeIds;
    QJsonArray activeNames;
    if (sim) {
        for (quint64 id : sim->configuration()) {
            activeIds.append(static_cast<qint64>(id));
            if (doc) {
                for (const auto& s : doc->machine().states) {
                    if (s.id == id) {
                        activeNames.append(s.name);
                        break;
                    }
                }
            }
        }
    }
    res[QStringLiteral("activeStateIds")] = activeIds;
    res[QStringLiteral("activeStateNames")] = activeNames;

    QJsonArray traceArr;
    if (sim) {
        for (const auto& t : sim->trace()) {
            traceArr.append(t);
        }
    }
    res[QStringLiteral("history")] = traceArr;

    QJsonArray liveInvocationsArr;
    if (sim && doc) {
        const auto liveIds = sim->liveInvocationIds();
        for (const auto& s : doc->machine().states) {
            for (const auto& inv : s.effectiveInvocations()) {
                if (inv.src.isEmpty()) continue;
                const QString effId = effectiveInvocationId(inv);
                if (!liveIds.contains(effId)) continue;
                QJsonObject invObj = invocationToJson(inv);
                invObj[QStringLiteral("stateId")] = static_cast<qint64>(s.id);
                invObj[QStringLiteral("stateName")] = s.name;
                liveInvocationsArr.append(invObj);
            }
        }
    }
    res[QStringLiteral("liveInvocations")] = liveInvocationsArr;

    return res;
}

QJsonObject McpRuntimeServer::handleCaptureCanvas(const QJsonObject& params) {
    EditorView* view = activeView();
    if (!view || !view->canvasView()) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active canvas view currently focused.");
        return err;
    }

    CanvasView* cv = view->canvasView();
    const bool fullScene = params.value(QStringLiteral("fullScene")).toBool(false);

    QImage img;
    if (fullScene && cv->scene()) {
        QRectF rect = cv->scene()->itemsBoundingRect().adjusted(-20, -20, 20, 20);
        if (rect.width() < 100) {
            rect.setWidth(400);
        }
        if (rect.height() < 100) {
            rect.setHeight(300);
        }
        img = QImage(rect.size().toSize(), QImage::Format_ARGB32_Premultiplied);
        // Canvas background, so the capture matches the screen.
        img.fill(design::color(design::kCanvasBackground));
        QPainter painter(&img);
        painter.setRenderHint(QPainter::Antialiasing);
        cv->scene()->render(&painter, QRectF(0, 0, rect.width(), rect.height()), rect);
    } else {
        const QPixmap pix = cv->viewport()->grab();
        img = pix.toImage();
    }

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");

    QJsonObject res;
    res[QStringLiteral("width")] = img.width();
    res[QStringLiteral("height")] = img.height();
    res[QStringLiteral("format")] = QStringLiteral("png");
    res[QStringLiteral("imageBase64")] = QString::fromLatin1(bytes.toBase64());
    res[QStringLiteral("activeMachine")] = view->session() ? view->session()->machineName() : QString();
    return res;
}

QJsonObject McpRuntimeServer::handleAddState(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    const QString name = params.value(QStringLiteral("name")).toString();
    if (name.trimmed().isEmpty()) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'name' parameter for add_state.");
        return err;
    }

    const quint64 parentId = static_cast<quint64>(params.value(QStringLiteral("parentId")).toInteger(0));
    const double x = params.value(QStringLiteral("x")).toDouble(120.0);
    const double y = params.value(QStringLiteral("y")).toDouble(120.0);
    const QString kindStr = params.value(QStringLiteral("kind")).toString(QStringLiteral("normal")).toLower();

    StateKind kind = StateKind::Normal;
    if (kindStr == QStringLiteral("parallel")) {
        kind = StateKind::Parallel;
    } else if (kindStr == QStringLiteral("final")) {
        kind = StateKind::Final;
    } else if (kindStr == QStringLiteral("history") || kindStr == QStringLiteral("historyshallow") || kindStr == QStringLiteral("historydeep")) {
        kind = StateKind::History;
    }

    session->kernel().send(events::AddStateRequested{
        .pos = QPointF(x, y),
        .parentId = parentId
    });

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->machine().states.empty()) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32002;
        err[QStringLiteral("message")] = QStringLiteral("Failed to add state to machine document.");
        return err;
    }

    const quint64 newId = doc->machine().states.back().id;
    session->kernel().send(events::RenameStateRequested{.id = newId, .name = name});
    if (kind != StateKind::Normal) {
        session->kernel().send(events::SetStateKindRequested{.id = newId, .kind = kind});
    }
    if (doc->machine().initialStateId == 0 && parentId == 0) {
        session->kernel().send(events::SetInitialStateRequested{.id = newId});
    }

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("stateId")] = static_cast<qint64>(newId);
    res[QStringLiteral("name")] = name;
    return res;
}

QJsonObject McpRuntimeServer::handleMoveState(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("MachineDocAgent not found in active session.");
        return err;
    }

    quint64 stateId = static_cast<quint64>(params.value(QStringLiteral("stateId")).toInteger(0));
    const QString name = params.value(QStringLiteral("name")).toString();

    if (stateId == 0 && !name.isEmpty()) {
        for (const auto& s : doc->machine().states) {
            if (s.name == name) {
                stateId = s.id;
                break;
            }
        }
    }

    if (stateId == 0 || !doc->findState(stateId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("State not found.");
        return err;
    }

    if (!params.contains(QStringLiteral("x")) || !params.contains(QStringLiteral("y"))) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'x' or 'y' coordinate parameters.");
        return err;
    }

    const double x = params.value(QStringLiteral("x")).toDouble();
    const double y = params.value(QStringLiteral("y")).toDouble();

    session->kernel().send(events::MoveStateRequested{
        .id = stateId,
        .pos = QPointF(x, y)
    });

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("stateId")] = static_cast<qint64>(stateId);
    res[QStringLiteral("x")] = x;
    res[QStringLiteral("y")] = y;
    return res;
}

// Same path as the Edit menu's Auto Layout: DocumentSession::runAutoLayout sends
// one ApplyLayoutPlanRequested, so the layout is one undo step. Direction
// strings are the LayoutDirection enumerator names.
QJsonObject McpRuntimeServer::handleApplyAutoLayout(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    const QString direction = params.value(QStringLiteral("direction")).toString(QStringLiteral("LeftToRight"));
    AutoLayoutOptions options;
    if (const std::optional<LayoutDirection> parsed = layoutDirectionFromName(direction)) {
        options.direction = *parsed;
    } else {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] =
            QStringLiteral("Unknown direction '%1' (LeftToRight, RightToLeft, TopToBottom or BottomToTop).").arg(direction);
        return err;
    }
    options.layerGap = params.value(QStringLiteral("layerGap")).toDouble(kDefaultLayerGap);
    options.nodeGap = params.value(QStringLiteral("nodeGap")).toDouble(kDefaultNodeGap);

    ensureDesignMode(session);

    const DocumentSession::AutoLayoutRun run = session->runAutoLayout(options);
    if (!run.ran) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("Auto layout could not run (no document or no GUI application).");
        return err;
    }

    QJsonObject res;
    res[QStringLiteral("movedStates")] = run.movedStates;
    res[QStringLiteral("changedLabels")] = run.changedLabels;
    res[QStringLiteral("residuals")] = run.residuals;
    res[QStringLiteral("direction")] = direction;
    return res;
}

QJsonObject McpRuntimeServer::handleAddTransition(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    if (!params.contains(QStringLiteral("sourceId")) || !params.contains(QStringLiteral("targetId"))) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'sourceId' or 'targetId' parameter.");
        return err;
    }

    const quint64 fromId = static_cast<quint64>(params.value(QStringLiteral("sourceId")).toInteger(0));
    const quint64 toId = static_cast<quint64>(params.value(QStringLiteral("targetId")).toInteger(0));
    const QString event = params.value(QStringLiteral("event")).toString();
    const QString guard = params.value(QStringLiteral("guard")).toString();
    const QString action = params.value(QStringLiteral("action")).toString();

    session->kernel().send(events::AddTransitionRequested{
        .from = fromId,
        .to = toId
    });

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->machine().transitions.empty()) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32002;
        err[QStringLiteral("message")] = QStringLiteral("Failed to add transition to machine document.");
        return err;
    }

    const quint64 newId = doc->machine().transitions.back().id;
    if (!event.isEmpty()) {
        session->kernel().send(events::SetTransitionEventRequested{.id = newId, .event = event});
    }
    if (!guard.isEmpty()) {
        session->kernel().send(events::SetTransitionGuardRequested{.id = newId, .guard = guard});
    }
    if (!action.isEmpty()) {
        session->kernel().send(events::SetTransitionActionRequested{.id = newId, .action = action});
    }

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("transitionId")] = static_cast<qint64>(newId);
    res[QStringLiteral("event")] = event;
    return res;
}

QJsonObject McpRuntimeServer::handleMoveTransitionLabel(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("MachineDocAgent not found in active session.");
        return err;
    }

    const quint64 tId = static_cast<quint64>(params.value(QStringLiteral("transitionId")).toInteger(0));
    if (tId == 0 || !doc->findTransition(tId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Transition not found.");
        return err;
    }

    const double offsetX = params.value(QStringLiteral("offsetX")).toDouble(0.0);
    const double offsetY = params.value(QStringLiteral("offsetY")).toDouble(0.0);
    const bool resetBendpoints = params.value(QStringLiteral("resetBendpoints")).toBool(false);

    session->kernel().send(events::MoveTransitionLabelRequested{
        .id = tId,
        .offset = QPointF(offsetX, offsetY),
        .resetBendpoints = resetBendpoints
    });

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("transitionId")] = static_cast<qint64>(tId);
    res[QStringLiteral("offsetX")] = offsetX;
    res[QStringLiteral("offsetY")] = offsetY;
    return res;
}

QJsonObject McpRuntimeServer::handleRenameState(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    const quint64 stateId = static_cast<quint64>(params.value(QStringLiteral("stateId")).toInteger(0));
    const QString name = params.value(QStringLiteral("name")).toString();
    if (stateId == 0 || name.trimmed().isEmpty()) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing or invalid 'stateId' or 'name' parameter for rename_state.");
        return err;
    }

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || !doc->findState(stateId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("State not found: %1").arg(stateId);
        return err;
    }

    session->kernel().send(events::RenameStateRequested{.id = stateId, .name = name});

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("stateId")] = static_cast<qint64>(stateId);
    res[QStringLiteral("name")] = name;
    return res;
}

QJsonObject McpRuntimeServer::handleSetStateKind(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    const quint64 stateId = static_cast<quint64>(params.value(QStringLiteral("stateId")).toInteger(0));
    const QString kindStr = params.value(QStringLiteral("kind")).toString().toLower();
    if (stateId == 0 || kindStr.isEmpty()) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'stateId' or 'kind' parameter for set_state_kind.");
        return err;
    }

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || !doc->findState(stateId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("State not found: %1").arg(stateId);
        return err;
    }

    StateKind kind = StateKind::Normal;
    bool deep = false;
    if (kindStr == QStringLiteral("parallel")) {
        kind = StateKind::Parallel;
    } else if (kindStr == QStringLiteral("final")) {
        kind = StateKind::Final;
    } else if (kindStr == QStringLiteral("history") || kindStr == QStringLiteral("history_shallow") || kindStr == QStringLiteral("historyshallow")) {
        kind = StateKind::History;
        deep = false;
    } else if (kindStr == QStringLiteral("history_deep") || kindStr == QStringLiteral("historydeep")) {
        kind = StateKind::History;
        deep = true;
    }

    session->kernel().send(events::SetStateKindRequested{.id = stateId, .kind = kind});
    if (kind == StateKind::History) {
        session->kernel().send(events::SetHistoryDeepRequested{.stateId = stateId, .deep = deep});
    }

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("stateId")] = static_cast<qint64>(stateId);
    res[QStringLiteral("kind")] = kindStr;
    return res;
}

QJsonObject McpRuntimeServer::handleDeleteState(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    const quint64 stateId = static_cast<quint64>(params.value(QStringLiteral("stateId")).toInteger(0));
    if (stateId == 0) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'stateId' parameter for delete_state.");
        return err;
    }

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || !doc->findState(stateId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("State not found: %1").arg(stateId);
        return err;
    }

    session->kernel().send(events::DeleteStateRequested{.id = stateId});

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("deletedStateId")] = static_cast<qint64>(stateId);
    return res;
}

QJsonObject McpRuntimeServer::handleReparentState(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    const quint64 stateId = static_cast<quint64>(params.value(QStringLiteral("stateId")).toInteger(0));
    const quint64 parentId = static_cast<quint64>(params.value(QStringLiteral("parentId")).toInteger(0));
    if (stateId == 0) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'stateId' parameter for reparent_state.");
        return err;
    }

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || !doc->findState(stateId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("State not found: %1").arg(stateId);
        return err;
    }
    if (parentId != 0 && !doc->findState(parentId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Parent state not found: %1").arg(parentId);
        return err;
    }

    session->kernel().send(events::ReparentStateRequested{.id = stateId, .parentId = parentId});

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("stateId")] = static_cast<qint64>(stateId);
    res[QStringLiteral("parentId")] = static_cast<qint64>(parentId);
    return res;
}

QJsonObject McpRuntimeServer::handleSetInitialState(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    const quint64 stateId = static_cast<quint64>(params.value(QStringLiteral("stateId")).toInteger(0));
    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (stateId != 0 && (!doc || !doc->findState(stateId))) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("State not found: %1").arg(stateId);
        return err;
    }

    session->kernel().send(events::SetInitialStateRequested{.id = stateId});

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("initialStateId")] = static_cast<qint64>(stateId);
    return res;
}

QJsonObject McpRuntimeServer::handleUpdateTransition(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    const quint64 transitionId = static_cast<quint64>(params.value(QStringLiteral("transitionId")).toInteger(0));
    if (transitionId == 0) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'transitionId' parameter for update_transition.");
        return err;
    }

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || !doc->findTransition(transitionId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Transition not found: %1").arg(transitionId);
        return err;
    }

    if (params.contains(QStringLiteral("event"))) {
        session->kernel().send(events::SetTransitionEventRequested{
            .id = transitionId,
            .event = params.value(QStringLiteral("event")).toString()
        });
    }
    if (params.contains(QStringLiteral("guard"))) {
        session->kernel().send(events::SetTransitionGuardRequested{
            .id = transitionId,
            .guard = params.value(QStringLiteral("guard")).toString()
        });
    }
    if (params.contains(QStringLiteral("action"))) {
        session->kernel().send(events::SetTransitionActionRequested{
            .id = transitionId,
            .action = params.value(QStringLiteral("action")).toString()
        });
    }
    if (params.contains(QStringLiteral("delayMs"))) {
        session->kernel().send(events::SetTransitionDelayRequested{
            .id = transitionId,
            .delayMs = params.value(QStringLiteral("delayMs")).toInt(0)
        });
    }

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("transitionId")] = static_cast<qint64>(transitionId);
    return res;
}

QJsonObject McpRuntimeServer::handleSetTransitionFlags(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    const quint64 transitionId = static_cast<quint64>(params.value(QStringLiteral("transitionId")).toInteger(0));
    if (transitionId == 0) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'transitionId' parameter for set_transition_flags.");
        return err;
    }

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || !doc->findTransition(transitionId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Transition not found: %1").arg(transitionId);
        return err;
    }

    if (params.contains(QStringLiteral("always"))) {
        session->kernel().send(events::SetTransitionAlwaysRequested{
            .id = transitionId,
            .always = params.value(QStringLiteral("always")).toBool()
        });
    }
    if (params.contains(QStringLiteral("reenter"))) {
        session->kernel().send(events::SetTransitionReenterRequested{
            .id = transitionId,
            .reenter = params.value(QStringLiteral("reenter")).toBool()
        });
    }

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("transitionId")] = static_cast<qint64>(transitionId);
    return res;
}

QJsonObject McpRuntimeServer::handleDeleteTransition(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    const quint64 transitionId = static_cast<quint64>(params.value(QStringLiteral("transitionId")).toInteger(0));
    if (transitionId == 0) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'transitionId' parameter for delete_transition.");
        return err;
    }

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || !doc->findTransition(transitionId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Transition not found: %1").arg(transitionId);
        return err;
    }

    session->kernel().send(events::DeleteTransitionRequested{.id = transitionId});

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("deletedTransitionId")] = static_cast<qint64>(transitionId);
    return res;
}

QJsonObject McpRuntimeServer::handleAddContextVariable(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    const QString name = params.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'name' parameter for add_context_variable.");
        return err;
    }

    const QString typeStr = params.value(QStringLiteral("type")).toString(QStringLiteral("int")).toLower();
    ContextType type = ContextType::Int;
    if (typeStr == QStringLiteral("bool")) {
        type = ContextType::Bool;
    } else if (typeStr == QStringLiteral("double")) {
        type = ContextType::Double;
    } else if (typeStr == QStringLiteral("string")) {
        type = ContextType::String;
    } else if (typeStr == QStringLiteral("object")) {
        type = ContextType::Object;
    }

    const QString initialValue = params.contains(QStringLiteral("initialValue"))
        ? params.value(QStringLiteral("initialValue")).toString()
        : (type == ContextType::Bool ? QStringLiteral("false")
           : (type == ContextType::Double ? QStringLiteral("0.0")
              : (type == ContextType::String ? QStringLiteral("\"\"")
                 : (type == ContextType::Object ? QStringLiteral("{}") : QStringLiteral("0")))));

    const QString customTypeName = params.value(QStringLiteral("customTypeName")).toString();

    session->kernel().send(events::AddContextVariableRequested{});

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc || doc->machine().context.empty()) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32002;
        err[QStringLiteral("message")] = QStringLiteral("Failed to add context variable to machine document.");
        return err;
    }

    const quint64 varId = doc->machine().context.back().id;
    session->kernel().send(events::RenameContextVariableRequested{.id = varId, .name = name});
    if (type != ContextType::Int) {
        session->kernel().send(events::SetContextTypeRequested{.id = varId, .type = type});
    }
    if (!initialValue.isEmpty() && initialValue != QStringLiteral("0")) {
        session->kernel().send(events::SetContextInitialValueRequested{.id = varId, .initialValue = initialValue});
    }
    if (!customTypeName.isEmpty()) {
        session->kernel().send(events::SetContextCustomTypeNameRequested{.id = varId, .customTypeName = customTypeName});
    }

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("variableId")] = static_cast<qint64>(varId);
    res[QStringLiteral("name")] = name;
    res[QStringLiteral("type")] = typeStr;
    res[QStringLiteral("initialValue")] = initialValue;
    return res;
}

QJsonObject McpRuntimeServer::handleUpdateContextVariable(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("MachineDocAgent not found in active session.");
        return err;
    }

    quint64 varId = static_cast<quint64>(params.value(QStringLiteral("variableId")).toInteger(0));
    const QString name = params.value(QStringLiteral("name")).toString();

    const ContextVariable* targetVar = nullptr;
    if (varId != 0) {
        targetVar = doc->findContextVariable(varId);
    } else if (!name.isEmpty()) {
        for (const auto& cv : doc->machine().context) {
            if (cv.name == name) {
                targetVar = &cv;
                varId = cv.id;
                break;
            }
        }
    }

    if (!targetVar) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Context variable not found.");
        return err;
    }

    if (params.contains(QStringLiteral("newName"))) {
        const QString newName = params.value(QStringLiteral("newName")).toString().trimmed();
        if (!newName.isEmpty()) {
            session->kernel().send(events::RenameContextVariableRequested{.id = varId, .name = newName});
        }
    }
    if (params.contains(QStringLiteral("type"))) {
        const QString typeStr = params.value(QStringLiteral("type")).toString().toLower();
        ContextType type = ContextType::Int;
        if (typeStr == QStringLiteral("bool")) {
            type = ContextType::Bool;
        } else if (typeStr == QStringLiteral("double")) {
            type = ContextType::Double;
        } else if (typeStr == QStringLiteral("string")) {
            type = ContextType::String;
        } else if (typeStr == QStringLiteral("object")) {
            type = ContextType::Object;
        }
        session->kernel().send(events::SetContextTypeRequested{.id = varId, .type = type});
    }
    if (params.contains(QStringLiteral("initialValue"))) {
        session->kernel().send(events::SetContextInitialValueRequested{
            .id = varId,
            .initialValue = params.value(QStringLiteral("initialValue")).toString()
        });
    }
    if (params.contains(QStringLiteral("customTypeName"))) {
        session->kernel().send(events::SetContextCustomTypeNameRequested{
            .id = varId,
            .customTypeName = params.value(QStringLiteral("customTypeName")).toString()
        });
    }

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("variableId")] = static_cast<qint64>(varId);
    return res;
}

QJsonObject McpRuntimeServer::handleDeleteContextVariable(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("MachineDocAgent not found in active session.");
        return err;
    }

    quint64 varId = static_cast<quint64>(params.value(QStringLiteral("variableId")).toInteger(0));
    const QString name = params.value(QStringLiteral("name")).toString();

    if (varId == 0 && !name.isEmpty()) {
        for (const auto& cv : doc->machine().context) {
            if (cv.name == name) {
                varId = cv.id;
                break;
            }
        }
    }

    if (varId == 0 || !doc->findContextVariable(varId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Context variable not found.");
        return err;
    }

    session->kernel().send(events::DeleteContextVariableRequested{.id = varId});

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("deletedVariableId")] = static_cast<qint64>(varId);
    return res;
}

QJsonObject McpRuntimeServer::handleSetInvocations(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("MachineDocAgent not found in active session.");
        return err;
    }

    quint64 stateId = static_cast<quint64>(params.value(QStringLiteral("stateId")).toInteger(0));
    const QString name = params.value(QStringLiteral("name")).toString();
    if (stateId == 0 && !name.isEmpty()) {
        for (const auto& s : doc->machine().states) {
            if (s.name == name) {
                stateId = s.id;
                break;
            }
        }
    }

    if (stateId == 0 || !doc->findState(stateId)) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("State not found for set_invocations.");
        return err;
    }

    QVector<Invocation> invocations;
    if (params.value(QStringLiteral("invocations")).isArray()) {
        for (const QJsonValue& v : params.value(QStringLiteral("invocations")).toArray()) {
            if (!v.isObject()) continue;
            Invocation inv = invocationFromJson(v.toObject());
            if (inv.src.isEmpty()) continue;
            invocations.append(inv);
        }
    }

    session->kernel().send(events::SetInvocationsRequested{.stateId = stateId, .invocations = invocations});

    QJsonArray invArr;
    if (const State* updated = doc->findState(stateId)) {
        for (const auto& inv : updated->effectiveInvocations()) {
            invArr.append(invocationToJson(inv));
        }
    }

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("stateId")] = static_cast<qint64>(stateId);
    res[QStringLiteral("invocationCount")] = invArr.size();
    res[QStringLiteral("invocations")] = invArr;
    return res;
}

QJsonObject McpRuntimeServer::handleAddInvocation(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("MachineDocAgent not found in active session.");
        return err;
    }

    quint64 stateId = static_cast<quint64>(params.value(QStringLiteral("stateId")).toInteger(0));
    const QString name = params.value(QStringLiteral("name")).toString();
    if (stateId == 0 && !name.isEmpty()) {
        for (const auto& s : doc->machine().states) {
            if (s.name == name) {
                stateId = s.id;
                break;
            }
        }
    }

    const State* state = stateId != 0 ? doc->findState(stateId) : nullptr;
    if (!state) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("State not found for add_invocation.");
        return err;
    }

    Invocation newInv = invocationFromJson(params);
    if (newInv.src.isEmpty()) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'src' parameter for add_invocation.");
        return err;
    }

    QVector<Invocation> invocations = state->effectiveInvocations();
    invocations.append(newInv);

    session->kernel().send(events::SetInvocationsRequested{.stateId = stateId, .invocations = invocations});

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("stateId")] = static_cast<qint64>(stateId);
    res[QStringLiteral("effectiveId")] = effectiveInvocationId(newInv);
    res[QStringLiteral("invocationCount")] = invocations.size();
    return res;
}

QJsonObject McpRuntimeServer::handleDeleteInvocation(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);

    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("MachineDocAgent not found in active session.");
        return err;
    }

    quint64 stateId = static_cast<quint64>(params.value(QStringLiteral("stateId")).toInteger(0));
    const QString name = params.value(QStringLiteral("name")).toString();
    if (stateId == 0 && !name.isEmpty()) {
        for (const auto& s : doc->machine().states) {
            if (s.name == name) {
                stateId = s.id;
                break;
            }
        }
    }

    const State* state = stateId != 0 ? doc->findState(stateId) : nullptr;
    if (!state) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("State not found for delete_invocation.");
        return err;
    }

    QVector<Invocation> invocations = state->effectiveInvocations();

    int removeIndex = -1;
    if (params.contains(QStringLiteral("invokeId"))) {
        const QString invokeId = params.value(QStringLiteral("invokeId")).toString();
        for (int i = 0; i < invocations.size(); ++i) {
            if (effectiveInvocationId(invocations[i]) == invokeId) {
                removeIndex = i;
                break;
            }
        }
        if (removeIndex < 0) {
            QJsonObject err;
            err[QStringLiteral("__error__")] = true;
            err[QStringLiteral("code")] = -32602;
            err[QStringLiteral("message")] = QStringLiteral("No invocation found with invokeId: %1").arg(invokeId);
            return err;
        }
    } else if (params.contains(QStringLiteral("index"))) {
        const int idx = params.value(QStringLiteral("index")).toInt(-1);
        if (idx < 0 || idx >= invocations.size()) {
            QJsonObject err;
            err[QStringLiteral("__error__")] = true;
            err[QStringLiteral("code")] = -32602;
            err[QStringLiteral("message")] = QStringLiteral("Invocation index out of range: %1").arg(idx);
            return err;
        }
        removeIndex = idx;
    } else {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'invokeId' or 'index' parameter for delete_invocation.");
        return err;
    }

    const QString removedEffectiveId = effectiveInvocationId(invocations[removeIndex]);
    invocations.removeAt(removeIndex);

    session->kernel().send(events::SetInvocationsRequested{.stateId = stateId, .invocations = invocations});

    QJsonObject res;
    res[QStringLiteral("success")] = true;
    res[QStringLiteral("stateId")] = static_cast<qint64>(stateId);
    res[QStringLiteral("removedEffectiveId")] = removedEffectiveId;
    res[QStringLiteral("invocationCount")] = invocations.size();
    return res;
}

QJsonObject McpRuntimeServer::handleCompleteInvocation(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    // No ensureDesignMode(session): switching to Design would tear down the
    // simulation run being completed.

    const QString invokeId = params.value(QStringLiteral("invokeId")).toString();
    if (invokeId.isEmpty()) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Missing required 'invokeId' parameter for complete_invocation.");
        return err;
    }

    auto sim = session->kernel().agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("SimulationAgent not found.");
        return err;
    }
    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("MachineDocAgent not found in active session.");
        return err;
    }

    // No auto-start: starting resets the run to its initial configuration,
    // where the invocation cannot be live yet.
    if (sim->mode() != events::Mode::Simulate || !sim->running()) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32002;
        err[QStringLiteral("message")] = QStringLiteral(
            "Simulation is not running. Call step_simulation first to start the run.");
        return err;
    }

    const auto liveIds = sim->liveInvocationIds();
    if (!liveIds.contains(invokeId)) {
        QStringList liveList(liveIds.begin(), liveIds.end());
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32602;
        err[QStringLiteral("message")] = QStringLiteral("Invocation '%1' is not live. Live invocations: %2")
            .arg(invokeId, liveList.isEmpty() ? QStringLiteral("(none)") : liveList.join(QStringLiteral(", ")));
        return err;
    }

    const bool completionOk = params.value(QStringLiteral("ok")).toBool(true);

    QVariant payloadVariant;
    if (completionOk) {
        ContextType outputType = ContextType::Int;
        for (const auto& s : doc->machine().states) {
            bool found = false;
            for (const auto& inv : s.effectiveInvocations()) {
                if (effectiveInvocationId(inv) == invokeId) {
                    outputType = inv.outputType;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }

        const QJsonValue payloadVal = params.value(QStringLiteral("payload"));
        switch (outputType) {
            case ContextType::Bool:
                payloadVariant = payloadVal.toBool();
                break;
            case ContextType::Double:
                payloadVariant = payloadVal.toDouble();
                break;
            case ContextType::String:
                payloadVariant = payloadVal.toString();
                break;
            default:
                payloadVariant = payloadVal.toInt();
                break;
        }
    } else {
        // An error completion's payload is always a string, whatever the
        // invocation's outputType.
        QString errMsg = params.value(QStringLiteral("error")).toString();
        if (errMsg.isEmpty()) {
            errMsg = params.value(QStringLiteral("errorMessage")).toString();
        }
        payloadVariant = errMsg;
    }

    session->kernel().send(events::CompleteInvocationRequested{
        .invokeId = invokeId,
        .ok = completionOk,
        .payload = payloadVariant
    });

    return handleGetSimulationStatus(QJsonObject());
}

QJsonObject McpRuntimeServer::handleStepSimulation(const QJsonObject& params) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    auto sim = session->kernel().agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("SimulationAgent not found.");
        return err;
    }

    if (sim->mode() != events::Mode::Simulate) {
        session->kernel().send(events::SetModeRequested{.mode = events::Mode::Simulate});
    }
    if (!sim->running()) {
        session->kernel().send(events::RunRequested{});
    }

    const QString eventName = params.value(QStringLiteral("event")).toString();
    if (!eventName.isEmpty()) {
        session->kernel().send(events::SendEventRequested{.name = eventName});
    } else {
        session->kernel().send(events::TickElapsed{.elapsedMs = 50});
    }

    return handleGetSimulationStatus(QJsonObject());
}

QJsonObject McpRuntimeServer::handleResetSimulation(const QJsonObject&) {
    DocumentSession* session = activeSession();
    if (!session) {
        QJsonObject err;
        err[QStringLiteral("__error__")] = true;
        err[QStringLiteral("code")] = -32001;
        err[QStringLiteral("message")] = QStringLiteral("No active document session currently focused.");
        return err;
    }

    ensureDesignMode(session);
    return handleGetSimulationStatus(QJsonObject());
}

void McpRuntimeServer::ensureDesignMode(DocumentSession* session) {
    if (!session) {
        return;
    }
    auto sim = session->kernel().agentAs<SimulationAgent>(SimulationAgent::kName);
    if (sim && sim->mode() != events::Mode::Design) {
        session->kernel().send(events::ResetRequested{});
        session->kernel().send(events::SetModeRequested{.mode = events::Mode::Design});
    }
}

QJsonObject McpRuntimeServer::makeResult(const QJsonValue& id, const QJsonValue& result) {
    if (result.isObject() && result.toObject().value(QStringLiteral("__error__")).toBool(false)) {
        const QJsonObject errObj = result.toObject();
        return makeError(id, errObj.value(QStringLiteral("code")).toInt(-32000), errObj.value(QStringLiteral("message")).toString());
    }

    QJsonObject resp;
    resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    resp[QStringLiteral("id")] = id;
    resp[QStringLiteral("result")] = result;
    return resp;
}

QJsonObject McpRuntimeServer::makeError(const QJsonValue& id, int code, const QString& message, const QJsonValue& data) {
    QJsonObject resp;
    resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    resp[QStringLiteral("id")] = id;

    QJsonObject err;
    err[QStringLiteral("code")] = code;
    err[QStringLiteral("message")] = message;
    if (!data.isNull() && !data.isUndefined()) {
        err[QStringLiteral("data")] = data;
    }

    resp[QStringLiteral("error")] = err;
    return resp;
}

}  // namespace app
