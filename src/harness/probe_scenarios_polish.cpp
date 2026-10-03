// --gui-probe polish scenarios: add-child-state, guarded groups, historyDeep,
// alignment guides, the edge-style menu, the minimap, container authoring,
// simulation features, and the XState interop round trip (which must run last).

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHash>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPair>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointF>
#include <QRect>
#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <ordo/core/kernel.h>
#include <ordo/qt/view_host.h>

#include "controller/edit_commands.h"
#include "controller/sim_commands.h"
#include "controller/undo_capture.h"
#include "controller/undo_commands.h"
#include "infra/code_generator.h"
#include "infra/machine_validator.h"
#include "infra/project_io.h"
#include "infra/sim_clock.h"
#include "infra/xstate_v5_io.h"
#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/sim_agent.h"
#include "model/sim_events.h"
#include "model/undo_events.h"
#include "model/undo_store.h"
#include "view/canvas/canvas_presenter.h"
#include "view/canvas/canvas_view.h"
#include "view/generated/canvas_interaction_core.h"  // drives the machine directly
#include "view/shell/document_session.h"
#include "view/shell/editor_view.h"
#include "view/shell/inspector_panel.h"
#include "view/shell/logic_panel.h"
#include "view/shell/main_window.h"
#include "view/items/machine_frame_item.h"
#include "view/shell/minimap_view.h"
#include "view/items/note_item.h"
#include "view/geometry/pill_port_resolver.h"  // asserts the rule table 1:1
#include "view/items/state_item.h"
#include "view/shell/theme.h"
#include "view/shell/trace_panel.h"
#include "view/items/transition_item.h"

#include "harness/harness.h"
#include "harness/probe_scenarios.h"

// Scenario "xstate-interop": the dialog-free cores behind File > Import/Export
// XState v5. Exports the login-flow machine to JSON in the probe dir,
// re-imports it through the menu handler's adopt path
// (MainWindow::debugImportXState), and asserts the new session opens focused
// with the machine reproduced by name-keyed identity (ids differ). Runs last:
// it leaves the imported session open, since the shell has no Close Machine verb.
int runXStateInteropScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr) {
        std::fprintf(stderr, "FAIL: xstate-interop scenario preconditions (doc) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    const app::Machine original = doc->machine();

    const QString jsonPath = probeCaptureDir() + QStringLiteral("/probe-xstate-login-flow.json");
    QStringList exportDiagnostics;
    QString error;
    if (!app::exportXStateFile(original, jsonPath, &exportDiagnostics, &error)) {
        std::fprintf(stderr, "FAIL: exportXStateFile refused the login-flow machine: %s\n", qUtf8Printable(error));
        return 1;
    }
    if (!exportDiagnostics.isEmpty()) {
        for (const QString& d : exportDiagnostics) {
            std::fprintf(stderr, "DIAGNOSTIC: %s\n", qUtf8Printable(d));
        }
        std::fprintf(stderr, "FAIL: exporting the login-flow machine produced %d diagnostic(s), want none\n",
                     static_cast<int>(exportDiagnostics.size()));
        return 1;
    }

    QStringList importDiagnostics;
    app::DocumentSession* imported = window.debugImportXState(jsonPath, &importDiagnostics, &error);
    QApplication::processEvents();
    if (imported == nullptr) {
        std::fprintf(stderr, "FAIL: debugImportXState failed: %s\n", qUtf8Printable(error));
        return 1;
    }
    if (!importDiagnostics.isEmpty()) {
        std::fprintf(stderr, "FAIL: importing this app's own export produced %d diagnostic(s), want none\n",
                     static_cast<int>(importDiagnostics.size()));
        return 1;
    }

    auto importedDoc = imported->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (importedDoc == nullptr) {
        std::fprintf(stderr, "FAIL: the imported session has no MachineDocAgent\n");
        return 1;
    }
    const app::Machine& copy = importedDoc->machine();
    if (copy.name != original.name || copy.states.size() != original.states.size() ||
        copy.transitions.size() != original.transitions.size()) {
        std::fprintf(stderr, "FAIL: reimported machine differs (name/state/transition counts)\n");
        return 1;
    }
    // Name-keyed identity: the initial state's NAME survives; geometry rides
    // meta.ordo, so each state also sits exactly where its original does.
    for (const app::State& state : copy.states) {
        const app::State* match = nullptr;
        for (const app::State& originalState : original.states) {
            if (originalState.name == state.name) {
                match = &originalState;
            }
        }
        if (match == nullptr || match->pos != state.pos || match->kind != state.kind) {
            std::fprintf(stderr, "FAIL: reimported state '%s' has no name/pos/kind match\n",
                         qUtf8Printable(state.name));
            return 1;
        }
        if ((original.initialStateId == match->id) != (copy.initialStateId == state.id)) {
            std::fprintf(stderr, "FAIL: initial-state identity did not survive by name\n");
            return 1;
        }
    }
    // The adopt path opened the imported machine in the focused group --
    // the focused view must now be bound to the NEW session, not loginPane's.
    app::EditorView* focused = window.focusedView();
    if (focused == nullptr || focused->session() != imported) {
        std::fprintf(stderr, "FAIL: the imported session did not open focused\n");
        return 1;
    }
    // The capture evidences the meta.ordo geometry claim: the imported
    // canvas must be pixel-comparable to the login-flow original.
    if (!saveSceneCapture(focused, "probe-xstate-interop")) {
        return 1;
    }

    // Also test native .sdm import via debugImportStateMachine
    app::DocumentSession* importedSdm = window.debugImportStateMachine(
        QStringLiteral("src/machines/motion-controller.sdm"), nullptr, &error);
    QApplication::processEvents();
    if (importedSdm == nullptr) {
        std::fprintf(stderr, "FAIL: debugImportStateMachine failed for motion-controller.sdm: %s\n",
                     qUtf8Printable(error));
        return 1;
    }
    if (importedSdm->machineName() != QStringLiteral("Motion Controller")) {
        std::fprintf(stderr, "FAIL: importedSdm name '%s' != 'Motion Controller'\n",
                     qUtf8Printable(importedSdm->machineName()));
        return 1;
    }
    app::EditorView* sdmView = window.focusedView();
    if (sdmView == nullptr || sdmView->session() != importedSdm) {
        std::fprintf(stderr, "FAIL: importedSdm did not open focused\n");
        return 1;
    }
    saveSceneCapture(sdmView, "probe-import-motion-controller");
    if (window.debugInspector() != nullptr) {
        saveWidgetCapture(window.debugInspector(), "probe-import-motion-inspector");
    }

    std::printf("PASS: gui-probe scenario xstate-interop (export -> import opens focused, geometry preserved; .sdm native import verified)\n");
    return 0;
}
// Scenario "add-child-state": builds P{A}, then adds via a synthetic interior
// double-click in empty space inside P's box (debugCanvasDoubleClicked bypasses
// CanvasView, so onCanvasDoubleClicked's parent resolution is what is proved)
// and checks the new state is P's child and P's box grew. P's context menu
// offers "Add Child State"; LoggedIn (id 3, Final) does not. One undo reverts
// the add.
int runAddChildStateScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || view == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: add-child-state scenario preconditions (doc/presenter/view/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;

    // ---- build P{A}, far outside the fixture, 12px off the 24px grid (a
    // ---- position exactly at kSnapRadius=4.0 would settle onto its snapped
    // ---- neighbor when undo restores it) ---------------------------------------
    const quint64 kP = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2412.0, 708.0)});
    const quint64 kA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2364.0, 852.0)});
    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = kP});
    QApplication::processEvents();

    app::StateItem* pItem = findStateItemById(scene, kP);
    app::StateItem* aItem = findStateItemById(scene, kA);
    if (pItem == nullptr || aItem == nullptr || !pItem->isContainerMode()) {
        std::fprintf(stderr, "FAIL: add-child-state scenario's P{A} setup did not take\n");
        return 1;
    }

    // ---- synthetic interior double-click: empty space inside P's box, clear
    // ---- of A's own leaf rect -------------------------------------------------
    const QRectF pRectBefore = pItem->sceneRect();
    const QPointF clickPoint(pRectBefore.right() - 8.0, pRectBefore.bottom() - 8.0);
    if (aItem->sceneRect().contains(clickPoint)) {
        std::fprintf(stderr, "FAIL: add-child-state scenario's click point unexpectedly lands on A's own rect\n");
        return 1;
    }
    const quint64 kChild = doc->machine().nextId;
    presenter->debugCanvasDoubleClicked(clickPoint);
    QApplication::processEvents();

    if (doc->findState(kChild) == nullptr || doc->findState(kChild)->parentId != kP) {
        std::fprintf(stderr, "FAIL: an interior double-click did not add a new state parented to P\n");
        return 1;
    }
    app::StateItem* childItem = findStateItemById(scene, kChild);
    if (childItem == nullptr || !pItem->sceneRect().contains(childItem->sceneRect())) {
        std::fprintf(stderr, "FAIL: P's container box did not grow to hold the double-click-added child\n");
        return 1;
    }

    if (!saveSceneRegionCapture(loginPane, pItem->sceneRect().adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-add-child-state")) {
        return 1;
    }

    // ---- menu structure: P (Normal, has children) offers "Add Child State";
    // ---- LoggedIn (id 3, Final) does not --------------------------------------
    QMenu* pMenu = presenter->debugBuildStateContextMenu(kP);
    if (pMenu == nullptr) {
        std::fprintf(stderr, "FAIL: debugBuildStateContextMenu(P) returned no menu\n");
        return 1;
    }
    bool pHasAddChild = false;
    for (QAction* action : pMenu->actions()) {
        pHasAddChild = pHasAddChild || action->text() == QStringLiteral("Add Child State");
    }
    pMenu->deleteLater();
    if (!pHasAddChild) {
        std::fprintf(stderr, "FAIL: P's (Normal) context menu does not offer \"Add Child State\"\n");
        return 1;
    }
    QMenu* finalMenu = presenter->debugBuildStateContextMenu(3);  // LoggedIn -- Final kind
    if (finalMenu == nullptr) {
        std::fprintf(stderr, "FAIL: debugBuildStateContextMenu(3) returned no menu\n");
        return 1;
    }
    bool finalHasAddChild = false;
    for (QAction* action : finalMenu->actions()) {
        finalHasAddChild = finalHasAddChild || action->text() == QStringLiteral("Add Child State");
    }
    finalMenu->deleteLater();
    if (finalHasAddChild) {
        std::fprintf(stderr, "FAIL: a Final-kind state's context menu unexpectedly offers \"Add Child State\"\n");
        return 1;
    }

    // ---- ONE undo reverts the double-click add --------------------------------
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findState(kChild) != nullptr) {
        std::fprintf(stderr, "FAIL: ONE undo did not remove the double-click-added child\n");
        return 1;
    }

    // ---- clean up: delete P (cascades A) as ONE batch --------------------------
    scene->clearSelection();
    pItem->setSelected(true);
    QApplication::processEvents();
    presenter->debugDeleteSelection();
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the add-child-state scenario did not restore the machine it borrowed\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario add-child-state (context-menu verb + interior double-click parent "
                "resolution, container growth, menu-structure Normal/Final gating, one-undo)\n");
    return 0;
}

// Scenario "guarded-group": the guard-aware duplicate-(from, event) gate proved
// on the real canvas, through AddStateRequested/AddTransitionRequested plus the
// intents InspectorPanel's Event/Guard fields send
// (SetTransitionEventRequested/SetTransitionGuardRequested). Builds two fresh
// destination states off one fresh source, far outside the login-flow fixture.
int runGuardedGroupScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    QGraphicsScene* scene = loginPane->canvasView() != nullptr ? loginPane->canvasView()->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: guarded-group scenario preconditions (doc/presenter/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;

    // ---- build ONE source, TWO destinations, far outside the fixture ---------
    const quint64 kSrc = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2412.0, 300.0)});
    const quint64 kDstA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2700.0, 200.0)});
    const quint64 kDstB = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2700.0, 450.0)});

    // ---- two transitions sharing (kSrc, "Check") with DISTINCT guards --------
    // -- allowed because neither candidate is unguarded.
    const quint64 kT1 = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kSrc, .to = kDstA});
    kernel.send(app::events::SetTransitionEventRequested{.id = kT1, .event = QStringLiteral("Check")});
    kernel.send(app::events::SetTransitionGuardRequested{.id = kT1, .guard = QStringLiteral("condA")});
    const quint64 kT2 = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kSrc, .to = kDstB});
    kernel.send(app::events::SetTransitionEventRequested{.id = kT2, .event = QStringLiteral("Check")});
    kernel.send(app::events::SetTransitionGuardRequested{.id = kT2, .guard = QStringLiteral("condB")});
    QApplication::processEvents();

    if (!doc->findTransition(kT1) || !doc->findTransition(kT2) ||
        doc->findTransition(kT1)->event != QStringLiteral("Check") ||
        doc->findTransition(kT2)->event != QStringLiteral("Check") ||
        doc->findTransition(kT1)->guard != QStringLiteral("condA") ||
        doc->findTransition(kT2)->guard != QStringLiteral("condB")) {
        std::fprintf(stderr, "FAIL: guarded-group scenario's two distinct-guard transitions were not both authored\n");
        return 1;
    }
    if (!presenter->debugHasTransitionVisual(kT1) || !presenter->debugHasTransitionVisual(kT2)) {
        std::fprintf(stderr,
                     "FAIL: both transitions sharing (kSrc, 'Check') must have their own canvas visual (two pills "
                     "on one source for one event)\n");
        return 1;
    }

    // ---- capture: both pills visibly coexisting -------------------------------
    app::StateItem* srcItem = findStateItemById(scene, kSrc);
    app::StateItem* dstAItem = findStateItemById(scene, kDstA);
    app::StateItem* dstBItem = findStateItemById(scene, kDstB);
    if (srcItem == nullptr || dstAItem == nullptr || dstBItem == nullptr) {
        std::fprintf(stderr, "FAIL: guarded-group scenario's three fixture states did not all land on the scene\n");
        return 1;
    }
    const QRectF region =
        srcItem->sceneRect().united(dstAItem->sceneRect()).united(dstBItem->sceneRect()).adjusted(-40.0, -40.0, 40.0, 40.0);
    if (!saveSceneRegionCapture(loginPane, region, "probe-guarded-group")) {
        return 1;
    }

    // ---- refusal: a second unguarded member is rejected, canvas unchanged ----
    // id kT3 joins as the group's one unguarded fallback (legal -- kT1/kT2
    // are both guarded); id kT4 then tries to join the SAME group unguarded
    // too and must be refused, leaving its event blank.
    const quint64 kT3 = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kSrc, .to = kDstA});
    kernel.send(app::events::SetTransitionEventRequested{.id = kT3, .event = QStringLiteral("Check")});
    if (!doc->findTransition(kT3) || doc->findTransition(kT3)->event != QStringLiteral("Check") ||
        !doc->findTransition(kT3)->guard.isEmpty()) {
        std::fprintf(stderr, "FAIL: guarded-group scenario's unguarded fallback (id kT3) was not authored\n");
        return 1;
    }
    const quint64 kT4 = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kSrc, .to = kDstB});
    kernel.send(app::events::SetTransitionEventRequested{.id = kT4, .event = QStringLiteral("Check")});
    if (!doc->findTransition(kT4) || !doc->findTransition(kT4)->event.isEmpty()) {
        std::fprintf(stderr,
                     "FAIL: a second unguarded transition (id kT4) joining a group that already has one (id kT3) "
                     "was not refused -- canvas would show a second unguarded candidate\n");
        return 1;
    }

    // ---- clean up: delete all three fixture states (cascades every transition
    // ---- above), restoring the machine the scenario borrowed -----------------
    kernel.send(app::events::DeleteStateRequested{.id = kSrc});
    kernel.send(app::events::DeleteStateRequested{.id = kDstA});
    kernel.send(app::events::DeleteStateRequested{.id = kDstB});
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the guarded-group scenario did not restore the machine it borrowed\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario guarded-group (distinct-guard duplicate (from, event) authored, two "
                "pills coexist, second-unguarded-member refused, machine restored)\n");
    return 0;
}

// Scenario "history-deep": the Deep-history checkbox and SetHistoryDeepRequested.
// With P{A,B}+H far outside the fixture: the Deep-history row shows for H and is
// hidden for Normal sibling A, toggling the checkbox puts history:"deep" for H
// in the XState export (machineToXStateJson), and one undo reverts it.
int runHistoryDeepScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr) {
        std::fprintf(stderr, "FAIL: history-deep scenario preconditions (doc) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;

    // ---- build P{A,B}+H, far outside the fixture -------------------------------
    const quint64 kP = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1800.0, -520.0)});
    const quint64 kA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1760.0, -380.0)});
    const quint64 kB = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1960.0, -380.0)});
    const quint64 kH = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1860.0, -300.0)});
    kernel.send(app::events::RenameStateRequested{.id = kP, .name = QStringLiteral("HistDeepP")});
    kernel.send(app::events::RenameStateRequested{.id = kA, .name = QStringLiteral("HistDeepA")});
    kernel.send(app::events::RenameStateRequested{.id = kB, .name = QStringLiteral("HistDeepB")});
    kernel.send(app::events::RenameStateRequested{.id = kH, .name = QStringLiteral("HistDeepH")});
    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = kP});
    kernel.send(app::events::ReparentStateRequested{.id = kB, .parentId = kP});
    // Kind before reparent.
    kernel.send(app::events::SetStateKindRequested{.id = kH, .kind = app::StateKind::History});
    kernel.send(app::events::ReparentStateRequested{.id = kH, .parentId = kP});
    QApplication::processEvents();

    if (doc->findState(kH) == nullptr || doc->findState(kH)->kind != app::StateKind::History ||
        doc->findState(kH)->parentId != kP) {
        std::fprintf(stderr, "FAIL: history-deep scenario's P{A,B}+H fixture did not build\n");
        return 1;
    }

    // ---- Inspector shows the Deep-history row only for a History selection ----
    window.debugSelectState(loginPane, kH);
    QApplication::processEvents();
    if (!window.debugInspector()->debugHistoryDeepRowVisible()) {
        std::fprintf(stderr, "FAIL: the Inspector did not show the Deep-history row for a History-kind state\n");
        return 1;
    }
    if (!saveWidgetCapture(window.debugInspector(), "probe-history-deep")) {
        return 1;
    }

    window.debugSelectState(loginPane, kA);  // A is Normal
    QApplication::processEvents();
    if (window.debugInspector()->debugHistoryDeepRowVisible()) {
        std::fprintf(stderr, "FAIL: the Inspector showed the Deep-history row for a Normal-kind state\n");
        return 1;
    }

    window.debugSelectState(loginPane, kH);
    QApplication::processEvents();

    // ---- toggle through the checkbox lever (the same intent a real click sends) ----
    window.debugInspector()->debugClickHistoryDeepCheckbox();
    QApplication::processEvents();
    if (!doc->findState(kH)->historyDeep) {
        std::fprintf(stderr, "FAIL: toggling the Deep-history checkbox did not set State::historyDeep true\n");
        return 1;
    }

    // ---- XState export now carries history:"deep" -----------------------------
    const app::XStateExportResult exported = app::machineToXStateJson(doc->machine());
    if (!exported.ok) {
        std::fprintf(stderr, "FAIL: exporting the history-deep fixture was refused: %s\n",
                     qUtf8Printable(exported.error));
        return 1;
    }
    const QJsonObject pStates = exported.json.value(QStringLiteral("states"))
                                    .toObject()
                                    .value(QStringLiteral("HistDeepP"))
                                    .toObject()
                                    .value(QStringLiteral("states"))
                                    .toObject();
    if (pStates.value(QStringLiteral("HistDeepH")).toObject().value(QStringLiteral("history")).toString() !=
        QStringLiteral("deep")) {
        std::fprintf(stderr, "FAIL: the XState export did not carry history:\"deep\" for H after the toggle\n");
        return 1;
    }

    // ---- one undo reverts the toggle -------------------------------------------
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findState(kH)->historyDeep) {
        std::fprintf(stderr, "FAIL: ONE undo did not revert the Deep-history toggle\n");
        return 1;
    }

    // ---- clean up: delete P (cascades A/B/H) directly, restoring the machine ---
    kernel.send(app::events::DeleteStateRequested{.id = kP});
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the history-deep scenario did not restore the machine it borrowed\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario history-deep (Deep-history row shown only for History kind, checkbox "
                "toggle -> XState export, one-undo)\n");
    return 0;
}

// Scenario "alignment-guides": alignment snap lines during a real NodeDrag
// beat the 24px grid snap when a centerY candidate is within ~5 scene px. A and
// B are plain leaves of identical height (font metrics and fixed padding only),
// so "B's centerY aligns with A's" equals "B's pos().y() equals A's". A's
// authored y (kAlignAY) sits 12px off the grid, past the 4px grid snap radius
// and the ~5px threshold, so the expectation ignores the platform-variable height.
int runAlignmentGuidesScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || view == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: alignment-guides scenario preconditions (doc/presenter/view/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;

    // ---- build A and B, far outside the fixture --------------------------------
    // kAlignAY = -4008 + 12: -4008 is a 24px grid multiple, so kAlignAY sits
    // 12px clear of it and of the next line up. kAlignDX is far wider than any
    // leaf box, so only the Y axis can engage.
    constexpr qreal kAlignAX = 4800.0;
    constexpr qreal kAlignAY = -3996.0;
    constexpr qreal kAlignDX = 300.0;
    constexpr qreal kAlignBYOffset = -40.0;  // B starts well outside the ~5px Y-alignment threshold
    const quint64 kA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(kAlignAX, kAlignAY)});
    const quint64 kB = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(kAlignAX + kAlignDX, kAlignAY + kAlignBYOffset)});
    QApplication::processEvents();

    app::StateItem* aItem = findStateItemById(scene, kA);
    app::StateItem* bItem = findStateItemById(scene, kB);
    if (aItem == nullptr || bItem == nullptr) {
        std::fprintf(stderr, "FAIL: alignment-guides scenario's A/B StateItems were not both created\n");
        return 1;
    }
    const qreal alignedCenterY = aItem->sceneRect().center().y();  // A's own centerY, whatever its box height is
    // The off-grid guarantee is about A's pos().y() (kAlignAY), not centerY:
    // B has A's height, so the snap target for B's Y reduces to A's pos().y(),
    // while centerY adds a font-derived half height unrelated to the grid.
    if (std::abs(aItem->pos().y() - std::round(aItem->pos().y() / 24.0) * 24.0) < 5.0) {
        std::fprintf(stderr, "FAIL: alignment-guides scenario's engineered aligned-Y landed ON the 24px grid\n");
        return 1;
    }

    // ---- synthetic-drag B slowly toward A's centerY -----------------------------
    const QPointF pressPoint = bItem->sceneRect().center();
    view->debugMousePress(pressPoint);
    // Still 25px off -- outside the ~5px threshold: no premature snap/guide.
    view->debugMouseMove(QPointF(pressPoint.x(), alignedCenterY - 25.0));
    if (bItem->sceneRect().center().y() == alignedCenterY || presenter->debugHorizontalAlignGuideVisible()) {
        std::fprintf(stderr, "FAIL: alignment engaged 25px away from the threshold\n");
        return 1;
    }
    // Now 3px off, inside the threshold: the adjuster must snap B's y onto A's
    // aligned value even though it is off the 24px grid (a); the horizontal
    // guide must show (b), the vertical one must not (no X-axis candidate).
    view->debugMouseMove(QPointF(pressPoint.x(), alignedCenterY - 3.0));
    if (bItem->pos().y() != aItem->pos().y()) {
        std::fprintf(stderr,
                      "FAIL: alignment did not snap B's y onto A's centerY-aligned value (B.y=%.2f, want %.2f)\n",
                      bItem->pos().y(), aItem->pos().y());
        return 1;
    }
    if (!presenter->debugHorizontalAlignGuideVisible()) {
        std::fprintf(stderr, "FAIL: the horizontal alignment guide was not shown while B was snapped to A\n");
        return 1;
    }
    if (presenter->debugVerticalAlignGuideVisible()) {
        std::fprintf(stderr, "FAIL: the vertical alignment guide showed despite no X-axis alignment candidate\n");
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane,
                                 aItem->sceneRect().united(bItem->sceneRect()).adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-alignment-guides")) {
        return 1;
    }

    // ---- release: guides hidden, the committed pos is the aligned one (c) -----
    view->debugMouseRelease(QPointF(pressPoint.x(), alignedCenterY - 3.0));
    QApplication::processEvents();
    if (presenter->debugHorizontalAlignGuideVisible() || presenter->debugVerticalAlignGuideVisible()) {
        std::fprintf(stderr, "FAIL: alignment guides did not hide once the NodeDrag session committed\n");
        return 1;
    }
    if (doc->findState(kB)->pos.y() != doc->findState(kA)->pos.y()) {
        std::fprintf(stderr, "FAIL: the committed position was not the aligned one\n");
        return 1;
    }

    // ---- an Esc-aborted drag also hides the guides (d) -------------------------
    // B already sits on the aligned line after the commit (c). Move comfortably
    // away first (debugMouseMove rounds through integer viewport pixels) before
    // coming back within the threshold, so there is a real engagement to abort,
    // not a same-pixel no-op.
    const QPointF secondPress = bItem->sceneRect().center();
    view->debugMousePress(secondPress);
    view->debugMouseMove(QPointF(secondPress.x(), alignedCenterY - 30.0));
    const QPointF secondTarget(secondPress.x(), alignedCenterY - 3.0);
    view->debugMouseMove(secondTarget);
    if (!presenter->debugHorizontalAlignGuideVisible()) {
        std::fprintf(stderr, "FAIL: alignment-guides scenario's Esc setup never engaged the guide to begin with\n");
        return 1;
    }
    view->escapePressed();  // Q_SIGNALS is public: abort without committing
    QApplication::processEvents();
    if (presenter->debugHorizontalAlignGuideVisible() || presenter->debugVerticalAlignGuideVisible()) {
        std::fprintf(stderr, "FAIL: an Esc-aborted drag left an alignment guide visible\n");
        return 1;
    }
    view->debugMouseRelease(secondTarget);  // closes the synthetic gesture; fsm_ is already Idle, a logged no-op
    QApplication::processEvents();

    // ---- clean up: delete A and B, restoring the machine ------------------------
    kernel.send(app::events::DeleteStateRequested{.id = kA});
    kernel.send(app::events::DeleteStateRequested{.id = kB});
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the alignment-guides scenario did not restore the machine it borrowed\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario alignment-guides (centerY snap beats the grid, guide shown while active, "
                "hidden on commit AND Esc-abort)\n");
    return 0;
}

// Scenario "edge-style-menu": drives View > Edge Style through real
// QAction::trigger() calls, on its own two-state fixture. Every style consumes
// the same waypoints (buildStyledPath only changes path construction), so
// labelAnchor and the arrowhead must stay byte-identical across a style switch
// while the painted sourceHalf/targetHalf visibly differ.
int runEdgeStyleMenuScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    if (doc == nullptr || presenter == nullptr) {
        std::fprintf(stderr, "FAIL: edge-style-menu scenario preconditions (doc/presenter) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);

    if (app::edgeStyle() != app::EdgeStyle::SmallFillet) {
        std::fprintf(stderr, "FAIL: edge-style-menu scenario started with a non-default EdgeStyle\n");
        return 1;
    }

    const QVector<QAction*> actions = window.debugEdgeStyleActions();
    if (actions.size() != 3) {
        std::fprintf(stderr, "FAIL: View > Edge Style menu does not expose exactly 3 actions (got %d)\n",
                     static_cast<int>(actions.size()));
        return 1;
    }
    QAction* smallFilletAction = actions[0];
    QAction* largeFilletAction = actions[1];
    QAction* curvedAction = actions[2];
    if (!smallFilletAction->isCheckable() || !largeFilletAction->isCheckable() || !curvedAction->isCheckable()) {
        std::fprintf(stderr, "FAIL: edge-style-menu scenario's actions are not all checkable\n");
        return 1;
    }
    if (!smallFilletAction->isChecked() || largeFilletAction->isChecked() || curvedAction->isChecked()) {
        std::fprintf(stderr, "FAIL: View > Edge Style menu did not start with Small fillet checked\n");
        return 1;
    }

    // ---- own fixture: two states, one ordinary (Normal-class) transition ----
    // The two states are offset on both axes on purpose: a side-by-side pair
    // routes as one straight run with no corner to round or curve, so every
    // style would paint it identically. A diagonal pair gets perpendicular
    // ports and a one-corner L. Coordinates stay 5+px clear of the 24px grid
    // lines so soft snap cannot re-align them into a straight.
    const auto originalStates = doc->machine().states;
    const auto originalTransitions = doc->machine().transitions;
    const quint64 fromId = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(900.0, 700.0)});
    kernel.send(app::events::RenameStateRequested{.id = fromId, .name = QStringLiteral("EdgeStyleFrom")});
    const quint64 toId = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(1286.0, 850.0)});
    kernel.send(app::events::RenameStateRequested{.id = toId, .name = QStringLiteral("EdgeStyleTo")});
    const quint64 transitionId = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = fromId, .to = toId});
    kernel.send(
        app::events::SetTransitionEventRequested{.id = transitionId, .event = QStringLiteral("EdgeStyleSignal")});
    QApplication::processEvents();

    if (!presenter->debugHasTransitionVisual(transitionId)) {
        std::fprintf(stderr, "FAIL: edge-style-menu scenario's fixture transition has no visual\n");
        return 1;
    }
    // Every reroute goes through MachineSnapshotRequested -> rebuildAll(), which
    // recreates every scene item, so re-fetch the TransitionItem* after each
    // trigger; a held pointer would be a use-after-free.
    const app::TransitionItem* edgeItem = presenter->debugTransitionItem(transitionId);
    if (edgeItem == nullptr) {
        std::fprintf(stderr, "FAIL: edge-style-menu scenario could not read the fixture transition's route\n");
        return 1;
    }
    const app::RoutedEdge before = edgeItem->debugRoute();
    // Fixture drift guard: a corner-free polyline paints identically under
    // every style, so the comparison below would prove nothing.
    if (app::canonicalWaypoints(before).size() < 3) {
        std::fprintf(stderr,
                     "FAIL: edge-style-menu scenario's fixture edge is corner-free (%d waypoints) -- "
                     "no style can differ on it\n",
                     static_cast<int>(app::canonicalWaypoints(before).size()));
        return 1;
    }

    // ---- trigger "Curved" through the REAL action, not a debug lever --------
    curvedAction->trigger();
    QApplication::processEvents();

    if (app::edgeStyle() != app::EdgeStyle::Bezier) {
        std::fprintf(stderr, "FAIL: triggering the Curved action did not switch edgeStyle() to Bezier\n");
        return 1;
    }
    if (!curvedAction->isChecked() || smallFilletAction->isChecked() || largeFilletAction->isChecked()) {
        std::fprintf(stderr, "FAIL: the Edge Style menu's checked state did not follow the Curved trigger\n");
        return 1;
    }

    edgeItem = presenter->debugTransitionItem(transitionId);  // rebuilt: re-fetch
    if (edgeItem == nullptr) {
        std::fprintf(stderr, "FAIL: edge-style-menu scenario's fixture transition lost its visual after Curved\n");
        return 1;
    }
    const app::RoutedEdge afterCurved = edgeItem->debugRoute();
    // The two halves are slices of one polyline cut at the label, so on an
    // L-route the corner sits in exactly one of them and the other is a
    // straight run every builder draws the same way. "The painted path
    // changed" therefore means one half changed, not both.
    if (afterCurved.sourceHalf == before.sourceHalf && afterCurved.targetHalf == before.targetHalf) {
        std::fprintf(stderr, "FAIL: switching to Curved did not change the fixture edge's painted path\n");
        return 1;
    }
    if (app::canonicalWaypoints(afterCurved) != app::canonicalWaypoints(before) ||
        std::hypot(afterCurved.labelAnchor.x() - before.labelAnchor.x(),
                    afterCurved.labelAnchor.y() - before.labelAnchor.y()) > 0.01 ||
        afterCurved.arrowhead != before.arrowhead) {
        std::fprintf(stderr,
                     "FAIL: Curved moved the route's waypoints (labelAnchor/arrowhead) -- the gate invariant "
                     "requires only the path builder to differ\n");
        return 1;
    }

    if (!saveSceneCapture(loginPane, "probe-edge-style-menu")) {
        return 1;
    }

    // ---- trigger "Small fillet" back through the real action, restoring -----
    smallFilletAction->trigger();
    QApplication::processEvents();
    if (app::edgeStyle() != app::EdgeStyle::SmallFillet) {
        std::fprintf(stderr, "FAIL: triggering Small fillet did not restore edgeStyle() to SmallFillet\n");
        return 1;
    }
    if (!smallFilletAction->isChecked() || largeFilletAction->isChecked() || curvedAction->isChecked()) {
        std::fprintf(stderr, "FAIL: the Edge Style menu's checked state did not restore after Small fillet\n");
        return 1;
    }
    edgeItem = presenter->debugTransitionItem(transitionId);  // rebuilt again: re-fetch
    if (edgeItem == nullptr) {
        std::fprintf(stderr,
                     "FAIL: edge-style-menu scenario's fixture transition lost its visual after Small fillet\n");
        return 1;
    }
    const app::RoutedEdge afterRestore = edgeItem->debugRoute();
    if (!(afterRestore.sourceHalf == before.sourceHalf) || !(afterRestore.targetHalf == before.targetHalf)) {
        std::fprintf(stderr, "FAIL: restoring Small fillet did not reproduce the original painted path\n");
        return 1;
    }

    // ---- clean up: delete the fixture, restoring the machine this borrowed --
    kernel.send(app::events::DeleteStateRequested{.id = fromId});  // cascades its one transition
    kernel.send(app::events::DeleteStateRequested{.id = toId});
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: edge-style-menu scenario did not restore the machine it borrowed\n");
        return 1;
    }

    std::printf(
        "PASS: gui-probe scenario edge-style-menu (View menu exposes SmallFillet/LargeFillet/Curved checked "
        "exclusively, real trigger() switches edgeStyle() and reroutes every open session, waypoints stay fixed "
        "while the painted path changes, default restored)\n");
    return 0;
}
// Scenario "minimap": a second QGraphicsView over the scene loginPane's
// CanvasView renders. The fixture adds one near and one far state after the pane
// is bound: bindSession() fits the view once at attach and nothing re-fits on a
// later AddStateRequested, so the far state lands outside the viewport by
// construction (asserted below).
int runMinimapScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::MinimapView* minimap = loginPane->minimap();
    app::CanvasView* mainView = loginPane->canvasView();
    if (doc == nullptr || minimap == nullptr || mainView == nullptr) {
        std::fprintf(stderr, "FAIL: minimap scenario preconditions (doc/minimap/mainView) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);

    if (!minimap->isVisible()) {
        std::fprintf(stderr, "FAIL: minimap is not visible\n");
        return 1;
    }
    if (minimap->scene() != mainView->scene()) {
        std::fprintf(stderr, "FAIL: minimap does not share the pane's canvas scene\n");
        return 1;
    }

    // ---- own fixture: one near state, one far state -----------------------
    const auto originalStates = doc->machine().states;
    const auto originalTransitions = doc->machine().transitions;
    const quint64 nearId = doc->machine().nextId;
    const QPointF nearPos(120.0, 140.0);  // inside login-flow's own already-fitted range
    kernel.send(app::events::AddStateRequested{.pos = nearPos});
    kernel.send(app::events::RenameStateRequested{.id = nearId, .name = QStringLiteral("MinimapNear")});
    const quint64 farId = doc->machine().nextId;
    const QPointF farPos(6000.0, 4000.0);  // far outside canvasView()'s current (unmoved) viewport
    kernel.send(app::events::AddStateRequested{.pos = farPos});
    kernel.send(app::events::RenameStateRequested{.id = farId, .name = QStringLiteral("MinimapFar")});
    QApplication::processEvents();

    if (mainView->viewport()->rect().contains(mainView->mapFromScene(farPos))) {
        std::fprintf(stderr,
                     "FAIL: minimap scenario's far fixture state is still inside the main view's viewport -- this "
                     "scenario does not actually exercise the minimap's reason to exist\n");
        return 1;
    }

    // ---- the minimap's fit must contain BOTH the near and far items -------
    // Let the throttled re-fit run (pumpEventsFor) before reading the
    // minimap's transform.
    pumpEventsFor(150);
    const QRect minimapRect = minimap->viewport()->rect();
    if (!minimapRect.contains(minimap->mapFromScene(nearPos))) {
        std::fprintf(stderr, "FAIL: minimap's fitted transform does not contain the near fixture state\n");
        return 1;
    }
    if (!minimapRect.contains(minimap->mapFromScene(farPos))) {
        std::fprintf(stderr, "FAIL: minimap's fitted transform does not contain the far fixture state -- it is not "
                              "showing the whole scene\n");
        return 1;
    }

    if (!saveWidgetRegionCapture(loginPane, minimap->geometry().adjusted(-8, -8, 8, 8), "probe-minimap")) {
        return 1;
    }

    // ---- simulate a navigate: press the minimap at the far state ----------
    // Synthetic press through MinimapView's own mousePressEvent, the production
    // path a real click takes. EditorView wires onNavigate to
    // canvasView()->centerOn(...); the main view's resulting center proves it.
    minimap->debugMousePress(farPos);
    QApplication::processEvents();

    const QPointF mainCenter = mainView->mapToScene(mainView->viewport()->rect().center());
    // Tolerance absorbs the minimap's viewport-pixel rounding (mapToScene/
    // mapFromScene use integer QPoints): at ~6000 scene units across ~180px one
    // pixel is tens of units. Still 20x tighter than the ~7500-unit distance to
    // the near cluster it must not have centered on.
    constexpr double kNavigateTolerance = 250.0;
    const double distance = std::hypot(mainCenter.x() - farPos.x(), mainCenter.y() - farPos.y());
    if (distance > kNavigateTolerance) {
        std::fprintf(stderr,
                     "FAIL: clicking the minimap at the far state did not center the main view there (distance=%.1f, "
                     "tolerance=%.1f)\n",
                     distance, kNavigateTolerance);
        return 1;
    }

    // ---- clean up: delete the fixture, restoring the machine this borrowed --
    kernel.send(app::events::DeleteStateRequested{.id = nearId});
    kernel.send(app::events::DeleteStateRequested{.id = farId});
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: minimap scenario did not restore the machine it borrowed\n");
        return 1;
    }

    std::printf(
        "PASS: gui-probe scenario minimap (corner navigator shares the pane's scene, fitted transform contains a "
        "far-outside-the-main-view state, click-to-jump centers the main CanvasView there)\n");
    return 0;
}

// Scenario "container-authoring": (a) a selected container shows four wire-drag
// side handles on its derived border, glued through container growth, and a
// wire drag from the border commits a compound-source transition. (b) The
// directional quick-add from a nested state births the new state inside the
// source's parent and the container grows around it. One undo per gesture.
int runContainerAuthoringScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::CanvasView* view = loginPane->canvasView();
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (doc == nullptr || presenter == nullptr || view == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: container-authoring scenario preconditions (doc/presenter/view/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;

    // ---- build P{A} plus a root-level wire target T1, far outside the
    // ---- fixture, 12px off the 24px grid (soft-snap radius) -------------------
    const quint64 kP = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2412.0, 1236.0)});
    const quint64 kA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2364.0, 1332.0)});
    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = kP});
    const quint64 kT1 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2772.0, 1236.0)});
    QApplication::processEvents();

    app::StateItem* pItem = findStateItemById(scene, kP);
    app::StateItem* t1Item = findStateItemById(scene, kT1);
    if (pItem == nullptr || t1Item == nullptr || !pItem->isContainerMode()) {
        std::fprintf(stderr, "FAIL: container-authoring scenario's P{A}+T1 setup did not take\n");
        return 1;
    }

    // ---- (a1) selecting the container shows handles on the DERIVED border ----
    presenter->debugSelectState(kP);
    QApplication::processEvents();
    if (!pItem->debugSideHandlesVisible()) {
        std::fprintf(stderr, "FAIL: a selected container does not show its four side handles\n");
        return 1;
    }
    {
        const QPointF expect = app::sidePortAnchor(pItem->sceneRect(), app::PortSide::Right, 0.0);
        const QPointF got = pItem->debugHandleScenePos(app::PortSide::Right);
        if (std::hypot(got.x() - expect.x(), got.y() - expect.y()) > 0.5) {
            std::fprintf(stderr,
                         "FAIL: the container's Right handle sits at (%.1f, %.1f), not the derived border middle "
                         "(%.1f, %.1f)\n",
                         got.x(), got.y(), expect.x(), expect.y());
            return 1;
        }
    }
    if (!saveSceneRegionCapture(loginPane, pItem->sceneRect().adjusted(-24.0, -24.0, 24.0, 24.0),
                                 "probe-container-authoring")) {
        return 1;
    }

    // ---- (a2) wire drag from the container border -> compound-source
    // ---- transition, ONE undo removes it --------------------------------------
    const quint64 kWireT = doc->machine().nextId;
    presenter->debugWireDragStarted(kP, app::PortSide::Right);
    const QPointF t1Center = t1Item->sceneRect().center();
    presenter->debugWireDragMoved(t1Center);
    presenter->debugWireDragFinished(t1Center);
    QApplication::processEvents();
    {
        const app::Transition* wireT = doc->findTransition(kWireT);
        if (wireT == nullptr || wireT->from != kP || wireT->to != kT1) {
            std::fprintf(stderr, "FAIL: a wire drag from the container's Right border did not commit P -> T1\n");
            return 1;
        }
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findTransition(kWireT) != nullptr) {
        std::fprintf(stderr, "FAIL: ONE undo did not remove the container-source wire transition\n");
        return 1;
    }

    // ---- (b) directional quick-add from nested A: sibling born inside P,
    // ---- container grows, handles stay glued to the GROWN border, ONE undo ----
    const QRectF pRectBefore = pItem->sceneRect();
    const quint64 kSib = doc->machine().nextId;
    presenter->debugAddTransitionInDirection(kA, app::PortSide::Right);
    QApplication::processEvents();
    const app::State* sib = doc->findState(kSib);
    if (sib == nullptr || sib->parentId != kP) {
        std::fprintf(stderr, "FAIL: a quick-add from nested A did not birth its state inside A's parent P\n");
        return 1;
    }
    bool sibTransitionFound = false;
    for (const app::Transition& transition : doc->machine().transitions) {
        sibTransitionFound = sibTransitionFound || (transition.from == kA && transition.to == kSib);
    }
    if (!sibTransitionFound) {
        std::fprintf(stderr, "FAIL: the quick-add's own A -> sibling transition is missing\n");
        return 1;
    }
    app::StateItem* sibItem = findStateItemById(scene, kSib);
    if (sibItem == nullptr || !pItem->sceneRect().contains(sibItem->sceneRect()) ||
        pItem->sceneRect().width() <= pRectBefore.width()) {
        std::fprintf(stderr, "FAIL: P's container box did not grow around the quick-add-born sibling\n");
        return 1;
    }
    // The quick-add selected its fresh transition: re-select P and prove the
    // handles re-derive and track the grown border (the live half of (a1)).
    presenter->debugSelectState(kP);
    QApplication::processEvents();
    {
        const QPointF expect = app::sidePortAnchor(pItem->sceneRect(), app::PortSide::Right, 0.0);
        const QPointF got = pItem->debugHandleScenePos(app::PortSide::Right);
        if (!pItem->debugSideHandlesVisible() ||
            std::hypot(got.x() - expect.x(), got.y() - expect.y()) > 0.5) {
            std::fprintf(stderr, "FAIL: the container's handles did not track its grown border\n");
            return 1;
        }
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findState(kSib) != nullptr) {
        std::fprintf(stderr, "FAIL: ONE undo did not remove the quick-add's whole state+transition batch\n");
        return 1;
    }

    // ---- clean up: delete P (cascades A) and T1, restoring the machine --------
    scene->clearSelection();
    presenter->debugSelectState(kP);
    QApplication::processEvents();
    presenter->debugDeleteSelection();
    kernel.send(app::events::DeleteStateRequested{.id = kT1});
    QApplication::processEvents();
    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the container-authoring scenario did not restore the machine it borrowed\n");
        return 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario container-authoring (derived-border side handles + glued-through-growth, "
                "compound-source wire drag, born-in-parent quick-add, one-undo per gesture)\n");
    return 0;
}

int runCrossHighlightScenario(app::MainWindow& window, app::EditorView* loginPane) {
    if (loginPane == nullptr || loginPane->session() == nullptr || loginPane->presenter() == nullptr) {
        std::fprintf(stderr, "FAIL: cross-highlight scenario received a null pane\n");
        return 1;
    }
    app::DocumentSession* session = loginPane->session();
    ordo::core::Kernel& kernel = session->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr) {
        std::fprintf(stderr, "FAIL: cross-highlight scenario could not reach MachineDocAgent\n");
        return 1;
    }
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::LogicPanel* logic = window.debugLogicPanel();
    if (logic == nullptr) {
        std::fprintf(stderr, "FAIL: cross-highlight scenario could not reach LogicPanel\n");
        return 1;
    }

    // 1. Add context variable "attempts"
    kernel.send(app::events::AddContextVariableRequested{});
    QApplication::processEvents();
    if (doc->machine().context.isEmpty()) {
        std::fprintf(stderr, "FAIL: could not add context variable for cross-highlight scenario\n");
        return 1;
    }
    const quint64 varId = doc->machine().context.last().id;
    kernel.send(app::events::RenameContextVariableRequested{.id = varId, .name = QStringLiteral("attempts")});
    kernel.send(app::events::SetContextInitialValueRequested{.id = varId, .initialValue = QStringLiteral("0")});

    // 2. Set transition 5 guard to "attempts < 3" and transition 7 action to "attempts = attempts + 1"
    kernel.send(app::events::SetTransitionGuardRequested{.id = 5, .guard = QStringLiteral("attempts < 3")});
    kernel.send(app::events::SetTransitionActionRequested{.id = 7, .action = QStringLiteral("attempts = attempts + 1")});
    QApplication::processEvents();

    // 3. Click trace button for row 0 ("attempts")
    logic->debugClickContextTrace(0);
    QApplication::processEvents();

    if (!logic->debugContextTraceActive(0)) {
        std::fprintf(stderr, "FAIL: trace button not checked after click\n");
        return 1;
    }
    if (presenter->activeHighlightedVariable() != QStringLiteral("attempts")) {
        std::fprintf(stderr, "FAIL: presenter activeHighlightedVariable was \"%s\", want \"attempts\"\n",
                     qUtf8Printable(presenter->activeHighlightedVariable()));
        return 1;
    }
    if (!presenter->debugIsTransitionHighlighted(5) || !presenter->debugIsTransitionHighlighted(7)) {
        std::fprintf(stderr, "FAIL: transitions 5 and 7 should be highlighted for 'attempts'\n");
        return 1;
    }
    if (presenter->debugIsTransitionHighlighted(6) || !presenter->debugIsTransitionDimmed(6)) {
        std::fprintf(stderr, "FAIL: transition 6 should NOT be highlighted, and should be dimmed\n");
        return 1;
    }
    if (!presenter->debugIsStateDimmed(1) || !presenter->debugIsStateDimmed(2)) {
        std::fprintf(stderr, "FAIL: states should be dimmed during variable trace\n");
        return 1;
    }

    // Capture visual artifact for verification
    saveSceneCapture(loginPane, "probe-cross-highlight");
    saveWidgetCapture(logic, "probe-cross-highlight-logic");

    // 4. Toggle off by clicking trace button again
    logic->debugClickContextTrace(0);
    QApplication::processEvents();

    if (logic->debugContextTraceActive(0)) {
        std::fprintf(stderr, "FAIL: trace button still checked after toggle click\n");
        return 1;
    }
    if (!presenter->activeHighlightedVariable().isEmpty()) {
        std::fprintf(stderr, "FAIL: presenter activeHighlightedVariable not cleared after toggle\n");
        return 1;
    }
    if (presenter->debugIsTransitionHighlighted(5) || presenter->debugIsTransitionDimmed(6) ||
        presenter->debugIsStateDimmed(1)) {
        std::fprintf(stderr, "FAIL: items not restored after clearing highlight\n");
        return 1;
    }

    // 5. Test dismiss via presenter clear: trace again, then clearVariableHighlight
    logic->debugClickContextTrace(0);
    QApplication::processEvents();
    if (!logic->debugContextTraceActive(0)) {
        std::fprintf(stderr, "FAIL: trace button not reactivated\n");
        return 1;
    }
    presenter->clearVariableHighlight();
    QApplication::processEvents();
    if (logic->debugContextTraceActive(0)) {
        std::fprintf(stderr, "FAIL: trace button not cleared when presenter highlight cleared\n");
        return 1;
    }

    // 6. Clean up: restore original machine state
    kernel.send(app::events::DeleteContextVariableRequested{.id = varId});
    kernel.send(app::events::SetTransitionGuardRequested{.id = 5, .guard = QStringLiteral("canLogin")});
    kernel.send(app::events::SetTransitionActionRequested{.id = 7, .action = QString()});
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario cross-highlight (variable trace, matching edges, dimmed non-matching, toggle & dismiss)\n");
    return 0;
}

int runSimulationBreakpointsScenario(app::MainWindow& window, app::EditorView* loginPane) {
    if (loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: simulation-breakpoints scenario has no login-flow pane\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    app::InspectorPanel* inspector = window.debugInspector();
    if (doc == nullptr || sim == nullptr || presenter == nullptr || inspector == nullptr) {
        std::fprintf(stderr, "FAIL: simulation-breakpoints scenario missing agents or presenter\n");
        return 1;
    }

    // 1. Arm breakpoint on state 2 ("Authenticating")
    constexpr quint64 kBreakpointStateId = 2;
    presenter->debugToggleBreakpoint(kBreakpointStateId);
    QApplication::processEvents();

    if (!presenter->debugHasBreakpoint(kBreakpointStateId)) {
        std::fprintf(stderr, "FAIL: breakpoint not armed on state 2 via debugToggleBreakpoint\n");
        return 1;
    }
    if (!sim->hasBreakpoint(kBreakpointStateId)) {
        std::fprintf(stderr, "FAIL: simulation agent hasBreakpoint(2) is false after toggle\n");
        return 1;
    }

    // Verify context menu has Breakpoint checked
    QMenu* menu = presenter->debugBuildStateContextMenu(kBreakpointStateId);
    if (menu == nullptr) {
        std::fprintf(stderr, "FAIL: debugBuildStateContextMenu(2) returned nullptr\n");
        return 1;
    }
    bool foundCheckedBpAction = false;
    for (QAction* action : menu->actions()) {
        if (action->text() == QStringLiteral("Breakpoint") && action->isChecked()) {
            foundCheckedBpAction = true;
            break;
        }
    }
    delete menu;
    if (!foundCheckedBpAction) {
        std::fprintf(stderr, "FAIL: context menu for state 2 did not have checked 'Breakpoint' action\n");
        return 1;
    }

    // 2. Test Timescale control in Inspector
    inspector->debugSetTimescale(2.0);
    QApplication::processEvents();
    if (std::abs(inspector->debugTimescale() - 2.0) > 0.001) {
        std::fprintf(stderr, "FAIL: inspector timescale was not set to 2.0x\n");
        return 1;
    }
    inspector->debugSetTimescale(1.0);
    QApplication::processEvents();
    if (std::abs(inspector->debugTimescale() - 1.0) > 0.001) {
        std::fprintf(stderr, "FAIL: inspector timescale was not reset to 1.0x\n");
        return 1;
    }

    // 3. Enter Simulate mode and Run
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();

    if (sim->activeStateId() != 1) {
        std::fprintf(stderr, "FAIL: simulation did not start at initial state 1 (LoggedOut)\n");
        return 1;
    }
    if (!sim->running()) {
        std::fprintf(stderr, "FAIL: simulation should be running at initial state 1\n");
        return 1;
    }
    if (presenter->debugIsBreakpointHit(kBreakpointStateId)) {
        std::fprintf(stderr, "FAIL: state 2 breakpoint should not be marked hit yet\n");
        return 1;
    }

    // 4. Fire "Login" transition targeting state 2 (has armed breakpoint)
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Login")});
    QApplication::processEvents();

    if (sim->activeStateId() != kBreakpointStateId) {
        std::fprintf(stderr, "FAIL: simulation did not transition to state 2 on Login event\n");
        return 1;
    }
    if (sim->running()) {
        std::fprintf(stderr, "FAIL: simulation should auto-pause when entering breakpoint state 2\n");
        return 1;
    }
    if (!presenter->debugIsBreakpointHit(kBreakpointStateId)) {
        std::fprintf(stderr, "FAIL: canvas presenter debugIsBreakpointHit(2) should be true\n");
        return 1;
    }

    // Capture visual artifact of breakpoint hit with halo and chip
    saveSceneCapture(loginPane, "probe-sim-breakpoint-hit");
    saveWidgetCapture(inspector, "probe-sim-breakpoint-inspector");

    // 5. Manual event injection while paused: send "Success" to advance to state 3 (LoggedIn)
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Success")});
    QApplication::processEvents();

    if (sim->activeStateId() != 3) {
        std::fprintf(stderr, "FAIL: manual event injection while halted did not advance to state 3\n");
        return 1;
    }

    // 6. Resume / Run execution from state 3
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();

    if (!sim->running()) {
        std::fprintf(stderr, "FAIL: simulation did not resume after RunRequested\n");
        return 1;
    }
    if (presenter->debugIsBreakpointHit(kBreakpointStateId)) {
        std::fprintf(stderr, "FAIL: breakpoint hit flag was not cleared after resume\n");
        return 1;
    }

    // 7. Clean up: toggle off breakpoint, reset mode to Design
    presenter->debugToggleBreakpoint(kBreakpointStateId);
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    kernel.send(app::events::SimulationReset{});
    QApplication::processEvents();

    if (presenter->debugHasBreakpoint(kBreakpointStateId)) {
        std::fprintf(stderr, "FAIL: breakpoint on state 2 was not removed during clean up\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario simulation breakpoints (pin arm, hit auto-pause, halo, event injection, resume)\n");
    return 0;
}

int runReenterAlwaysScenario(app::MainWindow& window, app::EditorView* loginPane) {
    if (loginPane == nullptr || loginPane->session() == nullptr || loginPane->presenter() == nullptr) {
        std::fprintf(stderr, "FAIL: reenter & always scenario: loginPane or session is null\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    auto* presenter = loginPane->presenter();
    auto* inspector = window.debugInspector();
    if (inspector == nullptr) {
        std::fprintf(stderr, "FAIL: reenter & always scenario: debugInspector() is null\n");
        return 1;
    }
    auto& kernel = loginPane->session()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (doc == nullptr || sim == nullptr) {
        std::fprintf(stderr, "FAIL: reenter & always scenario: doc or sim agent is null\n");
        return 1;
    }

    // 1. Select transition 5 (LoggedOut -> Authenticating)
    constexpr quint64 kTargetTransitionId = 5;
    presenter->selectTransition(kTargetTransitionId);
    QApplication::processEvents();

    if (inspector->debugTransitionReenterChecked()) {
        std::fprintf(stderr, "FAIL: transition 5 reenter checkbox should be unchecked by default (reenter == false)\n");
        return 1;
    }
    if (inspector->debugTransitionAlwaysChecked()) {
        std::fprintf(stderr, "FAIL: transition 5 always checkbox should be unchecked by default\n");
        return 1;
    }

    // 2. Toggle Re-enter checkbox: unchecked -> checked (external / full LCCA exit-entry)
    inspector->debugClickTransitionReenterCheckbox();
    QApplication::processEvents();

    if (!inspector->debugTransitionReenterChecked()) {
        std::fprintf(stderr, "FAIL: transition 5 reenter checkbox was not checked after click\n");
        return 1;
    }
    const app::Transition* t5 = doc->findTransition(kTargetTransitionId);
    if (t5 == nullptr || !t5->reenter) {
        std::fprintf(stderr, "FAIL: document transition 5 reenter flag was not set to true\n");
        return 1;
    }

    // Toggle Re-enter checkbox back: checked -> unchecked (reenter == false)
    inspector->debugClickTransitionReenterCheckbox();
    QApplication::processEvents();
    if (inspector->debugTransitionReenterChecked()) {
        std::fprintf(stderr, "FAIL: transition 5 reenter checkbox was not unchecked after second click\n");
        return 1;
    }

    // 3. Toggle Always checkbox: unchecked -> checked
    inspector->debugClickTransitionAlwaysCheckbox();
    QApplication::processEvents();

    if (!inspector->debugTransitionAlwaysChecked()) {
        std::fprintf(stderr, "FAIL: transition 5 always checkbox was not checked after click\n");
        return 1;
    }
    t5 = doc->findTransition(kTargetTransitionId);
    if (t5 == nullptr || !t5->isAlways()) {
        std::fprintf(stderr, "FAIL: document transition 5 isAlways() should be true\n");
        return 1;
    }
    if (!presenter->debugIsTransitionAlways(kTargetTransitionId)) {
        std::fprintf(stderr, "FAIL: presenter debugIsTransitionAlways(5) should be true\n");
        return 1;
    }

    // Capture visual artifacts of always transition on canvas and inspector panel
    saveSceneCapture(loginPane, "probe-reenter-always-canvas");
    saveWidgetCapture(inspector, "probe-reenter-always-inspector");

    // 4. Test simulate mode: initial state 1 (LoggedOut) has an 'always' transition (5) to state 2 (Authenticating)
    // Starting simulation should autonomously microstep from 1 to 2 without an external 'Login' event!
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();

    if (sim->activeStateId() != 2) {
        std::fprintf(stderr, "FAIL: simulation did not autonomously fire always transition from 1 to 2 (activeStateId=%llu)\n",
                     static_cast<unsigned long long>(sim->activeStateId()));
        return 1;
    }

    // 5. Clean up: reset simulation, return to Design mode, restore transition 5
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    kernel.send(app::events::SimulationReset{});
    QApplication::processEvents();

    presenter->selectTransition(kTargetTransitionId);
    QApplication::processEvents();

    if (inspector->debugTransitionAlwaysChecked()) {
        inspector->debugClickTransitionAlwaysCheckbox();
    }
    if (inspector->debugTransitionReenterChecked()) {
        inspector->debugClickTransitionReenterCheckbox();
    }
    kernel.send(app::events::SetTransitionEventRequested{.id = kTargetTransitionId, .event = QStringLiteral("Login")});
    kernel.send(app::events::SetTransitionGuardRequested{.id = kTargetTransitionId, .guard = QStringLiteral("canLogin")});
    QApplication::processEvents();

    t5 = doc->findTransition(kTargetTransitionId);
    if (t5 == nullptr || t5->reenter || t5->isAlways() || t5->event != QStringLiteral("Login") || t5->guard != QStringLiteral("canLogin")) {
        std::fprintf(stderr, "FAIL: transition 5 flags were not restored properly during cleanup\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario reenter & always transitions (inspector toggles, canvas always label, simulate microstep firing)\n");
    return 0;
}

int runRaiseMicrostepScenario(app::MainWindow& window, app::EditorView* loginPane) {
    if (loginPane == nullptr || loginPane->session() == nullptr || loginPane->presenter() == nullptr) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: loginPane or session is null\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    auto* presenter = loginPane->presenter();
    auto* inspector = window.debugInspector();
    if (inspector == nullptr) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: debugInspector() is null\n");
        return 1;
    }
    auto& kernel = loginPane->session()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (doc == nullptr || sim == nullptr) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: doc or sim agent is null\n");
        return 1;
    }

    // 1. Select transition 5 (LoggedOut -> Authenticating) and test inline Action field feedback
    constexpr quint64 kTargetTransitionId = 5;
    presenter->selectTransition(kTargetTransitionId);
    QApplication::processEvents();

    // Malformed raise -> expects error feedback
    inspector->debugTypeAction(QStringLiteral("raise()"));
    QApplication::processEvents();
    if (!inspector->debugActionFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: raise() did not produce error feedback\n");
        return 1;
    }
    if (!inspector->debugActionFeedbackText().contains(QStringLiteral("requires an event name"))) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: error feedback text unexpected: %s\n",
                     inspector->debugActionFeedbackText().toUtf8().constData());
        return 1;
    }

    // Valid raise -> expects note feedback
    inspector->debugTypeAction(QStringLiteral("raise(Step1)"));
    QApplication::processEvents();
    if (inspector->debugActionFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: valid raise(Step1) flagged as error\n");
        return 1;
    }
    if (!inspector->debugActionFeedbackText().contains(QStringLiteral("Step1"))) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: note feedback text missing Step1: %s\n",
                     inspector->debugActionFeedbackText().toUtf8().constData());
        return 1;
    }

    // Revert transition action
    inspector->debugTypeAction(QString());
    QApplication::processEvents();
    if (!inspector->debugActionFeedbackText().isEmpty()) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: empty action should have cleared feedback\n");
        return 1;
    }

    // 2. Set entry action on State 2 (Authenticating) to raise(Success)
    // Transition 6 is Authenticating -> LoggedIn on event "Success".
    // When Authenticating is entered upon "Login", raise(Success) should enqueue "Success",
    // and the microstep loop should immediately fire Transition 6 to reach LoggedIn (3).
    constexpr quint64 kAuthenticatingStateId = 2;
    presenter->selectState(kAuthenticatingStateId);
    QApplication::processEvents();

    kernel.send(app::events::SetEntryActionsRequested{
        .id = kAuthenticatingStateId,
        .entryActions = QStringList{QStringLiteral("raise(Success)")}
    });
    QApplication::processEvents();

    // Visual capture of canvas and inspector with raise action
    saveSceneCapture(loginPane, "probe-raise-microstep-canvas");
    saveWidgetCapture(inspector, "probe-raise-microstep-inspector");

    // 3. Enter Simulate mode and run
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();

    if (sim->activeStateId() != 1) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: initial state should be LoggedOut (1), got %llu\n",
                     static_cast<unsigned long long>(sim->activeStateId()));
        return 1;
    }

    // Send external event "Login"
    sim->sendEvent(QStringLiteral("Login"));
    QApplication::processEvents();

    // The microstep cascade should autonomously reach state 3 (LoggedIn)
    if (sim->activeStateId() != 3) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: did not reach LoggedIn (3) via microstep cascade, got %llu\n",
                     static_cast<unsigned long long>(sim->activeStateId()));
        return 1;
    }

    // Verify trace contents
    bool foundRaiseTrace = false;
    bool foundSuccessTransitionTrace = false;
    for (const QString& line : sim->trace()) {
        if (line.contains(QStringLiteral("raise: Success"))) {
            foundRaiseTrace = true;
        }
        if (line.contains(QStringLiteral("Success → LoggedIn"))) {
            foundSuccessTransitionTrace = true;
        }
    }
    if (!foundRaiseTrace) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: trace missing 'raise: Success'\n");
        return 1;
    }
    if (!foundSuccessTransitionTrace) {
        std::fprintf(stderr, "FAIL: raise microstep scenario: trace missing 'Success → LoggedIn'\n");
        return 1;
    }

    // 4. Clean up: reset simulation, return to Design mode, restore original entry action
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    kernel.send(app::events::SimulationReset{});
    QApplication::processEvents();

    kernel.send(app::events::SetEntryActionsRequested{
        .id = kAuthenticatingStateId,
        .entryActions = QStringList{QStringLiteral("beginLogin()")}
    });
    presenter->selectState(1);
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario raise microstep (inspector action feedback, canvas raise highlight, RTC cascade to destination)\n");
    return 0;
}

int runActorCommunicationScenario(app::MainWindow& window, app::EditorView* loginPane) {
    if (loginPane == nullptr || loginPane->session() == nullptr || loginPane->presenter() == nullptr) {
        std::fprintf(stderr, "FAIL: actor communication scenario: loginPane or session is null\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    auto* presenter = loginPane->presenter();
    auto* inspector = window.debugInspector();
    if (inspector == nullptr) {
        std::fprintf(stderr, "FAIL: actor communication scenario: debugInspector() is null\n");
        return 1;
    }
    auto& kernel = loginPane->session()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (doc == nullptr || sim == nullptr) {
        std::fprintf(stderr, "FAIL: actor communication scenario: doc or sim agent is null\n");
        return 1;
    }

    // 1. Select transition 5 (LoggedOut -> Authenticating) and test inline Action field feedback
    constexpr quint64 kTargetTransitionId = 5;
    presenter->selectTransition(kTargetTransitionId);
    QApplication::processEvents();

    // Malformed sendTo -> expects error feedback
    inspector->debugTypeAction(QStringLiteral("sendTo(authWorker)"));
    QApplication::processEvents();
    if (!inspector->debugActionFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: actor communication scenario: sendTo(authWorker) did not produce error feedback\n");
        return 1;
    }
    if (!inspector->debugActionFeedbackText().contains(QStringLiteral("requires two arguments"))) {
        std::fprintf(stderr, "FAIL: actor communication scenario: error feedback text unexpected: %s\n",
                     inspector->debugActionFeedbackText().toUtf8().constData());
        return 1;
    }

    // Valid sendTo -> expects note feedback
    inspector->debugTypeAction(QStringLiteral("sendTo(authWorker, AUTH_REQ)"));
    QApplication::processEvents();
    if (inspector->debugActionFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: actor communication scenario: valid sendTo flagged as error\n");
        return 1;
    }
    if (!inspector->debugActionFeedbackText().contains(QStringLiteral("authWorker")) ||
        !inspector->debugActionFeedbackText().contains(QStringLiteral("AUTH_REQ"))) {
        std::fprintf(stderr, "FAIL: actor communication scenario: note feedback text missing actor/event: %s\n",
                     inspector->debugActionFeedbackText().toUtf8().constData());
        return 1;
    }

    // Malformed sendParent -> expects error feedback
    inspector->debugTypeAction(QStringLiteral("sendParent()"));
    QApplication::processEvents();
    if (!inspector->debugActionFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: actor communication scenario: sendParent() did not produce error feedback\n");
        return 1;
    }

    // Valid sendParent -> expects note feedback
    inspector->debugTypeAction(QStringLiteral("sendParent(LOGIN_ATTEMPT)"));
    QApplication::processEvents();
    if (inspector->debugActionFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: actor communication scenario: valid sendParent flagged as error\n");
        return 1;
    }
    if (!inspector->debugActionFeedbackText().contains(QStringLiteral("LOGIN_ATTEMPT"))) {
        std::fprintf(stderr, "FAIL: actor communication scenario: note feedback text missing event: %s\n",
                     inspector->debugActionFeedbackText().toUtf8().constData());
        return 1;
    }

    // Revert transition action
    inspector->debugTypeAction(QString());
    QApplication::processEvents();

    // 2. Set transition 5 action to sendTo(authWorker, AUTH_REQ)
    kernel.send(app::events::SetTransitionActionRequested{
        .id = kTargetTransitionId,
        .action = QStringLiteral("sendTo(authWorker, AUTH_REQ)")
    });
    // Set entry action on State 2 (Authenticating) to sendParent(STARTED_AUTH)
    constexpr quint64 kAuthenticatingStateId = 2;
    kernel.send(app::events::SetEntryActionsRequested{
        .id = kAuthenticatingStateId,
        .entryActions = QStringList{QStringLiteral("sendParent(STARTED_AUTH)")}
    });
    presenter->selectTransition(kTargetTransitionId);
    QApplication::processEvents();

    // Visual capture of canvas and inspector with sendTo/sendParent action
    saveSceneCapture(loginPane, "probe-actor-comm-canvas");
    saveWidgetCapture(inspector, "probe-actor-comm-inspector");

    // 3. Enter Simulate mode and run
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();

    if (sim->activeStateId() != 1) {
        std::fprintf(stderr, "FAIL: actor communication scenario: initial state should be LoggedOut (1), got %llu\n",
                     static_cast<unsigned long long>(sim->activeStateId()));
        return 1;
    }

    // Send external event "Login"
    sim->sendEvent(QStringLiteral("Login"));
    QApplication::processEvents();

    if (sim->activeStateId() != 2) {
        std::fprintf(stderr, "FAIL: actor communication scenario: did not transition to Authenticating (2), got %llu\n",
                     static_cast<unsigned long long>(sim->activeStateId()));
        return 1;
    }

    // Verify trace contents
    bool foundSendToTrace = false;
    bool foundSendParentTrace = false;
    for (const QString& line : sim->trace()) {
        if (line.contains(QStringLiteral("sendTo: authWorker <- AUTH_REQ"))) {
            foundSendToTrace = true;
        }
        if (line.contains(QStringLiteral("sendParent: STARTED_AUTH"))) {
            foundSendParentTrace = true;
        }
    }
    if (!foundSendToTrace) {
        std::fprintf(stderr, "FAIL: actor communication scenario: trace missing 'sendTo: authWorker <- AUTH_REQ'\n");
        return 1;
    }
    if (!foundSendParentTrace) {
        std::fprintf(stderr, "FAIL: actor communication scenario: trace missing 'sendParent: STARTED_AUTH'\n");
        return 1;
    }

    // 4. Clean up: reset simulation, return to Design mode, restore original actions
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    kernel.send(app::events::SimulationReset{});
    QApplication::processEvents();

    kernel.send(app::events::SetTransitionActionRequested{
        .id = kTargetTransitionId,
        .action = QString()
    });
    kernel.send(app::events::SetEntryActionsRequested{
        .id = kAuthenticatingStateId,
        .entryActions = QStringList{QStringLiteral("beginLogin()")}
    });
    presenter->selectState(1);
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario actor communication (inspector feedback, canvas indigo syntax tinting, dispatch trace logging)\n");
    return 0;
}

int runWildcardEventsScenario(app::MainWindow& window, app::EditorView* loginPane) {
    if (loginPane == nullptr || loginPane->session() == nullptr || loginPane->presenter() == nullptr) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: loginPane or session is null\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    auto* presenter = loginPane->presenter();
    auto* inspector = window.debugInspector();
    if (inspector == nullptr) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: debugInspector() is null\n");
        return 1;
    }
    auto& kernel = loginPane->session()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (doc == nullptr || sim == nullptr) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: doc or sim agent is null\n");
        return 1;
    }

    // 1. Select transition 5 (LoggedOut -> Authenticating) and test inline Event field feedback
    constexpr quint64 kTargetTransitionId = 5;
    presenter->selectTransition(kTargetTransitionId);
    QApplication::processEvents();

    // Malformed wildcard with middle asterisk -> expects error feedback
    inspector->debugTypeEvent(QStringLiteral("auth*token"));
    QApplication::processEvents();
    if (!inspector->debugEventFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: 'auth*token' did not produce error feedback\n");
        return 1;
    }
    if (!inspector->debugEventFeedbackText().contains(QStringLiteral("Invalid wildcard"))) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: error feedback text unexpected: %s\n",
                     inspector->debugEventFeedbackText().toUtf8().constData());
        return 1;
    }

    // Malformed wildcard with multiple asterisks -> expects error feedback
    inspector->debugTypeEvent(QStringLiteral("auth.*.*"));
    QApplication::processEvents();
    if (!inspector->debugEventFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: 'auth.*.*' did not produce error feedback\n");
        return 1;
    }

    // Valid prefix wildcard -> expects note feedback
    inspector->debugTypeEvent(QStringLiteral("auth.*"));
    QApplication::processEvents();
    if (inspector->debugEventFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: valid 'auth.*' flagged as error\n");
        return 1;
    }
    if (!inspector->debugEventFeedbackText().contains(QStringLiteral("Prefix wildcard")) ||
        !inspector->debugEventFeedbackText().contains(QStringLiteral("auth"))) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: note feedback text missing prefix: %s\n",
                     inspector->debugEventFeedbackText().toUtf8().constData());
        return 1;
    }

    // Valid universal wildcard -> expects note feedback
    inspector->debugTypeEvent(QStringLiteral("*"));
    QApplication::processEvents();
    if (inspector->debugEventFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: valid '*' flagged as error\n");
        return 1;
    }
    if (!inspector->debugEventFeedbackText().contains(QStringLiteral("Universal wildcard"))) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: note feedback text missing universal: %s\n",
                     inspector->debugEventFeedbackText().toUtf8().constData());
        return 1;
    }

    // 2. Set transition 5 event to 'auth.*' and capture visuals
    kernel.send(app::events::SetTransitionEventRequested{
        .id = kTargetTransitionId,
        .event = QStringLiteral("auth.*")
    });
    presenter->selectTransition(kTargetTransitionId);
    QApplication::processEvents();

    saveSceneCapture(loginPane, "probe-wildcard-canvas");
    saveWidgetCapture(inspector, "probe-wildcard-inspector");

    // 3. Enter Simulate mode and run
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();

    if (sim->activeStateId() != 1) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: initial state should be LoggedOut (1), got %llu\n",
                     static_cast<unsigned long long>(sim->activeStateId()));
        return 1;
    }

    // Send matching subevent "auth.token" -> should fire transition 5 into Authenticating (2)
    sim->sendEvent(QStringLiteral("auth.token"));
    QApplication::processEvents();

    if (sim->activeStateId() != 2) {
        std::fprintf(stderr, "FAIL: wildcard events scenario: did not transition to Authenticating (2), got %llu\n",
                     static_cast<unsigned long long>(sim->activeStateId()));
        return 1;
    }

    // 4. Clean up: reset simulation, return to Design mode, restore original event "Login"
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    kernel.send(app::events::SimulationReset{});
    QApplication::processEvents();

    kernel.send(app::events::SetTransitionEventRequested{
        .id = kTargetTransitionId,
        .event = QStringLiteral("Login")
    });
    presenter->selectState(1);
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario wildcard-events (inspector inline hint, canvas orange styling, prefix/universal matching sim)\n");
    return 0;
}

int runMultipleTargetsScenario(app::MainWindow& window, app::EditorView* loginPane) {
    if (loginPane == nullptr || loginPane->session() == nullptr || loginPane->presenter() == nullptr) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: loginPane or session is null\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    auto* presenter = loginPane->presenter();
    auto* inspector = window.debugInspector();
    QGraphicsScene* scene = loginPane->canvasView() != nullptr ? loginPane->canvasView()->scene() : nullptr;
    if (inspector == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: inspector or scene is null\n");
        return 1;
    }
    auto& kernel = loginPane->session()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (doc == nullptr || sim == nullptr) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: doc or sim agent is null\n");
        return 1;
    }

    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;
    const quint64 originalInitial = doc->machine().initialStateId;

    // 1. Build a test fixture far outside the normal canvas:
    // Root state kRoot
    // Parallel container kPar with two orthogonal regions:
    //   - Region 1: kR1, containing leaf states kR1A, kR1B
    //   - Region 2: kR2, containing leaf states kR2A, kR2B
    const quint64 kRoot = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3200.0, -900.0)});
    kernel.send(app::events::RenameStateRequested{.id = kRoot, .name = QStringLiteral("Setup")});

    const quint64 kPar = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3500.0, -900.0)});
    kernel.send(app::events::RenameStateRequested{.id = kPar, .name = QStringLiteral("ParApp")});

    const quint64 kR1 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3440.0, -760.0)});
    kernel.send(app::events::RenameStateRequested{.id = kR1, .name = QStringLiteral("Audio")});

    const quint64 kR1A = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3400.0, -620.0)});
    kernel.send(app::events::RenameStateRequested{.id = kR1A, .name = QStringLiteral("Muted")});

    const quint64 kR1B = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3480.0, -620.0)});
    kernel.send(app::events::RenameStateRequested{.id = kR1B, .name = QStringLiteral("Playing")});

    const quint64 kR2 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3660.0, -760.0)});
    kernel.send(app::events::RenameStateRequested{.id = kR2, .name = QStringLiteral("Video")});

    const quint64 kR2A = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3620.0, -620.0)});
    kernel.send(app::events::RenameStateRequested{.id = kR2A, .name = QStringLiteral("Paused")});

    const quint64 kR2B = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(3700.0, -620.0)});
    kernel.send(app::events::RenameStateRequested{.id = kR2B, .name = QStringLiteral("Rendering")});

    // Reparent regions into ParApp, and leaves into regions
    kernel.send(app::events::ReparentStateRequested{.id = kR1, .parentId = kPar});
    kernel.send(app::events::ReparentStateRequested{.id = kR1A, .parentId = kR1});
    kernel.send(app::events::ReparentStateRequested{.id = kR1B, .parentId = kR1});
    kernel.send(app::events::ReparentStateRequested{.id = kR2, .parentId = kPar});
    kernel.send(app::events::ReparentStateRequested{.id = kR2A, .parentId = kR2});
    kernel.send(app::events::ReparentStateRequested{.id = kR2B, .parentId = kR2});

    // Set ParApp kind to Parallel
    kernel.send(app::events::SetStateKindRequested{.id = kPar, .kind = app::StateKind::Parallel});
    QApplication::processEvents();

    // 2. Add transition from Setup to Audio.Muted
    const quint64 kTransId = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kRoot, .to = kR1A});
    kernel.send(app::events::SetTransitionEventRequested{.id = kTransId, .event = QStringLiteral("START")});
    QApplication::processEvents();

    presenter->selectTransition(kTransId);
    QApplication::processEvents();

    // 3. Test Inspector Target Field Live Feedback
    // A) Unknown target state -> Error
    inspector->debugTypeTarget(QStringLiteral("NonExistentState"));
    QApplication::processEvents();
    if (!inspector->debugTargetFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: unknown state should show error feedback\n");
        return 1;
    }

    // B) Duplicate target state -> Error
    inspector->debugTypeTarget(QStringLiteral("Muted, Muted"));
    QApplication::processEvents();
    if (!inspector->debugTargetFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: duplicate target should show error feedback\n");
        return 1;
    }

    // C) Non-orthogonal sibling targets (both in Audio region) -> Error
    inspector->debugTypeTarget(QStringLiteral("Muted, Playing"));
    QApplication::processEvents();
    if (!inspector->debugTargetFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: non-orthogonal siblings should show error feedback\n");
        return 1;
    }

    // D) Orthogonal targets across Audio and Video regions -> Note (Valid)
    inspector->debugTypeTarget(QStringLiteral("Playing, Rendering"));
    QApplication::processEvents();
    if (inspector->debugTargetFeedbackIsError()) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: valid orthogonal targets flagged as error\n");
        return 1;
    }
    if (!inspector->debugTargetFeedbackText().contains(QStringLiteral("Multiple targets"))) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: feedback text missing 'Multiple targets': %s\n",
                     inspector->debugTargetFeedbackText().toUtf8().constData());
        return 1;
    }

    // 4. Commit multi-target edit
    inspector->debugCommitTarget(QStringLiteral("Playing, Rendering"));
    QApplication::processEvents();

    const app::Transition* transAfterCommit = doc->findTransition(kTransId);
    if (!transAfterCommit || !transAfterCommit->isMultiTarget()) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: transition is not multi-target after commit\n");
        return 1;
    }
    const QList<quint64> expectedTargets = {kR1B, kR2B};
    if (transAfterCommit->effectiveTargets() != expectedTargets) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: effectiveTargets mismatch\n");
        return 1;
    }

    // Capture visual artifacts
    saveSceneCapture(loginPane, "probe-multiple-targets-canvas");
    saveWidgetCapture(inspector, "probe-multiple-targets-inspector");

    // 5. Test Simulation Execution with Multi-Target Transition
    kernel.send(app::events::SetInitialStateRequested{.id = kRoot});
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();

    if (sim->activeStateId() != kRoot) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: initial active state should be Setup (%llu), got %llu\n",
                     static_cast<unsigned long long>(kRoot), static_cast<unsigned long long>(sim->activeStateId()));
        return 1;
    }

    // Fire START event -> should enter both Playing (kR1B) and Rendering (kR2B) simultaneously
    sim->sendEvent(QStringLiteral("START"));
    QApplication::processEvents();

    const auto activeConfig = sim->configuration();
    const bool hasR1B = std::find(activeConfig.begin(), activeConfig.end(), kR1B) != activeConfig.end();
    const bool hasR2B = std::find(activeConfig.begin(), activeConfig.end(), kR2B) != activeConfig.end();
    if (!hasR1B || !hasR2B) {
        std::fprintf(stderr, "FAIL: multiple targets scenario: simultaneous entry failed (hasR1B=%d, hasR2B=%d)\n",
                     hasR1B ? 1 : 0, hasR2B ? 1 : 0);
        return 1;
    }

    // 6. Cleanup: return to Design mode, reset simulation, delete created states, restore original machine
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    kernel.send(app::events::SimulationReset{});
    QApplication::processEvents();

    kernel.send(app::events::SetInitialStateRequested{.id = originalInitial});
    scene->clearSelection();
    if (auto* rootItem = findStateItemById(scene, kRoot)) {
        rootItem->setSelected(true);
    }
    if (auto* parItem = findStateItemById(scene, kPar)) {
        parItem->setSelected(true);
    }
    QApplication::processEvents();
    presenter->debugDeleteSelection();
    QApplication::processEvents();

    if (doc->machine().states != originalStates || doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: multiple targets scenario did not restore the original machine\n");
        return 1;
    }
    scene->clearSelection();
    presenter->selectState(1);
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario multiple-targets (inspector target feedback, canvas multi-target routing, concurrent orthogonal sim entry)\n");
    return 0;
}


