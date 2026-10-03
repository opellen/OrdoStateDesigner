#pragma once

#include <memory>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

#include "model/machine.h"

class QLocalServer;
class QLocalSocket;

namespace app {

class MainWindow;
class DocumentSession;
class EditorView;

// Shared wire-shape (de)serializers for Invocation, the single parsing
// authority mcp_runtime_server.cpp and mcp_runtime_server_batch.cpp both
// call -- never duplicate this mapping.
Invocation invocationFromJson(const QJsonObject& obj);
QJsonObject invocationToJson(const Invocation& inv);

// McpRuntimeServer exposes State Designer's live GUI runtime to external AI assistants
// (Claude Desktop, Cursor, Antigravity) via local IPC (QLocalServer / Named Pipe).
// Dispatches line-delimited JSON-RPC 2.0 messages directly on the Qt main thread.
class McpRuntimeServer : public QObject {
    Q_OBJECT

public:
    explicit McpRuntimeServer(MainWindow* window, QObject* parent = nullptr);
    ~McpRuntimeServer() override;

    bool start(const QString& serverName = QStringLiteral("state-designer-live"));
    void stop();

    bool isListening() const;
    QString serverName() const { return serverName_; }

    // Direct invocation without sockets (used by headless smoke tests)
    QJsonObject handleRpcRequest(const QJsonObject& request);
    QByteArray processLine(const QByteArray& line);

private Q_SLOTS:
    void onNewConnection();
    void onClientReadyRead();
    void onClientDisconnected();

private:
    void sendResponse(QLocalSocket* socket, const QJsonObject& response);
    DocumentSession* activeSession() const;
    EditorView* activeView() const;

    // Tool method handlers
    QJsonObject handleGetActiveMachine(const QJsonObject& params);
    QJsonObject handleGetSelection(const QJsonObject& params);
    QJsonObject handleGetSimulationStatus(const QJsonObject& params);
    QJsonObject handleCaptureCanvas(const QJsonObject& params);
    QJsonObject handleCreateMachine(const QJsonObject& params);
    QJsonObject handleBatchCreateMachine(const QJsonObject& params);
    QJsonObject handleCloseProject(const QJsonObject& params);
    QJsonObject handleAddState(const QJsonObject& params);
    QJsonObject handleMoveState(const QJsonObject& params);
    QJsonObject handleApplyAutoLayout(const QJsonObject& params);
    QJsonObject handleRenameState(const QJsonObject& params);
    QJsonObject handleSetStateKind(const QJsonObject& params);
    QJsonObject handleDeleteState(const QJsonObject& params);
    QJsonObject handleReparentState(const QJsonObject& params);
    QJsonObject handleSetInitialState(const QJsonObject& params);
    QJsonObject handleAddTransition(const QJsonObject& params);
    QJsonObject handleMoveTransitionLabel(const QJsonObject& params);
    QJsonObject handleUpdateTransition(const QJsonObject& params);
    QJsonObject handleSetTransitionFlags(const QJsonObject& params);
    QJsonObject handleDeleteTransition(const QJsonObject& params);
    QJsonObject handleAddContextVariable(const QJsonObject& params);
    QJsonObject handleUpdateContextVariable(const QJsonObject& params);
    QJsonObject handleDeleteContextVariable(const QJsonObject& params);
    QJsonObject handleSetInvocations(const QJsonObject& params);
    QJsonObject handleAddInvocation(const QJsonObject& params);
    QJsonObject handleDeleteInvocation(const QJsonObject& params);
    QJsonObject handleCompleteInvocation(const QJsonObject& params);
    QJsonObject handleStepSimulation(const QJsonObject& params);
    QJsonObject handleResetSimulation(const QJsonObject& params);

    // Helpers
    void ensureDesignMode(DocumentSession* session);
    QJsonObject makeError(const QJsonValue& id, int code, const QString& message, const QJsonValue& data = QJsonValue());
    QJsonObject makeResult(const QJsonValue& id, const QJsonValue& result);

    MainWindow* window_ = nullptr;
    QLocalServer* server_ = nullptr;
    QString serverName_;
    QList<QLocalSocket*> clients_;
    QMap<QLocalSocket*, QByteArray> clientBuffers_;
};

}  // namespace app
