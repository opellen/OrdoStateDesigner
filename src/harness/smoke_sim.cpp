// --smoke phases: the sim interpreter family -- the three flat sim smokes
// (topology interpreter, targetless-self, root-event fallback), hierarchical
// execution semantics, History (shallow/deep) semantics, and Back() replay
// hardening.

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
#include "view/generated/canvas_interaction_core.h"  // phase 5b drives the machine directly
#include "view/shell/document_session.h"
#include "view/shell/editor_view.h"
#include "view/shell/inspector_panel.h"
#include "view/shell/main_window.h"
#include "view/items/machine_frame_item.h"
#include "view/shell/minimap_view.h"
#include "view/items/note_item.h"
#include "view/geometry/pill_port_resolver.h"  // phase 8 asserts the rule table 1:1
#include "view/items/state_item.h"
#include "view/shell/theme.h"
#include "view/shell/trace_panel.h"
#include "view/items/transition_item.h"

#include "harness/harness.h"

// Topology interpreter drive. A dedicated kernel/doc/sim with no UndoStore
// (sim runtime state is never undo-worthy). Builds a 3-state machine
// (Idle -Start[guard canWork]-> Working -(1500ms delayed)-> Done) via the
// edit intents, then drives the run/pause/reset/send-event/guard/tick/back
// contract. SimClock stays in manual mode, so advanceTicks() drives every tick
// synchronously and no QApplication is needed.
int runSimSmoke() {
    ordo::core::Kernel kernel;
    kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
    kernel.registerAgent(std::make_shared<app::SimulationAgent>());
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (!doc || !sim) {
        std::fprintf(stderr, "FAIL: sim-phase agents did not all register under kName\n");
        return 1;
    }

    registerEditCommands(kernel);

    // ---- build the 3-state machine: Idle -Start[guard canWork]-> Working -(1500ms delayed)-> Done ----
    kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});     // id 1: Idle
    kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});   // id 2: Working
    kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});   // id 3: Done
    kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Idle")});
    kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Working")});
    kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Done")});
    kernel.send(app::events::SetInitialStateRequested{.id = 1});
    kernel.send(app::events::SetStateKindRequested{.id = 3, .kind = app::StateKind::Final});

    kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 4: Idle -> Working
    kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 5: Working -> Done (delayed)
    kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Start")});
    kernel.send(app::events::SetTransitionGuardRequested{.id = 4, .guard = QStringLiteral("canWork")});
    kernel.send(app::events::SetTransitionDelayRequested{.id = 5, .delayMs = 1500});
    // Transition 5's event stays blank: delayMs > 0 + a blank event is a
    // pure delayed transition.

    if (doc->machine().states.size() != 3 || doc->machine().transitions.size() != 2) {
        std::fprintf(stderr, "FAIL: sim-phase topology build did not produce 3 states / 2 transitions\n");
        return 1;
    }
    const quint64 idleId = 1;
    const quint64 workingId = 2;
    const quint64 doneId = 3;
    const quint64 startTransitionId = 4;
    const quint64 delayedTransitionId = 5;

    // Simulate mode rejects every edit intent (covered by runSmoke()'s policy
    // test), so drop the edit-command registration rather than leave it inert.
    removeEditCommands(kernel);

    app::SimClock clock;
    clock.setManualMode(true);
    clock.onTick = [&kernel](int elapsedMs) { kernel.send(app::events::TickElapsed{.elapsedMs = elapsedMs}); };
    registerSimCommands(kernel, clock);

    int obsOwner = 0;
    std::vector<app::events::ActiveStateChanged> activeChanges;
    std::vector<app::events::TransitionFired> firedEvents;
    kernel.dispatcher().subscribe<app::events::ActiveStateChanged>(
        &obsOwner, [&activeChanges](const app::events::ActiveStateChanged& fact) { activeChanges.push_back(fact); });
    kernel.dispatcher().subscribe<app::events::TransitionFired>(
        &obsOwner, [&firedEvents](const app::events::TransitionFired& fact) { firedEvents.push_back(fact); });

    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    if (sim->mode() != app::events::Mode::Simulate) {
        std::fprintf(stderr, "FAIL: sim-phase SetModeRequested{Simulate} did not flip SimulationAgent::mode()\n");
        return 1;
    }

    // ---- Run -> active=Idle + trace init entry -------------------------------
    kernel.send(app::events::RunRequested{});
    if (!sim->running() || sim->activeStateId() != idleId) {
        std::fprintf(stderr, "FAIL: RunRequested did not activate Idle (running=%d activeStateId=%llu)\n",
                     sim->running(), static_cast<unsigned long long>(sim->activeStateId()));
        return 1;
    }
    if (sim->trace().isEmpty() || sim->trace().first() != QStringLiteral("init → Idle")) {
        std::fprintf(stderr, "FAIL: RunRequested did not append an 'init -> Idle' trace entry\n");
        return 1;
    }

    // ---- SendEvent(Start), guard default-true -> Working ---------------------
    activeChanges.clear();
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Start")});
    if (sim->activeStateId() != workingId) {
        std::fprintf(stderr, "FAIL: SendEvent(Start) with default-true guard did not reach Working\n");
        return 1;
    }
    if (activeChanges.empty() || activeChanges.back().toId != workingId ||
        activeChanges.back().viaTransitionId != startTransitionId) {
        std::fprintf(stderr, "FAIL: SendEvent(Start) did not publish the expected ActiveStateChanged\n");
        return 1;
    }

    // ---- advanceTicks(1000) -> still Working (1500ms delay not yet due) -----
    clock.advanceTicks(1000);
    if (sim->activeStateId() != workingId) {
        std::fprintf(stderr, "FAIL: advanceTicks(1000) fired the delayed transition early\n");
        return 1;
    }

    // ---- advanceTicks(500) -> Done + TransitionFired -------------------------
    firedEvents.clear();
    clock.advanceTicks(500);
    if (sim->activeStateId() != doneId) {
        std::fprintf(stderr, "FAIL: advanceTicks(500) (1500ms total) did not fire the delayed transition to Done\n");
        return 1;
    }
    if (firedEvents.empty() || firedEvents.back().transitionId != delayedTransitionId) {
        std::fprintf(stderr, "FAIL: the delayed transition's firing did not publish TransitionFired\n");
        return 1;
    }

    // ---- Reset -> Idle again (running stays true) ---
    kernel.send(app::events::ResetRequested{});
    if (sim->activeStateId() != idleId || !sim->running()) {
        std::fprintf(stderr, "FAIL: ResetRequested did not return to a running Idle\n");
        return 1;
    }

    // ---- guard=false path: SendEvent(Start) ignored, no fact -----------------
    kernel.send(app::events::SetGuardResultRequested{.name = QStringLiteral("canWork"), .result = false});
    activeChanges.clear();
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Start")});
    if (sim->activeStateId() != idleId) {
        std::fprintf(stderr, "FAIL: SendEvent(Start) fired despite canWork=false\n");
        return 1;
    }
    if (!activeChanges.empty()) {
        std::fprintf(stderr, "FAIL: SendEvent(Start) with a false guard still published ActiveStateChanged\n");
        return 1;
    }
    kernel.send(app::events::SetGuardResultRequested{.name = QStringLiteral("canWork"), .result = true});

    // ---- Back: after two firings, state equals the post-first-firing state ---
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Start")});  // firing 1: Idle -> Working
    if (sim->activeStateId() != workingId) {
        std::fprintf(stderr, "FAIL: SendEvent(Start) (guard restored to true) did not reach Working\n");
        return 1;
    }
    clock.advanceTicks(1500);  // firing 2: Working -> Done (delayed)
    if (sim->activeStateId() != doneId) {
        std::fprintf(stderr, "FAIL: advanceTicks(1500) did not fire the delayed transition to Done\n");
        return 1;
    }
    kernel.send(app::events::BackRequested{});
    if (sim->activeStateId() != workingId) {
        std::fprintf(stderr, "FAIL: BackRequested after 2 firings did not land on the post-first-firing state\n");
        return 1;
    }

    // ---- Pause freezes: partial tick, Pause, advanceTicks while paused is a no-op, Run resumes, remaining time fires ----
    clock.advanceTicks(400);  // 1500ms armed (re-armed by Back's replay) - 400ms = 1100ms remaining
    if (sim->activeStateId() != workingId) {
        std::fprintf(stderr, "FAIL: advanceTicks(400) fired the delayed transition early after Back\n");
        return 1;
    }
    kernel.send(app::events::PauseRequested{});
    if (sim->running()) {
        std::fprintf(stderr, "FAIL: PauseRequested did not stop running()\n");
        return 1;
    }
    clock.advanceTicks(5000);
    if (sim->activeStateId() != workingId) {
        std::fprintf(stderr, "FAIL: advanceTicks(5000) while paused advanced the armed countdown\n");
        return 1;
    }
    kernel.send(app::events::RunRequested{});
    if (!sim->running()) {
        std::fprintf(stderr, "FAIL: RunRequested did not resume after Pause\n");
        return 1;
    }
    clock.advanceTicks(1100);  // exactly the remaining time (1500 - 400)
    if (sim->activeStateId() != doneId) {
        std::fprintf(stderr, "FAIL: advanceTicks(1100) after resuming did not fire after the remaining time\n");
        return 1;
    }

    // ---- Breakpoint toggle, persistence, hit-pause, and resume ----
    std::vector<app::events::BreakpointHit> bpHits;
    kernel.dispatcher().subscribe<app::events::BreakpointHit>(
        &obsOwner, [&bpHits](const app::events::BreakpointHit& fact) { bpHits.push_back(fact); });

    kernel.send(app::events::ToggleBreakpointRequested{.stateId = workingId});
    if (!sim->hasBreakpoint(workingId)) {
        std::fprintf(stderr, "FAIL: ToggleBreakpointRequested did not arm breakpoint on Working\n");
        return 1;
    }
    // Breakpoints persist across Reset
    kernel.send(app::events::ResetRequested{});
    if (!sim->hasBreakpoint(workingId)) {
        std::fprintf(stderr, "FAIL: Breakpoint on Working did not persist across ResetRequested\n");
        return 1;
    }
    kernel.send(app::events::RunRequested{});
    bpHits.clear();
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Start")});
    if (sim->activeStateId() != workingId) {
        std::fprintf(stderr, "FAIL: SendEvent(Start) did not enter Working\n");
        return 1;
    }
    if (sim->running()) {
        std::fprintf(stderr, "FAIL: Simulation did not auto-pause upon entering breakpoint state Working\n");
        return 1;
    }
    if (bpHits.empty() || bpHits.back().stateId != workingId) {
        std::fprintf(stderr, "FAIL: BreakpointHit fact was not published for Working\n");
        return 1;
    }
    kernel.send(app::events::RunRequested{});
    if (!sim->running()) {
        std::fprintf(stderr, "FAIL: RunRequested did not resume after breakpoint pause\n");
        return 1;
    }
    kernel.send(app::events::ToggleBreakpointRequested{.stateId = workingId});
    if (sim->hasBreakpoint(workingId)) {
        std::fprintf(stderr, "FAIL: ToggleBreakpointRequested did not clear breakpoint on Working\n");
        return 1;
    }

    // ---- Timescale scaling ----
    kernel.send(app::events::SetTimescaleRequested{.scale = 2.0});
    if (clock.timeScale() != 2.0) {
        std::fprintf(stderr, "FAIL: SetTimescaleRequested did not update clock.timeScale()\n");
        return 1;
    }
    kernel.send(app::events::SetTimescaleRequested{.scale = 1.0});
    if (clock.timeScale() != 1.0) {
        std::fprintf(stderr, "FAIL: SetTimescaleRequested(1.0) did not restore clock.timeScale()\n");
        return 1;
    }

    kernel.dispatcher().unsubscribe(&obsOwner);
    removeSimCommands(kernel);

    std::printf("PASS: state-designer sim smoke\n");
    return 0;
}

// Targetless + self-transition interpreter semantics. Own kernel + 3-state machine:
//   A(initial) -Go-> B;  B -Retry-> B (self, entry re-runs + re-arm);
//   B -Log-> (targetless, action logIt);  B -(500ms)-> (targetless, beep);
//   B -(1500ms)-> C.
// A targetless firing leaves the active state and other armed countdowns
// untouched; a self firing re-enters the state (entry actions, countdowns re-armed).
int runTargetlessSelfSimSmoke() {
    ordo::core::Kernel kernel;
    kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
    kernel.registerAgent(std::make_shared<app::SimulationAgent>());
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (!doc || !sim) {
        std::fprintf(stderr, "FAIL: targetless-phase agents did not all register under kName\n");
        return 1;
    }

    registerEditCommands(kernel);
    kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});     // id 1: A
    kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});   // id 2: B
    kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});   // id 3: C
    kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("A")});
    kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("B")});
    kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("C")});
    kernel.send(app::events::SetInitialStateRequested{.id = 1});
    kernel.send(app::events::SetEntryActionsRequested{.id = 2, .entryActions = QStringList{QStringLiteral("enterB()")}});
    kernel.send(app::events::SetExitActionsRequested{.id = 2, .exitActions = QStringList{QStringLiteral("exitB()")}});
    kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 4: Go
    kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 2});  // id 5: Retry (self)
    kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 0});  // id 6: Log (targetless)
    kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 0});  // id 7: 500ms beep (targetless delayed)
    kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 8: 1500ms -> C
    kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Go")});
    kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Retry")});
    kernel.send(app::events::SetTransitionReenterRequested{.id = 5, .reenter = true});
    kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Log")});
    kernel.send(app::events::SetTransitionActionRequested{.id = 6, .action = QStringLiteral("logIt()")});
    kernel.send(app::events::SetTransitionDelayRequested{.id = 7, .delayMs = 500});
    kernel.send(app::events::SetTransitionActionRequested{.id = 7, .action = QStringLiteral("beep()")});
    kernel.send(app::events::SetTransitionDelayRequested{.id = 8, .delayMs = 1500});
    if (doc->machine().transitions.size() != 5) {
        std::fprintf(stderr, "FAIL: targetless-phase topology build did not produce 5 transitions\n");
        return 1;
    }
    removeEditCommands(kernel);

    app::SimClock clock;
    clock.setManualMode(true);
    clock.onTick = [&kernel](int elapsedMs) { kernel.send(app::events::TickElapsed{.elapsedMs = elapsedMs}); };
    registerSimCommands(kernel, clock);

    const auto traceCount = [&sim](const char* needle) {
        int count = 0;
        for (const QString& line : sim->trace()) {
            if (line.contains(QLatin1String(needle))) {
                ++count;
            }
        }
        return count;
    };

    // ---- targetless: fires actions, moves nothing, cancels nothing ----------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Go")});
    if (sim->activeStateId() != 2 || traceCount("enterB()") != 1) {
        std::fprintf(stderr, "FAIL: Go did not land in B with one entry action\n");
        return 1;
    }
    clock.advanceTicks(800);  // t=800: the 500ms targetless beep fired, the 1500ms countdown has 700 left
    if (sim->activeStateId() != 2 || traceCount("beep()") != 1) {
        std::fprintf(stderr, "FAIL: the delayed targetless transition did not fire in place at 500ms\n");
        return 1;
    }
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Log")});
    if (sim->activeStateId() != 2 || traceCount("logIt()") != 1 || traceCount("Log (internal)") != 1 ||
        traceCount("enterB()") != 1 || traceCount("exitB()") != 0) {
        std::fprintf(stderr, "FAIL: targetless Log did not fire action-only (state/entry/exit must be untouched)\n");
        return 1;
    }
    clock.advanceTicks(700);  // completes the ORIGINAL 1500ms -- neither targetless firing disturbed it
    if (sim->activeStateId() != 3) {
        std::fprintf(stderr, "FAIL: the 1500ms countdown was disturbed by targetless firings\n");
        return 1;
    }

    // ---- self: re-enters the state -- entry actions again, re-armed timers ----
    kernel.send(app::events::ResetRequested{});
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Go")});
    clock.advanceTicks(800);  // beep fired; 1500ms countdown at 700 left
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Retry")});
    if (sim->activeStateId() != 2 || traceCount("enterB()") != 2 || traceCount("exitB()") != 1 ||
        traceCount("Retry") != 1) {
        std::fprintf(stderr, "FAIL: self Retry did not exit + re-enter B (v5 firing order)\n");
        return 1;
    }
    clock.advanceTicks(700);  // 700 < the RE-armed 1500 -- without the re-arm this would fire
    if (sim->activeStateId() != 2) {
        std::fprintf(stderr, "FAIL: self Retry did not re-arm B's delayed countdown (fired after 700ms)\n");
        return 1;
    }
    if (traceCount("beep()") != 2) {
        std::fprintf(stderr, "FAIL: the re-armed 500ms targetless did not fire again after Retry\n");
        return 1;
    }
    clock.advanceTicks(800);  // completes the re-armed 1500ms
    if (sim->activeStateId() != 3) {
        std::fprintf(stderr, "FAIL: the re-armed 1500ms countdown did not complete after Retry\n");
        return 1;
    }

    // ---- self (internal, reenter: false): does NOT exit/re-enter, timers untouched ----
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    doc->setTransitionReenter(5, false);
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Go")});
    clock.advanceTicks(800);  // 1500ms countdown has 700 left
    const int enterBBefore = traceCount("enterB()");
    const int exitBBefore = traceCount("exitB()");
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Retry")});
    if (sim->activeStateId() != 2 || traceCount("enterB()") != enterBBefore || traceCount("exitB()") != exitBBefore) {
        std::fprintf(stderr, "FAIL: internal self Retry (reenter: false) must NOT exit or enter B\n");
        return 1;
    }
    clock.advanceTicks(700);  // completes the ORIGINAL 1500ms countdown -- internal self-transition did NOT re-arm it
    if (sim->activeStateId() != 3) {
        std::fprintf(stderr, "FAIL: internal self Retry disturbed B's delayed countdown\n");
        return 1;
    }

    removeSimCommands(kernel);
    std::printf("PASS: state-designer targetless/self sim smoke\n");
    return 0;
}

// Root-event interpreter smoke: machine-level (from == 0) transitions as
// one-hop bubbling -- state handlers first, root group second; a root `after`
// armed at Run/Reset survives state entries; exit actions of the ACTIVE state
// run on a root firing. Also asserts the generated code's root support
// textually (the fallback block exists, stateIdent(0)'s "Unknown" never leaks
// into a switch).
int runRootEventSimSmoke() {
    ordo::core::Kernel kernel;
    kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
    kernel.registerAgent(std::make_shared<app::SimulationAgent>());
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (!doc || !sim) {
        std::fprintf(stderr, "FAIL: root-event phase agents did not all register under kName\n");
        return 1;
    }
    registerEditCommands(kernel);

    // Idle(exit leaveIdle) --Go--> Working(exit leaveWork); root: Go->Working,
    // Home->Idle, Log (targetless, logIt), after-1000->Done; Idle after-300->Working.
    kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});     // id 1: Idle
    kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});   // id 2: Working
    kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});   // id 3: Done
    kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Idle")});
    kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Working")});
    kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Done")});
    kernel.send(app::events::SetInitialStateRequested{.id = 1});
    kernel.send(app::events::SetExitActionsRequested{.id = 1, .exitActions = QStringList{QStringLiteral("leaveIdle")}});
    kernel.send(app::events::SetExitActionsRequested{.id = 2, .exitActions = QStringList{QStringLiteral("leaveWork")}});
    kernel.send(app::events::SetMachineNameRequested{.name = QStringLiteral("RootDemo")});

    kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 4: Idle --Go--> Working
    kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Go")});
    kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 2});  // id 5: root Go -> Working
    kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Go")});
    kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 1});  // id 6: root Home -> Idle
    kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Home")});
    kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 0});  // id 7: root targetless Log
    kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Log")});
    kernel.send(app::events::SetTransitionActionRequested{.id = 7, .action = QStringLiteral("logIt")});
    kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 3});  // id 8: root after 1000ms -> Done
    kernel.send(app::events::SetTransitionDelayRequested{.id = 8, .delayMs = 1000});
    kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 9: Idle after 300ms -> Working
    kernel.send(app::events::SetTransitionDelayRequested{.id = 9, .delayMs = 300});
    kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 0});  // id 10: root #machine Restart
    kernel.send(app::events::SetMachineSelfRequested{.id = 10, .machineSelf = true});
    kernel.send(app::events::SetTransitionEventRequested{.id = 10, .event = QStringLiteral("Restart")});
    if (doc->machine().transitions.size() != 7 || !doc->findTransition(10)->machineSelf) {
        std::fprintf(stderr, "FAIL: root-event topology build did not produce 7 transitions incl. machineSelf\n");
        return 1;
    }
    removeEditCommands(kernel);

    app::SimClock clock;
    clock.setManualMode(true);
    clock.onTick = [&kernel](int elapsedMs) { kernel.send(app::events::TickElapsed{.elapsedMs = elapsedMs}); };
    registerSimCommands(kernel, clock);

    int obsOwner = 0;
    std::vector<app::events::ActiveStateChanged> activeChanges;
    std::vector<app::events::TransitionFired> firedEvents;
    kernel.dispatcher().subscribe<app::events::ActiveStateChanged>(
        &obsOwner, [&activeChanges](const app::events::ActiveStateChanged& fact) { activeChanges.push_back(fact); });
    kernel.dispatcher().subscribe<app::events::TransitionFired>(
        &obsOwner, [&firedEvents](const app::events::TransitionFired& fact) { firedEvents.push_back(fact); });

    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    kernel.send(app::events::RunRequested{});
    if (!sim->running() || sim->activeStateId() != 1) {
        std::fprintf(stderr, "FAIL: root-event phase Run did not activate Idle\n");
        return 1;
    }

    // ---- the root countdown survives a state entry ----------------------------
    clock.advanceTicks(300);  // Idle's own delayed (id 9) fires
    if (sim->activeStateId() != 2 || activeChanges.empty() || activeChanges.back().viaTransitionId != 9) {
        std::fprintf(stderr, "FAIL: Idle's 300ms delayed transition did not fire first\n");
        return 1;
    }
    clock.advanceTicks(700);  // root after-1000 (id 8): 300 already elapsed, 700 remain
    if (sim->activeStateId() != 3 || activeChanges.back().viaTransitionId != 8) {
        std::fprintf(stderr, "FAIL: the root after-1000 countdown did not survive the state entry and fire\n");
        return 1;
    }
    if (!sim->trace().contains(QStringLiteral("action: leaveWork"))) {
        std::fprintf(stderr, "FAIL: the root delayed firing did not run the ACTIVE state's exit action\n");
        return 1;
    }

    // ---- state handler outranks the root group --------------------------------
    kernel.send(app::events::ResetRequested{});
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Go")});
    if (sim->activeStateId() != 2 || activeChanges.back().viaTransitionId != 4) {
        std::fprintf(stderr, "FAIL: SendEvent(Go) in Idle did not prefer the state-level transition 4\n");
        return 1;
    }

    // ---- unhandled event falls through to the root group ----------------------
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Go")});  // Working has no Go handler
    if (sim->activeStateId() != 2 || activeChanges.back().viaTransitionId != 5) {
        std::fprintf(stderr, "FAIL: SendEvent(Go) in Working did not fall through to root transition 5\n");
        return 1;
    }
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Home")});
    if (sim->activeStateId() != 1 || activeChanges.back().viaTransitionId != 6) {
        std::fprintf(stderr, "FAIL: SendEvent(Home) did not fire root transition 6 back to Idle\n");
        return 1;
    }

    // ---- machine self-target: a #machine root event RESTARTS the
    // ---- machine -- whole-config exit (leaveWork runs), initial re-entry ------
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Go")});  // Idle -> Working via 4
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Restart")});
    if (sim->activeStateId() != 1 || activeChanges.back().viaTransitionId != 10 || activeChanges.back().toId != 1) {
        std::fprintf(stderr, "FAIL: the machineSelf root event did not restart the machine to its initial state\n");
        return 1;
    }
    // filter(), not contains() -- QStringList::contains is whole-element
    // equality and the headline element is "Restart → #machine".
    if (sim->trace().filter(QStringLiteral("#machine")).isEmpty()) {
        std::fprintf(stderr, "FAIL: the machineSelf firing's headline does not name #machine\n");
        return 1;
    }

    // ---- targetless root: acts in place ---------------------------------------
    activeChanges.clear();
    firedEvents.clear();
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Log")});
    if (sim->activeStateId() != 1 || !activeChanges.empty() || firedEvents.empty() ||
        firedEvents.back().transitionId != 7 || !sim->trace().contains(QStringLiteral("action: logIt"))) {
        std::fprintf(stderr, "FAIL: targetless root Log did not act in place\n");
        return 1;
    }
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("Nope")});
    if (!activeChanges.empty()) {
        std::fprintf(stderr, "FAIL: an event neither state nor root handles was not ignored\n");
        return 1;
    }

    // ---- generated code carries the root support ------------------------------
    // The firing order lives in <machine>_core.h alone: the root fallback and
    // arm bodies are asserted there, and the kernel files must be delegations
    // (a guard/action call in a command body would be a drift-prone second copy).
    const QVector<app::GeneratedFile> files = app::generate(doc->machine(), QStringLiteral("app::generated"));
    QString coreText;
    QString agentText;
    QString commandsText;
    QString bootstrapText;
    for (const app::GeneratedFile& file : files) {
        if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
            coreText = file.content;
        } else if (file.relativePath.endsWith(QStringLiteral("_agent.h"))) {
            agentText = file.content;
        } else if (file.relativePath.endsWith(QStringLiteral("_commands.h"))) {
            commandsText = file.content;
        } else if (file.relativePath.endsWith(QStringLiteral("_bootstrap.h"))) {
            bootstrapText = file.content;
        }
    }
    if (coreText.isEmpty()) {
        std::fprintf(stderr, "FAIL: generate() emitted no <machine>_core.h\n");
        return 1;
    }
    if (!coreText.contains(QStringLiteral("Root fallback")) || coreText.contains(QStringLiteral("State::Unknown")) ||
        !coreText.contains(QStringLiteral("void armRootDelayedTransitions()")) ||
        !coreText.contains(QStringLiteral("void armDelayedTransitions()"))) {
        std::fprintf(stderr, "FAIL: the generated core is missing the root fallback/arm support (or leaks Unknown)\n");
        return 1;
    }
    if (coreText.contains(QStringLiteral("ordo/core")) || coreText.contains(QStringLiteral("ordo::core"))) {
        std::fprintf(stderr, "FAIL: the generated core is not ordo-free\n");
        return 1;
    }
    if (!commandsText.contains(QStringLiteral("armRootDemoRootDelayedTransitions")) ||
        !bootstrapText.contains(QStringLiteral("armRootDemoRootDelayedTransitions"))) {
        std::fprintf(stderr, "FAIL: the kernel layer lost the root-arm entry point\n");
        return 1;
    }
    if (commandsText.contains(QStringLiteral("actions_.get()")) ||
        commandsText.contains(QStringLiteral("guards_.get()")) ||
        !commandsText.contains(QStringLiteral("agent->core()."))) {
        std::fprintf(stderr, "FAIL: a command body still inlines firing logic instead of delegating to the core\n");
        return 1;
    }
    if (!agentText.contains(QStringLiteral("setOnStateChanged")) ||
        !agentText.contains(QStringLiteral("events::StateChanged{previous, next}"))) {
        std::fprintf(stderr, "FAIL: the generated agent no longer republishes the core's state changes\n");
        return 1;
    }

    kernel.dispatcher().unsubscribe(&obsOwner);
    removeSimCommands(kernel);

    std::printf("PASS: state-designer root-event smoke (fallback order + surviving root countdown + machine-self "
                "restart + codegen)\n");
    return 0;
}

