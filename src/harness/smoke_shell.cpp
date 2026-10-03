#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <cstdio>
#include <memory>

#include <ordo/core/kernel.h>

#include "controller/edit_commands.h"
#include "controller/undo_commands.h"
#include "infra/project_io.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/undo_store.h"
#include "view/items/state_item.h"
#include "view/shell/document_session.h"
#include "view/shell/editor_view.h"
#include "view/shell/main_window.h"
#include "view/canvas/canvas_view.h"

#include "harness/harness.h"

// Multi-kernel shell smoke: two DocumentSessions (manualClock=true, ticks via
// advanceClock()) driven through the shell's public surface. Proves: (b) edit
// + simulation isolation, (c) a view surviving detach/reattach, (d) two views
// on one kernel stay in lockstep, (e) project save/load reproduces both
// machines with empty undo stacks, (f) post-load edit+undo works. Needs a real
// QApplication, declared first so every QWidget built below dies before it.
int runShellSmoke() {
    int fakeArgc = 1;
    char fakeArgv0[] = "state-designer-shell-smoke";
    char* fakeArgv[] = {fakeArgv0};
    QApplication application(fakeArgc, fakeArgv);

    // ---- a. Two sessions, built via edit intents on each session's own ----
    // kernel. A: a small login-like machine (LoggedOut -Login-> Authenticating
    // -Success-> LoggedIn). B: a traffic-light machine (Red -1000ms-> Green
    // -1000ms-> Yellow -600ms-> Red), pure delayed transitions.
    app::DocumentSession sessionA(QStringLiteral("Login Flow"), /*manualClock=*/true);
    app::DocumentSession sessionB(QStringLiteral("Traffic Light"), /*manualClock=*/true);

    auto docA = sessionA.kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto simA = sessionA.kernel().agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    auto docB = sessionB.kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto simB = sessionB.kernel().agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (!docA || !simA || !docB || !simB) {
        std::fprintf(stderr, "FAIL: shell smoke sessions did not register MachineDocAgent/SimulationAgent\n");
        return 1;
    }

    sessionA.kernel().send(app::events::AddStateRequested{.pos = QPointF(80, 200)});   // id 1: LoggedOut
    sessionA.kernel().send(app::events::AddStateRequested{.pos = QPointF(320, 180)});  // id 2: Authenticating
    sessionA.kernel().send(app::events::AddStateRequested{.pos = QPointF(560, 120)});  // id 3: LoggedIn
    sessionA.kernel().send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("LoggedOut")});
    sessionA.kernel().send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Authenticating")});
    sessionA.kernel().send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("LoggedIn")});
    sessionA.kernel().send(app::events::SetInitialStateRequested{.id = 1});
    sessionA.kernel().send(app::events::SetStateKindRequested{.id = 3, .kind = app::StateKind::Final});
    sessionA.kernel().send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 4: Login
    sessionA.kernel().send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 5: Success
    sessionA.kernel().send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Login")});
    sessionA.kernel().send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Success")});

    sessionB.kernel().send(app::events::AddStateRequested{.pos = QPointF(80, 160)});   // id 1: Red
    sessionB.kernel().send(app::events::AddStateRequested{.pos = QPointF(320, 160)});  // id 2: Green
    sessionB.kernel().send(app::events::AddStateRequested{.pos = QPointF(560, 160)});  // id 3: Yellow
    sessionB.kernel().send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Red")});
    sessionB.kernel().send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Green")});
    sessionB.kernel().send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Yellow")});
    sessionB.kernel().send(app::events::SetInitialStateRequested{.id = 1});
    sessionB.kernel().send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 4: Red -> Green
    sessionB.kernel().send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 5: Green -> Yellow
    sessionB.kernel().send(app::events::AddTransitionRequested{.from = 3, .to = 1});  // id 6: Yellow -> Red
    sessionB.kernel().send(app::events::SetTransitionDelayRequested{.id = 4, .delayMs = 1000});
    sessionB.kernel().send(app::events::SetTransitionDelayRequested{.id = 5, .delayMs = 1000});
    sessionB.kernel().send(app::events::SetTransitionDelayRequested{.id = 6, .delayMs = 600});

    if (docA->machine().states.size() != 3 || docA->machine().transitions.size() != 2) {
        std::fprintf(stderr, "FAIL: shell smoke session A's build did not produce 3 states / 2 transitions\n");
        return 1;
    }
    if (docB->machine().states.size() != 3 || docB->machine().transitions.size() != 3) {
        std::fprintf(stderr, "FAIL: shell smoke session B's build did not produce 3 states / 3 transitions\n");
        return 1;
    }
    const app::Machine baselineB = docB->machine();

    // ---- b. Isolation: edits + simulation on A, manual ticks on B ---------
    sessionA.kernel().send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("LoggedOutV2")});
    sessionA.kernel().send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    sessionA.kernel().send(app::events::RunRequested{});                                    // active: LoggedOutV2
    sessionA.kernel().send(app::events::SendEventRequested{.name = QStringLiteral("Login")});  // active: Authenticating

    sessionB.kernel().send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    sessionB.kernel().send(app::events::RunRequested{});  // active: Red, arms Red->Green (1000ms)
    sessionB.advanceClock(1000);                          // fires Red->Green, arms Green->Yellow (1000ms)
    sessionB.advanceClock(1000);                          // fires Green->Yellow, arms Yellow->Red (600ms)

    if (!(docB->machine() == baselineB)) {
        std::fprintf(stderr, "FAIL: shell smoke isolation -- session B's topology changed by session A's edits\n");
        return 1;
    }
    if (docA->machine().states.size() != 3 || docA->machine().transitions.size() != 2 ||
        docA->findState(1) == nullptr || docA->findState(1)->name != QStringLiteral("LoggedOutV2")) {
        std::fprintf(stderr, "FAIL: shell smoke isolation -- session A's own edit did not land intact\n");
        return 1;
    }
    if (simA->activeStateId() != 2) {
        std::fprintf(stderr,
                     "FAIL: shell smoke isolation -- session A's active state is %llu (want 2, Authenticating)\n",
                     static_cast<unsigned long long>(simA->activeStateId()));
        return 1;
    }
    if (simB->activeStateId() != 3) {
        std::fprintf(stderr, "FAIL: shell smoke isolation -- session B's active state is %llu (want 3, Yellow)\n",
                     static_cast<unsigned long long>(simB->activeStateId()));
        return 1;
    }
    if (simA->trace().size() != 2) {
        std::fprintf(stderr, "FAIL: shell smoke isolation -- session A's trace has %d entries (want 2)\n",
                     static_cast<int>(simA->trace().size()));
        return 1;
    }
    if (simB->trace().size() != 3) {
        std::fprintf(stderr, "FAIL: shell smoke isolation -- session B's trace has %d entries (want 3)\n",
                     static_cast<int>(simB->trace().size()));
        return 1;
    }
    const QString traceA = simA->trace().join(QStringLiteral("|"));
    const QString traceB = simB->trace().join(QStringLiteral("|"));
    if (traceA.contains(QStringLiteral("Red")) || traceA.contains(QStringLiteral("Green")) ||
        traceA.contains(QStringLiteral("Yellow"))) {
        std::fprintf(stderr, "FAIL: shell smoke isolation -- session A's trace leaked session B content: %s\n",
                     qUtf8Printable(traceA));
        return 1;
    }
    if (traceB.contains(QStringLiteral("Login")) || traceB.contains(QStringLiteral("Authenticating")) ||
        traceB.contains(QStringLiteral("LoggedOut"))) {
        std::fprintf(stderr, "FAIL: shell smoke isolation -- session B's trace leaked session A content: %s\n",
                     qUtf8Printable(traceB));
        return 1;
    }

    // ---- c. View attach/reattach on A --------------------------------------
    // Back to Design so edits are accepted again (rejected while Simulate).
    sessionA.kernel().send(app::events::SetModeRequested{.mode = app::events::Mode::Design});

    auto viewA1 = std::make_unique<app::CanvasView>();
    app::CanvasPresenter* presenter1 = sessionA.attachView(viewA1.get());
    if (presenter1 == nullptr || viewA1->scene() == nullptr) {
        std::fprintf(stderr, "FAIL: shell smoke -- attachView on session A did not produce a presenter/scene\n");
        return 1;
    }
    const int itemsBaseline1 = viewA1->scene()->items().size();

    sessionA.kernel().send(app::events::AddStateRequested{.pos = QPointF(700, 700)});  // id 6, orphan state
    const int itemsAfterAdd1 = viewA1->scene()->items().size();
    if (itemsAfterAdd1 <= itemsBaseline1) {
        std::fprintf(stderr, "FAIL: shell smoke -- presenter 1's item count did not grow after AddStateRequested\n");
        return 1;
    }
    const int perStateItemDelta = itemsAfterAdd1 - itemsBaseline1;

    sessionA.kernel().send(app::events::DeleteStateRequested{.id = 6});
    if (viewA1->scene()->items().size() != itemsBaseline1) {
        std::fprintf(stderr, "FAIL: shell smoke -- presenter 1's item count did not return to baseline after delete\n");
        return 1;
    }

    sessionA.detachView(viewA1.get());
    if (viewA1->scene() != nullptr) {
        std::fprintf(stderr, "FAIL: shell smoke -- detachView did not clear the view's scene\n");
        return 1;
    }

    // Edit while detached: no crash, and the agent (the source of truth)
    // keeps advancing with zero presenters attached.
    sessionA.kernel().send(app::events::AddStateRequested{.pos = QPointF(700, 700)});  // id 7
    if (docA->machine().states.size() != 4 || docA->findState(7) == nullptr) {
        std::fprintf(stderr, "FAIL: shell smoke -- session A did not advance while detached\n");
        return 1;
    }

    auto viewA2 = std::make_unique<app::CanvasView>();
    app::CanvasPresenter* presenter2 = sessionA.attachView(viewA2.get());
    if (presenter2 == nullptr || viewA2->scene() == nullptr) {
        std::fprintf(stderr, "FAIL: shell smoke -- re-attaching a fresh view on session A failed\n");
        return 1;
    }
    const int itemsBaseline2 = viewA2->scene()->items().size();
    if (itemsBaseline2 != itemsBaseline1 + perStateItemDelta) {
        std::fprintf(stderr,
                     "FAIL: shell smoke -- reattached presenter's snapshot rebuild (%d items) does not match the "
                     "current +1-state truth (want %d)\n",
                     itemsBaseline2, itemsBaseline1 + perStateItemDelta);
        return 1;
    }

    sessionA.kernel().send(app::events::AddStateRequested{.pos = QPointF(750, 750)});  // id 8
    if (viewA2->scene()->items().size() - itemsBaseline2 != perStateItemDelta) {
        std::fprintf(stderr, "FAIL: shell smoke -- reattached presenter did not track a new add the same way\n");
        return 1;
    }
    sessionA.kernel().send(app::events::DeleteStateRequested{.id = 8});
    if (viewA2->scene()->items().size() != itemsBaseline2) {
        std::fprintf(stderr, "FAIL: shell smoke -- reattached presenter did not track a delete back to baseline\n");
        return 1;
    }
    sessionA.detachView(viewA2.get());

    // ---- d. Two views on ONE kernel stay in lockstep -----------------------
    auto viewA3 = std::make_unique<app::CanvasView>();
    auto viewA4 = std::make_unique<app::CanvasView>();
    app::CanvasPresenter* presenter3 = sessionA.attachView(viewA3.get());
    app::CanvasPresenter* presenter4 = sessionA.attachView(viewA4.get());
    if (presenter3 == nullptr || presenter4 == nullptr) {
        std::fprintf(stderr, "FAIL: shell smoke -- two simultaneous attachViews on session A failed\n");
        return 1;
    }

    app::StateItem* item3Before = findStateItemById(viewA3->scene(), 1);
    app::StateItem* item4Before = findStateItemById(viewA4->scene(), 1);
    if (item3Before == nullptr || item4Before == nullptr) {
        std::fprintf(stderr, "FAIL: shell smoke -- state 1's StateItem missing from one of the two views\n");
        return 1;
    }
    const qreal width3Before = item3Before->boundingRect().width();
    const qreal width4Before = item4Before->boundingRect().width();

    sessionA.kernel().send(app::events::RenameStateRequested{
        .id = 1, .name = QStringLiteral("A Considerably Longer Renamed State Label For Width")});

    app::StateItem* item3After = findStateItemById(viewA3->scene(), 1);
    app::StateItem* item4After = findStateItemById(viewA4->scene(), 1);
    if (item3After == nullptr || item4After == nullptr) {
        std::fprintf(stderr, "FAIL: shell smoke -- state 1's StateItem disappeared after a rename\n");
        return 1;
    }
    if (!(item3After->boundingRect().width() > width3Before)) {
        std::fprintf(stderr, "FAIL: shell smoke -- presenter 3 did not reflect the RenameStateRequested\n");
        return 1;
    }
    if (!(item4After->boundingRect().width() > width4Before)) {
        std::fprintf(stderr, "FAIL: shell smoke -- presenter 4 did not reflect the RenameStateRequested\n");
        return 1;
    }

    sessionA.detachView(viewA3.get());
    sessionA.detachView(viewA4.get());

    // ---- e. Persistence round trip -----------------------------------------
    const QString smokeDir = QDir::temp().filePath(QStringLiteral("ordo-state-designer-shell-smoke"));
    QDir(smokeDir).removeRecursively();  // stale files from a killed prior run must not leak in
    QDir().mkpath(smokeDir);
    const QString projectPath = smokeDir + QStringLiteral("/project.sdp");

    const std::vector<app::DocumentSession*> toSave{&sessionA, &sessionB};
    app::Project savedProject;
    QString ioError;
    if (!app::saveProjectSessions(projectPath, toSave, QStringLiteral("generated"), QStringLiteral("app::generated"),
                                   &savedProject, &ioError)) {
        std::fprintf(stderr, "FAIL: shell smoke -- saveProjectSessions failed: %s\n", qUtf8Printable(ioError));
        return 1;
    }

    // Loaded back through the dialog-free path MainWindow::openProject() uses;
    // manualClock=true keeps the reloaded sessions drivable (part f).
    app::Project loadedProjectMeta;
    std::vector<std::unique_ptr<app::DocumentSession>> loadedSessions =
        app::loadProjectSessions(projectPath, &loadedProjectMeta, &ioError, /*manualClock=*/true);
    if (loadedSessions.size() != 2) {
        std::fprintf(stderr, "FAIL: shell smoke -- loadProjectSessions failed or returned %d session(s) (want 2): %s\n",
                     static_cast<int>(loadedSessions.size()), qUtf8Printable(ioError));
        return 1;
    }
    if (loadedProjectMeta.outputDir != QStringLiteral("generated") ||
        loadedProjectMeta.rootNamespace != QStringLiteral("app::generated")) {
        std::fprintf(stderr, "FAIL: shell smoke -- reloaded Project's outputDir/rootNamespace did not round-trip\n");
        return 1;
    }

    auto loadedDocA = loadedSessions[0]->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto loadedDocB = loadedSessions[1]->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto loadedUndoA = loadedSessions[0]->kernel().agentAs<app::UndoStore>(app::UndoStore::kName);
    auto loadedUndoB = loadedSessions[1]->kernel().agentAs<app::UndoStore>(app::UndoStore::kName);
    if (!loadedDocA || !loadedDocB || !loadedUndoA || !loadedUndoB) {
        std::fprintf(stderr, "FAIL: shell smoke -- reloaded sessions did not register MachineDocAgent/UndoStore\n");
        return 1;
    }

    // Deep topology equality via sameTopology(), which ignores nextId.
    if (!sameTopology(loadedDocA->machine(), docA->machine())) {
        std::fprintf(stderr, "FAIL: shell smoke -- reloaded session A's topology does not match the saved one\n");
        return 1;
    }
    if (!sameTopology(loadedDocB->machine(), docB->machine())) {
        std::fprintf(stderr, "FAIL: shell smoke -- reloaded session B's topology does not match the saved one\n");
        return 1;
    }
    if (loadedUndoA->canUndo() || loadedUndoA->canRedo() || loadedUndoB->canUndo() || loadedUndoB->canRedo()) {
        std::fprintf(stderr, "FAIL: shell smoke -- a reloaded session's undo stack is not empty\n");
        return 1;
    }

    // ---- f. Undo interleave guard: one edit + undo after load --------------
    app::DocumentSession* loadedA = loadedSessions[0].get();
    const app::Machine loadedBaseline = loadedDocA->machine();
    if (loadedBaseline.states.isEmpty()) {
        std::fprintf(stderr, "FAIL: shell smoke -- reloaded session A has no states to exercise the undo guard on\n");
        return 1;
    }
    const quint64 firstStateId = loadedBaseline.states.first().id;

    loadedA->kernel().send(
        app::events::RenameStateRequested{.id = firstStateId, .name = QStringLiteral("Post-Load Undo Guard Name")});
    if (loadedDocA->findState(firstStateId) == nullptr ||
        loadedDocA->findState(firstStateId)->name != QStringLiteral("Post-Load Undo Guard Name")) {
        std::fprintf(stderr, "FAIL: shell smoke -- post-load edit did not apply\n");
        return 1;
    }
    loadedA->kernel().send(app::events::UndoRequested{});
    if (!(loadedDocA->machine() == loadedBaseline)) {
        std::fprintf(stderr, "FAIL: shell smoke -- undo after a post-load edit did not restore the loaded state\n");
        return 1;
    }

    // ---- g. Struct context commands & undo/redo verification on session -----
    // Verify SetExternalHeadersRequested, AddStructDefinitionRequested,
    // SetStructDefinitionRequested, SetTransitionPayloadTypeRequested,
    // SetContextCustomTypeNameRequested, DeleteStructDefinitionRequested,
    // and that UndoRequested accurately reverts them.

    // 1) External headers
    loadedA->kernel().send(app::events::SetExternalHeadersRequested{
        .headers = {QStringLiteral("\"custom_types.h\""), QStringLiteral("<vector>")}});
    if (loadedDocA->machine().externalHeaders.size() != 2 ||
        loadedDocA->machine().externalHeaders.at(0) != QStringLiteral("\"custom_types.h\"")) {
        std::fprintf(stderr, "FAIL: shell smoke -- SetExternalHeadersRequested did not apply\n");
        return 1;
    }
    loadedA->kernel().send(app::events::UndoRequested{});
    if (!loadedDocA->machine().externalHeaders.isEmpty()) {
        std::fprintf(stderr, "FAIL: shell smoke -- undo of SetExternalHeadersRequested failed\n");
        return 1;
    }
    loadedA->kernel().send(app::events::RedoRequested{});
    if (loadedDocA->machine().externalHeaders.size() != 2) {
        std::fprintf(stderr, "FAIL: shell smoke -- redo of SetExternalHeadersRequested failed\n");
        return 1;
    }

    // 2) Add struct definition
    app::StructDefinition structDef;
    structDef.name = QStringLiteral("Packet");
    app::StructField f1;
    f1.name = QStringLiteral("seq");
    f1.type = app::FieldType::Int;
    f1.initialValue = QStringLiteral("1");
    structDef.fields.append(f1);
    loadedA->kernel().send(app::events::AddStructDefinitionRequested{.definition = structDef});
    if (loadedDocA->machine().types.size() != 1 || loadedDocA->machine().types.at(0).name != QStringLiteral("Packet")) {
        std::fprintf(stderr, "FAIL: shell smoke -- AddStructDefinitionRequested did not apply\n");
        return 1;
    }
    const quint64 packetStructId = loadedDocA->machine().types.at(0).id;

    // 3) Set struct definition
    app::StructDefinition modifiedDef = loadedDocA->machine().types.at(0);
    modifiedDef.name = QStringLiteral("NetworkPacket");
    loadedA->kernel().send(app::events::SetStructDefinitionRequested{
        .id = packetStructId,
        .definition = modifiedDef
    });
    if (loadedDocA->machine().types.at(0).name != QStringLiteral("NetworkPacket")) {
        std::fprintf(stderr, "FAIL: shell smoke -- SetStructDefinitionRequested did not apply\n");
        return 1;
    }
    loadedA->kernel().send(app::events::UndoRequested{});
    if (loadedDocA->machine().types.at(0).name != QStringLiteral("Packet")) {
        std::fprintf(stderr, "FAIL: shell smoke -- undo of SetStructDefinitionRequested failed\n");
        return 1;
    }

    // 4) Set transition payload type
    const quint64 firstTransId = loadedDocA->machine().transitions.first().id;
    loadedA->kernel().send(app::events::SetTransitionPayloadTypeRequested{
        .id = firstTransId,
        .payloadType = QStringLiteral("Packet")
    });
    if (loadedDocA->findTransition(firstTransId)->payloadType != QStringLiteral("Packet")) {
        std::fprintf(stderr, "FAIL: shell smoke -- SetTransitionPayloadTypeRequested did not apply\n");
        return 1;
    }
    loadedA->kernel().send(app::events::UndoRequested{});
    if (!loadedDocA->findTransition(firstTransId)->payloadType.isEmpty()) {
        std::fprintf(stderr, "FAIL: shell smoke -- undo of SetTransitionPayloadTypeRequested failed\n");
        return 1;
    }

    // 5) Set context custom type name
    loadedA->kernel().send(app::events::AddContextVariableRequested{});
    const quint64 pktVarId = loadedDocA->machine().context.last().id;
    loadedA->kernel().send(app::events::SetContextTypeRequested{
        .id = pktVarId,
        .type = app::ContextType::Object
    });
    loadedA->kernel().send(app::events::SetContextCustomTypeNameRequested{
        .id = pktVarId,
        .customTypeName = QStringLiteral("Packet")
    });
    if (loadedDocA->findContextVariable(pktVarId)->customTypeName != QStringLiteral("Packet")) {
        std::fprintf(stderr, "FAIL: shell smoke -- SetContextCustomTypeNameRequested did not apply\n");
        return 1;
    }
    loadedA->kernel().send(app::events::UndoRequested{});
    if (!loadedDocA->findContextVariable(pktVarId)->customTypeName.isEmpty()) {
        std::fprintf(stderr, "FAIL: shell smoke -- undo of SetContextCustomTypeNameRequested failed\n");
        return 1;
    }

    // 6) Delete struct definition and undo
    loadedA->kernel().send(app::events::DeleteStructDefinitionRequested{.id = packetStructId});
    if (!loadedDocA->machine().types.isEmpty()) {
        std::fprintf(stderr, "FAIL: shell smoke -- DeleteStructDefinitionRequested did not apply\n");
        return 1;
    }
    loadedA->kernel().send(app::events::UndoRequested{});
    if (loadedDocA->machine().types.size() != 1 || loadedDocA->machine().types.at(0).id != packetStructId) {
        std::fprintf(stderr, "FAIL: shell smoke -- undo of DeleteStructDefinitionRequested failed\n");
        return 1;
    }

    // 7) Machine renaming, undo, and relativePath persistence in project
    if (loadedA->relativePath() != QStringLiteral("login_flow.sdm")) {
        std::fprintf(stderr, "FAIL: shell smoke -- loadedA relativePath is '%s', expected 'login_flow.sdm'\n",
                     loadedA->relativePath().toUtf8().constData());
        return 1;
    }
    const QString origName = loadedDocA->machine().name;
    loadedA->kernel().send(app::events::SetMachineNameRequested{.name = QStringLiteral("MachineARenamed")});
    if (loadedDocA->machine().name != QStringLiteral("MachineARenamed")) {
        std::fprintf(stderr, "FAIL: shell smoke -- SetMachineNameRequested did not update machine name\n");
        return 1;
    }
    loadedA->setMachineName(QStringLiteral("MachineARenamed"));
    if (loadedA->machineName() != QStringLiteral("MachineARenamed")) {
        std::fprintf(stderr, "FAIL: shell smoke -- setMachineName failed\n");
        return 1;
    }
    loadedA->kernel().send(app::events::UndoRequested{});
    if (loadedDocA->machine().name != origName) {
        std::fprintf(stderr, "FAIL: shell smoke -- undo of SetMachineNameRequested failed: got '%s', want '%s'\n",
                     loadedDocA->machine().name.toUtf8().constData(), origName.toUtf8().constData());
        return 1;
    }

    // 8) MainWindow Close Project and empty workspace lifecycle
    {
        app::MainWindow window;
        // A dev-demo build boots with two sessions; a default build boots
        // empty, so open two the way a user would.
        if (window.debugSessionCount() == 0) {
            window.createNewMachine(QStringLiteral("BootA"));
            window.createNewMachine(QStringLiteral("BootB"));
        }
        if (window.debugSessionCount() != 2) {
            std::fprintf(stderr, "FAIL: shell smoke -- expected 2 boot sessions, got %d\n",
                         window.debugSessionCount());
            return 1;
        }
        if (window.debugCloseProjectAction() == nullptr || !window.debugCloseProjectAction()->isEnabled()) {
            std::fprintf(stderr, "FAIL: shell smoke -- closeProjectAction should be enabled when sessions are open\n");
            return 1;
        }
        if (window.focusedView() == nullptr) {
            std::fprintf(stderr, "FAIL: shell smoke -- focusedView is null before closeProject\n");
            return 1;
        }

        bool closed = window.closeProject(/*promptIfDirty=*/false);
        if (!closed) {
            std::fprintf(stderr, "FAIL: shell smoke -- closeProject returned false\n");
            return 1;
        }
        if (window.debugSessionCount() != 0) {
            std::fprintf(stderr, "FAIL: shell smoke -- sessions count should be 0 after closeProject, got %d\n",
                         window.debugSessionCount());
            return 1;
        }
        if (window.focusedView() != nullptr) {
            std::fprintf(stderr, "FAIL: shell smoke -- focusedView should be null after closeProject\n");
            return 1;
        }
        if (window.debugCloseProjectAction()->isEnabled()) {
            std::fprintf(stderr, "FAIL: shell smoke -- closeProjectAction should be disabled in empty workspace\n");
            return 1;
        }

        // Create new machine in empty workspace
        app::DocumentSession* newSession = window.createNewMachine(QStringLiteral("FreshMachine"));
        if (!newSession || window.debugSessionCount() != 1) {
            std::fprintf(stderr, "FAIL: shell smoke -- createNewMachine in empty workspace failed\n");
            return 1;
        }
        if (window.focusedView() == nullptr || window.focusedView()->session() != newSession) {
            std::fprintf(stderr, "FAIL: shell smoke -- newly created machine is not focused\n");
            return 1;
        }
        if (!window.debugCloseProjectAction()->isEnabled()) {
            std::fprintf(stderr, "FAIL: shell smoke -- closeProjectAction should be re-enabled after creating machine\n");
            return 1;
        }

        // Close project again
        window.closeProject(/*promptIfDirty=*/false);
        if (window.debugSessionCount() != 0) {
            std::fprintf(stderr, "FAIL: shell smoke -- sessions count should be 0 after second closeProject\n");
            return 1;
        }
    }

    std::printf("PASS: state-designer shell smoke (isolation + view lifecycle + project round trip + struct/type commands and undo + close project)\n");
    return 0;
}
