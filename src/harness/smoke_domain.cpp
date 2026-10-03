// --smoke phases: domain + undo journal + hierarchy data model, plus runSmoke(),
// which calls every other --smoke phase in order and owns phase 1's inline
// domain-edit + project round-trip body.

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
#include "infra/recent_projects.h"
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

// Phase 2: undo/redo journal drive on its own kernel, so "the initial machine"
// is a fresh empty Machine.
int runUndoSmoke() {
    ordo::core::Kernel kernel;
    kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
    kernel.registerAgent(std::make_shared<app::SimulationAgent>());
    kernel.registerAgent(std::make_shared<app::UndoStore>());
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    auto undo = kernel.agentAs<app::UndoStore>(app::UndoStore::kName);
    if (!doc || !sim || !undo) {
        std::fprintf(stderr, "FAIL: undo-phase agents did not all register under kName\n");
        return 1;
    }

    // Manual mode: only SetModeRequested's policy gate is exercised, but
    // registerSimCommands() needs a SimClock.
    app::SimClock clock;
    clock.setManualMode(true);
    registerUndoPhaseCommands(kernel);
    registerSimCommands(kernel, clock);

    // Observes UndoStateChanged via a dispatcher subscription owned by a local's address.
    int undoStateOwner = 0;
    std::vector<app::events::UndoStateChanged> undoStateEvents;
    kernel.dispatcher().subscribe<app::events::UndoStateChanged>(
        &undoStateOwner,
        [&undoStateEvents](const app::events::UndoStateChanged& fact) { undoStateEvents.push_back(fact); });

    // ---- scripted edit sequence: 9 ops, including a delete-state cascade ----
    // Deletes record the erased vector index and restores re-insert there, so
    // undo/redo reproduces the exact vector order. Only nextId never round-trips
    // (see sameTopology).
    std::vector<app::Machine> checkpoints;
    checkpoints.push_back(doc->machine());  // checkpoint[0]: the initial machine, before any undo-tracked edit

    kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // op1: id 1
    checkpoints.push_back(doc->machine());
    kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // op2: id 2
    checkpoints.push_back(doc->machine());
    kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // op3: id 3
    checkpoints.push_back(doc->machine());
    kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Working")});  // op4
    checkpoints.push_back(doc->machine());
    kernel.send(app::events::SetInitialStateRequested{.id = 1});  // op5
    checkpoints.push_back(doc->machine());
    kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // op6: id 4
    checkpoints.push_back(doc->machine());
    kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // op7: id 5
    checkpoints.push_back(doc->machine());
    kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("go")});  // op8
    checkpoints.push_back(doc->machine());
    kernel.send(app::events::DeleteStateRequested{.id = 3});  // op9: cascade removes transition 5
    checkpoints.push_back(doc->machine());

    const int opCount = static_cast<int>(checkpoints.size()) - 1;  // 9
    if (opCount < 8) {
        std::fprintf(stderr, "FAIL: undo-phase scripted sequence has only %d ops (want >= 8)\n", opCount);
        return 1;
    }
    if (doc->findState(3) != nullptr || doc->findTransition(5) != nullptr) {
        std::fprintf(stderr,
                     "FAIL: undo-phase op9's DeleteStateRequested did not cascade-remove state 3/transition 5\n");
        return 1;
    }

    // Exactly one UndoStateChanged over the edit sequence: the first push flips
    // canUndo false->true; later pushes change neither flag.
    if (undoStateEvents.size() != 1 || !undoStateEvents[0].canUndo || undoStateEvents[0].canRedo) {
        std::fprintf(stderr,
                     "FAIL: expected exactly 1 UndoStateChanged{canUndo=true,canRedo=false} during the edit "
                     "sequence, got %d event(s)\n",
                     static_cast<int>(undoStateEvents.size()));
        return 1;
    }
    undoStateEvents.clear();

    // ---- undo step-by-step down to the initial machine ----------------------
    for (int i = opCount; i >= 1; --i) {
        kernel.send(app::events::UndoRequested{});
        if (!sameTopology(doc->machine(), checkpoints[i - 1])) {
            std::fprintf(stderr, "FAIL: undo #%d did not reproduce checkpoint[%d]'s topology\n", opCount - i + 1,
                         i - 1);
            return 1;
        }
    }
    if (undo->canUndo()) {
        std::fprintf(stderr, "FAIL: canUndo() is still true after undoing all %d ops\n", opCount);
        return 1;
    }
    // Exactly 2 flips during undo: the first flips canRedo false->true, the
    // last flips canUndo true->false.
    if (undoStateEvents.size() != 2 || !undoStateEvents[0].canUndo || !undoStateEvents[0].canRedo ||
        undoStateEvents[1].canUndo || !undoStateEvents[1].canRedo) {
        std::fprintf(stderr, "FAIL: undo-phase UndoStateChanged sequence during undo x%d was not [(T,T),(F,T)]\n",
                     opCount);
        return 1;
    }
    undoStateEvents.clear();

    // ---- redo back to the top -------------------------------------------------
    for (int i = 1; i <= opCount; ++i) {
        kernel.send(app::events::RedoRequested{});
        if (!sameTopology(doc->machine(), checkpoints[i])) {
            std::fprintf(stderr, "FAIL: redo #%d did not reproduce checkpoint[%d]'s topology\n", i, i);
            return 1;
        }
    }
    if (undo->canRedo()) {
        std::fprintf(stderr, "FAIL: canRedo() is still true after redoing all %d ops\n", opCount);
        return 1;
    }
    // Deep equality (nextId included) is valid here: nextId has not moved since
    // checkpoints[9] was captured.
    if (!(doc->machine() == checkpoints.back())) {
        std::fprintf(stderr, "FAIL: after redoing every op, the machine does not deep-equal the final checkpoint\n");
        return 1;
    }
    // Exactly 2 flips during redo: the first flips canUndo false->true, the
    // last flips canRedo true->false.
    if (undoStateEvents.size() != 2 || !undoStateEvents[0].canUndo || !undoStateEvents[0].canRedo ||
        !undoStateEvents[1].canUndo || undoStateEvents[1].canRedo) {
        std::fprintf(stderr, "FAIL: undo-phase UndoStateChanged sequence during redo x%d was not [(T,T),(T,F)]\n",
                     opCount);
        return 1;
    }
    undoStateEvents.clear();

    // ---- undo x2, then a fresh edit clears redo --------------------------------
    kernel.send(app::events::UndoRequested{});
    kernel.send(app::events::UndoRequested{});
    if (!undo->canRedo()) {
        std::fprintf(stderr, "FAIL: canRedo() is false after undoing x2 from the top (expected true)\n");
        return 1;
    }
    kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});  // a fresh edit, not a redo
    if (undo->canRedo()) {
        std::fprintf(stderr, "FAIL: a fresh edit after undo x2 left canRedo() true (push must clear redo)\n");
        return 1;
    }
    if (!undo->canUndo()) {
        std::fprintf(stderr, "FAIL: canUndo() is false right after committing a fresh edit\n");
        return 1;
    }
    // One flip on the undo x2 (canRedo false->true), one on the fresh edit
    // (canRedo true->false).
    if (undoStateEvents.size() != 2 || !undoStateEvents[0].canUndo || !undoStateEvents[0].canRedo ||
        !undoStateEvents[1].canUndo || undoStateEvents[1].canRedo) {
        std::fprintf(stderr,
                     "FAIL: undo-phase UndoStateChanged sequence during undo x2 + fresh edit was not "
                     "[(T,T),(T,F)]\n");
        return 1;
    }
    undoStateEvents.clear();

    // ---- metadata vocabulary: set + single-step undo round trips ---------------
    kernel.send(app::events::SetDescriptionRequested{.id = 1, .description = QStringLiteral("waits for input")});
    kernel.send(app::events::SetTagsRequested{.id = 1, .tags = QStringList{QStringLiteral("ui")}});
    kernel.send(app::events::SetExitActionsRequested{.id = 1, .exitActions = QStringList{QStringLiteral("bye()")}});
    kernel.send(app::events::SetMachineNameRequested{.name = QStringLiteral("Meta")});
    if (doc->findState(1)->description != QStringLiteral("waits for input") ||
        doc->findState(1)->tags != QStringList{QStringLiteral("ui")} ||
        doc->findState(1)->exitActions != QStringList{QStringLiteral("bye()")} ||
        doc->machine().name != QStringLiteral("Meta")) {
        std::fprintf(stderr, "FAIL: task-11.1 metadata setters did not apply\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});  // machine name
    kernel.send(app::events::UndoRequested{});  // exit actions
    kernel.send(app::events::UndoRequested{});  // tags
    kernel.send(app::events::UndoRequested{});  // description
    if (!doc->findState(1)->description.isEmpty() || !doc->findState(1)->tags.isEmpty() ||
        !doc->findState(1)->exitActions.isEmpty() || doc->machine().name != QString()) {
        std::fprintf(stderr, "FAIL: task-11.1 metadata undo x4 did not restore the pre-edit values\n");
        return 1;
    }
    undoStateEvents.clear();

    // ---- initialStateId: move + policy + delete-clears + undo ------------------
    // op5 left initialStateId == 1; the undo x2 above only took back op9/op8.
    kernel.send(app::events::SetInitialStateRequested{.id = 2});
    if (doc->machine().initialStateId != 2) {
        std::fprintf(stderr, "FAIL: SetInitialStateRequested{2} did not move initialStateId\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    if (doc->machine().initialStateId != 1) {
        std::fprintf(stderr, "FAIL: undo did not move initialStateId back to 1\n");
        return 1;
    }
    kernel.send(app::events::SetInitialStateRequested{.id = 999});  // no such state -- policy-rejected
    if (doc->machine().initialStateId != 1) {
        std::fprintf(stderr, "FAIL: SetInitialStateRequested{999} was not rejected (dangling id written)\n");
        return 1;
    }
    // Pill offset: set + single-step undo round trip.
    kernel.send(app::events::MoveTransitionLabelRequested{.id = 4, .offset = QPointF(18.0, -12.0)});
    if (doc->findTransition(4)->labelOffset != QPointF(18.0, -12.0)) {
        std::fprintf(stderr, "FAIL: MoveTransitionLabelRequested did not set the label offset\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    if (doc->findTransition(4)->labelOffset != QPointF()) {
        std::fprintf(stderr, "FAIL: undo did not clear the label offset\n");
        return 1;
    }

    // labelRatio: set / unchanged-is-a-no-op / journal-replay round trip on
    // transition 4 (a plain Normal transition, 1 -> 2). Drives
    // MachineDocAgent::setTransitionLabelRatio directly and replays a
    // hand-built TransactionDelta through applyDelta; neither pushes onto
    // UndoStore since no Transaction wraps them.
    int labelRatioOwner = 0;
    std::vector<app::events::TransitionLabelRatioChanged> labelRatioEvents;
    kernel.dispatcher().subscribe<app::events::TransitionLabelRatioChanged>(
        &labelRatioOwner, [&labelRatioEvents](const app::events::TransitionLabelRatioChanged& fact) {
            labelRatioEvents.push_back(fact);
        });

    doc->setTransitionLabelRatio(4, 0.3);
    if (!doc->findTransition(4)->labelRatio.has_value() || *doc->findTransition(4)->labelRatio != 0.3) {
        std::fprintf(stderr, "FAIL: setTransitionLabelRatio(4, 0.3) did not set the ratio\n");
        return 1;
    }
    if (labelRatioEvents.size() != 1 || labelRatioEvents[0].id != 4 || !labelRatioEvents[0].ratio.has_value() ||
        *labelRatioEvents[0].ratio != 0.3) {
        std::fprintf(stderr,
                     "FAIL: setTransitionLabelRatio(4, 0.3) did not publish TransitionLabelRatioChanged{4, 0.3}\n");
        return 1;
    }
    labelRatioEvents.clear();

    // Setting the same value again publishes nothing.
    doc->setTransitionLabelRatio(4, 0.3);
    if (!labelRatioEvents.empty()) {
        std::fprintf(stderr,
                     "FAIL: setTransitionLabelRatio(4, 0.3) again published a fact (unchanged should no-op)\n");
        return 1;
    }

    // Journal-replay half: TransitionLabelRatioOp{4, nullopt, 0.3} replayed
    // backward restores nullopt, forward restores 0.3, both through the doc
    // setter so the fact fires each time.
    {
        // in_place_type on purpose: a braced UndoOp literal makes GCC run out of
        // memory searching all the variant alternatives.
        app::TransactionDelta ratioDelta;
        ratioDelta.ops.push_back(app::UndoOp{std::in_place_type<app::TransitionLabelRatioOp>,
                                             app::TransitionLabelRatioOp{.id = 4, .before = std::nullopt, .after = 0.3}});
        labelRatioEvents.clear();
        app::applyDelta(*doc, ratioDelta, app::ApplyDirection::Backward);
        if (doc->findTransition(4)->labelRatio.has_value()) {
            std::fprintf(stderr, "FAIL: TransitionLabelRatioOp backward replay did not restore nullopt\n");
            return 1;
        }
        if (labelRatioEvents.size() != 1 || labelRatioEvents[0].ratio.has_value()) {
            std::fprintf(stderr, "FAIL: TransitionLabelRatioOp backward replay did not publish a nullopt fact\n");
            return 1;
        }
        labelRatioEvents.clear();
        app::applyDelta(*doc, ratioDelta, app::ApplyDirection::Forward);
        if (!doc->findTransition(4)->labelRatio.has_value() || *doc->findTransition(4)->labelRatio != 0.3) {
            std::fprintf(stderr, "FAIL: TransitionLabelRatioOp forward replay did not restore 0.3\n");
            return 1;
        }
        if (labelRatioEvents.size() != 1 || !labelRatioEvents[0].ratio.has_value() ||
            *labelRatioEvents[0].ratio != 0.3) {
            std::fprintf(stderr, "FAIL: TransitionLabelRatioOp forward replay did not publish 0.3\n");
            return 1;
        }
    }
    kernel.dispatcher().unsubscribe(&labelRatioOwner);
    // Leave the ratio cleared; nothing later expects transition 4 to carry one.
    doc->setTransitionLabelRatio(4, std::nullopt);

    // Deleting the initial state clears initialStateId in the same undo step;
    // one undo restores the state, its cascaded transition and the pointer.
    kernel.send(app::events::DeleteStateRequested{.id = 1});
    if (doc->machine().initialStateId != 0 || doc->findState(1) != nullptr) {
        std::fprintf(stderr, "FAIL: deleting the initial state did not clear initialStateId\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    if (doc->machine().initialStateId != 1 || doc->findState(1) == nullptr || doc->findTransition(4) == nullptr) {
        std::fprintf(stderr, "FAIL: undo of delete-initial did not restore state 1 / transition 4 / initialStateId\n");
        return 1;
    }
    undoStateEvents.clear();

    // ---- batch: a Begin/End pair merges every op inside into ONE step ----------
    const int statesBeforeBatch = static_cast<int>(doc->machine().states.size());
    kernel.send(app::events::BeginUndoBatchRequested{});
    kernel.send(app::events::AddStateRequested{.pos = QPointF(400, 0)});
    kernel.send(app::events::AddStateRequested{.pos = QPointF(500, 0)});
    kernel.send(app::events::EndUndoBatchRequested{});
    if (static_cast<int>(doc->machine().states.size()) != statesBeforeBatch + 2) {
        std::fprintf(stderr, "FAIL: batched AddState x2 did not add 2 states\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    if (static_cast<int>(doc->machine().states.size()) != statesBeforeBatch) {
        std::fprintf(stderr, "FAIL: ONE undo did not remove the whole 2-op batch\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (static_cast<int>(doc->machine().states.size()) != statesBeforeBatch + 2) {
        std::fprintf(stderr, "FAIL: ONE redo did not restore the whole 2-op batch\n");
        return 1;
    }
    undoStateEvents.clear();

    // ---- policy: Undo rejected while Simulate ----------------------------------
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    const app::Machine beforeUndoAttempt = doc->machine();
    const bool canUndoBeforeAttempt = undo->canUndo();
    kernel.send(app::events::UndoRequested{});
    if (!(doc->machine() == beforeUndoAttempt) || undo->canUndo() != canUndoBeforeAttempt) {
        std::fprintf(stderr, "FAIL: UndoRequested was not rejected while simulating\n");
        return 1;
    }
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});

    kernel.dispatcher().unsubscribe(&undoStateOwner);
    removeSimCommands(kernel);
    removeUndoPhaseCommands(kernel);

    // ---- layout plan: one ApplyLayoutPlanRequested = ONE undo step ---------------
    // Own kernel so the undo stack starts empty. UndoStore exposes no depth:
    // "exactly one entry" means one UndoRequested empties canUndo(); "no entry"
    // means one undo still restores the originals.
    {
        ordo::core::Kernel planKernel;
        planKernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        planKernel.registerAgent(std::make_shared<app::SimulationAgent>());
        planKernel.registerAgent(std::make_shared<app::UndoStore>());
        auto planDoc = planKernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto planUndo = planKernel.agentAs<app::UndoStore>(app::UndoStore::kName);
        registerUndoPhaseCommands(planKernel);

        app::Machine planMachine;
        for (quint64 id = 1; id <= 3; ++id) {
            app::State state;
            state.id = id;
            state.name = QStringLiteral("S%1").arg(id);
            state.pos = QPointF(static_cast<qreal>(id - 1) * 200.0, 0.0);
            planMachine.states.push_back(state);
        }
        app::Transition planTransition;
        planTransition.id = 4;
        planTransition.from = 1;
        planTransition.to = 2;
        planTransition.event = QStringLiteral("go");
        // A leftover drag offset: the plan must clear it and undo must restore it.
        planTransition.labelOffset = QPointF(18.0, -12.0);
        planMachine.transitions.push_back(planTransition);
        planMachine.initialStateId = 1;
        planMachine.nextId = 5;
        planDoc->restore(planMachine);
        planUndo->clearAll();

        // Bitwise compare, not operator== (QPointF's is fuzzy).
        const auto matches = [&planDoc](const QVector<QPointF>& positions, std::optional<qreal> ratio,
                                        QPointF offset) {
            for (int i = 0; i < positions.size(); ++i) {
                const app::State* state = planDoc->findState(static_cast<quint64>(i + 1));
                if (state == nullptr || state->pos.x() != positions[i].x() || state->pos.y() != positions[i].y()) {
                    return false;
                }
            }
            const app::Transition* transition = planDoc->findTransition(4);
            return transition != nullptr && transition->labelRatio == ratio &&
                   transition->labelOffset.x() == offset.x() && transition->labelOffset.y() == offset.y();
        };
        const QPointF originalOffset(18.0, -12.0);
        const QVector<QPointF> originalPositions = {QPointF(0, 0), QPointF(200, 0), QPointF(400, 0)};
        const QVector<QPointF> plannedPositions = {QPointF(24, 48), QPointF(264, 72), QPointF(400, 0)};

        // Two moves, one ratio, and one UNCHANGED placement (state 3) that the
        // command must skip.
        app::events::ApplyLayoutPlanRequested plan;
        for (int i = 0; i < plannedPositions.size(); ++i) {
            plan.states.push_back(app::StatePlacement{.id = static_cast<quint64>(i + 1), .pos = plannedPositions[i]});
        }
        plan.labels.push_back(app::LabelPlacement{.id = 4, .ratio = 0.25});

        planKernel.send(plan);
        if (!matches(plannedPositions, 0.25, QPointF()) || !planUndo->canUndo()) {
            std::fprintf(stderr, "FAIL: ApplyLayoutPlanRequested did not apply the plan as an undo entry\n");
            return 1;
        }
        planKernel.send(app::events::UndoRequested{});
        if (!matches(originalPositions, std::nullopt, originalOffset)) {
            std::fprintf(stderr, "FAIL: one undo of a layout plan did not restore every position and ratio bitwise\n");
            return 1;
        }
        if (planUndo->canUndo()) {
            std::fprintf(stderr, "FAIL: a layout plan dispatch pushed more than ONE undo entry\n");
            return 1;
        }
        planKernel.send(app::events::RedoRequested{});
        if (!matches(plannedPositions, 0.25, QPointF()) || planUndo->canRedo()) {
            std::fprintf(stderr, "FAIL: redo of a layout plan did not re-apply it\n");
            return 1;
        }

        // Idempotence: the identical plan again records nothing, so one undo
        // still lands on the originals and empties the stack.
        planKernel.send(plan);
        planKernel.send(app::events::UndoRequested{});
        if (!matches(originalPositions, std::nullopt, originalOffset) || planUndo->canUndo()) {
            std::fprintf(stderr, "FAIL: re-sending an already-applied layout plan recorded an undo entry\n");
            return 1;
        }
        std::printf("PASS: layout plan -- one dispatch = one undo entry (positions, ratio, and the leftover label "
                    "offset cleared), undo/redo bitwise incl. the offset, identical re-send records nothing\n");

        removeUndoPhaseCommands(planKernel);
    }

    return 0;
}

// Phase 2b: hierarchy data model + persistence + undo, on its own kernel.
// Structural only, except block 6, which checks that SimulationAgent::setMode()
// accepts a hierarchical machine and a real RunRequested runs it.
int runHierarchySmoke() {
    ordo::core::Kernel kernel;
    kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
    kernel.registerAgent(std::make_shared<app::SimulationAgent>());
    kernel.registerAgent(std::make_shared<app::UndoStore>());
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    auto undo = kernel.agentAs<app::UndoStore>(app::UndoStore::kName);
    if (!doc || !sim || !undo) {
        std::fprintf(stderr, "FAIL: hierarchy-phase agents did not all register under kName\n");
        return 1;
    }

    // Manual mode: the simulator never runs, but registerSimCommands() needs a SimClock.
    app::SimClock clock;
    clock.setManualMode(true);
    registerUndoPhaseCommands(kernel);
    registerSimCommands(kernel, clock);

    // ---- 1. auto initialChildId: assign / reassign-to-sibling / clear -------
    kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: P
    kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // id 2: A
    kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});  // id 3: B
    constexpr quint64 kP = 1;
    constexpr quint64 kA = 2;
    constexpr quint64 kB = 3;

    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = kP});
    if (doc->findState(kA)->parentId != kP || doc->findState(kP)->initialChildId != kA) {
        std::fprintf(stderr, "FAIL: reparenting the first child under P did not auto-assign initialChildId\n");
        return 1;
    }
    kernel.send(app::events::ReparentStateRequested{.id = kB, .parentId = kP});
    if (doc->findState(kB)->parentId != kP || doc->findState(kP)->initialChildId != kA) {
        std::fprintf(stderr, "FAIL: reparenting a second child under P moved initialChildId away from the first\n");
        return 1;
    }
    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = 0});  // A reparented away from P
    if (doc->findState(kA)->parentId != 0 || doc->findState(kP)->initialChildId != kB) {
        std::fprintf(stderr,
                     "FAIL: reparenting the initial child away did not reassign initialChildId to the remaining "
                     "sibling\n");
        return 1;
    }
    // One command captures two ops (auto InitialChildOp reassignment, then the
    // StateReparentOp): one undo restores both fields, one redo reproduces both.
    kernel.send(app::events::UndoRequested{});
    if (doc->findState(kA)->parentId != kP || doc->findState(kP)->initialChildId != kA) {
        std::fprintf(stderr,
                     "FAIL: undoing the reparent-away-with-auto-side-effect did not restore both parentId and "
                     "initialChildId\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (doc->findState(kA)->parentId != 0 || doc->findState(kP)->initialChildId != kB) {
        std::fprintf(stderr,
                     "FAIL: redoing the reparent-away-with-auto-side-effect did not reproduce both parentId and "
                     "initialChildId\n");
        return 1;
    }
    kernel.send(app::events::ReparentStateRequested{.id = kB, .parentId = 0});  // B reparented away -- P childless
    if (doc->findState(kB)->parentId != 0 || doc->findState(kP)->initialChildId != 0) {
        std::fprintf(stderr, "FAIL: reparenting the last remaining child away did not clear initialChildId\n");
        return 1;
    }

    // Rebuild the 2-level tree (P{A,B}, initialChildId == A) for the checks
    // below.
    kernel.send(app::events::ReparentStateRequested{.id = kA, .parentId = kP});
    kernel.send(app::events::ReparentStateRequested{.id = kB, .parentId = kP});
    if (doc->findState(kP)->initialChildId != kA || doc->findState(kA)->parentId != kP ||
        doc->findState(kB)->parentId != kP) {
        std::fprintf(stderr, "FAIL: rebuilding the 2-level tree P{A,B} did not leave initialChildId at A\n");
        return 1;
    }

    // ---- 2. policy: self/nonexistent-parent/cycle/bad-kind all refused, machine unchanged ----
    const app::Machine beforeSimpleRefusals = doc->machine();
    kernel.send(app::events::ReparentStateRequested{.id = kP, .parentId = kP});    // self
    kernel.send(app::events::ReparentStateRequested{.id = kP, .parentId = 999});   // nonexistent parent
    kernel.send(app::events::ReparentStateRequested{.id = kP, .parentId = kA});    // 1-hop cycle: A is P's own child
    if (!(doc->machine() == beforeSimpleRefusals)) {
        std::fprintf(stderr, "FAIL: self/nonexistent-parent/1-hop-cycle reparents were not all refused unchanged\n");
        return 1;
    }

    kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});  // id 4: F
    constexpr quint64 kF = 4;
    kernel.send(app::events::SetStateKindRequested{.id = kF, .kind = app::StateKind::Final});
    const app::Machine beforeBadKind = doc->machine();
    kernel.send(app::events::ReparentStateRequested{.id = kB, .parentId = kF});  // Final cannot contain children
    if (!(doc->machine() == beforeBadKind)) {
        std::fprintf(stderr, "FAIL: reparenting under a Final-kind state was not refused\n");
        return 1;
    }

    kernel.send(app::events::AddStateRequested{.pos = QPointF(400, 0)});  // id 5: C, parented under A -- P -> A -> C
    constexpr quint64 kC = 5;
    kernel.send(app::events::ReparentStateRequested{.id = kC, .parentId = kA});
    const app::Machine beforeDeepCycle = doc->machine();
    kernel.send(app::events::ReparentStateRequested{.id = kP, .parentId = kC});  // P into its own grandchild
    if (!(doc->machine() == beforeDeepCycle)) {
        std::fprintf(stderr, "FAIL: a 2-hop parentId cycle (P -> A -> C, then C as P's parent) was not refused\n");
        return 1;
    }
    kernel.send(app::events::ReparentStateRequested{.id = kC, .parentId = 0});  // clean up C/F before phase 3
    kernel.send(app::events::DeleteStateRequested{.id = kC});
    kernel.send(app::events::DeleteStateRequested{.id = kF});
    if (doc->findState(kP)->initialChildId != kA || doc->findState(kA)->parentId != kP ||
        doc->findState(kB)->parentId != kP) {
        std::fprintf(stderr, "FAIL: the P{A,B} tree was disturbed by the policy-refusal checks above\n");
        return 1;
    }

    // ---- 3. deleteState cascades the subtree + attached transitions; ONE undo restores it, ONE redo re-cascades ----
    // P{A,B} is a mid-vector compound with two children: the cascade deletes A
    // then B and undo restores B then A at their erased indices (append-only
    // restores would break the order-sensitive Machine::operator==). P2{A2} is
    // appended first so P sits mid-vector; it doubles as the suffix auto-assign check.
    const quint64 kP2 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(500, 0)});
    const quint64 kA2 = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(600, 0)});
    kernel.send(app::events::ReparentStateRequested{.id = kA2, .parentId = kP2});
    if (doc->findState(kP2)->initialChildId != kA2) {
        std::fprintf(stderr, "FAIL: setting up the P2{A2} suffix compound did not auto-assign initialChildId\n");
        return 1;
    }

    const quint64 transitionId = doc->machine().nextId;
    kernel.send(app::events::AddTransitionRequested{.from = kA, .to = kB});  // inside P's subtree, cascades with it
    kernel.send(app::events::SetTransitionEventRequested{.id = transitionId, .event = QStringLiteral("go")});

    const app::Machine preDelete = doc->machine();
    kernel.send(app::events::DeleteStateRequested{.id = kP});  // ONE command dispatch -> ONE undo step
    if (doc->findState(kP) != nullptr || doc->findState(kA) != nullptr || doc->findState(kB) != nullptr ||
        doc->findTransition(transitionId) != nullptr) {
        std::fprintf(stderr, "FAIL: deleting the compound P did not cascade both children and their transition\n");
        return 1;
    }
    if (doc->findState(kP2) == nullptr || doc->findState(kA2) == nullptr) {
        std::fprintf(stderr, "FAIL: deleting P took the unrelated P2{A2} compound with it\n");
        return 1;
    }
    const app::Machine postDelete = doc->machine();

    kernel.send(app::events::UndoRequested{});
    if (!(doc->machine() == preDelete)) {
        std::fprintf(stderr,
                     "FAIL: ONE undo of the mid-vector two-child compound delete did not deep-equal the "
                     "pre-delete machine (sibling/vector order included)\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (!(doc->machine() == postDelete)) {
        std::fprintf(stderr, "FAIL: ONE redo of the compound delete did not deep-equal the post-delete machine\n");
        return 1;
    }

    // ---- 4. .sdm round trip: byte-stable save, and v4-style files load 0 defaults ----
    {
        app::Machine hierMachine;
        hierMachine.name = QStringLiteral("Hier");
        hierMachine.states.push_back(app::State{
            .id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Normal, .initialChildId = 2});
        hierMachine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 1});
        hierMachine.states.push_back(
            app::State{.id = 3, .name = QStringLiteral("B"), .kind = app::StateKind::Normal, .parentId = 1});
        hierMachine.nextId = 4;
        hierMachine.initialStateId = 1;

        const QString path1 = QDir::temp().filePath(QStringLiteral("ordo-state-designer-hier-smoke-1.sdm"));
        const QString path2 = QDir::temp().filePath(QStringLiteral("ordo-state-designer-hier-smoke-2.sdm"));
        QFile::remove(path1);
        QFile::remove(path2);
        QString ioError;
        if (!app::saveMachine(hierMachine, path1, &ioError) || !app::saveMachine(hierMachine, path2, &ioError)) {
            std::fprintf(stderr, "FAIL: saveMachine failed for the hierarchy byte-stability check: %s\n",
                         qUtf8Printable(ioError));
            return 1;
        }
        QFile file1(path1);
        QFile file2(path2);
        if (!file1.open(QIODevice::ReadOnly) || !file2.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "FAIL: could not reopen the two hierarchy .sdm saves to compare bytes\n");
            return 1;
        }
        const QByteArray bytes1 = file1.readAll();
        const QByteArray bytes2 = file2.readAll();
        if (bytes1.isEmpty() || bytes1 != bytes2) {
            std::fprintf(stderr, "FAIL: saving the same Machine twice did not produce byte-identical .sdm files\n");
            return 1;
        }

        app::Machine loadedHier;
        if (!app::loadMachine(path1, &loadedHier, &ioError)) {
            std::fprintf(stderr, "FAIL: loadMachine failed for the hierarchy round trip: %s\n",
                         qUtf8Printable(ioError));
            return 1;
        }
        if (!(loadedHier == hierMachine)) {
            std::fprintf(stderr, "FAIL: parentId/initialChildId did not round-trip through the .sdm save/load\n");
            return 1;
        }
    }
    {
        // Hand-built v4-style file (no "parentId"/"initialChildId" keys):
        // machineFromJson() must read both as 0.
        QJsonObject v4;
        v4[QStringLiteral("formatVersion")] = 4;
        v4[QStringLiteral("name")] = QStringLiteral("PreHierarchy");
        v4[QStringLiteral("nextId")] = 2;
        v4[QStringLiteral("initialStateId")] = 1;
        QJsonArray states;
        QJsonObject stateObject;
        stateObject[QStringLiteral("id")] = 1;
        stateObject[QStringLiteral("name")] = QStringLiteral("Root");
        stateObject[QStringLiteral("kind")] = QStringLiteral("Normal");
        states.append(stateObject);
        v4[QStringLiteral("states")] = states;
        v4[QStringLiteral("notes")] = QJsonArray();
        const app::Machine legacy = app::machineFromJson(v4);
        if (legacy.states.size() != 1 || legacy.states[0].parentId != 0 || legacy.states[0].initialChildId != 0) {
            std::fprintf(stderr,
                         "FAIL: a v4-style file with no parentId/initialChildId keys did not load 0/0 defaults\n");
            return 1;
        }
    }

    // ---- 5. machine_validator hierarchy invariants ---------------------------
    {
        // a. parentId names a nonexistent state.
        app::Machine machine;
        machine.name = QStringLiteral("BadParent");
        machine.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 999});
        machine.nextId = 2;
        machine.initialStateId = 1;
        bool found = false;
        for (const app::Problem& problem : app::validate(machine)) {
            found = found || (problem.severity == app::ProblemSeverity::Error && problem.stateId == 1 &&
                               problem.text.contains(QStringLiteral("does not exist")));
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not flag a parentId naming a nonexistent state\n");
            return 1;
        }
    }
    {
        // b. parentId cycle.
        app::Machine machine;
        machine.name = QStringLiteral("Cycle");
        machine.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 2});
        machine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal, .parentId = 1});
        machine.nextId = 3;
        machine.initialStateId = 1;
        bool found = false;
        for (const app::Problem& problem : app::validate(machine)) {
            found = found ||
                    (problem.severity == app::ProblemSeverity::Error && problem.text.contains(QStringLiteral("cycle")));
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not flag a parentId cycle\n");
            return 1;
        }
    }
    {
        // c. a parent whose kind is Final/History.
        app::Machine machine;
        machine.name = QStringLiteral("BadKindParent");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("F"), .kind = app::StateKind::Final});
        machine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 1});
        machine.nextId = 3;
        machine.initialStateId = 2;
        bool found = false;
        for (const app::Problem& problem : app::validate(machine)) {
            found = found || (problem.severity == app::ProblemSeverity::Error && problem.stateId == 2 &&
                               problem.text.contains(QStringLiteral("Final/History")));
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not flag a child of a Final-kind parent\n");
            return 1;
        }
    }
    {
        // d. initialChildId that is not a direct child.
        app::Machine machine;
        machine.name = QStringLiteral("BadInitialChild");
        machine.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Normal, .initialChildId = 2});
        machine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});  // parentId 0, not P's child
        machine.nextId = 3;
        machine.initialStateId = 1;
        bool found = false;
        for (const app::Problem& problem : app::validate(machine)) {
            found = found || (problem.severity == app::ProblemSeverity::Error && problem.stateId == 1 &&
                               problem.text.contains(QStringLiteral("not a direct child")));
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not flag an initialChildId that is not a direct child\n");
            return 1;
        }
    }
    {
        // e. a state WITH children whose initialChildId == 0.
        app::Machine machine;
        machine.name = QStringLiteral("MissingInitialChild");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Normal});
        machine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 1});
        machine.nextId = 3;
        machine.initialStateId = 1;
        bool found = false;
        for (const app::Problem& problem : app::validate(machine)) {
            found = found || (problem.severity == app::ProblemSeverity::Error && problem.stateId == 1 &&
                               problem.text.contains(QStringLiteral("no initialChildId set")));
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not flag a state with children but no initialChildId\n");
            return 1;
        }
    }
    {
        // e (counter-case). A Parallel state enters every region, so it needs
        // no initialChildId.
        app::Machine machine;
        machine.name = QStringLiteral("ParallelNoInitialChild");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Parallel});
        machine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("R1"), .kind = app::StateKind::Normal, .parentId = 1});
        machine.states.push_back(
            app::State{.id = 3, .name = QStringLiteral("R2"), .kind = app::StateKind::Normal, .parentId = 1});
        machine.nextId = 4;
        machine.initialStateId = 1;
        for (const app::Problem& problem : app::validate(machine)) {
            if (problem.severity == app::ProblemSeverity::Error && problem.stateId == 1 &&
                problem.text.contains(QStringLiteral("no initialChildId set"))) {
                std::fprintf(stderr, "FAIL: validate() flagged a Parallel state for having no initialChildId\n");
                return 1;
            }
        }
    }
    {
        // f. History at the machine root, and History with children of its own.
        app::Machine machine;
        machine.name = QStringLiteral("BadHistory");
        machine.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("H"), .kind = app::StateKind::History});  // parentId 0
        machine.states.push_back(app::State{.id = 2, .name = QStringLiteral("HP"), .kind = app::StateKind::Normal});
        machine.states.push_back(
            app::State{.id = 3, .name = QStringLiteral("HC"), .kind = app::StateKind::History, .parentId = 2});
        machine.states.push_back(
            app::State{.id = 4, .name = QStringLiteral("HCC"), .kind = app::StateKind::Normal, .parentId = 3});
        machine.nextId = 5;
        machine.initialStateId = 2;
        bool foundRoot = false;
        bool foundChildren = false;
        for (const app::Problem& problem : app::validate(machine)) {
            if (problem.severity != app::ProblemSeverity::Error) {
                continue;
            }
            foundRoot = foundRoot || (problem.stateId == 1 && problem.text.contains(QStringLiteral("machine root")));
            foundChildren =
                foundChildren || (problem.stateId == 3 && problem.text.contains(QStringLiteral("children of its own")));
        }
        if (!foundRoot || !foundChildren) {
            std::fprintf(stderr,
                         "FAIL: validate() did not flag both a root-level History state and a History state with "
                         "children\n");
            return 1;
        }
    }
    {
        // A valid 2-level hierarchy produces no Errors.
        app::Machine machine;
        machine.name = QStringLiteral("ValidHierarchy");
        machine.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Normal, .initialChildId = 2});
        machine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 1});
        machine.states.push_back(
            app::State{.id = 3, .name = QStringLiteral("B"), .kind = app::StateKind::Normal, .parentId = 1});
        machine.nextId = 4;
        machine.initialStateId = 1;
        int errorCount = 0;
        for (const app::Problem& problem : app::validate(machine)) {
            if (problem.severity == app::ProblemSeverity::Error) {
                ++errorCount;
            }
        }
        if (errorCount != 0) {
            std::fprintf(stderr, "FAIL: a valid 2-level hierarchy produced %d unexpected Error(s)\n", errorCount);
            return 1;
        }
    }

    // ---- 6. SimulationAgent::setMode() accepts a hierarchical machine ---------
    // The live kernel's machine is still what block 3 left, P2{A2}, so it is
    // already hierarchical; blocks 4/5 only used local Machine values.
    if (!app::isHierarchical(doc->machine())) {
        std::fprintf(stderr, "FAIL: isHierarchical() is false for the live P2{A2} machine (A2.parentId != 0)\n");
        return 1;
    }
    {
        app::Machine flatCheck;
        flatCheck.name = QStringLiteral("Flat");
        flatCheck.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        flatCheck.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal});
        flatCheck.nextId = 3;
        if (app::isHierarchical(flatCheck)) {
            std::fprintf(stderr, "FAIL: isHierarchical() is true for a machine where every parentId is 0\n");
            return 1;
        }
    }

    // setMode(Simulate) succeeds on the hierarchical machine and RunRequested
    // through the registered sim commands runs it. initialStateId is pointed at
    // P2 first; otherwise activateInitialState() falls back and activates nothing.
    kernel.send(app::events::SetInitialStateRequested{.id = kP2});
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    if (sim->mode() != app::events::Mode::Simulate) {
        std::fprintf(stderr, "FAIL: SimulationAgent refused Simulate mode on a hierarchical machine (P2{A2})\n");
        return 1;
    }
    kernel.send(app::events::RunRequested{});
    bool configurationHasP2 = false;
    bool configurationHasA2 = false;
    for (quint64 activeId : sim->configuration()) {
        configurationHasP2 = configurationHasP2 || activeId == kP2;
        configurationHasA2 = configurationHasA2 || activeId == kA2;
    }
    if (!sim->running() || sim->configuration().size() != 2 || !configurationHasP2 || !configurationHasA2) {
        std::fprintf(stderr,
                      "FAIL: RunRequested through the command front door did not activate the hierarchical "
                      "P2{A2} configuration (P2 + its own initial child A2)\n");
        return 1;
    }
    // Stop the run first: the simulating() guard would refuse the reparent below.
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    // P2 loses its only child: initialChildId auto-clears and the machine is flat again.
    kernel.send(app::events::ReparentStateRequested{.id = kA2, .parentId = 0});
    if (app::isHierarchical(doc->machine())) {
        std::fprintf(stderr, "FAIL: reparenting A2 to root did not make the machine flat again\n");
        return 1;
    }
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    if (sim->mode() != app::events::Mode::Simulate) {
        std::fprintf(stderr, "FAIL: SimulationAgent refused Simulate mode on a now-flat machine\n");
        return 1;
    }
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});  // leave the kernel as found

    // ---- 7. Add Child State: a parented add auto-assigns the childless parent's
    // initialChildId; one undo removes the child and reverts initialChildId
    // together, one redo reproduces both; refusals (nonexistent or Final-kind
    // parentId) leave the machine deep-equal. P2 is childless (block 6).
    {
        const app::Machine beforeAddChild = doc->machine();
        const quint64 kChild = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(700, 0), .parentId = kP2});
        if (doc->findState(kChild) == nullptr || doc->findState(kChild)->parentId != kP2 ||
            doc->findState(kP2)->initialChildId != kChild) {
            std::fprintf(stderr,
                          "FAIL: command-path parented add did not create the child under P2 or auto-assign "
                          "initialChildId\n");
            return 1;
        }
        const app::Machine afterAddChild = doc->machine();

        kernel.send(app::events::UndoRequested{});
        // sameTopology(): undoing a fresh mint never rolls Machine::nextId back.
        if (!sameTopology(doc->machine(), beforeAddChild)) {
            std::fprintf(stderr,
                          "FAIL: ONE undo of the parented add did not deep-equal the pre-add machine (child + "
                          "initialChildId both reverted)\n");
            return 1;
        }
        kernel.send(app::events::RedoRequested{});
        if (!(doc->machine() == afterAddChild)) {
            std::fprintf(stderr, "FAIL: ONE redo of the parented add did not reproduce the post-add machine\n");
            return 1;
        }

        // Refusal: a nonexistent parentId leaves the machine unchanged.
        const app::Machine beforeRefusals = doc->machine();
        kernel.send(app::events::AddStateRequested{.pos = QPointF(700, 0), .parentId = 99999});
        if (!(doc->machine() == beforeRefusals)) {
            std::fprintf(stderr, "FAIL: AddStateRequested with a nonexistent parentId was not refused unchanged\n");
            return 1;
        }

        // Refusal: a Final-kind parentId leaves the machine unchanged.
        const quint64 kFinal = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(800, 0)});
        kernel.send(app::events::SetStateKindRequested{.id = kFinal, .kind = app::StateKind::Final});
        const app::Machine beforeFinalRefusal = doc->machine();
        kernel.send(app::events::AddStateRequested{.pos = QPointF(700, 0), .parentId = kFinal});
        if (!(doc->machine() == beforeFinalRefusal)) {
            std::fprintf(stderr, "FAIL: AddStateRequested with a Final-kind parentId was not refused unchanged\n");
            return 1;
        }
    }

    // ---- 8. HistoryDeep authoring: SetHistoryDeepCommand accepts only a
    // History-kind target (kHist), refuses others (kP2, still Normal) with a
    // qWarning and no change, and is undoable. The authored true value survives
    // an .sdm round trip.
    {
        const quint64 kHist = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(900, 0)});
        kernel.send(app::events::SetStateKindRequested{.id = kHist, .kind = app::StateKind::History});
        if (doc->findState(kHist)->historyDeep) {
            std::fprintf(stderr, "FAIL: a freshly added History state did not default historyDeep to false\n");
            return 1;
        }

        const app::Machine beforeToggle = doc->machine();
        kernel.send(app::events::SetHistoryDeepRequested{.stateId = kHist, .deep = true});
        if (!doc->findState(kHist)->historyDeep) {
            std::fprintf(stderr, "FAIL: SetHistoryDeepRequested on a History state did not set historyDeep true\n");
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        if (!(doc->machine() == beforeToggle)) {
            std::fprintf(stderr,
                          "FAIL: ONE undo of the historyDeep toggle did not deep-equal the pre-toggle machine\n");
            return 1;
        }
        kernel.send(app::events::RedoRequested{});
        if (!doc->findState(kHist)->historyDeep) {
            std::fprintf(stderr, "FAIL: ONE redo of the historyDeep toggle did not reproduce historyDeep true\n");
            return 1;
        }

        // Refusal: a non-History (Normal) target is rejected, machine unchanged.
        const app::Machine beforeKindRefusal = doc->machine();
        kernel.send(app::events::SetHistoryDeepRequested{.stateId = kP2, .deep = true});
        if (!(doc->machine() == beforeKindRefusal)) {
            std::fprintf(stderr, "FAIL: SetHistoryDeepRequested on a Normal-kind state was not refused unchanged\n");
            return 1;
        }

        // .sdm round trip: the command-authored true value survives save/load.
        const QString histDeepPath =
            QDir::temp().filePath(QStringLiteral("ordo-state-designer-hier-smoke-historydeep.sdm"));
        QFile::remove(histDeepPath);
        QString histDeepIoError;
        if (!app::saveMachine(doc->machine(), histDeepPath, &histDeepIoError)) {
            std::fprintf(stderr, "FAIL: saveMachine failed for the historyDeep authoring round trip: %s\n",
                         qUtf8Printable(histDeepIoError));
            return 1;
        }
        app::Machine reloadedHistDeep;
        if (!app::loadMachine(histDeepPath, &reloadedHistDeep, &histDeepIoError)) {
            std::fprintf(stderr, "FAIL: loadMachine failed for the historyDeep authoring round trip: %s\n",
                         qUtf8Printable(histDeepIoError));
            return 1;
        }
        const app::State* reloadedHist = nullptr;
        for (const app::State& state : reloadedHistDeep.states) {
            if (state.id == kHist) {
                reloadedHist = &state;
                break;
            }
        }
        if (reloadedHist == nullptr || !reloadedHist->historyDeep) {
            std::fprintf(stderr,
                          "FAIL: the command-authored historyDeep=true value did not survive the .sdm round trip\n");
            return 1;
        }
    }

    // ---- 8b. Element color: setter trio command path + one undo, missing-id
    // silent no-op, JSON round trip for all three element kinds, and files
    // without "color" keys loading as Default ---------------------------------
    {
        const app::Machine beforeColor = doc->machine();
        const quint64 kColorTarget = beforeColor.states.first().id;
        kernel.send(app::events::SetStateColorRequested{.id = kColorTarget, .color = app::ElementColor::Green});
        if (doc->findState(kColorTarget)->color != app::ElementColor::Green) {
            std::fprintf(stderr, "FAIL: SetStateColorRequested did not set the state's color\n");
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        if (!(doc->machine() == beforeColor)) {
            std::fprintf(stderr, "FAIL: ONE undo of a state recolor did not deep-equal the pre-color machine\n");
            return 1;
        }
        kernel.send(app::events::SetStateColorRequested{.id = 987654, .color = app::ElementColor::Red});
        if (!(doc->machine() == beforeColor)) {
            std::fprintf(stderr, "FAIL: SetStateColorRequested on a missing id was not a silent no-op\n");
            return 1;
        }

        app::Machine colored;
        colored.name = QStringLiteral("colors");
        colored.nextId = 10;
        colored.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .color = app::ElementColor::Red});
        colored.transitions.push_back(app::Transition{.id = 2, .from = 1, .to = 0, .color = app::ElementColor::Blue});
        colored.notes.push_back(app::Note{.id = 3, .text = QStringLiteral("n"), .color = app::ElementColor::Pink});
        const QJsonObject coloredJson = app::machineToJson(colored);
        if (!(app::machineFromJson(coloredJson) == colored)) {
            std::fprintf(stderr, "FAIL: element colors did not survive the v7 JSON round trip\n");
            return 1;
        }
        const auto stripColors = [](QJsonObject object, const char* key) {
            QJsonArray out;
            for (const QJsonValue& value : object.value(QLatin1String(key)).toArray()) {
                QJsonObject element = value.toObject();
                element.remove(QStringLiteral("color"));
                out.append(element);
            }
            object[QLatin1String(key)] = out;
            return object;
        };
        const QJsonObject legacyJson =
            stripColors(stripColors(stripColors(coloredJson, "states"), "transitions"), "notes");
        const app::Machine legacy = app::machineFromJson(legacyJson);
        if (legacy.states.first().color != app::ElementColor::Default ||
            legacy.transitions.first().color != app::ElementColor::Default ||
            legacy.notes.first().color != app::ElementColor::Default) {
            std::fprintf(stderr, "FAIL: a pre-v7 file (no color keys) did not load every color as Default\n");
            return 1;
        }
    }

    // ---- 8c. Machine self-target: authoring flag, retarget clears it in one
    // step, JSON IO, XState #machine round trip -------------------------------
    {
        const app::Machine beforeSelf = doc->machine();
        const quint64 kSelfT = beforeSelf.nextId;
        const quint64 kAnyState = beforeSelf.states.first().id;
        kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 0});
        kernel.send(app::events::SetMachineSelfRequested{.id = kSelfT, .machineSelf = true});
        if (doc->findTransition(kSelfT) == nullptr || !doc->findTransition(kSelfT)->machineSelf) {
            std::fprintf(stderr, "FAIL: SetMachineSelfRequested did not flag the fresh root transition\n");
            return 1;
        }
        // Retargeting the self pill onto a real state clears the flag in the
        // same undo step; one undo restores both to==0 and the flag.
        kernel.send(app::events::RetargetTransitionRequested{.id = kSelfT, .from = 0, .to = kAnyState});
        if (doc->findTransition(kSelfT)->to != kAnyState || doc->findTransition(kSelfT)->machineSelf) {
            std::fprintf(stderr, "FAIL: retargeting a machineSelf pill did not clear the flag with the retarget\n");
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        if (doc->findTransition(kSelfT)->to != 0 || !doc->findTransition(kSelfT)->machineSelf) {
            std::fprintf(stderr, "FAIL: ONE undo did not restore both the target and the machineSelf flag\n");
            return 1;
        }
        kernel.send(app::events::UndoRequested{});  // the flag set
        kernel.send(app::events::UndoRequested{});  // the add
        // sameTopology, not operator==: nextId never rolls back across an undo-of-an-add.
        if (!sameTopology(doc->machine(), beforeSelf)) {
            std::fprintf(stderr, "FAIL: undoing the machineSelf fixture did not restore the machine\n");
            return 1;
        }

        app::Machine selfMachine;
        selfMachine.name = QStringLiteral("selfio");
        selfMachine.nextId = 5;
        selfMachine.initialStateId = 1;
        selfMachine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A")});
        selfMachine.transitions.push_back(
            app::Transition{.id = 2, .event = QStringLiteral("Restart"), .machineSelf = true});
        if (!(app::machineFromJson(app::machineToJson(selfMachine)) == selfMachine)) {
            std::fprintf(stderr, "FAIL: machineSelf did not survive the v7 JSON round trip\n");
            return 1;
        }
        const app::XStateExportResult selfExport = app::machineToXStateJson(selfMachine);
        const app::XStateImportResult selfImport = app::machineFromXStateJson(selfExport.json);
        if (selfImport.machine.transitions.size() != 1 || !selfImport.machine.transitions.first().machineSelf ||
            selfImport.machine.transitions.first().to != 0) {
            std::fprintf(stderr, "FAIL: the XState '#machine' self-target did not round-trip (export+import)\n");
            return 1;
        }
    }

    // ---- 8d. Transition reenter and always authoring:
    // intents + undo/redo replay + mutual exclusivity with event/delay + JSON round trip
    {
        const app::Machine beforeRa = doc->machine();
        const quint64 kStateA = beforeRa.states.first().id;
        const quint64 kT = beforeRa.nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateA});
        if (doc->findTransition(kT) == nullptr || doc->findTransition(kT)->reenter || doc->findTransition(kT)->always) {
            std::fprintf(stderr, "FAIL: fresh transition did not default reenter=false and always=false\n");
            return 1;
        }

        // 1) SetTransitionReenterRequested
        kernel.send(app::events::SetTransitionReenterRequested{.id = kT, .reenter = true});
        if (!doc->findTransition(kT)->reenter) {
            std::fprintf(stderr, "FAIL: SetTransitionReenterRequested did not set reenter to true\n");
            return 1;
        }
        kernel.send(app::events::UndoRequested{});
        if (doc->findTransition(kT)->reenter) {
            std::fprintf(stderr, "FAIL: undo did not restore reenter=false\n");
            return 1;
        }
        kernel.send(app::events::RedoRequested{});
        if (!doc->findTransition(kT)->reenter) {
            std::fprintf(stderr, "FAIL: redo did not re-apply reenter=true\n");
            return 1;
        }

        // 2) SetTransitionAlwaysRequested
        kernel.send(app::events::SetTransitionEventRequested{.id = kT, .event = QStringLiteral("Click")});
        kernel.send(app::events::SetTransitionDelayRequested{.id = kT, .delayMs = 500});
        kernel.send(app::events::SetTransitionAlwaysRequested{.id = kT, .always = true});
        if (!doc->findTransition(kT)->always || !doc->findTransition(kT)->isAlways() ||
            !doc->findTransition(kT)->event.isEmpty() || doc->findTransition(kT)->delayMs != 0) {
            std::fprintf(stderr, "FAIL: setting always=true did not clear event and delayMs\n");
            return 1;
        }

        // Setting non-empty event clears always
        kernel.send(app::events::SetTransitionEventRequested{.id = kT, .event = QStringLiteral("Wake")});
        if (doc->findTransition(kT)->always) {
            std::fprintf(stderr, "FAIL: setting non-empty event on always transition did not clear always\n");
            return 1;
        }

        // Setting always=true again
        kernel.send(app::events::SetTransitionAlwaysRequested{.id = kT, .always = true});
        if (!doc->findTransition(kT)->always) {
            std::fprintf(stderr, "FAIL: re-setting always=true failed\n");
            return 1;
        }
        // Setting non-zero delay clears always
        kernel.send(app::events::SetTransitionDelayRequested{.id = kT, .delayMs = 200});
        if (doc->findTransition(kT)->always) {
            std::fprintf(stderr, "FAIL: setting non-zero delay on always transition did not clear always\n");
            return 1;
        }

        // 3) JSON round trip with reenter and always
        app::Machine testMachine;
        testMachine.name = QStringLiteral("reenter_always_io");
        testMachine.nextId = 4;
        testMachine.initialStateId = 1;
        testMachine.states.push_back(app::State{.id = 1, .name = QStringLiteral("S1")});
        testMachine.transitions.push_back(app::Transition{
            .id = 2, .from = 1, .to = 1, .event = QStringLiteral("E1"), .reenter = true, .always = false});
        testMachine.transitions.push_back(app::Transition{
            .id = 3, .from = 1, .to = 1, .event = QString(), .reenter = false, .always = true});
        const app::Machine reloaded = app::machineFromJson(app::machineToJson(testMachine));
        if (!(reloaded == testMachine)) {
            std::fprintf(stderr, "FAIL: reenter and always flags did not round-trip through JSON\n");
            return 1;
        }

        // Clean up undo stack back to beforeRa
        while (doc->machine().transitions.size() > beforeRa.transitions.size()) {
            kernel.send(app::events::UndoRequested{});
        }
    }

    // ---- 8e. Multiple targets retargeting and undo/redo
    {
        const app::Machine beforeMt = doc->machine();
        if (beforeMt.states.size() >= 2) {
            const quint64 kState1 = beforeMt.states[0].id;
            const quint64 kState2 = beforeMt.states[1].id;
            const quint64 kState3 = beforeMt.states.size() > 2 ? beforeMt.states[2].id : kState1;
            const quint64 kTransId = beforeMt.nextId;
            kernel.send(app::events::AddTransitionRequested{.from = kState1, .to = kState2});
            if (doc->findTransition(kTransId) == nullptr || doc->findTransition(kTransId)->isMultiTarget()) {
                std::fprintf(stderr, "FAIL: fresh transition should not be multi-target\n");
                return 1;
            }

            int retargetCount = 0;
            QList<quint64> broadcastTargets;
            int retargetOwner = 0;
            kernel.dispatcher().subscribe<app::events::TransitionRetargeted>(
                &retargetOwner, [&](const app::events::TransitionRetargeted& ev) {
                    if (ev.id == kTransId) {
                        ++retargetCount;
                        broadcastTargets = ev.targets;
                    }
                });

            // Retarget to multiple targets: {kState2, kState3}
            const QList<quint64> newTargets = {kState2, kState3};
            kernel.send(app::events::RetargetTransitionRequested{
                .id = kTransId, .from = kState1, .to = kState2, .targets = newTargets});

            const app::Transition* tAfter = doc->findTransition(kTransId);
            if (!tAfter || !tAfter->isMultiTarget() || tAfter->effectiveTargets() != newTargets ||
                tAfter->to != kState2 || broadcastTargets != newTargets || retargetCount != 1) {
                std::fprintf(stderr, "FAIL: RetargetTransitionRequested with multiple targets failed\n");
                return 1;
            }

            // Undo: should revert to single target
            kernel.send(app::events::UndoRequested{});
            const app::Transition* tUndone = doc->findTransition(kTransId);
            if (!tUndone || tUndone->isMultiTarget() || tUndone->to != kState2 || !tUndone->targets.isEmpty()) {
                std::fprintf(stderr, "FAIL: Undo of multi-target retarget failed to restore single target\n");
                return 1;
            }

            // Redo: should restore multiple targets
            kernel.send(app::events::RedoRequested{});
            const app::Transition* tRedone = doc->findTransition(kTransId);
            if (!tRedone || !tRedone->isMultiTarget() || tRedone->effectiveTargets() != newTargets) {
                std::fprintf(stderr, "FAIL: Redo of multi-target retarget failed to restore multiple targets\n");
                return 1;
            }

            kernel.dispatcher().unsubscribe(&retargetOwner);

            // Clean up undo stack back to beforeMt
            while (doc->machine().transitions.size() > beforeMt.transitions.size()) {
                kernel.send(app::events::UndoRequested{});
            }
        }
    }

    removeSimCommands(kernel);
    removeUndoPhaseCommands(kernel);

    return 0;
}

// Phase 2c: context variable authoring on its own kernel. Each verb goes
// command path -> assert document -> one undo -> one redo. sameTopology() (not
// operator==) is used only for the add's undo, since nextId never rolls back.
int runContextSmoke() {
    ordo::core::Kernel kernel;
    kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
    kernel.registerAgent(std::make_shared<app::SimulationAgent>());
    kernel.registerAgent(std::make_shared<app::UndoStore>());
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    auto undo = kernel.agentAs<app::UndoStore>(app::UndoStore::kName);
    if (!doc || !sim || !undo) {
        std::fprintf(stderr, "FAIL: context-phase agents did not all register under kName\n");
        return 1;
    }

    // Manual mode: the simulator never runs, but registerSimCommands() needs a SimClock.
    app::SimClock clock;
    clock.setManualMode(true);
    registerUndoPhaseCommands(kernel);
    registerSimCommands(kernel, clock);

    // ---- 1/2a. AddContextVariableRequested mints var<id>/Int/"0" -------------
    const app::Machine beforeAdd = doc->machine();
    const quint64 kVar1 = doc->machine().nextId;
    kernel.send(app::events::AddContextVariableRequested{});
    const app::ContextVariable* mintedVar1 = doc->findContextVariable(kVar1);
    if (mintedVar1 == nullptr || mintedVar1->name != QStringLiteral("var%1").arg(kVar1) ||
        mintedVar1->type != app::ContextType::Int || mintedVar1->initialValue != QStringLiteral("0")) {
        std::fprintf(stderr, "FAIL: AddContextVariableRequested did not mint var<id>/ContextType::Int/\"0\"\n");
        return 1;
    }
    const app::Machine afterAdd = doc->machine();
    kernel.send(app::events::UndoRequested{});
    if (!sameTopology(doc->machine(), beforeAdd)) {
        std::fprintf(stderr, "FAIL: ONE undo of AddContextVariableRequested did not restore the pre-add machine\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (!(doc->machine() == afterAdd)) {
        std::fprintf(stderr, "FAIL: ONE redo of AddContextVariableRequested did not reproduce the minted variable\n");
        return 1;
    }

    // ---- 1/2b. RenameContextVariableRequested ---------------------------------
    const app::Machine beforeRename = doc->machine();
    kernel.send(app::events::RenameContextVariableRequested{.id = kVar1, .name = QStringLiteral("counter")});
    if (doc->findContextVariable(kVar1)->name != QStringLiteral("counter")) {
        std::fprintf(stderr, "FAIL: RenameContextVariableRequested did not rename the context variable\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    if (!(doc->machine() == beforeRename)) {
        std::fprintf(stderr,
                      "FAIL: ONE undo of RenameContextVariableRequested did not deep-equal the pre-rename machine\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (doc->findContextVariable(kVar1)->name != QStringLiteral("counter")) {
        std::fprintf(stderr, "FAIL: ONE redo of RenameContextVariableRequested did not reproduce the new name\n");
        return 1;
    }

    // ---- 1/2c. SetContextTypeRequested ----------------------------------------
    const app::Machine beforeType = doc->machine();
    kernel.send(app::events::SetContextTypeRequested{.id = kVar1, .type = app::ContextType::Bool});
    if (doc->findContextVariable(kVar1)->type != app::ContextType::Bool) {
        std::fprintf(stderr, "FAIL: SetContextTypeRequested did not retype the context variable to Bool\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    if (!(doc->machine() == beforeType)) {
        std::fprintf(stderr, "FAIL: ONE undo of SetContextTypeRequested did not deep-equal the pre-retype machine\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (doc->findContextVariable(kVar1)->type != app::ContextType::Bool) {
        std::fprintf(stderr, "FAIL: ONE redo of SetContextTypeRequested did not reproduce ContextType::Bool\n");
        return 1;
    }

    // ---- 1/2d. SetContextInitialValueRequested --------------------------------
    const app::Machine beforeInitialValue = doc->machine();
    kernel.send(app::events::SetContextInitialValueRequested{.id = kVar1, .initialValue = QStringLiteral("true")});
    if (doc->findContextVariable(kVar1)->initialValue != QStringLiteral("true")) {
        std::fprintf(stderr, "FAIL: SetContextInitialValueRequested did not set the context variable's initial value\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    if (!(doc->machine() == beforeInitialValue)) {
        std::fprintf(
            stderr,
            "FAIL: ONE undo of SetContextInitialValueRequested did not deep-equal the pre-reinitialise machine\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (doc->findContextVariable(kVar1)->initialValue != QStringLiteral("true")) {
        std::fprintf(stderr, "FAIL: ONE redo of SetContextInitialValueRequested did not reproduce \"true\"\n");
        return 1;
    }

    // ---- 1/2e + 3. DeleteContextVariableRequested: two variables, delete the
    // first; one undo restores it at index 0 (not appended), one redo
    // reproduces the post-delete document
    // -----------------------------------------------------------------------
    const quint64 kVar2 = doc->machine().nextId;
    kernel.send(app::events::AddContextVariableRequested{});
    if (doc->machine().context.size() != 2 || doc->machine().context.at(0).id != kVar1 ||
        doc->machine().context.at(1).id != kVar2) {
        std::fprintf(stderr, "FAIL: the second AddContextVariableRequested did not append var<id> after var<%llu>\n",
                     static_cast<unsigned long long>(kVar1));
        return 1;
    }
    const app::Machine beforeDelete = doc->machine();
    kernel.send(app::events::DeleteContextVariableRequested{.id = kVar1});
    if (doc->machine().context.size() != 1 || doc->machine().context.at(0).id != kVar2) {
        std::fprintf(stderr, "FAIL: DeleteContextVariableRequested did not remove the first of two variables\n");
        return 1;
    }
    const app::Machine afterDelete = doc->machine();
    kernel.send(app::events::UndoRequested{});
    if (!(doc->machine() == beforeDelete)) {
        std::fprintf(stderr, "FAIL: ONE undo of the first-variable delete did not deep-equal the pre-delete machine\n");
        return 1;
    }
    if (doc->machine().context.size() != 2 || doc->machine().context.at(0).id != kVar1 ||
        doc->machine().context.at(1).id != kVar2) {
        std::fprintf(stderr,
                      "FAIL: ONE undo of the first-variable delete restored it by APPENDING rather than at its "
                      "original vector index 0\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (!(doc->machine() == afterDelete)) {
        std::fprintf(stderr, "FAIL: ONE redo of the first-variable delete did not deep-equal the post-delete machine\n");
        return 1;
    }

    // ---- 4. .sdm round trip: reloaded operator== the original; saving the
    // same context-bearing machine twice is byte-identical ---------------------
    {
        app::Machine withContext;
        withContext.name = QStringLiteral("ContextIO");
        withContext.states.push_back(app::State{.id = 1, .name = QStringLiteral("A")});
        withContext.initialStateId = 1;
        withContext.context.push_back(app::ContextVariable{.id = 2,
                                                             .name = QStringLiteral("counter"),
                                                             .type = app::ContextType::Int,
                                                             .initialValue = QStringLiteral("0")});
        withContext.context.push_back(app::ContextVariable{.id = 3,
                                                             .name = QStringLiteral("locked"),
                                                             .type = app::ContextType::Bool,
                                                             .initialValue = QStringLiteral("true")});
        withContext.context.push_back(app::ContextVariable{.id = 4,
                                                             .name = QStringLiteral("label"),
                                                             .type = app::ContextType::String,
                                                             .initialValue = QStringLiteral("hi")});
        withContext.context.push_back(app::ContextVariable{.id = 5,
                                                             .name = QStringLiteral("user"),
                                                             .type = app::ContextType::Object,
                                                             .initialValue = QStringLiteral("{\"name\":\"Alice\",\"age\":30}")});
        withContext.nextId = 6;

        const QString path1 = QDir::temp().filePath(QStringLiteral("ordo-state-designer-context-smoke-1.sdm"));
        const QString path2 = QDir::temp().filePath(QStringLiteral("ordo-state-designer-context-smoke-2.sdm"));
        QFile::remove(path1);
        QFile::remove(path2);
        QString ioError;
        if (!app::saveMachine(withContext, path1, &ioError) || !app::saveMachine(withContext, path2, &ioError)) {
            std::fprintf(stderr, "FAIL: saveMachine failed for the context byte-stability check: %s\n",
                         qUtf8Printable(ioError));
            return 1;
        }
        QFile file1(path1);
        QFile file2(path2);
        if (!file1.open(QIODevice::ReadOnly) || !file2.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "FAIL: could not reopen the two context .sdm saves to compare bytes\n");
            return 1;
        }
        const QByteArray bytes1 = file1.readAll();
        const QByteArray bytes2 = file2.readAll();
        if (bytes1.isEmpty() || bytes1 != bytes2) {
            std::fprintf(stderr, "FAIL: saving the same context-bearing Machine twice did not produce "
                                  "byte-identical .sdm files\n");
            return 1;
        }

        app::Machine reloadedContext;
        if (!app::loadMachine(path1, &reloadedContext, &ioError)) {
            std::fprintf(stderr, "FAIL: loadMachine failed for the context .sdm round trip: %s\n",
                         qUtf8Printable(ioError));
            return 1;
        }
        if (!(reloadedContext == withContext)) {
            std::fprintf(stderr,
                          "FAIL: the reloaded machine did not deep-equal the saved one (context .sdm round trip)\n");
            return 1;
        }

        // ---- 5. back-compat: a hand-built v7-shaped object (no "context" key)
        // loads an empty context ------------------------------------------------
        QJsonObject v7Shaped = app::machineToJson(withContext);
        v7Shaped.remove(QStringLiteral("context"));
        v7Shaped[QStringLiteral("formatVersion")] = 7;
        const app::Machine backCompat = app::machineFromJson(v7Shaped);
        if (!backCompat.context.isEmpty()) {
            std::fprintf(stderr,
                          "FAIL: a v7-shaped file with no \"context\" key did not load an empty context\n");
            return 1;
        }

        // ---- 6. context-free byte-identity: a machine with no context
        // variables writes no "context" key at all (asserted on the serialized
        // JSON directly, not via a reload) --------------------------------------
        app::Machine contextFree;
        contextFree.name = QStringLiteral("ContextFree");
        contextFree.states.push_back(app::State{.id = 1, .name = QStringLiteral("A")});
        contextFree.initialStateId = 1;
        contextFree.nextId = 2;
        const QJsonObject contextFreeJson = app::machineToJson(contextFree);
        if (contextFreeJson.contains(QStringLiteral("context"))) {
            std::fprintf(stderr, "FAIL: a context-free machine's v8 JSON still carries a \"context\" key\n");
            return 1;
        }
    }

    // ---- 7. validator: bad identifier + duplicate name Errors ------------------
    {
        app::Machine invalidName;
        invalidName.name = QStringLiteral("BadContextName");
        invalidName.states.push_back(app::State{.id = 1, .name = QStringLiteral("A")});
        invalidName.initialStateId = 1;
        invalidName.context.push_back(app::ContextVariable{
            .id = 2, .name = QStringLiteral("2bad"), .type = app::ContextType::Int, .initialValue = QStringLiteral("0")});
        invalidName.nextId = 3;
        const QVector<app::Problem> invalidProblems = app::validate(invalidName);
        bool foundInvalid = false;
        for (const app::Problem& problem : invalidProblems) {
            foundInvalid = foundInvalid || (problem.severity == app::ProblemSeverity::Error &&
                                             problem.text.contains(QStringLiteral("not a valid identifier")));
        }
        if (!foundInvalid) {
            std::fprintf(stderr,
                          "FAIL: validate() did not flag a context variable name that is not a valid identifier\n");
            return 1;
        }

        app::Machine dupName;
        dupName.name = QStringLiteral("DupContextName");
        dupName.states.push_back(app::State{.id = 1, .name = QStringLiteral("A")});
        dupName.initialStateId = 1;
        dupName.context.push_back(app::ContextVariable{
            .id = 2, .name = QStringLiteral("shared"), .type = app::ContextType::Int, .initialValue = QStringLiteral("0")});
        dupName.context.push_back(app::ContextVariable{.id = 3,
                                                         .name = QStringLiteral("shared"),
                                                         .type = app::ContextType::Bool,
                                                         .initialValue = QStringLiteral("false")});
        dupName.nextId = 4;
        const QVector<app::Problem> dupProblems = app::validate(dupName);
        bool foundDup = false;
        for (const app::Problem& problem : dupProblems) {
            foundDup = foundDup || (problem.severity == app::ProblemSeverity::Error &&
                                     problem.text.contains(QStringLiteral("share the name")));
        }
        if (!foundDup) {
            std::fprintf(stderr, "FAIL: validate() did not flag two context variables sharing the same name\n");
            return 1;
        }

        app::Machine badJsonObj;
        badJsonObj.name = QStringLiteral("BadJsonContext");
        badJsonObj.states.push_back(app::State{.id = 1, .name = QStringLiteral("A")});
        badJsonObj.initialStateId = 1;
        badJsonObj.context.push_back(app::ContextVariable{
            .id = 2, .name = QStringLiteral("badObject"), .type = app::ContextType::Object, .initialValue = QStringLiteral("{not valid json}")});
        badJsonObj.nextId = 3;
        const QVector<app::Problem> badJsonProblems = app::validate(badJsonObj);
        bool foundBadJson = false;
        for (const app::Problem& problem : badJsonProblems) {
            foundBadJson = foundBadJson || (problem.severity == app::ProblemSeverity::Error &&
                                            problem.text.contains(QStringLiteral("not a valid JSON object")));
        }
        if (!foundBadJson) {
            std::fprintf(stderr, "FAIL: validate() did not flag an Object context variable with invalid JSON initialValue\n");
            return 1;
        }
    }

    removeSimCommands(kernel);
    removeUndoPhaseCommands(kernel);

    return 0;
}

// Invoke declaration authoring: SetInvokeSrcRequested/SetInvokeIdRequested set
// State::invokeSrc/invokeId and, whenever the effective id (invokeId if set,
// else invokeSrc) changes, rename every "done.invoke.<id>"/
// "error.platform.<id>" Transition::event the state owns.
// File-local: called only from runSmoke() below.
static int runInvokeSmoke() {
    ordo::core::Kernel kernel;
    kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
    kernel.registerAgent(std::make_shared<app::SimulationAgent>());
    kernel.registerAgent(std::make_shared<app::UndoStore>());
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    auto undo = kernel.agentAs<app::UndoStore>(app::UndoStore::kName);
    if (!doc || !sim || !undo) {
        std::fprintf(stderr, "FAIL: invoke-phase agents did not all register under kName\n");
        return 1;
    }

    // Manual mode: the simulator never runs, but registerSimCommands() needs a SimClock.
    app::SimClock clock;
    clock.setManualMode(true);
    registerUndoPhaseCommands(kernel);
    registerSimCommands(kernel, clock);

    // ---- 1. AddStateRequested mints the invoking state A; undo-of-an-add
    // uses sameTopology() since nextId never rolls back ------------------------
    const app::Machine beforeAddA = doc->machine();
    const quint64 kStateA = doc->machine().nextId;
    kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
    if (doc->findState(kStateA) == nullptr) {
        std::fprintf(stderr, "FAIL: AddStateRequested did not mint the invoking state\n");
        return 1;
    }
    const app::Machine afterAddA = doc->machine();
    kernel.send(app::events::UndoRequested{});
    if (!sameTopology(doc->machine(), beforeAddA)) {
        std::fprintf(stderr,
                      "FAIL: ONE undo of AddStateRequested (invoking state) did not restore pre-add topology\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (!(doc->machine() == afterAddA)) {
        std::fprintf(stderr, "FAIL: ONE redo of AddStateRequested (invoking state) did not reproduce the minted state\n");
        return 1;
    }
    kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});  // B: the onDone/onError target
    const quint64 kStateB = kStateA + 1;
    if (doc->findState(kStateB) == nullptr) {
        std::fprintf(stderr, "FAIL: AddStateRequested did not mint the onDone/onError target state\n");
        return 1;
    }

    // ---- 2. SetInvokeSrcRequested, first set: no cascade (old effective id was empty) --
    const app::Machine beforeSrc = doc->machine();
    kernel.send(app::events::SetInvokeSrcRequested{.stateId = kStateA, .src = QStringLiteral("fetchData")});
    if (doc->findState(kStateA)->invokeSrc != QStringLiteral("fetchData") ||
        !doc->findState(kStateA)->invokeId.isEmpty()) {
        std::fprintf(stderr, "FAIL: SetInvokeSrcRequested did not set invokeSrc (invokeId should stay empty)\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    if (!(doc->machine() == beforeSrc)) {
        std::fprintf(stderr, "FAIL: ONE undo of the first SetInvokeSrcRequested did not deep-equal the pre-set machine\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (doc->findState(kStateA)->invokeSrc != QStringLiteral("fetchData")) {
        std::fprintf(stderr, "FAIL: ONE redo of the first SetInvokeSrcRequested did not reproduce \"fetchData\"\n");
        return 1;
    }

    // ---- 3. Wire onDone/onError against the current effective id ("fetchData",
    // from invokeSrc since invokeId is still empty) ---------------------------
    kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
    const quint64 kDoneTransition = kStateB + 1;
    kernel.send(app::events::SetTransitionEventRequested{.id = kDoneTransition,
                                                          .event = QStringLiteral("done.invoke.fetchData")});
    kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
    const quint64 kErrorTransition = kDoneTransition + 1;
    kernel.send(app::events::SetTransitionEventRequested{.id = kErrorTransition,
                                                          .event = QStringLiteral("error.platform.fetchData")});
    if (doc->findTransition(kDoneTransition)->event != QStringLiteral("done.invoke.fetchData") ||
        doc->findTransition(kErrorTransition)->event != QStringLiteral("error.platform.fetchData")) {
        std::fprintf(stderr, "FAIL: could not wire the onDone/onError transitions under the initial effective id\n");
        return 1;
    }

    // ---- 4. Rename cascade #1: changing invokeSrc (invokeId empty) changes the
    // effective id, so both transitions cascade in the same undo transaction --
    const app::Machine beforeSrcRename = doc->machine();
    kernel.send(app::events::SetInvokeSrcRequested{.stateId = kStateA, .src = QStringLiteral("fetchOther")});
    if (doc->findState(kStateA)->invokeSrc != QStringLiteral("fetchOther") ||
        doc->findTransition(kDoneTransition)->event != QStringLiteral("done.invoke.fetchOther") ||
        doc->findTransition(kErrorTransition)->event != QStringLiteral("error.platform.fetchOther")) {
        std::fprintf(stderr,
                      "FAIL: renaming invokeSrc (id derived from it) did not cascade to both done.invoke./"
                      "error.platform. transitions\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    if (!(doc->machine() == beforeSrcRename)) {
        std::fprintf(stderr,
                      "FAIL: ONE undo of the invokeSrc rename did not restore BOTH the field and the cascaded "
                      "transition events together\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (doc->findState(kStateA)->invokeSrc != QStringLiteral("fetchOther") ||
        doc->findTransition(kDoneTransition)->event != QStringLiteral("done.invoke.fetchOther") ||
        doc->findTransition(kErrorTransition)->event != QStringLiteral("error.platform.fetchOther")) {
        std::fprintf(stderr, "FAIL: ONE redo of the invokeSrc rename did not reproduce the cascaded rename\n");
        return 1;
    }

    // ---- 5. Rename cascade #2: SetInvokeIdRequested sets an explicit id that
    // overrides the src-derived one; cascades the same way --------------------
    const app::Machine beforeIdSet = doc->machine();
    kernel.send(app::events::SetInvokeIdRequested{.stateId = kStateA, .invokeId = QStringLiteral("fetchOtherActor")});
    if (doc->findState(kStateA)->invokeId != QStringLiteral("fetchOtherActor") ||
        doc->findTransition(kDoneTransition)->event != QStringLiteral("done.invoke.fetchOtherActor") ||
        doc->findTransition(kErrorTransition)->event != QStringLiteral("error.platform.fetchOtherActor")) {
        std::fprintf(stderr,
                      "FAIL: setting an explicit invokeId did not cascade to both done.invoke./error.platform. "
                      "transitions\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    if (!(doc->machine() == beforeIdSet)) {
        std::fprintf(stderr,
                      "FAIL: ONE undo of the invokeId set did not restore BOTH the field and the cascaded "
                      "transition events together\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (doc->findState(kStateA)->invokeId != QStringLiteral("fetchOtherActor") ||
        doc->findTransition(kDoneTransition)->event != QStringLiteral("done.invoke.fetchOtherActor") ||
        doc->findTransition(kErrorTransition)->event != QStringLiteral("error.platform.fetchOtherActor")) {
        std::fprintf(stderr, "FAIL: ONE redo of the invokeId set did not reproduce the cascaded rename\n");
        return 1;
    }

    // ---- 5b. SetInvokeOutputTypeRequested: plain field, no cascade (the value
    // never appears in a Transition::event); one undo/redo round trip ---------
    const app::Machine beforeOutputType = doc->machine();
    kernel.send(app::events::SetInvokeOutputTypeRequested{.stateId = kStateA, .type = app::ContextType::String});
    if (doc->findState(kStateA)->invokeOutputType != app::ContextType::String) {
        std::fprintf(stderr, "FAIL: SetInvokeOutputTypeRequested did not set invokeOutputType\n");
        return 1;
    }
    kernel.send(app::events::UndoRequested{});
    if (!(doc->machine() == beforeOutputType)) {
        std::fprintf(stderr,
                      "FAIL: ONE undo of SetInvokeOutputTypeRequested did not deep-equal the pre-set machine\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (doc->findState(kStateA)->invokeOutputType != app::ContextType::String) {
        std::fprintf(stderr, "FAIL: ONE redo of SetInvokeOutputTypeRequested did not reproduce String\n");
        return 1;
    }

    // ---- 6. Delete cascade: deleting the invoking state takes its invoke fields
    // and its onDone/onError transitions with it ------------------------------
    const app::Machine beforeDelete = doc->machine();
    kernel.send(app::events::DeleteStateRequested{.id = kStateA});
    if (doc->findState(kStateA) != nullptr || doc->findTransition(kDoneTransition) != nullptr ||
        doc->findTransition(kErrorTransition) != nullptr) {
        std::fprintf(stderr,
                      "FAIL: deleting the invoking state did not cascade its onDone/onError transitions away\n");
        return 1;
    }
    const app::Machine afterDelete = doc->machine();
    kernel.send(app::events::UndoRequested{});
    if (!(doc->machine() == beforeDelete)) {
        std::fprintf(stderr, "FAIL: ONE undo of the invoking-state delete did not deep-equal the pre-delete machine\n");
        return 1;
    }
    if (doc->findState(kStateA)->invokeSrc != QStringLiteral("fetchOther") ||
        doc->findState(kStateA)->invokeId != QStringLiteral("fetchOtherActor") ||
        doc->findState(kStateA)->invokeOutputType != app::ContextType::String) {
        std::fprintf(stderr,
                      "FAIL: undoing the invoking-state delete did not restore its invokeSrc/invokeId/"
                      "invokeOutputType\n");
        return 1;
    }
    kernel.send(app::events::RedoRequested{});
    if (!(doc->machine() == afterDelete)) {
        std::fprintf(stderr, "FAIL: ONE redo of the invoking-state delete did not deep-equal the post-delete machine\n");
        return 1;
    }

    // ---- 7. .sdm v9 round trip: save -> load -> operator== -----------------
    {
        app::Machine withInvoke;
        withInvoke.name = QStringLiteral("InvokeIO");
        withInvoke.states.push_back(app::State{.id = 1,
                                                .name = QStringLiteral("Loading"),
                                                .invokeSrc = QStringLiteral("fetchUser"),
                                                .invokeId = QStringLiteral("fetchUserActor"),
                                                // Non-default value (String, not the Int default) so the
                                                // round trip does not pass vacuously.
                                                .invokeOutputType = app::ContextType::String});
        withInvoke.states.push_back(app::State{.id = 2, .name = QStringLiteral("Success")});
        withInvoke.states.push_back(app::State{.id = 3, .name = QStringLiteral("Failure")});
        withInvoke.transitions.push_back(
            app::Transition{.id = 4, .from = 1, .to = 2, .event = QStringLiteral("done.invoke.fetchUserActor")});
        withInvoke.transitions.push_back(
            app::Transition{.id = 5, .from = 1, .to = 3, .event = QStringLiteral("error.platform.fetchUserActor")});
        withInvoke.initialStateId = 1;
        withInvoke.nextId = 6;

        const QString path = QDir::temp().filePath(QStringLiteral("ordo-state-designer-invoke-smoke.sdm"));
        QFile::remove(path);
        QString ioError;
        if (!app::saveMachine(withInvoke, path, &ioError)) {
            std::fprintf(stderr, "FAIL: saveMachine failed for the invoke .sdm round trip: %s\n",
                         qUtf8Printable(ioError));
            return 1;
        }
        app::Machine reloaded;
        if (!app::loadMachine(path, &reloaded, &ioError)) {
            std::fprintf(stderr, "FAIL: loadMachine failed for the invoke .sdm round trip: %s\n",
                         qUtf8Printable(ioError));
            return 1;
        }
        if (!(reloaded == withInvoke)) {
            std::fprintf(stderr,
                          "FAIL: the reloaded machine did not deep-equal the saved one (invoke .sdm round trip)\n");
            return 1;
        }

        // ---- 8. back-compat: a hand-built v8-shaped fixture (no "invokeSrc"/
        // "invokeId" keys) loads an empty invoke declaration --------------------
        QJsonObject v8Shaped;
        v8Shaped[QStringLiteral("formatVersion")] = 8;
        v8Shaped[QStringLiteral("name")] = QStringLiteral("PreInvoke");
        v8Shaped[QStringLiteral("nextId")] = 2;
        v8Shaped[QStringLiteral("initialStateId")] = 1;
        QJsonArray v8States;
        QJsonObject v8State;
        v8State[QStringLiteral("id")] = 1;
        v8State[QStringLiteral("name")] = QStringLiteral("Solo");
        v8State[QStringLiteral("kind")] = QStringLiteral("Normal");
        // No "invokeSrc"/"invokeId"/"invokeOutputType" keys at all.
        v8States.append(v8State);
        v8Shaped[QStringLiteral("states")] = v8States;
        v8Shaped[QStringLiteral("notes")] = QJsonArray();
        const app::Machine backCompat = app::machineFromJson(v8Shaped);
        if (backCompat.states.size() != 1 || !backCompat.states[0].invokeSrc.isEmpty() ||
            !backCompat.states[0].invokeId.isEmpty() ||
            backCompat.states[0].invokeOutputType != app::ContextType::Int) {
            std::fprintf(stderr,
                          "FAIL: a v8-shaped file with no invokeSrc/invokeId/invokeOutputType keys did not load an "
                          "empty invoke declaration (invokeOutputType should default to Int)\n");
            return 1;
        }

        // ---- 9. back-compat: a v9 file with "invokeSrc" but no "invokeOutputType";
        // the absent-key default must cover it like the v8 case above ----------
        QJsonObject v9PreTask5;
        v9PreTask5[QStringLiteral("formatVersion")] = 9;
        v9PreTask5[QStringLiteral("name")] = QStringLiteral("PreOutputType");
        v9PreTask5[QStringLiteral("nextId")] = 2;
        v9PreTask5[QStringLiteral("initialStateId")] = 1;
        QJsonArray v9States;
        QJsonObject v9State;
        v9State[QStringLiteral("id")] = 1;
        v9State[QStringLiteral("name")] = QStringLiteral("Loading");
        v9State[QStringLiteral("kind")] = QStringLiteral("Normal");
        v9State[QStringLiteral("invokeSrc")] = QStringLiteral("fetchUser");
        v9State[QStringLiteral("invokeId")] = QString();
        // No "invokeOutputType" key.
        v9States.append(v9State);
        v9PreTask5[QStringLiteral("states")] = v9States;
        v9PreTask5[QStringLiteral("notes")] = QJsonArray();
        const app::Machine preTask5 = app::machineFromJson(v9PreTask5);
        if (preTask5.states.size() != 1 || preTask5.states[0].invokeSrc != QStringLiteral("fetchUser") ||
            preTask5.states[0].invokeOutputType != app::ContextType::Int) {
            std::fprintf(stderr,
                          "FAIL: a v9 file with invokeSrc set and no invokeOutputType key did not load "
                          "invokeOutputType as the Int default\n");
            return 1;
        }
    }

    // ---- 10. Multi-invoke: SetInvocationsRequested, multi-service lifecycle,
    // rename cascade, undo/redo, and .sdm serialization -----------------------
    {
        const app::Machine beforeMulti = doc->machine();
        const QVector<app::Invocation> invs = {
            app::Invocation{.src = QStringLiteral("serviceA"), .id = QStringLiteral("actorA"), .outputType = app::ContextType::Int},
            app::Invocation{.src = QStringLiteral("serviceB"), .id = QStringLiteral("actorB"), .outputType = app::ContextType::String},
        };
        kernel.send(app::events::SetInvocationsRequested{.stateId = kStateB, .invocations = invs});
        const app::State* stateB = doc->findState(kStateB);
        if (!stateB || stateB->invocations.size() != 2 || stateB->invokeSrc != QStringLiteral("serviceA") ||
            stateB->invokeId != QStringLiteral("actorA") || stateB->invokeOutputType != app::ContextType::Int) {
            std::fprintf(stderr, "FAIL: SetInvocationsRequested did not populate invocations or sync legacy fields\n");
            return 1;
        }
        if (stateB->effectiveInvocations().size() != 2) {
            std::fprintf(stderr, "FAIL: effectiveInvocations() did not return 2 invocations\n");
            return 1;
        }

        kernel.send(app::events::UndoRequested{});
        if (!(doc->machine() == beforeMulti)) {
            std::fprintf(stderr, "FAIL: undo of SetInvocationsRequested did not restore machine\n");
            return 1;
        }

        kernel.send(app::events::RedoRequested{});
        if (doc->findState(kStateB)->invocations.size() != 2) {
            std::fprintf(stderr, "FAIL: redo of SetInvocationsRequested did not restore 2 invocations\n");
            return 1;
        }

        // Serialization round trip of multi-invoke machine
        const QString multiPath = QDir::temp().filePath(QStringLiteral("ordo-state-designer-multi-invoke-smoke.sdm"));
        QFile::remove(multiPath);
        if (!app::saveMachine(doc->machine(), multiPath)) {
            std::fprintf(stderr, "FAIL: saving multi-invoke machine failed\n");
            return 1;
        }
        app::Machine loadedMulti;
        if (!app::loadMachine(multiPath, &loadedMulti)) {
            std::fprintf(stderr, "FAIL: loading multi-invoke machine failed\n");
            return 1;
        }
        QFile::remove(multiPath);
        if (!(loadedMulti == doc->machine())) {
            std::fprintf(stderr, "FAIL: loaded multi-invoke machine != saved machine\n");
            return 1;
        }
    }

    removeSimCommands(kernel);
    removeUndoPhaseCommands(kernel);

    std::printf(
        "PASS: state-designer invoke declaration authoring (field-setter vocabulary + rename cascade + "
        "invokeOutputType field-setter + delete cascade + .sdm v9 round trip + v8/v9-pre-task-5 back-compat)\n");
    return 0;
}

// SCXML §3.12 / XState v5 event descriptor matching smoke.
static int runEventDescriptorSmoke() {
    // 1. Exact match
    if (!app::eventMatches(QStringLiteral("mouse.click"), QStringLiteral("mouse.click")) ||
        app::eventMatches(QStringLiteral("mouse.click"), QStringLiteral("mouse.move")) ||
        !app::eventMatches(QStringLiteral("Start"), QStringLiteral("Start")) ||
        app::eventMatches(QStringLiteral("Start"), QStringLiteral("Stop"))) {
        std::fprintf(stderr, "FAIL: event descriptor smoke: exact match failed\n");
        return 1;
    }

    // 2. Empty inputs
    if (app::eventMatches(QString(), QStringLiteral("Start")) ||
        app::eventMatches(QStringLiteral("Start"), QString()) ||
        app::eventMatches(QString(), QString())) {
        std::fprintf(stderr, "FAIL: event descriptor smoke: empty inputs should not match\n");
        return 1;
    }

    // 3. Universal wildcard '*'
    if (!app::eventMatches(QStringLiteral("*"), QStringLiteral("anyEvent")) ||
        !app::eventMatches(QStringLiteral("*"), QStringLiteral("mouse.click")) ||
        !app::eventMatches(QStringLiteral("*"), QStringLiteral("a.b.c.d")) ||
        app::eventMatches(QStringLiteral("*"), QString())) {
        std::fprintf(stderr, "FAIL: event descriptor smoke: universal wildcard '*' failed\n");
        return 1;
    }

    // 4. Prefix wildcard 'prefix.*'
    if (!app::eventMatches(QStringLiteral("mouse.*"), QStringLiteral("mouse.click")) ||
        !app::eventMatches(QStringLiteral("mouse.*"), QStringLiteral("mouse.down.left")) ||
        !app::eventMatches(QStringLiteral("mouse.*"), QStringLiteral("mouse")) ||
        app::eventMatches(QStringLiteral("mouse.*"), QStringLiteral("mousepad.click")) ||
        app::eventMatches(QStringLiteral("mouse.*"), QStringLiteral("keyboard.press"))) {
        std::fprintf(stderr, "FAIL: event descriptor smoke: prefix wildcard 'mouse.*' failed\n");
        return 1;
    }

    // 5. Multi-segment prefix wildcard 'a.b.*'
    if (!app::eventMatches(QStringLiteral("auth.token.*"), QStringLiteral("auth.token.refresh")) ||
        !app::eventMatches(QStringLiteral("auth.token.*"), QStringLiteral("auth.token.expired.v2")) ||
        !app::eventMatches(QStringLiteral("auth.token.*"), QStringLiteral("auth.token")) ||
        app::eventMatches(QStringLiteral("auth.token.*"), QStringLiteral("auth.user.login")) ||
        app::eventMatches(QStringLiteral("auth.token.*"), QStringLiteral("auth.tokens.all"))) {
        std::fprintf(stderr, "FAIL: event descriptor smoke: multi-segment prefix wildcard failed\n");
        return 1;
    }

    // 6. isWildcardEventDescriptor
    if (!app::isWildcardEventDescriptor(QStringLiteral("*")) ||
        !app::isWildcardEventDescriptor(QStringLiteral("mouse.*")) ||
        !app::isWildcardEventDescriptor(QStringLiteral("auth.token.*")) ||
        app::isWildcardEventDescriptor(QStringLiteral("mouse.click")) ||
        app::isWildcardEventDescriptor(QStringLiteral("Start")) ||
        app::isWildcardEventDescriptor(QString())) {
        std::fprintf(stderr, "FAIL: event descriptor smoke: isWildcardEventDescriptor failed\n");
        return 1;
    }

    // 7. eventDescriptorSpecificity
    const int specExact = app::eventDescriptorSpecificity(QStringLiteral("mouse.click"));
    const int specMultiPrefix = app::eventDescriptorSpecificity(QStringLiteral("auth.token.*"));
    const int specPrefix = app::eventDescriptorSpecificity(QStringLiteral("auth.*"));
    const int specUniversal = app::eventDescriptorSpecificity(QStringLiteral("*"));
    const int specEmpty = app::eventDescriptorSpecificity(QString());

    if (specExact <= specMultiPrefix ||
        specMultiPrefix <= specPrefix ||
        specPrefix <= specUniversal ||
        specUniversal <= specEmpty ||
        specEmpty != 0 ||
        specUniversal != 1) {
        std::fprintf(stderr, "FAIL: event descriptor smoke: specificity ordering failed (%d > %d > %d > %d > %d)\n",
                     specExact, specMultiPrefix, specPrefix, specUniversal, specEmpty);
        return 1;
    }

    std::printf("PASS: state-designer event descriptor matching smoke (exact + prefix.* + universal * + specificity ordering)\n");
    return 0;
}

// Struct Context & C++ Type Binding model and formatVersion 10 serialization smoke.
static int runStructContextModelSmoke() {
    app::Machine machine;
    machine.name = QStringLiteral("CanDecoder");
    machine.nextId = 10;
    machine.initialStateId = 1;
    machine.externalHeaders = {QStringLiteral("can_types.h"), QStringLiteral("custom_types.h")};

    app::StructDefinition canMsg;
    canMsg.id = 2;
    canMsg.name = QStringLiteral("CanMessage");
    canMsg.external = true;
    canMsg.headerPath = QStringLiteral("can_types.h");
    canMsg.fields = {
        app::StructField{.name = QStringLiteral("id"), .type = app::FieldType::Int, .initialValue = QStringLiteral("0")},
        app::StructField{.name = QStringLiteral("dlc"), .type = app::FieldType::Int, .initialValue = QStringLiteral("8")},
        app::StructField{.name = QStringLiteral("data"), .type = app::FieldType::Int, .isArray = true, .arraySize = 8}
    };
    machine.types.push_back(canMsg);

    app::StructDefinition pointDef;
    pointDef.id = 3;
    pointDef.name = QStringLiteral("Point");
    pointDef.external = false;
    pointDef.fields = {
        app::StructField{.name = QStringLiteral("x"), .type = app::FieldType::Double, .initialValue = QStringLiteral("0.0")},
        app::StructField{.name = QStringLiteral("y"), .type = app::FieldType::Double, .initialValue = QStringLiteral("0.0")}
    };
    machine.types.push_back(pointDef);

    app::State state;
    state.id = 1;
    state.name = QStringLiteral("Idle");
    machine.states.push_back(state);

    app::Transition trans;
    trans.id = 4;
    trans.from = 1;
    trans.to = 1;
    trans.event = QStringLiteral("rxFrame");
    trans.payloadType = QStringLiteral("CanMessage");
    machine.transitions.push_back(trans);

    app::ContextVariable ctxVar;
    ctxVar.id = 5;
    ctxVar.name = QStringLiteral("origin");
    ctxVar.type = app::ContextType::Object;
    ctxVar.customTypeName = QStringLiteral("Point");
    ctxVar.initialValue = QStringLiteral("{}");
    machine.context.push_back(ctxVar);

    // 1. Round-trip through machineToJson -> machineFromJson
    const QJsonObject json = app::machineToJson(machine);
    if (json.value(QStringLiteral("formatVersion")).toInt() != 10) {
        std::fprintf(stderr, "FAIL: struct context model smoke: formatVersion != 10\n");
        return 1;
    }
    if (!json.contains(QStringLiteral("types")) || !json.contains(QStringLiteral("externalHeaders"))) {
        std::fprintf(stderr, "FAIL: struct context model smoke: missing types or externalHeaders in JSON\n");
        return 1;
    }
    const app::Machine reloaded = app::machineFromJson(json);
    if (!(reloaded == machine)) {
        std::fprintf(stderr, "FAIL: struct context model smoke: machineFromJson did not equal original machine\n");
        return 1;
    }

    // 2. File IO save & load round-trip
    const QString tempPath = QStringLiteral("temp_m7_struct_test.sdm");
    QString ioError;
    if (!app::saveMachine(machine, tempPath, &ioError)) {
        std::fprintf(stderr, "FAIL: struct context model smoke: saveMachine failed: %s\n", qUtf8Printable(ioError));
        return 1;
    }
    app::Machine fromFile;
    if (!app::loadMachine(tempPath, &fromFile, &ioError)) {
        std::fprintf(stderr, "FAIL: struct context model smoke: loadMachine failed: %s\n", qUtf8Printable(ioError));
        QFile::remove(tempPath);
        return 1;
    }
    QFile::remove(tempPath);
    if (!(fromFile == machine)) {
        std::fprintf(stderr, "FAIL: struct context model smoke: loadMachine did not equal original machine\n");
        return 1;
    }

    // 3. Back-compat: formatVersion 9 JSON without types or payloadType
    QJsonObject v9Json;
    v9Json[QStringLiteral("formatVersion")] = 9;
    v9Json[QStringLiteral("name")] = QStringLiteral("LegacyV9");
    v9Json[QStringLiteral("nextId")] = 2;
    v9Json[QStringLiteral("initialStateId")] = 1;
    QJsonArray v9States;
    QJsonObject v9State;
    v9State[QStringLiteral("id")] = 1;
    v9State[QStringLiteral("name")] = QStringLiteral("Init");
    v9State[QStringLiteral("kind")] = QStringLiteral("Normal");
    v9States.append(v9State);
    v9Json[QStringLiteral("states")] = v9States;
    const app::Machine legacy = app::machineFromJson(v9Json);
    if (!legacy.types.isEmpty() || !legacy.externalHeaders.isEmpty()) {
        std::fprintf(stderr, "FAIL: struct context model smoke: legacy v9 file loaded non-empty types/headers\n");
        return 1;
    }

    // 4. Clean omission: machine with empty types and empty externalHeaders does not emit types or externalHeaders keys
    app::Machine noTypesMachine;
    noTypesMachine.name = QStringLiteral("NoTypes");
    noTypesMachine.states.push_back(state);
    noTypesMachine.initialStateId = 1;
    const QJsonObject noTypesJson = app::machineToJson(noTypesMachine);
    if (noTypesJson.contains(QStringLiteral("types")) || noTypesJson.contains(QStringLiteral("externalHeaders"))) {
        std::fprintf(stderr, "FAIL: struct context model smoke: empty types/headers machine emitted types/headers keys\n");
        return 1;
    }

    std::printf("PASS: state-designer struct context model smoke (types + externalHeaders + payloadType + formatVersion 10 round-trip + v9 back-compat)\n");
    return 0;
}

// Unknown enum strings fail the .sdm load: one fixture per converter family,
// hand-built JSON on disk (machineToJson() can only emit valid strings), loaded
// through app::loadMachine(), the real File > Open entry point. Each fixture but
// the last is otherwise valid, so the bad value is the only unknown one.
static int runEnumFailFastSmoke() {
    const QString dir = QStringLiteral("temp/code/enum-fail-fast");
    QDir().mkpath(dir);

    // Writes obj as-is (bypassing machineToJson()) and loads it via loadMachine().
    const auto writeAndLoad = [&dir](const QString& fileName, const QJsonObject& obj, app::Machine* machine,
                                      QString* error) -> bool {
        const QString path = dir + QStringLiteral("/") + fileName;
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::fprintf(stderr, "FAIL: enum fail-fast smoke: could not write fixture %s\n", qUtf8Printable(path));
            return false;
        }
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        file.close();
        const bool ok = app::loadMachine(path, machine, error);
        QFile::remove(path);
        return ok;
    };

    // Asserts loadMachine() failed and the error names the element and the bad value.
    const auto expectFail = [](const char* label, bool loaded, const QString& error, const QString& mustContainElement,
                                const QString& badValue) -> bool {
        if (loaded) {
            std::fprintf(stderr, "FAIL: enum fail-fast smoke (%s): loadMachine unexpectedly succeeded\n", label);
            return false;
        }
        if (!error.contains(mustContainElement) || !error.contains(badValue)) {
            std::fprintf(stderr,
                          "FAIL: enum fail-fast smoke (%s): error text missing element ('%s') or value ('%s'): %s\n",
                          label, qUtf8Printable(mustContainElement), qUtf8Printable(badValue), qUtf8Printable(error));
            return false;
        }
        return true;
    };

    // 1. Unknown state kind.
    {
        QJsonObject state;
        state[QStringLiteral("id")] = 1;
        state[QStringLiteral("name")] = QStringLiteral("Weird");
        state[QStringLiteral("kind")] = QStringLiteral("Bogus");
        QJsonObject obj;
        obj[QStringLiteral("formatVersion")] = 10;
        obj[QStringLiteral("name")] = QStringLiteral("EnumFailKind");
        obj[QStringLiteral("nextId")] = 2;
        obj[QStringLiteral("initialStateId")] = 1;
        obj[QStringLiteral("states")] = QJsonArray{state};
        app::Machine machine;
        QString error;
        const bool loaded = writeAndLoad(QStringLiteral("kind.sdm"), obj, &machine, &error);
        if (!expectFail("state kind", loaded, error, QStringLiteral("Weird"), QStringLiteral("Bogus"))) {
            return 1;
        }
    }

    // 2. Unknown state color.
    {
        QJsonObject state;
        state[QStringLiteral("id")] = 1;
        state[QStringLiteral("name")] = QStringLiteral("Weird");
        state[QStringLiteral("kind")] = QStringLiteral("Normal");
        state[QStringLiteral("color")] = QStringLiteral("Teal");
        QJsonObject obj;
        obj[QStringLiteral("formatVersion")] = 10;
        obj[QStringLiteral("name")] = QStringLiteral("EnumFailStateColor");
        obj[QStringLiteral("nextId")] = 2;
        obj[QStringLiteral("initialStateId")] = 1;
        obj[QStringLiteral("states")] = QJsonArray{state};
        app::Machine machine;
        QString error;
        const bool loaded = writeAndLoad(QStringLiteral("state-color.sdm"), obj, &machine, &error);
        if (!expectFail("state color", loaded, error, QStringLiteral("Weird"), QStringLiteral("Teal"))) {
            return 1;
        }
    }

    // 3. Unknown transition color.
    {
        QJsonObject stateA;
        stateA[QStringLiteral("id")] = 1;
        stateA[QStringLiteral("name")] = QStringLiteral("A");
        stateA[QStringLiteral("kind")] = QStringLiteral("Normal");
        QJsonObject stateB;
        stateB[QStringLiteral("id")] = 2;
        stateB[QStringLiteral("name")] = QStringLiteral("B");
        stateB[QStringLiteral("kind")] = QStringLiteral("Normal");
        QJsonObject transition;
        transition[QStringLiteral("id")] = 10;
        transition[QStringLiteral("from")] = 1;
        transition[QStringLiteral("to")] = 2;
        transition[QStringLiteral("event")] = QStringLiteral("go");
        transition[QStringLiteral("color")] = QStringLiteral("Mauve");
        QJsonObject obj;
        obj[QStringLiteral("formatVersion")] = 10;
        obj[QStringLiteral("name")] = QStringLiteral("EnumFailTransitionColor");
        obj[QStringLiteral("nextId")] = 11;
        obj[QStringLiteral("initialStateId")] = 1;
        obj[QStringLiteral("states")] = QJsonArray{stateA, stateB};
        obj[QStringLiteral("transitions")] = QJsonArray{transition};
        app::Machine machine;
        QString error;
        const bool loaded = writeAndLoad(QStringLiteral("transition-color.sdm"), obj, &machine, &error);
        if (!expectFail("transition color", loaded, error, QStringLiteral("transition 10"), QStringLiteral("Mauve"))) {
            return 1;
        }
    }

    // 4. Unknown note color.
    {
        QJsonObject state;
        state[QStringLiteral("id")] = 1;
        state[QStringLiteral("name")] = QStringLiteral("A");
        state[QStringLiteral("kind")] = QStringLiteral("Normal");
        QJsonObject note;
        note[QStringLiteral("id")] = 5;
        note[QStringLiteral("x")] = 0;
        note[QStringLiteral("y")] = 0;
        note[QStringLiteral("text")] = QStringLiteral("hi");
        note[QStringLiteral("color")] = QStringLiteral("Beige");
        QJsonObject obj;
        obj[QStringLiteral("formatVersion")] = 10;
        obj[QStringLiteral("name")] = QStringLiteral("EnumFailNoteColor");
        obj[QStringLiteral("nextId")] = 6;
        obj[QStringLiteral("initialStateId")] = 1;
        obj[QStringLiteral("states")] = QJsonArray{state};
        obj[QStringLiteral("notes")] = QJsonArray{note};
        app::Machine machine;
        QString error;
        const bool loaded = writeAndLoad(QStringLiteral("note-color.sdm"), obj, &machine, &error);
        if (!expectFail("note color", loaded, error, QStringLiteral("note 5"), QStringLiteral("Beige"))) {
            return 1;
        }
    }

    // 5. Unknown context variable type.
    {
        QJsonObject state;
        state[QStringLiteral("id")] = 1;
        state[QStringLiteral("name")] = QStringLiteral("A");
        state[QStringLiteral("kind")] = QStringLiteral("Normal");
        QJsonObject ctxVar;
        ctxVar[QStringLiteral("id")] = 7;
        ctxVar[QStringLiteral("name")] = QStringLiteral("retries");
        ctxVar[QStringLiteral("type")] = QStringLiteral("Integer");
        ctxVar[QStringLiteral("initialValue")] = QStringLiteral("0");
        QJsonObject obj;
        obj[QStringLiteral("formatVersion")] = 10;
        obj[QStringLiteral("name")] = QStringLiteral("EnumFailContextType");
        obj[QStringLiteral("nextId")] = 8;
        obj[QStringLiteral("initialStateId")] = 1;
        obj[QStringLiteral("states")] = QJsonArray{state};
        obj[QStringLiteral("context")] = QJsonArray{ctxVar};
        app::Machine machine;
        QString error;
        const bool loaded = writeAndLoad(QStringLiteral("context-type.sdm"), obj, &machine, &error);
        if (!expectFail("context variable type", loaded, error, QStringLiteral("retries"), QStringLiteral("Integer"))) {
            return 1;
        }
    }

    // 6. Unknown state invokeOutputType.
    {
        QJsonObject state;
        state[QStringLiteral("id")] = 1;
        state[QStringLiteral("name")] = QStringLiteral("Loader");
        state[QStringLiteral("kind")] = QStringLiteral("Normal");
        state[QStringLiteral("invokeSrc")] = QStringLiteral("svc");
        state[QStringLiteral("invokeOutputType")] = QStringLiteral("Number");
        QJsonObject obj;
        obj[QStringLiteral("formatVersion")] = 10;
        obj[QStringLiteral("name")] = QStringLiteral("EnumFailInvokeOutputType");
        obj[QStringLiteral("nextId")] = 2;
        obj[QStringLiteral("initialStateId")] = 1;
        obj[QStringLiteral("states")] = QJsonArray{state};
        app::Machine machine;
        QString error;
        const bool loaded = writeAndLoad(QStringLiteral("invoke-output-type.sdm"), obj, &machine, &error);
        if (!expectFail("invokeOutputType", loaded, error, QStringLiteral("Loader"), QStringLiteral("Number"))) {
            return 1;
        }
    }

    // 7. Unknown invocations[].outputType.
    {
        QJsonObject invocation;
        invocation[QStringLiteral("src")] = QStringLiteral("svc");
        invocation[QStringLiteral("id")] = QStringLiteral("actorA");
        invocation[QStringLiteral("outputType")] = QStringLiteral("Number");
        QJsonObject state;
        state[QStringLiteral("id")] = 1;
        state[QStringLiteral("name")] = QStringLiteral("A");
        state[QStringLiteral("kind")] = QStringLiteral("Normal");
        state[QStringLiteral("invocations")] = QJsonArray{invocation};
        QJsonObject obj;
        obj[QStringLiteral("formatVersion")] = 10;
        obj[QStringLiteral("name")] = QStringLiteral("EnumFailInvocationOutputType");
        obj[QStringLiteral("nextId")] = 2;
        obj[QStringLiteral("initialStateId")] = 1;
        obj[QStringLiteral("states")] = QJsonArray{state};
        app::Machine machine;
        QString error;
        const bool loaded = writeAndLoad(QStringLiteral("invocation-output-type.sdm"), obj, &machine, &error);
        if (!expectFail("invocation outputType", loaded, error, QStringLiteral("actorA"), QStringLiteral("Number"))) {
            return 1;
        }
    }

    // 8. Unknown struct field type.
    {
        QJsonObject field;
        field[QStringLiteral("name")] = QStringLiteral("dlc");
        field[QStringLiteral("type")] = QStringLiteral("Byte");
        QJsonObject structDef;
        structDef[QStringLiteral("id")] = 9;
        structDef[QStringLiteral("name")] = QStringLiteral("Frame");
        structDef[QStringLiteral("external")] = false;
        structDef[QStringLiteral("fields")] = QJsonArray{field};
        QJsonObject state;
        state[QStringLiteral("id")] = 1;
        state[QStringLiteral("name")] = QStringLiteral("A");
        state[QStringLiteral("kind")] = QStringLiteral("Normal");
        QJsonObject obj;
        obj[QStringLiteral("formatVersion")] = 10;
        obj[QStringLiteral("name")] = QStringLiteral("EnumFailFieldType");
        obj[QStringLiteral("nextId")] = 10;
        obj[QStringLiteral("initialStateId")] = 1;
        obj[QStringLiteral("states")] = QJsonArray{state};
        obj[QStringLiteral("types")] = QJsonArray{structDef};
        app::Machine machine;
        QString error;
        const bool loaded = writeAndLoad(QStringLiteral("field-type.sdm"), obj, &machine, &error);
        if (!expectFail("struct field type", loaded, error, QStringLiteral("Frame"), QStringLiteral("Byte")) ||
            !error.contains(QStringLiteral("dlc"))) {
            std::fprintf(stderr, "FAIL: enum fail-fast smoke (struct field type): error missing field name 'dlc': %s\n",
                         qUtf8Printable(error));
            return 1;
        }
    }

    // 9. Positive control: a v1-shaped file (no formatVersion, no color/context/
    // invokeOutputType/notes/types keys) whose state kind is the legacy "Initial"
    // still loads; absent keys keep their defaults.
    {
        QJsonObject startState;
        startState[QStringLiteral("id")] = 1;
        startState[QStringLiteral("name")] = QStringLiteral("Start");
        startState[QStringLiteral("kind")] = QStringLiteral("Initial");
        QJsonObject nextState;
        nextState[QStringLiteral("id")] = 2;
        nextState[QStringLiteral("name")] = QStringLiteral("Next");
        nextState[QStringLiteral("kind")] = QStringLiteral("Normal");
        QJsonObject obj;
        // No "formatVersion" key -> v1. No "color"/"invokeOutputType"/
        // "invokeSrc"/"invokeId"/"notes"/"context"/"types" keys anywhere.
        obj[QStringLiteral("name")] = QStringLiteral("LegacyInitial");
        obj[QStringLiteral("nextId")] = 3;
        obj[QStringLiteral("states")] = QJsonArray{startState, nextState};
        app::Machine machine;
        QString error;
        const bool loaded = writeAndLoad(QStringLiteral("legacy-initial.sdm"), obj, &machine, &error);
        if (!loaded) {
            std::fprintf(stderr, "FAIL: enum fail-fast smoke (legacy Initial): loadMachine failed unexpectedly: %s\n",
                         qUtf8Printable(error));
            return 1;
        }
        if (!error.isEmpty()) {
            std::fprintf(stderr, "FAIL: enum fail-fast smoke (legacy Initial): error was non-empty on success: %s\n",
                         qUtf8Printable(error));
            return 1;
        }
        if (machine.initialStateId != 1 || machine.states.size() != 2 ||
            machine.states[0].kind != app::StateKind::Normal || machine.states[0].color != app::ElementColor::Default ||
            machine.states[0].invokeOutputType != app::ContextType::Int || !machine.notes.isEmpty() ||
            !machine.context.isEmpty() || !machine.types.isEmpty()) {
            std::fprintf(stderr,
                          "FAIL: enum fail-fast smoke (legacy Initial): absent-key defaults or Initial promotion "
                          "did not load as expected\n");
            return 1;
        }
    }

    QDir(dir).removeRecursively();
    std::printf("PASS: state-designer enum fail-fast smoke (unknown kind/color/context-type/field-type "
                "fail loadMachine; absent keys + legacy \"Initial\" still load)\n");
    return 0;
}

// Headless edit drive (add/rename/move/wire/policy/cascade) + .sdm/.sdp
// round-trip equality against a bare Kernel. Observation subscribes straight
// on kernel.dispatcher() with a local variable's address as the owner cookie.
int runSmoke() {
    // Redirect before any later phase constructs a MainWindow: a --smoke run
    // must never touch the real recent-projects list.
    app::RecentProjects::setDefaultPathOverride(
        QDir::currentPath() + QStringLiteral("/temp/code/open-recent/recent-smoke-isolation.json"));

    if (runEventDescriptorSmoke() != 0) {
        return 1;
    }
    if (runStructContextModelSmoke() != 0) {
        return 1;
    }
    if (runEnumFailFastSmoke() != 0) {
        return 1;
    }
    if (runSettingsSmoke() != 0) {
        return 1;
    }
    if (runRecentProjectsSmoke() != 0) {
        return 1;
    }
    if (runIoSmoke() != 0) {
        return 1;
    }
    if (runScxmlSmoke() != 0) {
        return 1;
    }
    if (runMcpSmoke() != 0) {
        return 1;
    }

    ordo::core::Kernel kernel;
    kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
    kernel.registerAgent(std::make_shared<app::SimulationAgent>());
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
    if (!doc) {
        std::fprintf(stderr, "FAIL: MachineDocAgent did not register under kName\n");
        return 1;
    }
    if (!sim) {
        std::fprintf(stderr, "FAIL: SimulationAgent did not register under kName\n");
        return 1;
    }

    // Manual mode: only SetModeRequested's policy gate is exercised, but
    // registerSimCommands() needs a SimClock.
    app::SimClock clock;
    clock.setManualMode(true);
    registerEditCommands(kernel);
    registerSimCommands(kernel, clock);

    // ---- 1. Add 3 states (initial-marked/Normal/Final) ----------------------
    kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});     // id 1
    kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});   // id 2
    kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});   // id 3
    if (doc->machine().states.size() != 3) {
        std::fprintf(stderr, "FAIL: after 3 AddStateRequested states.size()=%d (want 3)\n",
                     static_cast<int>(doc->machine().states.size()));
        return 1;
    }
    if (!doc->findState(1) || !doc->findState(2) || !doc->findState(3)) {
        std::fprintf(stderr, "FAIL: states 1, 2, 3 not all present after 3 adds\n");
        return 1;
    }

    kernel.send(app::events::SetInitialStateRequested{.id = 1});
    kernel.send(app::events::SetStateKindRequested{.id = 3, .kind = app::StateKind::Final});
    if (doc->machine().initialStateId != 1 || doc->findState(1)->kind != app::StateKind::Normal ||
        doc->findState(2)->kind != app::StateKind::Normal || doc->findState(3)->kind != app::StateKind::Final) {
        std::fprintf(stderr,
                     "FAIL: initialStateId/kinds after SetInitialState+SetStateKind are not 1/Normal/Normal/Final\n");
        return 1;
    }

    // ---- rename one, move one ------------------------------------------------
    kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Working")});
    if (doc->findState(2)->name != QStringLiteral("Working")) {
        std::fprintf(stderr, "FAIL: RenameStateRequested did not rename state 2 (name='%s')\n",
                     qUtf8Printable(doc->findState(2)->name));
        return 1;
    }

    kernel.send(app::events::MoveStateRequested{.id = 1, .pos = QPointF(5, 5)});
    if (doc->findState(1)->pos != QPointF(5, 5)) {
        std::fprintf(stderr, "FAIL: MoveStateRequested did not move state 1\n");
        return 1;
    }

    // ---- 2 transitions with events, both leaving state 1 --------------------
    kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 4
    kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 3});  // id 5
    if (doc->machine().transitions.size() != 2 || !doc->findTransition(4) || !doc->findTransition(5)) {
        std::fprintf(stderr, "FAIL: after 2 AddTransitionRequested transitions.size()=%d (want 2, ids 4 and 5)\n",
                     static_cast<int>(doc->machine().transitions.size()));
        return 1;
    }

    kernel.send(app::events::SetTransitionEventRequested{.id = 4, .event = QStringLiteral("go")});
    kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("finish")});
    if (doc->findTransition(4)->event != QStringLiteral("go") ||
        doc->findTransition(5)->event != QStringLiteral("finish")) {
        std::fprintf(stderr, "FAIL: SetTransitionEventRequested did not set 'go'/'finish'\n");
        return 1;
    }

    // ---- guard/action/delay on one -------------------------------------------
    kernel.send(app::events::SetTransitionGuardRequested{.id = 4, .guard = QStringLiteral("isReady")});
    kernel.send(app::events::SetTransitionActionRequested{.id = 4, .action = QStringLiteral("logTransition")});
    kernel.send(app::events::SetTransitionDelayRequested{.id = 4, .delayMs = 250});
    if (doc->findTransition(4)->guard != QStringLiteral("isReady") ||
        doc->findTransition(4)->action != QStringLiteral("logTransition") ||
        doc->findTransition(4)->delayMs != 250) {
        std::fprintf(stderr, "FAIL: guard/action/delay were not all set on transition 4\n");
        return 1;
    }

    // ---- policy: a second UNGUARDED candidate on the same (source, event) is
    // ---- rejected ------------------------------------------------------------
    // Transitions 4 and 5 both leave state 1; transition 4 already owns "go".
    // Its guard is cleared first, since a guarded 4 would make the join legal
    // (see the guarded-group block below). A counting subscriber on
    // TransitionEventChanged proves the rejection publishes no fact.
    kernel.send(app::events::SetTransitionGuardRequested{.id = 4, .guard = QString()});
    if (!doc->findTransition(4)->guard.isEmpty()) {
        std::fprintf(stderr, "FAIL: clearing transition 4's guard (test setup) did not take\n");
        return 1;
    }
    int duplicatePolicyOwner = 0;
    int transitionEventChangedCount = 0;
    kernel.dispatcher().subscribe<app::events::TransitionEventChanged>(
        &duplicatePolicyOwner, [&transitionEventChangedCount](const app::events::TransitionEventChanged&) {
            ++transitionEventChangedCount;
        });
    kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("go")});
    if (transitionEventChangedCount != 0) {
        std::fprintf(stderr,
                     "FAIL: duplicate event 'go' on source state 1 fired TransitionEventChanged %d time(s) "
                     "(want 0)\n",
                     transitionEventChangedCount);
        return 1;
    }
    if (doc->findTransition(5)->event != QStringLiteral("finish")) {
        std::fprintf(stderr, "FAIL: rejected duplicate-event edit still changed transition 5's event to '%s'\n",
                     qUtf8Printable(doc->findTransition(5)->event));
        return 1;
    }
    kernel.dispatcher().unsubscribe(&duplicatePolicyOwner);

    // ---- policy: edits rejected while Simulate, accepted back in Design -----
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
    if (sim->mode() != app::events::Mode::Simulate) {
        std::fprintf(stderr, "FAIL: SetModeRequested{Simulate} did not flip SimulationAgent::mode()\n");
        return 1;
    }
    kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("ShouldNotApply")});
    if (doc->findState(2)->name != QStringLiteral("Working")) {
        std::fprintf(stderr, "FAIL: RenameStateRequested was not rejected while simulating (name='%s')\n",
                     qUtf8Printable(doc->findState(2)->name));
        return 1;
    }
    kernel.send(app::events::SetModeRequested{.mode = app::events::Mode::Design});
    if (sim->mode() != app::events::Mode::Design) {
        std::fprintf(stderr, "FAIL: SetModeRequested{Design} did not flip SimulationAgent::mode() back\n");
        return 1;
    }
    kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("WorkingRenamed")});
    if (doc->findState(2)->name != QStringLiteral("WorkingRenamed")) {
        std::fprintf(stderr, "FAIL: RenameStateRequested was not accepted again back in Design mode\n");
        return 1;
    }

    // ---- delete-state cascade removes its transitions ------------------------
    // State 3 has one incoming transition (id 5, "finish"); deleting it takes
    // that transition along and leaves state/transition 1/4 untouched.
    kernel.send(app::events::DeleteStateRequested{.id = 3});
    if (doc->findState(3) != nullptr || doc->findTransition(5) != nullptr) {
        std::fprintf(stderr, "FAIL: DeleteStateRequested{3} did not cascade-remove state 3 and transition 5\n");
        return 1;
    }
    if (doc->machine().states.size() != 2 || doc->machine().transitions.size() != 1 || !doc->findState(1) ||
        !doc->findState(2) || !doc->findTransition(4)) {
        std::fprintf(stderr,
                     "FAIL: after cascade delete states.size()=%d transitions.size()=%d (want 2, 1; states 1+2, "
                     "transition 4 intact)\n",
                     static_cast<int>(doc->machine().states.size()),
                     static_cast<int>(doc->machine().transitions.size()));
        return 1;
    }

    // ---- policy: guard-aware duplicate-(from, event) gate. A group may hold
    // several transitions with distinct guards, or guarded candidates plus one
    // trailing unguarded fallback, but never two or more unguarded members
    // (wouldCreateSecondUnguarded()). Uses a fresh pair (states 6/7) so the
    // cascade-delete counts above stay intact.
    kernel.send(app::events::AddStateRequested{.pos = QPointF(400, 400)});  // id 6
    kernel.send(app::events::AddStateRequested{.pos = QPointF(500, 400)});  // id 7
    kernel.send(app::events::AddTransitionRequested{.from = 6, .to = 7});   // id 8
    kernel.send(app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("Check")});
    kernel.send(app::events::SetTransitionGuardRequested{.id = 8, .guard = QStringLiteral("condA")});
    kernel.send(app::events::AddTransitionRequested{.from = 6, .to = 7});  // id 9
    kernel.send(app::events::SetTransitionEventRequested{.id = 9, .event = QStringLiteral("Check")});
    kernel.send(app::events::SetTransitionGuardRequested{.id = 9, .guard = QStringLiteral("condB")});
    // (a) two same-(from, event) transitions with distinct guards: neither refused.
    if (!doc->findTransition(8) || !doc->findTransition(9) ||
        doc->findTransition(8)->event != QStringLiteral("Check") ||
        doc->findTransition(9)->event != QStringLiteral("Check") ||
        doc->findTransition(8)->guard != QStringLiteral("condA") ||
        doc->findTransition(9)->guard != QStringLiteral("condB")) {
        std::fprintf(stderr,
                     "FAIL: two distinct-guard transitions sharing (from=6, event='Check') were not both authored\n");
        return 1;
    }

    // A trailing unguarded fallback (last in document order) is legal too.
    kernel.send(app::events::AddTransitionRequested{.from = 6, .to = 7});  // id 10
    kernel.send(app::events::SetTransitionEventRequested{.id = 10, .event = QStringLiteral("Check")});
    if (!doc->findTransition(10) || doc->findTransition(10)->event != QStringLiteral("Check") ||
        !doc->findTransition(10)->guard.isEmpty()) {
        std::fprintf(stderr,
                     "FAIL: the trailing unguarded fallback (id 10) on (from=6, event='Check') was not authored\n");
        return 1;
    }
    // (c), legal half: validate() raises no Error; the one unguarded row (id 10)
    // is last in document order.
    {
        bool foundShadowError = false;
        for (const app::Problem& problem : app::validate(doc->machine())) {
            foundShadowError = foundShadowError || (problem.severity == app::ProblemSeverity::Error &&
                                                      problem.text.contains(QStringLiteral("shadows transition")));
        }
        if (foundShadowError) {
            std::fprintf(stderr,
                         "FAIL: validate() flagged a legal guarded/guarded/unguarded-last group on (from=6, "
                         "event='Check')\n");
            return 1;
        }
    }

    // (b) a second unguarded member is refused at all three authoring sites,
    // each leaving the machine unchanged (id 10 is already the unguarded one).

    // -- event-join: a fresh blank transition joining the group.
    kernel.send(app::events::AddTransitionRequested{.from = 6, .to = 7});  // id 11
    int eventJoinOwner = 0;
    int eventChangedCount = 0;
    kernel.dispatcher().subscribe<app::events::TransitionEventChanged>(
        &eventJoinOwner, [&eventChangedCount](const app::events::TransitionEventChanged&) { ++eventChangedCount; });
    kernel.send(app::events::SetTransitionEventRequested{.id = 11, .event = QStringLiteral("Check")});
    if (eventChangedCount != 0 || !doc->findTransition(11)->event.isEmpty()) {
        std::fprintf(stderr,
                     "FAIL: event-join onto a group that already has an unguarded member (id 10) was not refused\n");
        return 1;
    }
    kernel.dispatcher().unsubscribe(&eventJoinOwner);

    // -- guard-clear: clearing id 9's guard would leave id 9 AND id 10 both
    // unguarded in the same group.
    int guardClearOwner = 0;
    int guardChangedCount = 0;
    kernel.dispatcher().subscribe<app::events::TransitionGuardChanged>(
        &guardClearOwner, [&guardChangedCount](const app::events::TransitionGuardChanged&) { ++guardChangedCount; });
    kernel.send(app::events::SetTransitionGuardRequested{.id = 9, .guard = QString()});
    if (guardChangedCount != 0 || doc->findTransition(9)->guard != QStringLiteral("condB")) {
        std::fprintf(stderr,
                     "FAIL: clearing transition 9's guard onto a group that already has an unguarded member (id 10) "
                     "was not refused\n");
        return 1;
    }
    kernel.dispatcher().unsubscribe(&guardClearOwner);

    // -- retarget-join: an unguarded transition elsewhere (id 12, source state 1)
    // retargeted into state 6's group.
    kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 12
    kernel.send(app::events::SetTransitionEventRequested{.id = 12, .event = QStringLiteral("Check")});
    if (!doc->findTransition(12) || doc->findTransition(12)->from != 1 ||
        doc->findTransition(12)->event != QStringLiteral("Check")) {
        std::fprintf(stderr, "FAIL: setup transition 12 (from=1, event='Check', unguarded) was not authored\n");
        return 1;
    }
    int retargetJoinOwner = 0;
    int retargetedCount = 0;
    kernel.dispatcher().subscribe<app::events::TransitionRetargeted>(
        &retargetJoinOwner, [&retargetedCount](const app::events::TransitionRetargeted&) { ++retargetedCount; });
    kernel.send(app::events::RetargetTransitionRequested{.id = 12, .from = 6, .to = 7});
    if (retargetedCount != 0 || doc->findTransition(12)->from != 1) {
        std::fprintf(stderr,
                     "FAIL: retargeting transition 12 into (from=6, event='Check') -- already holding an unguarded "
                     "member (id 10) -- was not refused\n");
        return 1;
    }
    kernel.dispatcher().unsubscribe(&retargetJoinOwner);

    // (c), Error half: validate() still flags the same group with its unguarded
    // row (id 10) moved to the front. The editor only appends, so the reorder
    // is done on a local copy.
    {
        app::Machine reordered = doc->machine();
        int index8 = -1;
        int index10 = -1;
        for (int i = 0; i < reordered.transitions.size(); ++i) {
            if (reordered.transitions[i].id == 8) {
                index8 = i;
            } else if (reordered.transitions[i].id == 10) {
                index10 = i;
            }
        }
        if (index8 < 0 || index10 < 0) {
            std::fprintf(stderr, "FAIL: could not locate transitions 8/10 to reorder for the check-5 Error proof\n");
            return 1;
        }
        const app::Transition temp = reordered.transitions[index8];
        reordered.transitions[index8] = reordered.transitions[index10];
        reordered.transitions[index10] = temp;
        bool found = false;
        for (const app::Problem& problem : app::validate(reordered)) {
            found = found || (problem.severity == app::ProblemSeverity::Error && problem.transitionId == 10 &&
                               problem.text.contains(QStringLiteral("shadows transition 9")));
        }
        if (!found) {
            std::fprintf(stderr,
                         "FAIL: validate() did not flag the unguarded, non-last transition 10 shadowing transition "
                         "9 once reordered to the front\n");
            return 1;
        }
    }

    // ---- a note survives the .sdm round trip below --------------------------
    const quint64 noteId = doc->machine().nextId;
    kernel.send(app::events::AddNoteRequested{.pos = QPointF(96.0, 240.0)});
    kernel.send(app::events::SetNoteTextRequested{.id = noteId, .text = QStringLiteral("remember this")});
    if (doc->machine().notes.size() != 1 || doc->findNote(noteId) == nullptr ||
        doc->findNote(noteId)->text != QStringLiteral("remember this")) {
        std::fprintf(stderr, "FAIL: AddNoteRequested/SetNoteTextRequested did not produce one live note\n");
        return 1;
    }

    // ---- .sdm round trip: save, load into a second kernel via restore() -----
    const QString machinePath = QDir::temp().filePath(QStringLiteral("ordo-state-designer-smoke.sdm"));
    QFile::remove(machinePath);  // a stale file from a killed prior run must not leak in

    QString ioError;
    if (!app::saveMachine(doc->machine(), machinePath, &ioError)) {
        std::fprintf(stderr, "FAIL: saveMachine failed: %s\n", qUtf8Printable(ioError));
        return 1;
    }

    ordo::core::Kernel kernel2;
    kernel2.registerAgent(std::make_shared<app::MachineDocAgent>());
    auto doc2 = kernel2.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (!doc2) {
        std::fprintf(stderr, "FAIL: second kernel's MachineDocAgent did not register under kName\n");
        return 1;
    }

    app::Machine loadedMachine;
    if (!app::loadMachine(machinePath, &loadedMachine, &ioError)) {
        std::fprintf(stderr, "FAIL: loadMachine failed: %s\n", qUtf8Printable(ioError));
        return 1;
    }
    doc2->restore(loadedMachine);
    if (!(doc2->machine() == doc->machine())) {
        std::fprintf(stderr, "FAIL: .sdm round trip did not reproduce the original Machine (deep equality failed)\n");
        return 1;
    }
    // Explicit note check, though the deep equality above already covers it.
    if (doc2->machine().notes.size() != 1 || doc2->findNote(noteId) == nullptr ||
        doc2->findNote(noteId)->text != QStringLiteral("remember this")) {
        std::fprintf(stderr, "FAIL: the note did not survive the .sdm save/load round trip\n");
        return 1;
    }

    // ---- format v2 vocabulary round trip ---------------------------------------
    {
        app::Machine machine;
        machine.name = QStringLiteral("V2Kinds");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        machine.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Parallel});
        machine.states.push_back(app::State{.id = 3, .name = QStringLiteral("C"), .kind = app::StateKind::History});
        machine.transitions.push_back(app::Transition{
            .id = 4, .from = 1, .to = 1, .event = QStringLiteral("Self"), .labelOffset = QPointF(24.0, -16.0)});
        machine.transitions.push_back(
            app::Transition{.id = 5, .from = 1, .to = 0, .event = QStringLiteral("Internal")});
        machine.nextId = 6;
        machine.initialStateId = 1;
        const app::Machine reloaded = app::machineFromJson(app::machineToJson(machine));
        if (!(reloaded == machine)) {
            std::fprintf(stderr,
                         "FAIL: format-v2 kinds/initialStateId/self/targetless did not round-trip\n");
            return 1;
        }
    }

    // ---- format v4 vocabulary round trip (canvas notes) ---------------------
    {
        app::Machine machine;
        machine.name = QStringLiteral("V4Notes");
        machine.notes.push_back(app::Note{.id = 1, .pos = QPointF(48.0, -24.0), .text = QStringLiteral("hello")});
        machine.notes.push_back(app::Note{.id = 2, .pos = QPointF(-96.0, 120.0), .text = QString()});
        machine.nextId = 3;
        const app::Machine reloaded = app::machineFromJson(app::machineToJson(machine));
        if (!(reloaded == machine)) {
            std::fprintf(stderr, "FAIL: format-v4 notes did not round-trip\n");
            return 1;
        }
    }

    // ---- v3 legacy loader: a formatVersion-3 file (no "notes" key) loads
    // with an empty notes list --------------------------------------------------
    {
        QJsonObject v3;
        v3[QStringLiteral("formatVersion")] = 3;
        v3[QStringLiteral("name")] = QStringLiteral("PreNotes");
        v3[QStringLiteral("nextId")] = 1;
        // No "notes" key at all.
        const app::Machine legacy = app::machineFromJson(v3);
        if (!legacy.notes.isEmpty()) {
            std::fprintf(stderr, "FAIL: a v3 file with no \"notes\" key loaded %d note(s), want 0\n",
                         static_cast<int>(legacy.notes.size()));
            return 1;
        }
    }

    // ---- v1 legacy loader: kind "Initial" promotes to initialStateId ----------
    // Hand-built pre-formatVersion JSON (no "formatVersion"/"initialStateId",
    // start state marked the v1 way). The loader must promote the first Initial
    // in document order and read every promoted kind as Normal.
    {
        QJsonArray states;
        {
            QJsonObject s;
            s[QStringLiteral("id")] = 1;
            s[QStringLiteral("name")] = QStringLiteral("Off");
            s[QStringLiteral("kind")] = QStringLiteral("Normal");
            states.append(s);
        }
        {
            QJsonObject s;
            s[QStringLiteral("id")] = 2;
            s[QStringLiteral("name")] = QStringLiteral("On");
            s[QStringLiteral("kind")] = QStringLiteral("Initial");
            states.append(s);
        }
        {
            QJsonObject s;
            s[QStringLiteral("id")] = 3;
            s[QStringLiteral("name")] = QStringLiteral("Broken");
            s[QStringLiteral("kind")] = QStringLiteral("Initial");  // a second Initial -- v1 readers ignored it too
            states.append(s);
        }
        QJsonObject v1;
        v1[QStringLiteral("name")] = QStringLiteral("Legacy");
        v1[QStringLiteral("nextId")] = 4;
        v1[QStringLiteral("states")] = states;
        const app::Machine legacy = app::machineFromJson(v1);
        if (legacy.initialStateId != 2 || legacy.states.size() != 3 ||
            legacy.states[1].kind != app::StateKind::Normal || legacy.states[2].kind != app::StateKind::Normal) {
            std::fprintf(stderr,
                         "FAIL: v1 legacy loader did not promote the first Initial kind to initialStateId == 2\n");
            return 1;
        }
    }

    // ---- .sdp round trip (Project manifest) ----------------------------------
    const app::Project project{
        .name = QStringLiteral("Login Flow"),
        .machineFiles = QStringList{QStringLiteral("login_flow.sdm"), QStringLiteral("checkout.sdm")},
        .outputDir = QStringLiteral("generated"),
        .rootNamespace = QStringLiteral("app::generated"),
    };
    const QString projectPath = QDir::temp().filePath(QStringLiteral("ordo-state-designer-smoke.sdp"));
    QFile::remove(projectPath);
    if (!app::saveProject(project, projectPath, &ioError)) {
        std::fprintf(stderr, "FAIL: saveProject failed: %s\n", qUtf8Printable(ioError));
        return 1;
    }
    app::Project loadedProject;
    if (!app::loadProject(projectPath, &loadedProject, &ioError)) {
        std::fprintf(stderr, "FAIL: loadProject failed: %s\n", qUtf8Printable(ioError));
        return 1;
    }
    if (!(loadedProject == project)) {
        std::fprintf(stderr, "FAIL: .sdp round trip did not reproduce the original Project\n");
        return 1;
    }

    // ---- snapshot request/publish round trip ---------------------------------
    int snapshotOwner = 0;
    std::optional<app::events::MachineSnapshotPublished> snapshot;
    kernel.dispatcher().subscribe<app::events::MachineSnapshotPublished>(
        &snapshotOwner,
        [&snapshot](const app::events::MachineSnapshotPublished& fact) { snapshot = fact; });
    kernel.send(app::events::MachineSnapshotRequested{});
    if (!snapshot.has_value()) {
        std::fprintf(stderr, "FAIL: MachineSnapshotRequested produced no MachineSnapshotPublished\n");
        return 1;
    }
    if (!(snapshot->machine == doc->machine())) {
        std::fprintf(stderr, "FAIL: MachineSnapshotPublished's machine did not match the agent's own state\n");
        return 1;
    }
    kernel.dispatcher().unsubscribe(&snapshotOwner);

    // Drop every command registration before `kernel` goes out of scope.
    // kernel2 registered none (restore() was called directly).
    removeSimCommands(kernel);
    removeEditCommands(kernel);

    // ---- Phase 2: undo/redo journal drive ------------------------------------
    const int undoResult = runUndoSmoke();
    if (undoResult != 0) {
        return undoResult;  // runUndoSmoke() already printed its own FAIL line
    }

    // ---- Phase 2b: hierarchy data model + persistence + undo ------------------
    const int hierarchyResult = runHierarchySmoke();
    if (hierarchyResult != 0) {
        return hierarchyResult;  // runHierarchySmoke() already printed its own FAIL line
    }

    // ---- Phase 2c: extended-state context variable authoring -------------------
    const int contextResult = runContextSmoke();
    if (contextResult != 0) {
        return contextResult;  // runContextSmoke() already printed its own FAIL line
    }

    // ---- Phase 2e: invoke declaration authoring ---------------------------------
    const int invokeResult = runInvokeSmoke();
    if (invokeResult != 0) {
        return invokeResult;  // runInvokeSmoke() already printed its own FAIL line
    }

    // ---- Phase 2d: guard expression unit: parser/typeCheck/evaluate/print/rename
    // tables over infra/expression.h. Runs after the context-schema phase because
    // typeCheck resolves identifiers against the ContextVariable list it authors.
    const int expressionResult = runExpressionSmoke();
    if (expressionResult != 0) {
        return expressionResult;  // runExpressionSmoke() already printed its own FAIL line
    }

    // ---- Phase 3: sim interpreter drive --------------------------------------
    const int targetlessResult = runTargetlessSelfSimSmoke();
    if (targetlessResult != 0) {
        return targetlessResult;  // runTargetlessSelfSimSmoke() already printed its own FAIL line
    }

    const int simResult = runSimSmoke();
    if (simResult != 0) {
        return simResult;  // runSimSmoke() already printed its own FAIL line
    }

    const int rootEventResult = runRootEventSimSmoke();
    if (rootEventResult != 0) {
        return rootEventResult;  // runRootEventSimSmoke() already printed its own FAIL line
    }

    // ---- Phase 3c: hierarchical execution semantics -----------------------------
    const int hierarchySimResult = runHierarchySimSmoke();
    if (hierarchySimResult != 0) {
        return hierarchySimResult;  // runHierarchySimSmoke() already printed its own FAIL line
    }

    // ---- Phase 3d: History (shallow/deep) semantics ------------------------------
    const int historySimResult = runHistorySimSmoke();
    if (historySimResult != 0) {
        return historySimResult;  // runHistorySimSmoke() already printed its own FAIL line
    }

    // ---- Phase 3e: Back() hardening under hierarchy -------------------------------
    const int backReplayResult = runBackReplaySimSmoke();
    if (backReplayResult != 0) {
        return backReplayResult;  // runBackReplaySimSmoke() already printed its own FAIL line
    }

    // ---- Phase 3f: hierarchical codegen ---------------------------------------------
    const int hierarchicalCodegenResult = runHierarchicalCodegenSmoke();
    if (hierarchicalCodegenResult != 0) {
        return hierarchicalCodegenResult;  // runHierarchicalCodegenSmoke() already printed its own FAIL line
    }

    // ---- Phase 4: codegen validator drive -------------------------------------
    const int validatorResult = runValidatorSmoke();
    if (validatorResult != 0) {
        return validatorResult;  // runValidatorSmoke() already printed its own FAIL line
    }

    // ---- Phase 5: XState v5 export interop -------------------------------------
    const int xstateResult = runXStateInteropSmoke();
    if (xstateResult != 0) {
        return xstateResult;  // runXStateInteropSmoke() already printed its own FAIL line
    }

    // ---- Phase 5b: canvas interaction FSM legality ---------------------------
    const int interactionResult = runInteractionFsmSmoke();
    if (interactionResult != 0) {
        return interactionResult;  // runInteractionFsmSmoke() already printed its own FAIL line
    }

    // ---- Phase 5c: note-editor machine dispatch -----------------------------------
    const int noteEditorResult = runNoteEditorMachineSmoke();
    if (noteEditorResult != 0) {
        return noteEditorResult;  // runNoteEditorMachineSmoke() already printed its own FAIL line
    }

    // ---- Phase 8: pill-port resolver rule table -------------------------------
    // Defined in harness/smoke_codegen.cpp; shares portAssignmentHoldsInvariant
    // (harness/probe_support.h) with the --gui-probe scenarios.
    const int pillPortResult = runPillPortSmoke();
    if (pillPortResult != 0) {
        return pillPortResult;  // runPillPortSmoke() already printed its own FAIL line
    }

    // Phase 8b: auto-layout + labelRatio
    const int autoLayoutResult = runAutoLayoutSmoke();
    if (autoLayoutResult != 0) {
        return autoLayoutResult;  // runAutoLayoutSmoke() already printed its own FAIL line
    }

    // ---- Phase 6: multi-kernel shell smoke ------------------------------------
    const int shellResult = runShellSmoke();
    if (shellResult != 0) {
        return shellResult;  // runShellSmoke() already printed its own FAIL line
    }

    // ---- Phase 6b: MCP invoke-completion behavioural smoke ------------------
    // Runs after runShellSmoke(), never nested: each phase constructs and
    // destroys its own QApplication.
    const int mcpInvokeResult = runMcpInvokeSmoke();
    if (mcpInvokeResult != 0) {
        return mcpInvokeResult;  // runMcpInvokeSmoke() already printed its own FAIL line
    }

    std::printf(
        "PASS: state-designer smoke (domain + undo journal + hierarchy + add child state + historyDeep authoring + "
        "context variable authoring + invoke declaration authoring + guard expressions + sim interpreter + "
        "hierarchical execution + history "
        "semantics + back-replay hardening + hierarchical codegen + validator + xstate v5 interop + interaction fsm + "
        "pill-port resolver + auto-layout/labelRatio + "
        "shell isolation + project round trip + mcp invoke completion)\n");
    return 0;
}
