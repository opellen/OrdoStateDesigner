  // --smoke phases: the sim interpreter family (flat sim smokes, hierarchical
// execution, History semantics, Back() replay).

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


// The JIT runs invocations: entering an invoking state arms a record (traced
// "invoke: <id> started"); leaving drops it ("... cancelled"). completeInvocation()
// drives a real macrostep with the payload bound as "output"/"error" for that
// macrostep only; a completion naming a dead invocation is a silent no-op.
static int runInvokeJitFixtures() {
    const auto traceIndexOf = [](const QStringList& trace, const QString& needle) {
        for (int i = 0; i < trace.size(); ++i) {
            if (trace.at(i).contains(needle)) {
                return i;
            }
        }
        return -1;
    };
    const auto addVariable = [](ordo::core::Kernel& kernel, app::MachineDocAgent& doc, const QString& name,
                                 app::ContextType type, const QString& initialValue) {
        const quint64 id = doc.machine().nextId;
        kernel.send(app::events::AddContextVariableRequested{});
        kernel.send(app::events::RenameContextVariableRequested{.id = id, .name = name});
        kernel.send(app::events::SetContextTypeRequested{.id = id, .type = type});
        kernel.send(app::events::SetContextInitialValueRequested{.id = id, .initialValue = initialValue});
        return id;
    };

    // ==== fixture 1: lifecycle (arm on entry, drop on exit) + a completion
    // against a dropped invocation is a silent no-op ============================
    // Idle -Go-> Loading(invoke fetchUser) -Cancel-> Idle. No context, no
    // onDone/onError wiring: only the record, its trace lines, and cancellation.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 1 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Idle
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: Loading
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Idle")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Loading")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::SetInvokeSrcRequested{.stateId = 2, .src = QStringLiteral("fetchUser")});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 3: Idle -Go-> Loading
        kernel.send(app::events::SetTransitionEventRequested{.id = 3, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 1});  // id 4: Loading -Cancel-> Idle
        kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Cancel")});
        if (doc->machine().transitions.size() != 2 || doc->findState(2)->invokeSrc != QStringLiteral("fetchUser")) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 1 topology build did not produce the expected machine\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();
        if (sim->activeStateId() != 1 || traceIndexOf(sim->trace(), QStringLiteral("invoke:")) >= 0) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 1 Run armed an invocation before Loading was ever entered\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != 2 ||
            traceIndexOf(sim->trace(), QStringLiteral("invoke: fetchUser started")) < 0) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 1 entering Loading did not arm + trace the invocation\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Cancel"));
        if (sim->activeStateId() != 1 ||
            traceIndexOf(sim->trace(), QStringLiteral("invoke: fetchUser cancelled")) < 0) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 1 leaving Loading did not drop + trace the cancellation\n");
            return 1;
        }

        // The invocation is gone: a completion naming it must be a total
        // no-op (no state change, no fact, no new trace() entry).
        const int traceSizeBeforeNoOp = sim->trace().size();
        sim->completeInvocation(QStringLiteral("fetchUser"), /*ok=*/true, QVariant(qint64(1)));
        if (sim->activeStateId() != 1 || sim->trace().size() != traceSizeBeforeNoOp) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 1 a completion against a DROPPED invocation was not a silent "
                          "no-op (state changed, or a trace line was appended)\n");
            return 1;
        }
    }

    // ==== fixture 2: onDone, guard reads `output` TRUE, assign persists the
    // payload, and the payload itself never lands in contextValues() =========
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 2 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("result"), app::ContextType::Int, QStringLiteral("-1"));
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 2: Idle
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: Loading
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 4: Success
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 100)});  // id 5: Failure
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Idle")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Loading")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Success")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("Failure")});
        kernel.send(app::events::SetInitialStateRequested{.id = 2});
        kernel.send(app::events::SetInvokeSrcRequested{.stateId = 3, .src = QStringLiteral("fetchUser")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: Idle -Go-> Loading
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Go")});
        // id 7 (guarded) must precede id 8 (unguarded catch-all) in document
        // order: within a candidate group only the last row may be unguarded.
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 7: onDone[output > 0] -> Success
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("done.invoke.fetchUser")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 7, .guard = QStringLiteral("output > 0")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 7, .action = QStringLiteral("result = output")});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 5});  // id 8: onDone (unguarded) -> Failure
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("done.invoke.fetchUser")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != 3) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 2 Go did not reach Loading\n");
            return 1;
        }
        sim->completeInvocation(QStringLiteral("fetchUser"), /*ok=*/true, QVariant(qint64(42)));
        if (sim->activeStateId() != 4) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 2 onDone with output=42 did not take the 'output > 0' branch to "
                          "Success\n");
            return 1;
        }
        if (sim->contextValues().value(QStringLiteral("result")).toLongLong() != 42) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 2 the onDone transition's assign did not persist the payload "
                          "into context (result=%lld, expected 42)\n",
                          static_cast<long long>(sim->contextValues().value(QStringLiteral("result")).toLongLong()));
            return 1;
        }
        if (sim->contextValues().contains(QStringLiteral("output"))) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 2 the completion payload leaked into contextValues() under its "
                          "own identifier name -- it must be scoped to the one macrostep, never written into "
                          "extended state\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), QStringLiteral("invoke: fetchUser done")) < 0 ||
            traceIndexOf(sim->trace(), QStringLiteral("assign: result = 42")) < 0) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 2 did not trace the completion + the resulting assign\n");
            return 1;
        }
    }

    // ==== fixture 3: onDone, guard reads `output` FALSE -- falls through to
    // the unguarded catch-all row (proves the SAME guard decides both ways) ===
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 3 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("result"), app::ContextType::Int, QStringLiteral("-1"));
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 100)});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Idle")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Loading")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Success")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("Failure")});
        kernel.send(app::events::SetInitialStateRequested{.id = 2});
        kernel.send(app::events::SetInvokeSrcRequested{.stateId = 3, .src = QStringLiteral("fetchUser")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("done.invoke.fetchUser")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 7, .guard = QStringLiteral("output > 0")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 7, .action = QStringLiteral("result = output")});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 5});
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("done.invoke.fetchUser")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        sim->completeInvocation(QStringLiteral("fetchUser"), /*ok=*/true, QVariant(qint64(0)));
        if (sim->activeStateId() != 5) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 3 onDone with output=0 did not fall through the failing "
                          "'output > 0' guard to the unguarded catch-all (Failure)\n");
            return 1;
        }
        if (sim->contextValues().value(QStringLiteral("result")).toLongLong() != -1) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 3 the untaken branch's assign ran anyway (result should still "
                          "be the seeded -1)\n");
            return 1;
        }
    }

    // ==== fixture 4: onError, assign persists a STRING payload ==================
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 4 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("lastError"), app::ContextType::String, QString());
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 2: Idle
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: Loading
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 4: Failure
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Idle")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Loading")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Failure")});
        kernel.send(app::events::SetInitialStateRequested{.id = 2});
        kernel.send(app::events::SetInvokeSrcRequested{.stateId = 3, .src = QStringLiteral("fetchUser")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 5: Idle -Go-> Loading
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 6: onError -> Failure
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("error.platform.fetchUser")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 6, .action = QStringLiteral("lastError = error")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        sim->completeInvocation(QStringLiteral("fetchUser"), /*ok=*/false, QVariant(QStringLiteral("TIMEOUT")));
        if (sim->activeStateId() != 4) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 4 onError did not fire the error.platform transition\n");
            return 1;
        }
        if (sim->contextValues().value(QStringLiteral("lastError")).toString() != QStringLiteral("TIMEOUT")) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 4 the onError transition's assign did not persist the error "
                          "payload into context\n");
            return 1;
        }
        if (sim->contextValues().contains(QStringLiteral("error"))) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 4 the error payload leaked into contextValues() under its own "
                          "identifier name\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), QStringLiteral("invoke: fetchUser error")) < 0 ||
            traceIndexOf(sim->trace(), QStringLiteral("assign: lastError = 'TIMEOUT'")) < 0) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 4 did not trace the error completion + the resulting assign\n");
            return 1;
        }
    }

    // ==== fixture 5: Back replays a COMPLETION-driven macrostep with its own
    // recorded payload. Without the payload, replay would skip the
    // "result = output" assign and reproduce the wrong context. ===============
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 5 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("result"), app::ContextType::Int, QStringLiteral("-1"));
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 2: Idle
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: Loading
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 4: Success
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});  // id 5: Done
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Idle")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Loading")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Success")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("Done")});
        kernel.send(app::events::SetInitialStateRequested{.id = 2});
        kernel.send(app::events::SetInvokeSrcRequested{.stateId = 3, .src = QStringLiteral("fetchUser")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: Idle -Go-> Loading
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 7: onDone -> Success
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("done.invoke.fetchUser")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 7, .action = QStringLiteral("result = output")});
        kernel.send(app::events::AddTransitionRequested{.from = 4, .to = 5});  // id 8: Success -Go2-> Done
        kernel.send(app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("Go2")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));                                                      // macrostep 1
        sim->completeInvocation(QStringLiteral("fetchUser"), /*ok=*/true, QVariant(qint64(42)));    // macrostep 2
        sim->sendEvent(QStringLiteral("Go2"));                                                      // macrostep 3
        if (sim->activeStateId() != 5) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 5 setup did not reach Done after 3 macrosteps\n");
            return 1;
        }
        sim->back();  // undoes macrostep 3 -- lands on macrostep 2's own result: Success, result=42
        if (sim->activeStateId() != 4) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 5 Back did not land on Success, the post-completion state\n");
            return 1;
        }
        if (sim->contextValues().value(QStringLiteral("result")).toLongLong() != 42) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 5 Back's replay did not reproduce the completion-driven assign "
                          "(result=%lld, expected 42) -- the recorded macrostep's own payload was not replayed\n",
                          static_cast<long long>(sim->contextValues().value(QStringLiteral("result")).toLongLong()));
            return 1;
        }
    }

    // ==== fixture 6: the JIT derives its PayloadBinding from the invoking
    // state's invokeOutputType and agrees with the validator on the same
    // machine. Two sub-cases over the guard "output == 'ok'": 6a declares
    // String (guard type-checks, live completion takes the guarded branch);
    // 6b keeps the Int default (guard is ill-typed, guardPasses() refuses it
    // and the macrostep falls through to the unguarded catch-all). ============
    {
        // 6a: String ------------------------------------------------------------
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 6a agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Idle
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: Loading
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 3: Success
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 100)});  // id 4: Failure
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Idle")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Loading")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Success")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Failure")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::SetInvokeSrcRequested{.stateId = 2, .src = QStringLiteral("fetchThing")});
        kernel.send(
            app::events::SetInvokeOutputTypeRequested{.stateId = 2, .type = app::ContextType::String});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 5: Idle -Go-> Loading
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Go")});
        // id 6 (guarded) MUST precede id 7 (unguarded catch-all) -- check 5's
        // ordering rule, same as invoke-jit fixture 2 above.
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: onDone[output=='ok']->Success
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("done.invoke.fetchThing")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 6, .guard = QStringLiteral("output == 'ok'")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 4});  // id 7: onDone (unguarded)->Failure
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("done.invoke.fetchThing")});
        removeEditCommands(kernel);

        // The validator agrees BEFORE the JIT ever runs: same machine, zero
        // "is ill-typed" findings on transition 6.
        for (const app::Problem& problem : app::validate(doc->machine())) {
            if (problem.transitionId == 6 && problem.text.contains(QStringLiteral("is ill-typed:"))) {
                std::fprintf(stderr,
                              "FAIL: invoke-jit fixture 6a the validator reported an ill-typed finding for "
                              "\"output == 'ok'\" off a String-declared invoke (want none): %s\n",
                              qUtf8Printable(problem.text));
                return 1;
            }
        }

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        sim->completeInvocation(QStringLiteral("fetchThing"), /*ok=*/true, QVariant(QStringLiteral("ok")));
        if (sim->activeStateId() != 3) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 6a onDone with output='ok' (String-declared) did not take the "
                          "'output == 'ok'' branch to Success -- the JIT still type-checked output as Int\n");
            return 1;
        }
    }
    {
        // 6b: Int (the default) --------------------------------------------------
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: invoke-jit fixture 6b agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 100)});
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Idle")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Loading")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Success")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Failure")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        // invokeOutputType is left at its Int default on purpose.
        kernel.send(app::events::SetInvokeSrcRequested{.stateId = 2, .src = QStringLiteral("fetchThing")});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("done.invoke.fetchThing")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 6, .guard = QStringLiteral("output == 'ok'")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 4});
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("done.invoke.fetchThing")});
        removeEditCommands(kernel);

        // The validator already reports an "is ill-typed" finding on
        // transition 6 (Int vs the string literal 'ok') before the JIT runs.
        bool validatorFoundIllTyped = false;
        for (const app::Problem& problem : app::validate(doc->machine())) {
            if (problem.transitionId == 6 && problem.text.contains(QStringLiteral("is ill-typed:"))) {
                validatorFoundIllTyped = true;
            }
        }
        if (!validatorFoundIllTyped) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 6b the validator did not report an ill-typed finding for "
                          "\"output == 'ok'\" off an Int-declared invoke (want one)\n");
            return 1;
        }

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        sim->completeInvocation(QStringLiteral("fetchThing"), /*ok=*/true, QVariant(QStringLiteral("ok")));
        if (sim->activeStateId() != 4) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 6b onDone with an Int-declared invoke did not refuse the "
                          "ill-typed 'output == 'ok'' guard and fall through to the unguarded catch-all "
                          "(Failure) -- the JIT and the validator disagree\n");
            return 1;
        }
        // guardPasses() traces the raw type-mismatch message, not the
        // validator's "is ill-typed" wrapper.
        if (traceIndexOf(sim->trace(), QStringLiteral("cannot compare")) < 0) {
            std::fprintf(stderr,
                          "FAIL: invoke-jit fixture 6b did not trace the guard refusal's type-mismatch reason\n");
            return 1;
        }
    }

    std::printf(
        "PASS: state-designer invoke JIT smoke (arm on entry + drop on exit + dead-invocation no-op + onDone "
        "guard true/false + onError + payload scoped to one macrostep, never persisted into context + Back "
        "replays a completion-driven macrostep's own payload + per-state invokeOutputType agrees with the "
        "validator, both directions)\n");
    return 0;
}

// Reenter transition flag and always (eventless) transitions in the JIT interpreter.
static int runReenterAlwaysFixtures() {
    const auto configEquals = [](const std::vector<quint64>& actual, std::initializer_list<quint64> expected) {
        return actual == std::vector<quint64>(expected);
    };
    const auto traceIndexOf = [](const QStringList& trace, const QString& needle) {
        for (int i = 0; i < trace.size(); ++i) {
            if (trace[i].contains(needle)) {
                return i;
            }
        }
        return -1;
    };
    const auto addVariable = [](ordo::core::Kernel& kernel, app::MachineDocAgent& doc, const QString& name,
                                 app::ContextType type, const QString& initialValue) {
        const quint64 id = doc.machine().nextId;
        kernel.send(app::events::AddContextVariableRequested{});
        kernel.send(app::events::RenameContextVariableRequested{.id = id, .name = name});
        kernel.send(app::events::SetContextTypeRequested{.id = id, .type = type});
        kernel.send(app::events::SetContextInitialValueRequested{.id = id, .initialValue = initialValue});
        return id;
    };

    // ==== fixture 1: hierarchical internal (reenter: false) vs external (reenter: true) self-transition ====
    // Parent P (id 1), Children A (id 2, initial), B (id 3).
    // Outgoing from P: Next -> B (id 4), Ping -> P (id 5, reenter: false), ResetP -> P (id 6, reenter: true).
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 1 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: P
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: B
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("P")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("B")});
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 1});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::SetEntryActionsRequested{.id = 1, .entryActions = QStringList{QStringLiteral("enterP")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 1, .exitActions = QStringList{QStringLiteral("exitP")}});
        kernel.send(app::events::SetEntryActionsRequested{.id = 2, .entryActions = QStringList{QStringLiteral("enterA")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 2, .exitActions = QStringList{QStringLiteral("exitA")}});
        kernel.send(app::events::SetEntryActionsRequested{.id = 3, .entryActions = QStringList{QStringLiteral("enterB")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 3, .exitActions = QStringList{QStringLiteral("exitB")}});

        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 4: A -Next-> B
        kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Next")});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 1});  // id 5: P -Ping-> P (internal)
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Ping")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 5, .action = QStringLiteral("pingAct")});
        kernel.send(app::events::SetTransitionReenterRequested{.id = 5, .reenter = false});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 1});  // id 6: P -ResetP-> P (external)
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("ResetP")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 6, .action = QStringLiteral("resetAct")});
        kernel.send(app::events::SetTransitionReenterRequested{.id = 6, .reenter = true});
        removeEditCommands(kernel);

        sim->run();
        if (!configEquals(sim->configuration(), {1, 2})) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 1 Run did not land in P.A\n");
            return 1;
        }

        sim->sendEvent(QStringLiteral("Next"));
        if (!configEquals(sim->configuration(), {1, 3})) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 1 Next did not land in P.B\n");
            return 1;
        }

        // Internal self-transition Ping (reenter: false):
        // P is NOT exited, B is NOT exited, A is NOT re-entered. Active config stays {1, 3}!
        const int traceLenBeforePing = sim->trace().size();
        sim->sendEvent(QStringLiteral("Ping"));
        if (!configEquals(sim->configuration(), {1, 3})) {
            std::fprintf(stderr, "FAIL: reenter: false self-transition Ping disturbed active child B\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "action: pingAct") < traceLenBeforePing) {
            std::fprintf(stderr, "FAIL: reenter: false self-transition Ping did not run action: pingAct\n");
            return 1;
        }
        for (int i = traceLenBeforePing; i < sim->trace().size(); ++i) {
            if (sim->trace()[i].contains(QStringLiteral("exit")) || sim->trace()[i].contains(QStringLiteral("enter"))) {
                std::fprintf(stderr, "FAIL: reenter: false self-transition Ping ran exit/entry actions (%s)\n",
                              sim->trace()[i].toUtf8().constData());
                return 1;
            }
        }

        // External self-transition ResetP (reenter: true):
        // Exits B, exits P, runs resetAct, re-enters P, re-enters initial child A. Active config becomes {1, 2}!
        const int traceLenBeforeReset = sim->trace().size();
        sim->sendEvent(QStringLiteral("ResetP"));
        if (!configEquals(sim->configuration(), {1, 2})) {
            std::fprintf(stderr, "FAIL: reenter: true self-transition ResetP did not re-enter P.A\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "action: resetAct") < traceLenBeforeReset) {
            std::fprintf(stderr, "FAIL: reenter: true self-transition ResetP did not run action: resetAct\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "action: exitB") < traceLenBeforeReset ||
            traceIndexOf(sim->trace(), "action: exitP") < traceLenBeforeReset ||
            traceIndexOf(sim->trace(), "action: enterP") < traceLenBeforeReset ||
            traceIndexOf(sim->trace(), "action: enterA") < traceLenBeforeReset) {
            std::fprintf(stderr, "FAIL: reenter: true self-transition ResetP did not run exitB/exitP/enterP/enterA\n");
            return 1;
        }
    }

    // ==== fixture 2: always transition on initial state activation ====
    // Init (id 1) has always -> Ready (id 2). Run() immediately lands in Ready.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 2 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Init
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: Ready
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Init")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Ready")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 3: always -> Ready
        kernel.send(app::events::SetTransitionAlwaysRequested{.id = 3, .always = true});
        removeEditCommands(kernel);

        sim->run();
        if (sim->activeStateId() != 2) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 2 initial always transition did not land in Ready (got %llu)\n",
                          static_cast<unsigned long long>(sim->activeStateId()));
            return 1;
        }
        if (traceIndexOf(sim->trace(), QStringLiteral("(always) → Ready")) < 0) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 2 trace does not contain '(always) → Ready'\n");
            return 1;
        }
    }

    // ==== fixture 3: always cascading / microstep chaining across states ====
    // S1 -Go-> S2 (stage=1) -always [stage==1]-> S3 (stage=2) -always [stage==2]-> S4 (stage=3)
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 3 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("stage"), app::ContextType::Int, QStringLiteral("0"));
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 2: S1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 3: S2
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 4: S3
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 5: S4
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("S1")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("S2")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("S3")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("S4")});
        kernel.send(app::events::SetInitialStateRequested{.id = 2});

        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: S1 -Go-> S2
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 6, .action = QStringLiteral("stage = 1")});

        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 7: S2 -always-> S3
        kernel.send(app::events::SetTransitionAlwaysRequested{.id = 7, .always = true});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 7, .guard = QStringLiteral("stage == 1")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 7, .action = QStringLiteral("stage = 2")});

        kernel.send(app::events::AddTransitionRequested{.from = 4, .to = 5});  // id 8: S3 -always-> S4
        kernel.send(app::events::SetTransitionAlwaysRequested{.id = 8, .always = true});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 8, .guard = QStringLiteral("stage == 2")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 8, .action = QStringLiteral("stage = 3")});
        removeEditCommands(kernel);

        sim->run();
        if (sim->activeStateId() != 2) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 3 Run did not start in S1\n");
            return 1;
        }

        sim->sendEvent(QStringLiteral("Go"));
        // Single macrostep RTC should cascade S2 -> S3 -> S4
        if (sim->activeStateId() != 5) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 3 cascading always did not reach S4 (got %llu)\n",
                          static_cast<unsigned long long>(sim->activeStateId()));
            return 1;
        }
        if (sim->contextValues().value(QStringLiteral("stage")).toLongLong() != 3) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 3 stage context variable was not 3 (got %lld)\n",
                          sim->contextValues().value(QStringLiteral("stage")).toLongLong());
            return 1;
        }
        if (traceIndexOf(sim->trace(), QStringLiteral("(always) → S3")) < 0 || traceIndexOf(sim->trace(), QStringLiteral("(always) → S4")) < 0) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 3 trace missing intermediate always transitions\n");
            return 1;
        }
    }

    // ==== fixture 4: always guarded fallback group ====
    // Branch state with 3 always transitions: [val > 10]->High, [val > 5]->Mid, fallback->Low
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 4 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("val"), app::ContextType::Int, QStringLiteral("7"));
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 2: Branch
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, -30)}); // id 3: High
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});   // id 4: Mid
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 30)});  // id 5: Low
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Branch")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("High")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Mid")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("Low")});
        kernel.send(app::events::SetInitialStateRequested{.id = 2});

        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: [val > 10] -> High
        kernel.send(app::events::SetTransitionAlwaysRequested{.id = 6, .always = true});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 6, .guard = QStringLiteral("val > 10")});

        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 4});  // id 7: [val > 5] -> Mid
        kernel.send(app::events::SetTransitionAlwaysRequested{.id = 7, .always = true});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 7, .guard = QStringLiteral("val > 5")});

        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 5});  // id 8: -> Low
        kernel.send(app::events::SetTransitionAlwaysRequested{.id = 8, .always = true});
        removeEditCommands(kernel);

        // val is 7: should select Mid
        sim->run();
        if (sim->activeStateId() != 4) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 4 val=7 did not land in Mid (got %llu)\n",
                          static_cast<unsigned long long>(sim->activeStateId()));
            return 1;
        }
    }

    // ==== fixture 5: loop quota protection (100 steps) ====
    // Spin (id 1) with unguarded always self-transition (reenter: false).
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 5 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});  // id 1: Spin
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Spin")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 1});  // id 2: Spin -always-> Spin
        kernel.send(app::events::SetTransitionAlwaysRequested{.id = 2, .always = true});
        kernel.send(app::events::SetTransitionReenterRequested{.id = 2, .reenter = false});
        removeEditCommands(kernel);

        sim->run();
        if (sim->activeStateId() != 1) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 5 active state disturbed during infinite loop\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "warning: always transition loop exceeded quota (100 steps)") < 0) {
            std::fprintf(stderr, "FAIL: reenter/always fixture 5 did not trace quota warning\n");
            return 1;
        }
    }

    std::printf(
        "PASS: state-designer reenter & always simulation smoke (hierarchical reenter:false vs reenter:true + "
        "always on initial descent + microstep chaining + guarded fallback + quota loop limit)\n");
    return 0;
}

// raise() action and the internal event queue RTC microstep loop in the JIT interpreter.
static int runRaiseSimFixtures() {
    const auto configEquals = [](const std::vector<quint64>& actual, std::initializer_list<quint64> expected) {
        return actual == std::vector<quint64>(expected);
    };
    const auto traceIndexOf = [](const QStringList& trace, const QString& needle) {
        for (int i = 0; i < trace.size(); ++i) {
            if (trace[i].contains(needle)) {
                return i;
            }
        }
        return -1;
    };

    // ==== fixture 1: basic raise cascade ====
    // Idle (id 1) -Start-> Active (id 2, entryAction: raise(Next)) -Next-> Done (id 3).
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: raise fixture 1 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Idle
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: Active
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 3: Done
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Idle")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Active")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Done")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = 2, .entryActions = QStringList{QStringLiteral("raise(Next)")}});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 4: Idle -> Active
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 5: Active -> Done
        kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Start")});
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Next")});
        removeEditCommands(kernel);

        sim->run();
        if (sim->activeStateId() != 1) {
            std::fprintf(stderr, "FAIL: raise fixture 1 initial state not Idle\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Start"));
        if (sim->activeStateId() != 3) {
            std::fprintf(stderr, "FAIL: raise fixture 1 Start did not cascade through Active to Done (got %llu)\n",
                         static_cast<unsigned long long>(sim->activeStateId()));
            return 1;
        }
        if (traceIndexOf(sim->trace(), "Start → Active") < 0 ||
            traceIndexOf(sim->trace(), "raise: Next") < 0 ||
            traceIndexOf(sim->trace(), "Next → Done") < 0) {
            std::fprintf(stderr, "FAIL: raise fixture 1 trace missing expected cascade lines\n");
            return 1;
        }
        if (!sim->internalQueue().empty()) {
            std::fprintf(stderr, "FAIL: raise fixture 1 internalQueue_ not empty after quiescence\n");
            return 1;
        }
    }

    // ==== fixture 2: FIFO microstep queue with multiple raised events ====
    // S1 -Go/raise(EventA)-> S2 (entryAction: raise(EventB))
    // S2 -EventA-> S3
    // S3 -EventB-> S4
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: raise fixture 2 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: S1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: S2
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 3: S3
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});  // id 4: S4
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("S1")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("S2")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("S3")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("S4")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = 2, .entryActions = QStringList{QStringLiteral("raise(EventB)")}});

        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 5: S1 -> S2
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: S2 -> S3
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 7: S3 -> S4
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 5, .action = QStringLiteral("raise(EventA)")});
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("EventA")});
        kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("EventB")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != 4) {
            std::fprintf(stderr, "FAIL: raise fixture 2 FIFO queue did not cascade to S4 (got %llu)\n",
                         static_cast<unsigned long long>(sim->activeStateId()));
            return 1;
        }
        const int idxA = traceIndexOf(sim->trace(), "EventA → S3");
        const int idxB = traceIndexOf(sim->trace(), "EventB → S4");
        if (idxA < 0 || idxB < 0 || idxA > idxB) {
            std::fprintf(stderr, "FAIL: raise fixture 2 FIFO order violated (idxA=%d, idxB=%d)\n", idxA, idxB);
            return 1;
        }
    }

    // ==== fixture 3: SCXML RTC priority - always transitions preempt internal queue ====
    // A -Trigger/raise(Internal)-> B
    // B -always-> C
    // C -Internal-> D
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: raise fixture 3 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: B
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 3: C
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});  // id 4: D
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("B")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("C")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("D")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});

        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 5: A -> B
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: B -> C (always)
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 7: C -> D (Internal)
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Trigger")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 5, .action = QStringLiteral("raise(Internal)")});
        kernel.send(app::events::SetTransitionAlwaysRequested{.id = 6, .always = true});
        kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Internal")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Trigger"));
        if (sim->activeStateId() != 4) {
            std::fprintf(stderr, "FAIL: raise fixture 3 always preemption did not land in D (got %llu)\n",
                         static_cast<unsigned long long>(sim->activeStateId()));
            return 1;
        }
        const int idxAlways = traceIndexOf(sim->trace(), "(always) → C");
        const int idxInternal = traceIndexOf(sim->trace(), "Internal → D");
        if (idxAlways < 0 || idxInternal < 0 || idxAlways > idxInternal) {
            std::fprintf(stderr, "FAIL: raise fixture 3 trace order incorrect (always=%d, internal=%d)\n",
                         idxAlways, idxInternal);
            return 1;
        }
    }

    // ==== fixture 4: unhandled internal event is silently discarded ====
    // S1 -Go/raise(Orphan)-> S2
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: raise fixture 4 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: S1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: S2
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("S1")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("S2")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 3: S1 -> S2
        kernel.send(app::events::SetTransitionEventRequested{.id = 3, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 3, .action = QStringLiteral("raise(Orphan)")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != 2) {
            std::fprintf(stderr, "FAIL: raise fixture 4 unhandled event failed to stay in S2\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "raise: Orphan") < 0) {
            std::fprintf(stderr, "FAIL: raise fixture 4 missing raise trace line\n");
            return 1;
        }
        if (!sim->internalQueue().empty()) {
            std::fprintf(stderr, "FAIL: raise fixture 4 unhandled event was not discarded from queue\n");
            return 1;
        }
    }

    // ==== fixture 5: malformed raise action is skipped and traced ====
    // S1 -Go/raise()-> S2
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: raise fixture 5 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: S1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: S2
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("S1")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("S2")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 3: S1 -> S2
        kernel.send(app::events::SetTransitionEventRequested{.id = 3, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 3, .action = QStringLiteral("raise()")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != 2) {
            std::fprintf(stderr, "FAIL: raise fixture 5 did not land in S2\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "raise skipped: raise() (raise requires an event name inside parentheses)") < 0) {
            std::fprintf(stderr, "FAIL: raise fixture 5 did not trace 'raise skipped:' warning\n");
            return 1;
        }
    }

    // ==== fixture 6: ping-pong loop quota protection (100 steps) ====
    // Left -Ping/raise(Pong)-> Right -Pong/raise(Ping)-> Left
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: raise fixture 6 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Left
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: Right
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Left")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Right")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 3: Left -> Right
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 1});  // id 4: Right -> Left
        kernel.send(app::events::SetTransitionEventRequested{.id = 3, .event = QStringLiteral("Ping")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 3, .action = QStringLiteral("raise(Pong)")});
        kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Pong")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 4, .action = QStringLiteral("raise(Ping)")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Ping"));
        if (traceIndexOf(sim->trace(), "warning: microstep loop exceeded quota (100 steps)") < 0) {
            std::fprintf(stderr, "FAIL: raise fixture 6 did not trace microstep loop quota warning\n");
            return 1;
        }
    }

    // ==== fixture 7: back replay restores quiescence and clears queue ====
    // A -Go-> B (entryAction: raise(Step)) -Step-> C
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: raise fixture 7 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: B
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 3: C
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("B")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("C")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = 2, .entryActions = QStringList{QStringLiteral("raise(Step)")}});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 4: A -> B
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 5: B -> C
        kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Step")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != 3) {
            std::fprintf(stderr, "FAIL: raise fixture 7 did not reach C\n");
            return 1;
        }
        sim->back();
        if (sim->activeStateId() != 2) {
            std::fprintf(stderr, "FAIL: raise fixture 7 back did not restore state B (got %llu)\n",
                         static_cast<unsigned long long>(sim->activeStateId()));
            return 1;
        }
        if (!sim->internalQueue().empty()) {
            std::fprintf(stderr, "FAIL: raise fixture 7 internal queue was not cleared after back\n");
            return 1;
        }
    }

    std::printf(
        "PASS: state-designer raise simulation smoke (microstep cascade + FIFO order + RTC always preemption + "
        "orphan discard + malformed skip + quota loop limit + back replay)\n");
    return 0;
}

// sendTo() and sendParent() action execution trace in the JIT interpreter.
static int runActorCommunicationSimFixtures() {
    const auto traceIndexOf = [](const QStringList& trace, const QString& needle) {
        for (int i = 0; i < trace.size(); ++i) {
            if (trace[i].contains(needle)) {
                return i;
            }
        }
        return -1;
    };

    // ==== fixture 1: sendTo and sendParent normal dispatch and malformed skip ====
    // Idle (id 1) -Start/sendTo(worker, GO)-> Active (id 2, entryAction: sendParent(ENTERED)) -Finish/sendTo(worker, STOP)-> Done (id 3, entryAction: sendParent(FINISHED))
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: actor comm fixture agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Idle
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: Active
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 3: Done
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Idle")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Active")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Done")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = 2, .entryActions = QStringList{QStringLiteral("sendParent(ENTERED)")}});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = 3, .entryActions = QStringList{QStringLiteral("sendParent(FINISHED)")}});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 4: Idle -> Active
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 5: Active -> Done
        kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Start")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 4, .action = QStringLiteral("sendTo(worker, GO)")});
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Finish")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 5, .action = QStringLiteral("sendTo(worker, STOP)")});
        removeEditCommands(kernel);

        sim->run();
        if (sim->activeStateId() != 1) {
            std::fprintf(stderr, "FAIL: actor comm fixture initial state not Idle\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Start"));
        if (sim->activeStateId() != 2) {
            std::fprintf(stderr, "FAIL: actor comm fixture Start did not transition to Active\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "sendTo: worker <- GO") < 0) {
            std::fprintf(stderr, "FAIL: actor comm fixture missing 'sendTo: worker <- GO' trace\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "sendParent: ENTERED") < 0) {
            std::fprintf(stderr, "FAIL: actor comm fixture missing 'sendParent: ENTERED' trace\n");
            return 1;
        }

        sim->sendEvent(QStringLiteral("Finish"));
        if (sim->activeStateId() != 3) {
            std::fprintf(stderr, "FAIL: actor comm fixture Finish did not transition to Done\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "sendTo: worker <- STOP") < 0) {
            std::fprintf(stderr, "FAIL: actor comm fixture missing 'sendTo: worker <- STOP' trace\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "sendParent: FINISHED") < 0) {
            std::fprintf(stderr, "FAIL: actor comm fixture missing 'sendParent: FINISHED' trace\n");
            return 1;
        }
    }

    // ==== fixture 2: malformed sendTo / sendParent skipped tracing ====
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: S1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: S2
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = 2, .entryActions = QStringList{QStringLiteral("sendParent()")}});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 3: S1 -> S2
        kernel.send(app::events::SetTransitionEventRequested{.id = 3, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 3, .action = QStringLiteral("sendTo(bad)")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != 2) {
            std::fprintf(stderr, "FAIL: actor comm fixture 2 did not transition to S2\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "sendTo skipped: sendTo(bad)") < 0) {
            std::fprintf(stderr, "FAIL: actor comm fixture 2 missing 'sendTo skipped:' trace\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "sendParent skipped: sendParent()") < 0) {
            std::fprintf(stderr, "FAIL: actor comm fixture 2 missing 'sendParent skipped:' trace\n");
            return 1;
        }
    }

    std::printf("PASS: state-designer actor communication simulation smoke (sendTo + sendParent + execution trace logging)\n");
    return 0;
}

// SCXML §3.12 / XState v5 wildcard event fixtures.
static int runWildcardEventSimFixtures() {
    const auto configEquals = [](const std::vector<quint64>& actual, std::initializer_list<quint64> expected) {
        return actual == std::vector<quint64>(expected);
    };

    // ==== Fixture 1: Specificity Ordering (Exact > Prefix > Universal) ============
    // Universal defined FIRST, Exact defined LAST -- verifies specificity overrides document order
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        registerEditCommands(kernel);

        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: S
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: Exact
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 3: Prefix
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});  // id 4: Universal
        kernel.send(app::events::SetInitialStateRequested{.id = 1});

        // t1: S -(*) -> Universal
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 4}); // id 5
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("*")});

        // t2: S -(mouse.*) -> Prefix
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 3}); // id 6
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("mouse.*")});

        // t3: S -(mouse.click) -> Exact
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2}); // id 7
        kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("mouse.click")});

        // 1a: Exact match wins over prefix and universal
        sim->run();
        sim->sendEvent(QStringLiteral("mouse.click"));
        if (!configEquals(sim->configuration(), {2})) {
            std::fprintf(stderr, "FAIL: wildcard fixture 1a: 'mouse.click' did not fire exact transition\n");
            return 1;
        }

        // 1b: Prefix match wins over universal
        sim->reset();
        sim->run();
        sim->sendEvent(QStringLiteral("mouse.move"));
        if (!configEquals(sim->configuration(), {3})) {
            std::fprintf(stderr, "FAIL: wildcard fixture 1b: 'mouse.move' did not fire prefix transition\n");
            return 1;
        }

        // 1c: Universal matches unrelated event
        sim->reset();
        sim->run();
        sim->sendEvent(QStringLiteral("keyboard.press"));
        if (!configEquals(sim->configuration(), {4})) {
            std::fprintf(stderr, "FAIL: wildcard fixture 1c: 'keyboard.press' did not fire universal transition\n");
            return 1;
        }
    }

    // ==== Fixture 2: Guarded Fallback across Specificity Tiers ====================
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        registerEditCommands(kernel);
        const auto addVar = [](ordo::core::Kernel& k, app::MachineDocAgent& d, const QString& name, int initialValue) {
            const quint64 id = d.machine().nextId;
            k.send(app::events::AddContextVariableRequested{});
            k.send(app::events::RenameContextVariableRequested{.id = id, .name = name});
            k.send(app::events::SetContextTypeRequested{.id = id, .type = app::ContextType::Int});
            k.send(app::events::SetContextInitialValueRequested{.id = id, .initialValue = QString::number(initialValue)});
            return id;
        };

        const quint64 exactVarId = addVar(kernel, *doc, QStringLiteral("exactGate"), 0);
        const quint64 prefixVarId = addVar(kernel, *doc, QStringLiteral("prefixGate"), 0);

        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id: S
        const quint64 sId = doc->machine().states.last().id;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id: Exact
        const quint64 exactTargetId = doc->machine().states.last().id;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id: Prefix
        const quint64 prefixTargetId = doc->machine().states.last().id;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});  // id: Universal
        const quint64 universalTargetId = doc->machine().states.last().id;
        kernel.send(app::events::SetInitialStateRequested{.id = sId});

        // t1: S -(mouse.click)[exactGate == 1] -> Exact
        kernel.send(app::events::AddTransitionRequested{.from = sId, .to = exactTargetId});
        const quint64 t1Id = doc->machine().transitions.last().id;
        kernel.send(app::events::SetTransitionEventRequested{.id = t1Id, .event = QStringLiteral("mouse.click")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = t1Id, .guard = QStringLiteral("exactGate == 1")});

        // t2: S -(mouse.*)[prefixGate == 1] -> Prefix
        kernel.send(app::events::AddTransitionRequested{.from = sId, .to = prefixTargetId});
        const quint64 t2Id = doc->machine().transitions.last().id;
        kernel.send(app::events::SetTransitionEventRequested{.id = t2Id, .event = QStringLiteral("mouse.*")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = t2Id, .guard = QStringLiteral("prefixGate == 1")});

        // t3: S -(*) -> Universal
        kernel.send(app::events::AddTransitionRequested{.from = sId, .to = universalTargetId});
        const quint64 t3Id = doc->machine().transitions.last().id;
        kernel.send(app::events::SetTransitionEventRequested{.id = t3Id, .event = QStringLiteral("*")});

        // 2a: Both exact and prefix guards false -> falls back to universal
        sim->run();
        sim->sendEvent(QStringLiteral("mouse.click"));
        if (!configEquals(sim->configuration(), {universalTargetId})) {
            std::fprintf(stderr, "FAIL: wildcard fixture 2a: fallback to universal failed\n");
            return 1;
        }

        // 2b: Exact false, prefix true -> falls back to prefix
        kernel.send(app::events::SetContextInitialValueRequested{.id = prefixVarId, .initialValue = QStringLiteral("1")});
        sim->reset();
        sim->sendEvent(QStringLiteral("mouse.click"));
        if (!configEquals(sim->configuration(), {prefixTargetId})) {
            std::fprintf(stderr, "FAIL: wildcard fixture 2b: fallback to prefix failed\n");
            return 1;
        }
    }

    // ==== Fixture 3: Hierarchical Inner-First Bubbling with Wildcard ==============
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        registerEditCommands(kernel);

        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Parent
        kernel.send(app::events::SetStateKindRequested{.id = 1, .kind = app::StateKind::Normal});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(10, 10), .parentId = 1}); // id 2: Child
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 3: ChildTarget
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});  // id 4: ParentTarget
        kernel.send(app::events::SetInitialStateRequested{.id = 1});

        // Parent -(mouse.click) -> ParentTarget
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 4}); // id 5
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("mouse.click")});

        // Child -(*) -> ChildTarget
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3}); // id 6
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("*")});

        sim->run();
        // Inner-first rule: Child's wildcard is evaluated before bubbling to Parent's exact match
        sim->sendEvent(QStringLiteral("mouse.click"));
        if (!configEquals(sim->configuration(), {3})) {
            std::fprintf(stderr, "FAIL: wildcard fixture 3: inner-first child wildcard did not preempt parent\n");
            return 1;
        }
    }

    // ==== Fixture 4: Root Fallback Wildcard =======================================
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        registerEditCommands(kernel);

        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: S1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: RootTarget
        kernel.send(app::events::SetInitialStateRequested{.id = 1});

        // Root transition: -(sys.*) -> RootTarget (id: 2)
        kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 2}); // id 3
        kernel.send(app::events::SetTransitionEventRequested{.id = 3, .event = QStringLiteral("sys.*")});

        sim->run();
        sim->sendEvent(QStringLiteral("sys.shutdown"));
        if (!configEquals(sim->configuration(), {2})) {
            std::fprintf(stderr, "FAIL: wildcard fixture 4: root fallback sys.* did not fire\n");
            return 1;
        }
    }

    std::printf("PASS: state-designer wildcard event simulation smoke (specificity + guarded fallback + inner-first bubbling + root fallback)\n");
    return 0;
}


// SCXML §3.3.1 / XState v5 Multi-Target Simulation Fixtures
static int runMultiTargetSimFixtures() {
    const auto configEquals = [](const std::vector<quint64>& actual, std::initializer_list<quint64> expected) {
        return actual == std::vector<quint64>(expected);
    };

    // ==== fixture 1: fork transition to parallel orthogonal regions ====
    // 1 Off (initial); 2 Active (Parallel) with regions 3 RegA {4 A_Ready
    // (initial), 5 A_Running} and 6 RegB {7 B_Ready (initial), 8 B_Running}.
    // Fork: Off -START-> [5, 8]. Join/exit: Active -STOP-> Off.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());

        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);

        app::Machine m;
        m.initialStateId = 1;
        m.states.push_back(app::State{.id = 1, .name = QStringLiteral("Off"), .kind = app::StateKind::Normal});
        m.states.push_back(app::State{.id = 2, .name = QStringLiteral("Active"), .kind = app::StateKind::Parallel});
        m.states.push_back(app::State{.id = 3, .name = QStringLiteral("RegA"), .kind = app::StateKind::Normal, .parentId = 2, .initialChildId = 4});
        m.states.push_back(app::State{.id = 4, .name = QStringLiteral("A_Ready"), .kind = app::StateKind::Normal, .parentId = 3});
        m.states.push_back(app::State{.id = 5, .name = QStringLiteral("A_Running"), .kind = app::StateKind::Normal, .parentId = 3});
        m.states.push_back(app::State{.id = 6, .name = QStringLiteral("RegB"), .kind = app::StateKind::Normal, .parentId = 2, .initialChildId = 7});
        m.states.push_back(app::State{.id = 7, .name = QStringLiteral("B_Ready"), .kind = app::StateKind::Normal, .parentId = 6});
        m.states.push_back(app::State{.id = 8, .name = QStringLiteral("B_Running"), .kind = app::StateKind::Normal, .parentId = 6});

        // Fork transition: Off -> [5, 8]
        m.transitions.push_back(app::Transition{
            .id = 1,
            .from = 1,
            .to = 5,
            .event = QStringLiteral("START"),
            .targets = {5, 8}
        });
        // Exit transition: Active -> Off
        m.transitions.push_back(app::Transition{
            .id = 2,
            .from = 2,
            .to = 1,
            .event = QStringLiteral("STOP")
        });

        doc->restore(m);

        sim->run();
        if (!configEquals(sim->configuration(), {1})) {
            std::fprintf(stderr, "FAIL: multi-target fixture 1: initial config was not {Off}\n");
            return 1;
        }

        // Fire START -> Fork into both orthogonal regions
        sim->sendEvent(QStringLiteral("START"));
        if (!configEquals(sim->configuration(), {2, 3, 5, 6, 8})) {
            std::fprintf(stderr, "FAIL: multi-target fixture 1: fork transition did not enter all targets\n");
            return 1;
        }

        // Fire STOP -> Exit parallel state to Off
        sim->sendEvent(QStringLiteral("STOP"));
        if (!configEquals(sim->configuration(), {1})) {
            std::fprintf(stderr, "FAIL: multi-target fixture 1: stop did not return to Off\n");
            return 1;
        }

        // Back() -> Restore fork configuration
        sim->back();
        if (!configEquals(sim->configuration(), {2, 3, 5, 6, 8})) {
            std::fprintf(stderr, "FAIL: multi-target fixture 1: back did not restore fork configuration\n");
            return 1;
        }

        // Back() -> Restore initial configuration
        sim->back();
        if (!configEquals(sim->configuration(), {1})) {
            std::fprintf(stderr, "FAIL: multi-target fixture 1: repeated back did not restore initial state\n");
            return 1;
        }
    }

    // ==== fixture 2: partial targeting with default descent in unspecified region ====
    // Transition targeting only A_Running in RegA, while RegB defaults to initial child B_Ready
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());

        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);

        app::Machine m;
        m.initialStateId = 1;
        m.states.push_back(app::State{.id = 1, .name = QStringLiteral("Off"), .kind = app::StateKind::Normal});
        m.states.push_back(app::State{.id = 2, .name = QStringLiteral("Active"), .kind = app::StateKind::Parallel});
        m.states.push_back(app::State{.id = 3, .name = QStringLiteral("RegA"), .kind = app::StateKind::Normal, .parentId = 2, .initialChildId = 4});
        m.states.push_back(app::State{.id = 4, .name = QStringLiteral("A_Ready"), .kind = app::StateKind::Normal, .parentId = 3});
        m.states.push_back(app::State{.id = 5, .name = QStringLiteral("A_Running"), .kind = app::StateKind::Normal, .parentId = 3});
        m.states.push_back(app::State{.id = 6, .name = QStringLiteral("RegB"), .kind = app::StateKind::Normal, .parentId = 2, .initialChildId = 7});
        m.states.push_back(app::State{.id = 7, .name = QStringLiteral("B_Ready"), .kind = app::StateKind::Normal, .parentId = 6});
        m.states.push_back(app::State{.id = 8, .name = QStringLiteral("B_Running"), .kind = app::StateKind::Normal, .parentId = 6});

        // Multi-target transition targeting only 5 (A_Running): targets = {5}
        m.transitions.push_back(app::Transition{
            .id = 1,
            .from = 1,
            .to = 5,
            .event = QStringLiteral("START"),
            .targets = {5}
        });

        doc->restore(m);

        sim->run();
        sim->sendEvent(QStringLiteral("START"));

        // Expected: Active(2), RegA(3), A_Running(5), RegB(6), B_Ready(7)
        if (!configEquals(sim->configuration(), {2, 3, 5, 6, 7})) {
            std::fprintf(stderr, "FAIL: multi-target fixture 2: partial target did not descend unspecified region to initial child\n");
            return 1;
        }

        sim->back();
        if (!configEquals(sim->configuration(), {1})) {
            std::fprintf(stderr, "FAIL: multi-target fixture 2: back did not restore initial state\n");
            return 1;
        }
    }

    std::printf("PASS: state-designer multi-target simulation smoke (fork to orthogonal regions + partial descent + exit join + back replay)\n");
    return 0;
}


static int runStructContextSimFixtures() {
    // 1. Define types
    app::StructField fTimestamp{.name = QStringLiteral("timestamp"), .type = app::FieldType::Int, .initialValue = QStringLiteral("0")};
    app::StructField fPriority{.name = QStringLiteral("priority"), .type = app::FieldType::Int, .initialValue = QStringLiteral("0")};
    app::StructDefinition headerDef{
        .id = 1,
        .name = QStringLiteral("CanHeader"),
        .fields = {fTimestamp, fPriority}
    };

    app::StructField fId{.name = QStringLiteral("id"), .type = app::FieldType::Int, .initialValue = QStringLiteral("0")};
    app::StructField fDlc{.name = QStringLiteral("dlc"), .type = app::FieldType::Int, .initialValue = QStringLiteral("0")};
    app::StructField fExt{.name = QStringLiteral("extended"), .type = app::FieldType::Bool, .initialValue = QStringLiteral("false")};
    app::StructField fHdr{.name = QStringLiteral("header"), .type = app::FieldType::Custom, .customTypeName = QStringLiteral("CanHeader")};
    app::StructDefinition msgDef{
        .id = 2,
        .name = QStringLiteral("CanMessage"),
        .fields = {fId, fDlc, fExt, fHdr}
    };

    // 2. Define machine
    app::Machine m;
    m.name = QStringLiteral("StructSimMachine");
    m.initialStateId = 1;
    m.types = {headerDef, msgDef};

    m.states.push_back(app::State{.id = 1, .name = QStringLiteral("Idle"), .kind = app::StateKind::Normal});
    m.states.push_back(app::State{
        .id = 2,
        .name = QStringLiteral("Processing"),
        .kind = app::StateKind::Normal,
        .entryActions = {QStringLiteral("rxCount = rxCount + 1")}
    });
    m.states.push_back(app::State{
        .id = 3,
        .name = QStringLiteral("Emergency"),
        .kind = app::StateKind::Normal,
        .entryActions = {QStringLiteral("errorCount = errorCount + 1")}
    });

    // Transitions
    // id 4: Idle -> Processing on rxFrame: CanMessage [event.id == 256 && event.dlc == 8] / lastMsg = event
    m.transitions.push_back(app::Transition{
        .id = 4,
        .from = 1,
        .to = 2,
        .event = QStringLiteral("rxFrame"),
        .guard = QStringLiteral("event.id == 256 && event.dlc == 8"),
        .action = QStringLiteral("lastMsg = event"),
        .payloadType = QStringLiteral("CanMessage")
    });

    // id 5: Processing -> Processing on rxFrame: CanMessage [event.id == 512] / lastMsg.id = event.id
    m.transitions.push_back(app::Transition{
        .id = 5,
        .from = 2,
        .to = 2,
        .event = QStringLiteral("rxFrame"),
        .guard = QStringLiteral("event.id == 512"),
        .action = QStringLiteral("lastMsg.id = event.id"),
        .payloadType = QStringLiteral("CanMessage")
    });

    // id 6: Processing -> Processing on tick: int [event == 42] / rxCount = rxCount + event
    m.transitions.push_back(app::Transition{
        .id = 6,
        .from = 2,
        .to = 2,
        .event = QStringLiteral("tick"),
        .guard = QStringLiteral("event == 42"),
        .action = QStringLiteral("rxCount = rxCount + event"),
        .payloadType = QStringLiteral("int")
    });

    // id 7: Processing -> Emergency on rxFrame: CanMessage [event.payload.header.priority == 1] / lastMsg = event.payload
    m.transitions.push_back(app::Transition{
        .id = 7,
        .from = 2,
        .to = 3,
        .event = QStringLiteral("rxFrame"),
        .guard = QStringLiteral("event.payload.header.priority == 1"),
        .action = QStringLiteral("lastMsg = event.payload"),
        .payloadType = QStringLiteral("CanMessage")
    });

    // Context variables
    app::ContextVariable lastMsgVar{
        .id = 8,
        .name = QStringLiteral("lastMsg"),
        .type = app::ContextType::Object,
        .initialValue = QString(),
        .customTypeName = QStringLiteral("CanMessage")
    };
    app::ContextVariable rxCountVar{
        .id = 9,
        .name = QStringLiteral("rxCount"),
        .type = app::ContextType::Int,
        .initialValue = QStringLiteral("0")
    };
    app::ContextVariable errorCountVar{
        .id = 10,
        .name = QStringLiteral("errorCount"),
        .type = app::ContextType::Int,
        .initialValue = QStringLiteral("0")
    };
    m.context = {lastMsgVar, rxCountVar, errorCountVar};
    m.nextId = 11;

    ordo::core::Kernel kernel;
    kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
    kernel.registerAgent(std::make_shared<app::SimulationAgent>());
    app::SimClock clock;
    registerSimCommands(kernel, clock);
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    doc->restore(m);

    // Initial state: run
    sim->run();
    if (!sim->running() || sim->activeStateId() != 1) {
        std::fprintf(stderr, "FAIL: struct context sim: initial state is not Idle\n");
        return 1;
    }

    // Verify initial context default seeding
    const QVariantMap initialLastMsg = sim->contextValues().value(QStringLiteral("lastMsg")).toMap();
    if (initialLastMsg.value(QStringLiteral("id")).toLongLong() != 0 ||
        initialLastMsg.value(QStringLiteral("header")).toMap().value(QStringLiteral("priority")).toLongLong() != 0) {
        std::fprintf(stderr, "FAIL: struct context sim: initial lastMsg was not default-seeded\n");
        return 1;
    }
    if (sim->contextValues().value(QStringLiteral("rxCount")).toLongLong() != 0) {
        std::fprintf(stderr, "FAIL: struct context sim: initial rxCount is not 0\n");
        return 1;
    }

    // 1. Send unmatched event (wrong id)
    QVariantMap headerMap;
    headerMap[QStringLiteral("timestamp")] = 1000;
    headerMap[QStringLiteral("priority")] = 2;
    QVariantMap badMsgMap;
    badMsgMap[QStringLiteral("id")] = 999;
    badMsgMap[QStringLiteral("dlc")] = 8;
    badMsgMap[QStringLiteral("extended")] = true;
    badMsgMap[QStringLiteral("header")] = headerMap;

    sim->sendEvent(QStringLiteral("rxFrame"), badMsgMap);
    if (sim->activeStateId() != 1) {
        std::fprintf(stderr, "FAIL: struct context sim: badMsg should not have triggered transition\n");
        return 1;
    }

    // 2. Send matching event: id 256, dlc 8
    QVariantMap normalMsgMap = badMsgMap;
    normalMsgMap[QStringLiteral("id")] = 256;

    sim->sendEvent(QStringLiteral("rxFrame"), normalMsgMap);
    // Should transition to Processing (id 2)
    if (sim->activeStateId() != 2) {
        std::fprintf(stderr, "FAIL: struct context sim: normalMsg did not transition to Processing\n");
        return 1;
    }
    // Context check: lastMsg should equal normalMsgMap, rxCount == 1 (entry action)
    const QVariantMap step1LastMsg = sim->contextValues().value(QStringLiteral("lastMsg")).toMap();
    if (step1LastMsg.value(QStringLiteral("id")).toLongLong() != 256 ||
        step1LastMsg.value(QStringLiteral("dlc")).toLongLong() != 8 ||
        !step1LastMsg.value(QStringLiteral("extended")).toBool() ||
        step1LastMsg.value(QStringLiteral("header")).toMap().value(QStringLiteral("timestamp")).toLongLong() != 1000) {
        std::fprintf(stderr, "FAIL: struct context sim: lastMsg context variable was not correctly updated by event payload\n");
        return 1;
    }
    if (sim->contextValues().value(QStringLiteral("rxCount")).toLongLong() != 1) {
        std::fprintf(stderr, "FAIL: struct context sim: rxCount was not incremented by entry action\n");
        return 1;
    }

    // 3. Member assignment via self-transition: event.id == 512, lastMsg.id = event.id
    QVariantMap idOnlyMap;
    idOnlyMap[QStringLiteral("id")] = 512;
    sim->sendEvent(QStringLiteral("rxFrame"), idOnlyMap);
    if (sim->activeStateId() != 2) {
        std::fprintf(stderr, "FAIL: struct context sim: self-transition did not stay in Processing\n");
        return 1;
    }
    const QVariantMap step2LastMsg = sim->contextValues().value(QStringLiteral("lastMsg")).toMap();
    if (step2LastMsg.value(QStringLiteral("id")).toLongLong() != 512 ||
        step2LastMsg.value(QStringLiteral("dlc")).toLongLong() != 8 ||
        step2LastMsg.value(QStringLiteral("header")).toMap().value(QStringLiteral("timestamp")).toLongLong() != 1000) {
        std::fprintf(stderr, "FAIL: struct context sim: member assignment lastMsg.id did not preserve other fields\n");
        return 1;
    }
    // Internal self-transition: Processing not exited/re-entered -> rxCount remains 1
    if (sim->contextValues().value(QStringLiteral("rxCount")).toLongLong() != 1) {
        std::fprintf(stderr, "FAIL: struct context sim: rxCount should remain 1 on internal self-transition\n");
        return 1;
    }

    // 4. Scalar event payload: tick with 42
    sim->sendEvent(QStringLiteral("tick"), 42);
    if (sim->activeStateId() != 2) {
        std::fprintf(stderr, "FAIL: struct context sim: tick self-transition failed\n");
        return 1;
    }
    // Internal self-transition: rxCount += event (42) -> 1 + 42 = 43
    if (sim->contextValues().value(QStringLiteral("rxCount")).toLongLong() != 43) {
        std::fprintf(stderr, "FAIL: struct context sim: scalar payload calculation failed (rxCount=%lld, want 43)\n",
                     sim->contextValues().value(QStringLiteral("rxCount")).toLongLong());
        return 1;
    }

    // 5. Emergency transition: event.payload.header.priority == 1
    QVariantMap alarmHeader;
    alarmHeader[QStringLiteral("timestamp")] = 9999;
    alarmHeader[QStringLiteral("priority")] = 1;
    QVariantMap alarmMsg;
    alarmMsg[QStringLiteral("id")] = 888;
    alarmMsg[QStringLiteral("dlc")] = 8;
    alarmMsg[QStringLiteral("extended")] = false;
    alarmMsg[QStringLiteral("header")] = alarmHeader;

    // Test SendEventRequested command dispatch via Kernel in Simulate mode
    sim->setMode(app::events::Mode::Simulate);
    kernel.send(app::events::SendEventRequested{.name = QStringLiteral("rxFrame"), .payload = alarmMsg});
    if (sim->activeStateId() != 3) {
        std::fprintf(stderr, "FAIL: struct context sim: alarmMsg did not transition to Emergency\n");
        return 1;
    }
    if (sim->contextValues().value(QStringLiteral("errorCount")).toLongLong() != 1) {
        std::fprintf(stderr, "FAIL: struct context sim: errorCount is not 1\n");
        return 1;
    }
    const QVariantMap step4LastMsg = sim->contextValues().value(QStringLiteral("lastMsg")).toMap();
    if (step4LastMsg.value(QStringLiteral("id")).toLongLong() != 888 ||
        step4LastMsg.value(QStringLiteral("header")).toMap().value(QStringLiteral("priority")).toLongLong() != 1) {
        std::fprintf(stderr, "FAIL: struct context sim: emergency lastMsg not updated\n");
        return 1;
    }

    // 6. Multi-step Back replay verification!
    // Step 1 Back: undo Emergency step 5 -> back to Processing, errorCount == 0, rxCount == 43
    sim->back();
    if (sim->activeStateId() != 2) {
        std::fprintf(stderr, "FAIL: struct context sim: Back 1 did not restore Processing state\n");
        return 1;
    }
    if (sim->contextValues().value(QStringLiteral("errorCount")).toLongLong() != 0) {
        std::fprintf(stderr, "FAIL: struct context sim: Back 1 did not restore errorCount to 0\n");
        return 1;
    }
    if (sim->contextValues().value(QStringLiteral("rxCount")).toLongLong() != 43) {
        std::fprintf(stderr, "FAIL: struct context sim: Back 1 did not restore rxCount to 43\n");
        return 1;
    }

    // Step 2 Back: undo tick step 4 -> rxCount == 1
    sim->back();
    if (sim->contextValues().value(QStringLiteral("rxCount")).toLongLong() != 1) {
        std::fprintf(stderr, "FAIL: struct context sim: Back 2 did not restore rxCount to 1\n");
        return 1;
    }

    // Step 3 Back: undo member assignment step 3 -> lastMsg.id == 256, rxCount == 1
    sim->back();
    if (sim->contextValues().value(QStringLiteral("lastMsg")).toMap().value(QStringLiteral("id")).toLongLong() != 256) {
        std::fprintf(stderr, "FAIL: struct context sim: Back 3 did not restore lastMsg.id to 256\n");
        return 1;
    }
    if (sim->contextValues().value(QStringLiteral("rxCount")).toLongLong() != 1) {
        std::fprintf(stderr, "FAIL: struct context sim: Back 3 did not restore rxCount to 1\n");
        return 1;
    }

    // Step 4 Back: undo step 2 -> back to Idle (id 1), rxCount == 0, lastMsg.id == 0
    sim->back();
    if (sim->activeStateId() != 1) {
        std::fprintf(stderr, "FAIL: struct context sim: Back 4 did not restore Idle state\n");
        return 1;
    }
    if (sim->contextValues().value(QStringLiteral("rxCount")).toLongLong() != 0) {
        std::fprintf(stderr, "FAIL: struct context sim: Back 4 did not restore rxCount to 0\n");
        return 1;
    }
    if (sim->contextValues().value(QStringLiteral("lastMsg")).toMap().value(QStringLiteral("id")).toLongLong() != 0) {
        std::fprintf(stderr, "FAIL: struct context sim: Back 4 did not restore lastMsg to initial default\n");
        return 1;
    }

    std::printf("PASS: state-designer struct context simulation smoke (structured payload dispatch + guard evaluation + field mutation + multi-step back replay)\n");
    return 0;
}


int runSimFeaturesSmoke() {
    if (const int invokeJitResult = runInvokeJitFixtures(); invokeJitResult != 0) {
        return invokeJitResult;
    }
    if (const int reenterResult = runReenterAlwaysFixtures(); reenterResult != 0) {
        return reenterResult;
    }
    if (const int raiseResult = runRaiseSimFixtures(); raiseResult != 0) {
        return raiseResult;
    }
    if (const int actorCommResult = runActorCommunicationSimFixtures(); actorCommResult != 0) {
        return actorCommResult;
    }
    if (const int wildcardResult = runWildcardEventSimFixtures(); wildcardResult != 0) {
        return wildcardResult;
    }
    if (const int multiTargetResult = runMultiTargetSimFixtures(); multiTargetResult != 0) {
        return multiTargetResult;
    }
    return runStructContextSimFixtures();
}

