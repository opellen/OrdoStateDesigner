// --gui-probe shell scenarios: initial-state marker, kind warnings, targetless
// loops, state metadata, context-menu verbs, the code tab preview, pill
// reconnect, the Logic and Inspector panels, and project-level flows.

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>  // multi-invoke card Output-type combo (wheel-event guard)
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>  // type-ramp base font
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHash>
#include <QIcon>  // app-icon scenario
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>  // Edit > Auto Layout...'s shortcut
#include <QLabel>  // project-settings round trip's disabled-card guidance text
#include <QLineEdit>  // the Logic panel's in-place rename editor
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>  // Open Project's failure modal
#include <QPainter>
#include <QPair>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointF>
#include <QPointer>  // liveness check for the wheel-event guard
#include <QPushButton>  // AutoLayoutDialog's Apply button
#include <QStatusBar>
#include <QRect>
#include <QRegularExpression>
#include <QScrollArea>  // Inspector Machine-tab viewport
#include <QScrollBar>   // canvas scroll position; EVENTS/GUARD RESULTS "NAME (N)" header format check
#include <QElapsedTimer>
#include <QSet>
#include <QDoubleSpinBox>  // Int/Double payload spin boxes (arrow/PgUp-PgDn probe)
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QToolButton>  // project-settings round trip's Reset button
#include <QTreeWidget>  // Machines panel separators
#include <QTransform>
#include <QWheelEvent>

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

#include "infra/mcp_runtime_server.h"
#include "controller/edit_commands.h"
#include "controller/sim_commands.h"
#include "controller/undo_capture.h"
#include "controller/undo_commands.h"
#include "infra/code_generator.h"
#include "infra/io_dispatcher.h"
#include "infra/logic_inventory.h"
#include "infra/machine_validator.h"
#include "infra/project_io.h"
#include "infra/recent_projects.h"
#include "infra/settings_store.h"
#include "infra/sim_clock.h"
#include "infra/scxml_io.h"
#include "infra/xstate_v5_io.h"
#include "view/shell/progress_overlay_widget.h"
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
#include "view/shell/export_dialog.h"
#include "view/shell/expression_editor_dialog.h"
#include "view/shell/activity_rail.h"
#include "view/shell/auto_layout_dialog.h"
#include "view/shell/inspector_panel.h"
#include "view/shell/node_code_preview_widget.h"
#include "view/shell/logic_panel.h"
#include "view/shell/machines_panel.h"
#include "view/shell/main_window.h"
#include "view/shell/settings_view.h"
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

// Single-TU helper (only runInitialStateScenario/runKindWarningsScenario use it).
static bool problemsContain(const app::MainWindow& window, const QString& severity, const QString& fragment) {
    for (const QString& row : window.debugProblemsTexts()) {
        if (row.startsWith(severity + QStringLiteral("|")) && row.contains(fragment)) {
            return true;
        }
    }
    return false;
}

// Scenario "initial-state": unchecking the
// Inspector's Initial checkbox on the login flow's LoggedOut must clear
// Machine::initialStateId, remove the canvas marker, and surface the live
// "no initial state" Error; re-checking it on Authenticating must move the
// marker there and clean the Problems list; then restore LoggedOut.
int runInitialStateScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto doc = window.loginFlowSession()->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    if (doc == nullptr || presenter == nullptr) {
        std::fprintf(stderr, "FAIL: initial-state scenario preconditions (doc/presenter) missing\n");
        return 1;
    }

    window.debugFocusView(loginPane);
    window.debugSelectState(loginPane, 1);  // LoggedOut, the boot-time initial state
    QApplication::processEvents();
    if (!window.debugProblemsTexts().isEmpty()) {
        std::fprintf(stderr, "FAIL: login-flow Problems list is not clean before the scenario\n");
        return 1;
    }
    if (presenter->debugInitialMarkerStateId() != 1) {
        std::fprintf(stderr, "FAIL: initial marker is not anchored to LoggedOut (id 1) at scenario start\n");
        return 1;
    }

    window.debugInspector()->debugClickInitialCheckbox();  // checked -> unchecked
    QApplication::processEvents();
    if (doc->machine().initialStateId != 0) {
        std::fprintf(stderr, "FAIL: unchecking the Initial checkbox did not clear initialStateId\n");
        return 1;
    }
    if (presenter->debugInitialMarkerStateId() != 0) {
        std::fprintf(stderr, "FAIL: the initial marker is still drawn with no initial state set\n");
        return 1;
    }
    if (!problemsContain(window, QStringLiteral("Error"), QStringLiteral("no initial state"))) {
        std::fprintf(stderr, "FAIL: live Problems list did not gain the 'no initial state' Error\n");
        return 1;
    }
    if (!saveWidgetCapture(window.debugProblemsWidget(), "probe-initial-missing")) {
        return 1;  // the artifact is the row just asserted
    }

    window.debugSelectState(loginPane, kAuthenticatingStateId);
    QApplication::processEvents();
    window.debugInspector()->debugClickInitialCheckbox();  // unchecked -> checked on Authenticating
    QApplication::processEvents();
    if (doc->machine().initialStateId != kAuthenticatingStateId ||
        presenter->debugInitialMarkerStateId() != kAuthenticatingStateId) {
        std::fprintf(stderr, "FAIL: checking Initial on Authenticating did not move initialStateId/marker to id 2\n");
        return 1;
    }
    if (!window.debugProblemsTexts().isEmpty()) {
        std::fprintf(stderr, "FAIL: Problems list did not come back clean after re-setting an initial state\n");
        return 1;
    }
    if (!saveSceneCapture(loginPane, "probe-initial-moved")) {
        return 1;  // the marker at Authenticating is scene content
    }

    window.debugSelectState(loginPane, 1);
    QApplication::processEvents();
    window.debugInspector()->debugClickInitialCheckbox();  // restore LoggedOut as initial
    QApplication::processEvents();
    if (doc->machine().initialStateId != 1 || presenter->debugInitialMarkerStateId() != 1) {
        std::fprintf(stderr, "FAIL: restoring LoggedOut as initial did not land (initialStateId/marker != 1)\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario initial-state (uncheck -> Error+marker gone, move, restore)\n");
    return 0;
}

// Scenario "kind-warnings": Parallel/History are selectable, persisted and
// execute for real. Parallel on a childless state is valid, with nothing to
// warn about. History picks up a Warning (check 8): Authenticating already has
// two outgoing transitions (Success/Failure), and a History pseudo-state's own
// outgoing transitions can never fire (model/sim_agent.h). Returning to Normal
// must clear it.
int runKindWarningsScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto doc = window.loginFlowSession()->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr) {
        std::fprintf(stderr, "FAIL: kind-warnings scenario preconditions (doc) missing\n");
        return 1;
    }

    window.debugFocusView(loginPane);
    window.debugSelectState(loginPane, kAuthenticatingStateId);
    QApplication::processEvents();

    window.debugInspector()->debugSetKind(app::StateKind::Parallel);
    QApplication::processEvents();
    if (doc->findState(kAuthenticatingStateId)->kind != app::StateKind::Parallel) {
        std::fprintf(stderr, "FAIL: Inspector kind pick did not set Authenticating to Parallel\n");
        return 1;
    }
    if (!window.debugProblemsTexts().isEmpty()) {
        std::fprintf(stderr, "FAIL: a childless Parallel state surfaced an unexpected Problems row\n");
        return 1;
    }

    window.debugInspector()->debugSetKind(app::StateKind::History);
    QApplication::processEvents();
    if (!problemsContain(window, QStringLiteral("Warning"), QStringLiteral("History"))) {
        std::fprintf(stderr, "FAIL: live Problems list did not gain the History-outgoing-transition Warning\n");
        return 1;
    }
    if (!saveWidgetCapture(window.debugProblemsWidget(), "probe-kind-warning")) {
        return 1;
    }

    window.debugInspector()->debugSetKind(app::StateKind::Normal);
    QApplication::processEvents();
    if (doc->findState(kAuthenticatingStateId)->kind != app::StateKind::Normal ||
        !window.debugProblemsTexts().isEmpty()) {
        std::fprintf(stderr, "FAIL: returning Authenticating to Normal did not clear the kind Warning\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario kind-warnings (Parallel on a childless state stays clean, History's own "
                "outgoing transition still warns, Normal clears)\n");
    return 0;
}

// Scenario "loops-targetless": create a self-transition on
// LoggedOut and a targetless transition on Authenticating through the same
// intents the context menus send, and assert the canvas grew a
// routed visual for each (self-loop arc, dangling stub) with the Problems
// list still clean; undo x4 restores the boot topology.
int runLoopsTargetlessScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    if (doc == nullptr || presenter == nullptr) {
        std::fprintf(stderr, "FAIL: loops-targetless scenario preconditions (doc/presenter) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    const int transitionsBefore = static_cast<int>(doc->machine().transitions.size());

    kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 1});  // self on LoggedOut
    kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 0});  // targetless on Authenticating
    QApplication::processEvents();
    if (static_cast<int>(doc->machine().transitions.size()) != transitionsBefore + 2) {
        std::fprintf(stderr, "FAIL: self/targetless AddTransition did not append 2 transitions\n");
        return 1;
    }
    const quint64 selfId = doc->machine().transitions[transitionsBefore].id;
    const quint64 targetlessId = doc->machine().transitions[transitionsBefore + 1].id;
    kernel.send(app::events::SetTransitionEventRequested{.id = selfId, .event = QStringLiteral("Ping")});
    kernel.send(app::events::SetTransitionEventRequested{.id = targetlessId, .event = QStringLiteral("Audit")});
    QApplication::processEvents();

    if (!presenter->debugHasTransitionVisual(selfId) || !presenter->debugHasTransitionVisual(targetlessId)) {
        std::fprintf(stderr, "FAIL: the canvas did not grow visuals for the self/targetless transitions\n");
        return 1;
    }
    if (!window.debugProblemsTexts().isEmpty()) {
        std::fprintf(stderr, "FAIL: self/targetless transitions dirtied the Problems list\n");
        return 1;
    }
    if (!saveSceneCapture(loginPane, "probe-loops-targetless")) {
        return 1;
    }

    for (int i = 0; i < 4; ++i) {  // 2 adds + 2 event sets, one undo step each
        kernel.send(app::events::UndoRequested{});
    }
    QApplication::processEvents();
    if (static_cast<int>(doc->machine().transitions.size()) != transitionsBefore ||
        presenter->debugHasTransitionVisual(selfId) || presenter->debugHasTransitionVisual(targetlessId)) {
        std::fprintf(stderr, "FAIL: undo x4 did not restore the boot topology/visuals\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario loops-targetless (self-loop + stub render, problems clean, undo)\n");
    return 0;
}

// Scenario "state-metadata": commit Description/Tags/Exit actions through the
// Inspector's State-tab fields (debugCommit* levers stand in for the
// FocusOut/editingFinished moment) and assert the document took each one;
// undo x3 clears them back.
int runStateMetadataScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto doc = window.loginFlowSession()->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr) {
        std::fprintf(stderr, "FAIL: state-metadata scenario preconditions (doc) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    window.debugSelectState(loginPane, kAuthenticatingStateId);
    QApplication::processEvents();

    window.debugInspector()->debugCommitDescription(QStringLiteral("Verifies the submitted credentials"));
    window.debugInspector()->debugCommitTags(QStringLiteral("auth, async"));
    window.debugInspector()->debugCommitExitActions(QStringLiteral("stopSpinner()"));
    QApplication::processEvents();

    const app::State* state = doc->findState(kAuthenticatingStateId);
    if (state == nullptr || state->description != QStringLiteral("Verifies the submitted credentials") ||
        state->tags != QStringList{QStringLiteral("auth"), QStringLiteral("async")} ||
        state->exitActions != QStringList{QStringLiteral("stopSpinner()")}) {
        std::fprintf(stderr, "FAIL: Inspector metadata commits did not land in the document\n");
        return 1;
    }
    if (!saveWidgetCapture(window.debugInspector(), "probe-state-metadata")) {
        return 1;
    }

    for (int i = 0; i < 3; ++i) {
        window.loginFlowSession()->kernel().send(app::events::UndoRequested{});
    }
    QApplication::processEvents();
    state = doc->findState(kAuthenticatingStateId);
    if (state == nullptr || !state->description.isEmpty() || !state->tags.isEmpty() ||
        !state->exitActions.isEmpty()) {
        std::fprintf(stderr, "FAIL: undo x3 did not clear the metadata commits\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario state-metadata (description/tags/exit actions commit + undo)\n");
    return 0;
}

// Scenario "invoke-authoring": the Inspector's Service/Id/Output-type fields.
// Id/Output-type are disabled while Service is blank and enabled the instant it
// is set, and the debug levers commit through invokeSrcEditingFinished/
// invokeIdEditingFinished (the Enter moment), never the focus-out half, as
// buildStateTab()'s commit-on-Enter contract requires for these two fields.
int runInvokeAuthoringScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto doc = window.loginFlowSession()->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr) {
        std::fprintf(stderr, "FAIL: invoke-authoring scenario preconditions (doc) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    window.debugSelectState(loginPane, kAuthenticatingStateId);
    QApplication::processEvents();

    const app::State* baseline = doc->findState(kAuthenticatingStateId);
    if (baseline == nullptr || !baseline->invokeSrc.isEmpty() || !baseline->invokeId.isEmpty()) {
        std::fprintf(stderr, "FAIL: invoke-authoring scenario's fixture state is not invoke-empty at boot\n");
        return 1;
    }

    app::InspectorPanel* inspector = window.debugInspector();
    if (inspector->debugInvokeIdEnabled() || inspector->debugInvokeOutputTypeEnabled()) {
        std::fprintf(stderr, "FAIL: Id/Output-type fields are not inert while Service is empty\n");
        return 1;
    }

    // Commit the Service field via returnPressed only, never the focus-out
    // half: it cascades onto done.invoke./error.platform. transition events.
    inspector->debugCommitInvokeSrc(QStringLiteral("fetchUser"));
    QApplication::processEvents();

    const app::State* afterSrc = doc->findState(kAuthenticatingStateId);
    if (afterSrc == nullptr || afterSrc->invokeSrc != QStringLiteral("fetchUser")) {
        std::fprintf(stderr, "FAIL: committing the Service field did not set State::invokeSrc\n");
        return 1;
    }
    if (inspector->debugInvokeIdPlaceholder() != QStringLiteral("fetchUser")) {
        std::fprintf(stderr, "FAIL: the Id field's placeholder does not show the derived effective id (got '%s')\n",
                      qUtf8Printable(inspector->debugInvokeIdPlaceholder()));
        return 1;
    }
    if (!inspector->debugInvokeIdEnabled() || !inspector->debugInvokeOutputTypeEnabled()) {
        std::fprintf(stderr, "FAIL: Id/Output-type fields stayed inert after Service was set\n");
        return 1;
    }

    // Commit an explicit Id: it becomes the id every done.invoke./
    // error.platform. event on this state names.
    inspector->debugCommitInvokeId(QStringLiteral("fetchUserActor"));
    QApplication::processEvents();
    const app::State* afterId = doc->findState(kAuthenticatingStateId);
    if (afterId == nullptr || afterId->invokeId != QStringLiteral("fetchUserActor")) {
        std::fprintf(stderr, "FAIL: committing the Id field did not set State::invokeId\n");
        return 1;
    }

    // Output type is a combo: activated() fires only on user interaction,
    // never on a rebuild.
    inspector->debugSetInvokeOutputType(app::ContextType::String);
    const app::State* afterType = doc->findState(kAuthenticatingStateId);
    if (afterType == nullptr || afterType->invokeOutputType != app::ContextType::String) {
        std::fprintf(stderr, "FAIL: the Output-type combo did not set State::invokeOutputType\n");
        return 1;
    }

    // Capture the three Invoke rows only: a whole-panel capture would let a
    // side-bar section that widens the side bar pass unnoticed. The scenario
    // stage runs with window updates disabled and saveWidgetRegionCapture()
    // does not re-enable them, so its grab comes back blank; the re-enable is
    // done inline here.
    QWidget* topLevel = inspector->window();
    const bool reenableUpdates = topLevel != nullptr && !topLevel->updatesEnabled();
    if (reenableUpdates) {
        topLevel->setUpdatesEnabled(true);
        QApplication::processEvents();
    }
    const QImage invokeCapture = inspector->grab(inspector->debugInvokeFieldsRegion()).toImage();
    if (reenableUpdates) {
        topLevel->setUpdatesEnabled(false);
    }
    if (!writeProbeImage(invokeCapture, "probe-invoke-fields")) {
        return 1;
    }

    for (int i = 0; i < 3; ++i) {  // 3 commits above, one undo step each
        window.loginFlowSession()->kernel().send(app::events::UndoRequested{});
    }
    QApplication::processEvents();
    const app::State* restored = doc->findState(kAuthenticatingStateId);
    if (restored == nullptr || !restored->invokeSrc.isEmpty() || !restored->invokeId.isEmpty() ||
        restored->invokeOutputType != app::ContextType::Int) {
        std::fprintf(stderr, "FAIL: undo x3 did not restore the invoke-empty baseline\n");
        return 1;
    }
    if (inspector->debugInvokeIdEnabled() || inspector->debugInvokeOutputTypeEnabled()) {
        std::fprintf(stderr, "FAIL: Id/Output-type fields did not go inert again after undo cleared Service\n");
        return 1;
    }

    // Multi-invoke testing via Inspector levers:
    inspector->debugClickAddInvocation();
    QApplication::processEvents();
    if (inspector->debugInvocationCardCount() < 2) {
        std::fprintf(stderr, "FAIL: debugClickAddInvocation did not create card (count: %d)\n",
                     inspector->debugInvocationCardCount());
        return 1;
    }
    inspector->debugCommitInvocationCardSrc(1, QStringLiteral("extraService"));
    QApplication::processEvents();
    const app::State* multiState = doc->findState(kAuthenticatingStateId);
    if (!multiState || multiState->effectiveInvocations().size() < 2) {
        std::fprintf(stderr, "FAIL: multi-invocation card edit did not update state invocations\n");
        return 1;
    }

    // Wheel-event crash guard. Scrolling a card's Output-type combo emits
    // QComboBox::activated() synchronously; a synchronous card rebuild would
    // run on that same stack and delete the combo whose signal is still
    // executing. Reuses card 1's combo from the src-edit step above.
    QComboBox* cardCombo = inspector->debugInvocationCardOutputTypeCombo(1);
    if (cardCombo == nullptr) {
        std::fprintf(stderr, "FAIL: wheel-crash guard setup: card 1's Output-type combo is missing\n");
        return 1;
    }
    QPointer<QComboBox> comboGuard(cardCombo);
    const int indexBeforeWheel = cardCombo->currentIndex();

    // A genuine QWheelEvent via sendEvent() runs QComboBox::wheelEvent() as a
    // real scroll does; offscreen it moves currentIndex() and fires activated().
    QWheelEvent cardWheel(QPointF(cardCombo->width() / 2.0, cardCombo->height() / 2.0),
                          cardCombo->mapToGlobal(QPoint(cardCombo->width() / 2, cardCombo->height() / 2)),
                          QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(cardCombo, &cardWheel);

    // Key assertion: before any processEvents(), the combo that just emitted
    // activated() must still be alive. A synchronous rebuild would delete it.
    if (comboGuard.isNull()) {
        std::fprintf(stderr,
                     "FAIL: wheel-crash guard: the combo was destroyed inside its own activated() emission "
                     "(the ACCESS_VIOLATION this scenario pins, state-designer.log line 1523166)\n");
        return 1;
    }
    if (cardCombo->currentIndex() == indexBeforeWheel) {
        std::fprintf(stderr, "FAIL: wheel-crash guard: the QWheelEvent did not move the combo's currentIndex()\n");
        return 1;
    }
    const int newTypeData = comboGuard->currentData().toInt();

    QApplication::processEvents();
    if (!comboGuard.isNull()) {
        std::fprintf(stderr, "FAIL: wheel-crash guard: the card rebuild never ran after the wheel-driven edit\n");
        return 1;
    }
    const app::State* afterWheelEdit = doc->findState(kAuthenticatingStateId);
    if (afterWheelEdit == nullptr || afterWheelEdit->effectiveInvocations().size() < 2 ||
        static_cast<int>(afterWheelEdit->effectiveInvocations()[1].outputType) != newTypeData) {
        std::fprintf(stderr, "FAIL: the wheel-driven Output-type edit did not commit to the document\n");
        return 1;
    }

    // Remove the extra invocation
    inspector->debugRemoveInvocation(1);
    QApplication::processEvents();
    if (doc->findState(kAuthenticatingStateId)->effectiveInvocations().size() != 1) {
        std::fprintf(stderr, "FAIL: debugRemoveInvocation did not remove invocation\n");
        return 1;
    }

    // Clean up with undo (undo remove, undo wheel-driven output-type edit,
    // undo commit src, undo add invocation)
    window.loginFlowSession()->kernel().send(app::events::UndoRequested{});
    window.loginFlowSession()->kernel().send(app::events::UndoRequested{});
    window.loginFlowSession()->kernel().send(app::events::UndoRequested{});
    window.loginFlowSession()->kernel().send(app::events::UndoRequested{});
    QApplication::processEvents();
    const app::State* finalRestored = doc->findState(kAuthenticatingStateId);
    if (finalRestored == nullptr || !finalRestored->invokeSrc.isEmpty() || !finalRestored->invocations.isEmpty()) {
        std::fprintf(stderr, "FAIL: multi-invocation cleanup did not restore empty state\n");
        return 1;
    }

    std::printf(
        "PASS: gui-probe scenario invoke-authoring (Service/Id/Output-type commit, multi-card add/remove, derive "
        "placeholder, inert gating, undo)\n");
    return 0;
}

// Scenario "context-verbs": the state context menu's structure
// (asserted without exec() via the presenter's builder) and the quick-add
// verbs it shares with the "+" handle -- a self/targetless add lands as ONE
// undo step (transition + auto "Event N" name in a batch) and one undo
// removes it wholly.
int runContextVerbsScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto doc = window.loginFlowSession()->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    if (doc == nullptr || presenter == nullptr) {
        std::fprintf(stderr, "FAIL: context-verbs scenario preconditions (doc/presenter) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);

    QMenu* menu = presenter->debugBuildStateContextMenu(1);  // LoggedOut, the machine's initial state
    if (menu == nullptr) {
        std::fprintf(stderr, "FAIL: debugBuildStateContextMenu(1) returned no menu\n");
        return 1;
    }
    QStringList titles;
    QAction* initialAction = nullptr;
    for (QAction* action : menu->actions()) {
        if (action->isSeparator()) {
            continue;
        }
        titles.push_back(action->text());
        if (action->text() == QStringLiteral("Initial State")) {
            initialAction = action;
        }
    }
    const QStringList expected{
        QStringLiteral("Zoom to Selection"),  // the element menus' shared first entry
        QStringLiteral("Rename"),          QStringLiteral("State Type"),
        QStringLiteral("Initial State"),   QStringLiteral("Breakpoint"),
        QStringLiteral("Add Transition"),
        QStringLiteral("Add Self-Transition"), QStringLiteral("Add Targetless Transition"),
        QStringLiteral("Add Child State"),  // LoggedOut is Normal kind
        QStringLiteral("Add Entry Action"), QStringLiteral("Add Exit Action"),
        QStringLiteral("Add Description"), QStringLiteral("Add Tag"),
        QStringLiteral("Delete"),
    };
    const bool structureOk = titles == expected && initialAction != nullptr && initialAction->isChecked();
    menu->deleteLater();
    if (!structureOk) {
        std::fprintf(stderr, "FAIL: state context menu structure/checkmarks are not plan §5.3's (got: %s)\n",
                     qUtf8Printable(titles.join(QStringLiteral(" | "))));
        return 1;
    }

    const int transitionsBefore = static_cast<int>(doc->machine().transitions.size());
    presenter->debugAddSelfTransition(1);
    QApplication::processEvents();
    const app::Transition* added = nullptr;
    for (const app::Transition& transition : doc->machine().transitions) {
        if (transition.from == 1 && transition.to == 1) {
            added = &transition;
        }
    }
    if (added == nullptr || !added->event.startsWith(QStringLiteral("Event"))) {
        std::fprintf(stderr, "FAIL: quick-add self did not create a self transition with an auto Event name\n");
        return 1;
    }
    if (!saveSceneCapture(loginPane, "probe-context-quick-add")) {
        return 1;
    }
    window.loginFlowSession()->kernel().send(app::events::UndoRequested{});  // the whole batch, one step
    QApplication::processEvents();
    if (static_cast<int>(doc->machine().transitions.size()) != transitionsBefore) {
        std::fprintf(stderr, "FAIL: ONE undo did not remove the quick-add self batch\n");
        return 1;
    }

    presenter->debugAddTargetlessTransition(2);
    QApplication::processEvents();
    quint64 targetlessId = 0;
    for (const app::Transition& transition : doc->machine().transitions) {
        if (transition.from == 2 && transition.to == 0) {
            targetlessId = transition.id;
        }
    }
    if (targetlessId == 0 || !presenter->debugHasTransitionVisual(targetlessId)) {
        std::fprintf(stderr, "FAIL: quick-add targetless did not create + render a targetless transition\n");
        return 1;
    }
    window.loginFlowSession()->kernel().send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (static_cast<int>(doc->machine().transitions.size()) != transitionsBefore) {
        std::fprintf(stderr, "FAIL: ONE undo did not remove the quick-add targetless batch\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario context-verbs (menu structure, quick-add self/targetless, batch undo)\n");
    return 0;
}

// Scenario "code-tab": the domain/ stubs are part of the live
// preview set and the file combo navigates onto them; the capture shows the
// syntax-highlighted stub. The on-disk write-once behavior is
// --codegen-check's assertion (tamper sentinel), not this scenario's.
int runCodeTabScenario(app::MainWindow& window, app::EditorView* loginPane) {
    window.debugFocusView(loginPane);
    window.debugShowCodeTab(QStringLiteral("_hooks_impl.h"));
    QApplication::processEvents();
    if (!window.debugInspector()->debugCurrentCodeFile().endsWith(QStringLiteral("_hooks_impl.h"))) {
        std::fprintf(stderr, "FAIL: the domain/ hooks stub is missing from the Code tab's preview set\n");
        return 1;
    }
    if (!saveWidgetCapture(window.debugInspector(), "probe-code-tab-stub")) {
        return 1;
    }
    window.debugShowCodeTab(QStringLiteral("_commands.h"));
    QApplication::processEvents();
    if (!window.debugInspector()->debugCurrentCodeFile().endsWith(QStringLiteral("_commands.h"))) {
        std::fprintf(stderr, "FAIL: Code tab navigation back to the commands file failed\n");
        return 1;
    }

    // Dev-Mode contextual live code projection in the Inspector
    loginPane->presenter()->debugSelectState(1);  // Select state 1 (LoggedOut)
    QApplication::processEvents();
    auto* statePreview = window.debugInspector()->debugStateCodePreview();
    if (statePreview == nullptr) {
        std::fprintf(stderr, "FAIL: debugStateCodePreview is null\n");
        return 1;
    }
    const QString stateText = statePreview->debugDisplayedText();
    if (!stateText.contains("LoggedOut")) {
        std::fprintf(stderr, "FAIL: state code projection does not contain LoggedOut: %s\n",
                     stateText.toUtf8().constData());
        return 1;
    }
    statePreview->debugSelectFilter(app::NodeCodePreviewWidget::Filter::StateEnum);
    if (!statePreview->debugDisplayedText().contains("LoggedOut")) {
        std::fprintf(stderr, "FAIL: StateEnum filter failed\n");
        return 1;
    }

    loginPane->presenter()->debugSelectTransition(1);  // Select transition 1
    QApplication::processEvents();
    auto* transPreview = window.debugInspector()->debugTransitionCodePreview();
    if (transPreview == nullptr) {
        std::fprintf(stderr, "FAIL: debugTransitionCodePreview is null\n");
        return 1;
    }
    if (transPreview->debugDisplayedText().isEmpty()) {
        std::fprintf(stderr, "FAIL: transition code projection is empty\n");
        return 1;
    }
    if (!saveWidgetCapture(window.debugInspector(), "probe-code-projection-inspector")) {
        return 1;
    }

    // In-node semantic action syntax tinting
    loginPane->session()->kernel().send(app::events::SetEntryActionsRequested{
        .id = 1,
        .entryActions = {QStringLiteral("startHeating(ctx)"), QStringLiteral("showStatus(\"heating\")")}});
    loginPane->session()->kernel().send(app::events::SetExitActionsRequested{
        .id = 1,
        .exitActions = {QStringLiteral("clearProcessing()")}});
    QApplication::processEvents();

    app::StateItem* stateItem = findStateItemById(loginPane->canvasView()->scene(), 1);
    if (stateItem == nullptr) {
        std::fprintf(stderr, "FAIL: stateItem 1 not found for in-node syntax tinting capture\n");
        return 1;
    }
    if (!saveSceneRegionCapture(loginPane, stateItem->sceneRect().adjusted(-12.0, -12.0, 12.0, 12.0),
                                "probe-in-node-syntax-tinting")) {
        return 1;
    }

    // Revert fixture back via Undo
    loginPane->session()->kernel().send(app::events::UndoRequested{});
    loginPane->session()->kernel().send(app::events::UndoRequested{});
    QApplication::processEvents();

    // In-canvas HUD Code Lens
    loginPane->presenter()->debugSelectState(1);
    QApplication::processEvents();
    loginPane->presenter()->debugToggleCodeLens();
    QApplication::processEvents();

    if (!loginPane->presenter()->debugCodeLensVisible()) {
        std::fprintf(stderr, "FAIL: Code Lens is not visible after debugToggleCodeLens\n");
        return 1;
    }
    if (loginPane->presenter()->debugCodeLensTitle() != QStringLiteral("LoggedOut")) {
        std::fprintf(stderr, "FAIL: Code Lens title is not LoggedOut: %s\n",
                     loginPane->presenter()->debugCodeLensTitle().toUtf8().constData());
        return 1;
    }
    if (!loginPane->presenter()->debugCodeLensCode().contains("LoggedOut")) {
        std::fprintf(stderr, "FAIL: Code Lens code does not contain LoggedOut\n");
        return 1;
    }

    const QRectF lensCaptureRect = (stateItem->sceneRect() | loginPane->presenter()->debugCodeLensRect()).adjusted(-14.0, -14.0, 14.0, 14.0);
    if (!saveSceneRegionCapture(loginPane, lensCaptureRect, "probe-code-lens-hud")) {
        return 1;
    }

    // Dismiss via toggle
    loginPane->presenter()->debugToggleCodeLens();
    QApplication::processEvents();
    if (loginPane->presenter()->debugCodeLensVisible()) {
        std::fprintf(stderr, "FAIL: Code Lens still visible after toggle dismiss\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario code-tab (domain stubs in preview, combo navigation, contextual projection, in-node action tinting, code-lens hud)\n");
    return 0;
}

// Scenario "pill-reconnect": the pill-offset and retarget verb
// layer -- a finished pill drag and a finished reconnect drag each commit
// exactly these intents (the mouse gesture itself is the probe's recorded
// limit). Asserts persistence in the document, the visual surviving a
// retarget, and undo restoring both.
int runPillReconnectScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::CanvasPresenter* presenter = loginPane->presenter();
    if (doc == nullptr || presenter == nullptr) {
        std::fprintf(stderr, "FAIL: pill-reconnect scenario preconditions (doc/presenter) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);

    kernel.send(app::events::MoveTransitionLabelRequested{.id = 5, .offset = QPointF(0.0, -40.0)});  // Login's pill
    QApplication::processEvents();
    if (doc->findTransition(5)->labelOffset != QPointF(0.0, -40.0)) {
        std::fprintf(stderr, "FAIL: the pill offset did not land in the document\n");
        return 1;
    }

    kernel.send(app::events::RetargetTransitionRequested{.id = 8, .from = 4, .to = 2});  // Reset -> Authenticating
    QApplication::processEvents();
    if (doc->findTransition(8)->to != 2 || !presenter->debugHasTransitionVisual(8)) {
        std::fprintf(stderr, "FAIL: retarget did not land (document/visual)\n");
        return 1;
    }
    if (!saveSceneCapture(loginPane, "probe-pill-reconnect")) {
        return 1;
    }

    kernel.send(app::events::UndoRequested{});  // the retarget
    kernel.send(app::events::UndoRequested{});  // the pill offset
    QApplication::processEvents();
    if (doc->findTransition(8)->to != 1 || doc->findTransition(5)->labelOffset != QPointF()) {
        std::fprintf(stderr, "FAIL: undo x2 did not restore the retarget and the pill offset\n");
        return 1;
    }

    // Regression: an extreme offset, far past the target node, must leave (a)
    // both drawn halves non-empty, (b) the target half ending exactly at the
    // arrowhead's centroid (= lineStop), and (c) the pill pinned on the bent
    // route's label anchor.
    kernel.send(app::events::MoveTransitionLabelRequested{.id = 5, .offset = QPointF(2000.0, 300.0)});
    QApplication::processEvents();
    {
        const app::TransitionItem* edge = presenter->debugTransitionItem(5);
        if (edge == nullptr) {
            std::fprintf(stderr, "FAIL: transition 5 has no edge item\n");
            return 1;
        }
        const app::RoutedEdge& route = edge->debugRoute();
        if (route.sourceHalf.isEmpty() || route.targetHalf.isEmpty() || route.arrowhead.isEmpty()) {
            std::fprintf(stderr, "FAIL: extreme pill offset truncated a drawn half (or the arrowhead) away\n");
            return 1;
        }
        QPointF arrowCentroid;
        for (const QPointF& point : route.arrowhead) {
            arrowCentroid += point;
        }
        arrowCentroid /= static_cast<qreal>(route.arrowhead.size());
        const QPointF lineEnd = route.targetHalf.currentPosition();
        if (std::hypot(lineEnd.x() - arrowCentroid.x(), lineEnd.y() - arrowCentroid.y()) > 1.0) {
            std::fprintf(stderr, "FAIL: the target half no longer reaches the arrowhead (detached arrow)\n");
            return 1;
        }
        if (presenter->debugLabelCenter(5) != route.labelAnchor) {
            std::fprintf(stderr, "FAIL: the pill is not pinned on the bent route's label anchor\n");
            return 1;
        }
    }
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findTransition(5)->labelOffset != QPointF()) {
        std::fprintf(stderr, "FAIL: undo did not clear the extreme pill offset\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario pill-reconnect (offset persists, retarget reroutes, undo)\n");
    return 0;
}

// Scenario "logic-rows": the Guards/Actions sections: row merge-by-source-text,
// Hook/InlineExpression classification, reference count, click-to-reveal, and
// refresh on edits, add/delete and the undo/redo rebuild. The fixture's own
// "canLogin" Hook covers items 1 and 6; the rest is built far outside it with
// blank transition events and torn down, so runContextSectionScenario's "context
// starts empty" precondition holds. Must run right before it.
int runLogicRowsScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::LogicPanel* logic = window.debugLogicPanel();
    if (doc == nullptr || logic == nullptr) {
        std::fprintf(stderr, "FAIL: logic-rows scenario preconditions (doc/panel) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    if (!doc->machine().context.isEmpty()) {
        std::fprintf(stderr, "FAIL: logic-rows scenario precondition -- login flow already has context vars\n");
        return 1;
    }
    const QVector<app::State> originalStates = doc->machine().states;
    const QVector<app::Transition> originalTransitions = doc->machine().transitions;

    // The login-flow demo's own guard, found by value: the id is read back
    // rather than assumed.
    quint64 loginTransitionId = 0;
    for (const app::Transition& transition : doc->machine().transitions) {
        if (transition.guard.trimmed() == QStringLiteral("canLogin")) {
            loginTransitionId = transition.id;
            break;
        }
    }
    if (loginTransitionId == 0) {
        std::fprintf(stderr, "FAIL: logic-rows scenario precondition -- login-flow's own canLogin guard is missing\n");
        return 1;
    }

    // debugGuardRow()/debugActionRow() return LogicRow{} (empty name) past the
    // end; a linear scan by name is enough at these row counts.
    auto findRow = [](auto rowAt, int count, const QString& name) -> app::LogicRow {
        for (int i = 0; i < count; ++i) {
            const app::LogicRow row = rowAt(i);
            if (row.name == name) {
                return row;
            }
        }
        return app::LogicRow{};
    };

    // ---- 1) baseline: Guards/Actions list rows, canLogin renders Hook -------
    const int guardCount0 = logic->debugGuardRowCount();
    const int actionCount0 = logic->debugActionRowCount();
    if (guardCount0 == 0 || actionCount0 == 0) {
        std::fprintf(stderr, "FAIL: logic-rows scenario precondition -- Guards/Actions sections are empty before any edit\n");
        return 1;
    }
    const app::LogicRow canLoginRow =
        findRow([logic](int i) { return logic->debugGuardRow(i); }, guardCount0, QStringLiteral("canLogin"));
    if (canLoginRow.name != QStringLiteral("canLogin") || canLoginRow.rowClass != app::LogicRowClass::Hook ||
        canLoginRow.transitionIds != QVector<quint64>{loginTransitionId}) {
        std::fprintf(stderr, "FAIL: the login-flow's own canLogin guard did not render as a single Hook row over its transition\n");
        return 1;
    }

    // ---- 6) clicking a guard row reveals its transition ----------------------
    QGraphicsScene* scene = loginPane->canvasView() != nullptr ? loginPane->canvasView()->scene() : nullptr;
    if (scene != nullptr) {
        scene->clearSelection();
        QApplication::processEvents();
    }
    int canLoginIndex = -1;
    for (int i = 0; i < guardCount0; ++i) {
        if (logic->debugGuardRow(i).name == QStringLiteral("canLogin")) {
            canLoginIndex = i;
            break;
        }
    }
    if (canLoginIndex < 0) {
        std::fprintf(stderr, "FAIL: could not re-locate the canLogin row by index for the click-to-reveal assertion\n");
        return 1;
    }
    logic->debugClickGuardRow(canLoginIndex);
    QApplication::processEvents();
    const app::CanvasPresenter::Selection revealSelection = loginPane->presenter() != nullptr
                                                                  ? loginPane->presenter()->currentSelection()
                                                                  : app::CanvasPresenter::Selection{};
    if (revealSelection.kind != app::SelectionKind::Transition || revealSelection.id != loginTransitionId) {
        std::fprintf(stderr,
                      "FAIL: clicking the canLogin guard row did not select its transition on the canvas (kind=%d "
                      "id=%llu, want id=%llu)\n",
                      static_cast<int>(revealSelection.kind), static_cast<unsigned long long>(revealSelection.id),
                      static_cast<unsigned long long>(loginTransitionId));
        return 1;
    }

    // ---- 2) an expression guard classifies InlineExpression ------------------
    // A context variable first, so "count > 3" is authored against a real
    // schema entry and type-checks.
    kernel.send(app::events::AddContextVariableRequested{});
    QApplication::processEvents();
    const quint64 countVarId = doc->machine().context.last().id;
    kernel.send(app::events::RenameContextVariableRequested{.id = countVarId, .name = QStringLiteral("count")});
    kernel.send(app::events::SetContextTypeRequested{.id = countVarId, .type = app::ContextType::Int});
    QApplication::processEvents();

    // Far outside the login-flow fixture: one fresh source, two destinations,
    // torn down at the end. Every transition keeps a blank event, so
    // detail::wouldCreateSecondUnguarded()'s named-event gate never engages.
    const quint64 kSrc = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2400.0, -700.0)});
    const quint64 kDstA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2650.0, -760.0)});
    const quint64 kDstB = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2650.0, -640.0)});

    const quint64 kT1 = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kSrc, .to = kDstA});
    kernel.send(app::events::SetTransitionGuardRequested{.id = kT1, .guard = QStringLiteral("count > 3")});
    QApplication::processEvents();

    const int guardCount1 = logic->debugGuardRowCount();
    const app::LogicRow exprRow1 =
        findRow([logic](int i) { return logic->debugGuardRow(i); }, guardCount1, QStringLiteral("count > 3"));
    if (exprRow1.name != QStringLiteral("count > 3") || exprRow1.rowClass != app::LogicRowClass::InlineExpression ||
        exprRow1.transitionIds != QVector<quint64>{kT1}) {
        std::fprintf(stderr,
                      "FAIL: setting a transition guard to the expression 'count > 3' did not render an "
                      "InlineExpression row over it\n");
        return 1;
    }

    // ---- 3) a second transition sharing the SAME guard text merges, refcount 2 --
    const quint64 kT2 = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kSrc, .to = kDstB});
    kernel.send(app::events::SetTransitionGuardRequested{.id = kT2, .guard = QStringLiteral("count > 3")});
    QApplication::processEvents();

    const int guardCount2 = logic->debugGuardRowCount();
    if (guardCount2 != guardCount1) {
        std::fprintf(stderr,
                      "FAIL: a second transition sharing guard text 'count > 3' minted a SEPARATE row instead of "
                      "merging (row count %d -> %d)\n",
                      guardCount1, guardCount2);
        return 1;
    }
    const app::LogicRow exprRow2 =
        findRow([logic](int i) { return logic->debugGuardRow(i); }, guardCount2, QStringLiteral("count > 3"));
    if (exprRow2.transitionIds != (QVector<quint64>{kT1, kT2})) {
        std::fprintf(stderr,
                      "FAIL: the merged 'count > 3' guard row's reference list is not [kT1, kT2] in document order "
                      "(size=%d)\n",
                      exprRow2.transitionIds.size());
        return 1;
    }

    // ---- 5) a transition action and a state's entry action sharing a name merge, refcount 2 --
    const quint64 kT3 = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kSrc, .to = kDstA});
    kernel.send(app::events::SetTransitionActionRequested{.id = kT3, .action = QStringLiteral("logMetric")});
    kernel.send(
        app::events::SetEntryActionsRequested{.id = kDstA, .entryActions = QStringList{QStringLiteral("logMetric")}});
    QApplication::processEvents();

    const int actionCount1 = logic->debugActionRowCount();
    const app::LogicRow sharedActionRow =
        findRow([logic](int i) { return logic->debugActionRow(i); }, actionCount1, QStringLiteral("logMetric"));
    if (sharedActionRow.name != QStringLiteral("logMetric") || sharedActionRow.transitionIds != QVector<quint64>{kT3} ||
        sharedActionRow.stateIds != QVector<quint64>{kDstA} ||
        sharedActionRow.transitionIds.size() + sharedActionRow.stateIds.size() != 2) {
        std::fprintf(stderr,
                      "FAIL: a transition action and a state entry action sharing the name 'logMetric' did not "
                      "merge into one row with both references (refcount %d)\n",
                      sharedActionRow.transitionIds.size() + sharedActionRow.stateIds.size());
        return 1;
    }

    // Not saveWidgetRegionCapture(): it does a plain grab(region) with no
    // update re-enable, and the scenario stage runs with window updates
    // disabled, so the grab comes back blank. Re-enable the top-level window's
    // updates for one paint here, then hand the image to writeProbeImage().
    QWidget* logicTopLevel = logic->window();
    const bool reenableUpdates = logicTopLevel != nullptr && !logicTopLevel->updatesEnabled();
    if (reenableUpdates) {
        logicTopLevel->setUpdatesEnabled(true);
        QApplication::processEvents();
    }
    const QImage logicRowsCapture =
        logic->grab(logic->debugSectionWidget(1)->geometry().united(logic->debugSectionWidget(2)->geometry()))
            .toImage();
    if (reenableUpdates) {
        logicTopLevel->setUpdatesEnabled(false);
    }
    if (!writeProbeImage(logicRowsCapture, "probe-logic-rows")) {
        return 1;
    }

    // ---- 4) clearing a guard removes its row; emptying the section restores the placeholder --
    kernel.send(app::events::SetTransitionGuardRequested{.id = kT1, .guard = QString()});
    kernel.send(app::events::SetTransitionGuardRequested{.id = kT2, .guard = QString()});
    QApplication::processEvents();
    const int guardCount3 = logic->debugGuardRowCount();
    if (guardCount3 != guardCount0 ||
        findRow([logic](int i) { return logic->debugGuardRow(i); }, guardCount3, QStringLiteral("count > 3")).name ==
            QStringLiteral("count > 3")) {
        std::fprintf(stderr, "FAIL: clearing both transitions' guard text did not remove the merged 'count > 3' row\n");
        return 1;
    }

    // Clear the login-flow fixture's own guard too, so the Guards section goes
    // fully empty and the placeholder is exercised. Step 7 restores it via
    // UndoRequested, so this is also that assertion's setup.
    kernel.send(app::events::SetTransitionGuardRequested{.id = loginTransitionId, .guard = QString()});
    QApplication::processEvents();
    if (logic->debugGuardRowCount() != 0 || !logic->debugGuardPlaceholderVisible()) {
        std::fprintf(stderr,
                      "FAIL: clearing the login-flow's own last guard did not empty the Guards section and restore "
                      "its placeholder\n");
        return 1;
    }

    // ---- 7) ONE undo restores the rows to match the restored document --------
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findTransition(loginTransitionId) == nullptr ||
        doc->findTransition(loginTransitionId)->guard != QStringLiteral("canLogin")) {
        std::fprintf(stderr, "FAIL: ONE undo did not restore the login-flow's own canLogin guard on the document\n");
        return 1;
    }
    if (logic->debugGuardRowCount() != 1 || logic->debugGuardPlaceholderVisible()) {
        std::fprintf(stderr,
                      "FAIL: ONE undo restored the document's canLogin guard but the Guards section did not "
                      "rebuild from the fact\n");
        return 1;
    }
    const app::LogicRow restoredCanLoginRow = logic->debugGuardRow(0);
    if (restoredCanLoginRow.name != QStringLiteral("canLogin") ||
        restoredCanLoginRow.rowClass != app::LogicRowClass::Hook ||
        restoredCanLoginRow.transitionIds != QVector<quint64>{loginTransitionId}) {
        std::fprintf(stderr, "FAIL: the undo-restored Guards row does not match the restored canLogin guard\n");
        return 1;
    }

    // ---- cleanup: delete the borrowed fixture, restoring the machine ---------
    kernel.send(app::events::DeleteContextVariableRequested{.id = countVarId});
    kernel.send(app::events::DeleteStateRequested{.id = kSrc});
    kernel.send(app::events::DeleteStateRequested{.id = kDstA});
    kernel.send(app::events::DeleteStateRequested{.id = kDstB});
    QApplication::processEvents();
    if (!doc->machine().context.isEmpty() || doc->machine().states != originalStates ||
        doc->machine().transitions != originalTransitions) {
        std::fprintf(stderr, "FAIL: the logic-rows scenario did not restore the login-flow machine it borrowed\n");
        return 1;
    }
    if (scene != nullptr) {
        scene->clearSelection();
        QApplication::processEvents();
    }

    std::printf(
        "PASS: gui-probe scenario logic-rows (Guards/Actions row lists, Hook/InlineExpression classification, "
        "merge-by-source-text with reference counts, click-to-reveal via selectTransition, clear removes row + "
        "restores placeholder, one undo restores rows)\n");
    return 0;
}

// Scenario "context-section": drives the Logic panel's Context-table
// affordances, not the kernel, since the wiring to the ContextVariable* intents
// is under test. Two variables are minted so undo is seen restoring the first at
// index 0, not appending it. Must finish before runLogicPanelScenario(), which
// rebuilds the pane layout. clearFocus() delivers a real FocusOut, because a row
// rebuild inside focusOutEvent would delete the widget mid-handler.
int runFocusOutCommitScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::LogicPanel* logic = window.debugLogicPanel();
    if (doc == nullptr || logic == nullptr || loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: focus-out-commit scenario preconditions (doc/panel/pane) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    if (app::InspectorPanel* inspector = window.debugInspector()) {
        inspector->showTabForSelection(app::SelectionKind::None);
    }
    window.activateWindow();
    kernel.send(app::events::AddContextVariableRequested{});
    QApplication::processEvents();
    if (doc->machine().context.isEmpty()) {
        std::fprintf(stderr, "FAIL: focus-out-commit scenario could not mint a context variable\n");
        return 1;
    }
    const quint64 variableId = doc->machine().context.last().id;

    // Rows are rebuilt on a zero-timer (LogicPanel::scheduleFlush), so one more
    // loop turn makes them real, focusable widgets.
    QApplication::processEvents();
    QLineEdit* nameEdit = logic->debugContextNameEdit(0);
    if (nameEdit == nullptr) {
        std::fprintf(stderr, "FAIL: focus-out-commit scenario found no Context row name editor\n");
        return 1;
    }

    // Type, then move focus away, as a user does.
    bool destroyedDuringClear = false;
    bool insideClear = false;
    QObject::connect(nameEdit, &QObject::destroyed, nameEdit,
                     [&destroyedDuringClear, &insideClear] { destroyedDuringClear = insideClear; });
    nameEdit->setFocus();
    if (!nameEdit->hasFocus()) {
        // Without real focus the rest of this scenario would pass without
        // running the path under test.
        std::fprintf(stderr, "FAIL: focus-out-commit scenario could not give the name editor real focus\n");
        return 1;
    }
    nameEdit->setText(QStringLiteral("renamedByFocusOut"));
    insideClear = true;
    nameEdit->clearFocus();  // focusOutEvent -> editingFinished -> intent -> fact -> rebuild
    insideClear = false;

    // Assert that the widget is not destroyed inside its own event handler,
    // not merely that nothing crashed: that is undefined behaviour which can
    // survive this path while faulting on the mouse-driven one.
    if (destroyedDuringClear) {
        std::fprintf(stderr,
                      "FAIL: the Context row editor was DESTROYED inside its own focusOutEvent -- a rebuild is "
                      "running synchronously again (see LogicPanel::scheduleFlush)\n");
        return 1;
    }
    QApplication::processEvents();

    const app::ContextVariable* renamed = doc->findContextVariable(variableId);
    if (renamed == nullptr || renamed->name != QStringLiteral("renamedByFocusOut")) {
        std::fprintf(stderr, "FAIL: a focus-out commit did not reach the document (got \"%s\")\n",
                      renamed == nullptr ? "<deleted>" : qUtf8Printable(renamed->name));
        return 1;
    }
    // The panel must have caught up: a deferred rebuild that never ran would
    // leave the old name on screen.
    if (logic->debugContextRow(0).name != QStringLiteral("renamedByFocusOut")) {
        std::fprintf(stderr, "FAIL: the Context row did not show the focus-out commit after settling (got \"%s\")\n",
                      qUtf8Printable(logic->debugContextRow(0).name));
        return 1;
    }

    kernel.send(app::events::DeleteContextVariableRequested{.id = variableId});
    QApplication::processEvents();
    if (!doc->machine().context.isEmpty()) {
        std::fprintf(stderr, "FAIL: focus-out-commit scenario did not leave the fixture as it found it\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario focus-out-commit (a real FocusOut commit survives the rebuild it "
                 "triggers, and the row catches up)\n");
    return 0;
}

// The live run overlay: context values in the Context section and the last
// macrostep's guard results in the Guards section, shown only while a run is
// live. It must follow the run through facts the simulator already publishes
// (TraceAppended fires on every assign). Leaving Simulate must clear it: a
// stale value on screen after a run ends is the worst failure.
int runLiveOverlayScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    app::LogicPanel* logic = window.debugLogicPanel();
    if (doc == nullptr || sim == nullptr || logic == nullptr || loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: live-overlay scenario preconditions (doc/sim/panel/pane) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    // Author everything before Run: every edit intent is refused in Simulate.
    kernel.send(app::events::AddContextVariableRequested{});
    QApplication::processEvents();
    if (doc->machine().context.isEmpty()) {
        std::fprintf(stderr, "FAIL: live-overlay scenario could not mint a context variable\n");
        return 1;
    }
    const app::ContextVariable minted = doc->machine().context.last();
    const quint64 variableId = minted.id;
    const QString variableName = minted.name;

    // An assign on the login-flow machine's own guarded transition, so one
    // macrostep both evaluates a guard and moves a context value.
    quint64 guardedTransitionId = 0;
    for (const app::Transition& transition : doc->machine().transitions) {
        if (transition.guard.trimmed() == QStringLiteral("canLogin")) {
            guardedTransitionId = transition.id;
            break;
        }
    }
    if (guardedTransitionId == 0) {
        std::fprintf(stderr, "FAIL: live-overlay scenario precondition -- login-flow's canLogin guard is missing\n");
        return 1;
    }
    const QString originalAction = doc->findTransition(guardedTransitionId)->action;
    const QString firingEvent = doc->findTransition(guardedTransitionId)->event;
    kernel.send(app::events::SetTransitionActionRequested{
        .id = guardedTransitionId, .action = QStringLiteral("%1 = 7").arg(variableName)});
    QApplication::processEvents();

    // ---- 1. no run yet: the overlay shows nothing at all --------------------
    if (!logic->debugLiveContextValue(0).isEmpty()) {
        std::fprintf(stderr, "FAIL: the live value overlay showed before any run started (\"%s\")\n",
                      qUtf8Printable(logic->debugLiveContextValue(0)));
        return 1;
    }

    // ---- 2. Run seeds the overlay from the schema --------------------------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();
    if (logic->debugLiveContextValue(0) != minted.initialValue) {
        std::fprintf(stderr, "FAIL: Run did not show the seeded live value (want \"%s\", got \"%s\")\n",
                      qUtf8Printable(minted.initialValue), qUtf8Printable(logic->debugLiveContextValue(0)));
        return 1;
    }

    // ---- 3. the assign moves the displayed value ---------------------------
    kernel.send(app::events::SendEventRequested{.name = firingEvent});
    QApplication::processEvents();
    if (logic->debugLiveContextValue(0) != QStringLiteral("7")) {
        std::fprintf(stderr, "FAIL: the assign did not reach the live value overlay (got \"%s\")\n",
                      qUtf8Printable(logic->debugLiveContextValue(0)));
        return 1;
    }

    // ---- 4. the macrostep's guard result reaches its Guards row ------------
    int hookRowIndex = -1;
    for (int i = 0; i < logic->debugGuardRowCount(); ++i) {
        if (logic->debugGuardRow(i).name == QStringLiteral("canLogin")) {
            hookRowIndex = i;
            break;
        }
    }
    if (hookRowIndex < 0) {
        std::fprintf(stderr, "FAIL: live-overlay scenario found no canLogin Guards row to overlay\n");
        return 1;
    }
    const QString recordedResult = logic->debugLiveGuardResult(hookRowIndex);
    if (recordedResult != QStringLiteral("true") && recordedResult != QStringLiteral("false")) {
        std::fprintf(stderr, "FAIL: the guard row showed no live result after a macrostep (got \"%s\")\n",
                      qUtf8Printable(recordedResult));
        return 1;
    }

    if (!saveWidgetCapture(logic, "probe-live-overlay")) {
        return 1;
    }

    // ---- 5. leaving Simulate clears the overlay outright -------------------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();
    if (!logic->debugLiveContextValue(0).isEmpty() || !logic->debugLiveGuardResult(hookRowIndex).isEmpty()) {
        std::fprintf(stderr,
                      "FAIL: leaving Simulate left a stale live overlay (value \"%s\", guard \"%s\")\n",
                      qUtf8Printable(logic->debugLiveContextValue(0)),
                      qUtf8Printable(logic->debugLiveGuardResult(hookRowIndex)));
        return 1;
    }

    // ---- 6. leave the fixture as it was found -----------------------------
    kernel.send(app::events::SetTransitionActionRequested{.id = guardedTransitionId, .action = originalAction});
    kernel.send(app::events::DeleteContextVariableRequested{.id = variableId});
    QApplication::processEvents();
    if (!doc->machine().context.isEmpty() ||
        doc->findTransition(guardedTransitionId)->action != originalAction) {
        std::fprintf(stderr, "FAIL: live-overlay scenario did not leave the fixture as it found it\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario live-overlay (hidden before a run, seeded on Run, assign moves the value, "
                 "guard result per macrostep, cleared on leaving Simulate)\n");
    return 0;
}

// Scenario "actors-section": Actors rows derive from a state's invoke
// declaration (fields equal invokeSrc/invokeId/invokeOutputType) and a click
// reveals the owning state. The live overlay follows invocation arming:
// invisible before Run and at the initial state, live the instant the invoking
// state activates, gone the instant it exits.
int runActorsSectionScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::LogicPanel* logic = window.debugLogicPanel();
    if (doc == nullptr || logic == nullptr || loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: actors-section scenario preconditions (doc/panel/pane) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    if (logic->debugActorRowCount() != 0 || !logic->debugActorPlaceholderVisible()) {
        std::fprintf(stderr, "FAIL: actors-section scenario precondition -- login-flow already has an actor row\n");
        return 1;
    }

    // ---- 1) declaring an invoke on Authenticating populates the section ----
    kernel.send(app::events::SetInvokeSrcRequested{.stateId = kAuthenticatingStateId,
                                                    .src = QStringLiteral("fetchUser")});
    kernel.send(app::events::SetInvokeIdRequested{.stateId = kAuthenticatingStateId,
                                                   .invokeId = QStringLiteral("fetchUserActor")});
    kernel.send(app::events::SetInvokeOutputTypeRequested{.stateId = kAuthenticatingStateId,
                                                           .type = app::ContextType::String});
    QApplication::processEvents();

    if (logic->debugActorRowCount() != 1 || logic->debugActorPlaceholderVisible()) {
        std::fprintf(stderr,
                      "FAIL: declaring an invoke on Authenticating did not populate one Actors row (count=%d)\n",
                      logic->debugActorRowCount());
        return 1;
    }
    const app::ActorRow row = logic->debugActorRow(0);
    if (row.stateId != kAuthenticatingStateId || row.stateName != QStringLiteral("Authenticating") ||
        row.src != QStringLiteral("fetchUser") || row.effectiveId != QStringLiteral("fetchUserActor") ||
        row.outputType != app::ContextType::String) {
        std::fprintf(stderr,
                      "FAIL: the Actors row does not carry the state's own invoke declaration (state=%llu "
                      "name=%s src=%s id=%s type=%d)\n",
                      static_cast<unsigned long long>(row.stateId), qUtf8Printable(row.stateName),
                      qUtf8Printable(row.src), qUtf8Printable(row.effectiveId), static_cast<int>(row.outputType));
        return 1;
    }
    const app::State* authenticating = doc->findState(kAuthenticatingStateId);
    if (authenticating == nullptr || row.effectiveId != app::effectiveInvokeId(*authenticating)) {
        std::fprintf(stderr,
                      "FAIL: the Actors row's effectiveId diverges from model/machine.h's own effectiveInvokeId()\n");
        return 1;
    }

    // ---- 2) clicking the row reveals its owning state -----------------------
    QGraphicsScene* scene = loginPane->canvasView() != nullptr ? loginPane->canvasView()->scene() : nullptr;
    if (scene != nullptr) {
        scene->clearSelection();
        QApplication::processEvents();
    }
    logic->debugClickActorRow(0);
    QApplication::processEvents();
    const app::CanvasPresenter::Selection revealSelection = loginPane->presenter() != nullptr
                                                                  ? loginPane->presenter()->currentSelection()
                                                                  : app::CanvasPresenter::Selection{};
    if (revealSelection.kind != app::SelectionKind::State || revealSelection.id != kAuthenticatingStateId) {
        std::fprintf(stderr,
                      "FAIL: clicking the Actors row did not select its owning state on the canvas (kind=%d "
                      "id=%llu)\n",
                      static_cast<int>(revealSelection.kind), static_cast<unsigned long long>(revealSelection.id));
        return 1;
    }

    // Capture the Actors section region only, so a section that widens the side
    // bar cannot pass unnoticed. saveWidgetRegionCapture() does not re-enable
    // window updates and comes back blank on this stage, so the re-enable is
    // done inline. debugSectionWidget(3) is Actors (fixed order Context/
    // Guards/Actions/Actors).
    QWidget* logicTopLevel = logic->window();
    const bool reenableUpdates = logicTopLevel != nullptr && !logicTopLevel->updatesEnabled();
    if (reenableUpdates) {
        logicTopLevel->setUpdatesEnabled(true);
        QApplication::processEvents();
    }
    const QImage actorsCapture = logic->debugSectionWidget(3)->grab().toImage();
    if (reenableUpdates) {
        logicTopLevel->setUpdatesEnabled(false);
    }
    if (!writeProbeImage(actorsCapture, "probe-actors-section")) {
        return 1;
    }

    // ---- 3) the live overlay follows a run through the invoking state -------
    if (!logic->debugLiveActorState(0).isEmpty()) {
        std::fprintf(stderr, "FAIL: the Actors live overlay showed before any run started (\"%s\")\n",
                      qUtf8Printable(logic->debugLiveActorState(0)));
        return 1;
    }
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();
    if (!logic->debugLiveActorState(0).isEmpty()) {
        std::fprintf(stderr, "FAIL: the Actors row showed live before entering its invoking state (\"%s\")\n",
                      qUtf8Printable(logic->debugLiveActorState(0)));
        return 1;
    }

    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Login")});  // LoggedOut -> Authenticating
    QApplication::processEvents();
    if (logic->debugLiveActorState(0) != QStringLiteral("live")) {
        std::fprintf(stderr, "FAIL: entering Authenticating did not mark its Actors row live (got \"%s\")\n",
                      qUtf8Printable(logic->debugLiveActorState(0)));
        return 1;
    }

    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Failure")});  // Authenticating -> Error
    QApplication::processEvents();
    if (!logic->debugLiveActorState(0).isEmpty()) {
        std::fprintf(stderr, "FAIL: leaving Authenticating did not clear its Actors row's live overlay (got \"%s\")\n",
                      qUtf8Printable(logic->debugLiveActorState(0)));
        return 1;
    }

    // ---- 4) leave the fixture as it was found --------------------------------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();
    for (int i = 0; i < 3; ++i) {  // 3 commits above (src/id/output-type), one undo step each
        kernel.send(app::events::UndoRequested{});
    }
    QApplication::processEvents();
    const app::State* restored = doc->findState(kAuthenticatingStateId);
    if (restored == nullptr || !restored->invokeSrc.isEmpty() || !restored->invokeId.isEmpty() ||
        restored->invokeOutputType != app::ContextType::Int || logic->debugActorRowCount() != 0 ||
        !logic->debugActorPlaceholderVisible()) {
        std::fprintf(stderr, "FAIL: actors-section scenario did not leave the fixture as it found it\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario actors-section (populates from a state's invoke declaration, click-to-"
                "reveal, live overlay tracks entering/leaving the invoking state)\n");
    return 0;
}

// Scenario "invoke-completion": the payload editor and Done/Error buttons live
// on the Inspector's Machine tab (a State tab is unreachable during a run).
// Run, enter the invoking state, set a payload, click Done/Error, then assert
// the active state and context value: a guarded onDone ("output > 0") proves the
// payload reached the guard, an assign ("<var> = output") proves it reached context.
// Shared check: the tab's content stays inside the viewport (no horizontal bar).
static int checkInspectorControlsFit(app::InspectorPanel* inspector, const char* when) {
    QApplication::processEvents();
    QList<QPushButton*> transport;
    for (const QString& text : {QStringLiteral("Run"), QStringLiteral("Pause"), QStringLiteral("Reset"),
                                QStringLiteral("Back")}) {
        for (QPushButton* button : inspector->findChildren<QPushButton*>()) {
            if (button->text() == text && button->isVisible()) {
                transport.push_back(button);
                break;
            }
        }
    }
    QScrollArea* scroll = nullptr;
    for (QWidget* w = transport.isEmpty() ? nullptr : transport.front()->parentWidget(); w != nullptr;
         w = w->parentWidget()) {
        if ((scroll = qobject_cast<QScrollArea*>(w)) != nullptr) {
            break;
        }
    }
    if (transport.size() != 4 || scroll == nullptr) {
        std::fprintf(stderr, "FAIL: controls-fit (%s) found %lld of 4 visible transport buttons (scroll %d)\n", when,
                     static_cast<long long>(transport.size()), scroll != nullptr ? 1 : 0);
        return 1;
    }

    int exitCode = 0;
    QWidget* viewport = scroll->viewport();
    QWidget* content = scroll->widget();
    if (content->width() > viewport->width()) {
        std::fprintf(stderr, "FAIL: controls-fit (%s): the Machine tab content is %dpx wide in a %dpx viewport\n", when,
                     content->width(), viewport->width());
        exitCode = 1;
    }
    for (QPushButton* button : transport) {
        const QRect inViewport(button->mapTo(viewport, QPoint(0, 0)), button->size());
        if (inViewport.right() >= viewport->width() || button->width() < button->minimumSizeHint().width()) {
            std::fprintf(stderr,
                         "FAIL: controls-fit (%s): transport button \"%s\" is not fully visible (x %d..%d, viewport "
                         "width %d, width %d, min hint %d)\n",
                         when, qUtf8Printable(button->text()), inViewport.left(), inViewport.right(), viewport->width(),
                         button->width(), button->minimumSizeHint().width());
            exitCode = 1;
        }
    }
    if (exitCode != 0) {
        std::fprintf(stderr, "  diag: tab content min width %d, viewport width %d; widest top-level rows:\n",
                     content->minimumSizeHint().width(), viewport->width());
        for (QWidget* child : content->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (child->isVisible() && child->minimumSizeHint().width() > viewport->width() - 40) {
                std::fprintf(stderr, "  diag:   %s \"%s\" min width %d\n", child->metaObject()->className(),
                             qUtf8Printable(child->objectName()), child->minimumSizeHint().width());
            }
        }
        for (QPushButton* button : transport) {
            std::fprintf(stderr, "  diag:   button \"%s\" min hint %d, size hint %d\n", qUtf8Printable(button->text()),
                         button->minimumSizeHint().width(), button->sizeHint().width());
        }
    }
    return exitCode;
}

int runInvokeCompletionScenario(app::MainWindow& window, app::EditorView* loginPane) {
    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    QGraphicsScene* scene = loginPane != nullptr && loginPane->canvasView() != nullptr
                                ? loginPane->canvasView()->scene()
                                : nullptr;
    if (doc == nullptr || sim == nullptr || loginPane == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: invoke-completion scenario preconditions (doc/sim/pane/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();
    QApplication::processEvents();
    app::InspectorPanel* inspector = window.debugInspector();

    // login-flow's own topology: 1 LoggedOut (initial), 2 Authenticating,
    // 3 LoggedIn (Final), 4 Error, with Login/Success/Failure/Reset
    // transitions already wired; borrowed and restored below.
    constexpr quint64 kLoggedOutStateId = 1;
    constexpr quint64 kLoggedInStateId = 3;
    constexpr quint64 kErrorStateId = 4;

    const app::State* baseline = doc->findState(kAuthenticatingStateId);
    if (baseline == nullptr || !baseline->invokeSrc.isEmpty()) {
        std::fprintf(stderr, "FAIL: invoke-completion scenario precondition -- Authenticating is not invoke-empty\n");
        return 1;
    }

    // ---- 0) not running at all: the Machine tab shows the not-running
    //         guidance, no canvas selection needed ------------------------------
    const QString kNotRunningGuidance = QStringLiteral("Fires only while running, with a state's invocation live");
    if (inspector->debugInvocationEntryCount() != 0 ||
        inspector->debugInvocationsGuidanceText() != kNotRunningGuidance) {
        std::fprintf(stderr,
                      "FAIL: the Machine tab did not show the not-running guidance before any run (count=%d "
                      "guidance=\"%s\")\n",
                      inspector->debugInvocationEntryCount(), qUtf8Printable(inspector->debugInvocationsGuidanceText()));
        return 1;
    }

    // ---- 1) declare the invoke: fetchUser #userLoader on Authenticating, plus
    //         a guarded onDone and an onError, repurposing the login-flow's
    //         Success/Failure transitions (restored after) --------------------
    kernel.send(
        app::events::SetInvokeSrcRequested{.stateId = kAuthenticatingStateId, .src = QStringLiteral("fetchUser")});
    kernel.send(
        app::events::SetInvokeIdRequested{.stateId = kAuthenticatingStateId, .invokeId = QStringLiteral("userLoader")});
    kernel.send(
        app::events::SetInvokeOutputTypeRequested{.stateId = kAuthenticatingStateId, .type = app::ContextType::Int});
    QApplication::processEvents();

    // Two context variables: `resultVar` (Int, the invoke's outputType) receives
    // the onDone assign's "output"; `errorVar` (String, the onError payload's
    // fixed type) receives the onError assign's "error". A guard alone would
    // not prove the payload survived into the document.
    kernel.send(app::events::AddContextVariableRequested{});
    QApplication::processEvents();
    const app::ContextVariable resultVar = doc->machine().context.last();
    kernel.send(app::events::AddContextVariableRequested{});
    QApplication::processEvents();
    const app::ContextVariable errorVar = doc->machine().context.last();
    kernel.send(app::events::SetContextTypeRequested{.id = errorVar.id, .type = app::ContextType::String});
    QApplication::processEvents();

    quint64 doneTransitionId = 0;
    quint64 errorTransitionId = 0;
    for (const app::Transition& transition : doc->machine().transitions) {
        if (transition.from != kAuthenticatingStateId) {
            continue;
        }
        if (transition.event == QStringLiteral("Success")) {
            doneTransitionId = transition.id;
        } else if (transition.event == QStringLiteral("Failure")) {
            errorTransitionId = transition.id;
        }
    }
    if (doneTransitionId == 0 || errorTransitionId == 0) {
        std::fprintf(stderr,
                      "FAIL: invoke-completion scenario precondition -- Success/Failure transitions missing\n");
        return 1;
    }
    const QString originalDoneEvent = doc->findTransition(doneTransitionId)->event;
    const QString originalErrorEvent = doc->findTransition(errorTransitionId)->event;
    const QString originalDoneAction = doc->findTransition(doneTransitionId)->action;
    const QString originalErrorAction = doc->findTransition(errorTransitionId)->action;

    // The guarded onDone ("output > 0") proves a Done click's payload reaches
    // the guard; the assign proves it reaches context.
    kernel.send(
        app::events::SetTransitionEventRequested{.id = doneTransitionId, .event = QStringLiteral("done.invoke.userLoader")});
    kernel.send(app::events::SetTransitionGuardRequested{.id = doneTransitionId, .guard = QStringLiteral("output > 0")});
    kernel.send(app::events::SetTransitionActionRequested{
        .id = doneTransitionId, .action = QStringLiteral("%1 = output").arg(resultVar.name)});
    kernel.send(app::events::SetTransitionEventRequested{
        .id = errorTransitionId, .event = QStringLiteral("error.platform.userLoader")});
    kernel.send(app::events::SetTransitionActionRequested{
        .id = errorTransitionId, .action = QStringLiteral("%1 = error").arg(errorVar.name)});
    QApplication::processEvents();

    // ---- 2) Run, enter Authenticating: exactly one live entry with the
    //         matching label, guidance clears ---------------------------------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();
    if (inspector->debugInvocationEntryCount() != 0) {
        std::fprintf(stderr, "FAIL: an entry appeared before Authenticating was even entered\n");
        return 1;
    }
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Login")});  // LoggedOut -> Authenticating
    QApplication::processEvents();
    if (inspector->debugInvocationEntryCount() != 1) {
        std::fprintf(stderr,
                      "FAIL: entering Authenticating did not produce exactly one Machine-tab entry (count=%d)\n",
                      inspector->debugInvocationEntryCount());
        return 1;
    }
    const QString expectedLabel =
        QStringLiteral("fetchUser #userLoader %1 Authenticating").arg(QString(QChar(0x2014)));
    if (inspector->debugInvocationEntryLabel(0) != expectedLabel) {
        std::fprintf(stderr, "FAIL: the entry label was \"%s\", want \"%s\"\n",
                      qUtf8Printable(inspector->debugInvocationEntryLabel(0)), qUtf8Printable(expectedLabel));
        return 1;
    }
    if (!inspector->debugInvocationsGuidanceText().isEmpty()) {
        std::fprintf(stderr, "FAIL: guidance text still showed with a live entry present (\"%s\")\n",
                      qUtf8Printable(inspector->debugInvocationsGuidanceText()));
        return 1;
    }

    // ---- 2b) Machine tab section order + EVENTS/GUARD RESULTS disclosure:
    //          INVOCATIONS before EVENTS before GUARD RESULTS on `outer`, GUARD
    //          RESULTS starts collapsed with its count readable in the header,
    //          and EVENTS' header toggles its rows. Run while a live invocation
    //          shows, to prove the layout coexists with a real run. -----------
    QWidget* invocationsSectionWidget = inspector->debugInvocationsSectionWidget();
    QWidget* eventsSectionWidget = inspector->debugEventsSectionWidget();
    QWidget* guardsSectionWidget = inspector->debugGuardsSectionWidget();
    if (invocationsSectionWidget == nullptr || eventsSectionWidget == nullptr || guardsSectionWidget == nullptr ||
        !(invocationsSectionWidget->y() < eventsSectionWidget->y()) ||
        !(eventsSectionWidget->y() < guardsSectionWidget->y())) {
        std::fprintf(
            stderr, "FAIL: Machine tab section order is not INVOCATIONS above EVENTS above GUARD RESULTS (y=%d/%d/%d)\n",
            invocationsSectionWidget != nullptr ? invocationsSectionWidget->y() : -1,
            eventsSectionWidget != nullptr ? eventsSectionWidget->y() : -1,
            guardsSectionWidget != nullptr ? guardsSectionWidget->y() : -1);
        return 1;
    }
    if (!inspector->debugEventsBodyVisible()) {
        std::fprintf(stderr, "FAIL: EVENTS did not default to expanded\n");
        return 1;
    }
    if (inspector->debugGuardsBodyVisible()) {
        std::fprintf(stderr, "FAIL: GUARD RESULTS did not default to collapsed\n");
        return 1;
    }
    // A collapsed section still answers "how much is in here": the count lives
    // in the header text, so it stays readable while GUARD RESULTS' rows are
    // hidden. The chevron is painted by SectionHeaderButton, not part of
    // text(), so the pattern is the bare title and expanded/collapsed state is
    // asserted via the body-visibility levers below.
    static const QRegularExpression kEventsHeaderPattern(QStringLiteral("^Events \\(\\d+\\)$"));
    static const QRegularExpression kGuardsHeaderPattern(QStringLiteral("^Guard Results \\(\\d+\\)$"));
    if (!kEventsHeaderPattern.match(inspector->debugEventsHeaderText()).hasMatch()) {
        std::fprintf(stderr, "FAIL: EVENTS header lost its item count (\"%s\")\n",
                      qUtf8Printable(inspector->debugEventsHeaderText()));
        return 1;
    }
    if (!kGuardsHeaderPattern.match(inspector->debugGuardsHeaderText()).hasMatch()) {
        std::fprintf(stderr, "FAIL: GUARD RESULTS header lost its item count while collapsed (\"%s\")\n",
                      qUtf8Printable(inspector->debugGuardsHeaderText()));
        return 1;
    }
    // Toggling EVENTS hides, then re-shows, its rows.
    inspector->debugClickEventsHeader();
    QApplication::processEvents();
    if (inspector->debugEventsBodyVisible()) {
        std::fprintf(stderr, "FAIL: clicking the EVENTS header did not collapse its rows\n");
        return 1;
    }
    inspector->debugClickEventsHeader();
    QApplication::processEvents();
    if (!inspector->debugEventsBodyVisible()) {
        std::fprintf(stderr, "FAIL: clicking the EVENTS header a second time did not re-expand its rows\n");
        return 1;
    }
    // GUARD RESULTS' header toggles the same way: expand, then re-collapse to
    // leave the default (collapsed).
    inspector->debugClickGuardsHeader();
    QApplication::processEvents();
    if (!inspector->debugGuardsBodyVisible()) {
        std::fprintf(stderr, "FAIL: clicking the GUARD RESULTS header did not expand its rows\n");
        return 1;
    }
    inspector->debugClickGuardsHeader();
    QApplication::processEvents();
    if (inspector->debugGuardsBodyVisible()) {
        std::fprintf(stderr, "FAIL: clicking the GUARD RESULTS header a second time did not re-collapse its rows\n");
        return 1;
    }

    // ---- 2c) design-grammar structural checks: the header's real
    //          left-alignment mechanism and the shared row-grid axis. Structure
    //          only; visual quality is not a probe assertion. -----------------
    // The chevron is painted from isChecked() inside SectionHeaderButton, which
    // a text assertion cannot see; what remains structural is that EVENTS is
    // back to expanded after the toggle dance.
    if (!inspector->debugEventsBodyVisible()) {
        std::fprintf(stderr, "FAIL: EVENTS did not return to expanded after the toggle dance\n");
        return 1;
    }
    // QSS text-align:left does not left-align a text-only QToolButton here, so
    // assert the mechanism that does (TextBesideIcon layout with a null icon),
    // not the inert QSS string.
    if (!inspector->debugEventsHeaderLeftAligned()) {
        std::fprintf(stderr, "FAIL: the EVENTS header is not using the left-align layout mechanism\n");
        return 1;
    }
    // Shared label/value axis: the Machine tab's meta form (QFormLayout) and
    // the Simulation block (manual QHBoxLayout pair) must agree on where the
    // value column starts.
    if (inspector->debugMachineFormLabelWidth() != inspector->debugSimulationLabelWidth() ||
        inspector->debugMachineFormLabelWidth() <= 0) {
        std::fprintf(stderr, "FAIL: the Machine form and Simulation rows do not share one label axis (%d vs %d)\n",
                      inspector->debugMachineFormLabelWidth(), inspector->debugSimulationLabelWidth());
        return 1;
    }
    // Capture the Machine tab (the InspectorPanel widget, not the window) with a
    // live invocation showing: meta form, Simulation rows, INVOCATIONS and the
    // EVENTS/GUARD RESULTS headers together.
    if (!saveWidgetCapture(inspector, "probe-grammar-batchA")) {
        return 1;
    }

    // ---- 2d) design-grammar hierarchy checks: Run is the one Primary control
    //          on this tab, Pause/Reset/Back are Secondary. Style fingerprint
    //          only. ---------------------------------------------------------
    if (!inspector->debugRunButtonIsPrimary()) {
        std::fprintf(stderr, "FAIL: Run does not carry the Primary button style\n");
        return 1;
    }
    if (inspector->debugPauseButtonIsPrimary() || inspector->debugResetButtonIsPrimary() ||
        inspector->debugBackButtonIsPrimary()) {
        std::fprintf(stderr, "FAIL: a Secondary transport button (Pause/Reset/Back) carries the Primary style\n");
        return 1;
    }
    // Same region as the capture above, with Run's Primary fill. The live card
    // is what widened the tab.
    if (checkInspectorControlsFit(inspector, "live invocation card") != 0) {
        return 1;
    }
    if (!saveWidgetCapture(inspector, "probe-grammar-batchB")) {
        return 1;
    }

    // ---- 3) the entry's payload editor is a real numeric control: Up/Down and
    //         PageUp/PageDown come from QAbstractSpinBox. Checked relatively,
    //         never against a literal step count. -----------------------------
    const auto sendKey = [](QWidget* target, int key) {
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(target, &press);
        QApplication::processEvents();
    };
    QSpinBox* payloadSpin = inspector->debugInvocationPayloadIntSpinBox(0);
    if (payloadSpin == nullptr) {
        std::fprintf(stderr, "FAIL: the entry's Int payload spin box is missing\n");
        return 1;
    }
    payloadSpin->setValue(0);
    payloadSpin->setFocus();
    QApplication::processEvents();
    sendKey(payloadSpin, Qt::Key_Up);
    const int afterUp = payloadSpin->value();
    if (!(afterUp > 0)) {
        std::fprintf(stderr, "FAIL: Up did not increment the entry's Int payload spin box\n");
        return 1;
    }
    sendKey(payloadSpin, Qt::Key_PageUp);
    if (!(payloadSpin->value() > afterUp)) {
        std::fprintf(stderr, "FAIL: PageUp did not step past a single Up on the entry's Int payload spin box\n");
        return 1;
    }
    payloadSpin->setValue(0);

    // Capture the entry region only, with a real value showing.
    // saveWidgetRegionCapture() comes back blank while the stage has window
    // updates disabled, so the re-enable is done inline.
    inspector->debugSetInvocationPayloadInt(0, 5);
    QWidget* topLevel = inspector->window();
    bool reenableUpdates = topLevel != nullptr && !topLevel->updatesEnabled();
    if (reenableUpdates) {
        topLevel->setUpdatesEnabled(true);
        QApplication::processEvents();
    }
    const QImage completionCapture = inspector->grab(inspector->debugInvocationEntryRegion(0)).toImage();
    if (reenableUpdates) {
        topLevel->setUpdatesEnabled(false);
    }
    if (!writeProbeImage(completionCapture, "probe-invoke-completion")) {
        return 1;
    }

    // ---- 4) Error: click the real button, assert the resulting active state
    //         and the context value the onError assign wrote ------------------
    inspector->debugSetInvocationErrorMessage(0, QStringLiteral("network down"));
    inspector->debugClickInvocationError(0);
    QApplication::processEvents();
    if (sim->activeStateId() != kErrorStateId) {
        std::fprintf(stderr, "FAIL: clicking Error did not move the active state to Error (got %llu)\n",
                      static_cast<unsigned long long>(sim->activeStateId()));
        return 1;
    }
    if (sim->contextValues().value(errorVar.name).toString() != QStringLiteral("network down")) {
        std::fprintf(stderr, "FAIL: the onError assign did not reach context (got \"%s\")\n",
                      qUtf8Printable(sim->contextValues().value(errorVar.name).toString()));
        return 1;
    }
    const QString kNothingLiveGuidance = QStringLiteral(
        "Running, but no invocation is currently live — enter an invoking state to arm one");
    if (inspector->debugInvocationEntryCount() != 0 ||
        inspector->debugInvocationsGuidanceText() != kNothingLiveGuidance) {
        std::fprintf(stderr,
                      "FAIL: completing the invocation did not clear the entry / show the running-but-nothing-"
                      "live guidance (count=%d guidance=\"%s\")\n",
                      inspector->debugInvocationEntryCount(), qUtf8Printable(inspector->debugInvocationsGuidanceText()));
        return 1;
    }

    // ---- 5) back through Reset -> Login: a fresh invocation, Done this time,
    //         again asserting active state and the context value --------------
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Reset")});  // Error -> LoggedOut
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Login")});  // LoggedOut -> Authenticating
    QApplication::processEvents();
    if (inspector->debugInvocationEntryCount() != 1) {
        std::fprintf(stderr, "FAIL: re-entering Authenticating did not re-arm one Machine-tab entry (count=%d)\n",
                      inspector->debugInvocationEntryCount());
        return 1;
    }
    inspector->debugSetInvocationPayloadInt(0, 5);
    inspector->debugClickInvocationDone(0);
    QApplication::processEvents();
    if (sim->activeStateId() != kLoggedInStateId) {
        std::fprintf(stderr, "FAIL: clicking Done did not move the active state to LoggedIn (got %llu)\n",
                      static_cast<unsigned long long>(sim->activeStateId()));
        return 1;
    }
    if (sim->contextValues().value(resultVar.name).toInt() != 5) {
        std::fprintf(stderr, "FAIL: the onDone assign did not reach context (got %d)\n",
                      sim->contextValues().value(resultVar.name).toInt());
        return 1;
    }

    // ---- 6) two live invocations at once (a hierarchical fixture): assert
    //         the count, and that completing one leaves the other live --------
    // P{RegionA{LeafA,LeafA2}, RegionB{LeafB}} (Parallel), two levels deep: a
    // Parallel state activates every direct child permanently, so a flat A2
    // would be live from the first Run. Settle in stages (add/reparent,
    // processEvents(), transitions, processEvents()), not one big batch.
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();
    const quint64 parallelId = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(900.0, 700.0)});  // P (Parallel)
    const quint64 regionAId = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(850.0, 780.0), .parentId = parallelId});  // RegionA
    const quint64 leafAId = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(850.0, 820.0), .parentId = regionAId});  // LeafA
    const quint64 leafA2Id = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(850.0, 860.0), .parentId = regionAId});  // LeafA2
    const quint64 regionBId = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(950.0, 780.0), .parentId = parallelId});  // RegionB
    const quint64 leafBId = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(950.0, 820.0), .parentId = regionBId});  // LeafB
    QApplication::processEvents();
    kernel.send(app::events::RenameStateRequested{.id = leafAId, .name = QStringLiteral("LeafA")});
    kernel.send(app::events::RenameStateRequested{.id = leafBId, .name = QStringLiteral("LeafB")});
    kernel.send(app::events::SetStateKindRequested{.id = parallelId, .kind = app::StateKind::Parallel});
    QApplication::processEvents();
    kernel.send(app::events::SetInvokeSrcRequested{.stateId = leafAId, .src = QStringLiteral("serviceA")});
    kernel.send(app::events::SetInvokeSrcRequested{.stateId = leafBId, .src = QStringLiteral("serviceB")});
    QApplication::processEvents();
    const quint64 doneAId = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = leafAId, .to = leafA2Id});  // within RegionA only
    kernel.send(
        app::events::SetTransitionEventRequested{.id = doneAId, .event = QStringLiteral("done.invoke.serviceA")});
    QApplication::processEvents();
    kernel.send(app::events::SetInitialStateRequested{.id = parallelId});
    QApplication::processEvents();

    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();
    if (inspector->debugInvocationEntryCount() != 2) {
        std::fprintf(stderr,
                      "FAIL: a Parallel machine with two invoking regions did not show two Machine-tab entries "
                      "(count=%d)\n",
                      inspector->debugInvocationEntryCount());
        return 1;
    }
    // Entry order follows Machine::states document order: LeafA (added before
    // LeafB) is entry 0.
    inspector->debugClickInvocationDone(0);
    QApplication::processEvents();
    if (inspector->debugInvocationEntryCount() != 1 ||
        inspector->debugInvocationEntryLabel(0).indexOf(QStringLiteral("serviceB")) < 0) {
        std::fprintf(stderr,
                      "FAIL: completing LeafA's invocation did not leave LeafB's own invocation live "
                      "(count=%d label=\"%s\")\n",
                      inspector->debugInvocationEntryCount(), qUtf8Printable(inspector->debugInvocationEntryLabel(0)));
        return 1;
    }

    // ---- 7) leave the fixture as it was found --------------------------------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();
    kernel.send(app::events::DeleteStateRequested{.id = parallelId});  // cascades: every descendant + their transitions
    kernel.send(app::events::SetInitialStateRequested{.id = kLoggedOutStateId});
    kernel.send(app::events::SetTransitionEventRequested{.id = doneTransitionId, .event = originalDoneEvent});
    kernel.send(app::events::SetTransitionGuardRequested{.id = doneTransitionId, .guard = QString()});
    kernel.send(app::events::SetTransitionActionRequested{.id = doneTransitionId, .action = originalDoneAction});
    kernel.send(app::events::SetTransitionEventRequested{.id = errorTransitionId, .event = originalErrorEvent});
    kernel.send(app::events::SetTransitionActionRequested{.id = errorTransitionId, .action = originalErrorAction});
    kernel.send(app::events::DeleteContextVariableRequested{.id = resultVar.id});
    kernel.send(app::events::DeleteContextVariableRequested{.id = errorVar.id});
    kernel.send(app::events::SetInvokeSrcRequested{.stateId = kAuthenticatingStateId, .src = QString()});
    kernel.send(app::events::SetInvokeIdRequested{.stateId = kAuthenticatingStateId, .invokeId = QString()});
    kernel.send(
        app::events::SetInvokeOutputTypeRequested{.stateId = kAuthenticatingStateId, .type = app::ContextType::Int});
    QApplication::processEvents();

    const app::State* restored = doc->findState(kAuthenticatingStateId);
    if (restored == nullptr || !restored->invokeSrc.isEmpty() || !restored->invokeId.isEmpty() ||
        restored->invokeOutputType != app::ContextType::Int || !doc->machine().context.isEmpty() ||
        doc->machine().initialStateId != kLoggedOutStateId || doc->findState(parallelId) != nullptr ||
        doc->findTransition(doneTransitionId)->event != originalDoneEvent ||
        doc->findTransition(doneTransitionId)->guard != QString() ||
        doc->findTransition(errorTransitionId)->event != originalErrorEvent ||
        inspector->debugInvocationEntryCount() != 0) {
        std::fprintf(stderr, "FAIL: invoke-completion scenario did not leave the fixture as it found it\n");
        return 1;
    }

    std::printf(
        "PASS: gui-probe scenario invoke-completion (Machine tab, reachable without selection: guidance before any "
        "run, one entry on entering the invoking state, Done/Error through the real widgets move the active state "
        "and reach context via guard+assign, two live invocations render two entries and completing one leaves the "
        "other live)\n");
    return 0;
}

// Hook rename as a cascade: the panel's in-place row editor and the two
// commands behind it. One Enter must rewrite every element referencing the
// hook, one Ctrl+Z must put all of them back, and an expression row must
// refuse to open an editor (the Inspector's Guard field edits those).
// Keystrokes are real QKeyEvents, which run the panel's installed eventFilter
// as a user's Esc would.
int runHookRenameScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::LogicPanel* logic = window.debugLogicPanel();
    if (doc == nullptr || logic == nullptr || loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: hook-rename scenario preconditions (doc/panel/pane) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    // A second transition carrying the same guard hook, so the cascade has more
    // than one element to reach. Both sends are separate undo entries; the
    // rename below is the third, and only it is undone.
    quint64 guardedTransitionId = 0;
    for (const app::Transition& transition : doc->machine().transitions) {
        if (transition.guard.trimmed() == QStringLiteral("canLogin")) {
            guardedTransitionId = transition.id;
            break;
        }
    }
    if (guardedTransitionId == 0) {
        std::fprintf(stderr, "FAIL: hook-rename scenario precondition -- login-flow's canLogin guard is missing\n");
        return 1;
    }
    const app::Transition* seed = doc->findTransition(guardedTransitionId);
    const quint64 seedFrom = seed->from;
    const quint64 seedTo = seed->to;
    kernel.send(app::events::AddTransitionRequested{.from = seedFrom, .to = seedTo});
    QApplication::processEvents();
    const quint64 secondTransitionId = doc->machine().transitions.last().id;
    kernel.send(app::events::SetTransitionGuardRequested{.id = secondTransitionId,
                                                          .guard = QStringLiteral("canLogin")});
    QApplication::processEvents();
    if (doc->findTransition(secondTransitionId)->guard != QStringLiteral("canLogin")) {
        std::fprintf(stderr, "FAIL: hook-rename scenario could not seed a second canLogin-guarded transition\n");
        return 1;
    }

    // ---- 1. the merged Hook row opens an in-place editor ---------------------
    int hookRowIndex = -1;
    for (int i = 0; i < logic->debugGuardRowCount(); ++i) {
        const app::LogicRow row = logic->debugGuardRow(i);
        if (row.name == QStringLiteral("canLogin") && row.rowClass == app::LogicRowClass::Hook) {
            hookRowIndex = i;
            break;
        }
    }
    if (hookRowIndex < 0) {
        std::fprintf(stderr, "FAIL: hook-rename scenario found no merged canLogin Hook row to rename\n");
        return 1;
    }
    if (!logic->debugBeginGuardRowRename(hookRowIndex) || logic->debugRenameEditor() == nullptr) {
        std::fprintf(stderr, "FAIL: a Hook row did not open an in-place rename editor\n");
        return 1;
    }

    // ---- 2. Esc ABORTS: the editor closes and nothing changes ---------------
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(logic->debugRenameEditor(), &escape);
    QApplication::processEvents();
    if (logic->debugRenameEditor() != nullptr ||
        doc->findTransition(guardedTransitionId)->guard != QStringLiteral("canLogin")) {
        std::fprintf(stderr, "FAIL: Esc did not abort the rename editor leaving the guard untouched\n");
        return 1;
    }

    // ---- 3. Enter COMMITS, and the cascade reaches BOTH transitions ---------
    if (!logic->debugBeginGuardRowRename(hookRowIndex)) {
        std::fprintf(stderr, "FAIL: the Hook row would not reopen its rename editor after an abort\n");
        return 1;
    }
    logic->debugRenameEditor()->setText(QStringLiteral("mayLogin"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(logic->debugRenameEditor(), &enter);
    QApplication::processEvents();
    if (doc->findTransition(guardedTransitionId)->guard != QStringLiteral("mayLogin") ||
        doc->findTransition(secondTransitionId)->guard != QStringLiteral("mayLogin")) {
        std::fprintf(stderr,
                      "FAIL: the hook rename did not reach both referencing transitions (got \"%s\" and \"%s\")\n",
                      qUtf8Printable(doc->findTransition(guardedTransitionId)->guard),
                      qUtf8Printable(doc->findTransition(secondTransitionId)->guard));
        return 1;
    }

    // ---- 4. ONE undo puts BOTH back (single transaction) ---------------------
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findTransition(guardedTransitionId)->guard != QStringLiteral("canLogin") ||
        doc->findTransition(secondTransitionId)->guard != QStringLiteral("canLogin")) {
        std::fprintf(stderr, "FAIL: ONE undo did not restore both guards the cascade rewrote\n");
        return 1;
    }

    // ---- 5. an EXPRESSION row refuses to open an editor at all --------------
    kernel.send(app::events::AddContextVariableRequested{});
    QApplication::processEvents();
    const QString variableName = doc->machine().context.last().name;
    kernel.send(app::events::SetTransitionGuardRequested{
        .id = secondTransitionId, .guard = QStringLiteral("%1 > 3").arg(variableName)});
    QApplication::processEvents();
    int exprRowIndex = -1;
    for (int i = 0; i < logic->debugGuardRowCount(); ++i) {
        if (logic->debugGuardRow(i).rowClass == app::LogicRowClass::InlineExpression) {
            exprRowIndex = i;
            break;
        }
    }
    if (exprRowIndex < 0) {
        std::fprintf(stderr, "FAIL: hook-rename scenario could not produce an expression row to refuse\n");
        return 1;
    }
    if (logic->debugBeginGuardRowRename(exprRowIndex) || logic->debugRenameEditor() != nullptr) {
        std::fprintf(stderr, "FAIL: an InlineExpression row opened a rename editor -- hooks only\n");
        return 1;
    }

    if (!saveWidgetCapture(logic, "probe-hook-rename")) {
        return 1;
    }

    // ---- 6. leave the fixture as it was found ------------------------------
    for (int i = 0; i < 4; ++i) {  // expression guard, context variable, seed guard, seed transition
        kernel.send(app::events::UndoRequested{});
    }
    QApplication::processEvents();
    if (!doc->machine().context.isEmpty() || doc->findTransition(secondTransitionId) != nullptr ||
        doc->findTransition(guardedTransitionId)->guard != QStringLiteral("canLogin")) {
        std::fprintf(stderr, "FAIL: hook-rename scenario did not leave the fixture as it found it\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario hook-rename (in-place editor, Esc aborts, Enter cascades to every "
                 "referencing element, ONE undo restores all, expression rows refused)\n");
    return 0;
}

// Inspector inline guard editing: the Guard field is free text, so under test
// is the feedback path: per-keystroke validation through infra/expression.h's
// single-string entry, the completer's suggestion model, and the note for a
// bare identifier. A valid expression says nothing, since the JIT evaluates it.
// debugTypeGuard() sets the real QLineEdit's text, so assertions ride the same
// textChanged path a keystroke does.
int runGuardEditorScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr || loginPane == nullptr || loginPane->presenter() == nullptr) {
        std::fprintf(stderr, "FAIL: guard-editor scenario preconditions (doc/pane/presenter) missing\n");
        return 1;
    }
    app::InspectorPanel* inspector = window.debugInspector();
    if (inspector == nullptr) {
        std::fprintf(stderr, "FAIL: guard-editor scenario has no Inspector panel\n");
        return 1;
    }

    // Read the fixture's guarded transition back by value rather than assuming
    // an id.
    quint64 guardedTransitionId = 0;
    for (const app::Transition& transition : doc->machine().transitions) {
        if (transition.guard.trimmed() == QStringLiteral("canLogin")) {
            guardedTransitionId = transition.id;
            break;
        }
    }
    if (guardedTransitionId == 0) {
        std::fprintf(stderr, "FAIL: guard-editor scenario precondition -- login-flow's canLogin guard is missing\n");
        return 1;
    }
    const QString originalGuard = doc->findTransition(guardedTransitionId)->guard;

    window.debugFocusView(loginPane);
    loginPane->presenter()->selectTransition(guardedTransitionId);
    QApplication::processEvents();

    // ---- 1. a bare identifier reads as a NAMED HOOK, with the escape named --
    inspector->debugTypeGuard(QStringLiteral("canLogin"));
    QApplication::processEvents();
    const QString hookNote = inspector->debugGuardFeedbackText();
    if (hookNote.isEmpty() || !hookNote.contains(QStringLiteral("named hook")) ||
        !hookNote.contains(QStringLiteral("canLogin == true")) || inspector->debugGuardFeedbackIsError()) {
        std::fprintf(stderr,
                      "FAIL: a bare-identifier guard did not read as a named hook naming its == true escape "
                      "(got \"%s\", error=%d)\n",
                      qUtf8Printable(hookNote), static_cast<int>(inspector->debugGuardFeedbackIsError()));
        return 1;
    }

    // ---- 2. an UNKNOWN identifier in an expression is an Error ---------------
    inspector->debugTypeGuard(QStringLiteral("missing > 3"));
    QApplication::processEvents();
    if (!inspector->debugGuardFeedbackIsError() || inspector->debugGuardFeedbackText().isEmpty()) {
        std::fprintf(stderr, "FAIL: an expression over an unknown identifier did not report an Error\n");
        return 1;
    }

    // ---- 3. a PARSE failure is an Error, and reports a position -------------
    inspector->debugTypeGuard(QStringLiteral("count >"));
    QApplication::processEvents();
    const QString parseError = inspector->debugGuardFeedbackText();
    if (!inspector->debugGuardFeedbackIsError() || !parseError.startsWith(QStringLiteral("at "))) {
        std::fprintf(stderr, "FAIL: an unparseable guard did not report a positioned Error (got \"%s\")\n",
                      qUtf8Printable(parseError));
        return 1;
    }

    // ---- 4. a well-typed expression says NOTHING (the JIT evaluates it) ------
    kernel.send(app::events::AddContextVariableRequested{});
    QApplication::processEvents();
    const app::Machine& withVar = doc->machine();
    if (withVar.context.isEmpty()) {
        std::fprintf(stderr, "FAIL: guard-editor scenario could not mint a context variable to type against\n");
        return 1;
    }
    const QString variableName = withVar.context.last().name;
    inspector->debugTypeGuard(QStringLiteral("%1 > 3").arg(variableName));
    QApplication::processEvents();
    const QString exprNote = inspector->debugGuardFeedbackText();
    if (inspector->debugGuardFeedbackIsError() || !exprNote.isEmpty()) {
        std::fprintf(stderr,
                      "FAIL: a well-typed expression guard still showed feedback (got \"%s\", error=%d)\n",
                      qUtf8Printable(exprNote), static_cast<int>(inspector->debugGuardFeedbackIsError()));
        return 1;
    }

    // ---- 5. a blank guard says NOTHING -- the common case must not nag ------
    inspector->debugTypeGuard(QString());
    QApplication::processEvents();
    if (!inspector->debugGuardFeedbackText().isEmpty()) {
        std::fprintf(stderr, "FAIL: a blank guard still showed feedback (\"%s\")\n",
                      qUtf8Printable(inspector->debugGuardFeedbackText()));
        return 1;
    }

    // ---- 6. the completer's model carries guards used elsewhere -------------
    // Reselecting refreshes the transition tab, where suggestions are rebuilt
    // from infra/logic_inventory.h.
    loginPane->presenter()->selectTransition(guardedTransitionId);
    QApplication::processEvents();
    const QStringList suggestions = inspector->debugGuardSuggestions();
    if (!suggestions.contains(QStringLiteral("canLogin"))) {
        std::fprintf(stderr, "FAIL: the guard completer's model does not carry the machine's own canLogin guard\n");
        return 1;
    }

    if (!saveWidgetCapture(inspector, "probe-guard-editor")) {
        return 1;
    }

    // ---- 7. restore the fixture: the context variable this scenario minted
    //         must not leak into later scenarios' machine ---------------------
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (!doc->machine().context.isEmpty() ||
        doc->findTransition(guardedTransitionId)->guard != originalGuard) {
        std::fprintf(stderr, "FAIL: guard-editor scenario did not leave the fixture as it found it\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario guard-editor (hook note + typed/parse errors with position + "
                 "valid expression stays silent + blank stays silent + completer suggestions)\n");
    return 0;
}

int runExpressionEditorDialogScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr || loginPane == nullptr || loginPane->presenter() == nullptr) {
        std::fprintf(stderr, "FAIL: expression-editor-dialog scenario preconditions (doc/pane/presenter) missing\n");
        return 1;
    }
    app::InspectorPanel* inspector = window.debugInspector();
    app::InspectorAdapter* inspectorAdapter = window.debugInspectorAdapter();
    if (inspector == nullptr || inspectorAdapter == nullptr) {
        std::fprintf(stderr, "FAIL: expression-editor-dialog scenario missing inspector or adapter\n");
        return 1;
    }

    // Read the login-flow machine's canLogin transition
    quint64 guardedTransitionId = 0;
    for (const app::Transition& transition : doc->machine().transitions) {
        if (transition.guard.trimmed() == QStringLiteral("canLogin")) {
            guardedTransitionId = transition.id;
            break;
        }
    }
    if (guardedTransitionId == 0) {
        std::fprintf(stderr, "FAIL: expression-editor-dialog precondition -- canLogin transition missing\n");
        return 1;
    }
    const QString originalGuard = doc->findTransition(guardedTransitionId)->guard;
    const QString originalAction = doc->findTransition(guardedTransitionId)->action;

    // ---- 1. Direct unit verification of ExpressionEditorDialog (Guard mode) ----
    QVector<app::ContextVariable> testVars{
        app::ContextVariable{.id = 1, .name = QStringLiteral("loginAttempts"), .type = app::ContextType::Int},
        app::ContextVariable{.id = 2, .name = QStringLiteral("isBlocked"), .type = app::ContextType::Bool}
    };
    app::ExpressionEditorDialog guardDialog(app::ExpressionEditorDialog::Kind::Guard,
                                           QStringLiteral("canLogin"),
                                           testVars,
                                           {},
                                           {},
                                           &window);
    if (!guardDialog.isValid() || !guardDialog.feedbackMessage().contains(QStringLiteral("Named guard hook"))) {
        std::fprintf(stderr, "FAIL: ExpressionEditorDialog did not recognise initial named hook 'canLogin'\n");
        return 1;
    }

    // Insert variable chip
    guardDialog.setExpressionText(QString());
    guardDialog.clickVariableChip(QStringLiteral("loginAttempts"));
    if (guardDialog.expressionText() != QStringLiteral("loginAttempts")) {
        std::fprintf(stderr, "FAIL: ExpressionEditorDialog clickVariableChip did not insert variable name (got '%s')\n",
                      qUtf8Printable(guardDialog.expressionText()));
        return 1;
    }

    // Insert operator button
    guardDialog.clickOperatorButton(QStringLiteral("<"));
    guardDialog.setExpressionText(QStringLiteral("loginAttempts < 5 && isBlocked == false"));
    if (!guardDialog.isValid() || !guardDialog.feedbackMessage().contains(QStringLiteral("Valid boolean expression"))) {
        std::fprintf(stderr, "FAIL: ExpressionEditorDialog valid expression rejected (feedback: '%s')\n",
                      qUtf8Printable(guardDialog.feedbackMessage()));
        return 1;
    }

    // Syntax error
    guardDialog.setExpressionText(QStringLiteral("loginAttempts <"));
    if (guardDialog.isValid()) {
        std::fprintf(stderr, "FAIL: ExpressionEditorDialog accepted malformed guard expression\n");
        return 1;
    }

    // ---- 2. Direct unit verification of ExpressionEditorDialog (Action mode) ---
    app::ExpressionEditorDialog actionDialog(app::ExpressionEditorDialog::Kind::Action,
                                            QStringLiteral("logAction"),
                                            testVars,
                                            {},
                                            {},
                                            &window);
    if (!actionDialog.isValid() || !actionDialog.feedbackMessage().contains(QStringLiteral("Named action hook"))) {
        std::fprintf(stderr, "FAIL: ExpressionEditorDialog Action mode did not recognise named hook\n");
        return 1;
    }

    // Test assign form
    actionDialog.setExpressionText(QStringLiteral("loginAttempts = loginAttempts + 1"));
    if (!actionDialog.isValid() || !actionDialog.feedbackMessage().contains(QStringLiteral("Assignment"))) {
        std::fprintf(stderr, "FAIL: ExpressionEditorDialog valid assignment rejected (feedback: '%s')\n",
                      qUtf8Printable(actionDialog.feedbackMessage()));
        return 1;
    }

    // Test raise form
    actionDialog.setExpressionText(QStringLiteral("raise(LOCKED)"));
    if (!actionDialog.isValid() || !actionDialog.feedbackMessage().contains(QStringLiteral("raise event 'LOCKED'"))) {
        std::fprintf(stderr, "FAIL: ExpressionEditorDialog valid raise rejected (feedback: '%s')\n",
                      qUtf8Printable(actionDialog.feedbackMessage()));
        return 1;
    }

    // Test undeclared context variable target
    actionDialog.setExpressionText(QStringLiteral("unknownVar = 1"));
    if (actionDialog.isValid()) {
        std::fprintf(stderr, "FAIL: ExpressionEditorDialog accepted assignment to undeclared variable\n");
        return 1;
    }

    // Capture dialog look
    guardDialog.setExpressionText(QStringLiteral("loginAttempts < 5 && isBlocked == false"));
    guardDialog.show();
    QApplication::processEvents();
    if (!saveWidgetCapture(&guardDialog, "probe-expression-editor-dialog")) {
        return 1;
    }
    guardDialog.close();

    // ---- 3. Verification of Inspector integration via expand buttons & runner -
    window.debugFocusView(loginPane);
    loginPane->presenter()->selectTransition(guardedTransitionId);
    QApplication::processEvents();

    bool runnerInvokedGuard = false;
    bool runnerInvokedAction = false;

    inspectorAdapter->setExpressionDialogRunner([&](app::ExpressionEditorDialog::Kind kind,
                                                    const QString& /*current*/,
                                                    const QVector<app::ContextVariable>& /*vars*/,
                                                    const QVector<app::StructDefinition>& /*types*/,
                                                    const app::expr::PayloadBinding& /*payload*/,
                                                    QWidget* /*parent*/) -> std::optional<QString> {
        if (kind == app::ExpressionEditorDialog::Kind::Guard) {
            runnerInvokedGuard = true;
            return QStringLiteral("loginAttempts < 3");
        } else {
            runnerInvokedAction = true;
            return QStringLiteral("loginAttempts = loginAttempts + 1");
        }
    });

    // Trigger guard expand button
    inspector->debugClickGuardExpand();
    QApplication::processEvents();

    if (!runnerInvokedGuard) {
        std::fprintf(stderr, "FAIL: inspector debugClickGuardExpand did not invoke expression dialog runner\n");
        return 1;
    }
    if (doc->findTransition(guardedTransitionId)->guard != QStringLiteral("loginAttempts < 3")) {
        std::fprintf(stderr, "FAIL: guard was not updated by expression dialog commit (got '%s')\n",
                      qUtf8Printable(doc->findTransition(guardedTransitionId)->guard));
        return 1;
    }

    // Trigger action expand button
    inspector->debugClickActionExpand();
    QApplication::processEvents();

    if (!runnerInvokedAction) {
        std::fprintf(stderr, "FAIL: inspector debugClickActionExpand did not invoke expression dialog runner\n");
        return 1;
    }
    if (doc->findTransition(guardedTransitionId)->action != QStringLiteral("loginAttempts = loginAttempts + 1")) {
        std::fprintf(stderr, "FAIL: action was not updated by expression dialog commit (got '%s')\n",
                      qUtf8Printable(doc->findTransition(guardedTransitionId)->action));
        return 1;
    }

    // Reset runner to default
    inspectorAdapter->setExpressionDialogRunner(nullptr);

    // ---- 4. Verify Undo restores original guard and action ------------------
    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findTransition(guardedTransitionId)->action != originalAction) {
        std::fprintf(stderr, "FAIL: undo did not restore action to original\n");
        return 1;
    }

    kernel.send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->findTransition(guardedTransitionId)->guard != originalGuard) {
        std::fprintf(stderr, "FAIL: undo did not restore guard to original\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario expression-editor-dialog (dialog UI, variable chips, operator snippets, "
                 "live syntax/type validation for guard and action, inspector expand buttons, undo/redo)\n");
    return 0;
}

int runContextSectionScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto doc = window.loginFlowSession()->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::LogicPanel* logic = window.debugLogicPanel();
    if (doc == nullptr || logic == nullptr) {
        std::fprintf(stderr, "FAIL: context-section scenario preconditions (doc/panel) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    // ---- precondition: the login-flow machine starts with no context vars --
    if (!doc->machine().context.isEmpty() || logic->debugContextRowCount() != 0 ||
        !logic->debugContextPlaceholderVisible()) {
        std::fprintf(stderr, "FAIL: context-section scenario precondition -- login flow already has context rows\n");
        return 1;
    }

    // ---- 1) "+" adds a row; document gains var<id>/Int/"0", table shows it --
    logic->debugClickContextAdd();
    QApplication::processEvents();
    if (doc->machine().context.size() != 1 || logic->debugContextRowCount() != 1 ||
        logic->debugContextPlaceholderVisible()) {  // placeholder must now be hidden
        std::fprintf(stderr, "FAIL: the '+' affordance did not add exactly one context variable\n");
        return 1;
    }
    const app::ContextVariable firstAdd = doc->machine().context.at(0);
    const quint64 idA = firstAdd.id;
    if (firstAdd.name != QStringLiteral("var%1").arg(idA) || firstAdd.type != app::ContextType::Int ||
        firstAdd.initialValue != QStringLiteral("0")) {
        std::fprintf(stderr, "FAIL: the minted context variable is not var<id>/Int/\"0\" (got name=%s)\n",
                     qUtf8Printable(firstAdd.name));
        return 1;
    }
    if (logic->debugContextRow(0) != firstAdd) {
        std::fprintf(stderr, "FAIL: the Context table's row 0 does not mirror the just-added document variable\n");
        return 1;
    }

    // A second variable, so the undo assertion below can tell "restored at its
    // original index" from "appended at the end".
    logic->debugClickContextAdd();
    QApplication::processEvents();
    if (doc->machine().context.size() != 2 || logic->debugContextRowCount() != 2) {
        std::fprintf(stderr, "FAIL: the '+' affordance's second click did not add a second context variable\n");
        return 1;
    }
    const app::ContextVariable secondAdd = doc->machine().context.at(1);
    const quint64 idB = secondAdd.id;
    if (logic->debugContextRow(0) != firstAdd || logic->debugContextRow(1) != secondAdd) {
        std::fprintf(stderr, "FAIL: the Context table's two rows are not in the document's own order\n");
        return 1;
    }

    // ---- 2) editing the name commits on editing-finished, document reflects it --
    logic->debugCommitContextName(0, QStringLiteral("isLoggedIn"));
    QApplication::processEvents();
    if (doc->findContextVariable(idA)->name != QStringLiteral("isLoggedIn") ||
        logic->debugContextRow(0).name != QStringLiteral("isLoggedIn")) {
        std::fprintf(stderr, "FAIL: committing the name field did not rename the context variable\n");
        return 1;
    }

    // ---- 3) the type combo commits on activation, document reflects it -----
    logic->debugSetContextType(0, app::ContextType::Bool);
    QApplication::processEvents();
    if (doc->findContextVariable(idA)->type != app::ContextType::Bool ||
        logic->debugContextRow(0).type != app::ContextType::Bool) {
        std::fprintf(stderr, "FAIL: activating the type combo did not retype the context variable\n");
        return 1;
    }

    // ---- 4) editing the initial value commits, document reflects it --------
    logic->debugCommitContextInitialValue(0, QStringLiteral("true"));
    QApplication::processEvents();
    if (doc->findContextVariable(idA)->initialValue != QStringLiteral("true") ||
        logic->debugContextRow(0).initialValue != QStringLiteral("true")) {
        std::fprintf(stderr, "FAIL: committing the initial-value field did not re-initialise the context variable\n");
        return 1;
    }
    if (!saveWidgetCapture(logic->debugSectionWidget(0), "probe-context-section")) {
        return 1;
    }

    // ---- 4b) row-truncation regression: the name column must show a fairly
    //          long identifier ("sampleCount") in full at the default panel
    //          width. Compares the field's rendered width against the
    //          fontMetrics of its own text, so it holds under the offscreen
    //          platform's font fallback. Renamed back to "isLoggedIn"
    //          immediately after, for steps 5-7. -----------------------------
    logic->debugCommitContextName(0, QStringLiteral("sampleCount"));
    QApplication::processEvents();
    QLineEdit* sampleNameEdit = logic->debugContextNameEdit(0);
    if (sampleNameEdit == nullptr) {
        std::fprintf(stderr, "FAIL: Context row 0 has no name QLineEdit to measure\n");
        return 1;
    }
    const int neededWidth = sampleNameEdit->fontMetrics().horizontalAdvance(sampleNameEdit->text());
    if (sampleNameEdit->width() < neededWidth) {
        std::fprintf(stderr,
                      "FAIL: Context name field is too narrow to show \"sampleCount\" without scrolling "
                      "(width=%d, needed=%d)\n",
                      sampleNameEdit->width(), neededWidth);
        return 1;
    }
    logic->debugCommitContextName(0, QStringLiteral("isLoggedIn"));
    QApplication::processEvents();

    // ---- 5) the delete affordance removes the row and the variable ---------
    const app::ContextVariable editedFirst = doc->machine().context.at(0);  // isLoggedIn/Bool/"true", the undo target below
    logic->debugClickContextDelete(0);
    QApplication::processEvents();
    if (doc->findContextVariable(idA) != nullptr || doc->machine().context.size() != 1 ||
        logic->debugContextRowCount() != 1 || logic->debugContextRow(0) != secondAdd) {
        std::fprintf(stderr, "FAIL: the delete affordance did not remove the first variable's row and document entry\n");
        return 1;
    }

    // ---- 6) ONE undo restores it AT ITS ORIGINAL INDEX (fact-driven proof) --
    window.loginFlowSession()->kernel().send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->machine().context.size() != 2 || doc->machine().context.at(0).id != idA ||
        doc->machine().context.at(1).id != idB) {
        std::fprintf(stderr, "FAIL: ONE undo did not restore the deleted variable at document index 0\n");
        return 1;
    }
    if (logic->debugContextRowCount() != 2 || logic->debugContextRow(0) != editedFirst ||
        logic->debugContextRow(1) != secondAdd) {
        std::fprintf(stderr,
                     "FAIL: ONE undo restored the document but the Context table did not rebuild from the fact "
                     "(row 0 should be the restored isLoggedIn/Bool/\"true\" variable)\n");
        return 1;
    }

    // ---- 7) deleting every variable returns the empty-state placeholder ----
    logic->debugClickContextDelete(0);
    QApplication::processEvents();
    logic->debugClickContextDelete(0);  // the sole remaining row is now always index 0
    QApplication::processEvents();
    if (!doc->machine().context.isEmpty() || logic->debugContextRowCount() != 0 ||
        !logic->debugContextPlaceholderVisible()) {
        std::fprintf(stderr, "FAIL: deleting every context variable did not restore the empty-state placeholder\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario context-section (add, rename, retype, re-initialise, delete, one undo "
                "restores original index, empty state)\n");
    return 0;
}

int runTypesSectionScenario(app::MainWindow& window, app::EditorView* loginPane) {
    auto doc = window.loginFlowSession()->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    app::LogicPanel* logic = window.debugLogicPanel();
    app::InspectorPanel* inspector = window.debugInspector();
    if (doc == nullptr || logic == nullptr || inspector == nullptr) {
        std::fprintf(stderr, "FAIL: types-section scenario preconditions missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    QApplication::processEvents();

    // 1) Preconditions: no types, placeholder visible
    if (!doc->machine().types.isEmpty() || logic->debugTypesRowCount() != 0 ||
        !logic->debugTypesPlaceholderVisible()) {
        std::fprintf(stderr, "FAIL: types-section scenario precondition -- types table already has rows\n");
        return 1;
    }

    // 2) External headers commit
    logic->debugCommitExternalHeaders(QStringLiteral("\"custom_can.h\", <vector>"));
    QApplication::processEvents();
    if (doc->machine().externalHeaders.size() != 2 ||
        doc->machine().externalHeaders.at(0) != QStringLiteral("\"custom_can.h\"") ||
        doc->machine().externalHeaders.at(1) != QStringLiteral("<vector>")) {
        std::fprintf(stderr, "FAIL: committing external headers did not update machine.externalHeaders\n");
        return 1;
    }

    // 3) "+" adds a struct definition
    logic->debugClickTypesAdd();
    QApplication::processEvents();
    if (doc->machine().types.size() != 1 || logic->debugTypesRowCount() != 1 ||
        logic->debugTypesPlaceholderVisible()) {
        std::fprintf(stderr, "FAIL: clicking '+' in Types section did not add a struct definition\n");
        return 1;
    }
    const quint64 structId = doc->machine().types.at(0).id;
    if (logic->debugTypesRowName(0) != QStringLiteral("Struct1")) {
        std::fprintf(stderr, "FAIL: added struct row has wrong name (got %s, expected Struct1)\n",
                     qUtf8Printable(logic->debugTypesRowName(0)));
        return 1;
    }

    // 4) Edit struct definition via SetStructDefinitionRequested
    app::StructDefinition updatedDef = doc->machine().types.at(0);
    updatedDef.name = QStringLiteral("CanMessage");
    app::StructField f1;
    f1.name = QStringLiteral("id");
    f1.type = app::FieldType::Int;
    f1.initialValue = QStringLiteral("0x100");
    updatedDef.fields.append(f1);
    window.loginFlowSession()->kernel().send(app::events::SetStructDefinitionRequested{
        .id = structId,
        .definition = updatedDef
    });
    QApplication::processEvents();
    if (doc->machine().types.at(0).name != QStringLiteral("CanMessage") ||
        doc->machine().types.at(0).fields.size() != 1 ||
        logic->debugTypesRowName(0) != QStringLiteral("CanMessage")) {
        std::fprintf(stderr, "FAIL: updating struct definition did not reflect in document and logic panel\n");
        return 1;
    }

    // 5) Transition payload type authoring
    // Login flow has transition from LoggedOut (1) to Authenticating (2) on event Login (id 5)
    window.loginFlowSession()->kernel().send(app::events::SetTransitionPayloadTypeRequested{
        .id = 5,
        .payloadType = QStringLiteral("CanMessage")
    });
    QApplication::processEvents();
    const app::Transition* trans5 = doc->findTransition(5);
    if (trans5 == nullptr || trans5->payloadType != QStringLiteral("CanMessage")) {
        std::fprintf(stderr, "FAIL: SetTransitionPayloadTypeRequested did not update transition payloadType\n");
        return 1;
    }

    // 6) Context variable struct binding
    logic->debugClickContextAdd();
    QApplication::processEvents();
    const quint64 ctxVarId = doc->machine().context.last().id;
    logic->debugSetContextCustomTypeName(logic->debugContextRowCount() - 1, QStringLiteral("CanMessage"));
    QApplication::processEvents();
    if (doc->findContextVariable(ctxVarId)->type != app::ContextType::Object ||
        doc->findContextVariable(ctxVarId)->customTypeName != QStringLiteral("CanMessage")) {
        std::fprintf(stderr, "FAIL: activating custom struct type on context variable did not set Object and customTypeName\n");
        return 1;
    }

    // 7) Simulation structured payload injection via debug levers
    window.loginFlowSession()->kernel().send(app::events::ModeChanged{.mode = app::events::Mode::Simulate});
    QApplication::processEvents();
    inspector->debugSetEventPayload(QStringLiteral("Login"), QStringLiteral("{\"id\": 512}"));
    if (inspector->debugEventPayloadText(QStringLiteral("Login")) != QStringLiteral("{\"id\": 512}")) {
        std::fprintf(stderr, "FAIL: inspector debugSetEventPayload did not update line edit text\n");
        return 1;
    }
    inspector->debugClickSendEvent(QStringLiteral("Login"));
    QApplication::processEvents();
    window.loginFlowSession()->kernel().send(app::events::ModeChanged{.mode = app::events::Mode::Design});
    QApplication::processEvents();

    // 8) Delete struct definition and test undo
    logic->debugClickTypesDelete(0);
    QApplication::processEvents();
    if (!doc->machine().types.isEmpty() || logic->debugTypesRowCount() != 0 ||
        !logic->debugTypesPlaceholderVisible()) {
        std::fprintf(stderr, "FAIL: clicking delete did not remove struct definition\n");
        return 1;
    }

    // Undo delete
    window.loginFlowSession()->kernel().send(app::events::UndoRequested{});
    QApplication::processEvents();
    if (doc->machine().types.size() != 1 || doc->machine().types.at(0).name != QStringLiteral("CanMessage") ||
        logic->debugTypesRowCount() != 1 || logic->debugTypesRowName(0) != QStringLiteral("CanMessage")) {
        std::fprintf(stderr, "FAIL: Undo did not restore deleted struct definition\n");
        return 1;
    }

    // Cleanup: delete struct and context var, reset external headers, reset transition payloadType
    logic->debugClickTypesDelete(0);
    logic->debugClickContextDelete(logic->debugContextRowCount() - 1);
    logic->debugCommitExternalHeaders(QStringLiteral(""));
    window.loginFlowSession()->kernel().send(app::events::SetTransitionPayloadTypeRequested{
        .id = 5,
        .payloadType = QStringLiteral("")
    });
    QApplication::processEvents();

    std::printf("PASS: gui-probe scenario types-section (external headers, add struct, edit struct, transition payload type, context struct binding, sim payload injection, delete + undo)\n");
    return 0;
}

// Scenario "logic-panel": the Logic side-bar shell (placement, four section
// headers, Appearance toggle, collapse/expand) and focus rebinding, including
// the "no machine focused" branch. Reaching it drains every group and
// EditorGroup::closeTab() deletes the view, so `loginPane` does not survive; the
// scenario reopens Login Flow into a fresh EditorView. Each header's top hairline
// must reach the screen at any DPI scale (try QT_SCALE_FACTOR=1.5).
int runSectionHeaderRhythmScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running section-header rhythm scenario...\n");
    app::InspectorPanel* inspector = window.debugInspector();
    QGraphicsScene* scene = loginPane != nullptr && loginPane->canvasView() != nullptr
                                ? loginPane->canvasView()->scene()
                                : nullptr;
    if (inspector == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: section-header rhythm scenario preconditions (inspector/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();  // no selection -> the Machine tab, where every section header lives
    QApplication::processEvents();

    using app::inspector_detail::SectionHeaderButton;
    std::vector<SectionHeaderButton*> headers;
    // No Q_OBJECT on SectionHeaderButton, so findChildren<> can only name its base.
    for (QToolButton* button : inspector->findChildren<QToolButton*>()) {
        auto* header = dynamic_cast<SectionHeaderButton*>(button);
        if (header != nullptr && header->isVisible()) {
            headers.push_back(header);
        }
    }
    // Machine tab (Simulation/Invocations/Events/Guard Results) + its Logic
    // section (Context/Guards/Actions/Actors/Types).
    constexpr std::size_t kExpectedHeaders = 9;
    if (headers.size() != kExpectedHeaders) {
        std::fprintf(stderr, "FAIL: section-header rhythm scenario found %zu visible section headers, want %zu\n",
                     headers.size(), kExpectedHeaders);
        return 1;
    }
    auto topIn = [inspector](const QWidget* w) { return w->mapTo(inspector, QPoint(0, 0)).y(); };
    std::sort(headers.begin(), headers.end(),
              [&topIn](const QWidget* a, const QWidget* b) { return topIn(a) < topIn(b); });

    // Collapse everything, so each header's band runs from its own hairline
    // to the next header's.
    std::vector<bool> wasExpanded;
    for (SectionHeaderButton* header : headers) {
        wasExpanded.push_back(header->isChecked());
        header->setChecked(false);
    }
    QApplication::processEvents();

    int exitCode = 0;
    QWidget* topLevel = inspector->window();
    const bool reenable = topLevel != nullptr && !topLevel->updatesEnabled();
    if (reenable) {  // grab() paints through the stage's disabled-updates bracket
        topLevel->setUpdatesEnabled(true);
        QApplication::processEvents();
    }
    const QColor hairline(QString::fromLatin1(app::design::kOutlineStrong));
    auto isHairline = [&hairline](const QColor& c) {
        return std::abs(c.red() - hairline.red()) <= 2 && std::abs(c.green() - hairline.green()) <= 2 &&
               std::abs(c.blue() - hairline.blue()) <= 2;
    };
    // Grab the whole panel once, never header->grab(): a header grabbed alone
    // paints at device origin 0, while on screen it sits at a fractional device
    // row at 125%/150%. Only the panel-level render reproduces that.
    const QImage panel = inspector->grab().toImage();
    const qreal dpr = panel.devicePixelRatio();
    int trailingActions = 0;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        SectionHeaderButton* header = headers[i];
        const QString title = header->property("sectionBaseTitle").toString();

        // Hairline: some device row inside the header's top kHairline band,
        // sampled at the middle and at the far right, where a sibling "+" slot
        // could cut the line short.
        const QPoint origin = header->mapTo(inspector, QPoint(0, 0));
        const int firstRow = static_cast<int>(std::floor(origin.y() * dpr));
        const int lastRow = static_cast<int>(std::ceil((origin.y() + app::design::kHairline) * dpr)) - 1;
        for (const int logicalX : {origin.x() + header->width() / 2, origin.x() + header->width() - 2}) {
            const int x = static_cast<int>(logicalX * dpr);
            bool found = false;
            for (int row = firstRow; row <= lastRow && !found; ++row) {
                found = row >= 0 && row < panel.height() && isHairline(panel.pixelColor(x, row));
            }
            if (!found) {
                std::fprintf(stderr, "FAIL: section header \"%s\" shows no hairline at x=%d, dpr %.2f (device rows "
                                     "%d..%d, first is %s, want %s)\n",
                             qUtf8Printable(title), logicalX, dpr, firstRow, lastRow,
                             qUtf8Printable(panel.pixelColor(x, std::max(firstRow, 0)).name()),
                             qUtf8Printable(hairline.name()));
                exitCode = 1;
            }
        }

        // Identity icon at device resolution: the pixmap must hold logical
        // size x dpr pixels, not a logical-size bitmap the painter upscales.
        if (header->iconSlug() != nullptr) {
            const QPixmap icon = app::icons::assetPixmap(header->iconSlug(), app::design::kSectionIconSize,
                                                         hairline, header->devicePixelRatioF());
            const int wantPixels = qRound(app::design::kSectionIconSize * header->devicePixelRatioF());
            if (icon.width() != wantPixels || qRound(icon.deviceIndependentSize().width()) !=
                                                  app::design::kSectionIconSize) {
                std::fprintf(stderr, "FAIL: section header \"%s\" icon is %dpx (logical %.1f) at dpr %.2f, want %dpx "
                                     "(logical %d)\n",
                             qUtf8Printable(title), icon.width(), icon.deviceIndependentSize().width(),
                             header->devicePixelRatioF(), wantPixels, app::design::kSectionIconSize);
                exitCode = 1;
            }
        }

        // One chevron column: every header spans to the same right edge.
        const int rightEdge = origin.x() + header->width();
        if (rightEdge != headers.front()->mapTo(inspector, QPoint(0, 0)).x() + headers.front()->width()) {
            std::fprintf(stderr, "FAIL: section header \"%s\" ends at x=%d, not in the shared chevron column\n",
                         qUtf8Printable(title), rightEdge);
            exitCode = 1;
        }

        // Trailing action: centered on the title row, clear of the chevron.
        if (QToolButton* action = header->trailingAction()) {
            ++trailingActions;
            const QRect slot = action->geometry();
            const int chevronLeft = header->width() - app::design::kSectionChevronRightInset - 4;
            if (std::abs(slot.center().y() - header->contentRect().center().y()) > 1 || slot.right() >= chevronLeft ||
                !action->isVisible()) {
                std::fprintf(stderr,
                             "FAIL: section header \"%s\" trailing action misplaced (center y %d vs %d, right %d vs "
                             "chevron %d, visible %d)\n",
                             qUtf8Printable(title), slot.center().y(), header->contentRect().center().y(),
                             slot.right(), chevronLeft, action->isVisible() ? 1 : 0);
                exitCode = 1;
            }
        }

        // Centering: the next header's top is the lower hairline.
        if (i + 1 < headers.size()) {
            const QRect content = header->contentRect();
            const double center = topIn(header) + (content.top() + content.bottom() + 1) / 2.0;
            const double above = center - (topIn(header) + app::design::kHairline);
            const double below = topIn(headers[i + 1]) - center;
            if (std::abs(above - below) > 1.0) {
                std::fprintf(stderr,
                             "FAIL: collapsed section header \"%s\" is not centered between hairlines "
                             "(%.1fpx above, %.1fpx below)\n",
                             qUtf8Printable(title), above, below);
                exitCode = 1;
            }
        }
    }
    if (reenable) {
        topLevel->setUpdatesEnabled(false);
    }
    constexpr int kExpectedTrailingActions = 2;  // Context "+", Types "+"
    if (trailingActions != kExpectedTrailingActions) {
        std::fprintf(stderr, "FAIL: section-header rhythm scenario found %d header trailing actions, want %d\n",
                     trailingActions, kExpectedTrailingActions);
        exitCode = 1;
    }
    if (!saveWidgetCapture(inspector, "probe-section-header-rhythm")) {
        exitCode = 1;
    }

    for (std::size_t i = 0; i < headers.size(); ++i) {
        headers[i]->setChecked(wasExpanded[i]);
    }
    QApplication::processEvents();

    if (exitCode == 0) {
        std::printf("PASS: gui-probe scenario section-header-rhythm (%zu headers: hairline visible in the panel render "
                    "at dpr %.2f, collapsed content centered between hairlines)\n",
                    headers.size(), dpr);
    }
    return exitCode;
}

// A selected machine frame keeps its selection when its title bar is
// right-clicked, as a state or transition does. The right press and the
// context-menu event go through the real viewport (the QGraphicsView -> scene ->
// item dispatch is what dropped it); the selection is checked again from inside
// QMenu::exec's nested loop.
int runFrameRightClickSelectionScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running frame right-click selection scenario...\n");
    app::CanvasView* view = loginPane != nullptr ? loginPane->canvasView() : nullptr;
    QGraphicsScene* scene = view != nullptr ? view->scene() : nullptr;
    if (scene == nullptr || window.loginFlowSession() == nullptr) {
        std::fprintf(stderr, "FAIL: frame right-click scenario preconditions (view/scene/session) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    window.loginFlowSession()->kernel().send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();

    app::MachineFrameItem* frame = nullptr;
    for (QGraphicsItem* item : scene->items()) {
        if (auto* candidate = qgraphicsitem_cast<app::MachineFrameItem*>(item); candidate != nullptr) {
            frame = candidate;
            break;
        }
    }
    if (frame == nullptr || !frame->isVisible()) {
        std::fprintf(stderr, "FAIL: frame right-click scenario found no visible machine frame\n");
        return 1;
    }
    // A title-bar point where the frame is the topmost item (a pill can sit
    // on the header band).
    const QRectF frameRect = frame->sceneFrameRect();
    std::optional<QPointF> titlePos;
    for (qreal x = frameRect.left() + 40.0; x < frameRect.right() - 40.0 && !titlePos; x += 24.0) {
        const QPointF candidate(x, frameRect.top() + 14.0);
        const QList<QGraphicsItem*> hits = scene->items(candidate);
        if (!hits.isEmpty() && hits.front() == frame) {
            titlePos = candidate;
        }
    }
    if (!titlePos) {
        std::fprintf(stderr, "FAIL: frame right-click scenario found no title-bar point with the frame on top\n");
        return 1;
    }

    scene->clearSelection();
    frame->setSelected(true);
    QApplication::processEvents();

    QWidget* viewport = view->viewport();
    const QPoint viewportPos = view->mapFromScene(*titlePos);
    const QPoint globalPos = viewport->mapToGlobal(viewportPos);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(viewportPos), QPointF(globalPos), Qt::RightButton,
                      Qt::RightButton, Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(viewportPos), QPointF(globalPos), Qt::RightButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &release);

    int exitCode = 0;
    if (!frame->isSelected()) {
        std::fprintf(stderr, "FAIL: right-pressing the selected machine frame's title cleared its selection\n");
        exitCode = 1;
    }

    auto selectedDuringMenu = std::make_shared<std::optional<bool>>();
    QTimer::singleShot(0, viewport, [selectedDuringMenu, frame] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            *selectedDuringMenu = frame->isSelected();
            menu->close();
        }
    });
    QContextMenuEvent contextMenu(QContextMenuEvent::Mouse, viewportPos, globalPos);
    QCoreApplication::sendEvent(viewport, &contextMenu);
    QApplication::processEvents();

    if (!selectedDuringMenu->has_value()) {
        std::fprintf(stderr, "FAIL: right-clicking the machine frame's title opened no context menu\n");
        exitCode = 1;
    } else if (!selectedDuringMenu->value()) {
        std::fprintf(stderr, "FAIL: the machine frame was not selected while its context menu was open\n");
        exitCode = 1;
    }
    scene->clearSelection();
    QApplication::processEvents();

    if (exitCode == 0) {
        std::printf("PASS: gui-probe scenario frame-right-click-selection (selected frame stays selected through a "
                    "real right press and while its context menu is open)\n");
    }
    return exitCode;
}

// A hairline separates machines in the Machines panel: at the top edge of every
// machine row after the first, never above the first. Measured in one panel
// render, the only way to reproduce fractional-scale row offsets.
int runMachinesSeparatorScenario(app::MainWindow& window) {
    std::printf("[PROBE] Running machines-separator scenario...\n");
    app::MachinesPanel* panel = window.debugMachinesPanel();
    QTreeWidget* tree = panel != nullptr ? panel->debugTree() : nullptr;
    if (tree == nullptr || !tree->isVisible() || tree->topLevelItemCount() < 2) {
        std::fprintf(stderr, "FAIL: machines-separator scenario needs a visible Machines panel with 2+ machines "
                             "(tree %d, visible %d, machines %d)\n",
                     tree != nullptr ? 1 : 0, tree != nullptr && tree->isVisible() ? 1 : 0,
                     tree != nullptr ? tree->topLevelItemCount() : 0);
        return 1;
    }
    QApplication::processEvents();

    QWidget* topLevel = panel->window();
    const bool reenable = topLevel != nullptr && !topLevel->updatesEnabled();
    if (reenable) {  // grab() paints through the stage's disabled-updates bracket
        topLevel->setUpdatesEnabled(true);
        QApplication::processEvents();
    }
    const QImage image = panel->grab().toImage();
    if (reenable) {
        topLevel->setUpdatesEnabled(false);
    }
    const qreal dpr = image.devicePixelRatio();
    const QColor hairline(QString::fromLatin1(app::design::kOutlineStrong));
    auto hairlineAtTopOf = [&](const QTreeWidgetItem* item) {
        const QRect rect = tree->visualItemRect(item);
        const QPoint origin = tree->viewport()->mapTo(panel, rect.topLeft());
        const int x = static_cast<int>((origin.x() + rect.width() / 2) * dpr);
        const int firstRow = static_cast<int>(std::floor(origin.y() * dpr));
        const int lastRow = static_cast<int>(std::ceil((origin.y() + app::design::kHairline) * dpr)) - 1;
        for (int row = firstRow; row <= lastRow; ++row) {
            if (row < 0 || row >= image.height()) {
                continue;
            }
            const QColor c = image.pixelColor(x, row);
            if (std::abs(c.red() - hairline.red()) <= 2 && std::abs(c.green() - hairline.green()) <= 2 &&
                std::abs(c.blue() - hairline.blue()) <= 2) {
                return true;
            }
        }
        return false;
    };

    int exitCode = 0;
    if (hairlineAtTopOf(tree->topLevelItem(0))) {
        std::fprintf(stderr, "FAIL: the first machine row has a separator above it\n");
        exitCode = 1;
    }
    for (int i = 1; i < tree->topLevelItemCount(); ++i) {
        if (!hairlineAtTopOf(tree->topLevelItem(i))) {
            std::fprintf(stderr, "FAIL: machine row %d (\"%s\") has no separator above it at dpr %.2f\n", i,
                         qUtf8Printable(tree->topLevelItem(i)->text(0)), dpr);
            exitCode = 1;
        }
    }
    if (!saveWidgetCapture(panel, "probe-machines-separator")) {
        exitCode = 1;
    }
    if (exitCode == 0) {
        std::printf("PASS: gui-probe scenario machines-separator (%d machines, separator above rows 2..%d at dpr "
                    "%.2f)\n",
                    tree->topLevelItemCount(), tree->topLevelItemCount(), dpr);
    }
    return exitCode;
}

// Text with no ramp fragment of its own lands on the 12px type ramp: the
// application base font, a token-styled button (kPrimaryButtonStyle pins no
// size), and a plain tree. Measured with QFontInfo, the size actually resolved.
int runTypeRampBaseFontScenario(app::MainWindow& window) {
    std::printf("[PROBE] Running type-ramp base font scenario...\n");
    int exitCode = 0;
    auto check = [&exitCode](const char* what, const QFont& font) {
        const int pixels = QFontInfo(font).pixelSize();
        if (pixels != app::design::kTypeSizePx) {
            std::fprintf(stderr, "FAIL: %s resolves to %dpx, want the %dpx type ramp\n", what, pixels,
                         app::design::kTypeSizePx);
            exitCode = 1;
        }
    };
    check("the application base font", QApplication::font());

    QPushButton* run = nullptr;
    if (app::InspectorPanel* inspector = window.debugInspector()) {
        for (QPushButton* button : inspector->findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Run")) {
                run = button;
                break;
            }
        }
    }
    if (run == nullptr) {
        std::fprintf(stderr, "FAIL: type-ramp scenario found no Inspector Run button\n");
        exitCode = 1;
    } else {
        run->ensurePolished();
        check("the Inspector Run button (kPrimaryButtonStyle)", run->font());
    }

    app::MachinesPanel* machines = window.debugMachinesPanel();
    if (machines == nullptr || machines->debugTree() == nullptr) {
        std::fprintf(stderr, "FAIL: type-ramp scenario found no Machines tree\n");
        exitCode = 1;
    } else {
        machines->debugTree()->ensurePolished();
        check("the Machines tree", machines->debugTree()->font());
    }

    if (exitCode == 0) {
        std::printf("PASS: gui-probe scenario type-ramp-base-font (app font, Run button, Machines tree all %dpx)\n",
                    app::design::kTypeSizePx);
    }
    return exitCode;
}

// At the Inspector's default width every Machine-tab transport button lies
// inside the visible viewport. The tab sits in a QScrollArea with no horizontal
// bar, so a wider content minimum is clipped silently; on failure this prints
// which top-level row carries the widest minimum.
int runInspectorControlsFitScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running inspector controls-fit scenario...\n");
    app::InspectorPanel* inspector = window.debugInspector();
    QGraphicsScene* scene = loginPane != nullptr && loginPane->canvasView() != nullptr
                                ? loginPane->canvasView()->scene()
                                : nullptr;
    if (inspector == nullptr || scene == nullptr) {
        std::fprintf(stderr, "FAIL: controls-fit scenario preconditions (inspector/scene) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    scene->clearSelection();  // the Machine tab
    QApplication::processEvents();

    int exitCode = checkInspectorControlsFit(inspector, "idle Machine tab");
    if (!saveWidgetCapture(inspector, "probe-inspector-controls-fit")) {
        exitCode = 1;
    }
    if (exitCode == 0) {
        std::printf("PASS: gui-probe scenario inspector-controls-fit (transport buttons and tab content inside the "
                    "viewport, idle; the live-card case is checked in invoke-completion)\n");
    }
    return exitCode;
}

int runLogicPanelScenario(app::MainWindow& window, app::EditorView* loginPane) {

    app::LogicPanel* logic = window.debugLogicPanel();
    if (logic == nullptr) {
        std::fprintf(stderr, "FAIL: logic-panel scenario preconditions (panel) missing\n");
        return 1;
    }
    const QString kLoginHeader = QStringLiteral("Logic ") + QChar(0x2014) + QStringLiteral(" Login Flow");
    const QString kTrafficHeader = QStringLiteral("Logic ") + QChar(0x2014) + QStringLiteral(" Traffic Light");
    const QString kNoMachineHeader = QStringLiteral("Logic ") + QChar(0x2014) + QStringLiteral(" no machine focused");
    // The chevron is painted by SectionHeaderButton, never part of text(), and
    // titles render in the caller's Title Case, so the expected titles are the
    // bare strings LogicPanel's builders pass in. Expanded/collapsed state is a
    // paint concern a text assertion cannot see.
    const QStringList kExpectedSections{QStringLiteral("Context"), QStringLiteral("Guards"),
                                        QStringLiteral("Actions"), QStringLiteral("Actors"),
                                        QStringLiteral("Types")};

    window.debugFocusView(loginPane);
    QApplication::processEvents();

    // ---- 1) sidebar placement + four section headers -----------------------
    if (!logic->isVisible() || logic->debugSectionTitles() != kExpectedSections) {
        std::fprintf(stderr, "FAIL: the Logic panel is not in the sidebar with its four section headers\n");
        return 1;
    }

    // ---- 2) header names the focused machine --------------------------------
    if (logic->debugHeaderText() != kLoginHeader) {
        std::fprintf(stderr, "FAIL: the Logic panel header did not name the focused machine (got: %s)\n",
                     qUtf8Printable(logic->debugHeaderText()));
        return 1;
    }
    if (!saveWidgetCapture(logic, "probe-logic-panel")) {
        return 1;
    }

    // ---- 3) Appearance toggle: drive the real QAction, not setVisible() -----
    QAction* toggle = window.debugLogicPanelToggleAction();
    if (toggle == nullptr) {
        std::fprintf(stderr, "FAIL: the Appearance menu's Logic-panel toggle QAction is missing\n");
        return 1;
    }
    toggle->trigger();
    QApplication::processEvents();
    if (logic->isVisible()) {
        std::fprintf(stderr, "FAIL: triggering the Appearance toggle did not hide the Logic panel\n");
        return 1;
    }
    toggle->trigger();
    QApplication::processEvents();
    if (!logic->isVisible()) {
        std::fprintf(stderr, "FAIL: triggering the Appearance toggle a second time did not re-show the Logic panel\n");
        return 1;
    }

    // ---- 4) one section collapses and re-expands its body -------------------
    logic->debugClickSectionHeader(0);  // Context
    QApplication::processEvents();
    if (logic->debugSectionBodyVisible(0)) {
        std::fprintf(stderr, "FAIL: clicking the Context section header did not collapse its body\n");
        return 1;
    }
    logic->debugClickSectionHeader(0);
    QApplication::processEvents();
    if (!logic->debugSectionBodyVisible(0)) {
        std::fprintf(stderr, "FAIL: clicking the Context section header a second time did not re-expand its body\n");
        return 1;
    }

    // ---- 5a) focus rebinding: split, bind the other machine, header follows ----
    app::EditorView* secondPane = window.debugSplitRight();
    if (secondPane == nullptr) {
        std::fprintf(stderr, "FAIL: debugSplitRight did not produce a second group\n");
        return 1;
    }
    window.debugOpenMachine(window.trafficLightSession());
    QApplication::processEvents();
    if (logic->debugHeaderText() != kTrafficHeader) {
        std::fprintf(stderr, "FAIL: the Logic header did not follow focus onto Traffic Light (got: %s)\n",
                     qUtf8Printable(logic->debugHeaderText()));
        return 1;
    }

    window.debugFocusView(loginPane);
    QApplication::processEvents();
    if (logic->debugHeaderText() != kLoginHeader) {
        std::fprintf(stderr, "FAIL: the Logic header did not follow focus back to Login Flow\n");
        return 1;
    }

    // ---- 5b) focus a pane with no bound machine: the empty-state branch -----
    // An emptied split collapses (MainWindow::handleGroupEmptied); only the last
    // group stays as an empty pane, so every group is emptied, the tour's own
    // split included.
    window.debugFocusView(secondPane);
    QApplication::processEvents();
    window.debugCloseAllTabs();
    QApplication::processEvents();

    if (logic->debugHeaderText() != kNoMachineHeader) {
        std::fprintf(stderr, "FAIL: focusing an empty pane did not read the Logic panel's no-machine state (got: %s)\n",
                     qUtf8Printable(logic->debugHeaderText()));
        return 1;
    }
    if (logic->debugSectionTitles() != kExpectedSections) {
        std::fprintf(stderr, "FAIL: the no-machine state left stale/altered section headers\n");
        return 1;
    }
    if (!saveWidgetCapture(logic, "probe-logic-panel-empty")) {
        return 1;
    }

    // ---- restore: reopen Login Flow so later scenarios have a live pane -----
    window.debugOpenMachine(window.loginFlowSession());
    QApplication::processEvents();
    if (logic->debugHeaderText() != kLoginHeader || window.focusedView() == nullptr) {
        std::fprintf(stderr, "FAIL: reopening Login Flow after the empty-state check did not restore a focused pane\n");
        return 1;
    }

    std::printf("PASS: gui-probe scenario logic-panel (sidebar placement + 4 section headers, header naming, "
                "Appearance toggle, section collapse, focus rebinding incl. no-machine state)\n");
    return 0;
}

// Scenario "undo-floor": the demo bootstrap builds its machine through
// journaled intents and then clears the UndoStore (a boundary history must not
// cross), so holding Ctrl+Z must bottom out at the built demo machine, not an
// empty canvas. Must run first: later, the suite's edits saturate the
// 100-entry cap and evict the bottom. Step 3's redo-drain is a no-op here but
// keeps the scenario order-robust.
int runUndoFloorScenario(app::MainWindow& window, app::EditorView* loginPane) {
    // ---- 0) Startup baseline isolation ---------------------------------------
    // main.cpp planted a bogus lastActiveProject in recent-probe.json before
    // MainWindow was constructed. Had auto-restore run, sessions_ would have
    // been replaced or mutated; verify exactly the two clean demo machines exist
    // and the planted lastActiveProject is still in recentProjects_.
    if (window.debugSessionCount() != 2) {
        std::fprintf(stderr,
                     "FAIL: probe baseline isolation -- expected exactly 2 demo machines, got %d (restore leak)\n",
                     window.debugSessionCount());
        return 1;
    }
    if (window.debugRecentProjects().lastActiveProject() != QStringLiteral("C:/bogus/planted_probe_project.sdp")) {
        std::fprintf(stderr, "FAIL: probe baseline isolation -- planted lastActiveProject missing from recentProjects_\n");
        return 1;
    }

    ordo::core::Kernel& kernel = window.loginFlowSession()->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto undo = kernel.agentAs<app::UndoStore>(app::UndoStore::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (doc == nullptr || undo == nullptr || sim == nullptr || loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: undo-floor scenario preconditions (doc/undo/sim/pane) missing\n");
        return 1;
    }
    // UndoCommand/RedoCommand silently no-op while simulating, so a Simulate
    // leak from an earlier scenario would spin both loops below to their cap
    // with a misleading "never exhausts" diagnosis; fail on it by name.
    if (sim->mode() != app::events::Mode::Design) {
        std::fprintf(stderr, "FAIL: undo-floor precondition -- login-flow session is not in Design mode\n");
        return 1;
    }

    // ---- 1) exhaust undo: walk the journal all the way down to its floor ----
    // 300 comfortably clears UndoStore's 100-entry cap: hitting it means
    // UndoRequested stopped consuming entries.
    constexpr int kMaxHistorySteps = 300;
    int undoSteps = 0;
    while (undo->canUndo() && undoSteps < kMaxHistorySteps) {
        kernel.send(app::events::UndoRequested{});
        QApplication::processEvents();
        ++undoSteps;
    }
    if (undo->canUndo()) {
        std::fprintf(stderr, "FAIL: undo-floor undo loop hit the %d-step cap without exhausting history\n",
                     kMaxHistorySteps);
        return 1;
    }

    // ---- 2) the floor must be the bootstrap baseline: demo machine intact ---
    struct ExpectedState {
        quint64 id;
        QString name;
    };
    const ExpectedState kExpectedStates[] = {{1, QStringLiteral("LoggedOut")},
                                             {2, QStringLiteral("Authenticating")},
                                             {3, QStringLiteral("LoggedIn")},
                                             {4, QStringLiteral("Error")}};
    for (const ExpectedState& expected : kExpectedStates) {
        const app::State* state = doc->findState(expected.id);
        if (state == nullptr) {
            std::fprintf(stderr, "FAIL: undo-floor -- demo state %llu (%s) vanished at the undo floor\n",
                         expected.id, qUtf8Printable(expected.name));
            return 1;
        }
        if (state->name != expected.name) {
            std::fprintf(stderr, "FAIL: undo-floor -- demo state %llu is named '%s' at the undo floor, want '%s'\n",
                         expected.id, qUtf8Printable(state->name), qUtf8Printable(expected.name));
            return 1;
        }
    }
    struct ExpectedTransition {
        quint64 id;
        QString event;
    };
    const ExpectedTransition kExpectedTransitions[] = {{5, QStringLiteral("Login")},
                                                       {6, QStringLiteral("Success")},
                                                       {7, QStringLiteral("Failure")},
                                                       {8, QStringLiteral("Reset")}};
    for (const ExpectedTransition& expected : kExpectedTransitions) {
        const app::Transition* transition = doc->findTransition(expected.id);
        if (transition == nullptr) {
            std::fprintf(stderr, "FAIL: undo-floor -- demo transition %llu (%s) vanished at the undo floor\n",
                         expected.id, qUtf8Printable(expected.event));
            return 1;
        }
        if (transition->event != expected.event) {
            std::fprintf(stderr,
                         "FAIL: undo-floor -- demo transition %llu fires '%s' at the undo floor, want '%s'\n",
                         expected.id, qUtf8Printable(transition->event), qUtf8Printable(expected.event));
            return 1;
        }
    }
    if (doc->machine().initialStateId != 1) {
        std::fprintf(stderr, "FAIL: undo-floor -- initial state is %llu at the undo floor, want 1 (LoggedOut)\n",
                     doc->machine().initialStateId);
        return 1;
    }

    // ---- 3) restore: redo everything back so the session leaves as it came --
    int redoSteps = 0;
    while (undo->canRedo() && redoSteps < kMaxHistorySteps) {
        kernel.send(app::events::RedoRequested{});
        QApplication::processEvents();
        ++redoSteps;
    }
    if (undo->canRedo()) {
        std::fprintf(stderr, "FAIL: undo-floor redo loop hit the %d-step cap without exhausting history\n",
                     kMaxHistorySteps);
        return 1;
    }

    std::printf("PASS: gui-probe scenario undo-floor (undo exhausted at the bootstrap baseline, demo machine intact, redo restored)\n");
    return 0;
}

// Scenario "generate-entry-points": the rail's Generate C++ action sits between
// Fit and the expanding spacer, and the File menu's second entry point reuses
// the same QAction (activity_rail.h's shared-QAction contract) with an F9
// shortcut, so enabled/checked state cannot drift. Reads the rail's
// QToolBar::actions() list, never a pixel position (the rail is icon-only).
// loginPane is unused: this inspects shell chrome, not document state.
int runGenerateEntryPointsScenario(app::MainWindow& window, app::EditorView* /*loginPane*/) {
    std::printf("[PROBE] Running Generate C++ entry-points scenario...\n");

    auto* rail = window.findChild<app::ActivityRail*>(QStringLiteral("activityRail"));
    if (rail == nullptr) {
        std::fprintf(stderr, "FAIL: activity rail not found\n");
        return 1;
    }

    QAction* fitAction = rail->fitAction();
    QAction* generateAction = rail->generateAction();
    const auto railActions = rail->actions();
    const int fitIndex = railActions.indexOf(fitAction);
    const int generateIndex = railActions.indexOf(generateAction);
    if (fitIndex < 0 || generateIndex < 0) {
        std::fprintf(stderr, "FAIL: Fit or Generate C++ action missing from the rail\n");
        return 1;
    }
    if (generateIndex != fitIndex + 2 || !railActions[fitIndex + 1]->isSeparator()) {
        std::fprintf(stderr, "FAIL: Generate C++ does not follow Fit + separator on the rail\n");
        return 1;
    }
    // The expanding spacer is the rail's last action (a QWidgetAction, not a
    // separator); Generate C++ must sit immediately before it.
    if (generateIndex != railActions.size() - 2 || railActions.constLast()->isSeparator()) {
        std::fprintf(stderr, "FAIL: Generate C++ does not immediately precede the expanding-spacer action\n");
        return 1;
    }

    if (generateAction->shortcut() != QKeySequence(Qt::Key_F9)) {
        std::fprintf(stderr, "FAIL: Generate C++ action's shortcut is not F9\n");
        return 1;
    }

    QMenu* fileMenu = nullptr;
    for (QAction* topLevel : window.menuBar()->actions()) {
        if (topLevel->text() == QStringLiteral("&File")) {
            fileMenu = topLevel->menu();
            break;
        }
    }
    if (fileMenu == nullptr || !fileMenu->actions().contains(generateAction)) {
        std::fprintf(stderr, "FAIL: File menu does not reference the rail's Generate C++ action\n");
        return 1;
    }

    std::printf(
        "PASS: gui-probe scenario generate-entry-points (rail order Fit -> separator -> Generate C++ -> spacer; "
        "File menu shares the same QAction, shortcut F9)\n");
    return 0;
}

int runSettingsViewScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running SettingsView tab editor scenario...\n");

    // 1. Open Settings tab via debug lever
    app::SettingsView* settingsView = window.debugOpenSettings();
    if (settingsView == nullptr) {
        std::fprintf(stderr, "FAIL: debugOpenSettings returned null\n");
        return 1;
    }
    QApplication::processEvents();

    // 2. Verify active scope and toggling
    if (settingsView->activeScope() != app::StoreScope::User) {
        std::fprintf(stderr, "FAIL: default settings scope should be User\n");
        return 1;
    }

    settingsView->setActiveScope(app::StoreScope::Workspace);
    QApplication::processEvents();
    if (settingsView->activeScope() != app::StoreScope::Workspace) {
        std::fprintf(stderr, "FAIL: switching to Workspace scope failed\n");
        return 1;
    }
    settingsView->setActiveScope(app::StoreScope::User);
    QApplication::processEvents();

    // 3. Test searching
    settingsView->setSearchQuery(QStringLiteral("grid"));
    QApplication::processEvents();

    settingsView->setSearchQuery(QStringLiteral(""));
    QApplication::processEvents();

    // 4. Capture screenshot of SettingsView
    QWidget* topLevel = window.window();
    const bool reenableUpdates = topLevel != nullptr && !topLevel->updatesEnabled();
    if (reenableUpdates) {
        topLevel->setUpdatesEnabled(true);
        QApplication::processEvents();
    }
    const QImage capture = settingsView->grab().toImage();
    if (reenableUpdates) {
        topLevel->setUpdatesEnabled(false);
    }
    if (!writeProbeImage(capture, "probe-settings-view")) {
        return 1;
    }

    // 5. Close settings tab and restore focus to loginPane
    window.debugCloseSettings();
    QApplication::processEvents();

    if (loginPane != nullptr) {
        window.debugFocusView(loginPane);
        QApplication::processEvents();
    }

    std::printf("PASS: gui-probe scenario settings-view (opened SettingsView tab, toggled scopes, filtered, captured, closed tab)\n");
    return 0;
}

// Scenario "project-settings-round-trip": the ProjectSettingsAccess seam with
// the real SettingsView and project_io, against a scratch Project (MainWindow
// has no headless lever to set currentProject_). triggerGenerate() is not
// driven; its directory formula runs against the seam-written outputDir instead.
int runProjectSettingsRoundTripScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running project-settings round-trip scenario...\n");

    // ---- 1. Inert state on the real window+SettingsView --------------------
    app::SettingsView* realView = window.debugOpenSettings();
    if (realView == nullptr) {
        std::fprintf(stderr, "FAIL: debugOpenSettings returned null\n");
        return 1;
    }
    QApplication::processEvents();

    app::SettingItemCard* realOutputDirCard = nullptr;
    app::SettingItemCard* realRootNamespaceCard = nullptr;
    for (auto* card : realView->findChildren<app::SettingItemCard*>()) {
        if (card->definition().key == QStringLiteral("codegen.outputDir")) realOutputDirCard = card;
        if (card->definition().key == QStringLiteral("codegen.rootNamespace")) realRootNamespaceCard = card;
    }
    if (realOutputDirCard == nullptr || realRootNamespaceCard == nullptr) {
        std::fprintf(stderr, "FAIL: codegen.outputDir/rootNamespace cards not found on the Preferences page\n");
        return 1;
    }
    if (realOutputDirCard->isEnabled() || realRootNamespaceCard->isEnabled()) {
        std::fprintf(stderr, "FAIL: ProjectOnly cards must render disabled with no project open\n");
        return 1;
    }
    bool sawGuidance = false;
    for (auto* label : realOutputDirCard->findChildren<QLabel*>()) {
        if (label->text().contains(QStringLiteral("Open a project"))) {
            sawGuidance = true;
        }
    }
    if (!sawGuidance) {
        std::fprintf(stderr, "FAIL: disabled ProjectOnly card is missing its one-line guidance\n");
        return 1;
    }
    window.debugCloseSettings();
    QApplication::processEvents();

    // ---- 2. Round trip through a standalone SettingsView over a scratch ---
    // Project, wired through the same ProjectSettingsAccess contract
    // MainWindow's triggerOpenSettings() constructs.
    const QString scratchPath =
        QDir::temp().filePath(QStringLiteral("ordo-state-designer-probe-project-settings.sdp"));
    QFile::remove(scratchPath);

    std::optional<app::Project> scratchProject;  // nullopt: no project open
    QString lastWriteError;

    app::ProjectSettingsAccess access;
    access.isProjectOpen = [&scratchProject] { return scratchProject.has_value(); };
    access.read = [&scratchProject](const QString& key) -> QJsonValue {
        if (!scratchProject.has_value()) {
            return QJsonValue();
        }
        if (key == QStringLiteral("codegen.outputDir")) {
            return scratchProject->outputDir;
        }
        if (key == QStringLiteral("codegen.rootNamespace")) {
            return scratchProject->rootNamespace;
        }
        return QJsonValue();
    };
    access.write = [&scratchProject, &scratchPath, &lastWriteError](const QString& key, const QJsonValue& val) {
        if (!scratchProject.has_value() || scratchPath.isEmpty()) {
            return;  // no project open: nothing to persist
        }
        if (key == QStringLiteral("codegen.outputDir")) {
            scratchProject->outputDir = val.toString();
        } else if (key == QStringLiteral("codegen.rootNamespace")) {
            scratchProject->rootNamespace = val.toString();
        } else {
            return;
        }
        QString error;
        if (!app::saveProject(*scratchProject, scratchPath, &error)) {
            lastWriteError = error;
        }
    };

    app::SettingsView localView(window.settingsStore(), access);
    app::SettingItemCard* outputDirCard = nullptr;
    app::SettingItemCard* rootNamespaceCard = nullptr;
    for (auto* card : localView.findChildren<app::SettingItemCard*>()) {
        if (card->definition().key == QStringLiteral("codegen.outputDir")) outputDirCard = card;
        if (card->definition().key == QStringLiteral("codegen.rootNamespace")) rootNamespaceCard = card;
    }
    if (outputDirCard == nullptr || rootNamespaceCard == nullptr) {
        std::fprintf(stderr, "FAIL: standalone SettingsView is missing the codegen.outputDir/rootNamespace cards\n");
        return 1;
    }

    // "Open" the scratch project, as a real Open Project would have already
    // persisted it (currentProject_ starts populated, never blank-then-saved).
    scratchProject = app::Project{
        .name = QStringLiteral("Probe Project"),
        .machineFiles = {},
        .outputDir = QString(),
        .rootNamespace = QString(),
    };
    QString seedError;
    if (!app::saveProject(*scratchProject, scratchPath, &seedError)) {
        std::fprintf(stderr, "FAIL: could not seed scratch .sdp: %s\n", qUtf8Printable(seedError));
        return 1;
    }
    outputDirCard->refreshValue(app::StoreScope::User);
    rootNamespaceCard->refreshValue(app::StoreScope::User);
    if (!outputDirCard->isEnabled() || !rootNamespaceCard->isEnabled()) {
        std::fprintf(stderr, "FAIL: ProjectOnly cards did not enable once a project opened\n");
        return 1;
    }

    // Commit through the real editor's editingFinished(), which setupUi() wires
    // to valueChanged(): the commit-not-per-keystroke path a real Enter/focus-out
    // takes. The DirPath card's browse button opens a modal QFileDialog, so drive
    // its QLineEdit sibling, which emits the same commit signal.
    const QString chosenOutputDir = QStringLiteral("generated_output");
    const QString chosenNamespace = QStringLiteral("probe::generated");

    auto* outputDirEdit = outputDirCard->findChild<QLineEdit*>();
    auto* namespaceEdit = rootNamespaceCard->findChild<QLineEdit*>();
    if (outputDirEdit == nullptr || namespaceEdit == nullptr) {
        std::fprintf(stderr, "FAIL: ProjectOnly cards are missing their line-edit editors\n");
        return 1;
    }
    outputDirEdit->setText(chosenOutputDir);
    outputDirEdit->editingFinished();
    namespaceEdit->setText(chosenNamespace);
    namespaceEdit->editingFinished();

    if (!lastWriteError.isEmpty()) {
        std::fprintf(stderr, "FAIL: seam write reported an error: %s\n", qUtf8Printable(lastWriteError));
        return 1;
    }
    if (scratchProject->outputDir != chosenOutputDir || scratchProject->rootNamespace != chosenNamespace) {
        std::fprintf(stderr, "FAIL: seam write did not update the in-memory Project fields\n");
        return 1;
    }

    // "confirm the .sdp on disk carries it"
    app::Project onDisk;
    QString loadError;
    if (!app::loadProject(scratchPath, &onDisk, &loadError)) {
        std::fprintf(stderr, "FAIL: could not reload the .sdp the seam just wrote: %s\n", qUtf8Printable(loadError));
        return 1;
    }
    if (onDisk.outputDir != chosenOutputDir || onDisk.rootNamespace != chosenNamespace) {
        std::fprintf(stderr, "FAIL: .sdp on disk does not carry the values set through the Preferences card\n");
        return 1;
    }

    // Reload the project and confirm the card shows it back: replace the
    // scratch Project with what was just reloaded from disk, then refresh the
    // cards as MainWindow's Open Project success path would.
    scratchProject = onDisk;
    outputDirCard->refreshValue(app::StoreScope::User);
    rootNamespaceCard->refreshValue(app::StoreScope::User);
    if (outputDirEdit->text() != chosenOutputDir || namespaceEdit->text() != chosenNamespace) {
        std::fprintf(stderr, "FAIL: reloaded card did not show the persisted values back\n");
        return 1;
    }

    // Reset means "clear the project field", not store_.remove().
    QPushButton* resetButton = nullptr;
    for (auto* btn : outputDirCard->findChildren<QPushButton*>()) {
        if (btn->text() == QStringLiteral("Reset")) {
            resetButton = btn;
            break;
        }
    }
    if (resetButton == nullptr) {
        std::fprintf(stderr, "FAIL: outputDir card has no Reset button once modified\n");
        return 1;
    }
    resetButton->click();
    if (!scratchProject->outputDir.isEmpty()) {
        std::fprintf(stderr, "FAIL: Reset did not clear the project's outputDir field\n");
        return 1;
    }
    // Setup for the generation proof below, not a seam assertion: restore the
    // field the seam already proved it can persist.
    scratchProject->outputDir = chosenOutputDir;

    // ---- 3. No-dialog generation proof -------------------------------------
    auto doc = window.loginFlowSession()->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr) {
        std::fprintf(stderr, "FAIL: login-flow session has no MachineDocAgent\n");
        return 1;
    }
    const app::Machine& machine = doc->machine();

    // Same formula as MainWindow::triggerGenerate() (main_window.cpp):
    // outputBaseDir = QFileInfo(currentProjectPath_).dir().filePath(outputDir),
    // machineDir = outputBaseDir + "/generated/" + sanitizeSnakeCase(name).
    const QString outputBaseDir = QFileInfo(scratchPath).dir().filePath(scratchProject->outputDir);
    const QString machineDir = outputBaseDir + QStringLiteral("/generated/") + app::sanitizeSnakeCase(machine.name);
    QDir(machineDir).removeRecursively();  // fresh scratch dir every run

    const QVector<app::GeneratedFile> files = app::generate(machine, scratchProject->rootNamespace);
    if (files.isEmpty()) {
        std::fprintf(stderr, "FAIL: generate() produced no files for the login-flow machine\n");
        return 1;
    }
    QString writeError;
    if (!app::writeGeneratedFiles(machineDir, files, &writeError)) {
        std::fprintf(stderr, "FAIL: writeGeneratedFiles failed: %s\n", qUtf8Printable(writeError));
        return 1;
    }
    for (const auto& file : files) {
        if (!QFileInfo::exists(machineDir + QStringLiteral("/") + file.relativePath)) {
            std::fprintf(stderr, "FAIL: generated file %s did not land in the seam-resolved directory %s\n",
                         qUtf8Printable(file.relativePath), qUtf8Printable(machineDir));
            return 1;
        }
    }

    QFile::remove(scratchPath);
    QDir(machineDir).removeRecursively();

    if (loginPane != nullptr) {
        window.debugFocusView(loginPane);
        QApplication::processEvents();
    }

    std::printf(
        "PASS: gui-probe scenario project-settings-round-trip (inert with no project open; seam write -> .sdp on "
        "disk -> reload shows it back -> reset clears the field; generation landed in the seam-resolved directory "
        "with no QFileDialog reachable)\n");
    return 0;
}

int runAsyncIoScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running Asynchronous I/O and Progress Overlay scenario...\n");

    // 1. Verify progress overlay existence on the main window
    auto* overlay = window.debugProgressOverlay();
    if (overlay == nullptr) {
        std::fprintf(stderr, "FAIL: debugProgressOverlay returned null\n");
        return 1;
    }

    if (overlay->isVisible()) {
        std::fprintf(stderr, "FAIL: progress overlay should be hidden initially\n");
        return 1;
    }

    // 2. Show operation on overlay and capture screenshot
    overlay->showOperation(101, QStringLiteral("Loading Project"), QStringLiteral("Reading project files..."), /*cancelable=*/true);
    overlay->setProgress(45, QStringLiteral("Reading project files (45%)..."));
    QApplication::processEvents();

    if (!overlay->isVisible()) {
        std::fprintf(stderr, "FAIL: progress overlay should be visible after showOperation\n");
        return 1;
    }

    QWidget* topLevel = window.window();
    const bool reenableUpdates = topLevel != nullptr && !topLevel->updatesEnabled();
    if (reenableUpdates) {
        topLevel->setUpdatesEnabled(true);
        QApplication::processEvents();
    }
    const QImage capture = overlay->grab().toImage();
    if (reenableUpdates) {
        topLevel->setUpdatesEnabled(false);
    }
    if (!writeProbeImage(capture, "probe-progress-overlay")) {
        return 1;
    }

    overlay->hideOperation();
    QApplication::processEvents();
    if (overlay->isVisible()) {
        std::fprintf(stderr, "FAIL: progress overlay should be hidden after hideOperation\n");
        return 1;
    }

    // 3. Verify IoDispatcher pipeline execution and fact observation
    auto& ioDispatcher = window.ioDispatcher();
    bool started = false;
    bool progressObserved = false;
    bool completed = false;
    quint64 taskOpId = 0;

    auto connStart = QObject::connect(&ioDispatcher, &app::IoDispatcher::ioStarted, [&](const app::events::IoStarted& ev) {
        if (ev.title == QStringLiteral("Probe Async Save")) {
            started = true;
            taskOpId = ev.opId;
        }
    });
    auto connProg = QObject::connect(&ioDispatcher, &app::IoDispatcher::ioProgress, [&](const app::events::IoProgress& ev) {
        if (taskOpId != 0 && ev.opId == taskOpId) {
            progressObserved = true;
        }
    });
    auto connDone = QObject::connect(&ioDispatcher, &app::IoDispatcher::ioCompleted, [&](const app::events::IoCompleted& ev) {
        if (taskOpId != 0 && ev.opId == taskOpId) {
            completed = true;
        }
    });

    taskOpId = ioDispatcher.submitAction(
        app::events::IoOperationKind::SaveProject,
        QStringLiteral("test-async-probe.sdp"),
        QStringLiteral("Probe Async Save"),
        /*isModal=*/false,
        [](const std::atomic<bool>& cancelToken, app::IoDispatcher::ProgressCallback progress, QString*) {
            if (progress) progress(50, QStringLiteral("Processing probe task..."));
            return true;
        }
    );

    ioDispatcher.waitForDone();
    QApplication::processEvents();

    QObject::disconnect(connStart);
    QObject::disconnect(connProg);
    QObject::disconnect(connDone);

    if (!started || !progressObserved || !completed) {
        std::fprintf(stderr, "FAIL: IoDispatcher task facts not received (started=%d, progress=%d, completed=%d)\n",
                     started, progressObserved, completed);
        return 1;
    }

    // 4. Verify debounced SettingsStore flushSync
    window.settingsStore().set(QStringLiteral("canvas.gridStep"), 24, app::StoreScope::User);
    window.settingsStore().flushSync();

    if (loginPane != nullptr) {
        window.debugFocusView(loginPane);
        QApplication::processEvents();
    }

    std::printf("PASS: gui-probe scenario async-io (progress overlay show/capture/hide, IoDispatcher events observation, settings flushSync)\n");
    return 0;
}

int runExportDialogScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running ExportDialog scenario (switch formats, options, scope, clipboard, capture)...\n");

    if (window.loginFlowSession() == nullptr) {
        std::fprintf(stderr, "FAIL: runExportDialogScenario requires loginFlowSession\n");
        return 1;
    }

    auto doc = window.loginFlowSession()->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr) {
        std::fprintf(stderr, "FAIL: runExportDialogScenario could not get MachineDocAgent\n");
        return 1;
    }

    const app::Machine& machine = doc->machine();

    app::ExportDialogConfig cfg;
    cfg.activeMachine = &machine;
    cfg.activeMachineName = QStringLiteral("LoginFlow");
    cfg.allMachines = {&machine};
    cfg.defaultDirectory = QDir::tempPath();

    app::ExportDialog dlg(cfg, &window);

    // 1. Initial format check
    if (dlg.selectedFormatId().isEmpty()) {
        std::fprintf(stderr, "FAIL: ExportDialog has empty initial format\n");
        return 1;
    }

    // 2. Select PlantUML
    dlg.selectFormat(QStringLiteral("plantuml"));
    if (dlg.selectedFormatId() != QStringLiteral("plantuml")) {
        std::fprintf(stderr, "FAIL: failed to select plantuml format (got '%s')\n",
                     qUtf8Printable(dlg.selectedFormatId()));
        return 1;
    }
    if (!dlg.destinationPath().endsWith(QStringLiteral(".puml"))) {
        std::fprintf(stderr, "FAIL: destination path did not update to .puml extension (got '%s')\n",
                     qUtf8Printable(dlg.destinationPath()));
        return 1;
    }

    // Check procedural options for PlantUML
    QVariant actOpt = dlg.optionValue(QStringLiteral("includeActionsGuards"));
    if (!actOpt.isValid() || !actOpt.toBool()) {
        std::fprintf(stderr, "FAIL: includeActionsGuards option missing or not defaulting to true\n");
        return 1;
    }
    // Toggle option off and on
    dlg.setOptionValue(QStringLiteral("includeActionsGuards"), false);
    if (dlg.optionValue(QStringLiteral("includeActionsGuards")).toBool() != false) {
        std::fprintf(stderr, "FAIL: failed to toggle includeActionsGuards option\n");
        return 1;
    }
    dlg.setOptionValue(QStringLiteral("includeActionsGuards"), true);

    // 3. Test Copy to Clipboard
    if (!dlg.isCopyToClipboardEnabled()) {
        std::fprintf(stderr, "FAIL: Copy to Clipboard should be enabled for PlantUML\n");
        return 1;
    }
    dlg.clickCopyToClipboard();
    QApplication::processEvents();

    const QString clipText = QGuiApplication::clipboard()->text();
    if (!clipText.contains(QStringLiteral("@startuml")) || !clipText.contains(QStringLiteral("@enduml"))) {
        std::fprintf(stderr, "FAIL: clipboard does not contain valid PlantUML\n");
        return 1;
    }
    if (!dlg.feedbackMessage().contains(QStringLiteral("Copied"))) {
        std::fprintf(stderr, "FAIL: feedback message not displayed on clipboard copy\n");
        return 1;
    }

    // 4. Switch to W3C SCXML
    dlg.selectFormat(QStringLiteral("scxml"));
    if (dlg.selectedFormatId() != QStringLiteral("scxml")) {
        std::fprintf(stderr, "FAIL: failed to select scxml format (got '%s')\n",
                     qUtf8Printable(dlg.selectedFormatId()));
        return 1;
    }
    if (!dlg.destinationPath().endsWith(QStringLiteral(".scxml"))) {
        std::fprintf(stderr, "FAIL: destination path did not update to .scxml extension (got '%s')\n",
                     qUtf8Printable(dlg.destinationPath()));
        return 1;
    }
    if (!dlg.isCopyToClipboardEnabled()) {
        std::fprintf(stderr, "FAIL: Copy to Clipboard should be enabled for W3C SCXML\n");
        return 1;
    }
    QVariant scxmlLayoutOpt = dlg.optionValue(QStringLiteral("includeLayout"));
    if (!scxmlLayoutOpt.isValid() || !scxmlLayoutOpt.toBool()) {
        std::fprintf(stderr, "FAIL: scxml includeLayout option missing or not defaulting to true\n");
        return 1;
    }
    dlg.clickCopyToClipboard();
    QApplication::processEvents();
    const QString scxmlClip = QGuiApplication::clipboard()->text();
    if (!scxmlClip.contains(QStringLiteral("<scxml")) || !scxmlClip.contains(QStringLiteral("</scxml>"))) {
        std::fprintf(stderr, "FAIL: clipboard does not contain valid W3C SCXML\n");
        return 1;
    }

    // 5. Test Destination & Export enablement
    const QString tempOut = QDir::tempPath() + QStringLiteral("/probe_export_login_flow.scxml");
    dlg.setDestinationPath(tempOut);
    if (!dlg.isExportEnabled()) {
        std::fprintf(stderr, "FAIL: Export button should be enabled with valid destination\n");
        return 1;
    }

    QApplication::processEvents();

    // 6. Capture screenshot of ExportDialog
    QWidget* topLevel = window.window();
    const bool reenableUpdates = topLevel != nullptr && !topLevel->updatesEnabled();
    if (reenableUpdates) {
        topLevel->setUpdatesEnabled(true);
        QApplication::processEvents();
    }
    const QImage capture = dlg.grab().toImage();
    if (reenableUpdates) {
        topLevel->setUpdatesEnabled(false);
    }
    if (!writeProbeImage(capture, "probe-export-dialog")) {
        return 1;
    }

    // 7. Verify export job execution
    app::ExportJob job = dlg.exportJob();
    if (!job.adapter || job.adapter->descriptor().id != QStringLiteral("scxml")) {
        std::fprintf(stderr, "FAIL: exportJob adapter mismatch (expected scxml)\n");
        return 1;
    }
    app::ExportJobResult expRes = app::executeExportJobSync(job);
    if (!expRes.ok || expRes.filesWritten != 1) {
        std::fprintf(stderr, "FAIL: executeExportJobSync failed for dialog job: %s\n",
                     expRes.error.toUtf8().constData());
        QFile::remove(tempOut);
        return 1;
    }

    // 8. Verify SCXML file round-trip import
    app::ScxmlImportResult impRes = app::importScxmlFile(tempOut);
    QFile::remove(tempOut);
    if (!impRes.ok || impRes.machine.states.isEmpty()) {
        std::fprintf(stderr, "FAIL: SCXML probe export file could not be imported back: %s\n",
                     impRes.error.toUtf8().constData());
        return 1;
    }

    if (loginPane != nullptr) {
        window.debugFocusView(loginPane);
        QApplication::processEvents();
    }

    std::printf("PASS: gui-probe scenario export-dialog (opened ExportDialog, switched formats, toggled options, tested clipboard, verified capture)\n");
    return 0;
}

// CanvasPresenter::zoomToElements()/zoomToSelection() center and fit their
// target, fully inside the viewport, centered, never past 100%, for a lone
// far-away state, a container (its whole subtree), a multi-selection (the
// union), and the machine frame (Zoom to Fit, which must zoom out here). Mints
// its own machine (deleted afterwards, loginPane refocused) so the shared
// Login Flow fixture stays untouched.
int runZoomToSelectionScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running zoom-to-selection scenario...\n");
    app::DocumentSession* session = window.createNewMachine(QStringLiteral("Zoom Probe"));
    QApplication::processEvents();
    app::EditorView* view = window.focusedView();
    if (session == nullptr || view == nullptr || view->session() != session || view->presenter() == nullptr ||
        view->canvasView() == nullptr) {
        std::fprintf(stderr, "FAIL: zoom-to-selection scenario could not bind a fresh machine\n");
        return 1;
    }
    app::CanvasPresenter* presenter = view->presenter();
    app::CanvasView* canvas = view->canvasView();
    QGraphicsScene* scene = canvas->scene();
    auto& kernel = session->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);

    // P contains A and B; F sits far away. Settle between kinds of edit, or an
    // unbatched send run crashes a later, unrelated capture offscreen.
    auto addState = [&](QPointF pos) {
        const quint64 id = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = pos});
        QApplication::processEvents();
        return id;
    };
    const quint64 kP = addState(QPointF(0.0, 0.0));
    const quint64 kA = addState(QPointF(-60.0, 150.0));
    const quint64 kB = addState(QPointF(180.0, 150.0));
    const quint64 kF = addState(QPointF(2600.0, 1800.0));
    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = kP});
    QApplication::processEvents();
    kernel.send(app::events::ReparentStateRequested{.id = kB, .parentId = kP});
    QApplication::processEvents();

    auto stateRect = [&](quint64 id) {
        const app::StateItem* item = presenter->debugStateItem(id);
        return item != nullptr ? item->sceneBoundingRect() : QRectF();
    };
    const QRect viewportRect = canvas->viewport()->rect();
    auto inView = [&](const QRectF& sceneRect) {
        return viewportRect.contains(canvas->mapFromScene(sceneRect).boundingRect());
    };
    int exitCode = 0;
    auto expectFramed = [&](const char* what, const QList<QRectF>& targets, bool expectZoomOut) {
        QApplication::processEvents();
        QRectF unionRect;
        for (const QRectF& rect : targets) {
            unionRect |= rect;
            if (rect.isEmpty() || !inView(rect)) {
                std::fprintf(stderr, "FAIL: zoom-to-selection (%s): a target is not fully inside the viewport\n", what);
                exitCode = 1;
                return;
            }
        }
        const QPoint center = canvas->mapFromScene(unionRect.center());
        const qreal zoom = canvas->transform().m11();
        if ((center - viewportRect.center()).manhattanLength() > 8 || zoom > 1.0 + 1e-9 ||
            (expectZoomOut && zoom >= 1.0)) {
            std::fprintf(stderr,
                         "FAIL: zoom-to-selection (%s): center off by (%d,%d) or zoom %.3f (want <= 1%s)\n", what,
                         center.x() - viewportRect.center().x(), center.y() - viewportRect.center().y(), zoom,
                         expectZoomOut ? ", and < 1" : "");
            exitCode = 1;
        }
    };
    auto lookAway = [&] {
        canvas->resetTransform();
        canvas->centerOn(QPointF(-6000.0, -6000.0));
        QApplication::processEvents();
    };

    lookAway();
    if (inView(stateRect(kF))) {
        std::fprintf(stderr, "FAIL: zoom-to-selection precondition -- F is already on screen\n");
        exitCode = 1;
    }
    presenter->zoomToElements({kF}, {});
    expectFramed("lone far-away state", {stateRect(kF)}, false);

    lookAway();
    presenter->zoomToElements({kP}, {});
    expectFramed("container and its subtree", {stateRect(kP), stateRect(kA), stateRect(kB)}, false);

    lookAway();
    scene->clearSelection();
    presenter->debugStateItem(kA)->setSelected(true);
    presenter->debugStateItem(kF)->setSelected(true);
    presenter->zoomToSelection();
    expectFramed("multi-selection union", {stateRect(kA), stateRect(kF)}, true);

    lookAway();
    scene->clearSelection();
    presenter->zoomToSelection();  // nothing selected -> Zoom to Fit, which frames the whole scene
    expectFramed("empty selection = Zoom to Fit", {scene->itemsBoundingRect()}, true);

    scene->clearSelection();
    window.debugDeleteMachine(session);
    QApplication::processEvents();
    window.debugFocusView(loginPane);
    QApplication::processEvents();
    if (loginPane == nullptr || loginPane->session() != window.loginFlowSession()) {
        std::fprintf(stderr, "FAIL: zoom-to-selection scenario did not leave the login pane on Login Flow\n");
        exitCode = 1;
    }
    if (exitCode == 0) {
        std::printf("PASS: gui-probe scenario zoom-to-selection (lone state, container subtree, multi-selection "
                    "union, empty selection -- inside the viewport, centered, zoom <= 1)\n");
    }
    return exitCode;
}

// The zoom-to-selection canvas exposure, through the real viewport (right
// press + QContextMenuEvent, the menu's QAction, a real F key). A right-click
// inside a multi-selection keeps it and the verb frames the union; on an
// unselected state it selects it alone; the transition and frame menus carry
// the verb; F frames the selection; in Simulate the menu zooms to the
// right-clicked element; a node double-click does nothing. Own machine.
int runZoomCanvasExposureScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running zoom canvas-exposure scenario...\n");
    app::DocumentSession* session = window.createNewMachine(QStringLiteral("Zoom Menu Probe"));
    QApplication::processEvents();
    app::EditorView* view = window.focusedView();
    if (session == nullptr || view == nullptr || view->session() != session || view->presenter() == nullptr ||
        view->canvasView() == nullptr) {
        std::fprintf(stderr, "FAIL: zoom canvas-exposure scenario could not bind a fresh machine\n");
        return 1;
    }
    app::CanvasPresenter* presenter = view->presenter();
    app::CanvasView* canvas = view->canvasView();
    QGraphicsScene* scene = canvas->scene();
    QWidget* viewport = canvas->viewport();
    auto& kernel = session->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);

    auto addState = [&](QPointF pos) {
        const quint64 id = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = pos});
        QApplication::processEvents();
        return id;
    };
    const quint64 kA = addState(QPointF(0.0, 0.0));
    const quint64 kB = addState(QPointF(0.0, 220.0));
    const quint64 kF = addState(QPointF(2600.0, 1800.0));
    const quint64 kT = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kA, .to = kB});
    QApplication::processEvents();

    const QRect viewportRect = viewport->rect();
    auto rectOf = [&](quint64 id) {
        const app::StateItem* item = presenter->debugStateItem(id);
        return item != nullptr ? item->sceneBoundingRect() : QRectF();
    };
    auto inView = [&](const QRectF& r) {
        return !r.isEmpty() && viewportRect.contains(canvas->mapFromScene(r).boundingRect());
    };
    auto showAt = [&](QPointF sceneCenter) {  // 1:1, centered; F stays off screen from A/B's area
        canvas->resetTransform();
        canvas->centerOn(sceneCenter);
        QApplication::processEvents();
    };
    auto isSelected = [&](quint64 id) {
        const app::StateItem* item = presenter->debugStateItem(id);
        return item != nullptr && item->isSelected();
    };
    auto rightPress = [&](QPointF scenePos) {
        const QPoint vp = canvas->mapFromScene(scenePos);
        const QPoint global = viewport->mapToGlobal(vp);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(vp), QPointF(global), Qt::RightButton, Qt::RightButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(vp), QPointF(global), Qt::RightButton,
                            Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &release);
    };
    // Opens the real context menu at scenePos and triggers its "Zoom to
    // Selection" action from inside the menu loop. Returns false when the
    // menu did not open or carried no such entry.
    auto zoomViaMenu = [&](QPointF scenePos) {
        rightPress(scenePos);
        auto found = std::make_shared<bool>(false);
        QTimer::singleShot(0, viewport, [found] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu == nullptr) {
                return;
            }
            // Chosen as a user does (highlight, then Enter) so exec() returns
            // the action; a menu that reads exec()'s result never sees a bare
            // QAction::trigger().
            for (QAction* action : menu->actions()) {
                if (action->text() == QStringLiteral("Zoom to Selection")) {
                    *found = true;
                    menu->setActiveAction(action);
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QCoreApplication::sendEvent(menu, &enter);
                    return;
                }
            }
            menu->close();
        });
        const QPoint vp = canvas->mapFromScene(scenePos);
        QContextMenuEvent contextMenu(QContextMenuEvent::Mouse, vp, viewport->mapToGlobal(vp));
        QCoreApplication::sendEvent(viewport, &contextMenu);
        QApplication::processEvents();
        return *found;
    };

    int exitCode = 0;
    auto fail = [&](const char* message) {
        std::fprintf(stderr, "FAIL: zoom canvas-exposure: %s\n", message);
        exitCode = 1;
    };

    // 1) Right-click inside a multi-selection keeps it; the menu verb frames the union.
    showAt(rectOf(kA).center());
    scene->clearSelection();
    presenter->debugStateItem(kA)->setSelected(true);
    presenter->debugStateItem(kF)->setSelected(true);
    if (!zoomViaMenu(rectOf(kA).center())) {
        fail("the state context menu has no Zoom to Selection entry");
    }
    if (!isSelected(kA) || !isSelected(kF)) {
        fail("right-clicking a state inside a multi-selection collapsed the selection");
    }
    if (!inView(rectOf(kA)) || !inView(rectOf(kF)) || canvas->transform().m11() >= 1.0) {
        fail("the state menu's Zoom to Selection did not frame the whole multi-selection");
    }
    if (!saveWidgetCapture(canvas, "probe-zoom-to-selection")) {  // two selected states, framed
        exitCode = 1;
    }

    // 2) Right-click on an unselected state selects it alone.
    showAt(rectOf(kB).center());
    rightPress(rectOf(kB).center());
    QContextMenuEvent closeOnly(QContextMenuEvent::Mouse, canvas->mapFromScene(rectOf(kB).center()));
    QTimer::singleShot(0, viewport, [] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            menu->close();
        }
    });
    QCoreApplication::sendEvent(viewport, &closeOnly);
    QApplication::processEvents();
    if (!isSelected(kB) || isSelected(kA) || isSelected(kF)) {
        fail("right-clicking an unselected state did not select it alone");
    }

    // 3) The transition menu carries the verb (right-click on its pill).
    showAt(presenter->debugLabelCenter(kT));
    if (!zoomViaMenu(presenter->debugLabelCenter(kT))) {
        fail("the transition context menu has no Zoom to Selection entry");
    }
    if (presenter->debugTransitionItem(kT) == nullptr || !presenter->debugTransitionItem(kT)->isSelected() ||
        !inView(presenter->debugTransitionItem(kT)->sceneBoundingRect())) {
        fail("the transition menu's Zoom to Selection did not frame the transition");
    }

    // 4) The machine frame's menu carries the verb (the whole machine: zoom out).
    app::MachineFrameItem* frame = nullptr;
    for (QGraphicsItem* item : scene->items()) {
        if (auto* candidate = qgraphicsitem_cast<app::MachineFrameItem*>(item)) {
            frame = candidate;
        }
    }
    if (frame == nullptr) {
        fail("no machine frame item");
    } else {
        showAt(rectOf(kA).center());
        const QRectF frameRect = frame->sceneFrameRect();
        std::optional<QPointF> headerPos;
        for (qreal x = frameRect.left() + 40.0; x < frameRect.right() - 40.0 && !headerPos; x += 24.0) {
            const QPointF candidate(x, frameRect.top() + 14.0);
            const QList<QGraphicsItem*> hits = scene->items(candidate);
            if (!hits.isEmpty() && hits.front() == frame && inView(QRectF(candidate, QSizeF(1.0, 1.0)))) {
                headerPos = candidate;
            }
        }
        if (!headerPos) {
            fail("no visible machine-frame header point to right-click");
        } else if (!zoomViaMenu(*headerPos)) {
            fail("the machine frame's context menu has no Zoom to Selection entry");
        } else if (!inView(rectOf(kA)) || !inView(rectOf(kF)) || canvas->transform().m11() >= 1.0) {
            fail("the frame menu's Zoom to Selection did not frame the whole machine");
        }
    }

    // 5) F frames the selection.
    scene->clearSelection();
    presenter->debugStateItem(kF)->setSelected(true);
    showAt(rectOf(kA).center());
    canvas->setFocus();
    QKeyEvent fKey(QEvent::KeyPress, Qt::Key_F, Qt::NoModifier, QStringLiteral("f"));
    QCoreApplication::sendEvent(canvas, &fKey);
    QApplication::processEvents();
    if (!inView(rectOf(kF))) {
        fail("F did not frame the selected state");
    }

    // 6) A node double-click still does nothing (no add, no zoom).
    showAt(rectOf(kA).center());
    const int statesBefore = static_cast<int>(doc->machine().states.size());
    const QTransform transformBefore = canvas->transform();
    const QPoint aPos = canvas->mapFromScene(rectOf(kA).center());
    QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(aPos), QPointF(viewport->mapToGlobal(aPos)), Qt::LeftButton,
                    Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &dbl);
    QMouseEvent dblRelease(QEvent::MouseButtonRelease, QPointF(aPos), QPointF(viewport->mapToGlobal(aPos)),
                           Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(viewport, &dblRelease);
    QApplication::processEvents();
    if (static_cast<int>(doc->machine().states.size()) != statesBefore || canvas->transform() != transformBefore) {
        fail("double-clicking a state changed the machine or the view");
    }

    // 7) Simulate: the menu zooms to the right-clicked element.
    scene->clearSelection();
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    QApplication::processEvents();
    showAt(rectOf(kF).center());
    canvas->scale(0.5, 0.5);  // off 1:1, so framing F at <= 1 is observable
    QApplication::processEvents();
    if (!zoomViaMenu(rectOf(kF).center())) {
        fail("the Simulate-mode canvas menu has no Zoom to Selection entry on a state");
    } else if (!inView(rectOf(kF)) || std::abs(canvas->transform().m11() - 1.0) > 1e-9) {
        fail("Simulate-mode Zoom to Selection did not frame the right-clicked state at 1:1");
    }
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();

    scene->clearSelection();
    window.debugDeleteMachine(session);
    QApplication::processEvents();
    window.debugFocusView(loginPane);
    QApplication::processEvents();
    if (loginPane == nullptr || loginPane->session() != window.loginFlowSession()) {
        fail("did not leave the login pane on Login Flow");
    }
    if (exitCode == 0) {
        std::printf("PASS: gui-probe scenario zoom-canvas-exposure (multi-selection kept + union framed, unselected "
                    "right-click selects alone, transition/frame menus, F key, Simulate menu, node double-click "
                    "inert)\n");
    }
    return exitCode;
}

// The zoom-to-selection Machines tree exposure, through the real tree: a
// double-click on the row (press/release/dblclick/release on its viewport
// rect) and the row's context menu. A state row frames the state, a
// transition row (menu) the transition, a machine row the whole machine; the
// double-click never toggles expansion; Simulate mode works too.
int runZoomMachinesTreeScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running zoom machines-tree scenario...\n");
    app::MachinesPanel* panel = window.debugMachinesPanel();
    QTreeWidget* tree = panel != nullptr ? panel->debugTree() : nullptr;
    app::DocumentSession* session = window.createNewMachine(QStringLiteral("Zoom Tree Probe"));
    QApplication::processEvents();
    app::EditorView* view = window.focusedView();
    if (tree == nullptr || session == nullptr || view == nullptr || view->session() != session ||
        view->presenter() == nullptr) {
        std::fprintf(stderr, "FAIL: zoom machines-tree scenario preconditions (tree/fresh machine) missing\n");
        return 1;
    }
    app::CanvasPresenter* presenter = view->presenter();
    app::CanvasView* canvas = view->canvasView();
    auto& kernel = session->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    const quint64 kA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(0.0, 0.0)});
    QApplication::processEvents();
    const quint64 kF = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(2600.0, 1800.0)});
    QApplication::processEvents();
    const quint64 kT = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kF, .to = kA});
    QApplication::processEvents();

    const QRect viewportRect = canvas->viewport()->rect();
    auto inView = [&](const QRectF& r) {
        return !r.isEmpty() && viewportRect.contains(canvas->mapFromScene(r).boundingRect());
    };
    auto rectOf = [&](quint64 id) {
        const app::StateItem* item = presenter->debugStateItem(id);
        return item != nullptr ? item->sceneBoundingRect() : QRectF();
    };
    auto lookAway = [&] {
        canvas->resetTransform();
        canvas->centerOn(QPointF(-6000.0, -6000.0));
        QApplication::processEvents();
    };
    // Rows are looked up fresh on every use: MachinesPanel rebuilds a machine's
    // subtree wholesale on outline changes, so a held row pointer dangles.
    auto rowFor = [&](quint64 stateId, quint64 transitionId) { return panel->debugRow(session, stateId, transitionId); };
    auto doubleClickRow = [&](quint64 stateId, quint64 transitionId) {
        QTreeWidgetItem* row = rowFor(stateId, transitionId);
        if (row == nullptr) {
            return false;
        }
        tree->scrollToItem(row);
        QApplication::processEvents();
        const QPoint pos = tree->visualItemRect(row).center();
        const QPoint global = tree->viewport()->mapToGlobal(pos);
        for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease, QEvent::MouseButtonDblClick,
                                  QEvent::MouseButtonRelease}) {
            const bool down = type != QEvent::MouseButtonRelease;
            QMouseEvent event(type, QPointF(pos), QPointF(global), Qt::LeftButton,
                              down ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(tree->viewport(), &event);
        }
        QApplication::processEvents();  // the deferred zoom
        return true;
    };
    auto zoomViaRowMenu = [&](quint64 stateId, quint64 transitionId) {
        QTreeWidgetItem* row = rowFor(stateId, transitionId);
        if (row == nullptr) {
            return false;
        }
        tree->scrollToItem(row);
        QApplication::processEvents();
        auto found = std::make_shared<bool>(false);
        QTimer::singleShot(0, tree, [found] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu == nullptr) {
                return;
            }
            // Chosen as a user does (highlight, then Enter) so exec() returns
            // the action; a menu that reads exec()'s result never sees a bare
            // QAction::trigger().
            for (QAction* action : menu->actions()) {
                if (action->text() == QStringLiteral("Zoom to Selection")) {
                    *found = true;
                    menu->setActiveAction(action);
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QCoreApplication::sendEvent(menu, &enter);
                    return;
                }
            }
            menu->close();
        });
        const QPoint pos = tree->visualItemRect(row).center();
        QContextMenuEvent contextMenu(QContextMenuEvent::Mouse, pos, tree->viewport()->mapToGlobal(pos));
        QCoreApplication::sendEvent(tree->viewport(), &contextMenu);
        QApplication::processEvents();
        QApplication::processEvents();  // the deferred zoom
        return *found;
    };

    int exitCode = 0;
    auto fail = [&](const char* message) {
        std::fprintf(stderr, "FAIL: zoom machines-tree: %s\n", message);
        exitCode = 1;
    };
    auto expanded = [&](quint64 stateId) {
        QTreeWidgetItem* row = rowFor(stateId, 0);
        return row != nullptr && row->isExpanded();
    };
    if (rowFor(0, 0) == nullptr || rowFor(kF, 0) == nullptr || rowFor(0, kT) == nullptr) {
        fail("the fresh machine's machine/state/transition rows are missing");
    } else {
        // 1) State row double-click frames the state and does not toggle expansion.
        lookAway();
        const bool stateExpanded = expanded(kF);
        doubleClickRow(kF, 0);
        if (!inView(rectOf(kF))) {
            fail("double-clicking a state row did not frame the state");
        }
        if (expanded(kF) != stateExpanded) {
            fail("double-clicking a state row toggled its expansion");
        }

        // 2) Transition row context menu frames the transition.
        lookAway();
        if (!zoomViaRowMenu(0, kT)) {
            fail("the transition row's context menu has no Zoom to Selection entry");
        } else if (presenter->debugTransitionItem(kT) == nullptr ||
                   !inView(presenter->debugTransitionItem(kT)->sceneBoundingRect())) {
            const QRectF edge = presenter->debugTransitionItem(kT) != nullptr
                                    ? presenter->debugTransitionItem(kT)->sceneBoundingRect()
                                    : QRectF();
            const QRect mapped = canvas->mapFromScene(edge).boundingRect();
            std::fprintf(stderr, "  diag: edge scene (%.0f,%.0f %.0fx%.0f) -> viewport (%d,%d %dx%d) of %dx%d, zoom %.3f\n",
                         edge.x(), edge.y(), edge.width(), edge.height(), mapped.x(), mapped.y(), mapped.width(),
                         mapped.height(), viewportRect.width(), viewportRect.height(), canvas->transform().m11());
            fail("the transition row's Zoom to Selection did not frame the transition");
        }
        // ...and it selected the transition first, though no left click touched
        // that row: step 1 left F selected, and the right-clicked row must win,
        // on the canvas and in the tree.
        if (presenter->debugTransitionItem(kT) == nullptr || !presenter->debugTransitionItem(kT)->isSelected() ||
            (presenter->debugStateItem(kF) != nullptr && presenter->debugStateItem(kF)->isSelected()) ||
            tree->currentItem() != rowFor(0, kT)) {
            fail("the transition row's Zoom to Selection did not select the transition first (canvas and tree)");
        }

        // 3) Machine row double-click frames the whole machine.
        lookAway();
        const bool machineExpanded = expanded(0);  // rowFor(0, 0) is the machine row
        doubleClickRow(0, 0);
        if (!inView(rectOf(kA)) || !inView(rectOf(kF)) || canvas->transform().m11() >= 1.0) {
            fail("double-clicking the machine row did not frame the whole machine");
        }
        if (expanded(0) != machineExpanded) {
            fail("double-clicking the machine row toggled its expansion");
        }

        // 4) Simulate mode: the state row still frames (ids, not selection).
        kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
        QApplication::processEvents();
        lookAway();
        if (!doubleClickRow(kF, 0) || !inView(rectOf(kF))) {
            fail("double-clicking a state row in Simulate mode did not frame the state");
        }
        kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
        QApplication::processEvents();

        // 5) Single click, element off screen: scrolled into view, zoom unchanged.
        auto singleClickRow = [&](quint64 stateId) {
            QTreeWidgetItem* row = rowFor(stateId, 0);
            if (row == nullptr) {
                return;
            }
            tree->scrollToItem(row);
            QApplication::processEvents();
            const QPoint pos = tree->visualItemRect(row).center();
            const QPoint global = tree->viewport()->mapToGlobal(pos);
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), QPointF(global), Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(tree->viewport(), &press);
            QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pos), QPointF(global), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(tree->viewport(), &release);
            QApplication::processEvents();  // the deferred reveal
        };
        lookAway();
        singleClickRow(kF);
        if (!inView(rectOf(kF)) || std::abs(canvas->transform().m11() - 1.0) > 1e-9) {
            fail("a single click on an off-screen state's row did not scroll it into view at the same zoom");
        }

        // 6) Single click, element already visible: the view does not move.
        canvas->resetTransform();
        canvas->centerOn(rectOf(kF).center() + QPointF(60.0, 40.0));
        QApplication::processEvents();
        const int hBefore = canvas->horizontalScrollBar()->value();
        const int vBefore = canvas->verticalScrollBar()->value();
        singleClickRow(kF);
        if (canvas->horizontalScrollBar()->value() != hBefore || canvas->verticalScrollBar()->value() != vBefore) {
            fail("a single click on an on-screen state's row moved the view");
        }
    }

    view->canvasView()->scene()->clearSelection();
    window.debugDeleteMachine(session);
    QApplication::processEvents();
    window.debugFocusView(loginPane);
    QApplication::processEvents();
    if (loginPane == nullptr || loginPane->session() != window.loginFlowSession()) {
        fail("did not leave the login pane on Login Flow");
    }
    if (exitCode == 0) {
        std::printf("PASS: gui-probe scenario zoom-machines-tree (state-row double-click, transition-row menu, "
                    "machine-row double-click, no expansion toggle, Simulate mode, single-click reveal off/on "
                    "screen)\n");
    }
    return exitCode;
}

// The canvas action box's SVG icons stay sharp when the canvas is zoomed in:
// the box is a scene item, so the view zoom scales it, and its icon pixmaps
// must be rendered at device pixel ratio x zoom, not dpr alone. View transform
// and scroll are restored exactly: a later drag scenario's integer rounding
// depends on them.
int runActionBoxIconSharpnessScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running action-box icon sharpness scenario...\n");
    app::CanvasPresenter* presenter = loginPane != nullptr ? loginPane->presenter() : nullptr;
    app::CanvasView* canvas = loginPane != nullptr ? loginPane->canvasView() : nullptr;
    if (presenter == nullptr || canvas == nullptr) {
        std::fprintf(stderr, "FAIL: action-box sharpness scenario preconditions (presenter/canvas) missing\n");
        return 1;
    }
    window.debugFocusView(loginPane);
    window.loginFlowSession()->kernel().send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();
    const QTransform savedTransform = canvas->transform();
    const int savedH = canvas->horizontalScrollBar()->value();
    const int savedV = canvas->verticalScrollBar()->value();

    int exitCode = 0;
    presenter->selectState(1);  // LoggedOut -> the State action box ([+][self][...], all icons)
    QApplication::processEvents();
    if (!presenter->debugActionBoxVisible()) {
        std::fprintf(stderr, "FAIL: selecting a state showed no action box\n");
        exitCode = 1;
    } else {
        constexpr qreal kZoom = 3.0;
        canvas->resetTransform();
        canvas->scale(kZoom, kZoom);
        canvas->centerOn(presenter->debugActionBoxRect().center());
        QApplication::processEvents();
        if (!saveWidgetCapture(canvas, "probe-action-box-zoomed")) {  // paints the box through the view
            exitCode = 1;
        }
        const qreal dpr = canvas->devicePixelRatioF();
        const qreal ratio = presenter->debugActionBoxIconRenderRatio();
        if (std::abs(ratio - kZoom * dpr) > 1e-6) {
            std::fprintf(stderr, "FAIL: action-box icons rendered at pixel ratio %.3f under a %.1fx zoom, want %.3f "
                                 "(dpr %.2f x zoom)\n",
                         ratio, kZoom, kZoom * dpr, dpr);
            exitCode = 1;
        }
    }

    canvas->scene()->clearSelection();
    canvas->setTransform(savedTransform);
    canvas->horizontalScrollBar()->setValue(savedH);
    canvas->verticalScrollBar()->setValue(savedV);
    QApplication::processEvents();
    if (exitCode == 0) {
        std::printf("PASS: gui-probe scenario action-box-icon-sharpness (icons rendered at dpr x zoom under a 3x "
                    "canvas zoom)\n");
    }
    return exitCode;
}

// A machine with no geometry (every state at the origin) opens laid out: boxes
// overlap only by containment, children sit inside their container, no two
// Normal pills intersect, no pill hits a foreign state box, every Normal
// transition has a labelRatio, and the residual count is 0. Driven through
// debugImportStateMachine on a flat and a hierarchical fixture.
int runInitialAutoLayoutScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running initial auto-layout scenario...\n");
    int exitCode = 0;
    auto fail = [&](const QString& message) {
        std::fprintf(stderr, "FAIL: initial auto-layout: %s\n", qUtf8Printable(message));
        exitCode = 1;
    };

    // The hierarchical fixture: A -> P{C1 -> C2} -> B, every state at (0,0).
    const QString hierPath = QDir(QCoreApplication::applicationDirPath())
                                 .absoluteFilePath(QStringLiteral("../temp/code/auto-layout/hierarchy-no-coordinates.sdm"));
    QDir().mkpath(QFileInfo(hierPath).absolutePath());
    {
        app::Machine machine;
        machine.name = QStringLiteral("AutoLayoutHierarchy");
        auto addState = [&machine](quint64 id, const QString& name, quint64 parentId) {
            app::State state;
            state.id = id;
            state.name = name;
            state.parentId = parentId;
            machine.states.push_back(state);
        };
        addState(1, QStringLiteral("A"), 0);
        addState(2, QStringLiteral("Processing"), 0);
        addState(3, QStringLiteral("Collect"), 2);
        addState(4, QStringLiteral("Validate"), 2);
        addState(5, QStringLiteral("B"), 0);
        machine.states[1].initialChildId = 3;
        machine.initialStateId = 1;
        auto addTransition = [&machine](quint64 id, quint64 from, quint64 to, const QString& event) {
            app::Transition transition;
            transition.id = id;
            transition.from = from;
            transition.to = to;
            transition.event = event;
            machine.transitions.push_back(transition);
        };
        addTransition(6, 1, 2, QStringLiteral("GO"));
        addTransition(7, 3, 4, QStringLiteral("NEXT"));
        addTransition(8, 4, 5, QStringLiteral("DONE"));
        addTransition(9, 2, 5, QStringLiteral("ABORT"));
        machine.nextId = 10;
        QString error;
        if (!app::saveMachine(machine, hierPath, &error)) {
            fail(QStringLiteral("could not write the hierarchical fixture: %1").arg(error));
            return exitCode;
        }
    }

    const struct {
        const char* label;
        QString path;
        const char* capture;
    } fixtures[] = {
        {"flat", QStringLiteral("src/machines/fixtures/auto-layout-no-coordinates.sdm"), "probe-auto-layout-flat"},
        {"hierarchy", hierPath, "probe-auto-layout-hierarchy"},
    };
    QVector<int> residualCounts;  // one entry per fixture, for the final PASS line
    for (const auto& fixture : fixtures) {
        QString error;
        app::DocumentSession* session = window.debugImportStateMachine(fixture.path, nullptr, &error);
        QApplication::processEvents();
        app::EditorView* view = window.focusedView();
        if (session == nullptr || view == nullptr || view->session() != session || view->presenter() == nullptr) {
            fail(QStringLiteral("%1: could not open %2 (%3)").arg(QLatin1String(fixture.label), fixture.path, error));
            continue;
        }
        auto doc = session->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        const app::Machine& machine = doc->machine();
        auto isAncestor = [&](quint64 ancestor, quint64 id) {
            for (const app::State* s = doc->findState(id); s != nullptr && s->parentId != 0;
                 s = doc->findState(s->parentId)) {
                if (s->parentId == ancestor) {
                    return true;
                }
            }
            return false;
        };
        QHash<quint64, QRectF> boxes;
        for (const app::State& state : machine.states) {
            const app::StateItem* item = view->presenter()->debugStateItem(state.id);
            boxes.insert(state.id, item != nullptr ? item->sceneRect() : QRectF());
        }
        int overlaps = 0;
        bool anyOffOrigin = false;
        for (const app::State& a : machine.states) {
            anyOffOrigin = anyOffOrigin || !a.pos.isNull();
            for (const app::State& b : machine.states) {
                if (a.id >= b.id || isAncestor(a.id, b.id) || isAncestor(b.id, a.id)) {
                    continue;
                }
                if (boxes.value(a.id).intersects(boxes.value(b.id))) {
                    ++overlaps;
                    fail(QStringLiteral("%1: '%2' and '%3' overlap").arg(QLatin1String(fixture.label), a.name, b.name));
                }
            }
            if (a.parentId != 0 && !boxes.value(a.parentId).contains(boxes.value(a.id))) {
                fail(QStringLiteral("%1: child '%2' is not inside its container").arg(QLatin1String(fixture.label), a.name));
            }
        }
        if (!anyOffOrigin) {
            fail(QStringLiteral("%1: every state is still at the origin").arg(QLatin1String(fixture.label)));
        }

        // Every Normal transition's pill sits clear of every other pill and of
        // every state box it does not belong to, and carries the labelRatio the
        // layout wrote.
        struct NormalTransitionLabel {
            quint64 transitionId = 0;
            quint64 from = 0;
            quint64 to = 0;
            QString event;
            QString fromName;
            QString toName;
            QRectF rect;
        };
        QVector<NormalTransitionLabel> labels;
        for (const app::Transition& t : machine.transitions) {
            if (t.from == 0 || t.to == 0 || t.from == t.to) {
                continue;
            }
            const QRectF labelRect = view->presenter()->debugPillRect(t.id);
            if (labelRect.isNull()) {
                fail(QStringLiteral("%1: transition '%2' (id %3) has no label rect")
                         .arg(QLatin1String(fixture.label))
                         .arg(t.event)
                         .arg(t.id));
                continue;
            }
            const app::State* fromState = doc->findState(t.from);
            const app::State* toState = doc->findState(t.to);
            labels.push_back(NormalTransitionLabel{
                t.id, t.from, t.to, t.event, fromState != nullptr ? fromState->name : QString(),
                toState != nullptr ? toState->name : QString(), labelRect});
        }
        for (int i = 0; i < labels.size(); ++i) {
            for (int j = i + 1; j < labels.size(); ++j) {
                if (labels.at(i).rect.intersects(labels.at(j).rect)) {
                    fail(QStringLiteral("%1: labels '%2' (%3->%4) and '%5' (%6->%7) overlap")
                             .arg(QLatin1String(fixture.label))
                             .arg(labels.at(i).event)
                             .arg(labels.at(i).fromName)
                             .arg(labels.at(i).toName)
                             .arg(labels.at(j).event)
                             .arg(labels.at(j).fromName)
                             .arg(labels.at(j).toName));
                }
            }
        }
        for (const NormalTransitionLabel& label : labels) {
            for (const app::State& state : machine.states) {
                if (isAncestor(state.id, label.from) || isAncestor(state.id, label.to)) {
                    continue;
                }
                if (label.rect.intersects(boxes.value(state.id))) {
                    fail(QStringLiteral("%1: label '%2' (%3->%4) overlaps state '%5'")
                             .arg(QLatin1String(fixture.label))
                             .arg(label.event)
                             .arg(label.fromName)
                             .arg(label.toName)
                             .arg(state.name));
                }
            }
        }
        for (const app::Transition& t : machine.transitions) {
            if (t.from == 0 || t.to == 0 || t.from == t.to) {
                continue;
            }
            if (!t.labelRatio.has_value()) {
                fail(QStringLiteral("%1: transition '%2' has no labelRatio")
                         .arg(QLatin1String(fixture.label))
                         .arg(t.event));
            }
        }
        const int residualCount = session->debugAutoLayoutResidualCount();
        residualCounts.push_back(residualCount);
        if (residualCount != 0) {
            fail(QStringLiteral("%1: auto-layout residual count is %2, want 0")
                     .arg(QLatin1String(fixture.label))
                     .arg(residualCount));
        }

        if (!saveSceneCapture(view, fixture.capture)) {
            exitCode = 1;
        }
        window.debugDeleteMachine(session);
        QApplication::processEvents();
    }

    window.debugFocusView(loginPane);
    QApplication::processEvents();
    if (loginPane == nullptr || loginPane->session() != window.loginFlowSession()) {
        fail(QStringLiteral("did not leave the login pane on Login Flow"));
    }
    if (exitCode == 0) {
        std::printf("PASS: gui-probe scenario initial-auto-layout (flat + hierarchy fixtures open laid out: no "
                    "overlapping boxes, children inside containers, off the origin; Normal-transition labels "
                    "disjoint from each other and from non-ancestor state boxes, every one carries a labelRatio, "
                    "residuals %d/%d)\n",
                    residualCounts.value(0, -1), residualCounts.value(1, -1));
    }
    return exitCode;
}

// The AutoLayoutDialog currently open, modal or not; nullptr when none is.
static app::AutoLayoutDialog* findOpenAutoLayoutDialog() {
    if (auto* dialog = qobject_cast<app::AutoLayoutDialog*>(QApplication::activeModalWidget())) {
        return dialog;
    }
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<app::AutoLayoutDialog*>(widget);
        if (dialog != nullptr && dialog->isVisible()) {
            return dialog;
        }
    }
    return nullptr;
}

// The on-demand auto-layout flow through the real entry points: Edit > Auto
// Layout... trigger()ed with its modal dialog driven from a zero timer (Top to
// Bottom, Apply) leaves boxes and pills disjoint and ranks monotone in y; one
// Undo restores positions and labelRatio bitwise, Redo re-applies them; the
// empty-canvas menu entry opens the dialog. The autoLayout.* User-scope keys
// are restored exactly (the store is the real user file).
int runOnDemandAutoLayoutScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running on-demand auto-layout scenario...\n");
    int exitCode = 0;
    auto fail = [&](const QString& message) {
        std::fprintf(stderr, "FAIL: on-demand auto-layout: %s\n", qUtf8Printable(message));
        exitCode = 1;
    };

    app::SettingsStore& store = window.settingsStore();
    const QStringList settingKeys = {QStringLiteral("autoLayout.direction"), QStringLiteral("autoLayout.layerGap"),
                                     QStringLiteral("autoLayout.nodeGap")};
    QHash<QString, QJsonValue> savedSettings;  // User-scope keys present before this scenario
    for (const QString& key : settingKeys) {
        if (store.has(key, app::StoreScope::User)) {
            savedSettings.insert(key, store.getValueInScope(key, app::StoreScope::User));
        }
    }
    auto restoreSettings = [&] {
        for (const QString& key : settingKeys) {
            if (savedSettings.contains(key)) {
                store.set(key, savedSettings.value(key), app::StoreScope::User);
            } else {
                store.remove(key, app::StoreScope::User);
            }
        }
    };
    // A known baseline, so the menu flow's store is observable below.
    app::AutoLayoutDialog::storeOptions(&store, app::AutoLayoutOptions{});

    // (1) Open the flat fixture; snapshot BEFORE.
    QString error;
    app::DocumentSession* session = window.debugImportStateMachine(
        QStringLiteral("src/machines/fixtures/auto-layout-no-coordinates.sdm"), nullptr, &error);
    QApplication::processEvents();
    app::EditorView* view = window.focusedView();
    if (session == nullptr || view == nullptr || view->session() != session || view->presenter() == nullptr) {
        fail(QStringLiteral("could not open the flat fixture (%1)").arg(error));
        restoreSettings();
        return exitCode;
    }
    auto doc = session->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = session->kernel().agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (doc == nullptr || sim == nullptr || sim->mode() != app::events::Mode::Design) {
        fail(QStringLiteral("the fixture session is not in Design mode"));
    }

    struct Geometry {
        QHash<quint64, QPointF> positions;
        QHash<quint64, std::optional<qreal>> ratios;
    };
    auto snapshot = [&] {
        Geometry geometry;
        for (const app::State& state : doc->machine().states) {
            geometry.positions.insert(state.id, state.pos);
        }
        for (const app::Transition& transition : doc->machine().transitions) {
            geometry.ratios.insert(transition.id, transition.labelRatio);
        }
        return geometry;
    };
    // Exact on purpose: QPointF::operator== is fuzzy (qFuzzyCompare).
    auto samePoint = [](QPointF a, QPointF b) { return a.x() == b.x() && a.y() == b.y(); };
    auto sameGeometry = [&](const Geometry& a, const Geometry& b, const QString& when) {
        bool same = a.positions.size() == b.positions.size() && a.ratios.size() == b.ratios.size();
        for (auto it = a.positions.constBegin(); it != a.positions.constEnd(); ++it) {
            if (!b.positions.contains(it.key()) || !samePoint(it.value(), b.positions.value(it.key()))) {
                fail(QStringLiteral("%1: state %2 is at (%3, %4), want (%5, %6)")
                         .arg(when)
                         .arg(it.key())
                         .arg(b.positions.value(it.key()).x())
                         .arg(b.positions.value(it.key()).y())
                         .arg(it.value().x())
                         .arg(it.value().y()));
                same = false;
            }
        }
        for (auto it = a.ratios.constBegin(); it != a.ratios.constEnd(); ++it) {
            if (!b.ratios.contains(it.key()) || b.ratios.value(it.key()) != it.value()) {
                fail(QStringLiteral("%1: transition %2's labelRatio differs").arg(when).arg(it.key()));
                same = false;
            }
        }
        return same;
    };
    const Geometry before = snapshot();

    // (2) The dialog on its own.
    {
        app::AutoLayoutDialog dialog(app::AutoLayoutOptions{}, &window);
        dialog.show();
        QApplication::processEvents();
        QComboBox* combo = dialog.debugDirectionCombo();
        if (combo == nullptr || combo->count() != 4) {
            fail(QStringLiteral("the Direction combo does not hold four directions"));
        } else {
            combo->setCurrentIndex(combo->findData(app::layoutDirectionName(app::LayoutDirection::TopToBottom)));
        }
        dialog.debugLayerGapSpin()->setValue(120);
        dialog.debugNodeGapSpin()->setValue(72);
        const app::AutoLayoutOptions options = dialog.options();
        if (options.direction != app::LayoutDirection::TopToBottom || options.layerGap != 120 ||
            options.nodeGap != 72) {
            fail(QStringLiteral("options() does not reflect Top to Bottom / 120 / 72"));
        }
        dialog.close();
    }
    QApplication::processEvents();

    // Arms a zero timer that finds the next AutoLayoutDialog to open -- its
    // exec() spins a nested loop inside the trigger -- records the direction it
    // opened with, runs `act`, and rejects it if `act` left it open. The caller
    // deletes the timer once the trigger has returned.
    struct DialogDriver {
        QTimer* timer = nullptr;
        std::shared_ptr<bool> seen;
        std::shared_ptr<QString> openedWith;
    };
    auto armDialogDriver = [&window](std::function<void(app::AutoLayoutDialog*)> act) {
        DialogDriver driver{new QTimer(&window), std::make_shared<bool>(false), std::make_shared<QString>()};
        driver.timer->setInterval(0);
        QTimer* timer = driver.timer;
        QObject::connect(timer, &QTimer::timeout, timer,
                         [timer, seen = driver.seen, openedWith = driver.openedWith, act] {
                             app::AutoLayoutDialog* dialog = findOpenAutoLayoutDialog();
                             if (dialog == nullptr) {
                                 return;
                             }
                             timer->stop();
                             *seen = true;
                             *openedWith = dialog->debugDirectionCombo()->currentData().toString();
                             act(dialog);
                             if (dialog->isVisible()) {
                                 dialog->reject();
                             }
                         });
        driver.timer->start();
        return driver;
    };

    // (3) Edit > Auto Layout..., Top to Bottom, Apply.
    QAction* action = window.debugAutoLayoutAction();
    bool inEditMenu = false;
    for (QAction* menuAction : window.menuBar()->actions()) {
        if (menuAction->menu() != nullptr && menuAction->text() == QStringLiteral("&Edit")) {
            inEditMenu = action != nullptr && menuAction->menu()->actions().contains(action);
        }
    }
    if (action == nullptr || !inEditMenu || action->shortcut() != QKeySequence(QStringLiteral("Ctrl+Shift+L"))) {
        fail(QStringLiteral("Edit menu has no Auto Layout... action with Ctrl+Shift+L"));
        window.debugDeleteMachine(session);
        restoreSettings();
        return exitCode;
    }
    DialogDriver applyDriver = armDialogDriver([](app::AutoLayoutDialog* dialog) {
        QComboBox* combo = dialog->debugDirectionCombo();
        combo->setCurrentIndex(combo->findData(app::layoutDirectionName(app::LayoutDirection::TopToBottom)));
        dialog->debugApplyButton()->click();
    });
    action->trigger();
    QApplication::processEvents();
    delete applyDriver.timer;
    if (!*applyDriver.seen) {
        fail(QStringLiteral("Edit > Auto Layout... did not open the dialog"));
    } else if (*applyDriver.openedWith != app::layoutDirectionName(app::LayoutDirection::LeftToRight)) {
        fail(QStringLiteral("the dialog opened on '%1', not the stored LeftToRight").arg(*applyDriver.openedWith));
    }
    if (!window.statusBar()->currentMessage().startsWith(QStringLiteral("Auto Layout:"))) {
        fail(QStringLiteral("no Auto Layout status message (got '%1')").arg(window.statusBar()->currentMessage()));
    }

    const Geometry after = snapshot();
    const app::Machine& machine = doc->machine();
    auto isAncestor = [&](quint64 ancestor, quint64 id) {
        for (const app::State* s = doc->findState(id); s != nullptr && s->parentId != 0; s = doc->findState(s->parentId)) {
            if (s->parentId == ancestor) {
                return true;
            }
        }
        return false;
    };
    QHash<quint64, QRectF> boxes;
    for (const app::State& state : machine.states) {
        const app::StateItem* item = view->presenter()->debugStateItem(state.id);
        boxes.insert(state.id, item != nullptr ? item->sceneRect() : QRectF());
    }
    for (const app::State& a : machine.states) {
        for (const app::State& b : machine.states) {
            if (a.id < b.id && !isAncestor(a.id, b.id) && !isAncestor(b.id, a.id) &&
                boxes.value(a.id).intersects(boxes.value(b.id))) {
                fail(QStringLiteral("after Top to Bottom, '%1' and '%2' overlap").arg(a.name, b.name));
            }
        }
    }
    struct Pill {
        quint64 id = 0;
        quint64 from = 0;
        quint64 to = 0;
        QString event;
        QRectF rect;
    };
    QVector<Pill> pills;
    for (const app::Transition& t : machine.transitions) {
        if (t.from == 0 || t.to == 0 || t.from == t.to) {
            continue;
        }
        const QRectF rect = view->presenter()->debugPillRect(t.id);
        if (rect.isNull()) {
            fail(QStringLiteral("transition '%1' (id %2) has no label rect").arg(t.event).arg(t.id));
            continue;
        }
        pills.push_back(Pill{t.id, t.from, t.to, t.event, rect});
    }
    for (int i = 0; i < pills.size(); ++i) {
        for (int j = i + 1; j < pills.size(); ++j) {
            if (pills.at(i).rect.intersects(pills.at(j).rect)) {
                fail(QStringLiteral("after Top to Bottom, labels '%1' and '%2' overlap")
                         .arg(pills.at(i).event, pills.at(j).event));
            }
        }
        for (const app::State& state : machine.states) {
            if (!isAncestor(state.id, pills.at(i).from) && !isAncestor(state.id, pills.at(i).to) &&
                pills.at(i).rect.intersects(boxes.value(state.id))) {
                fail(QStringLiteral("after Top to Bottom, label '%1' overlaps state '%2'")
                         .arg(pills.at(i).event, state.name));
            }
        }
    }
    auto yOf = [&](const QString& name) -> std::optional<qreal> {
        for (const app::State& state : machine.states) {
            if (state.name == name) {
                return state.pos.y();
            }
        }
        return std::nullopt;
    };
    const std::optional<qreal> idleY = yOf(QStringLiteral("Idle"));
    const std::optional<qreal> fetchingY = yOf(QStringLiteral("Fetching"));
    const std::optional<qreal> analyzingY = yOf(QStringLiteral("Analyzing"));
    const std::optional<qreal> reportY = yOf(QStringLiteral("Report"));
    if (!idleY || !fetchingY || !analyzingY || !reportY) {
        fail(QStringLiteral("the fixture lacks Idle/Fetching/Analyzing/Report"));
    } else if (!(*idleY < *fetchingY && *fetchingY < *analyzingY && *analyzingY < *reportY)) {
        fail(QStringLiteral("ranks are not monotone in y: Idle %1, Fetching %2, Analyzing %3, Report %4")
                 .arg(*idleY)
                 .arg(*fetchingY)
                 .arg(*analyzingY)
                 .arg(*reportY));
    }
    int moved = 0;
    for (auto it = before.positions.constBegin(); it != before.positions.constEnd(); ++it) {
        if (!samePoint(it.value(), after.positions.value(it.key()))) {
            ++moved;
        }
    }
    if (moved == 0) {
        fail(QStringLiteral("Top to Bottom moved no state"));
    }

    // (4) One Undo restores BEFORE bitwise; Redo restores AFTER.
    if (sim != nullptr && sim->mode() != app::events::Mode::Design) {
        fail(QStringLiteral("left Design mode before the undo walk"));
    }
    session->kernel().send(app::events::UndoRequested{});
    QApplication::processEvents();
    const bool undoRestored = sameGeometry(before, snapshot(), QStringLiteral("after one Undo"));
    session->kernel().send(app::events::RedoRequested{});
    QApplication::processEvents();
    const bool redoRestored = sameGeometry(after, snapshot(), QStringLiteral("after Redo"));

    // (5) The menu flow stored the direction; a re-opened dialog shows it.
    const app::AutoLayoutOptions stored = app::AutoLayoutDialog::loadOptions(&store);
    if (stored.direction != app::LayoutDirection::TopToBottom) {
        fail(QStringLiteral("the stored direction is '%1', want TopToBottom")
                 .arg(app::layoutDirectionName(stored.direction)));
    }
    {
        app::AutoLayoutDialog reopened(stored, &window);
        if (reopened.debugDirectionCombo()->currentData().toString() !=
            app::layoutDirectionName(app::LayoutDirection::TopToBottom)) {
            fail(QStringLiteral("the re-opened dialog does not show Top to Bottom"));
        }
    }

    // (6) The real empty-canvas menu: its Auto Layout... entry, chosen inside
    // the menu loop (setActiveAction + Enter), opens the dialog; the dialog
    // driver cancels it, so nothing may change.
    app::CanvasView* canvas = view->canvasView();
    QWidget* viewport = canvas->viewport();
    QPoint emptyPoint(-1, -1);
    for (const QPoint candidate : {QPoint(4, 4), QPoint(viewport->width() - 5, 4), QPoint(4, viewport->height() - 5),
                                   QPoint(viewport->width() - 5, viewport->height() - 5)}) {
        if (canvas->scene()->items(canvas->mapToScene(candidate)).isEmpty()) {
            emptyPoint = candidate;
            break;
        }
    }
    bool menuHadEntry = false;
    bool menuDialogSeen = false;
    if (emptyPoint.x() < 0) {
        fail(QStringLiteral("no empty viewport corner to right-click"));
    } else {
        auto found = std::make_shared<bool>(false);
        auto cancelDriver = std::make_shared<DialogDriver>();
        QTimer::singleShot(0, viewport, [found, cancelDriver, &armDialogDriver] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu == nullptr) {
                return;
            }
            for (QAction* menuAction : menu->actions()) {
                if (menuAction->text() == QStringLiteral("Auto Layout") + QChar(0x2026)) {
                    *found = true;
                    *cancelDriver = armDialogDriver([](app::AutoLayoutDialog*) {});  // leaves it open, so it is rejected
                    menu->setActiveAction(menuAction);
                    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QCoreApplication::sendEvent(menu, &enter);
                    return;
                }
            }
            menu->close();
        });
        QContextMenuEvent contextMenu(QContextMenuEvent::Mouse, emptyPoint, viewport->mapToGlobal(emptyPoint));
        QCoreApplication::sendEvent(viewport, &contextMenu);
        QApplication::processEvents();
        menuHadEntry = *found;
        menuDialogSeen = cancelDriver->seen != nullptr && *cancelDriver->seen;
        delete cancelDriver->timer;
        if (!menuHadEntry) {
            fail(QStringLiteral("the empty-canvas menu has no Auto Layout... entry"));
        } else if (!menuDialogSeen) {
            fail(QStringLiteral("the empty-canvas menu's Auto Layout... did not open the dialog"));
        }
        sameGeometry(after, snapshot(), QStringLiteral("after the cancelled canvas-menu dialog"));
    }

    // (7) Capture, clean up.
    if (!saveSceneCapture(view, "probe-auto-layout-tb")) {
        exitCode = 1;
    }
    window.debugDeleteMachine(session);
    QApplication::processEvents();
    window.debugFocusView(loginPane);
    QApplication::processEvents();
    restoreSettings();
    if (loginPane == nullptr || loginPane->session() != window.loginFlowSession()) {
        fail(QStringLiteral("did not leave the login pane on Login Flow"));
    }
    if (exitCode == 0 && undoRestored && redoRestored) {
        std::printf("PASS: gui-probe scenario on-demand-auto-layout (dialog: 4 directions, options() reflects Top to "
                    "Bottom/120/72; Edit > Auto Layout... (Ctrl+Shift+L) opened on the stored LeftToRight, Top to "
                    "Bottom + Apply moved %d state(s): boxes disjoint, Normal-transition labels disjoint from each "
                    "other and from non-ancestor boxes, Idle < Fetching < Analyzing < Report in y; one Undo "
                    "restored every position and labelRatio bitwise, Redo re-applied them; stored direction "
                    "TopToBottom; empty-canvas menu's Auto Layout... opened the dialog, cancel changed nothing)\n",
                    moved);
    }
    return exitCode;
}

int runMachineRelationshipScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running machine relationship, rename, and delete scenario...\n");

    // 1. Import machine and verify collision-free relative path allocation
    QString error;
    app::DocumentSession* imported = window.debugImportStateMachine(
        QStringLiteral("src/machines/motion-controller.sdm"), nullptr, &error);
    QApplication::processEvents();
    if (imported == nullptr) {
        std::fprintf(stderr, "FAIL: debugImportStateMachine failed for motion-controller.sdm: %s\n",
                     qUtf8Printable(error));
        return 1;
    }

    if (imported->relativePath().isEmpty() || !imported->relativePath().endsWith(QStringLiteral(".sdm"))) {
        std::fprintf(stderr, "FAIL: imported machine relativePath not properly allocated: '%s'\n",
                     qUtf8Printable(imported->relativePath()));
        return 1;
    }

    // 2. Rename machine and verify synchronization across session, doc agent, and tab
    const QString newName = QStringLiteral("Motion Controller Renamed");
    window.debugRenameMachine(imported, newName);
    QApplication::processEvents();

    if (imported->machineName() != newName) {
        std::fprintf(stderr, "FAIL: debugRenameMachine did not update session machineName: '%s'\n",
                     qUtf8Printable(imported->machineName()));
        return 1;
    }
    auto doc = imported->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr || doc->machine().name != newName) {
        std::fprintf(stderr, "FAIL: debugRenameMachine did not update doc agent machine name\n");
        return 1;
    }

    app::EditorView* curView = window.focusedView();
    if (curView != nullptr) {
        saveSceneCapture(curView, "probe-machine-relationship");
    }

    // 3. Delete machine and verify cleanup
    window.debugDeleteMachine(imported);
    QApplication::processEvents();

    // Verify deleted session is no longer focused
    if (window.focusedView() != nullptr && window.focusedView()->session() == imported) {
        std::fprintf(stderr, "FAIL: deleted machine is still focused after deletion\n");
        return 1;
    }

    // 4. Restore focus to loginPane
    if (loginPane != nullptr) {
        window.debugFocusView(loginPane);
        QApplication::processEvents();
    }

    std::printf("PASS: gui-probe scenario machine-relationship (import relative path, rename synchronization, safe deletion & view unbind)\n");
    return 0;
}

// Scenario "open-project-load-failure": a hand-broken .sdm with "color": "Teal"
// (outside ElementColor's set) must surface as a modal QMessageBox::warning,
// not a status-bar line. The load is async, so the 0-ms QTimer is armed before
// the trigger and keeps ticking through the nested loop QMessageBox::exec()
// opens. The window keeps its pre-attempt state.
int runOpenProjectLoadFailureScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running Open Project load-failure scenario...\n");

    if (loginPane == nullptr || loginPane->session() == nullptr) {
        std::fprintf(stderr, "FAIL: open-project-load-failure scenario has no focused session to compare against\n");
        return 1;
    }
    const int sessionsBefore = window.debugSessionCount();
    const QString machineNameBefore = loginPane->session()->machineName();

    // ---- 1. Write a valid scratch project, then hand-corrupt its one
    // ---- machine's color (an enum string outside the family's set) -----------
    const QString projectPath =
        QDir::temp().filePath(QStringLiteral("ordo-state-designer-probe-open-project-load-failure.sdp"));
    const QString machineRelativePath = QStringLiteral("color-failure.sdm");
    const QString machineFilePath = QFileInfo(projectPath).dir().filePath(machineRelativePath);
    QFile::remove(projectPath);
    QFile::remove(machineFilePath);

    app::Machine machine;
    machine.name = QStringLiteral("ColorFailure");
    machine.states.push_back(app::State{
        .id = 1, .name = QStringLiteral("S1"), .kind = app::StateKind::Normal, .color = app::ElementColor::Blue});
    machine.initialStateId = 1;

    app::MachineFileEntry entry{.relativePath = machineRelativePath, .name = machine.name, .machine = machine};
    app::Project project;
    project.name = QFileInfo(projectPath).completeBaseName();
    project.machineFiles = {entry.relativePath};

    QString writeError;
    if (!app::writeProjectFiles(projectPath, project, {entry}, nullptr, &writeError)) {
        std::fprintf(stderr, "FAIL: writeProjectFiles could not seed the scratch project: %s\n",
                     qUtf8Printable(writeError));
        return 1;
    }

    {
        QFile machineFile(machineFilePath);
        if (!machineFile.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "FAIL: could not reopen the scratch machine file to corrupt its color\n");
            return 1;
        }
        QJsonDocument machineDoc = QJsonDocument::fromJson(machineFile.readAll());
        machineFile.close();
        QJsonObject machineObj = machineDoc.object();
        QJsonArray statesArr = machineObj.value(QStringLiteral("states")).toArray();
        if (statesArr.isEmpty() ||
            statesArr.first().toObject().value(QStringLiteral("color")).toString() != QStringLiteral("Blue")) {
            std::fprintf(stderr, "FAIL: the scratch machine's on-disk shape is not what this scenario expects\n");
            return 1;
        }
        QJsonObject state0 = statesArr.first().toObject();
        state0[QStringLiteral("color")] = QStringLiteral("Teal");  // outside ElementColor's set on purpose
        statesArr.replace(0, state0);
        machineObj[QStringLiteral("states")] = statesArr;
        machineDoc.setObject(machineObj);
        QFile machineFileOut(machineFilePath);
        if (!machineFileOut.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::fprintf(stderr, "FAIL: could not rewrite the scratch machine file with its corrupted color\n");
            return 1;
        }
        machineFileOut.write(machineDoc.toJson(QJsonDocument::Indented));
        machineFileOut.close();
    }

    // ---- 2. Arm the modal driver, then trigger the (async) load -----------
    struct ModalCatch {
        QTimer* timer = nullptr;
        std::shared_ptr<bool> seen;
        std::shared_ptr<QString> text;
        std::shared_ptr<QString> title;
    };
    ModalCatch caught{new QTimer(&window), std::make_shared<bool>(false), std::make_shared<QString>(),
                      std::make_shared<QString>()};
    QTimer* timer = caught.timer;
    timer->setInterval(0);
    QObject::connect(
        timer, &QTimer::timeout, timer,
        [timer, seen = caught.seen, text = caught.text, title = caught.title] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (box == nullptr) {
                return;
            }
            timer->stop();
            *seen = true;
            *text = box->text();
            *title = box->windowTitle();
            box->close();
        });
    timer->start();

    window.debugOpenProjectAt(projectPath);
    for (int waited = 0; waited < 4000 && !*caught.seen; waited += 50) {
        pumpEventsFor(50);
    }
    delete timer;

    if (!*caught.seen) {
        std::fprintf(stderr, "FAIL: Open Project's load failure never showed a modal within the timeout\n");
        return 1;
    }
    std::printf("[PROBE] Open Project failure modal ('%s'): %s\n", qUtf8Printable(*caught.title),
               qUtf8Printable(*caught.text));
    if (*caught.title != QStringLiteral("Open Project")) {
        std::fprintf(stderr, "FAIL: the failure modal's title is '%s', want 'Open Project'\n",
                     qUtf8Printable(*caught.title));
        return 1;
    }
    if (!caught.text->contains(QStringLiteral("invalid color 'Teal'")) ||
        !caught.text->contains(QStringLiteral("Supported colors:"))) {
        std::fprintf(stderr, "FAIL: the failure modal's text does not name the bad color/supported list: '%s'\n",
                     qUtf8Printable(*caught.text));
        return 1;
    }

    // ---- 3. No half-opened project: same session count, same machine ------
    QApplication::processEvents();
    if (window.debugSessionCount() != sessionsBefore) {
        std::fprintf(stderr, "FAIL: session count changed from %d to %d after the failed open\n", sessionsBefore,
                     window.debugSessionCount());
        return 1;
    }
    if (window.focusedView() == nullptr || window.focusedView()->session() == nullptr ||
        window.focusedView()->session()->machineName() != machineNameBefore) {
        std::fprintf(stderr, "FAIL: the focused session's machine changed after the failed open (want '%s')\n",
                     qUtf8Printable(machineNameBefore));
        return 1;
    }

    std::printf(
        "PASS: gui-probe scenario open-project-load-failure (fail-fast color diagnostic reaches the user as a "
        "modal, no half-opened project)\n");
    return 0;
}

// Scenario "open-recent": writes a scratch project via writeProjectFiles(),
// touches recentProjects_ as the success callback would, closes it, and reopens
// it through the real File > Open Recent QAction (the async load is awaited with
// pumpEventsFor). A never-written path opened the same way must be removed by
// the failure callback. Wipes sessions_, so it runs last.
int runOpenRecentScenario(app::MainWindow& window, app::EditorView* loginPane) {
    std::printf("[PROBE] Running Open Recent scenario...\n");

    auto normalizedPath = [](const QString& path) {
        return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    };

    if (loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: open-recent scenario has no focused pane to seed a project from\n");
        return 1;
    }
    app::DocumentSession* session = loginPane->session();
    if (session == nullptr) {
        std::fprintf(stderr, "FAIL: open-recent scenario's focused pane has no session\n");
        return 1;
    }
    auto doc = session->kernel().agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr) {
        std::fprintf(stderr, "FAIL: open-recent scenario's session has no MachineDocAgent\n");
        return 1;
    }

    // ---- 1. Write a project straight through the same free function Save
    // ---- Project As uses, then touch() the way its success callback does --
    const QString projectPath =
        QDir::temp().filePath(QStringLiteral("ordo-state-designer-probe-open-recent.sdp"));
    QFile::remove(projectPath);

    const QString machineRelativePath = app::sanitizeSnakeCase(session->machineName()) + QStringLiteral(".sdm");
    const QString machineFilePath = QFileInfo(projectPath).dir().filePath(machineRelativePath);
    QFile::remove(machineFilePath);

    const QString savedMachineName = session->machineName();
    app::MachineFileEntry entry{
        .relativePath = machineRelativePath,
        .name = savedMachineName,
        .machine = doc->machine()
    };

    app::Project project;
    project.name = QFileInfo(projectPath).completeBaseName();
    project.machineFiles = {entry.relativePath};

    QString writeError;
    if (!app::writeProjectFiles(projectPath, project, {entry}, nullptr, &writeError)) {
        std::fprintf(stderr, "FAIL: writeProjectFiles could not seed the scratch project: %s\n",
                     qUtf8Printable(writeError));
        return 1;
    }

    window.debugRecentProjects().touch(projectPath);
    const QString normalizedProjectPath = window.debugRecentProjects().paths().value(0);
    if (normalizedProjectPath != normalizedPath(projectPath)) {
        std::fprintf(stderr, "FAIL: touch() did not place the saved project at the front of the recent list\n");
        return 1;
    }

    // ---- 2. Close, then reopen through the real File > Open Recent QAction --
    if (!window.closeProject(/*promptIfDirty=*/false)) {
        std::fprintf(stderr, "FAIL: closeProject(false) refused to close (a modal must have appeared)\n");
        return 1;
    }
    if (window.debugSessionCount() != 0) {
        std::fprintf(stderr, "FAIL: closeProject(false) left %d session(s) open, want 0\n",
                     window.debugSessionCount());
        return 1;
    }
    // An explicit close is what tells the next launch not to restore.
    if (!window.debugRecentProjects().lastActiveProject().isEmpty()) {
        std::fprintf(stderr, "FAIL: closeProject() did not clear lastActiveProject\n");
        return 1;
    }

    QMenu* openRecentMenu = window.debugOpenRecentMenu();
    if (openRecentMenu == nullptr) {
        std::fprintf(stderr, "FAIL: debugOpenRecentMenu returned null\n");
        return 1;
    }
    // Calling the (public) signal directly runs every connected slot
    // (MainWindow::rebuildOpenRecentMenu) synchronously, as Qt does right
    // before showing the submenu.
    emit openRecentMenu->aboutToShow();

    QAction* reopenAction = nullptr;
    for (QAction* action : openRecentMenu->actions()) {
        if (action->toolTip() == normalizedProjectPath) {
            reopenAction = action;
            break;
        }
    }
    if (reopenAction == nullptr) {
        std::fprintf(stderr, "FAIL: no Open Recent QAction found for %s\n",
                     qUtf8Printable(normalizedProjectPath));
        return 1;
    }
    if (!reopenAction->isEnabled()) {
        std::fprintf(stderr, "FAIL: the Open Recent entry for the saved project is disabled\n");
        return 1;
    }

    // sessions_ is empty (asserted above), so the "replaces every open
    // machine?" confirmation cannot fire.
    reopenAction->trigger();

    bool sessionsRestored = false;
    for (int waited = 0; waited < 4000 && !sessionsRestored; waited += 50) {
        pumpEventsFor(50);
        sessionsRestored = window.debugSessionCount() > 0;
    }
    if (!sessionsRestored) {
        std::fprintf(stderr,
                     "FAIL: reopening via the Open Recent menu action did not restore any session within the "
                     "timeout\n");
        return 1;
    }
    if (window.focusedView() == nullptr || window.focusedView()->session() == nullptr ||
        window.focusedView()->session()->machineName() != savedMachineName) {
        std::fprintf(stderr, "FAIL: the reopened session's machine name does not match what was saved\n");
        return 1;
    }
    if (!window.debugRecentProjects().isLastActiveProject(projectPath)) {
        std::fprintf(stderr, "FAIL: a successful open did not record the project as lastActiveProject\n");
        return 1;
    }

    // ---- 3. A missing-file entry is dropped after a failed open attempt -----
    const QString missingPath =
        QDir::temp().filePath(QStringLiteral("ordo-state-designer-probe-open-recent-missing.sdp"));
    QFile::remove(missingPath);  // must genuinely not exist

    window.debugRecentProjects().touch(missingPath);
    const QString normalizedMissingPath = window.debugRecentProjects().paths().value(0);
    if (normalizedMissingPath != normalizedPath(missingPath)) {
        std::fprintf(stderr, "FAIL: touch() did not add the missing-file path to the recent list\n");
        return 1;
    }

    // Close again so the missing-file trigger below also skips the
    // "replaces every open machine?" confirmation.
    if (!window.closeProject(/*promptIfDirty=*/false) || window.debugSessionCount() != 0) {
        std::fprintf(stderr, "FAIL: second closeProject(false) before the missing-file attempt failed\n");
        return 1;
    }

    emit openRecentMenu->aboutToShow();
    QAction* missingAction = nullptr;
    for (QAction* action : openRecentMenu->actions()) {
        if (action->toolTip() == normalizedMissingPath) {
            missingAction = action;
            break;
        }
    }
    if (missingAction == nullptr) {
        std::fprintf(stderr, "FAIL: no Open Recent QAction found for the missing-file entry\n");
        return 1;
    }
    missingAction->trigger();

    bool entryRemoved = false;
    for (int waited = 0; waited < 4000 && !entryRemoved; waited += 50) {
        pumpEventsFor(50);
        entryRemoved = !window.debugRecentProjects().paths().contains(normalizedMissingPath);
    }
    if (!entryRemoved) {
        std::fprintf(stderr, "FAIL: opening a missing-file recent entry did not remove it within the timeout\n");
        return 1;
    }
    if (window.debugSessionCount() != 0) {
        std::fprintf(stderr, "FAIL: the failed missing-file open unexpectedly left %d session(s) open\n",
                     window.debugSessionCount());
        return 1;
    }

    // ---- 4. lastActiveProject bookkeeping ------------------------------------
    // Waits until the async open settles: either sessions appear or the path
    // leaves the recent list (the failure callback's side effect).
    auto waitForOpenToSettle = [&window](const QString& path, bool expectSuccess) {
        for (int waited = 0; waited < 4000; waited += 50) {
            pumpEventsFor(50);
            if (expectSuccess ? window.debugSessionCount() > 0
                              : !window.debugRecentProjects().paths().contains(
                                    QDir::cleanPath(QFileInfo(path).absoluteFilePath()), Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
    };

    // 4a. Project A open and recorded; a failed open of some OTHER path B must
    // leave A's record alone.
    window.debugOpenProjectAt(projectPath);
    if (!waitForOpenToSettle(projectPath, /*expectSuccess=*/true) ||
        !window.debugRecentProjects().isLastActiveProject(projectPath)) {
        std::fprintf(stderr, "FAIL: re-opening the scratch project did not record it as lastActiveProject\n");
        return 1;
    }
    const QString otherMissing =
        QDir::temp().filePath(QStringLiteral("ordo-state-designer-probe-restore-other-missing.sdp"));
    QFile::remove(otherMissing);
    window.debugRecentProjects().touch(otherMissing);
    window.debugOpenProjectAt(otherMissing);
    if (!waitForOpenToSettle(otherMissing, /*expectSuccess=*/false)) {
        std::fprintf(stderr, "FAIL: the failed open of an unrelated missing path never settled\n");
        return 1;
    }
    if (!window.debugRecentProjects().isLastActiveProject(projectPath)) {
        std::fprintf(stderr,
                     "FAIL: a failed open of an unrelated path cleared the open project's lastActiveProject\n");
        return 1;
    }

    // 4b. MCP close_project reaches closeProject() without the menu and must
    // clear the record too.
    {
        app::McpRuntimeServer server(&window);
        QJsonObject req;
        req[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
        req[QStringLiteral("id")] = 1;
        req[QStringLiteral("method")] = QStringLiteral("close_project");
        req[QStringLiteral("params")] = QJsonObject();
        (void)server.handleRpcRequest(req);
    }
    if (!window.debugRecentProjects().lastActiveProject().isEmpty()) {
        std::fprintf(stderr, "FAIL: MCP close_project did not clear lastActiveProject\n");
        return 1;
    }

    // 4c. A failed restore clears the record, even when the failing path is
    // spelled differently (upper-cased file name) from the recorded one; the
    // comparison goes through RecentProjects' normalization.
    const QString restoreMissing =
        QDir::temp().filePath(QStringLiteral("ordo-state-designer-probe-restore-missing.sdp"));
    QFile::remove(restoreMissing);
    window.debugRecentProjects().setLastActiveProject(restoreMissing);
    window.debugRecentProjects().touch(restoreMissing);
    const QString restoreMissingOtherSpelling =
        QFileInfo(restoreMissing).dir().filePath(QFileInfo(restoreMissing).fileName().toUpper());
    window.debugOpenProjectAt(restoreMissingOtherSpelling);
    if (!waitForOpenToSettle(restoreMissing, /*expectSuccess=*/false)) {
        std::fprintf(stderr, "FAIL: the failed restore-style open never settled\n");
        return 1;
    }
#if defined(Q_OS_WIN)
    if (!window.debugRecentProjects().lastActiveProject().isEmpty()) {
        std::fprintf(stderr,
                     "FAIL: a failed open of the recorded project (different case) did not clear lastActiveProject\n");
        return 1;
    }
#endif

    // ---- Cleanup --------------------------------------------------------------
    QFile::remove(projectPath);
    QFile::remove(machineFilePath);

    std::printf(
        "PASS: gui-probe scenario open-recent (save touches the list; close -> reopen via the real Open Recent "
        "QAction restores sessions; a missing-file entry is removed after a failed open via the same menu "
        "path; lastActiveProject set on open, kept on an unrelated failure, cleared by closeProject/MCP "
        "close_project and by a failed open of the recorded path)\n");
    return 0;
}

// Scenario "inspector-empty-state": after Close Project the Inspector must show
// its empty page and the Trace panel 0 rows, and binding a fresh machine leaves
// the empty page (showTabForSelection() never re-selects it). `loginPane`
// dangles after runOpenRecentScenario's closeProject() calls, so this mints its
// own session; Close Project wipes sessions_, so it runs last.
int runInspectorEmptyStateScenario(app::MainWindow& window, app::EditorView* /*loginPane*/) {
    std::printf("[PROBE] Running Inspector empty-state scenario...\n");

    app::DocumentSession* session = window.createNewMachine(QStringLiteral("Empty State Probe"));
    if (session == nullptr) {
        std::fprintf(stderr, "FAIL: inspector-empty-state scenario could not create a fresh machine\n");
        return 1;
    }
    app::EditorView* view = window.focusedView();
    if (view == nullptr || view->session() != session) {
        std::fprintf(stderr, "FAIL: inspector-empty-state scenario's new machine did not become the focused view\n");
        return 1;
    }

    auto& kernel = session->kernel();
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (doc == nullptr) {
        std::fprintf(stderr, "FAIL: inspector-empty-state scenario's new session has no MachineDocAgent\n");
        return 1;
    }
    const quint64 stateId = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(400.0, 300.0)});
    QApplication::processEvents();

    window.debugSelectState(view, stateId);
    QApplication::processEvents();

    app::InspectorPanel* inspector = window.debugInspector();
    app::TracePanel* trace = window.debugTracePanel();
    if (inspector == nullptr || trace == nullptr) {
        std::fprintf(stderr, "FAIL: inspector-empty-state scenario found no Inspector/Trace panel\n");
        return 1;
    }
    if (inspector->debugIsShowingEmptyState()) {
        std::fprintf(stderr, "FAIL: the Inspector already reads empty before Close Project even ran\n");
        return 1;
    }

    // A genuine trace line (activate()'s "Run -> <name>"), so the 0-rows assert
    // below is not vacuous. Back to Design afterwards, since items lose
    // ItemIsSelectable in Simulate. Settle between kinds of edit: an unbroken
    // send() batch drains later and can overflow the offscreen paint recursion
    // in an unrelated scenario's capture.
    kernel.send(app::events::SetInitialStateRequested{.id = stateId});
    QApplication::processEvents();
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    QApplication::processEvents();
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    QApplication::processEvents();
    if (trace->rowCount() == 0) {
        std::fprintf(stderr, "FAIL: inspector-empty-state scenario's own Run did not produce a Trace row to clear\n");
        return 1;
    }

    // ---- Close Project (no-prompt path): the no-session branch of
    // ---- rebindPanelsToFocusedSession() must leave an explicit empty state,
    // ---- not the just-selected state's stale fields.
    if (!window.closeProject(/*promptIfDirty=*/false)) {
        std::fprintf(stderr, "FAIL: closeProject(false) refused to close (a modal must have appeared)\n");
        return 1;
    }
    if (window.debugSessionCount() != 0) {
        std::fprintf(stderr, "FAIL: closeProject(false) left %d session(s) open, want 0\n",
                     window.debugSessionCount());
        return 1;
    }
    QApplication::processEvents();

    if (!inspector->debugIsShowingEmptyState()) {
        std::fprintf(stderr, "FAIL: Close Project did not leave the Inspector on its empty-state page\n");
        return 1;
    }
    if (trace->rowCount() != 0) {
        std::fprintf(stderr, "FAIL: Close Project left %d stale row(s) in the Trace panel, want 0\n",
                     trace->rowCount());
        return 1;
    }

    if (!saveWidgetCapture(inspector, "probe-inspector-empty-state")) {
        return 1;
    }

    // ---- Binding a machine again must leave the empty page --------------------
    if (window.createNewMachine(QStringLiteral("Empty State Probe (rebind)")) == nullptr) {
        std::fprintf(stderr, "FAIL: inspector-empty-state scenario could not create the rebind machine\n");
        return 1;
    }
    QApplication::processEvents();
    if (inspector->debugIsShowingEmptyState()) {
        std::fprintf(stderr, "FAIL: binding a fresh machine did not leave the Inspector's empty-state page\n");
        return 1;
    }

    std::printf(
        "PASS: gui-probe scenario inspector-empty-state (select a state -> Close Project -> Inspector empty page + "
        "Trace 0 rows -> new machine leaves the empty page)\n");
    return 0;
}


// Scenario "mcp-batch-paint": MCP batch_create_machine authors a whole machine
// with no event-loop turn between the sends, on a session open in a canvas, so
// items are created, reparented and re-routed before the scene index catches
// up. Each round forces a real paint (window grab), and every other round wipes
// all sessions to delete the items in bulk. Under full page heap a stale index
// pointer faults on the first paint that reads it.
int runMcpBatchPaintScenario(app::MainWindow& window) {
    std::printf("[PROBE] Running MCP batch -> paint scenario...\n");
    app::McpRuntimeServer server(&window);

    constexpr int kRounds = 6;
    constexpr int kParents = 4;
    constexpr int kChildren = 6;
    for (int round = 0; round < kRounds; ++round) {
        QJsonArray states;
        QJsonArray transitions;
        for (int p = 0; p < kParents; ++p) {
            const QString parentName = QStringLiteral("P%1").arg(p);
            QJsonObject parent;
            parent[QStringLiteral("name")] = parentName;
            parent[QStringLiteral("x")] = 80.0 + p * 420.0;
            parent[QStringLiteral("y")] = 80.0;
            states.append(parent);
            for (int c = 0; c < kChildren; ++c) {
                QJsonObject child;
                child[QStringLiteral("name")] = QStringLiteral("P%1C%2").arg(p).arg(c);
                child[QStringLiteral("x")] = 110.0 + p * 420.0 + (c % 2) * 170.0;
                child[QStringLiteral("y")] = 160.0 + (c / 2) * 120.0;
                child[QStringLiteral("parent")] = parentName;
                states.append(child);
                if (c > 0) {
                    QJsonObject t;
                    t[QStringLiteral("source")] = QStringLiteral("P%1C%2").arg(p).arg(c - 1);
                    t[QStringLiteral("target")] = QStringLiteral("P%1C%2").arg(p).arg(c);
                    t[QStringLiteral("event")] = QStringLiteral("NEXT_%1").arg(c);
                    t[QStringLiteral("guard")] = QStringLiteral("ready");
                    t[QStringLiteral("action")] = QStringLiteral("step()");
                    transitions.append(t);
                }
            }
            if (p > 0) {
                QJsonObject t;
                t[QStringLiteral("source")] = QStringLiteral("P%1").arg(p - 1);
                t[QStringLiteral("target")] = parentName;
                t[QStringLiteral("event")] = QStringLiteral("HOP");
                transitions.append(t);
            }
        }
        QJsonObject params;
        params[QStringLiteral("name")] = QStringLiteral("BatchPaint%1").arg(round);
        params[QStringLiteral("states")] = states;
        params[QStringLiteral("transitions")] = transitions;
        params[QStringLiteral("initialState")] = QStringLiteral("P0");

        QJsonObject req;
        req[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
        req[QStringLiteral("id")] = round + 1;
        req[QStringLiteral("method")] = QStringLiteral("batch_create_machine");
        req[QStringLiteral("params")] = params;
        const QJsonObject resp = server.handleRpcRequest(req);
        if (!resp.value(QStringLiteral("result")).toObject().value(QStringLiteral("success")).toBool(false)) {
            std::fprintf(stderr, "FAIL: mcp-batch-paint round %d: batch_create_machine did not succeed\n", round);
            return 1;
        }

        // No settle before the paint: this is exactly what a batch hands the view.
        (void)window.grab();
        QApplication::processEvents();
        (void)window.grab();

        // Simulation churn: every step moves the active state, which swaps the
        // active-glow QGraphicsDropShadowEffect on/off StateItems -- the one
        // effective-bounding-rect change every observed crash had in common.
        const QStringList steps = {QStringLiteral("HOP"), QStringLiteral("HOP"), QStringLiteral("HOP")};
        int stepId = 1000 + round * 10;
        for (const QString& event : steps) {
            QJsonObject stepParams;
            stepParams[QStringLiteral("event")] = event;
            QJsonObject stepReq;
            stepReq[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
            stepReq[QStringLiteral("id")] = stepId++;
            stepReq[QStringLiteral("method")] = QStringLiteral("step_simulation");
            stepReq[QStringLiteral("params")] = stepParams;
            (void)server.handleRpcRequest(stepReq);
            (void)window.grab();
        }
        QJsonObject resetReq;
        resetReq[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
        resetReq[QStringLiteral("id")] = stepId++;
        resetReq[QStringLiteral("method")] = QStringLiteral("reset_simulation");
        resetReq[QStringLiteral("params")] = QJsonObject();
        (void)server.handleRpcRequest(resetReq);
        (void)window.grab();

        if (round % 2 == 1) {
            window.closeProject(/*promptIfDirty=*/false);
            QApplication::processEvents();
            (void)window.grab();
        }
    }

    std::printf(
        "PASS: gui-probe scenario mcp-batch-paint (%d rounds of an unsettled hierarchical MCP batch, each painted "
        "immediately, bulk-closed every other round)\n",
        kRounds);
    return 0;
}

// Scenario "app-icon": the multi-size QIcon carries the sizes Windows asks for,
// a top-level window picks it up (windowIcon() falls back to the application
// icon, so the cacheKey()s match), and the 256px pixmap is the mark's shape,
// sampled at the ring's hole (transparent) and mid-ring (opaque).
int runAppIconScenario(app::MainWindow& window) {
    std::printf("[PROBE] Running app-icon scenario...\n");
    int exitCode = 0;

    const QIcon appIcon = QApplication::windowIcon();
    if (appIcon.isNull()) {
        std::fprintf(stderr, "FAIL: app-icon scenario found a null QApplication::windowIcon()\n");
        return 1;
    }
    const QList<QSize> sizes = appIcon.availableSizes();
    if (!sizes.contains(QSize(16, 16)) || !sizes.contains(QSize(256, 256))) {
        std::fprintf(stderr,
                     "FAIL: app-icon scenario's QApplication::windowIcon() is missing 16x16 and/or 256x256 "
                     "(has %d size(s))\n",
                     static_cast<int>(sizes.size()));
        exitCode = 1;
    }

    if (window.windowIcon().cacheKey() != appIcon.cacheKey()) {
        std::fprintf(stderr,
                     "FAIL: app-icon scenario's window icon (cacheKey %llu) does not share the application "
                     "icon (cacheKey %llu)\n",
                     static_cast<unsigned long long>(window.windowIcon().cacheKey()),
                     static_cast<unsigned long long>(appIcon.cacheKey()));
        exitCode = 1;
    }

    // Shape check: the mark is a ring centred on the icon; its centre is the
    // hole (alpha 0) and a point mid-ring, on one of the four filled segments,
    // is opaque (alpha 255). At viewBox 1024 (outer radius 384, inner 268) the
    // mid-radius is (384+268)/2 = 326, i.e. 81.5px at this 256px render; a
    // point at that radius on the 45-degree diagonal sits inside a filled
    // segment, clear of the axis-aligned gaps between the four paths.
    const QPixmap pixmap = appIcon.pixmap(QSize(256, 256));
    if (pixmap.size() != QSize(256, 256)) {
        std::fprintf(stderr, "FAIL: app-icon scenario's 256px pixmap came back %dx%d\n", pixmap.width(),
                     pixmap.height());
        exitCode = 1;
    } else {
        const QImage image = pixmap.toImage();
        const QColor hole = image.pixelColor(128, 128);
        if (hole.alpha() != 0) {
            std::fprintf(stderr,
                         "FAIL: app-icon scenario's centre pixel (128,128) has alpha %d, want 0 (the ring's "
                         "hole)\n",
                         hole.alpha());
            exitCode = 1;
        }
        const int offset = 58;  // ~81.5px mid-ring radius at 45 degrees (58 * sqrt(2) ~= 82)
        const QColor ring = image.pixelColor(128 + offset, 128 - offset);
        if (ring.alpha() != 255) {
            std::fprintf(stderr, "FAIL: app-icon scenario's ring pixel (%d,%d) has alpha %d, want 255\n",
                         128 + offset, 128 - offset, ring.alpha());
            exitCode = 1;
        }
    }

    if (exitCode == 0) {
        std::printf(
            "PASS: gui-probe scenario app-icon (application + window icon share the brand mark at 16px and "
            "256px, 256px pixmap shows the ring's hole and body)\n");
    }
    return exitCode;
}
