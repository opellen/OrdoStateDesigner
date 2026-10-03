// --smoke phases: the sim interpreter family -- the three flat sim smokes
// (topology interpreter, targetless-self, root-event fallback), hierarchical
// execution semantics, History (shallow/deep) semantics, and Back() replay.

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


// The interpreter's extended state: the context map's three seeding points,
// expression guards gating, assign actions writing context in firing order, and
// Back landing the context where the replay leaves it. File-local; called from
// runBackReplaySimSmoke(), which supplies its entry point.
static int runContextJitFixtures() {
    // Substring search over trace lines: QStringList::contains is whole-element
    // equality and would never match these lines.
    const auto traceIndexOf = [](const QStringList& trace, const QString& needle) {
        for (int i = 0; i < trace.size(); ++i) {
            if (trace.at(i).contains(needle)) {
                return i;
            }
        }
        return -1;
    };
    // Mints one context variable through the same intents the Logic panel uses,
    // never by writing Machine::context directly. Returns its id.
    const auto addVariable = [](ordo::core::Kernel& kernel, app::MachineDocAgent& doc, const QString& name,
                                 app::ContextType type, const QString& initialValue) {
        const quint64 id = doc.machine().nextId;
        kernel.send(app::events::AddContextVariableRequested{});
        kernel.send(app::events::RenameContextVariableRequested{.id = id, .name = name});
        kernel.send(app::events::SetContextTypeRequested{.id = id, .type = type});
        kernel.send(app::events::SetContextInitialValueRequested{.id = id, .initialValue = initialValue});
        return id;
    };

    // ==== fixture 1: action/entry firing order ==================================
    // A -Go[gate == 0]-> B, action `gate = 1`, B's entry action `seen = gate`.
    // Actions run before transitionTo: the guard read the pre-assign gate, and
    // B's entry action read the post-assign one (seen == 1, not 0).
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: context-jit fixture 1 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("gate"), app::ContextType::Int, QStringLiteral("0"));
        addVariable(kernel, *doc, QStringLiteral("seen"), app::ContextType::Int, QStringLiteral("-1"));
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::RenameStateRequested{.id = aId, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = bId, .name = QStringLiteral("B")});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = bId, .entryActions = QStringList{QStringLiteral("seen = gate")}});
        const quint64 goId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = goId, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = goId, .guard = QStringLiteral("gate == 0")});
        kernel.send(app::events::SetTransitionActionRequested{.id = goId, .action = QStringLiteral("gate = 1")});
        removeEditCommands(kernel);

        sim->run();
        if (sim->contextValues().value(QStringLiteral("gate")).toLongLong() != 0 ||
            sim->contextValues().value(QStringLiteral("seen")).toLongLong() != -1) {
            std::fprintf(stderr, "FAIL: context-jit fixture 1 Run did not seed gate=0/seen=-1 from the schema\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != bId) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 1 Go did not fire -- the guard 'gate == 0' saw the POST-assign "
                          "value, breaking the v5 firing order\n");
            return 1;
        }
        if (sim->contextValues().value(QStringLiteral("seen")).toLongLong() != 1) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 1 destination entry action did not see the transition's assign "
                          "(seen=%lld, expected 1)\n",
                          static_cast<long long>(sim->contextValues().value(QStringLiteral("seen")).toLongLong()));
            return 1;
        }
        const QStringList trace = sim->trace();
        const int assignIndex = traceIndexOf(trace, QStringLiteral("assign: gate = 1"));
        const int entryIndex = traceIndexOf(trace, QStringLiteral("assign: seen = 1"));
        if (assignIndex < 0 || entryIndex < 0 || assignIndex > entryIndex) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 1 trace did not carry the transition assign BEFORE the "
                          "destination's entry assign (indices %d, %d)\n",
                          assignIndex, entryIndex);
            return 1;
        }
    }

    // ==== fixture 2: entry/exit assigns, exit-then-entry across an LCCA =========
    // P{A{X}, B}, X -Go-> B: the LCCA is P, so the exit set is {X, A}
    // deepest-first and the entry set is {B}. One assign per position
    // (step = 1 on X's exit, 2 on A's exit, 3 on B's entry) turns the ordering
    // into a value: the last writer wins, so step == 3 only if entry runs after
    // every exit.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: context-jit fixture 2 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("step"), app::ContextType::Int, QStringLiteral("0"));
        const quint64 pId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});
        const quint64 xId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});
        kernel.send(app::events::RenameStateRequested{.id = pId, .name = QStringLiteral("P")});
        kernel.send(app::events::RenameStateRequested{.id = aId, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = xId, .name = QStringLiteral("X")});
        kernel.send(app::events::RenameStateRequested{.id = bId, .name = QStringLiteral("B")});
        kernel.send(app::events::ReparentStateRequested{.id = aId, .parentId = pId});
        kernel.send(app::events::ReparentStateRequested{.id = xId, .parentId = aId});
        kernel.send(app::events::ReparentStateRequested{.id = bId, .parentId = pId});
        kernel.send(app::events::SetInitialChildRequested{.stateId = pId, .initialChildId = aId});
        kernel.send(app::events::SetInitialStateRequested{.id = pId});
        kernel.send(app::events::SetExitActionsRequested{.id = xId,
                                                          .exitActions = QStringList{QStringLiteral("step = 1")}});
        kernel.send(app::events::SetExitActionsRequested{.id = aId,
                                                          .exitActions = QStringList{QStringLiteral("step = 2")}});
        kernel.send(app::events::SetEntryActionsRequested{.id = bId,
                                                           .entryActions = QStringList{QStringLiteral("step = 3")}});
        const quint64 goId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = xId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = goId, .event = QStringLiteral("Go")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != bId) {
            std::fprintf(stderr, "FAIL: context-jit fixture 2 Go did not land on B\n");
            return 1;
        }
        if (sim->contextValues().value(QStringLiteral("step")).toLongLong() != 3) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 2 entry assign did not run LAST across the LCCA (step=%lld, "
                          "expected 3)\n",
                          static_cast<long long>(sim->contextValues().value(QStringLiteral("step")).toLongLong()));
            return 1;
        }
        const QStringList trace = sim->trace();
        const int deepExit = traceIndexOf(trace, QStringLiteral("assign: step = 1"));
        const int shallowExit = traceIndexOf(trace, QStringLiteral("assign: step = 2"));
        const int entry = traceIndexOf(trace, QStringLiteral("assign: step = 3"));
        if (deepExit < 0 || shallowExit < 0 || entry < 0 || !(deepExit < shallowExit && shallowExit < entry)) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 2 assign trace order was not deepest-exit, exit, entry "
                          "(indices %d, %d, %d)\n",
                          deepExit, shallowExit, entry);
            return 1;
        }
    }

    // ==== fixture 3: an expression guard GATES + Run/Reset reseed ===============
    // S -Go[level > 5]-> High and S -Go-> Low, in that document order: the
    // first passing guard wins, so the path taken is a pure readout of the
    // expression's value. Same machine, two runs: level 9 takes High, then the
    // schema's initialValue drops to 1 and Reset reseeds -- Low.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: context-jit fixture 3 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        const quint64 levelId =
            addVariable(kernel, *doc, QStringLiteral("level"), app::ContextType::Int, QStringLiteral("9"));
        const quint64 sId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 highId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        const quint64 lowId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});
        kernel.send(app::events::RenameStateRequested{.id = sId, .name = QStringLiteral("S")});
        kernel.send(app::events::RenameStateRequested{.id = highId, .name = QStringLiteral("High")});
        kernel.send(app::events::RenameStateRequested{.id = lowId, .name = QStringLiteral("Low")});
        kernel.send(app::events::SetInitialStateRequested{.id = sId});
        const quint64 toHighId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = sId, .to = highId});
        kernel.send(app::events::SetTransitionEventRequested{.id = toHighId, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = toHighId, .guard = QStringLiteral("level > 5")});
        const quint64 toLowId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = sId, .to = lowId});
        kernel.send(app::events::SetTransitionEventRequested{.id = toLowId, .event = QStringLiteral("Go")});

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != highId) {
            std::fprintf(stderr, "FAIL: context-jit fixture 3 level=9 did not take the 'level > 5' path to High\n");
            return 1;
        }
        kernel.send(app::events::SetContextInitialValueRequested{.id = levelId, .initialValue = QStringLiteral("1")});
        sim->reset();
        if (sim->contextValues().value(QStringLiteral("level")).toLongLong() != 1) {
            std::fprintf(stderr, "FAIL: context-jit fixture 3 Reset did not reseed level from the edited schema\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != lowId) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 3 level=1 did not fall through the failing guard to Low\n");
            return 1;
        }
        removeEditCommands(kernel);
    }

    // ==== fixture 4: undecidable guards are FALSE, with the reason traced =======
    // An unparseable guard ('count >') and an ill-typed one ('count > \'x\'',
    // Int against String) must each refuse to fire and say why -- never
    // true-by-default, never a throw.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: context-jit fixture 4 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("count"), app::ContextType::Int, QStringLiteral("1"));
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 badId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = badId, .event = QStringLiteral("Bad")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = badId, .guard = QStringLiteral("count >")});
        const quint64 worseId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = worseId, .event = QStringLiteral("Worse")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = worseId, .guard = QStringLiteral("count > 'x'")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Bad"));
        if (sim->activeStateId() != aId || !sim->firedTransitionIds().empty()) {
            std::fprintf(stderr, "FAIL: context-jit fixture 4 an UNPARSEABLE guard let its transition fire\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), QStringLiteral("guard: count > → false (")) < 0) {
            std::fprintf(stderr, "FAIL: context-jit fixture 4 unparseable guard did not trace its reason\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Worse"));
        if (sim->activeStateId() != aId || !sim->firedTransitionIds().empty()) {
            std::fprintf(stderr, "FAIL: context-jit fixture 4 an ILL-TYPED guard let its transition fire\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), QStringLiteral("guard: count > 'x' → false (")) < 0) {
            std::fprintf(stderr, "FAIL: context-jit fixture 4 ill-typed guard did not trace its reason\n");
            return 1;
        }
    }

    // ==== fixture 5: a bare identifier still routes through the toggle table ====
    // A context variable named canGo, seeded true, must not decide a guard whose
    // text is the bare word `canGo`; the guardResults_ toggle does, both ways.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: context-jit fixture 5 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("canGo"), app::ContextType::Bool, QStringLiteral("true"));
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 goId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = goId, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = goId, .guard = QStringLiteral("canGo")});
        removeEditCommands(kernel);

        sim->run();
        sim->setGuardResult(QStringLiteral("canGo"), false);
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != aId) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 5 a bare-identifier guard toggled FALSE still fired (the hook "
                          "path started consulting the schema)\n");
            return 1;
        }
        if (sim->guardResult(QStringLiteral("canGo"))) {
            std::fprintf(stderr, "FAIL: context-jit fixture 5 guardResult() stopped reading the toggle table\n");
            return 1;
        }
        sim->setGuardResult(QStringLiteral("canGo"), true);
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != bId) {
            std::fprintf(stderr, "FAIL: context-jit fixture 5 a bare-identifier guard toggled TRUE did not fire\n");
            return 1;
        }
    }

    // ==== fixture 6: Run and Reset reseed, Pause/resume does not ================
    // n starts at 5 and an assign takes it to 42. Reset reseeds; leaving Simulate
    // and Running again reseeds; Pause then Run is a resume and must keep 42.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: context-jit fixture 6 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("n"), app::ContextType::Int, QStringLiteral("5"));
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 goId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = goId, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionActionRequested{.id = goId, .action = QStringLiteral("n = 42")});
        removeEditCommands(kernel);

        sim->run();
        if (sim->contextValues().value(QStringLiteral("n")).toLongLong() != 5) {
            std::fprintf(stderr, "FAIL: context-jit fixture 6 Run did not seed n=5\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->contextValues().value(QStringLiteral("n")).toLongLong() != 42) {
            std::fprintf(stderr, "FAIL: context-jit fixture 6 the transition assign did not write n=42\n");
            return 1;
        }
        sim->reset();
        if (sim->contextValues().value(QStringLiteral("n")).toLongLong() != 5) {
            std::fprintf(stderr, "FAIL: context-jit fixture 6 Reset did not reseed n back to 5\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go"));
        sim->pause();
        sim->run();  // RESUME, not a fresh start -- the configuration and the context both stand
        if (sim->contextValues().value(QStringLiteral("n")).toLongLong() != 42) {
            std::fprintf(stderr, "FAIL: context-jit fixture 6 resuming from Pause wrongly reseeded the context\n");
            return 1;
        }
        // Leaving Simulate empties the configuration, so the next Run is a fresh start (reseeds without a Reset).
        sim->setMode(app::events::Mode::Simulate);
        sim->setMode(app::events::Mode::Design);
        sim->run();
        if (sim->contextValues().value(QStringLiteral("n")).toLongLong() != 5) {
            std::fprintf(stderr, "FAIL: context-jit fixture 6 Run's fresh-start path did not reseed n=5\n");
            return 1;
        }
    }

    // ==== fixture 7: Back restores the context ==================================
    // A -Go1[n = 1]-> B -Go2[n = 2]-> C. One Back undoes macrostep 2, so the
    // context must read 1 (what stood after macrostep 1); a second Back replays
    // nothing, so it must read the seeded 0. No context value is rolled back
    // directly: the replay base reseeds and the existing replay reapplies.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: context-jit fixture 7 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("n"), app::ContextType::Int, QStringLiteral("0"));
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        const quint64 cId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 go1Id = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = go1Id, .event = QStringLiteral("Go1")});
        kernel.send(app::events::SetTransitionActionRequested{.id = go1Id, .action = QStringLiteral("n = 1")});
        const quint64 go2Id = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = bId, .to = cId});
        kernel.send(app::events::SetTransitionEventRequested{.id = go2Id, .event = QStringLiteral("Go2")});
        kernel.send(app::events::SetTransitionActionRequested{.id = go2Id, .action = QStringLiteral("n = 2")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go1"));
        sim->sendEvent(QStringLiteral("Go2"));
        if (sim->activeStateId() != cId || sim->contextValues().value(QStringLiteral("n")).toLongLong() != 2) {
            std::fprintf(stderr, "FAIL: context-jit fixture 7 the two macrosteps did not reach C with n=2\n");
            return 1;
        }
        sim->back();
        if (sim->activeStateId() != bId || sim->contextValues().value(QStringLiteral("n")).toLongLong() != 1) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 7 one Back did not restore the pre-macrostep-2 context "
                          "(n=%lld, expected 1)\n",
                          static_cast<long long>(sim->contextValues().value(QStringLiteral("n")).toLongLong()));
            return 1;
        }
        sim->back();
        if (sim->activeStateId() != aId || sim->contextValues().value(QStringLiteral("n")).toLongLong() != 0) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 7 Back to the base did not leave the freshly seeded context "
                          "(n=%lld, expected 0)\n",
                          static_cast<long long>(sim->contextValues().value(QStringLiteral("n")).toLongLong()));
            return 1;
        }
    }

    // ==== fixture 8: the four typed assigns, with numeric promotion =============
    // All four ContextTypes: an Int into a Double target promotes (3 -> 3.0), a
    // mixed comparison promotes inside the RHS, and a Double into an Int target
    // truncates (emitted code must agree). The Check guard re-reads every value
    // through expr::evaluate.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: context-jit fixture 8 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("ratio"), app::ContextType::Double, QStringLiteral("0.5"));
        addVariable(kernel, *doc, QStringLiteral("count"), app::ContextType::Int, QStringLiteral("2"));
        addVariable(kernel, *doc, QStringLiteral("label"), app::ContextType::String, QStringLiteral("idle"));
        addVariable(kernel, *doc, QStringLiteral("flag"), app::ContextType::Bool, QStringLiteral("false"));
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        const quint64 cId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = bId,
            .entryActions = QStringList{QStringLiteral("ratio = 3"), QStringLiteral("flag = count > 1.5"),
                                        QStringLiteral("label = 'done'"), QStringLiteral("count = 2.75")}});
        const quint64 goId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = goId, .event = QStringLiteral("Go")});
        const quint64 checkId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = bId, .to = cId});
        kernel.send(app::events::SetTransitionEventRequested{.id = checkId, .event = QStringLiteral("Check")});
        kernel.send(app::events::SetTransitionGuardRequested{
            .id = checkId,
            .guard = QStringLiteral("ratio > 2.5 && flag == true && label == 'done' && count == 2")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        const QVariantMap values = sim->contextValues();
        if (values.value(QStringLiteral("ratio")).toDouble() != 3.0) {
            std::fprintf(stderr, "FAIL: context-jit fixture 8 an Int RHS did not promote into a Double target\n");
            return 1;
        }
        if (!values.value(QStringLiteral("flag")).toBool()) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 8 a Bool assign over a mixed Int/Double comparison did not "
                          "write true\n");
            return 1;
        }
        if (values.value(QStringLiteral("label")).toString() != QStringLiteral("done")) {
            std::fprintf(stderr, "FAIL: context-jit fixture 8 a String assign did not write its text\n");
            return 1;
        }
        if (values.value(QStringLiteral("count")).toLongLong() != 2) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 8 a Double RHS into an Int target did not truncate to 2 "
                          "(count=%lld)\n",
                          static_cast<long long>(values.value(QStringLiteral("count")).toLongLong()));
            return 1;
        }
        if (traceIndexOf(sim->trace(), QStringLiteral("assign: label = 'done'")) < 0) {
            std::fprintf(stderr, "FAIL: context-jit fixture 8 the String assign's trace line did not name its value\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Check"));
        if (sim->activeStateId() != cId) {
            std::fprintf(stderr,
                          "FAIL: context-jit fixture 8 the compound guard could not read the four written values "
                          "back through expr::evaluate\n");
            return 1;
        }
    }

    // ==== fixture 9: a malformed initialValue seeds zero and still runs =========
    // The validator reports a bad initialValue; the simulator runs anyway. 'abc'
    // as an Int seeds 0, traces one context line, and `n == 0` then passes.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: context-jit fixture 9 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("n"), app::ContextType::Int, QStringLiteral("abc"));
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 goId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = goId, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = goId, .guard = QStringLiteral("n == 0")});
        removeEditCommands(kernel);

        sim->run();
        if (!sim->running() || sim->contextValues().value(QStringLiteral("n")).toLongLong() != 0) {
            std::fprintf(stderr, "FAIL: context-jit fixture 9 a malformed Int initialValue did not seed 0 and run\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), QStringLiteral("context: n = 0 (initial value 'abc'")) < 0) {
            std::fprintf(stderr, "FAIL: context-jit fixture 9 the malformed initialValue was not traced once\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != bId) {
            std::fprintf(stderr, "FAIL: context-jit fixture 9 the run did not continue over the seeded zero\n");
            return 1;
        }
    }

    // ==== fixture 10: nested Object context seeding, member assign, and member guard evaluation =========
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("user"), app::ContextType::Object,
                    QStringLiteral("{\"name\": \"Alice\", \"age\": 30, \"active\": true, \"profile\": {\"score\": 90.5}}"));
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        const quint64 cId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});
        const quint64 dId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});

        // T1: A -> B on "Grow", action: "user.age = user.age + 1"
        const quint64 t1Id = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = t1Id, .event = QStringLiteral("Grow")});
        kernel.send(app::events::SetTransitionActionRequested{.id = t1Id, .action = QStringLiteral("user.age = user.age + 1")});

        // T2: B -> C on "Check", guard: "user.age == 31 && user.profile.score > 90.0", action: "user.profile.score = 99.0"
        const quint64 t2Id = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = bId, .to = cId});
        kernel.send(app::events::SetTransitionEventRequested{.id = t2Id, .event = QStringLiteral("Check")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = t2Id, .guard = QStringLiteral("user.age == 31 && user.profile.score > 90.0")});
        kernel.send(app::events::SetTransitionActionRequested{.id = t2Id, .action = QStringLiteral("user.profile.score = 99.0")});

        // T3: C -> D on "Rename", action: "user.name = 'Bob'"
        const quint64 t3Id = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = cId, .to = dId});
        kernel.send(app::events::SetTransitionEventRequested{.id = t3Id, .event = QStringLiteral("Rename")});
        kernel.send(app::events::SetTransitionActionRequested{.id = t3Id, .action = QStringLiteral("user.name = 'Bob'")});

        removeEditCommands(kernel);

        sim->run();
        if (!sim->running()) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 sim did not run\n");
            return 1;
        }

        // Verify seeded values
        const QVariantMap userMap = sim->contextValues().value(QStringLiteral("user")).toMap();
        if (userMap.value(QStringLiteral("name")).toString() != QStringLiteral("Alice") ||
            userMap.value(QStringLiteral("age")).toLongLong() != 30 ||
            userMap.value(QStringLiteral("profile")).toMap().value(QStringLiteral("score")).toDouble() != 90.5) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 Object initialValue not seeded into QVariantMap\n");
            return 1;
        }

        // 1. Send "Grow" -> transitions to B, updates user.age = 31
        sim->sendEvent(QStringLiteral("Grow"));
        if (sim->activeStateId() != bId) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 did not transition to B on Grow\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), QStringLiteral("assign: user.age = 31")) < 0) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 trace did not record user.age = 31\n");
            return 1;
        }
        if (sim->contextValues().value(QStringLiteral("user")).toMap().value(QStringLiteral("age")).toLongLong() != 31) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 user.age was not updated to 31\n");
            return 1;
        }

        // 2. Send "Check" -> guard passes (age == 31 && profile.score > 90.0), transitions to C, updates score = 99.0
        sim->sendEvent(QStringLiteral("Check"));
        if (sim->activeStateId() != cId) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 did not transition to C on Check with member guard\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), QStringLiteral("assign: user.profile.score = 99")) < 0) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 trace did not record user.profile.score update\n");
            return 1;
        }
        if (sim->contextValues().value(QStringLiteral("user")).toMap().value(QStringLiteral("profile")).toMap().value(QStringLiteral("score")).toDouble() != 99.0) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 user.profile.score was not updated to 99.0\n");
            return 1;
        }

        // 3. Send "Rename" -> transitions to D, updates user.name = 'Bob'
        sim->sendEvent(QStringLiteral("Rename"));
        if (sim->activeStateId() != dId) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 did not transition to D on Rename\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), QStringLiteral("assign: user.name = 'Bob'")) < 0) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 trace did not record user.name = 'Bob'\n");
            return 1;
        }
        if (sim->contextValues().value(QStringLiteral("user")).toMap().value(QStringLiteral("name")).toString() != QStringLiteral("Bob")) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 user.name was not updated to Bob\n");
            return 1;
        }

        // 4. Back replay: 3 steps
        sim->back(); // D -> C
        if (sim->activeStateId() != cId ||
            sim->contextValues().value(QStringLiteral("user")).toMap().value(QStringLiteral("name")).toString() != QStringLiteral("Alice")) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 back to C did not restore user.name to Alice\n");
            return 1;
        }
        sim->back(); // C -> B
        if (sim->activeStateId() != bId ||
            sim->contextValues().value(QStringLiteral("user")).toMap().value(QStringLiteral("profile")).toMap().value(QStringLiteral("score")).toDouble() != 90.5) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 back to B did not restore user.profile.score to 90.5\n");
            return 1;
        }
        sim->back(); // B -> A
        if (sim->activeStateId() != aId ||
            sim->contextValues().value(QStringLiteral("user")).toMap().value(QStringLiteral("age")).toLongLong() != 30) {
            std::fprintf(stderr, "FAIL: context-jit fixture 10 back to A did not restore user.age to 30\n");
            return 1;
        }
    }

    std::printf("PASS: state-designer context JIT smoke (v5 order trap + entry/exit assigns across an LCCA + "
                 "expression guard gating + undecidable guards false and traced + bare-identifier toggle path + "
                 "Run/Reset reseed + Back restores context + typed assigns with promotion + malformed initial "
                 "value + nested Object context & member assign)\n");
    return 0;
}

// The structured guard-evaluation record: lastGuardEvaluations() carries one
// entry per guard guardPasses() decided, hook and expression alike, in walk
// order, for the current macrostep only. File-local; called from
// runBackReplaySimSmoke()'s tail.
static int runGuardEvaluationFixtures() {
    // Same helper as in runContextJitFixtures(), redefined because it is local to that function.
    const auto addVariable = [](ordo::core::Kernel& kernel, app::MachineDocAgent& doc, const QString& name,
                                 app::ContextType type, const QString& initialValue) {
        const quint64 id = doc.machine().nextId;
        kernel.send(app::events::AddContextVariableRequested{});
        kernel.send(app::events::RenameContextVariableRequested{.id = id, .name = name});
        kernel.send(app::events::SetContextTypeRequested{.id = id, .type = type});
        kernel.send(app::events::SetContextInitialValueRequested{.id = id, .initialValue = initialValue});
        return id;
    };

    // ==== fixture 1: a guarded group of three candidates =========================
    // A -Go[x==1]-> B (fails), A -Go[x==0]-> C (passes, fires), A -Go[x==2]->
    // D (never reached: firstMatchingHandler() returns as soon as a guard
    // passes). The record holds exactly the first two, in walk order, with their
    // own transitionIds and results.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 1 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("x"), app::ContextType::Int, QStringLiteral("0"));
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        const quint64 cId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});
        const quint64 dId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});
        kernel.send(app::events::RenameStateRequested{.id = aId, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = bId, .name = QStringLiteral("B")});
        kernel.send(app::events::RenameStateRequested{.id = cId, .name = QStringLiteral("C")});
        kernel.send(app::events::RenameStateRequested{.id = dId, .name = QStringLiteral("D")});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 failId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = failId, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = failId, .guard = QStringLiteral("x == 1")});
        const quint64 passId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = cId});
        kernel.send(app::events::SetTransitionEventRequested{.id = passId, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = passId, .guard = QStringLiteral("x == 0")});
        const quint64 neverId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = dId});
        kernel.send(app::events::SetTransitionEventRequested{.id = neverId, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = neverId, .guard = QStringLiteral("x == 2")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != cId) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 1 the second candidate's passing guard did not fire\n");
            return 1;
        }
        const QVector<app::GuardEvaluation> evals = sim->lastGuardEvaluations();
        if (evals.size() != 2) {
            std::fprintf(stderr,
                          "FAIL: guard-eval fixture 1 expected exactly 2 evaluated candidates, got %d (the walk did "
                          "not stop where it should, or recorded too few)\n",
                          static_cast<int>(evals.size()));
            return 1;
        }
        if (evals.at(0).transitionId != failId || evals.at(0).source != QStringLiteral("x == 1") ||
            !evals.at(0).decided || evals.at(0).result) {
            std::fprintf(stderr,
                          "FAIL: guard-eval fixture 1 entry 0 did not describe the failing 'x == 1' candidate\n");
            return 1;
        }
        if (evals.at(1).transitionId != passId || evals.at(1).source != QStringLiteral("x == 0") ||
            !evals.at(1).decided || !evals.at(1).result) {
            std::fprintf(stderr,
                          "FAIL: guard-eval fixture 1 entry 1 did not describe the passing 'x == 0' candidate\n");
            return 1;
        }
        for (const app::GuardEvaluation& eval : evals) {
            if (eval.transitionId == neverId) {
                std::fprintf(stderr,
                              "FAIL: guard-eval fixture 1 the third candidate was evaluated after the walk had "
                              "already stopped\n");
                return 1;
            }
        }
    }

    // ==== fixture 2: a hook guard's entry carries the toggle table's result ====
    // (flipping the toggle flips it)
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 2 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::RenameStateRequested{.id = aId, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = bId, .name = QStringLiteral("B")});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 goId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = goId, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = goId, .guard = QStringLiteral("canGo")});
        removeEditCommands(kernel);

        sim->run();
        sim->setGuardResult(QStringLiteral("canGo"), false);
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != aId) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 2 the toggled-false hook guard wrongly fired\n");
            return 1;
        }
        QVector<app::GuardEvaluation> evals = sim->lastGuardEvaluations();
        if (evals.size() != 1 || evals.at(0).transitionId != goId || evals.at(0).source != QStringLiteral("canGo") ||
            !evals.at(0).decided || evals.at(0).result) {
            std::fprintf(stderr,
                          "FAIL: guard-eval fixture 2 the toggled-false hook's recorded result was not false\n");
            return 1;
        }
        sim->setGuardResult(QStringLiteral("canGo"), true);
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != bId) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 2 the toggled-true hook guard did not fire\n");
            return 1;
        }
        evals = sim->lastGuardEvaluations();
        if (evals.size() != 1 || evals.at(0).transitionId != goId || !evals.at(0).decided || !evals.at(0).result) {
            std::fprintf(stderr,
                          "FAIL: guard-eval fixture 2 flipping the toggle did not flip the recorded result to "
                          "true\n");
            return 1;
        }
    }

    // ==== fixture 3: an expression guard's entry carries its evaluated result ===
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 3 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("level"), app::ContextType::Int, QStringLiteral("9"));
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::RenameStateRequested{.id = aId, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = bId, .name = QStringLiteral("B")});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 goId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = goId, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = goId, .guard = QStringLiteral("level > 5")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != bId) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 3 the passing expression guard did not fire\n");
            return 1;
        }
        const QVector<app::GuardEvaluation> evals = sim->lastGuardEvaluations();
        if (evals.size() != 1 || evals.at(0).transitionId != goId ||
            evals.at(0).source != QStringLiteral("level > 5") || !evals.at(0).decided || !evals.at(0).result) {
            std::fprintf(stderr,
                          "FAIL: guard-eval fixture 3 the expression guard's entry did not carry its true "
                          "result\n");
            return 1;
        }
    }

    // ==== fixture 4: an undecidable guard's entry is decided==false, result==false
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 4 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        addVariable(kernel, *doc, QStringLiteral("count"), app::ContextType::Int, QStringLiteral("1"));
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::RenameStateRequested{.id = aId, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = bId, .name = QStringLiteral("B")});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 badId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = badId, .event = QStringLiteral("Bad")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = badId, .guard = QStringLiteral("count >")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Bad"));
        if (sim->activeStateId() != aId) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 4 an unparseable guard wrongly let its transition fire\n");
            return 1;
        }
        const QVector<app::GuardEvaluation> evals = sim->lastGuardEvaluations();
        if (evals.size() != 1 || evals.at(0).transitionId != badId ||
            evals.at(0).source != QStringLiteral("count >") || evals.at(0).decided || evals.at(0).result) {
            std::fprintf(stderr,
                          "FAIL: guard-eval fixture 4 the unparseable guard's entry was not decided==false, "
                          "result==false\n");
            return 1;
        }
    }

    // ==== fixture 5: cleared per macrostep (a second macrostep's record describes only the second)
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 5 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        const quint64 cId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});
        kernel.send(app::events::RenameStateRequested{.id = aId, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = bId, .name = QStringLiteral("B")});
        kernel.send(app::events::RenameStateRequested{.id = cId, .name = QStringLiteral("C")});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 go1Id = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = go1Id, .event = QStringLiteral("Go1")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = go1Id, .guard = QStringLiteral("hook1")});
        const quint64 go2Id = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = bId, .to = cId});
        kernel.send(app::events::SetTransitionEventRequested{.id = go2Id, .event = QStringLiteral("Go2")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = go2Id, .guard = QStringLiteral("hook2")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go1"));
        if (sim->activeStateId() != bId || sim->lastGuardEvaluations().size() != 1 ||
            sim->lastGuardEvaluations().at(0).transitionId != go1Id) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 5 the first macrostep did not record its own candidate\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go2"));
        if (sim->activeStateId() != cId) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 5 the second macrostep did not fire\n");
            return 1;
        }
        const QVector<app::GuardEvaluation> evals = sim->lastGuardEvaluations();
        if (evals.size() != 1 || evals.at(0).transitionId != go2Id) {
            std::fprintf(stderr,
                          "FAIL: guard-eval fixture 5 the second macrostep's record was not cleared of the first "
                          "macrostep's candidate\n");
            return 1;
        }
        for (const app::GuardEvaluation& eval : evals) {
            if (eval.transitionId == go1Id) {
                std::fprintf(stderr,
                              "FAIL: guard-eval fixture 5 the first macrostep's candidate leaked into the second's "
                              "record\n");
                return 1;
            }
        }
    }

    // ==== fixture 6: after back() the record is empty ============================
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 6 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::RenameStateRequested{.id = aId, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = bId, .name = QStringLiteral("B")});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 goId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = goId, .event = QStringLiteral("Go")});
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != bId || sim->lastGuardEvaluations().isEmpty()) {
            std::fprintf(stderr,
                          "FAIL: guard-eval fixture 6 setup did not fire with a non-empty record before Back\n");
            return 1;
        }
        sim->back();
        if (sim->activeStateId() != aId) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 6 Back did not undo the one macrostep\n");
            return 1;
        }
        if (!sim->lastGuardEvaluations().isEmpty()) {
            std::fprintf(stderr,
                          "FAIL: guard-eval fixture 6 the record was not EMPTY after Back's guardless replay\n");
            return 1;
        }
    }

    // ==== fixture 7: run()/reset() leave it empty until the first macrostep =====
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 7 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        const quint64 aId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 bId = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::RenameStateRequested{.id = aId, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = bId, .name = QStringLiteral("B")});
        kernel.send(app::events::SetInitialStateRequested{.id = aId});
        const quint64 goId = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = aId, .to = bId});
        kernel.send(app::events::SetTransitionEventRequested{.id = goId, .event = QStringLiteral("Go")});
        removeEditCommands(kernel);

        sim->run();
        if (!sim->lastGuardEvaluations().isEmpty()) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 7 run() left a stale record before any macrostep\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go"));
        if (sim->activeStateId() != bId || sim->lastGuardEvaluations().isEmpty()) {
            std::fprintf(stderr,
                          "FAIL: guard-eval fixture 7 setup did not fire with a non-empty record before Reset\n");
            return 1;
        }
        sim->reset();
        if (!sim->lastGuardEvaluations().isEmpty()) {
            std::fprintf(stderr, "FAIL: guard-eval fixture 7 reset() left a stale record from before the reset\n");
            return 1;
        }
    }

    std::printf(
        "PASS: state-designer guard-evaluation record smoke (three-candidate walk stop + hook toggle result + "
        "expression result + undecidable false-and-flagged + cleared per macrostep + Back leaves it empty + "
        "Run/Reset leave it empty)\n");
    return 0;
}


// Back() hardening: timer freshness across Back, historyRecords_ surviving a
// Back-then-refire round trip, repeated Back down to the base picture, diverging
// after Back, parallel timers in both regions, and flat Back staying
// byte-identical. Drives the agent directly (the Simulate-mode gate stays shut
// for a hierarchical machine).
int runBackReplaySimSmoke() {
    const auto configEquals = [](const std::vector<quint64>& actual, std::initializer_list<quint64> expected) {
        return actual == std::vector<quint64>(expected);
    };

    // ==== fixture 1: state timer restored fresh by Back ==========================
    // A -Go-> D (D owns a 1000ms delayed transition to E) -Away-> F: macrostep
    // 1 lands on D (arms D's 1000ms countdown), macrostep 2 leaves D for F
    // (disarms it). Back undoes macrostep 2, landing back on D: the countdown
    // must be the full 1000ms again (armStateTimers() re-armed fresh during
    // replay), never a resumed remainder.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: back-replay fixture 1 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: D
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: F
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 4: E
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("D")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("F")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("E")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 5: A -Go-> D
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: D -Away-> F
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Away")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 4});  // id 7: D -(1000ms)-> E
        kernel.send(app::events::SetTransitionDelayRequested{.id = 7, .delayMs = 1000});
        if (doc->machine().transitions.size() != 3) {
            std::fprintf(stderr, "FAIL: back-replay fixture 1 topology build did not produce 3 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));    // macrostep 1: A -> D, arms D's 1000ms timer
        if (!configEquals(sim->configuration(), {2})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 1 Go did not land on D\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Away"));  // macrostep 2: D -> F, disarms D's timer
        if (!configEquals(sim->configuration(), {3})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 1 Away did not land on F\n");
            return 1;
        }

        sim->back();  // undoes macrostep 2 -- replays only Go, re-arming D's timer fresh
        if (!configEquals(sim->configuration(), {2})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 1 Back did not restore D\n");
            return 1;
        }
        sim->tick(999);  // fresh 1000ms -> 1 remaining; a stale (already-ticked) remainder would fire here
        if (!configEquals(sim->configuration(), {2})) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 1 tick(999) fired D's timer early -- it was not re-armed "
                          "to a fresh 1000ms by Back's replay\n");
            return 1;
        }
        sim->tick(1);  // completes the fresh 1000ms exactly
        if (!configEquals(sim->configuration(), {4})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 1 tick(1) did not fire D's re-armed timer into E\n");
            return 1;
        }
    }

    // ==== fixture 2: root timer restored fresh by Back ============================
    // A machine-level (from == 0) 800ms delayed transition arms once at Run.
    // Ticking it partway, then firing an unrelated event and Back-ing it away,
    // must leave the root timer as fresh as Run leaves it: armed once per
    // activateInitialState() call, never stacked or left at a partial remainder.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: back-replay fixture 2 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: B
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: RootTimeout
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("B")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("RootTimeout")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 4: A -Go-> B
        kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 3});  // id 5: root -(800ms)-> RootTimeout
        kernel.send(app::events::SetTransitionDelayRequested{.id = 5, .delayMs = 800});
        if (doc->machine().transitions.size() != 2) {
            std::fprintf(stderr, "FAIL: back-replay fixture 2 topology build did not produce 2 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();       // arms the root timer at 800ms
        sim->tick(300);   // 500 remaining -- no macrostep recorded, nothing due
        sim->sendEvent(QStringLiteral("Go"));  // macrostep 1: A -> B; root timer untouched (owner 0, never disarmed)
        if (!configEquals(sim->configuration(), {2})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 2 Go did not land on B\n");
            return 1;
        }

        sim->back();  // undoes macrostep 1 -- replays nothing, re-derives via activateInitialState() alone
        if (!configEquals(sim->configuration(), {1})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 2 Back did not restore A\n");
            return 1;
        }
        sim->tick(799);  // fresh 800ms -> 1 remaining; the pre-Back 500-remaining reading would already be spent
        if (!configEquals(sim->configuration(), {1})) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 2 tick(799) fired the root timer early -- it was not "
                          "re-armed to a fresh 800ms by Back's activateInitialState() re-derivation\n");
            return 1;
        }
        sim->tick(1);  // completes the fresh 800ms exactly
        if (!configEquals(sim->configuration(), {3})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 2 tick(1) did not fire the re-armed root timer\n");
            return 1;
        }
    }

    // ==== fixture 3: Back rebuilds history records ================================
    // P{A,B} (initial A) + H(shallow, parent P) + Outside. Drive A->B, leave P
    // to Outside (records historyRecords_[P] = B), re-enter via ->H (lands B),
    // then B->Outside again. Back once lands on {P,B}; Back again on {Outside};
    // re-firing ->H live from there must still land B, because replay rebuilds
    // historyRecords_ (activateInitialState() clears it unconditionally).
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: back-replay fixture 3 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: P
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: B
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 4: Outside
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 50)});  // id 5: H
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("P")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("B")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Outside")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("H")});
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // A -> P (P.initialChildId = A)
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 1});  // B -> P
        kernel.send(app::events::SetStateKindRequested{.id = 5, .kind = app::StateKind::History});
        kernel.send(app::events::ReparentStateRequested{.id = 5, .parentId = 1});  // H -> P (shallow default)
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: A -Next-> B
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Next")});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 4});  // id 7: P -Leave-> Outside (bubble)
        kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Leave")});
        kernel.send(app::events::AddTransitionRequested{.from = 4, .to = 5});  // id 8: Outside -Restore-> H
        kernel.send(app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("Restore")});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 9: B -Away-> Outside
        kernel.send(app::events::SetTransitionEventRequested{.id = 9, .event = QStringLiteral("Away")});
        if (doc->machine().states.size() != 5 || doc->machine().transitions.size() != 4) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 3 topology build did not produce 5 states / 4 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Next"));   // macrostep 1: A -> B
        sim->sendEvent(QStringLiteral("Leave"));  // macrostep 2: P -> Outside (records historyRecords_[P] = B)
        if (!configEquals(sim->configuration(), {4})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 3 Leave did not land on Outside\n");
            return 1;
        }
        const std::vector<quint64> configAfterLeave(sim->configuration().begin(), sim->configuration().end());
        const QStringList traceAfterLeave = sim->trace();

        sim->sendEvent(QStringLiteral("Restore"));  // macrostep 3: Outside -> H, lands {P,B} from the record
        if (!configEquals(sim->configuration(), {1, 3})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 3 Restore did not land {P,B} from the history record\n");
            return 1;
        }
        const std::vector<quint64> configAfterRestore(sim->configuration().begin(), sim->configuration().end());
        const QStringList traceAfterRestore = sim->trace();

        sim->sendEvent(QStringLiteral("Away"));  // macrostep 4: B -> Outside again
        if (!configEquals(sim->configuration(), {4})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 3 Away did not land on Outside\n");
            return 1;
        }

        sim->back();  // undoes macrostep 4 -- the post-H-entry picture
        if (sim->configuration() != configAfterRestore || sim->trace() != traceAfterRestore) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 3 first Back did not restore the post-H-entry picture "
                          "exactly\n");
            return 1;
        }

        sim->back();  // undoes macrostep 3 -- the pre-H picture
        if (sim->configuration() != configAfterLeave || sim->trace() != traceAfterLeave) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 3 second Back did not restore the pre-H picture exactly\n");
            return 1;
        }

        sim->sendEvent(QStringLiteral("Restore"));  // LIVE re-fire of the same ->H transition
        if (!configEquals(sim->configuration(), {1, 3})) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 3 live re-fire after Back did not still land {P,B} -- "
                          "historyRecords_ was not rebuilt by replay\n");
            return 1;
        }
    }

    // ==== fixture 4: repeated Back returns to the freshly-activated base ==========
    // A -Go1-> B -Go2-> C -Go3-> D (flat, N = 3 macrosteps). Three Back calls
    // in a row must land exactly on the picture captured right after run(),
    // configuration and trace both. A fourth Back (0 macrosteps left) is a
    // no-op: it returns before touching activateInitialState().
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: back-replay fixture 4 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: B
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: C
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 4: D
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 5: A -Go1-> B
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Go1")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: B -Go2-> C
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Go2")});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 7: C -Go3-> D
        kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Go3")});
        if (doc->machine().transitions.size() != 3) {
            std::fprintf(stderr, "FAIL: back-replay fixture 4 topology build did not produce 3 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();
        const std::vector<quint64> freshConfig(sim->configuration().begin(), sim->configuration().end());
        const QStringList freshTrace = sim->trace();
        sim->sendEvent(QStringLiteral("Go1"));
        sim->sendEvent(QStringLiteral("Go2"));
        sim->sendEvent(QStringLiteral("Go3"));
        if (!configEquals(sim->configuration(), {4})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 4 Go1/Go2/Go3 did not land on D\n");
            return 1;
        }

        sim->back();
        sim->back();
        sim->back();
        if (sim->configuration() != freshConfig || sim->trace() != freshTrace || !sim->firedTransitionIds().empty()) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 4 three Back calls did not land exactly on the "
                          "freshly-activated base\n");
            return 1;
        }

        sim->back();  // one more -- 0 macrosteps remain, must no-op (not crash, not clear further)
        if (sim->configuration() != freshConfig || sim->trace() != freshTrace || !sim->firedTransitionIds().empty()) {
            std::fprintf(stderr, "FAIL: back-replay fixture 4 the extra no-op Back disturbed the base picture\n");
            return 1;
        }
    }

    // ==== fixture 5: Back then diverge replaces the undone macrostep ==============
    // A -Go-> B, then B -Original-> C. Back undoes Original, landing back on
    // B; sending a different event (Diverge, B -> D) must produce a new macrostep
    // that replaces the undone one: firedTransitionIds() holds Go plus Diverge,
    // never Original, and trace() carries no residue of the path not taken.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: back-replay fixture 5 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: B
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: C
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 4: D
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 5: A -Go-> B
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: B -Original-> C
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Original")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 4});  // id 7: B -Diverge-> D
        kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Diverge")});
        if (doc->machine().transitions.size() != 3) {
            std::fprintf(stderr, "FAIL: back-replay fixture 5 topology build did not produce 3 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));        // macrostep 1 (transition id 5)
        sim->sendEvent(QStringLiteral("Original"));  // macrostep 2 (transition id 6) -- to be undone
        if (!configEquals(sim->configuration(), {3})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 5 Original did not land on C\n");
            return 1;
        }

        sim->back();  // undoes Original, landing back on B
        if (!configEquals(sim->configuration(), {2})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 5 Back did not restore B\n");
            return 1;
        }

        sim->sendEvent(QStringLiteral("Diverge"));  // a DIFFERENT event than the one undone
        if (!configEquals(sim->configuration(), {4})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 5 Diverge did not land on D\n");
            return 1;
        }
        const std::vector<quint64> firedIds = sim->firedTransitionIds();
        if (firedIds != std::vector<quint64>{5, 7}) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 5 firedTransitionIds() was not the N-1 prefix {5} plus the "
                          "new firing {7} -- the undone macrostep (6) leaked back in\n");
            return 1;
        }
        for (const QString& line : sim->trace()) {
            if (line.contains(QStringLiteral("Original"))) {
                std::fprintf(stderr,
                              "FAIL: back-replay fixture 5 trace still carries residue of the undone Original "
                              "macrostep\n");
                return 1;
            }
        }
    }

    // ==== fixture 6: Back through a parallel macrostep re-arms BOTH regions =======
    // Q{R1{a,b},R2{c,d}} (Parallel), initial Q. b owns a 400ms timer, d owns
    // a 600ms timer. Go moves both regions into b/d, arming both; Go2 moves both
    // away, disarming both. Back lands on {b,d} and both timers must be re-armed
    // fresh (400ms, 600ms), each firing on its own schedule measured from the Back.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: back-replay fixture 6 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});      // id 1: Q
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});     // id 2: R1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(60, 0)});     // id 3: a
        kernel.send(app::events::AddStateRequested{.pos = QPointF(70, 0)});     // id 4: b
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});    // id 5: R2
        kernel.send(app::events::AddStateRequested{.pos = QPointF(110, 0)});    // id 6: c
        kernel.send(app::events::AddStateRequested{.pos = QPointF(120, 0)});    // id 7: d
        kernel.send(app::events::AddStateRequested{.pos = QPointF(70, 50)});    // id 8: bTimeout
        kernel.send(app::events::AddStateRequested{.pos = QPointF(120, 50)});   // id 9: dTimeout
        kernel.send(app::events::AddStateRequested{.pos = QPointF(70, 100)});   // id 10: b2
        kernel.send(app::events::AddStateRequested{.pos = QPointF(120, 100)});  // id 11: d2
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // R1 -> Q
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 2});  // a -> R1
        kernel.send(app::events::ReparentStateRequested{.id = 4, .parentId = 2});  // b -> R1
        kernel.send(app::events::ReparentStateRequested{.id = 5, .parentId = 1});  // R2 -> Q
        kernel.send(app::events::ReparentStateRequested{.id = 6, .parentId = 5});  // c -> R2
        kernel.send(app::events::ReparentStateRequested{.id = 7, .parentId = 5});  // d -> R2
        kernel.send(app::events::ReparentStateRequested{.id = 8, .parentId = 2});  // bTimeout -> R1 (stays in-region)
        kernel.send(app::events::ReparentStateRequested{.id = 9, .parentId = 5});  // dTimeout -> R2 (stays in-region)
        kernel.send(app::events::ReparentStateRequested{.id = 10, .parentId = 2});  // b2 -> R1 (stays in-region)
        kernel.send(app::events::ReparentStateRequested{.id = 11, .parentId = 5});  // d2 -> R2 (stays in-region)
        kernel.send(app::events::SetStateKindRequested{.id = 1, .kind = app::StateKind::Parallel});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 12: a -Go-> b
        kernel.send(app::events::SetTransitionEventRequested{.id = 12, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 6, .to = 7});  // id 13: c -Go-> d
        kernel.send(app::events::SetTransitionEventRequested{.id = 13, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 4, .to = 8});  // id 14: b -(400ms)-> bTimeout
        kernel.send(app::events::SetTransitionDelayRequested{.id = 14, .delayMs = 400});
        kernel.send(app::events::AddTransitionRequested{.from = 7, .to = 9});  // id 15: d -(600ms)-> dTimeout
        kernel.send(app::events::SetTransitionDelayRequested{.id = 15, .delayMs = 600});
        kernel.send(app::events::AddTransitionRequested{.from = 4, .to = 10});  // id 16: b -Go2-> b2
        kernel.send(app::events::SetTransitionEventRequested{.id = 16, .event = QStringLiteral("Go2")});
        kernel.send(app::events::AddTransitionRequested{.from = 7, .to = 11});  // id 17: d -Go2-> d2
        kernel.send(app::events::SetTransitionEventRequested{.id = 17, .event = QStringLiteral("Go2")});
        if (doc->machine().transitions.size() != 6) {
            std::fprintf(stderr, "FAIL: back-replay fixture 6 topology build did not produce 6 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();
        if (!configEquals(sim->configuration(), {1, 2, 3, 5, 6})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 6 Run did not arm both regions\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go"));  // macrostep 1: both regions land on b/d, arm 400ms/600ms
        if (!configEquals(sim->configuration(), {1, 2, 4, 5, 7})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 6 Go did not land {Q,R1,b,R2,d}\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go2"));  // macrostep 2: both regions move away, disarming b/d's timers
        if (!configEquals(sim->configuration(), {1, 2, 5, 10, 11})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 6 Go2 did not land {Q,R1,R2,b2,d2}\n");
            return 1;
        }

        sim->back();  // undoes macrostep 2 -- replays only Go, re-arming BOTH regions fresh
        if (!configEquals(sim->configuration(), {1, 2, 4, 5, 7})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 6 Back did not restore {Q,R1,b,R2,d}\n");
            return 1;
        }

        sim->tick(399);  // neither fresh countdown (400ms, 600ms) is due yet
        if (!configEquals(sim->configuration(), {1, 2, 4, 5, 7})) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 6 tick(399) fired a timer early -- Back did not re-arm both "
                          "regions to their FULL delay\n");
            return 1;
        }
        sim->tick(1);  // completes b's fresh 400ms exactly; d's independently-owned 600ms has 200ms left
        if (!configEquals(sim->configuration(), {1, 2, 5, 7, 8})) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 6 tick(1) did not fire b's re-armed 400ms timer alone into "
                          "bTimeout\n");
            return 1;
        }
        sim->tick(199);  // d's remaining 200ms -> 1, still not due
        if (!configEquals(sim->configuration(), {1, 2, 5, 7, 8})) {
            std::fprintf(stderr, "FAIL: back-replay fixture 6 tick(199) fired d's timer early\n");
            return 1;
        }
        sim->tick(1);  // completes d's fresh 600ms exactly (measured from the SAME Back, independent of b's region)
        if (!configEquals(sim->configuration(), {1, 2, 5, 8, 9})) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 6 tick(1) did not fire d's re-armed 600ms timer into "
                          "dTimeout\n");
            return 1;
        }
    }

    // ==== fixture 7: flat Back stays byte-identical to the pre-hierarchy contract =
    // A flat A -Go1-> B -Go2-> C -Go3-> D machine run in two kernels: Go1,Go2 in
    // one, Go1,Go2,Go3 + one Back() in the other. Both must land on the exact
    // same picture, byte-for-byte.
    {
        std::vector<quint64> straightConfig;
        QStringList straightTrace;
        {
            ordo::core::Kernel kernel;
            kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
            kernel.registerAgent(std::make_shared<app::SimulationAgent>());
            auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
            auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
            if (!doc || !sim) {
                std::fprintf(stderr, "FAIL: back-replay fixture 7 straight-kernel agents did not all register\n");
                return 1;
            }
            registerEditCommands(kernel);
            kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: A
            kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: B
            kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: C
            kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 4: D
            kernel.send(app::events::SetInitialStateRequested{.id = 1});
            kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 5: A -Go1-> B
            kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Go1")});
            kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: B -Go2-> C
            kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Go2")});
            kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 7: C -Go3-> D
            kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Go3")});
            removeEditCommands(kernel);

            sim->run();
            sim->sendEvent(QStringLiteral("Go1"));
            sim->sendEvent(QStringLiteral("Go2"));  // stop at N-1 = 2 steps -- Go3 never fires here
            straightConfig.assign(sim->configuration().begin(), sim->configuration().end());
            straightTrace = sim->trace();
        }

        std::vector<quint64> backConfig;
        QStringList backTrace;
        {
            ordo::core::Kernel kernel;
            kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
            kernel.registerAgent(std::make_shared<app::SimulationAgent>());
            auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
            auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
            if (!doc || !sim) {
                std::fprintf(stderr, "FAIL: back-replay fixture 7 back-kernel agents did not all register\n");
                return 1;
            }
            registerEditCommands(kernel);
            kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: A
            kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: B
            kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: C
            kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 4: D
            kernel.send(app::events::SetInitialStateRequested{.id = 1});
            kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 5: A -Go1-> B
            kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Go1")});
            kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: B -Go2-> C
            kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Go2")});
            kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 7: C -Go3-> D
            kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Go3")});
            removeEditCommands(kernel);

            sim->run();
            sim->sendEvent(QStringLiteral("Go1"));
            sim->sendEvent(QStringLiteral("Go2"));
            sim->sendEvent(QStringLiteral("Go3"));  // all N = 3 steps
            sim->back();                            // undo the 3rd -- should reproduce the straight kernel exactly
            backConfig.assign(sim->configuration().begin(), sim->configuration().end());
            backTrace = sim->trace();
        }

        if (backConfig != straightConfig) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 7 Back's configuration() was not byte-identical to running "
                          "straight to N-1 steps\n");
            return 1;
        }
        if (backTrace != straightTrace) {
            std::fprintf(stderr,
                          "FAIL: back-replay fixture 7 Back's trace() was not byte-identical to running straight "
                          "to N-1 steps\n");
            return 1;
        }
    }

    std::printf(
        "PASS: state-designer back-replay hardening smoke (state timer freshness + root timer freshness + "
        "history record rebuild + repeated Back to base + Back-then-diverge + parallel macrostep timers + "
        "flat Back byte-identical)\n");


    if (const int contextJitResult = runContextJitFixtures(); contextJitResult != 0) {
        return contextJitResult;
    }
    if (const int guardEvalResult = runGuardEvaluationFixtures(); guardEvalResult != 0) {
        return guardEvalResult;
    }
    return runSimFeaturesSmoke();
}
