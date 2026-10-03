#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointF>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <ordo/core/kernel.h>

#include "controller/edit_commands.h"
#include "controller/sim_commands.h"
#include "infra/code_generator.h"
#include "infra/machine_validator.h"
#include "infra/project_io.h"
#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/sim_agent.h"
#include "model/sim_events.h"
#include "view/generated/canvas_interaction_core.h"

#include "harness/harness.h"

// Hierarchical codegen: generates a core from a hierarchical sample (compound,
// parallel regions, shallow history, delayed and root fallback transitions) and
// asserts its text; a flat sample proves the flat shape is unchanged and the
// committed canvas-interaction.sdm must regenerate byte-identically. Parts 5,
// 5b and 5c drive the interpreter through the same script as compiled proofs.
int runCodegenCoreSmoke() {
    // ==== part 1: the hierarchical sample's generated core, textually ==========
    QVector<app::GeneratedFile> hierFiles;
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        if (!doc) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke's MachineDocAgent did not register under kName\n");
            return 1;
        }
        registerEditCommands(kernel);

        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});      // id 1: P
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});     // id 2: A
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});    // id 3: B
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 50)});    // id 4: H
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});    // id 5: Q
        kernel.send(app::events::AddStateRequested{.pos = QPointF(350, -30)});  // id 6: R1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(400, -30)});  // id 7: A1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(450, -30)});  // id 8: A2
        kernel.send(app::events::AddStateRequested{.pos = QPointF(350, 30)});   // id 9: R2
        kernel.send(app::events::AddStateRequested{.pos = QPointF(400, 30)});   // id 10: C1
        kernel.send(app::events::AddStateRequested{.pos = QPointF(450, 30)});   // id 11: C2
        kernel.send(app::events::AddStateRequested{.pos = QPointF(600, 0)});    // id 12: Out
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("P")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("B")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("H")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("Q")});
        kernel.send(app::events::RenameStateRequested{.id = 6, .name = QStringLiteral("R1")});
        kernel.send(app::events::RenameStateRequested{.id = 7, .name = QStringLiteral("A1")});
        kernel.send(app::events::RenameStateRequested{.id = 8, .name = QStringLiteral("A2")});
        kernel.send(app::events::RenameStateRequested{.id = 9, .name = QStringLiteral("R2")});
        kernel.send(app::events::RenameStateRequested{.id = 10, .name = QStringLiteral("C1")});
        kernel.send(app::events::RenameStateRequested{.id = 11, .name = QStringLiteral("C2")});
        kernel.send(app::events::RenameStateRequested{.id = 12, .name = QStringLiteral("Out")});
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // A -> P (P.initialChildId = A)
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 1});  // B -> P
        kernel.send(app::events::SetStateKindRequested{.id = 4, .kind = app::StateKind::History});
        kernel.send(app::events::ReparentStateRequested{.id = 4, .parentId = 1});  // H -> P (shallow -- historyDeep
                                                                                     // stays its false default)
        kernel.send(app::events::ReparentStateRequested{.id = 6, .parentId = 5});  // R1 -> Q
        kernel.send(app::events::ReparentStateRequested{.id = 7, .parentId = 6});  // A1 -> R1 (R1.initialChildId = A1)
        kernel.send(app::events::ReparentStateRequested{.id = 8, .parentId = 6});  // A2 -> R1
        kernel.send(app::events::ReparentStateRequested{.id = 9, .parentId = 5});  // R2 -> Q
        kernel.send(app::events::ReparentStateRequested{.id = 10, .parentId = 9}); // C1 -> R2 (R2.initialChildId = C1)
        kernel.send(app::events::ReparentStateRequested{.id = 11, .parentId = 9}); // C2 -> R2
        kernel.send(app::events::SetStateKindRequested{.id = 5, .kind = app::StateKind::Parallel});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});  // machine initial = P

        kernel.send(app::events::SetEntryActionsRequested{.id = 2, .entryActions = QStringList{QStringLiteral("enterA")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 2, .exitActions = QStringList{QStringLiteral("exitA")}});
        kernel.send(app::events::SetEntryActionsRequested{.id = 3, .entryActions = QStringList{QStringLiteral("enterB")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 3, .exitActions = QStringList{QStringLiteral("exitB")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 1, .exitActions = QStringList{QStringLiteral("exitP")}});
        kernel.send(app::events::SetEntryActionsRequested{.id = 5, .entryActions = QStringList{QStringLiteral("enterQ")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 5, .exitActions = QStringList{QStringLiteral("exitQ")}});
        kernel.send(app::events::SetEntryActionsRequested{.id = 7, .entryActions = QStringList{QStringLiteral("enterA1")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 7, .exitActions = QStringList{QStringLiteral("exitA1")}});
        kernel.send(app::events::SetEntryActionsRequested{.id = 8, .entryActions = QStringList{QStringLiteral("enterA2")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 8, .exitActions = QStringList{QStringLiteral("exitA2")}});
        kernel.send(app::events::SetEntryActionsRequested{.id = 10, .entryActions = QStringList{QStringLiteral("enterC1")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 10, .exitActions = QStringList{QStringLiteral("exitC1")}});
        kernel.send(app::events::SetEntryActionsRequested{.id = 11, .entryActions = QStringList{QStringLiteral("enterC2")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 11, .exitActions = QStringList{QStringLiteral("exitC2")}});
        kernel.send(app::events::SetEntryActionsRequested{.id = 12, .entryActions = QStringList{QStringLiteral("enterOut")}});
        kernel.send(app::events::SetExitActionsRequested{.id = 12, .exitActions = QStringList{QStringLiteral("exitOut")}});

        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 13: A -Next-> B
        kernel.send(app::events::SetTransitionEventRequested{.id = 13, .event = QStringLiteral("Next")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 13, .guard = QStringLiteral("canAdvance")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 13, .action = QStringLiteral("moveAB")});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 5});  // id 14: P -Bubble-> Q
        kernel.send(app::events::SetTransitionEventRequested{.id = 14, .event = QStringLiteral("Bubble")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 14, .action = QStringLiteral("bubbleAction")});
        kernel.send(app::events::AddTransitionRequested{.from = 7, .to = 8});  // id 15: A1 -Go-> A2
        kernel.send(app::events::SetTransitionEventRequested{.id = 15, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 15, .action = QStringLiteral("goA")});
        kernel.send(app::events::AddTransitionRequested{.from = 10, .to = 11});  // id 16: C1 -Go-> C2
        kernel.send(app::events::SetTransitionEventRequested{.id = 16, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 16, .action = QStringLiteral("goC")});
        kernel.send(app::events::AddTransitionRequested{.from = 8, .to = 7});  // id 17: A2 -(300ms)-> A1
        kernel.send(app::events::SetTransitionDelayRequested{.id = 17, .delayMs = 300});
        kernel.send(app::events::SetTransitionActionRequested{.id = 17, .action = QStringLiteral("timeoutBackToA1")});
        kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 12});  // id 18: root -Panic-> Out
        kernel.send(app::events::SetTransitionEventRequested{.id = 18, .event = QStringLiteral("Panic")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 18, .action = QStringLiteral("panicAction")});
        kernel.send(app::events::AddTransitionRequested{.from = 12, .to = 4});  // id 19: Out -Restore-> H
        kernel.send(app::events::SetTransitionEventRequested{.id = 19, .event = QStringLiteral("Restore")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 19, .action = QStringLiteral("restoreAction")});

        if (doc->machine().states.size() != 12 || doc->machine().transitions.size() != 7) {
            std::fprintf(stderr,
                          "FAIL: hierarchical-codegen smoke topology build did not produce 12 states / 7 "
                          "transitions\n");
            return 1;
        }
        app::Machine machine = doc->machine();
        machine.name = QStringLiteral("Hier Sample");  // restore()-escape-hatch, same as runCodegenCheck()'s
        doc->restore(machine);
        removeEditCommands(kernel);

        if (!app::isHierarchical(doc->machine())) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke's own sample did not read as hierarchical\n");
            return 1;
        }

        hierFiles = app::generate(doc->machine(), QStringLiteral("app::generated"));
        QString hierCoreText;
        for (const app::GeneratedFile& file : hierFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                hierCoreText = file.content;
            }
        }
        if (hierCoreText.isEmpty()) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke: generate() emitted no hier_sample_core.h\n");
            return 1;
        }

        // -- static tables, correct sizes --------------------------------------
        static const char* const kExpectedTableMarkers[] = {
            "kStateCount = 12",
            "kTransitionCount = 7",
            "kParentIndex[kStateCount]",
            "kDepth[kStateCount]",
            "kInitialChildIndex[kStateCount]",
            "kIsParallel[kStateCount]",
            "std::array<bool, kStateCount> active_",
            "kTransSource[kTransitionCount]",
            "kTransDeclaredTarget[kTransitionCount]",
            "kTransTarget[kTransitionCount]",
            "kTransLcca[kTransitionCount]",
            "kTransHistoryPolicy[kTransitionCount]",
        };
        for (const char* marker : kExpectedTableMarkers) {
            if (!hierCoreText.contains(QLatin1String(marker))) {
                std::fprintf(stderr, "FAIL: hierarchical-codegen smoke: hier_sample_core.h is missing '%s'\n", marker);
                return 1;
            }
        }

        // -- no framework leak (core-only TU proof discipline) -----------------
        if (hierCoreText.contains(QStringLiteral("<ordo/"))) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke: hier_sample_core.h leaks an <ordo/ include\n");
            return 1;
        }
        bool qtTypeLeak = false;
        for (int i = 0; i + 1 < hierCoreText.size(); ++i) {
            if (hierCoreText.at(i) == QLatin1Char('Q') && hierCoreText.at(i + 1).isUpper()) {
                qtTypeLeak = true;
                break;
            }
        }
        if (qtTypeLeak) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke: hier_sample_core.h leaks a Qt type (Q[A-Z])\n");
            return 1;
        }

        // -- per-transition guard/action inlining -------------------------------
        static const char* const kExpectedInlineMarkers[] = {
            "guards_.get().canAdvance()",
            "actions_.get().moveAB()",
            "actions_.get().bubbleAction()",
            "actions_.get().goA()",
            "actions_.get().timeoutBackToA1()",
            "actions_.get().panicAction()",
        };
        for (const char* marker : kExpectedInlineMarkers) {
            if (!hierCoreText.contains(QLatin1String(marker))) {
                std::fprintf(stderr,
                              "FAIL: hierarchical-codegen smoke: hier_sample_core.h is missing inlined call '%s'\n",
                              marker);
                return 1;
            }
        }
    }

    // ==== part 2: a flat sample still generates the OLD shape ==================
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        if (!doc) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke's flat-sample MachineDocAgent did not register\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: LoggedOut
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 2: LoggedIn
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("LoggedOut")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("LoggedIn")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 3: Login
        kernel.send(app::events::SetTransitionEventRequested{.id = 3, .event = QStringLiteral("Login")});
        removeEditCommands(kernel);

        if (app::isHierarchical(doc->machine())) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke's flat sample read as hierarchical\n");
            return 1;
        }
        const QVector<app::GeneratedFile> flatFiles = app::generate(doc->machine(), QStringLiteral("app::generated"));
        QString flatCoreText;
        QString flatHooksText;
        for (const app::GeneratedFile& file : flatFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                flatCoreText = file.content;
            }
            if (file.relativePath.endsWith(QStringLiteral("_hooks.h"))) {
                flatHooksText = file.content;
            }
        }
        if (!flatCoreText.contains(QStringLiteral("state_")) || !flatCoreText.contains(QStringLiteral("transitionTo"))) {
            std::fprintf(stderr,
                          "FAIL: hierarchical-codegen smoke: the flat sample's core lost the OLD state_/"
                          "transitionTo shape\n");
            return 1;
        }
        if (flatCoreText.contains(QStringLiteral("active_")) || flatCoreText.contains(QStringLiteral("kStateCount"))) {
            std::fprintf(stderr,
                          "FAIL: hierarchical-codegen smoke: the flat sample's core leaked hierarchical shape\n");
            return 1;
        }
        // A machine with no context variables emits no Context struct, no
        // <string>, and no context parameter anywhere; GenModel::hasContext is the
        // emitter's only branch point.
        if (flatHooksText.contains(QStringLiteral("struct Context")) ||
            flatHooksText.contains(QStringLiteral("#include <string>")) ||
            flatHooksText.contains(QStringLiteral("Context&")) ||
            flatCoreText.contains(QStringLiteral("context_"))) {
            std::fprintf(stderr,
                          "FAIL: hierarchical-codegen smoke: a CONTEXT-FREE flat machine emitted context shape -- "
                          "the byte-identity law is broken\n");
            return 1;
        }
    }

    // ==== part 3: in-smoke drift guarantee (canvas-interaction, byte-identical) =
    // Committed == regenerated. The canvas machine is hierarchical (its sessions
    // are children of a `Session` compound); part 2 owns the flat-emission law.
    {
        app::Machine canvasMachine;
        QString error;
        if (!app::loadMachine(QStringLiteral("src/machines/canvas-interaction.sdm"), &canvasMachine,
                               &error)) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke could not load canvas-interaction.sdm: %s\n",
                          qUtf8Printable(error));
            return 1;
        }
        if (!app::isHierarchical(canvasMachine)) {
            std::fprintf(stderr,
                          "FAIL: hierarchical-codegen smoke: canvas-interaction.sdm read as FLAT -- the "
                          "Session compound is gone from the machine document\n");
            return 1;
        }
        const QVector<app::GeneratedFile> canvasFiles =
            app::generate(canvasMachine, QStringLiteral("app::generated"));
        for (const app::GeneratedFile& file : canvasFiles) {
            const QString committedPath =
                QStringLiteral("src/view/generated/") + file.relativePath;
            QFile committed(committedPath);
            if (!committed.open(QIODevice::ReadOnly)) {
                std::fprintf(stderr, "FAIL: hierarchical-codegen smoke could not read committed %s\n",
                              qUtf8Printable(committedPath));
                return 1;
            }
            const QByteArray committedBytes = committed.readAll();
            if (committedBytes != file.content.toUtf8()) {
                std::fprintf(stderr,
                              "FAIL: hierarchical-codegen smoke: %s is not byte-identical to committed %s -- flat "
                              "codegen drifted\n",
                              qUtf8Printable(file.relativePath), qUtf8Printable(committedPath));
                return 1;
            }
        }
    }

    // ==== part 3b: machineSelf resolves at GENERATION time =====================
    // generate(machine-with-flag) must be byte-identical to generate(the same
    // machine hand-resolved to a plain root->initial transition): the flag is a
    // model-level rewrite before buildModel(), never an emitter branch.
    {
        app::Machine flagged;
        flagged.name = QStringLiteral("selfgen");
        flagged.nextId = 10;
        flagged.initialStateId = 1;
        app::State parent;
        parent.id = 1;
        parent.name = QStringLiteral("A");
        parent.initialChildId = 2;
        app::State child;
        child.id = 2;
        child.name = QStringLiteral("A1");
        child.parentId = 1;
        app::State other;
        other.id = 3;
        other.name = QStringLiteral("B");
        flagged.states = {parent, child, other};
        app::Transition restart;
        restart.id = 4;
        restart.event = QStringLiteral("Reset");
        restart.machineSelf = true;  // from == 0, to == 0
        app::Transition go;
        go.id = 5;
        go.from = 1;
        go.to = 3;
        go.event = QStringLiteral("Go");
        flagged.transitions = {restart, go};
        if (!app::isHierarchical(flagged)) {
            std::fprintf(stderr, "FAIL: the machineSelf codegen fixture must be hierarchical\n");
            return 1;
        }

        app::Machine resolved = flagged;
        resolved.transitions[0].machineSelf = false;
        resolved.transitions[0].to = resolved.initialStateId;

        const QVector<app::GeneratedFile> flaggedFiles = app::generate(flagged, QStringLiteral("app::generated"));
        const QVector<app::GeneratedFile> resolvedFiles = app::generate(resolved, QStringLiteral("app::generated"));
        if (flaggedFiles.size() != resolvedFiles.size() || flaggedFiles.isEmpty()) {
            std::fprintf(stderr, "FAIL: machineSelf and hand-resolved codegen produced different file sets\n");
            return 1;
        }
        for (int i = 0; i < flaggedFiles.size(); ++i) {
            if (flaggedFiles[i].relativePath != resolvedFiles[i].relativePath ||
                flaggedFiles[i].content != resolvedFiles[i].content) {
                std::fprintf(stderr,
                              "FAIL: generate(machineSelf) is not byte-identical to generate(hand-resolved) at %s\n",
                              qUtf8Printable(flaggedFiles[i].relativePath));
                return 1;
            }
        }
    }

    // ==== part 4: write the compile-proof inputs (temp-scoped) =================
    {
        QString error;
        if (!app::writeGeneratedFiles(QStringLiteral("temp/code/sd-core-proof/gen-hier"), hierFiles, &error)) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke could not write gen-hier/: %s\n",
                          qUtf8Printable(error));
            return 1;
        }
    }

    // ==== part 5: twin interpreter fixture -- same topology, event script and
    // hand-derived expectation as the compiled proof of part 4's generated core.
    // State ids match the part 1 sample one-for-one (add order == doc order ==
    // id, 1-12); transition ids 13-19. Change the script here, change it there.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke's twin-interpreter agents did not register\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::SetMachineNameRequested{.name = QStringLiteral("Hier Sample Twin")});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 50)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 0)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(350, -30)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(400, -30)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(450, -30)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(350, 30)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(400, 30)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(450, 30)});
        kernel.send(app::events::AddStateRequested{.pos = QPointF(600, 0)});
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 1});
        kernel.send(app::events::SetStateKindRequested{.id = 4, .kind = app::StateKind::History});
        kernel.send(app::events::ReparentStateRequested{.id = 4, .parentId = 1});
        kernel.send(app::events::ReparentStateRequested{.id = 6, .parentId = 5});
        kernel.send(app::events::ReparentStateRequested{.id = 7, .parentId = 6});
        kernel.send(app::events::ReparentStateRequested{.id = 8, .parentId = 6});
        kernel.send(app::events::ReparentStateRequested{.id = 9, .parentId = 5});
        kernel.send(app::events::ReparentStateRequested{.id = 10, .parentId = 9});
        kernel.send(app::events::ReparentStateRequested{.id = 11, .parentId = 9});
        kernel.send(app::events::SetStateKindRequested{.id = 5, .kind = app::StateKind::Parallel});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(
            app::events::SetEntryActionsRequested{.id = 2, .entryActions = QStringList{QStringLiteral("enterA")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 2, .exitActions = QStringList{QStringLiteral("exitA")}});
        kernel.send(
            app::events::SetEntryActionsRequested{.id = 3, .entryActions = QStringList{QStringLiteral("enterB")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 3, .exitActions = QStringList{QStringLiteral("exitB")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 1, .exitActions = QStringList{QStringLiteral("exitP")}});
        kernel.send(
            app::events::SetEntryActionsRequested{.id = 5, .entryActions = QStringList{QStringLiteral("enterQ")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 5, .exitActions = QStringList{QStringLiteral("exitQ")}});
        kernel.send(
            app::events::SetEntryActionsRequested{.id = 7, .entryActions = QStringList{QStringLiteral("enterA1")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 7, .exitActions = QStringList{QStringLiteral("exitA1")}});
        kernel.send(
            app::events::SetEntryActionsRequested{.id = 8, .entryActions = QStringList{QStringLiteral("enterA2")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 8, .exitActions = QStringList{QStringLiteral("exitA2")}});
        kernel.send(
            app::events::SetEntryActionsRequested{.id = 10, .entryActions = QStringList{QStringLiteral("enterC1")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 10, .exitActions = QStringList{QStringLiteral("exitC1")}});
        kernel.send(
            app::events::SetEntryActionsRequested{.id = 11, .entryActions = QStringList{QStringLiteral("enterC2")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 11, .exitActions = QStringList{QStringLiteral("exitC2")}});
        kernel.send(
            app::events::SetEntryActionsRequested{.id = 12, .entryActions = QStringList{QStringLiteral("enterOut")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 12, .exitActions = QStringList{QStringLiteral("exitOut")}});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 13
        kernel.send(app::events::SetTransitionEventRequested{.id = 13, .event = QStringLiteral("Next")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 13, .guard = QStringLiteral("canAdvance")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 13, .action = QStringLiteral("moveAB")});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 5});  // id 14
        kernel.send(app::events::SetTransitionEventRequested{.id = 14, .event = QStringLiteral("Bubble")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 14, .action = QStringLiteral("bubbleAction")});
        kernel.send(app::events::AddTransitionRequested{.from = 7, .to = 8});  // id 15
        kernel.send(app::events::SetTransitionEventRequested{.id = 15, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 15, .action = QStringLiteral("goA")});
        kernel.send(app::events::AddTransitionRequested{.from = 10, .to = 11});  // id 16
        kernel.send(app::events::SetTransitionEventRequested{.id = 16, .event = QStringLiteral("Go")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 16, .action = QStringLiteral("goC")});
        kernel.send(app::events::AddTransitionRequested{.from = 8, .to = 7});  // id 17
        kernel.send(app::events::SetTransitionDelayRequested{.id = 17, .delayMs = 300});
        kernel.send(app::events::SetTransitionActionRequested{.id = 17, .action = QStringLiteral("timeoutBackToA1")});
        kernel.send(app::events::AddTransitionRequested{.from = 0, .to = 12});  // id 18
        kernel.send(app::events::SetTransitionEventRequested{.id = 18, .event = QStringLiteral("Panic")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 18, .action = QStringLiteral("panicAction")});
        kernel.send(app::events::AddTransitionRequested{.from = 12, .to = 4});  // id 19
        kernel.send(app::events::SetTransitionEventRequested{.id = 19, .event = QStringLiteral("Restore")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 19, .action = QStringLiteral("restoreAction")});
        removeEditCommands(kernel);

        // The twin law only holds on a valid document: a validator Error means
        // the fixture, not the emitter, is under test.
        for (const app::Problem& problem : app::validate(doc->machine())) {
            if (problem.severity == app::ProblemSeverity::Error) {
                std::fprintf(stderr,
                              "FAIL: hierarchical-codegen smoke twin: the fixture machine has a validator Error: %s\n",
                              qUtf8Printable(problem.text));
                return 1;
            }
        }

        const auto configEquals = [](const std::vector<quint64>& actual, std::initializer_list<quint64> expected) {
            return actual == std::vector<quint64>(expected);
        };
        const auto orderedInTrace = [](const QStringList& trace, std::initializer_list<const char*> markers) {
            int last = -1;
            for (const char* marker : markers) {
                int idx = -1;
                for (int i = last + 1; i < trace.size(); ++i) {
                    if (trace[i].contains(QLatin1String(marker))) {
                        idx = i;
                        break;
                    }
                }
                if (idx < 0) {
                    return false;
                }
                last = idx;
            }
            return true;
        };

        int obsOwner = 0;
        std::vector<app::events::ActiveStateChanged> activeChanges;
        kernel.dispatcher().subscribe<app::events::ActiveStateChanged>(
            &obsOwner,
            [&activeChanges](const app::events::ActiveStateChanged& fact) { activeChanges.push_back(fact); });

        sim->run();  // service entrance
        if (!configEquals(sim->configuration(), {1, 2})) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke twin: Run did not descend P -> A\n");
            return 1;
        }

        // step 1: "Next" is a plain in-region transition (also exercises the guard).
        activeChanges.clear();
        sim->sendEvent(QStringLiteral("Next"));
        if (!configEquals(sim->configuration(), {1, 3}) || activeChanges.size() != 1 ||
            activeChanges.back().fromId != 2 || activeChanges.back().toId != 3 || activeChanges.back().viaTransitionId != 13) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke twin: Next did not move A -> B (via 13)\n");
            return 1;
        }
        if (!orderedInTrace(sim->trace(), {"action: exitA", "action: moveAB", "action: enterB"})) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke twin: Next's action order was wrong\n");
            return 1;
        }

        // step 2: bubbling -- B has no Bubble handler, P's does; also the
        // entry into Q's parallel regions.
        activeChanges.clear();
        sim->sendEvent(QStringLiteral("Bubble"));
        if (!configEquals(sim->configuration(), {5, 6, 7, 9, 10}) || activeChanges.size() != 1 ||
            activeChanges.back().fromId != 1 || activeChanges.back().toId != 5 || activeChanges.back().viaTransitionId != 14) {
            std::fprintf(stderr,
                          "FAIL: hierarchical-codegen smoke twin: Bubble did not walk B -> P's handler (via 14) "
                          "into Q's two regions\n");
            return 1;
        }
        if (!orderedInTrace(sim->trace(), {"action: exitB", "action: exitP", "action: bubbleAction", "action: enterQ",
                                            "action: enterA1", "action: enterC1"})) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke twin: Bubble's action order was wrong\n");
            return 1;
        }

        // step 3: parallel macrostep -- both regions' "Go" fire as two microsteps in one macrostep.
        activeChanges.clear();
        sim->sendEvent(QStringLiteral("Go"));
        if (!configEquals(sim->configuration(), {5, 6, 8, 9, 11}) || activeChanges.size() != 2 ||
            activeChanges[0].fromId != 7 || activeChanges[0].toId != 8 || activeChanges[0].viaTransitionId != 15 ||
            activeChanges[1].fromId != 10 || activeChanges[1].toId != 11 || activeChanges[1].viaTransitionId != 16) {
            std::fprintf(stderr,
                          "FAIL: hierarchical-codegen smoke twin: Go did not fire TWO microsteps (via 15 then 16) "
                          "in one macrostep\n");
            return 1;
        }

        // step 4: delayed fire (owned by A2, inside region R1) via tick().
        activeChanges.clear();
        sim->tick(300);
        if (!configEquals(sim->configuration(), {5, 6, 7, 9, 11}) || activeChanges.size() != 1 ||
            activeChanges.back().fromId != 8 || activeChanges.back().toId != 7 || activeChanges.back().viaTransitionId != 17) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke twin: tick(300) did not fire A2's delayed "
                                  "transition (via 17) back to A1\n");
            return 1;
        }

        // step 5: root fallback -- no region handles "Panic"; fromId
        // generalizes to the document-order-first atomic in the exit set (A1).
        activeChanges.clear();
        sim->sendEvent(QStringLiteral("Panic"));
        if (!configEquals(sim->configuration(), {12}) || activeChanges.size() != 1 || activeChanges.back().fromId != 7 ||
            activeChanges.back().toId != 12 || activeChanges.back().viaTransitionId != 18) {
            std::fprintf(stderr,
                          "FAIL: hierarchical-codegen smoke twin: Panic did not fall through to the root handler "
                          "(via 18), or fromId was not the exited atomic A1\n");
            return 1;
        }

        // step 6: shallow History restore -- redirects to P, toId reports H as
        // declared, restores {P,B} from the record Bubble wrote.
        activeChanges.clear();
        sim->sendEvent(QStringLiteral("Restore"));
        if (!configEquals(sim->configuration(), {1, 3}) || activeChanges.size() != 1 || activeChanges.back().fromId != 12 ||
            activeChanges.back().toId != 4 || activeChanges.back().viaTransitionId != 19) {
            std::fprintf(stderr,
                          "FAIL: hierarchical-codegen smoke twin: Restore did not shallow-redirect to {P,B} (toId "
                          "must be H=4 AS DECLARED)\n");
            return 1;
        }
        if (!orderedInTrace(sim->trace(), {"action: exitOut", "action: restoreAction", "action: enterB"})) {
            std::fprintf(stderr, "FAIL: hierarchical-codegen smoke twin: Restore's action order was wrong\n");
            return 1;
        }

        kernel.dispatcher().unsubscribe(&obsOwner);
    }

    // ==== part 5b: the same twin law over extended state =======================
    // Pins configuration and every context value at every step of a five-event
    // script (Start, Raise, Reset, Raise, Check). The second Raise fails its
    // expression guard so the unguarded fallback fires. Int->Double widens and
    // Double->Int truncates. The hooks (bare-identifier guard, named action)
    // must not change context, since a named hook is opaque to the interpreter.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: context-codegen twin: the agents did not register\n");
            return 1;
        }
        registerEditCommands(kernel);

        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Idle
        kernel.send(app::events::AddStateRequested{.pos = QPointF(130, 0)});  // id 2: Active
        kernel.send(app::events::AddStateRequested{.pos = QPointF(260, 0)});  // id 3: Busy
        kernel.send(app::events::AddStateRequested{.pos = QPointF(390, 0)});  // id 4: Done
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Idle")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Active")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Busy")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Done")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});

        // The schema, in document order; ids 5-8 share the states' nextId counter.
        // The default type is Int/"0", so `count` needs no retype.
        kernel.send(app::events::AddContextVariableRequested{});  // id 5
        kernel.send(app::events::RenameContextVariableRequested{.id = 5, .name = QStringLiteral("count")});
        kernel.send(app::events::AddContextVariableRequested{});  // id 6
        kernel.send(app::events::RenameContextVariableRequested{.id = 6, .name = QStringLiteral("locked")});
        kernel.send(app::events::SetContextTypeRequested{.id = 6, .type = app::ContextType::Bool});
        kernel.send(app::events::SetContextInitialValueRequested{.id = 6, .initialValue = QStringLiteral("false")});
        kernel.send(app::events::AddContextVariableRequested{});  // id 7
        kernel.send(app::events::RenameContextVariableRequested{.id = 7, .name = QStringLiteral("ratio")});
        kernel.send(app::events::SetContextTypeRequested{.id = 7, .type = app::ContextType::Double});
        kernel.send(app::events::SetContextInitialValueRequested{.id = 7, .initialValue = QStringLiteral("1.5")});
        kernel.send(app::events::AddContextVariableRequested{});  // id 8
        kernel.send(app::events::RenameContextVariableRequested{.id = 8, .name = QStringLiteral("label")});
        kernel.send(app::events::SetContextTypeRequested{.id = 8, .type = app::ContextType::String});
        kernel.send(app::events::SetContextInitialValueRequested{.id = 8, .initialValue = QStringLiteral("start")});

        kernel.send(app::events::SetEntryActionsRequested{
            .id = 2, .entryActions = QStringList{QStringLiteral("label = 'active'")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 2, .exitActions = QStringList{QStringLiteral("ratio = 3")}});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = 3, .entryActions = QStringList{QStringLiteral("count = 2.7")}});

        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 9
        kernel.send(app::events::SetTransitionEventRequested{.id = 9, .event = QStringLiteral("Start")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 9, .action = QStringLiteral("count = 3")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 10
        kernel.send(app::events::SetTransitionEventRequested{.id = 10, .event = QStringLiteral("Raise")});
        kernel.send(app::events::SetTransitionGuardRequested{
            .id = 10, .guard = QStringLiteral("count > 1 && !locked")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 10, .action = QStringLiteral("locked = true")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 1});  // id 11 -- unguarded, LAST in its group
        kernel.send(app::events::SetTransitionEventRequested{.id = 11, .event = QStringLiteral("Raise")});
        kernel.send(
            app::events::SetTransitionActionRequested{.id = 11, .action = QStringLiteral("label = 'fallback'")});
        kernel.send(app::events::AddTransitionRequested{.from = 3, .to = 2});  // id 12
        kernel.send(app::events::SetTransitionEventRequested{.id = 12, .event = QStringLiteral("Reset")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 12, .action = QStringLiteral("count = 0")});
        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 4});  // id 13
        kernel.send(app::events::SetTransitionEventRequested{.id = 13, .event = QStringLiteral("Check")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 13, .guard = QStringLiteral("allowFinish")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 13, .action = QStringLiteral("stampDone")});

        app::Machine ctxMachine = doc->machine();
        ctxMachine.name = QStringLiteral("Ctx Sample");  // restore()-escape-hatch, same as part 1's
        doc->restore(ctxMachine);
        removeEditCommands(kernel);

        if (doc->machine().context.size() != 4 || app::isHierarchical(doc->machine())) {
            std::fprintf(stderr, "FAIL: context-codegen twin: the fixture is not a 4-variable FLAT machine\n");
            return 1;
        }
        // A validator Error means the fixture, not the emitter, is under test.
        for (const app::Problem& problem : app::validate(doc->machine())) {
            if (problem.severity == app::ProblemSeverity::Error) {
                std::fprintf(stderr, "FAIL: context-codegen twin: the fixture machine has a validator Error: %s\n",
                              qUtf8Printable(problem.text));
                return 1;
            }
        }

        // ---- the emitted text ------------------------------------------------
        const QVector<app::GeneratedFile> ctxFiles = app::generate(doc->machine(), QStringLiteral("app::generated"));
        QString ctxCoreText;
        QString ctxHooksText;
        for (const app::GeneratedFile& file : ctxFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                ctxCoreText = file.content;
            }
            if (file.relativePath.endsWith(QStringLiteral("_hooks.h"))) {
                ctxHooksText = file.content;
            }
        }
        if (ctxCoreText.isEmpty() || ctxHooksText.isEmpty()) {
            std::fprintf(stderr, "FAIL: context-codegen twin: generate() emitted no ctx_sample core/hooks\n");
            return 1;
        }

        // struct Context: one typed member per variable, in document order, names verbatim.
        static const char* const kExpectedContextMembers[] = {
            "struct Context {",
            "    long long count = 0;",
            "    bool locked = false;",
            "    double ratio = 1.5;",
            "    std::string label = \"start\";",
            "#include <string>",  // and ONLY because a String member exists
        };
        for (const char* marker : kExpectedContextMembers) {
            if (!ctxHooksText.contains(QLatin1String(marker))) {
                std::fprintf(stderr, "FAIL: context-codegen twin: ctx_sample_hooks.h is missing '%s'\n", marker);
                return 1;
            }
        }
        // Context must be declared before the hook interfaces whose signatures name it.
        if (ctxHooksText.indexOf(QStringLiteral("struct Context {")) >
            ctxHooksText.indexOf(QStringLiteral("struct CtxSampleGuards {"))) {
            std::fprintf(stderr, "FAIL: context-codegen twin: struct Context is emitted AFTER the hook interfaces\n");
            return 1;
        }

        // Only the bare-identifier guard and the non-assign action become hooks;
        // assigns and the expression guard declare nothing.
        if (!ctxHooksText.contains(QStringLiteral("    virtual bool allowFinish(const Context&) = 0;\n")) ||
            !ctxHooksText.contains(QStringLiteral("    virtual void stampDone(Context&) = 0;\n")) ||
            ctxHooksText.count(QStringLiteral("    virtual bool ")) != 1 ||
            ctxHooksText.count(QStringLiteral("    virtual void ")) != 1) {
            std::fprintf(stderr,
                          "FAIL: context-codegen twin: the hook list is not exactly {allowFinish, stampDone} with "
                          "Context parameters\n");
            return 1;
        }

        // The core: the expression guard inline, every assign as a plain assignment
        // with the interpreter's coercion, and the two hooks handed the core's Context.
        static const char* const kExpectedCoreMarkers[] = {
            "if ((context_.count > 1LL) && (!context_.locked)) {",
            "context_.count = static_cast<long long>(3LL);",
            "context_.locked = true;",
            "context_.ratio = static_cast<double>(3LL);",
            "context_.count = static_cast<long long>(2.7);",
            "context_.label = \"fallback\";",
            "context_.label = \"active\";",
            "context_.count = static_cast<long long>(0LL);",
            "guards_.get().allowFinish(context_)",
            "actions_.get().stampDone(context_);",
            "Context context_;",
            "Context& context() { return context_; }",
        };
        for (const char* marker : kExpectedCoreMarkers) {
            if (!ctxCoreText.contains(QLatin1String(marker))) {
                std::fprintf(stderr, "FAIL: context-codegen twin: ctx_sample_core.h is missing '%s'\n", marker);
                return 1;
            }
        }
        // The expression guard must not be collected as a hook.
        if (ctxCoreText.contains(QStringLiteral("guards_.get().count")) ||
            ctxHooksText.contains(QStringLiteral("count1Locked")) ||
            ctxHooksText.contains(QStringLiteral("countLocked"))) {
            std::fprintf(stderr,
                          "FAIL: context-codegen twin: the expression guard was collected as a hook after all\n");
            return 1;
        }
        // Generated files stay framework-free (std::string is fine).
        if (ctxCoreText.contains(QStringLiteral("<ordo/")) || ctxHooksText.contains(QStringLiteral("<ordo/"))) {
            std::fprintf(stderr, "FAIL: context-codegen twin: a context file leaks an <ordo/ include\n");
            return 1;
        }
        for (const QString& text : {ctxCoreText, ctxHooksText}) {
            for (int i = 0; i + 1 < text.size(); ++i) {
                if (text.at(i) == QLatin1Char('Q') && text.at(i + 1).isUpper()) {
                    std::fprintf(stderr, "FAIL: context-codegen twin: a context file leaks a Qt type (Q[A-Z])\n");
                    return 1;
                }
            }
        }

        // The stubs live in `namespace domain` while Context does not, so their
        // Context parameter must be fully qualified; stubs are never overwritten
        // once written.
        QString ctxStubText;
        for (const app::GeneratedFile& stub : app::generateDomainStubs(doc->machine(), QStringLiteral("app::generated"))) {
            if (stub.relativePath.endsWith(QStringLiteral("_hooks_impl.h"))) {
                ctxStubText = stub.content;
            }
        }
        if (!ctxStubText.contains(
                QStringLiteral("bool allowFinish(const app::generated::ctx_sample::Context&) override")) ||
            !ctxStubText.contains(QStringLiteral("void stampDone(app::generated::ctx_sample::Context&) override"))) {
            std::fprintf(stderr,
                          "FAIL: context-codegen twin: the domain stubs do not override the Context-taking hook "
                          "signatures with a fully-qualified Context\n");
            return 1;
        }

        // ---- the interpreter side, step by step ------------------------------
        const auto configIs = [&sim](quint64 expected) {
            return sim->configuration() == std::vector<quint64>{expected};
        };
        const auto contextIs = [&sim](qint64 count, bool locked, double ratio, const char* label) {
            const QVariantMap values = sim->contextValues();
            return values.value(QStringLiteral("count")).toLongLong() == count &&
                   values.value(QStringLiteral("locked")).toBool() == locked &&
                   values.value(QStringLiteral("ratio")).toDouble() == ratio &&
                   values.value(QStringLiteral("label")).toString() == QLatin1String(label);
        };
        const auto step = [&](const char* what, quint64 state, qint64 count, bool locked, double ratio,
                               const char* label) {
            if (configIs(state) && contextIs(count, locked, ratio, label)) {
                return true;
            }
            const QVariantMap values = sim->contextValues();
            std::fprintf(stderr,
                          "FAIL: context-codegen twin: after %s the interpreter holds state=%llu count=%lld "
                          "locked=%d ratio=%f label='%s'\n",
                          what, sim->configuration().empty() ? 0ULL : sim->configuration().front(),
                          static_cast<long long>(values.value(QStringLiteral("count")).toLongLong()),
                          values.value(QStringLiteral("locked")).toBool() ? 1 : 0,
                          values.value(QStringLiteral("ratio")).toDouble(),
                          qUtf8Printable(values.value(QStringLiteral("label")).toString()));
            return false;
        };

        sim->run();  // service entrance
        if (!step("run()", 1, 0, false, 1.5, "start")) {
            return 1;
        }
        sim->sendEvent(QStringLiteral("Start"));
        if (!step("Start", 2, 3, false, 1.5, "active")) {
            return 1;
        }
        sim->sendEvent(QStringLiteral("Raise"));  // the expression guard PASSES
        if (!step("Raise (guard passes)", 3, 2, true, 3.0, "active")) {
            return 1;
        }
        sim->sendEvent(QStringLiteral("Reset"));
        if (!step("Reset", 2, 0, true, 3.0, "active")) {
            return 1;
        }
        sim->sendEvent(QStringLiteral("Raise"));  // the expression guard GATES -- fallback fires
        if (!step("Raise (guard gates)", 1, 0, true, 3.0, "fallback")) {
            return 1;
        }
        sim->sendEvent(QStringLiteral("Check"));  // bare-identifier guard hook + action hook
        if (!step("Check", 4, 0, true, 3.0, "fallback")) {
            return 1;
        }

        // ---- the compile-proof inputs ----------------------------------------
        // Written by this run, never hand edited, so the compiled proof cannot
        // drift from the emitter.
        QString error;
        if (!app::writeGeneratedFiles(QStringLiteral("temp/code/sd-core-proof/gen-context"), ctxFiles, &error)) {
            std::fprintf(stderr, "FAIL: context-codegen twin could not write gen-context/: %s\n",
                          qUtf8Printable(error));
            return 1;
        }
    }

    // ==== part 5c: typed event payloads on a HIERARCHICAL machine ==============
    // The twin law for `event` bound per row: payload rows on nested states and the
    // root, a `do.*` wildcard row, and an `always` row. "do.set" tries C's exact
    // row before its `do.*` row (specificity, then document order); if the exact
    // guard fails, the payload-free wildcard row fires instead.
    // Change the script here, change it in the compiled proof.
    {
        app::Machine machine;
        machine.name = QStringLiteral("Hier Payload");
        machine.initialStateId = 1;
        app::State p{.id = 1, .name = QStringLiteral("P")};
        p.initialChildId = 2;
        app::State a{.id = 2, .name = QStringLiteral("A")};
        a.parentId = 1;
        app::State b{.id = 3, .name = QStringLiteral("B")};
        b.parentId = 1;
        app::State c{.id = 4, .name = QStringLiteral("C")};
        machine.states = {p, a, b, c};
        machine.context = {
            app::ContextVariable{.id = 5, .name = QStringLiteral("level"), .type = app::ContextType::Int,
                                 .initialValue = QStringLiteral("0")},
            app::ContextVariable{.id = 6, .name = QStringLiteral("enabled"), .type = app::ContextType::Bool,
                                 .initialValue = QStringLiteral("false")},
            app::ContextVariable{.id = 7, .name = QStringLiteral("ticks"), .type = app::ContextType::Int,
                                 .initialValue = QStringLiteral("0")},
        };
        app::Transition setPass{.id = 8, .from = 2, .to = 3, .event = QStringLiteral("Set"),
                                .guard = QStringLiteral("event > 3"), .action = QStringLiteral("level = event")};
        setPass.payloadType = QStringLiteral("Int");
        app::Transition setBubble{.id = 9, .from = 1, .to = 0, .event = QStringLiteral("Set"),
                                  .guard = QStringLiteral("event <= 3"), .action = QStringLiteral("level = event")};
        setBubble.payloadType = QStringLiteral("Int");
        app::Transition flag{.id = 10, .from = 0, .to = 0, .event = QStringLiteral("Flag"),
                             .action = QStringLiteral("enabled = event")};
        flag.payloadType = QStringLiteral("Bool");
        app::Transition go{.id = 11, .from = 3, .to = 4, .event = QStringLiteral("Go")};
        app::Transition wildcard{.id = 12, .from = 4, .to = 2, .event = QStringLiteral("do.*"),
                                 .action = QStringLiteral("resetHit")};
        app::Transition eventless{.id = 13, .from = 3, .to = 4, .guard = QStringLiteral("level > 7"),
                                  .action = QStringLiteral("ticks = ticks + 1")};
        eventless.always = true;
        app::Transition doSet{.id = 14, .from = 4, .to = 3, .event = QStringLiteral("do.set"),
                              .guard = QStringLiteral("event >= 0"), .action = QStringLiteral("level = event")};
        doSet.payloadType = QStringLiteral("Int");
        machine.transitions = {setPass, setBubble, flag, go, wildcard, eventless, doSet};
        machine.nextId = 15;

        if (!app::isHierarchical(machine)) {
            std::fprintf(stderr, "FAIL: hierarchical payload twin: the fixture did not read as hierarchical\n");
            return 1;
        }
        // A validator Error means the fixture, not the emitter, is under test.
        for (const app::Problem& problem : app::validate(machine)) {
            if (problem.severity == app::ProblemSeverity::Error) {
                std::fprintf(stderr, "FAIL: hierarchical payload twin: the fixture has a validator Error: %s\n",
                              qUtf8Printable(problem.text));
                return 1;
            }
        }

        // ---- the emitted text ------------------------------------------------
        const QVector<app::GeneratedFile> payloadFiles = app::generate(machine, QStringLiteral("app::generated"));
        QString coreText;
        for (const app::GeneratedFile& file : payloadFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                coreText = file.content;
            }
        }
        if (coreText.isEmpty()) {
            std::fprintf(stderr, "FAIL: hierarchical payload twin: generate() emitted no hier_payload_core.h\n");
            return 1;
        }
        static const char* const kExpectedPayloadMarkers[] = {
            "void setRequested(const long long& event) {",
            "void flagRequested(const bool& event) {",
            "void goRequested() {",
            "int matchAt_setRequested(int stateIndex, [[maybe_unused]] const long long& event) const {",
            "int matchRoot_setRequested([[maybe_unused]] const long long& event) const {",
            "int matchRoot_flagRequested([[maybe_unused]] const bool& event) const {",
            "void fire_setRequested(int transitionIndex, const long long& event) {",
            "void fire_flagRequested(int transitionIndex, const bool& event) {",
            "void fireTransition0([[maybe_unused]] const long long& event) {",
            "void fireTransition1([[maybe_unused]] const long long& event) {",
            "void fireTransition2([[maybe_unused]] const bool& event) {",
            "void fireTransition3() {",
            "void fireTransition4() {",
            "void fireTransition5() {",
            "void fireTransition6([[maybe_unused]] const long long& event) {",
            "void doSetRequested(const long long& event) {",
            "void fire_doSetRequested(int transitionIndex, const long long& event) {",
            "int selectToFire(",
            "selectAndFireAlways(",
        };
        for (const char* marker : kExpectedPayloadMarkers) {
            if (!coreText.contains(QLatin1String(marker))) {
                std::fprintf(stderr, "FAIL: hierarchical payload twin: hier_payload_core.h is missing '%s'\n", marker);
                return 1;
            }
        }
        // `event` must not resolve as a context member.
        if (coreText.contains(QStringLiteral("context_.event"))) {
            std::fprintf(stderr, "FAIL: hierarchical payload twin: hier_payload_core.h reads `event` as context_.event\n");
            return 1;
        }
        // Payload rows are reachable only through fire_<event>(index, event);
        // the generic index switch has no payload to hand them.
        const int switchStart = coreText.indexOf(QStringLiteral("void fireTransition(int transitionIndex) {"));
        const int switchEnd = switchStart < 0 ? -1 : coreText.indexOf(QStringLiteral("default:"), switchStart);
        if (switchStart < 0 || switchEnd < 0) {
            std::fprintf(stderr, "FAIL: hierarchical payload twin: no fireTransition(int) switch in hier_payload_core.h\n");
            return 1;
        }
        const QString indexSwitch = coreText.mid(switchStart, switchEnd - switchStart);
        for (int i = 0; i < 7; ++i) {
            const bool present = indexSwitch.contains(QStringLiteral("case %1:").arg(i));
            if (present != (i >= 3 && i <= 5)) {
                std::fprintf(stderr,
                              "FAIL: hierarchical payload twin: fireTransition(int) %s case %d (payload rows 0-2 "
                              "and 6 must be absent, payload-free rows 3-5 present)\n",
                              present ? "has" : "lacks", i);
                return 1;
            }
        }
        // Mixed dispatch: the payload row takes `event`; the wildcard row that also
        // matches "do.set" has none, and selection must try the exact row first.
        const auto functionBody = [&coreText](const QString& signature) {
            const int start = coreText.indexOf(signature);
            const int end = start < 0 ? -1 : coreText.indexOf(QStringLiteral("\n    }\n"), start);
            return start < 0 || end < 0 ? QString() : coreText.mid(start, end - start);
        };
        const QString fireDoSet =
            functionBody(QStringLiteral("void fire_doSetRequested(int transitionIndex, const long long& event) {"));
        if (!fireDoSet.contains(QStringLiteral("fireTransition6(event);")) ||
            !fireDoSet.contains(QStringLiteral("fireTransition4();"))) {
            std::fprintf(stderr,
                          "FAIL: hierarchical payload twin: fire_doSetRequested lacks the payload case "
                          "fireTransition6(event) or the payload-free wildcard case fireTransition4()\n");
            return 1;
        }
        const QString matchDoSet = functionBody(
            QStringLiteral("int matchAt_doSetRequested(int stateIndex, [[maybe_unused]] const long long& event) const {"));
        const int exactAt = matchDoSet.indexOf(QStringLiteral("return 6;"));
        const int wildcardAt = matchDoSet.indexOf(QStringLiteral("return 4;"));
        if (exactAt < 0 || wildcardAt < 0 || exactAt > wildcardAt) {
            std::fprintf(stderr,
                          "FAIL: hierarchical payload twin: matchAt_doSetRequested does not try the exact row (6) "
                          "before the wildcard row (4)\n");
            return 1;
        }

        // ---- the compile-proof inputs ----------------------------------------
        QString error;
        if (!app::writeGeneratedFiles(QStringLiteral("temp/code/sd-core-proof/gen-hier-payload"), payloadFiles,
                                      &error)) {
            std::fprintf(stderr, "FAIL: hierarchical payload twin could not write gen-hier-payload/: %s\n",
                          qUtf8Printable(error));
            return 1;
        }

        // ---- the interpreter side, step by step ------------------------------
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: hierarchical payload twin: the agents did not register\n");
            return 1;
        }
        doc->restore(machine);

        struct Move {
            quint64 fromId;
            quint64 toId;
            quint64 viaTransitionId;
        };
        int obsOwner = 0;
        std::vector<app::events::ActiveStateChanged> moves;
        kernel.dispatcher().subscribe<app::events::ActiveStateChanged>(
            &obsOwner, [&moves](const app::events::ActiveStateChanged& fact) { moves.push_back(fact); });

        const auto step = [&](const char* what, std::initializer_list<quint64> config, qint64 level, bool enabled,
                              qint64 ticks, std::initializer_list<Move> expectedMoves) {
            const QVariantMap values = sim->contextValues();
            bool ok = sim->configuration() == std::vector<quint64>(config) &&
                      values.value(QStringLiteral("level")).toLongLong() == level &&
                      values.value(QStringLiteral("enabled")).toBool() == enabled &&
                      values.value(QStringLiteral("ticks")).toLongLong() == ticks &&
                      moves.size() == expectedMoves.size();
            if (ok) {
                auto expected = expectedMoves.begin();
                for (const app::events::ActiveStateChanged& actual : moves) {
                    ok = ok && actual.fromId == expected->fromId && actual.toId == expected->toId &&
                         actual.viaTransitionId == expected->viaTransitionId;
                    ++expected;
                }
            }
            // An undecidable guard counts as false and can still land plausibly; never let it pass a step.
            for (const app::GuardEvaluation& evaluation : sim->lastGuardEvaluations()) {
                ok = ok && evaluation.decided;
            }
            for (const QString& line : sim->trace()) {
                ok = ok && !line.contains(QStringLiteral("skipped:")) && !line.startsWith(QStringLiteral("warning:"));
            }
            moves.clear();
            if (ok) {
                return true;
            }
            QStringList configText;
            for (quint64 id : sim->configuration()) {
                configText << QString::number(id);
            }
            std::fprintf(stderr,
                          "FAIL: hierarchical payload twin: after %s the interpreter holds config={%s} level=%lld "
                          "enabled=%d ticks=%lld (or its moves / guard decisions / trace differ from the table)\n",
                          what, qUtf8Printable(configText.join(QLatin1Char(','))),
                          static_cast<long long>(values.value(QStringLiteral("level")).toLongLong()),
                          values.value(QStringLiteral("enabled")).toBool() ? 1 : 0,
                          static_cast<long long>(values.value(QStringLiteral("ticks")).toLongLong()));
            return false;
        };

        sim->run();
        moves.clear();  // run()'s own initial-descent fact is not a script step
        if (!step("run()", {1, 2}, 0, false, 0, {})) {
            return 1;
        }
        sim->sendEvent(QStringLiteral("Set"), QVariant::fromValue<qlonglong>(2));
        if (!step("Set(2)", {1, 2}, 2, false, 0, {})) {
            return 1;
        }
        sim->sendEvent(QStringLiteral("Set"), QVariant::fromValue<qlonglong>(5));
        if (!step("Set(5)", {1, 3}, 5, false, 0, {{2, 3, 8}})) {
            return 1;
        }
        sim->sendEvent(QStringLiteral("Flag"), QVariant(true));
        if (!step("Flag(true)", {1, 3}, 5, true, 0, {})) {
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go"));
        if (!step("Go", {4}, 5, true, 0, {{3, 4, 11}})) {
            return 1;
        }
        const int traceBeforeWildcard = static_cast<int>(sim->trace().size());
        sim->sendEvent(QStringLiteral("do.reset"));
        if (!step("do.reset", {1, 2}, 5, true, 0, {{4, 2, 12}})) {
            return 1;
        }
        if (sim->trace().mid(traceBeforeWildcard).filter(QStringLiteral("action: resetHit")).size() != 1) {
            std::fprintf(stderr, "FAIL: hierarchical payload twin: do.reset did not run resetHit exactly once\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Set"), QVariant::fromValue<qlonglong>(9));
        if (!step("Set(9)", {4}, 9, true, 1, {{2, 3, 8}, {3, 4, 13}})) {
            return 1;
        }
        int traceBefore = static_cast<int>(sim->trace().size());
        sim->sendEvent(QStringLiteral("do.set"), QVariant::fromValue<qlonglong>(4));
        if (!step("do.set(4)", {1, 3}, 4, true, 1, {{4, 3, 14}})) {
            return 1;
        }
        if (!sim->trace().mid(traceBefore).filter(QStringLiteral("action: resetHit")).isEmpty()) {
            std::fprintf(stderr, "FAIL: hierarchical payload twin: do.set(4) ran the wildcard row's resetHit\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go"));
        if (!step("Go (back to C)", {4}, 4, true, 1, {{3, 4, 11}})) {
            return 1;
        }
        traceBefore = static_cast<int>(sim->trace().size());
        sim->sendEvent(QStringLiteral("do.set"), QVariant::fromValue<qlonglong>(-1));
        if (!step("do.set(-1)", {1, 2}, 4, true, 1, {{4, 2, 12}})) {
            return 1;
        }
        if (sim->trace().mid(traceBefore).filter(QStringLiteral("action: resetHit")).size() != 1) {
            std::fprintf(stderr, "FAIL: hierarchical payload twin: do.set(-1) did not run resetHit exactly once\n");
            return 1;
        }
        kernel.dispatcher().unsubscribe(&obsOwner);

        std::printf(
            "PASS: state-designer hierarchical payload codegen twin (per-row `event` signatures + payload rows out "
            "of fireTransition(int) + selectToFire + mixed fire_doSetRequested + no context_.event + "
            "gen-hier-payload written + interpreter pinned: guard pass, bubble to P, root row from nested, "
            "payload-free, wildcard, always, exact beats wildcard, wildcard fallback)\n");
    }

    // ==== part 7: sanitizeIdentifier uppercase normalization & collision =======
    // SCREAMING_SNAKE_CASE and other patterns normalize to camelCase
    // (capitalizeFirst=false) and PascalCase (capitalizeFirst=true); "START" and
    // "start" both sanitize to "Start", which app::validate() flags as an Error.
    {
        struct TestCase {
            const char* raw;
            const char* camel;
            const char* pascal;
        };

        static const TestCase kCases[] = {
            {"SESSION_COMMITTED", "sessionCommitted", "SessionCommitted"},
            {"START_CALIBRATION_STEP", "startCalibrationStep", "StartCalibrationStep"},
            {"RESET", "reset", "Reset"},
            {"E_STOP", "eStop", "EStop"},
            {"A", "a", "A"},
            {"mouseClick", "mouseClick", "MouseClick"},
            {"logged_out", "loggedOut", "LoggedOut"},
            {"STEP_1", "step1", "Step1"},
            {"123_GO", "_123Go", "_123Go"},
            {"*", "wildcard", "Wildcard"},
        };

        for (const auto& tc : kCases) {
            const QString rawStr = QString::fromUtf8(tc.raw);
            const QString actualCamel = app::sanitizeIdentifier(rawStr, false);
            if (actualCamel != QString::fromUtf8(tc.camel)) {
                std::fprintf(stderr,
                             "FAIL: sanitizeIdentifier('%s', false) expected '%s', got '%s'\n",
                             tc.raw, tc.camel, qUtf8Printable(actualCamel));
                return 1;
            }
            const QString actualPascal = app::sanitizeIdentifier(rawStr, true);
            if (actualPascal != QString::fromUtf8(tc.pascal)) {
                std::fprintf(stderr,
                             "FAIL: sanitizeIdentifier('%s', true) expected '%s', got '%s'\n",
                             tc.raw, tc.pascal, qUtf8Printable(actualPascal));
                return 1;
            }
        }

        // New collision class: "START" and "start" both sanitize to "Start".
        // Verify that app::validate() flags this as an Error.
        {
            app::Machine machine;
            machine.name = QStringLiteral("CollisionMachine");
            machine.states.push_back(
                app::State{.id = 1, .name = QStringLiteral("Idle"), .kind = app::StateKind::Normal});
            machine.states.push_back(
                app::State{.id = 2, .name = QStringLiteral("Active"), .kind = app::StateKind::Normal});
            machine.transitions.push_back(
                app::Transition{.id = 3, .from = 1, .to = 2, .event = QStringLiteral("START")});
            machine.transitions.push_back(
                app::Transition{.id = 4, .from = 1, .to = 2, .event = QStringLiteral("start")});
            machine.nextId = 5;
            machine.initialStateId = 1;

            const QVector<app::Problem> problems = app::validate(machine);
            bool foundCollision = false;
            for (const app::Problem& p : problems) {
                if (p.severity == app::ProblemSeverity::Error &&
                    p.text.contains(QStringLiteral("both sanitize to the same identifier"))) {
                    foundCollision = true;
                    break;
                }
            }
            if (!foundCollision) {
                std::fprintf(stderr,
                             "FAIL: validate() did not flag 'START' and 'start' colliding after sanitization\n");
                return 1;
            }
        }
    }

    return 0;
}
