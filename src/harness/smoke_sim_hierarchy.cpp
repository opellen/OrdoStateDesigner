// --smoke phases for the sim interpreter: the three flat sim smokes (topology
// interpreter, targetless-self, root-event fallback), hierarchical execution
// semantics, History (shallow/deep) semantics, and Back() replay hardening.

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


// Execution semantics of hierarchical machines (the normative rules are in
// model/sim_agent.h). The fixtures drive the SimulationAgent directly
// (kernel.agentAs<SimulationAgent>()->run()/sendEvent()/tick()/back()), which
// only checks running(), never mode(), so no command layer is needed. Each
// fixture builds its own kernel and topology.
int runHierarchySimSmoke() {
    // Shared across every fixture block below.
    const auto traceIndexOf = [](const QStringList& trace, const char* needle) -> int {
        for (int i = 0; i < trace.size(); ++i) {
            if (trace[i].contains(QLatin1String(needle))) {
                return i;
            }
        }
        return -1;
    };
    const auto configEquals = [](const std::vector<quint64>& actual, std::initializer_list<quint64> expected) {
        return actual == std::vector<quint64>(expected);
    };

    // ==== fixture 1: nested entry/exit ==========================================
    // P{A{X},B}, initial P (descends P -> A -> X, rule 2). Firing X's own
    // "Go" -> B exits X then A (child-first, rule 3) and enters B (rule 2);
    // configuration() lands on {P,B} (doc order, rule 1) in ONE
    // ConfigurationChanged.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 1 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});     // id 1: P
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});    // id 2: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});   // id 3: X
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});   // id 4: B
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("P")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("X")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("B")});
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // A -> P (P.initialChildId = A)
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 2});  // X -> A (A.initialChildId = X)
        kernel.send(app::events::ReparentStateRequested{.id = 4, .parentId = 1});  // B -> P (A stays P's initial)
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 3, .exitActions = QStringList{QStringLiteral("exitX")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 2, .exitActions = QStringList{QStringLiteral("exitA")}});
        kernel.send(
            app::events::SetEntryActionsRequested{.id = 4, .entryActions = QStringList{QStringLiteral("enterB")}});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 5: X -Go-> B
        kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Go")});
        if (doc->machine().states.size() != 4 || doc->machine().transitions.size() != 1) {
            std::fprintf(stderr,
                          "FAIL: hierarchy-sim fixture 1 topology build did not produce 4 states / 1 transition\n");
            return 1;
        }
        removeEditCommands(kernel);

        int configOwner = 0;
        std::vector<app::events::ConfigurationChanged> configChanges;
        kernel.dispatcher().subscribe<app::events::ConfigurationChanged>(
            &configOwner,
            [&configChanges](const app::events::ConfigurationChanged& fact) { configChanges.push_back(fact); });

        sim->run();  // service entrance: mode() stays Design throughout (see this function's header comment)
        if (!configEquals(sim->configuration(), {1, 2, 3})) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 1 Run did not descend P -> A -> X\n");
            return 1;
        }
        if (sim->activeStateId() != 3) {
            std::fprintf(
                stderr, "FAIL: hierarchy-sim fixture 1 activeStateId() (flat-compat) did not read the lone atomic X\n");
            return 1;
        }
        configChanges.clear();

        sim->sendEvent(QStringLiteral("Go"));
        const int exitXIdx = traceIndexOf(sim->trace(), "action: exitX");
        const int exitAIdx = traceIndexOf(sim->trace(), "action: exitA");
        const int enterBIdx = traceIndexOf(sim->trace(), "action: enterB");
        if (exitXIdx < 0 || exitAIdx < 0 || enterBIdx < 0 || exitXIdx >= exitAIdx || exitAIdx >= enterBIdx) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 1 did not exit X then A before entering B\n");
            return 1;
        }
        if (!configEquals(sim->configuration(), {1, 4})) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 1 configuration() after Go was not {P,B}\n");
            return 1;
        }
        if (configChanges.size() != 1 || configChanges.back().activeIds != QVector<quint64>{1, 4}) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 1 did not publish exactly ONE ConfigurationChanged{P,B}\n");
            return 1;
        }
    }

    // ==== fixture 2: bubbling ====================================================
    // M{N{Y}}, initial M (descends M -> N -> Y). "Bubble" is handled only by
    // M (Y/N don't) -- proves the walk-up; "Local" is handled by BOTH Y and
    // M -- proves Y's own handler wins; "Panic" is handled only by root --
    // proves the root fallback fires once, only when nobody in the state
    // tree does.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 2 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: M
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: N
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: Y
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 4: Z
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 5: W
        kernel.send(app::events::AddStateRequested{.pos = QPointF(250, 0)});  // id 6: Z2
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // N -> M
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 2});  // Y -> N
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 4});  // id 7: M -Bubble-> Z
        kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Bubble")});
        kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 5});  // id 8: root -Bubble-> W (never wins)
        kernel.send(app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("Bubble")});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 6});  // id 9: Y -Local-> Z2
        kernel.send(app::events::SetTransitionEventRequested{.id = 9, .event = QStringLiteral("Local")});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 5});  // id 10: M -Local-> W (never wins)
        kernel.send(app::events::SetTransitionEventRequested{.id = 10, .event = QStringLiteral("Local")});
        kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 5});  // id 11: root -Panic-> W
        kernel.send(app::events::SetTransitionEventRequested{.id = 11, .event = QStringLiteral("Panic")});
        if (doc->machine().transitions.size() != 5) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 2 topology build did not produce 5 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        int firedOwner = 0;
        std::vector<app::events::TransitionFired> fired;
        std::vector<app::events::ActiveStateChanged> activeChanges;
        kernel.dispatcher().subscribe<app::events::TransitionFired>(
            &firedOwner, [&fired](const app::events::TransitionFired& fact) { fired.push_back(fact); });
        kernel.dispatcher().subscribe<app::events::ActiveStateChanged>(
            &firedOwner,
            [&activeChanges](const app::events::ActiveStateChanged& fact) { activeChanges.push_back(fact); });

        sim->run();
        if (!configEquals(sim->configuration(), {1, 2, 3})) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 2 Run did not descend M -> N -> Y\n");
            return 1;
        }

        fired.clear();
        sim->sendEvent(QStringLiteral("Bubble"));
        if (fired.empty() || fired.back().transitionId != 7 || !configEquals(sim->configuration(), {4}) ||
            activeChanges.empty() || activeChanges.back().fromId != 1) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 2 Bubble did not walk up to M's handler (id 7)\n");
            return 1;
        }

        sim->reset();
        fired.clear();
        sim->sendEvent(QStringLiteral("Local"));
        if (fired.empty() || fired.back().transitionId != 9 || !configEquals(sim->configuration(), {6})) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 2 Local did not let Y's own handler (id 9) win over M's\n");
            return 1;
        }

        sim->reset();
        fired.clear();
        activeChanges.clear();
        sim->sendEvent(QStringLiteral("Panic"));
        if (fired.empty() || fired.back().transitionId != 11 || !configEquals(sim->configuration(), {5}) ||
            activeChanges.empty() || activeChanges.back().fromId != 3) {
            std::fprintf(stderr,
                          "FAIL: hierarchy-sim fixture 2 Panic did not fall through to the root handler (id 11), "
                          "or fromId was not the exited atomic Y\n");
            return 1;
        }
    }

    // ==== fixture 3: parallel ====================================================
    // Q{R1{a,b},R2{c,d}} (Parallel), initial Q enters BOTH regions. "Go" has
    // a handler in each region -> TWO microsteps, ONE macrostep. "Wide" is
    // handled by Q and by c; Q's firing exits the whole parallel state, so
    // c's candidate is dropped (rule 5 phase 2).
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 3 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Q
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: R1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(60, 0)});   // id 3: a
        kernel.send(app::events::AddStateRequested{.pos = QPointF(70, 0)});   // id 4: b
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 5: R2
        kernel.send(app::events::AddStateRequested{.pos = QPointF(110, 0)});  // id 6: c
        kernel.send(app::events::AddStateRequested{.pos = QPointF(120, 0)});  // id 7: d
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 8: Ext
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // R1 -> Q
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 2});  // a -> R1
        kernel.send(app::events::ReparentStateRequested{.id = 4, .parentId = 2});  // b -> R1
        kernel.send(app::events::ReparentStateRequested{.id = 5, .parentId = 1});  // R2 -> Q
        kernel.send(app::events::ReparentStateRequested{.id = 6, .parentId = 5});  // c -> R2
        kernel.send(app::events::ReparentStateRequested{.id = 7, .parentId = 5});  // d -> R2
        kernel.send(app::events::SetStateKindRequested{.id = 1, .kind = app::StateKind::Parallel});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 9: a -Go-> b
        kernel.send(app::events::SetTransitionEventRequested{.id = 9, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 6, .to = 7});  // id 10: c -Go-> d
        kernel.send(app::events::SetTransitionEventRequested{.id = 10, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 8});  // id 11: Q -Wide-> Ext
        kernel.send(app::events::SetTransitionEventRequested{.id = 11, .event = QStringLiteral("Wide")});
        kernel.send(app::events::AddTransitionRequested{.from = 6, .to = 7});  // id 12: c -Wide-> d (dropped)
        kernel.send(app::events::SetTransitionEventRequested{.id = 12, .event = QStringLiteral("Wide")});
        if (doc->machine().transitions.size() != 4) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 3 topology build did not produce 4 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        int firedOwner = 0;
        std::vector<app::events::TransitionFired> fired;
        std::vector<app::events::ConfigurationChanged> configChanges;
        kernel.dispatcher().subscribe<app::events::TransitionFired>(
            &firedOwner, [&fired](const app::events::TransitionFired& fact) { fired.push_back(fact); });
        kernel.dispatcher().subscribe<app::events::ConfigurationChanged>(
            &firedOwner,
            [&configChanges](const app::events::ConfigurationChanged& fact) { configChanges.push_back(fact); });

        sim->run();
        if (!configEquals(sim->configuration(), {1, 2, 3, 5, 6}) || sim->activeStateId() != 0) {
            std::fprintf(stderr,
                          "FAIL: hierarchy-sim fixture 3 Run did not activate BOTH regions (or activeStateId() was "
                          "not the ambiguous-0 reading)\n");
            return 1;
        }

        fired.clear();
        configChanges.clear();
        sim->sendEvent(QStringLiteral("Go"));
        if (fired.size() != 2 || fired[0].transitionId != 9 || fired[1].transitionId != 10 ||
            configChanges.size() != 1 || !configEquals(sim->configuration(), {1, 2, 4, 5, 7})) {
            std::fprintf(stderr,
                          "FAIL: hierarchy-sim fixture 3 Go did not fire TWO microsteps (ids 9,10) in ONE macrostep\n");
            return 1;
        }

        sim->reset();
        fired.clear();
        sim->sendEvent(QStringLiteral("Wide"));
        if (fired.size() != 1 || fired.back().transitionId != 11 || !configEquals(sim->configuration(), {8})) {
            std::fprintf(stderr,
                          "FAIL: hierarchy-sim fixture 3 Wide did not drop c's redundant candidate (fired %d "
                          "transition(s), last id %llu)\n",
                          static_cast<int>(fired.size()),
                          fired.empty() ? 0ULL : static_cast<unsigned long long>(fired.back().transitionId));
            return 1;
        }
    }

    // ==== fixture 4: targetless + nested self ===================================
    // Outer{Inner{Leaf}}, initial Outer. Inner's targetless "Log" fires and
    // changes nothing; Inner's own "SelfEvt" self-transition re-exits/re-
    // enters Inner's whole subtree (Leaf included) and re-arms only Inner's
    // own 500ms timer -- Outer's independent 2000ms timer is untouched
    // throughout.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 4 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Outer
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: Inner
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: Leaf
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 4: Sibling
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 5: OuterTimeout
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // Inner -> Outer
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 2});  // Leaf -> Inner
        kernel.send(app::events::ReparentStateRequested{.id = 4, .parentId = 1});  // Sibling -> Outer
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(
            app::events::SetEntryActionsRequested{.id = 3, .entryActions = QStringList{QStringLiteral("enterLeaf")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 3, .exitActions = QStringList{QStringLiteral("exitLeaf")}});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = 2, .entryActions = QStringList{QStringLiteral("enterInner")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 2, .exitActions = QStringList{QStringLiteral("exitInner")}});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 5});  // id 6: Outer -(2000ms)-> OuterTimeout
        kernel.send(app::events::SetTransitionDelayRequested{.id = 6, .delayMs = 2000});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 2});  // id 7: Inner -SelfEvt-> Inner
        kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("SelfEvt")});
        kernel.send(app::events::SetTransitionReenterRequested{.id = 7, .reenter = true});
        kernel.send(app::events::SetTransitionActionRequested{.id = 7, .action = QStringLiteral("selfAction")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 0});  // id 8: Inner -Log-> (targetless)
        kernel.send(app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("Log")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 8, .action = QStringLiteral("logIt")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 4});  // id 9: Inner -(500ms)-> Sibling
        kernel.send(app::events::SetTransitionDelayRequested{.id = 9, .delayMs = 500});
        if (doc->machine().transitions.size() != 4) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 4 topology build did not produce 4 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        int activeOwner = 0;
        std::vector<app::events::ActiveStateChanged> activeChanges;
        kernel.dispatcher().subscribe<app::events::ActiveStateChanged>(
            &activeOwner,
            [&activeChanges](const app::events::ActiveStateChanged& fact) { activeChanges.push_back(fact); });

        sim->run();
        if (!configEquals(sim->configuration(), {1, 2, 3})) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 4 Run did not descend Outer -> Inner -> Leaf\n");
            return 1;
        }

        activeChanges.clear();
        sim->sendEvent(QStringLiteral("Log"));
        if (!sim->trace().contains(QStringLiteral("action: logIt")) || !configEquals(sim->configuration(), {1, 2, 3}) ||
            !activeChanges.empty()) {
            std::fprintf(stderr,
                          "FAIL: hierarchy-sim fixture 4 targetless Log disturbed the configuration or fired "
                          "ActiveStateChanged\n");
            return 1;
        }
        sim->tick(300);  // Inner's 500ms timer -> 200 remaining; Outer's 2000ms -> 1700 remaining; nothing due
        if (!configEquals(sim->configuration(), {1, 2, 3})) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 4 tick(300) fired something early\n");
            return 1;
        }

        sim->sendEvent(QStringLiteral("SelfEvt"));
        const int exitLeafIdx = traceIndexOf(sim->trace(), "action: exitLeaf");
        const int exitInnerIdx = traceIndexOf(sim->trace(), "action: exitInner");
        const int selfActionIdx = traceIndexOf(sim->trace(), "action: selfAction");
        const int enterInnerIdx = traceIndexOf(sim->trace(), "action: enterInner");
        const int enterLeafIdx = traceIndexOf(sim->trace(), "action: enterLeaf");
        if (exitLeafIdx < 0 || exitInnerIdx < 0 || selfActionIdx < 0 || enterInnerIdx < 0 || enterLeafIdx < 0 ||
            exitLeafIdx >= exitInnerIdx || exitInnerIdx >= selfActionIdx || selfActionIdx >= enterInnerIdx ||
            enterInnerIdx >= enterLeafIdx) {
            std::fprintf(stderr,
                          "FAIL: hierarchy-sim fixture 4 nested self did not exit Leaf,Inner then re-enter "
                          "Inner,Leaf around its own action\n");
            return 1;
        }
        if (!configEquals(sim->configuration(), {1, 2, 3}) || activeChanges.empty() ||
            activeChanges.back().fromId != 2 || activeChanges.back().toId != 2 ||
            activeChanges.back().viaTransitionId != 7) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 4 nested self did not publish ActiveStateChanged{2,2,7}\n");
            return 1;
        }

        sim->tick(200);  // Inner's RE-armed 500ms timer -> 300 remaining (would be due already if not re-armed)
        if (!configEquals(sim->configuration(), {1, 2, 3})) {
            std::fprintf(stderr,
                          "FAIL: hierarchy-sim fixture 4 tick(200) fired Inner's timer early -- it was not "
                          "re-armed to a fresh 500ms by the self-transition\n");
            return 1;
        }
        sim->tick(300);  // completes the RE-armed 500ms exactly; Outer's own 2000ms timer survives untouched
        if (!configEquals(sim->configuration(), {1, 4})) {
            std::fprintf(stderr,
                          "FAIL: hierarchy-sim fixture 4 Inner's re-armed timer did not fire into Sibling while "
                          "Outer stayed active\n");
            return 1;
        }
    }

    // ==== fixture 5: multi-owner timers ==========================================
    // Q{R1{a,aDone},R2{c,cDone}} (Parallel), initial Q. a's 300ms timer and
    // c's 700ms timer arm together on activation, owned independently, and
    // fire on their own schedules -- exiting R1 (a's region) never disarms
    // R2's (c's) still-counting timer.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 5 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Q
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: R1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(60, 0)});   // id 3: a
        kernel.send(app::events::AddStateRequested{.pos = QPointF(70, 0)});   // id 4: aDone
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 5: R2
        kernel.send(app::events::AddStateRequested{.pos = QPointF(110, 0)});  // id 6: c
        kernel.send(app::events::AddStateRequested{.pos = QPointF(120, 0)});  // id 7: cDone
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // R1 -> Q
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 2});  // a -> R1
        kernel.send(app::events::ReparentStateRequested{.id = 4, .parentId = 2});  // aDone -> R1
        kernel.send(app::events::ReparentStateRequested{.id = 5, .parentId = 1});  // R2 -> Q
        kernel.send(app::events::ReparentStateRequested{.id = 6, .parentId = 5});  // c -> R2
        kernel.send(app::events::ReparentStateRequested{.id = 7, .parentId = 5});  // cDone -> R2
        kernel.send(app::events::SetStateKindRequested{.id = 1, .kind = app::StateKind::Parallel});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 8: a -(300ms)-> aDone
        kernel.send(app::events::SetTransitionDelayRequested{.id = 8, .delayMs = 300});
        kernel.send(app::events::AddTransitionRequested{.from = 6, .to = 7});  // id 9: c -(700ms)-> cDone
        kernel.send(app::events::SetTransitionDelayRequested{.id = 9, .delayMs = 700});
        if (doc->machine().transitions.size() != 2) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 5 topology build did not produce 2 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();
        if (!configEquals(sim->configuration(), {1, 2, 3, 5, 6})) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 5 Run did not arm both regions\n");
            return 1;
        }

        sim->tick(300);  // a's timer fires; c's (owner-independent) drops to 400 remaining, untouched otherwise
        if (!configEquals(sim->configuration(), {1, 2, 4, 5, 6})) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 5 tick(300) did not fire a's timer alone\n");
            return 1;
        }
        sim->tick(400);  // exactly c's remaining time -- proves it kept counting independently of a's region
        if (!configEquals(sim->configuration(), {1, 2, 4, 5, 7})) {
            std::fprintf(stderr,
                          "FAIL: hierarchy-sim fixture 5 tick(400) did not fire c's timer on its own untouched "
                          "schedule\n");
            return 1;
        }
    }

    // ==== fixture 6: back through a parallel macrostep ==========================
    // Same Q{R1{a,b},R2{c,d}} shape as fixture 3, plus a second "Go2" wave
    // (b->a2, d->c2) so there are two macrosteps to choose between. One
    // Back undoes exactly the SECOND macrostep, landing configuration() and
    // trace() back on exactly what they were right after the first.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 6 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Q
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: R1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(60, 0)});   // id 3: a
        kernel.send(app::events::AddStateRequested{.pos = QPointF(70, 0)});   // id 4: b
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 5: R2
        kernel.send(app::events::AddStateRequested{.pos = QPointF(110, 0)});  // id 6: c
        kernel.send(app::events::AddStateRequested{.pos = QPointF(120, 0)});  // id 7: d
        kernel.send(app::events::AddStateRequested{.pos = QPointF(80, 50)});  // id 8: a2
        kernel.send(app::events::AddStateRequested{.pos = QPointF(130, 50)}); // id 9: c2
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // R1 -> Q
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 2});  // a -> R1
        kernel.send(app::events::ReparentStateRequested{.id = 4, .parentId = 2});  // b -> R1
        kernel.send(app::events::ReparentStateRequested{.id = 5, .parentId = 1});  // R2 -> Q
        kernel.send(app::events::ReparentStateRequested{.id = 6, .parentId = 5});  // c -> R2
        kernel.send(app::events::ReparentStateRequested{.id = 7, .parentId = 5});  // d -> R2
        kernel.send(app::events::ReparentStateRequested{.id = 8, .parentId = 2});  // a2 -> R1
        kernel.send(app::events::ReparentStateRequested{.id = 9, .parentId = 5});  // c2 -> R2
        kernel.send(app::events::SetStateKindRequested{.id = 1, .kind = app::StateKind::Parallel});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 10: a -Go-> b
        kernel.send(app::events::SetTransitionEventRequested{.id = 10, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 6, .to = 7});  // id 11: c -Go-> d
        kernel.send(app::events::SetTransitionEventRequested{.id = 11, .event = QStringLiteral("Go")});
        kernel.send(app::events::AddTransitionRequested{.from = 4, .to = 8});  // id 12: b -Go2-> a2
        kernel.send(app::events::SetTransitionEventRequested{.id = 12, .event = QStringLiteral("Go2")});
        kernel.send(app::events::AddTransitionRequested{.from = 7, .to = 9});  // id 13: d -Go2-> c2
        kernel.send(app::events::SetTransitionEventRequested{.id = 13, .event = QStringLiteral("Go2")});
        if (doc->machine().transitions.size() != 4) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 6 topology build did not produce 4 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Go"));  // macrostep 1
        if (!configEquals(sim->configuration(), {1, 2, 4, 5, 7})) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 6 Go (macrostep 1) did not land on {Q,R1,b,R2,d}\n");
            return 1;
        }
        const std::vector<quint64> configAfterMacrostep1(sim->configuration().begin(), sim->configuration().end());
        const QStringList traceAfterMacrostep1 = sim->trace();

        sim->sendEvent(QStringLiteral("Go2"));  // macrostep 2
        if (!configEquals(sim->configuration(), {1, 2, 5, 8, 9})) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 6 Go2 (macrostep 2) did not land on {Q,R1,R2,a2,c2}\n");
            return 1;
        }

        sim->back();  // undoes exactly macrostep 2
        if (sim->configuration() != configAfterMacrostep1) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 6 Back did not restore the post-macrostep-1 configuration\n");
            return 1;
        }
        if (sim->trace() != traceAfterMacrostep1) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 6 Back did not restore the post-macrostep-1 trace exactly\n");
            return 1;
        }
    }

    // ==== fixture 7: exit order under a doc-order-inverted tree =================
    // Child created BEFORE its parent, then dragged in (the normal reparent
    // flow), so the child sits at a LOWER vector index than the parent. Rule
    // 3's deepest-first must exit child-then-parent by REAL depth -- a plain
    // reverse-document-order exit would run the parent's exitActions first.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 7 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: C (the future CHILD, index 0)
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: P (the future PARENT, index 1)
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: Out
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("C")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("P")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Out")});
        kernel.send(app::events::ReparentStateRequested{.id = 1, .parentId = 2});  // C -> P: child index < parent index
        kernel.send(app::events::SetInitialStateRequested{.id = 2});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 1, .exitActions = QStringList{QStringLiteral("exitC")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 2, .exitActions = QStringList{QStringLiteral("exitP")}});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 3});  // id 4: C -Leave-> Out
        kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("Leave")});
        removeEditCommands(kernel);

        sim->run();  // service entrance (see the function header comment)
        if (!configEquals(sim->configuration(), {1, 2})) {  // doc order: C(index 0) before P(index 1)
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 7 Run did not activate the inverted P{C} tree\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Leave"));
        if (!configEquals(sim->configuration(), {3})) {
            std::fprintf(stderr, "FAIL: hierarchy-sim fixture 7 Leave did not land on Out\n");
            return 1;
        }
        const int exitCAt = traceIndexOf(sim->trace(), "action: exitC");
        const int exitPAt = traceIndexOf(sim->trace(), "action: exitP");
        if (exitCAt < 0 || exitPAt < 0 || exitCAt > exitPAt) {
            std::fprintf(stderr,
                          "FAIL: hierarchy-sim fixture 7 exit order was not child-then-parent by REAL depth "
                          "(exitC at %d, exitP at %d)\n",
                          exitCAt, exitPAt);
            return 1;
        }
    }

    std::printf(
        "PASS: state-designer hierarchy sim smoke (nested entry/exit + bubbling + parallel macrostep + "
        "targetless/nested-self + multi-owner timers + parallel back + inverted-doc-order exit)\n");
    return 0;
}

// History (shallow/deep) semantics: fireMicrostep()'s redirect off a
// History-kind target, historyRecords_ (compoundId -> last active direct
// child), and the model/IO/validator support around them. Same setup as
// runHierarchySimSmoke(): topology built via registerEditCommands, agent
// driven directly. historyDeep has no edit command, so fixture 2 builds via
// commands, copies doc->machine(), mutates the copy and doc->restore()s it.
int runHistorySimSmoke() {
    const auto traceIndexOf = [](const QStringList& trace, const char* needle) -> int {
        for (int i = 0; i < trace.size(); ++i) {
            if (trace[i].contains(QLatin1String(needle))) {
                return i;
            }
        }
        return -1;
    };
    const auto configEquals = [](const std::vector<quint64>& actual, std::initializer_list<quint64> expected) {
        return actual == std::vector<quint64>(expected);
    };

    // ==== fixture 1: shallow restore =============================================
    // P{A,B} (initial A) + H(shallow, parent P) + Out. Run -> A; move to B;
    // leave P entirely to Out (B's exit records historyRecords_[P] = B);
    // re-enter via ->H lands B (the record), never P's initialChildId (A).
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: history-sim fixture 1 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: P
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: B
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 4: Out
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 50)});  // id 5: H
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("P")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("B")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Out")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("H")});
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // A -> P (P.initialChildId = A)
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 1});  // B -> P
        kernel.send(app::events::SetStateKindRequested{.id = 5, .kind = app::StateKind::History});
        kernel.send(app::events::ReparentStateRequested{.id = 5, .parentId = 1});  // H -> P (shallow: historyDeep
                                                                                    // stays its false default)
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: A -Next-> B
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Next")});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 7: B -Leave-> Out
        kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Leave")});
        kernel.send(app::events::AddTransitionRequested{.from = 4, .to = 5});  // id 8: Out -Restore-> H
        kernel.send(app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("Restore")});
        if (doc->machine().states.size() != 5 || doc->machine().transitions.size() != 3) {
            std::fprintf(stderr,
                          "FAIL: history-sim fixture 1 topology build did not produce 5 states / 3 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();  // service entrance (see runHierarchySimSmoke()'s header comment for the same pattern)
        if (!configEquals(sim->configuration(), {1, 2})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 1 Run did not descend P -> A\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Next"));
        if (!configEquals(sim->configuration(), {1, 3})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 1 Next did not move A -> B\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Leave"));
        if (!configEquals(sim->configuration(), {4})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 1 Leave did not land on Out\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Restore"));
        if (!configEquals(sim->configuration(), {1, 3})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 1 shallow ->H did not restore {P,B} from the record\n");
            return 1;
        }
        const int headlineAt = traceIndexOf(sim->trace(), "Restore");
        const int historyAt = traceIndexOf(sim->trace(), "history: P -> B");
        if (headlineAt < 0 || historyAt < 0 || headlineAt >= historyAt) {
            std::fprintf(stderr,
                          "FAIL: history-sim fixture 1 trace did not carry the headline then the history redirect "
                          "line naming P -> B\n");
            return 1;
        }
    }

    // ==== fixture 2: deep vs shallow ==============================================
    // P{A{X,Y}} initial A/X, two History children of P: Hd(deep), Hs(shallow,
    // the default). Drive X -> Y, leave P (records P=A, A=Y); ->Hd re-enters
    // {P,A,Y}. Leave again; ->Hs re-enters {P,A,X}: P's record restores A, but
    // A's descent falls back to initialChildId X (shallow spends after one hop).
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: history-sim fixture 2 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});     // id 1: P
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});    // id 2: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});   // id 3: X
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});   // id 4: Y
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});   // id 5: Out
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 50)});   // id 6: Hd
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 100)});  // id 7: Hs
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("P")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("X")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Y")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("Out")});
        kernel.send(app::events::RenameStateRequested{.id = 6, .name = QStringLiteral("Hd")});
        kernel.send(app::events::RenameStateRequested{.id = 7, .name = QStringLiteral("Hs")});
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 2});  // X -> A (A.initialChildId = X)
        kernel.send(app::events::ReparentStateRequested{.id = 4, .parentId = 2});  // Y -> A
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // A -> P (P.initialChildId = A)
        kernel.send(app::events::SetStateKindRequested{.id = 6, .kind = app::StateKind::History});
        kernel.send(app::events::SetStateKindRequested{.id = 7, .kind = app::StateKind::History});
        kernel.send(app::events::ReparentStateRequested{.id = 6, .parentId = 1});  // Hd -> P
        kernel.send(app::events::ReparentStateRequested{.id = 7, .parentId = 1});  // Hs -> P
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 4});  // id 8: X -Next-> Y
        kernel.send(app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("Next")});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 5});  // id 9: P -Leave-> Out (bubble handler)
        kernel.send(app::events::SetTransitionEventRequested{.id = 9, .event = QStringLiteral("Leave")});
        kernel.send(app::events::AddTransitionRequested{.from = 5, .to = 6});  // id 10: Out -RestoreDeep-> Hd
        kernel.send(app::events::SetTransitionEventRequested{.id = 10, .event = QStringLiteral("RestoreDeep")});
        kernel.send(app::events::AddTransitionRequested{.from = 5, .to = 7});  // id 11: Out -RestoreShallow-> Hs
        kernel.send(app::events::SetTransitionEventRequested{.id = 11, .event = QStringLiteral("RestoreShallow")});
        if (doc->machine().states.size() != 7 || doc->machine().transitions.size() != 4) {
            std::fprintf(stderr,
                          "FAIL: history-sim fixture 2 topology build did not produce 7 states / 4 transitions\n");
            return 1;
        }
        // Hd.historyDeep has no edit command; set via the restore() path.
        app::Machine machine = doc->machine();
        for (app::State& state : machine.states) {
            if (state.id == 6) {
                state.historyDeep = true;
            }
        }
        doc->restore(machine);
        removeEditCommands(kernel);

        sim->run();
        if (!configEquals(sim->configuration(), {1, 2, 3})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 2 Run did not descend P -> A -> X\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Next"));
        if (!configEquals(sim->configuration(), {1, 2, 4})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 2 Next did not move X -> Y\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Leave"));
        if (!configEquals(sim->configuration(), {5})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 2 Leave did not bubble to P's handler and land on Out\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("RestoreDeep"));
        if (!configEquals(sim->configuration(), {1, 2, 4})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 2 deep ->Hd did not restore {P,A,Y}\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "history: P -> A") < 0) {
            std::fprintf(stderr, "FAIL: history-sim fixture 2 deep redirect did not trace 'history: P -> A'\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Leave"));
        if (!configEquals(sim->configuration(), {5})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 2 second Leave did not return to Out\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("RestoreShallow"));
        if (!configEquals(sim->configuration(), {1, 2, 3})) {
            std::fprintf(stderr,
                          "FAIL: history-sim fixture 2 shallow ->Hs did not restore {P,A,X} (record at P, plain "
                          "initialChildId below A)\n");
            return 1;
        }
    }

    // ==== fixture 3: no-record fallback ===========================================
    // Machine-level initial is Out (a sibling of P), so P's own children
    // have NEVER been entered when ->H fires -- historyRecords_ holds no
    // entry for P yet. The redirect must fall back to P's initialChildId
    // (A), and the trace line still names that fallback.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: history-sim fixture 3 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Out
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: P
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 4: B
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 50)});  // id 5: H
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Out")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("P")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("B")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("H")});
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 2});  // A -> P (P.initialChildId = A)
        kernel.send(app::events::ReparentStateRequested{.id = 4, .parentId = 2});  // B -> P
        kernel.send(app::events::SetStateKindRequested{.id = 5, .kind = app::StateKind::History});
        kernel.send(app::events::ReparentStateRequested{.id = 5, .parentId = 2});  // H -> P
        kernel.send(app::events::SetInitialStateRequested{.id = 1});               // machine initial = Out, not P
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 5});      // id 6: Out -Restore-> H
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Restore")});
        if (doc->machine().states.size() != 5 || doc->machine().transitions.size() != 1) {
            std::fprintf(stderr,
                          "FAIL: history-sim fixture 3 topology build did not produce 5 states / 1 transition\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();
        if (!configEquals(sim->configuration(), {1})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 3 Run did not land on Out\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Restore"));
        if (!configEquals(sim->configuration(), {2, 3})) {
            std::fprintf(stderr,
                          "FAIL: history-sim fixture 3 no-record ->H did not fall back to P's initialChildId "
                          "descent {P,A}\n");
            return 1;
        }
        if (traceIndexOf(sim->trace(), "history: P -> A") < 0) {
            std::fprintf(stderr,
                          "FAIL: history-sim fixture 3 trace did not name the initialChildId fallback (P -> A)\n");
            return 1;
        }
    }

    // ==== fixture 4: latest-wins ===================================================
    // Bounce A -> B -> A -> B inside P, then leave -- historyRecords_[P]
    // must hold whatever was active at leave-time (B), not the FIRST state
    // ever recorded there (A). H is shallow (the default).
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: history-sim fixture 4 agents did not all register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: P
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});   // id 2: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 3: B
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 4: Out
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 50)});  // id 5: H
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("P")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("B")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Out")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("H")});
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // A -> P (P.initialChildId = A)
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 1});  // B -> P
        kernel.send(app::events::SetStateKindRequested{.id = 5, .kind = app::StateKind::History});
        kernel.send(app::events::ReparentStateRequested{.id = 5, .parentId = 1});  // H -> P
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: A -Next-> B
        kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Next")});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 2});  // id 7: B -Back-> A
        kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Back")});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 4});  // id 8: P -Leave-> Out (bubble)
        kernel.send(app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("Leave")});
        kernel.send(app::events::AddTransitionRequested{.from = 4, .to = 5});  // id 9: Out -Restore-> H
        kernel.send(app::events::SetTransitionEventRequested{.id = 9, .event = QStringLiteral("Restore")});
        if (doc->machine().states.size() != 5 || doc->machine().transitions.size() != 4) {
            std::fprintf(stderr,
                          "FAIL: history-sim fixture 4 topology build did not produce 5 states / 4 transitions\n");
            return 1;
        }
        removeEditCommands(kernel);

        sim->run();
        sim->sendEvent(QStringLiteral("Next"));  // A -> B
        sim->sendEvent(QStringLiteral("Back"));   // B -> A
        sim->sendEvent(QStringLiteral("Next"));  // A -> B
        if (!configEquals(sim->configuration(), {1, 3})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 4 bounce A->B->A->B did not end on B\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Leave"));
        if (!configEquals(sim->configuration(), {4})) {
            std::fprintf(stderr, "FAIL: history-sim fixture 4 Leave did not land on Out\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Restore"));
        if (!configEquals(sim->configuration(), {1, 3})) {
            std::fprintf(stderr,
                          "FAIL: history-sim fixture 4 shallow ->H did not restore B -- the LAST record, not the "
                          "first (A)\n");
            return 1;
        }
    }

    // ==== fixture 5: IO (osd v6 + legacy load + xstate round trip) ================
    {
        // ---- .sdm v6 round trip: byte-stable save, historyDeep preserved ----------
        app::Machine historyMachine;
        historyMachine.name = QStringLiteral("HistoryIO");
        historyMachine.states.push_back(app::State{
            .id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Normal, .initialChildId = 2});
        historyMachine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 1});
        historyMachine.states.push_back(app::State{.id = 3,
                                                    .name = QStringLiteral("H"),
                                                    .kind = app::StateKind::History,
                                                    .parentId = 1,
                                                    .historyDeep = true});
        historyMachine.nextId = 4;
        historyMachine.initialStateId = 1;

        const QString path1 = QDir::temp().filePath(QStringLiteral("ordo-state-designer-history-smoke-1.sdm"));
        const QString path2 = QDir::temp().filePath(QStringLiteral("ordo-state-designer-history-smoke-2.sdm"));
        QFile::remove(path1);
        QFile::remove(path2);
        QString ioError;
        if (!app::saveMachine(historyMachine, path1, &ioError) || !app::saveMachine(historyMachine, path2, &ioError)) {
            std::fprintf(stderr, "FAIL: history-sim IO saveMachine failed: %s\n", qUtf8Printable(ioError));
            return 1;
        }
        QFile file1(path1);
        QFile file2(path2);
        if (!file1.open(QIODevice::ReadOnly) || !file2.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "FAIL: could not reopen the two history .sdm saves to compare bytes\n");
            return 1;
        }
        const QByteArray bytes1 = file1.readAll();
        const QByteArray bytes2 = file2.readAll();
        if (bytes1.isEmpty() || bytes1 != bytes2) {
            std::fprintf(stderr,
                          "FAIL: saving the same historyDeep Machine twice did not produce byte-identical .sdm "
                          "files\n");
            return 1;
        }
        app::Machine loadedHistory;
        if (!app::loadMachine(path1, &loadedHistory, &ioError)) {
            std::fprintf(stderr, "FAIL: loadMachine failed for the history round trip: %s\n",
                         qUtf8Printable(ioError));
            return 1;
        }
        if (!(loadedHistory == historyMachine)) {
            std::fprintf(stderr, "FAIL: historyDeep did not round-trip through the v6 .sdm save/load\n");
            return 1;
        }
    }
    {
        // ---- v5-style file (no "historyDeep" key) loads false ---------------------
        QJsonObject v5;
        v5[QStringLiteral("formatVersion")] = 5;
        v5[QStringLiteral("name")] = QStringLiteral("PreHistory");
        v5[QStringLiteral("nextId")] = 3;
        v5[QStringLiteral("initialStateId")] = 1;
        QJsonArray states;
        QJsonObject pObject;
        pObject[QStringLiteral("id")] = 1;
        pObject[QStringLiteral("name")] = QStringLiteral("P");
        pObject[QStringLiteral("kind")] = QStringLiteral("Normal");
        pObject[QStringLiteral("initialChildId")] = 2;
        states.append(pObject);
        QJsonObject hObject;
        hObject[QStringLiteral("id")] = 2;
        hObject[QStringLiteral("name")] = QStringLiteral("H");
        hObject[QStringLiteral("kind")] = QStringLiteral("History");
        hObject[QStringLiteral("parentId")] = 1;
        // No "historyDeep" key at all -- the v5-style file this fixture
        // stands in for.
        states.append(hObject);
        v5[QStringLiteral("states")] = states;
        v5[QStringLiteral("notes")] = QJsonArray();
        const app::Machine legacy = app::machineFromJson(v5);
        if (legacy.states.size() != 2 || legacy.states[1].historyDeep != false) {
            std::fprintf(stderr, "FAIL: a v5-style file with no historyDeep key did not load false\n");
            return 1;
        }
    }
    {
        // ---- XState export -> import -> export idempotent, shallow AND deep -------
        app::Machine historyXstate;
        historyXstate.name = QStringLiteral("HistoryXState");
        historyXstate.states.push_back(app::State{
            .id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Normal, .initialChildId = 2});
        historyXstate.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 1});
        historyXstate.states.push_back(
            app::State{.id = 3, .name = QStringLiteral("B"), .kind = app::StateKind::Normal, .parentId = 1});
        historyXstate.states.push_back(app::State{.id = 4,
                                                   .name = QStringLiteral("Hd"),
                                                   .kind = app::StateKind::History,
                                                   .parentId = 1,
                                                   .historyDeep = true});
        historyXstate.states.push_back(
            app::State{.id = 5, .name = QStringLiteral("Hs"), .kind = app::StateKind::History, .parentId = 1});
        historyXstate.nextId = 6;
        historyXstate.initialStateId = 1;

        const app::XStateExportResult exportedH = app::machineToXStateJson(historyXstate);
        if (!exportedH.ok) {
            std::fprintf(stderr, "FAIL: history xstate export refused a well-formed machine: %s\n",
                         qUtf8Printable(exportedH.error));
            return 1;
        }
        const QJsonObject pStates = exportedH.json.value(QStringLiteral("states"))
                                        .toObject()
                                        .value(QStringLiteral("P"))
                                        .toObject()
                                        .value(QStringLiteral("states"))
                                        .toObject();
        const QJsonObject hdJson = pStates.value(QStringLiteral("Hd")).toObject();
        const QJsonObject hsJson = pStates.value(QStringLiteral("Hs")).toObject();
        if (hdJson.value(QStringLiteral("history")).toString() != QStringLiteral("deep") ||
            hsJson.contains(QStringLiteral("history"))) {
            std::fprintf(
                stderr,
                "FAIL: history xstate export did not emit history:\"deep\" for Hd and omit it for shallow Hs\n");
            return 1;
        }

        const app::XStateImportResult reimportedH = app::machineFromXStateJson(exportedH.json);
        if (!reimportedH.ok || !reimportedH.diagnostics.isEmpty()) {
            std::fprintf(stderr, "FAIL: reimporting the history export was not clean (ok=%d, %d diagnostic(s))\n",
                         reimportedH.ok ? 1 : 0, static_cast<int>(reimportedH.diagnostics.size()));
            return 1;
        }
        QHash<QString, const app::State*> byNameH;
        for (const app::State& state : reimportedH.machine.states) {
            byNameH.insert(state.name, &state);
        }
        if (!byNameH.value(QStringLiteral("Hd"))->historyDeep || byNameH.value(QStringLiteral("Hs"))->historyDeep) {
            std::fprintf(stderr, "FAIL: reimported history states did not preserve historyDeep (Hd true, Hs false)\n");
            return 1;
        }

        const app::XStateExportResult reexportedH = app::machineToXStateJson(reimportedH.machine);
        if (!reexportedH.ok || reexportedH.json != exportedH.json) {
            std::fprintf(stderr, "FAIL: history export is not idempotent -- toJson(fromJson(toJson(M))) != toJson(M)\n");
            return 1;
        }
    }

    // ==== fixture 6: validator (check 8) ==========================================
    {
        // History with its own outgoing transition -> the new Warning fires.
        app::Machine machine;
        machine.name = QStringLiteral("HistoryWarn");
        machine.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Normal, .initialChildId = 2});
        machine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 1});
        machine.states.push_back(
            app::State{.id = 3, .name = QStringLiteral("H"), .kind = app::StateKind::History, .parentId = 1});
        machine.transitions.push_back(
            app::Transition{.id = 4, .from = 3, .to = 2, .event = QStringLiteral("Bad")});
        machine.nextId = 5;
        machine.initialStateId = 1;

        bool found = false;
        for (const app::Problem& problem : app::validate(machine)) {
            found = found || (problem.severity == app::ProblemSeverity::Warning && problem.stateId == 3 &&
                               problem.transitionId == 4 &&
                               problem.text.contains(QStringLiteral("outgoing transition")));
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not warn on a History state with its own outgoing transition\n");
            return 1;
        }
    }
    {
        // Same topology, minus the History's own outgoing transition -- no
        // new problem from check 8.
        app::Machine machine;
        machine.name = QStringLiteral("HistoryClean");
        machine.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Normal, .initialChildId = 2});
        machine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 1});
        machine.states.push_back(
            app::State{.id = 3, .name = QStringLiteral("H"), .kind = app::StateKind::History, .parentId = 1});
        machine.nextId = 4;
        machine.initialStateId = 1;

        for (const app::Problem& problem : app::validate(machine)) {
            if (problem.stateId == 3 && problem.text.contains(QStringLiteral("outgoing transition"))) {
                std::fprintf(stderr,
                              "FAIL: validate() wrongly warned a History state with NO outgoing transitions\n");
                return 1;
            }
        }
    }

    std::printf(
        "PASS: state-designer history sim smoke (shallow restore + deep vs shallow + no-record fallback + "
        "latest-wins + v6 IO round trip + validator warning)\n");
    return 0;
}

