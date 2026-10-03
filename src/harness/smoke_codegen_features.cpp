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

#include "harness/harness.h"

int runCodegenFeaturesSmoke() {
    // ==== part 6: FLAT invoke codegen + textual shape + twin interpreter ======
    // Twin: run -> Go -> a live onDone completion (`output > 0` passes with 42; the
    // winning row's assign persists the payload). Guard-fails fallback, onError and
    // cancellation with a late completion are covered only by the compiled proof.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: invoke-codegen twin: the agents did not register\n");
            return 1;
        }
        registerEditCommands(kernel);

        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});    // id 1: Idle
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 0)});  // id 2: Loading
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, -60)});  // id 3: Success
        kernel.send(app::events::AddStateRequested{.pos = QPointF(300, 60)});   // id 4: Failure
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Idle")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Loading")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("Success")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Failure")});
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::SetInvokeSrcRequested{.stateId = 2, .src = QStringLiteral("fetchUser")});

        // ids 5-6: the schema, in document order.
        kernel.send(app::events::AddContextVariableRequested{});  // id 5
        kernel.send(app::events::RenameContextVariableRequested{.id = 5, .name = QStringLiteral("result")});
        kernel.send(app::events::AddContextVariableRequested{});  // id 6
        kernel.send(app::events::RenameContextVariableRequested{.id = 6, .name = QStringLiteral("lastError")});
        kernel.send(app::events::SetContextTypeRequested{.id = 6, .type = app::ContextType::String});
        kernel.send(app::events::SetContextInitialValueRequested{.id = 6, .initialValue = QString()});

        kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 7: Idle -Go-> Loading
        kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Go")});
        // id 8 must precede id 9: only the last row in a candidate group may be unguarded.
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 8: onDone[output>0] -> Success
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("done.invoke.fetchUser")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 8, .guard = QStringLiteral("output > 0")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 8, .action = QStringLiteral("result = output")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 4});  // id 9: onDone (unguarded) -> Failure
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 9, .event = QStringLiteral("done.invoke.fetchUser")});
        kernel.send(
            app::events::SetTransitionActionRequested{.id = 9, .action = QStringLiteral("lastError = 'empty'")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 4});  // id 10: onError -> Failure
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 10, .event = QStringLiteral("error.platform.fetchUser")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 10, .action = QStringLiteral("lastError = error")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 1});  // id 11: Loading -Cancel-> Idle
        kernel.send(app::events::SetTransitionEventRequested{.id = 11, .event = QStringLiteral("Cancel")});

        app::Machine invokeMachine = doc->machine();
        invokeMachine.name = QStringLiteral("Invoke Sample");
        doc->restore(invokeMachine);
        removeEditCommands(kernel);

        if (doc->machine().context.size() != 2 || doc->machine().transitions.size() != 5 ||
            app::isHierarchical(doc->machine())) {
            std::fprintf(stderr, "FAIL: invoke-codegen twin: the fixture is not a 2-variable/5-transition FLAT machine\n");
            return 1;
        }
        for (const app::Problem& problem : app::validate(doc->machine())) {
            if (problem.severity == app::ProblemSeverity::Error) {
                std::fprintf(stderr, "FAIL: invoke-codegen twin: the fixture machine has a validator Error: %s\n",
                              qUtf8Printable(problem.text));
                return 1;
            }
        }

        // ---- the emitted text ------------------------------------------------
        const QVector<app::GeneratedFile> invokeFiles = app::generate(doc->machine(), QStringLiteral("app::generated"));
        QString invokeCoreText;
        QString invokeHooksText;
        for (const app::GeneratedFile& file : invokeFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                invokeCoreText = file.content;
            }
            if (file.relativePath.endsWith(QStringLiteral("_hooks.h"))) {
                invokeHooksText = file.content;
            }
        }
        if (invokeCoreText.isEmpty() || invokeHooksText.isEmpty()) {
            std::fprintf(stderr, "FAIL: invoke-codegen twin: generate() emitted no invoke_sample core/hooks\n");
            return 1;
        }

        // The hooks file has one start<Service> pure virtual per service (not per
        // done/error transition) and includes <stop_token>/<functional>/<string>.
        static const char* const kExpectedHooksMarkers[] = {
            "#include <stop_token>",
            "struct InvokeSampleInvocations {",
            "virtual void startFetchUser(std::stop_token stopToken,",
            "std::function<void(long long output)> onDone,",
            "std::function<void(const std::string& error)> onError) = 0;",
        };
        for (const char* marker : kExpectedHooksMarkers) {
            if (!invokeHooksText.contains(QLatin1String(marker))) {
                std::fprintf(stderr, "FAIL: invoke-codegen twin: invoke_sample_hooks.h is missing '%s'\n", marker);
                return 1;
            }
        }
        if (invokeHooksText.count(QStringLiteral("virtual void start")) != 1) {
            std::fprintf(stderr,
                          "FAIL: invoke-codegen twin: expected exactly ONE start<Service> hook (one per distinct "
                          "service, not one per onDone/onError transition)\n");
            return 1;
        }

        // The core: armInvocations()/cancelInvocations() exist and the done/error
        // rows use `output`/`error` bare (never context_.output).
        static const char* const kExpectedCoreMarkers[] = {
            "void armInvocations() {",
            "void cancelInvocations() {",
            "fetchUserStopSource_.request_stop();",
            "fetchUserStopSource_ = std::stop_source{};",
            "invocations_.get().startFetchUser(",
            "if (output > 0LL) {",
            "context_.result = static_cast<long long>(output);",
            "context_.lastError = \"empty\";",
            "context_.lastError = error;",
            "std::reference_wrapper<InvokeSampleInvocations> invocations_;",
            "std::stop_source fetchUserStopSource_;",
        };
        for (const char* marker : kExpectedCoreMarkers) {
            if (!invokeCoreText.contains(QLatin1String(marker))) {
                std::fprintf(stderr, "FAIL: invoke-codegen twin: invoke_sample_core.h is missing '%s'\n", marker);
                return 1;
            }
        }
        // An ordinary transition leaving Loading (Cancel) must also call
        // cancelInvocations(), not just the onDone/onError rows.
        const int cancelRequestedIdx = invokeCoreText.indexOf(QStringLiteral("void cancelRequested() {"));
        if (cancelRequestedIdx < 0 ||
            !invokeCoreText.mid(cancelRequestedIdx, 200).contains(QStringLiteral("cancelInvocations();"))) {
            std::fprintf(stderr,
                          "FAIL: invoke-codegen twin: the ordinary Cancel event does not call cancelInvocations() "
                          "-- leaving the invoking state by an ordinary event must cancel too\n");
            return 1;
        }
        // done.invoke./error.platform. rows must not become public methods.
        if (invokeCoreText.contains(QStringLiteral("doneInvoke")) ||
            invokeCoreText.contains(QStringLiteral("errorPlatform"))) {
            std::fprintf(stderr,
                          "FAIL: invoke-codegen twin: an onDone/onError row leaked into the ordinary event "
                          "vocabulary as a public method\n");
            return 1;
        }
        if (invokeCoreText.contains(QStringLiteral("<ordo/")) || invokeHooksText.contains(QStringLiteral("<ordo/"))) {
            std::fprintf(stderr, "FAIL: invoke-codegen twin: an invoke file leaks an <ordo/ include\n");
            return 1;
        }
        for (const QString& text : {invokeCoreText, invokeHooksText}) {
            for (int i = 0; i + 1 < text.size(); ++i) {
                if (text.at(i) == QLatin1Char('Q') && text.at(i + 1).isUpper()) {
                    std::fprintf(stderr, "FAIL: invoke-codegen twin: an invoke file leaks a Qt type (Q[A-Z])\n");
                    return 1;
                }
            }
        }

        // ---- the interpreter side ---------------------------------------------
        const auto configIs = [&sim](quint64 expected) {
            return sim->configuration() == std::vector<quint64>{expected};
        };
        const auto contextIs = [&sim](qint64 result, const char* lastError) {
            const QVariantMap values = sim->contextValues();
            return values.value(QStringLiteral("result")).toLongLong() == result &&
                   values.value(QStringLiteral("lastError")).toString() == QLatin1String(lastError);
        };
        sim->run();
        if (!configIs(1) || !contextIs(0, "")) {
            std::fprintf(stderr, "FAIL: invoke-codegen twin: run() did not land on Idle with the seeded context\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Go"));
        if (!configIs(2) || !contextIs(0, "")) {
            std::fprintf(stderr, "FAIL: invoke-codegen twin: Go did not reach Loading unchanged\n");
            return 1;
        }
        sim->completeInvocation(QStringLiteral("fetchUser"), /*ok=*/true, QVariant(qint64(42)));
        if (!configIs(3) || !contextIs(42, "")) {
            std::fprintf(stderr,
                          "FAIL: invoke-codegen twin: onDone(42) did not take the 'output > 0' branch to Success "
                          "with result=42\n");
            return 1;
        }

        // ---- the compile-proof inputs ------------------------------------------
        QString invokeError;
        if (!app::writeGeneratedFiles(QStringLiteral("temp/code/sd-core-proof/gen-invoke"), invokeFiles,
                                       &invokeError)) {
            std::fprintf(stderr, "FAIL: invoke-codegen twin could not write gen-invoke/: %s\n",
                          qUtf8Printable(invokeError));
            return 1;
        }
    }

    // ==== part 7: HIERARCHICAL invoke -- twin interpreter + compile guard =====
    // Leaving A for a top-level sibling of Parent puts Parent in the exit set too.
    // The interpreter pins onDone's passing branch (42 -> B) and onError's real
    // message reaching lastError; the unguarded fallback and cancellation with a
    // late completion no-op (`if (!active_[1]) return;`) are covered only by the
    // compiled proof.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: hier-invoke twin: the agents did not register\n");
            return 1;
        }
        registerEditCommands(kernel);
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});     // id 1: Parent
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 0)});    // id 2: A (invokes)
        kernel.send(app::events::AddStateRequested{.pos = QPointF(100, 0)});   // id 3: B
        kernel.send(app::events::AddStateRequested{.pos = QPointF(150, 60)});  // id 4: Failure
        kernel.send(app::events::AddStateRequested{.pos = QPointF(50, 60)});   // id 5: Cancelled
        kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("Parent")});
        kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("A")});
        kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("B")});
        kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Failure")});
        kernel.send(app::events::RenameStateRequested{.id = 5, .name = QStringLiteral("Cancelled")});
        kernel.send(app::events::ReparentStateRequested{.id = 2, .parentId = 1});  // A -> Parent (initialChildId)
        kernel.send(app::events::ReparentStateRequested{.id = 3, .parentId = 1});  // B -> Parent
        // Failure (4) and Cancelled (5) stay top-level, so transitions into them
        // cross Parent's boundary (LCCA = the machine root).
        kernel.send(app::events::SetInitialStateRequested{.id = 1});
        kernel.send(app::events::SetInvokeSrcRequested{.stateId = 2, .src = QStringLiteral("loadThing")});

        kernel.send(app::events::AddContextVariableRequested{});  // id 6: result (Int/"0" defaults, no retype)
        kernel.send(app::events::RenameContextVariableRequested{.id = 6, .name = QStringLiteral("result")});
        kernel.send(app::events::AddContextVariableRequested{});  // id 7: lastError
        kernel.send(app::events::RenameContextVariableRequested{.id = 7, .name = QStringLiteral("lastError")});
        kernel.send(app::events::SetContextTypeRequested{.id = 7, .type = app::ContextType::String});
        kernel.send(app::events::SetContextInitialValueRequested{.id = 7, .initialValue = QString()});

        kernel.send(
            app::events::SetExitActionsRequested{.id = 1, .exitActions = QStringList{QStringLiteral("exitParent")}});
        kernel.send(
            app::events::SetExitActionsRequested{.id = 2, .exitActions = QStringList{QStringLiteral("exitA")}});
        kernel.send(
            app::events::SetEntryActionsRequested{.id = 3, .entryActions = QStringList{QStringLiteral("enterB")}});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = 4, .entryActions = QStringList{QStringLiteral("enterFailure")}});
        kernel.send(app::events::SetEntryActionsRequested{
            .id = 5, .entryActions = QStringList{QStringLiteral("enterCancelled")}});

        // id 8 must precede id 9: only the last row in a candidate group may be unguarded.
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 8: onDone[output>0] -> B
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("done.invoke.loadThing")});
        kernel.send(app::events::SetTransitionGuardRequested{.id = 8, .guard = QStringLiteral("output > 0")});
        kernel.send(app::events::SetTransitionActionRequested{.id = 8, .action = QStringLiteral("result = output")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 4});  // id 9: onDone (unguarded) -> Failure
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 9, .event = QStringLiteral("done.invoke.loadThing")});
        kernel.send(
            app::events::SetTransitionActionRequested{.id = 9, .action = QStringLiteral("lastError = 'empty'")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 4});  // id 10: onError -> Failure
        kernel.send(
            app::events::SetTransitionEventRequested{.id = 10, .event = QStringLiteral("error.platform.loadThing")});
        kernel.send(
            app::events::SetTransitionActionRequested{.id = 10, .action = QStringLiteral("lastError = error")});
        kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 5});  // id 11: Cancel (ordinary) -> Cancelled
        kernel.send(app::events::SetTransitionEventRequested{.id = 11, .event = QStringLiteral("Cancel")});

        app::Machine hierInvokeMachine = doc->machine();
        hierInvokeMachine.name = QStringLiteral("Hier Invoke Sample");
        doc->restore(hierInvokeMachine);
        removeEditCommands(kernel);

        if (!app::isHierarchical(doc->machine())) {
            std::fprintf(stderr, "FAIL: hier-invoke twin: the fixture did not read as hierarchical\n");
            return 1;
        }
        for (const app::Problem& problem : app::validate(doc->machine())) {
            if (problem.severity == app::ProblemSeverity::Error) {
                std::fprintf(stderr, "FAIL: hier-invoke twin: the fixture has a validator Error: %s\n",
                              qUtf8Printable(problem.text));
                return 1;
            }
        }

        // ---- the emitted text (state indices: Parent=0, A=1, B=2) -------------
        const QVector<app::GeneratedFile> hierInvokeFiles =
            app::generate(doc->machine(), QStringLiteral("app::generated"));
        QString hierInvokeCoreText;
        for (const app::GeneratedFile& file : hierInvokeFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                hierInvokeCoreText = file.content;
            }
        }
        static const char* const kExpectedHierInvokeMarkers[] = {
            "void armStateInvocation(int stateIndex) {",
            "void cancelStateInvocation(int stateIndex) {",
            "loadThingStopSource_.request_stop();",
            "if (!active_[1]) return;",  // A's own index (Parent=0, A=1, B=2, Failure=3, Cancelled=4)
            "invocations_.get().startLoadThing(",
        };
        for (const char* marker : kExpectedHierInvokeMarkers) {
            if (!hierInvokeCoreText.contains(QLatin1String(marker))) {
                std::fprintf(stderr, "FAIL: hier-invoke twin: hier_invoke_sample_core.h is missing '%s'\n", marker);
                return 1;
            }
        }

        // ---- the interpreter side
        const auto configEquals = [](const std::vector<quint64>& actual, std::initializer_list<quint64> expected) {
            return actual == std::vector<quint64>(expected);
        };
        const auto contextIs = [&sim](qint64 result, const char* lastError) {
            const QVariantMap values = sim->contextValues();
            return values.value(QStringLiteral("result")).toLongLong() == result &&
                   values.value(QStringLiteral("lastError")).toString() == QLatin1String(lastError);
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

        sim->run();  // service entrance: descends Parent -> A, arming A's invoke
        if (!configEquals(sim->configuration(), {1, 2}) || !contextIs(0, "")) {
            std::fprintf(stderr, "FAIL: hier-invoke twin: run() did not land on {Parent,A} with the seeded context\n");
            return 1;
        }
        sim->completeInvocation(QStringLiteral("loadThing"), /*ok=*/true, QVariant(qint64(42)));
        if (!configEquals(sim->configuration(), {1, 3}) || !contextIs(42, "")) {
            std::fprintf(stderr,
                          "FAIL: hier-invoke twin: onDone(42) did not take the 'output > 0' branch to {Parent,B} "
                          "with result=42\n");
            return 1;
        }
        if (!orderedInTrace(sim->trace(), {"action: exitA", "action: enterB"})) {
            std::fprintf(stderr, "FAIL: hier-invoke twin: onDone(42)'s exit/entry action order was wrong\n");
            return 1;
        }

        // onError's real message must reach `lastError`. Uses a fresh kernel:
        // `sim` already completed A's invoke, so a second completion there would
        // be a stale no-op.
        {
            ordo::core::Kernel errorKernel;
            errorKernel.registerAgent(std::make_shared<app::MachineDocAgent>());
            errorKernel.registerAgent(std::make_shared<app::SimulationAgent>());
            auto errorDoc = errorKernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
            auto errorSim = errorKernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
            if (!errorDoc || !errorSim) {
                std::fprintf(stderr, "FAIL: hier-invoke twin: the onError twin's agents did not register\n");
                return 1;
            }
            errorDoc->restore(hierInvokeMachine);
            errorSim->run();  // lands on {Parent,A}, exactly like `sim` above
            errorSim->completeInvocation(QStringLiteral("loadThing"), /*ok=*/false,
                                          QVariant(QStringLiteral("TIMEOUT")));
            const QString gotLastError = errorSim->contextValues().value(QStringLiteral("lastError")).toString();
            if (!configEquals(errorSim->configuration(), {4}) || gotLastError != QLatin1String("TIMEOUT")) {
                std::fprintf(stderr,
                              "FAIL: hier-invoke twin: onError(\"TIMEOUT\") did not reach {Failure} with the real "
                              "message in lastError (got lastError=\"%s\")\n",
                              qUtf8Printable(gotLastError));
                return 1;
            }
        }

        QString hierInvokeError;
        if (!app::writeGeneratedFiles(QStringLiteral("temp/code/sd-core-proof/gen-hier-invoke"), hierInvokeFiles,
                                       &hierInvokeError)) {
            std::fprintf(stderr, "FAIL: hier-invoke twin could not write gen-hier-invoke/: %s\n",
                          qUtf8Printable(hierInvokeError));
            return 1;
        }
    }

    // ==== part 8: reenter & always codegen twin ============================
    // Flat: an internal self-transition (reenter == false) skips exit/entry/transitionTo,
    // an external one runs all of them; the always methods are emitted and called from
    // the ctor and public event methods. Hierarchical: an internal self fireTransitionN()
    // skips exitSequence/entrySequence; selectAndFireAlways() and friends are emitted.
    {
        // 8a: Flat machine with reenter:false vs reenter:true and always transitions
        app::Machine flatMachine;
        flatMachine.name = QStringLiteral("flat_ra");
        flatMachine.nextId = 10;
        flatMachine.initialStateId = 1;

        app::State s1;
        s1.id = 1;
        s1.name = QStringLiteral("Working");
        s1.entryActions = {QStringLiteral("enterWorking")};
        s1.exitActions = {QStringLiteral("exitWorking")};

        app::State s2;
        s2.id = 2;
        s2.name = QStringLiteral("Done");

        flatMachine.states = {s1, s2};

        // Transition 1: internal self-transition (reenter == false)
        app::Transition tInternal;
        tInternal.id = 3;
        tInternal.from = 1;
        tInternal.to = 1;
        tInternal.event = QStringLiteral("Tick");
        tInternal.action = QStringLiteral("onTick");
        tInternal.reenter = false;

        // Transition 2: external self-transition (reenter == true)
        app::Transition tExternal;
        tExternal.id = 4;
        tExternal.from = 1;
        tExternal.to = 1;
        tExternal.event = QStringLiteral("Reset");
        tExternal.action = QStringLiteral("onReset");
        tExternal.reenter = true;

        // Transition 3: always transition from Working -> Done (guarded)
        app::Transition tAlways;
        tAlways.id = 5;
        tAlways.from = 1;
        tAlways.to = 2;
        tAlways.always = true;
        tAlways.guard = QStringLiteral("isComplete");
        tAlways.action = QStringLiteral("onComplete");

        flatMachine.transitions = {tInternal, tExternal, tAlways};

        const QVector<app::GeneratedFile> flatFiles = app::generate(flatMachine, QStringLiteral("app::generated"));
        QString flatCoreText;
        for (const app::GeneratedFile& file : flatFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                flatCoreText = file.content;
            }
        }
        if (flatCoreText.isEmpty()) {
            std::fprintf(stderr, "FAIL: flat reenter/always codegen: generate() emitted no core file\n");
            return 1;
        }

        // Check internal self-transition marker in Tick
        if (!flatCoreText.contains(QStringLiteral("// internal self-transition (reenter == false): action only, skip exit/entry sequences"))) {
            std::fprintf(stderr, "FAIL: flat reenter/always codegen: missing internal self-transition comment in Tick\n");
            return 1;
        }
        // Verify that in tickRequested, actions_.get().onTick() is present, but transitionTo(FlatRaState::Working) is NOT present in tickRequested
        const int tickIdx = flatCoreText.indexOf(QStringLiteral("void tickRequested() {"));
        const int resetIdx = flatCoreText.indexOf(QStringLiteral("void resetRequested() {"));
        if (tickIdx < 0 || resetIdx < 0) {
            std::fprintf(stderr, "FAIL: flat reenter/always codegen: missing tickRequested() or resetRequested() methods\n");
            return 1;
        }
        const QString tickBody = flatCoreText.mid(tickIdx, resetIdx - tickIdx);
        if (tickBody.contains(QStringLiteral("transitionTo(")) || tickBody.contains(QStringLiteral("exitWorking")) || tickBody.contains(QStringLiteral("enterWorking"))) {
            std::fprintf(stderr, "FAIL: flat reenter/always codegen: internal self-transition in tickRequested() must NOT call exit/entry/transitionTo\n");
            return 1;
        }
        if (!tickBody.contains(QStringLiteral("actions_.get().onTick()"))) {
            std::fprintf(stderr, "FAIL: flat reenter/always codegen: tick() must execute action onTick\n");
            return 1;
        }

        // Verify reset() (reenter == true) calls transitionTo, exit, entry
        const QString resetBody = flatCoreText.mid(resetIdx, 500);
        if (!resetBody.contains(QStringLiteral("transitionTo(FlatRaState::Working)")) || !resetBody.contains(QStringLiteral("actions_.get().onReset()"))) {
            std::fprintf(stderr, "FAIL: flat reenter/always codegen: external self-transition in reset() must call transitionTo\n");
            return 1;
        }

        // Verify always methods generated and hooked
        if (!flatCoreText.contains(QStringLiteral("bool stepAlwaysTransitions()")) ||
            !flatCoreText.contains(QStringLiteral("void checkAlwaysTransitions()"))) {
            std::fprintf(stderr, "FAIL: flat reenter/always codegen: missing stepAlwaysTransitions() or checkAlwaysTransitions()\n");
            return 1;
        }
        if (!flatCoreText.contains(QStringLiteral("checkAlwaysTransitions();"))) {
            std::fprintf(stderr, "FAIL: flat reenter/always codegen: checkAlwaysTransitions() call missing in flat core\n");
            return 1;
        }

        // 8b: Hierarchical machine with reenter:false and always transitions
        app::Machine hierMachine;
        hierMachine.name = QStringLiteral("hier_ra");
        hierMachine.nextId = 20;
        hierMachine.initialStateId = 10;

        app::State hp;
        hp.id = 10;
        hp.name = QStringLiteral("RootState");
        hp.initialChildId = 11;

        app::State hc1;
        hc1.id = 11;
        hc1.name = QStringLiteral("Active");
        hc1.parentId = 10;

        app::State hc2;
        hc2.id = 12;
        hc2.name = QStringLiteral("Finished");
        hc2.parentId = 10;

        hierMachine.states = {hp, hc1, hc2};

        // Internal self transition on Active
        app::Transition htInternal;
        htInternal.id = 13;
        htInternal.from = 11;
        htInternal.to = 11;
        htInternal.event = QStringLiteral("Pulse");
        htInternal.action = QStringLiteral("doPulse");
        htInternal.reenter = false;

        // Always transition Active -> Finished
        app::Transition htAlways;
        htAlways.id = 14;
        htAlways.from = 11;
        htAlways.to = 12;
        htAlways.always = true;
        htAlways.guard = QStringLiteral("shouldFinish");
        htAlways.action = QStringLiteral("doFinish");

        hierMachine.transitions = {htInternal, htAlways};

        const QVector<app::GeneratedFile> hierFiles = app::generate(hierMachine, QStringLiteral("app::generated"));
        QString hierCoreText;
        for (const app::GeneratedFile& file : hierFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                hierCoreText = file.content;
            }
        }
        if (hierCoreText.isEmpty()) {
            std::fprintf(stderr, "FAIL: hier reenter/always codegen: generate() emitted no hier core file\n");
            return 1;
        }

        // Verify internal self transition in fireTransition0()
        if (!hierCoreText.contains(QStringLiteral("// internal self-transition (reenter == false): action only, skip exit/entry sequences"))) {
            std::fprintf(stderr, "FAIL: hier reenter/always codegen: missing internal self comment in fireTransition0\n");
            return 1;
        }
        if (!hierCoreText.contains(QStringLiteral("actions_.get().doPulse()"))) {
            std::fprintf(stderr, "FAIL: hier reenter/always codegen: missing doPulse action\n");
            return 1;
        }

        // Verify selectAndFireAlways & matchAt_always & checkAlwaysTransitions
        if (!hierCoreText.contains(QStringLiteral("bool selectAndFireAlways(")) ||
            !hierCoreText.contains(QStringLiteral("int matchAt_always(int stateIndex) const")) ||
            !hierCoreText.contains(QStringLiteral("void checkAlwaysTransitions()"))) {
            std::fprintf(stderr, "FAIL: hier reenter/always codegen: missing selectAndFireAlways or matchAt_always\n");
            return 1;
        }
    }

    // ==== part 8c: flat & hierarchical raise() codegen & twin interpreter =====
    {
        // 1. Flat machine with chained raise actions
        app::Machine flatRaise;
        flatRaise.name = QStringLiteral("flat_raise");
        flatRaise.nextId = 10;
        flatRaise.initialStateId = 1;

        app::State sIdle{.id = 1, .name = QStringLiteral("Idle"), .kind = app::StateKind::Normal};
        app::State sProc{.id = 2, .name = QStringLiteral("Processing"), .kind = app::StateKind::Normal};
        sProc.entryActions = {QStringLiteral("raise(Step1)")};
        app::State sPhase2{.id = 3, .name = QStringLiteral("Phase2"), .kind = app::StateKind::Normal};
        sPhase2.entryActions = {QStringLiteral("raise(Complete)")};
        app::State sDone{.id = 4, .name = QStringLiteral("Done"), .kind = app::StateKind::Normal};

        flatRaise.states = {sIdle, sProc, sPhase2, sDone};

        app::Transition tStart{.id = 5, .from = 1, .to = 2, .event = QStringLiteral("Start")};
        app::Transition tStep1{.id = 6, .from = 2, .to = 3, .event = QStringLiteral("Step1"), .action = QStringLiteral("raise(Step2)")};
        app::Transition tComplete{.id = 7, .from = 3, .to = 4, .event = QStringLiteral("Complete")};

        flatRaise.transitions = {tStart, tStep1, tComplete};

        // Pin JIT simulation trace
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: raise codegen twin: could not register doc or sim agents\n");
            return 1;
        }
        doc->restore(flatRaise);
        sim->run();
        if (sim->activeStateId() != 1) {
            std::fprintf(stderr, "FAIL: raise codegen twin: initial state should be Idle (1)\n");
            return 1;
        }
        sim->sendEvent(QStringLiteral("Start"));
        if (sim->activeStateId() != 4) {
            std::fprintf(stderr, "FAIL: raise codegen twin: microstep cascade should reach Done (4), got %llu\n",
                         static_cast<unsigned long long>(sim->activeStateId()));
            return 1;
        }

        // Verify AOT generated flat core
        const QVector<app::GeneratedFile> flatFiles = app::generate(flatRaise, QStringLiteral("app::generated"));
        QString flatCoreText;
        for (const app::GeneratedFile& file : flatFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                flatCoreText = file.content;
            }
        }
        if (flatCoreText.isEmpty()) {
            std::fprintf(stderr, "FAIL: raise codegen twin: generate() emitted no flat core file\n");
            return 1;
        }
        if (!flatCoreText.contains(QStringLiteral("#include <deque>"))) {
            std::fprintf(stderr, "FAIL: raise codegen twin: missing #include <deque> in flat core\n");
            return 1;
        }
        if (!flatCoreText.contains(QStringLiteral("enum class Event")) ||
            !flatCoreText.contains(QStringLiteral("void raise(Event event)")) ||
            !flatCoreText.contains(QStringLiteral("std::deque<Event> internalQueue_;")) ||
            !flatCoreText.contains(QStringLiteral("bool processingMicrosteps_ = false;"))) {
            std::fprintf(stderr, "FAIL: raise codegen twin: missing Event enum, raise(), or internalQueue_ in flat core\n");
            return 1;
        }
        if (!flatCoreText.contains(QStringLiteral("raise(Event::Step1);")) ||
            !flatCoreText.contains(QStringLiteral("raise(Event::Step2);")) ||
            !flatCoreText.contains(QStringLiteral("raise(Event::Complete);"))) {
            std::fprintf(stderr, "FAIL: raise codegen twin: missing emitted raise(Event::...) action calls\n");
            return 1;
        }
        if (!flatCoreText.contains(QStringLiteral("void dispatchInternal(Event event)"))) {
            std::fprintf(stderr, "FAIL: raise codegen twin: missing dispatchInternal in flat core\n");
            return 1;
        }

        // 2. Hierarchical machine with raise and always transitions
        app::Machine hierRaise;
        hierRaise.name = QStringLiteral("hier_raise_always");
        hierRaise.nextId = 30;
        hierRaise.initialStateId = 20;

        app::State hRoot{.id = 20, .name = QStringLiteral("Root"), .kind = app::StateKind::Normal};
        hRoot.initialChildId = 21;
        app::State hA{.id = 21, .name = QStringLiteral("A"), .kind = app::StateKind::Normal};
        hA.parentId = 20;
        hA.entryActions = {QStringLiteral("raise(Ping)")};
        app::State hB{.id = 22, .name = QStringLiteral("B"), .kind = app::StateKind::Normal};
        hB.parentId = 20;
        app::State hDone{.id = 23, .name = QStringLiteral("Finished"), .kind = app::StateKind::Normal};
        hDone.parentId = 20;

        hierRaise.states = {hRoot, hA, hB, hDone};

        app::Transition htPing{.id = 24, .from = 21, .to = 22, .event = QStringLiteral("Ping"), .action = QStringLiteral("raise(Pong)")};
        app::Transition htAlways{.id = 25, .from = 22, .to = 23, .action = QStringLiteral("finishAction"), .always = true};

        hierRaise.transitions = {htPing, htAlways};

        const QVector<app::GeneratedFile> hierFiles = app::generate(hierRaise, QStringLiteral("app::generated"));
        QString hierCoreText;
        for (const app::GeneratedFile& file : hierFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                hierCoreText = file.content;
            }
        }
        if (hierCoreText.isEmpty()) {
            std::fprintf(stderr, "FAIL: hier raise codegen twin: generate() emitted no hier core file\n");
            return 1;
        }
        if (!hierCoreText.contains(QStringLiteral("#include <deque>"))) {
            std::fprintf(stderr, "FAIL: hier raise codegen twin: missing #include <deque> in hier core\n");
            return 1;
        }
        if (!hierCoreText.contains(QStringLiteral("enum class Event")) ||
            !hierCoreText.contains(QStringLiteral("void raise(Event event)")) ||
            !hierCoreText.contains(QStringLiteral("std::deque<Event> internalQueue_;")) ||
            !hierCoreText.contains(QStringLiteral("selectAndFireAlways")) ||
            !hierCoreText.contains(QStringLiteral("raise(Event::Ping);")) ||
            !hierCoreText.contains(QStringLiteral("raise(Event::Pong);"))) {
            std::fprintf(stderr, "FAIL: hier raise codegen twin: missing Event, raise, or selectAndFireAlways\n");
            return 1;
        }
    }

    // ==== actor communication codegen twin (sendTo + sendParent) ====
    {
        // 1. Flat machine
        app::Machine flatMachine;
        flatMachine.name = QStringLiteral("ActorCommFlat");
        flatMachine.initialStateId = 1;

        app::State sIdle;
        sIdle.id = 1;
        sIdle.name = QStringLiteral("Idle");
        flatMachine.states.push_back(sIdle);

        app::State sActive;
        sActive.id = 2;
        sActive.name = QStringLiteral("Active");
        sActive.entryActions = QStringList{QStringLiteral("sendParent(CHILD_READY)")};
        flatMachine.states.push_back(sActive);

        app::Transition t1;
        t1.id = 3;
        t1.from = 1;
        t1.to = 2;
        t1.event = QStringLiteral("Start");
        t1.action = QStringLiteral("sendTo(downloader, FETCH)");
        flatMachine.transitions.push_back(t1);

        const QVector<app::GeneratedFile> flatFiles = app::generate(flatMachine, QStringLiteral("app::generated"));

        QString flatHooksText;
        QString flatCoreText;
        for (const auto& file : flatFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_hooks.h"))) {
                flatHooksText = file.content;
            } else if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                flatCoreText = file.content;
            }
        }
        if (!flatHooksText.contains(QStringLiteral("virtual void sendTo_downloader_FETCH() = 0;")) ||
            !flatHooksText.contains(QStringLiteral("virtual void sendParent_CHILD_READY() = 0;"))) {
            std::fprintf(stderr, "FAIL: flat hooks missing sendTo_downloader_FETCH or sendParent_CHILD_READY\n");
            return 1;
        }
        if (!flatCoreText.contains(QStringLiteral("actions_.get().sendTo_downloader_FETCH();")) ||
            !flatCoreText.contains(QStringLiteral("actions_.get().sendParent_CHILD_READY();"))) {
            std::fprintf(stderr, "FAIL: flat core missing action invocation calls\n");
            return 1;
        }

        // 2. Hierarchical machine
        app::Machine hierMachine;
        hierMachine.name = QStringLiteral("ActorCommHier");
        hierMachine.initialStateId = 10;

        app::State hRoot{.id = 10, .name = QStringLiteral("Root"), .kind = app::StateKind::Normal};
        hRoot.initialChildId = 11;
        app::State hA{.id = 11, .name = QStringLiteral("A"), .kind = app::StateKind::Normal};
        hA.parentId = 10;
        hA.entryActions = {QStringLiteral("sendParent(CHILD_READY)")};
        app::State hB{.id = 12, .name = QStringLiteral("B"), .kind = app::StateKind::Normal};
        hB.parentId = 10;
        hierMachine.states = {hRoot, hA, hB};

        app::Transition ht1{.id = 13, .from = 11, .to = 12, .event = QStringLiteral("Start"), .action = QStringLiteral("sendTo(downloader, FETCH)")};
        hierMachine.transitions = {ht1};

        const QVector<app::GeneratedFile> hierFiles = app::generate(hierMachine, QStringLiteral("app::generated"));
        QString hierHooksText;
        QString hierCoreText;
        for (const auto& file : hierFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_hooks.h"))) {
                hierHooksText = file.content;
            } else if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                hierCoreText = file.content;
            }
        }
        if (!hierHooksText.contains(QStringLiteral("virtual void sendTo_downloader_FETCH() = 0;")) ||
            !hierHooksText.contains(QStringLiteral("virtual void sendParent_CHILD_READY() = 0;"))) {
            std::fprintf(stderr, "FAIL: hier hooks missing sendTo_downloader_FETCH or sendParent_CHILD_READY\n");
            return 1;
        }
        if (!hierCoreText.contains(QStringLiteral("actions_.get().sendTo_downloader_FETCH();")) ||
            !hierCoreText.contains(QStringLiteral("actions_.get().sendParent_CHILD_READY();"))) {
            std::fprintf(stderr, "FAIL: hier core missing action invocation calls\n");
            return 1;
        }
    }

    // ==== part 11: wildcard event codegen & twin verification =================
    {
        // 1. Flat machine wildcard fallback chaining
        app::Machine flatMachine;
        flatMachine.name = QStringLiteral("WildcardTwinFlat");
        flatMachine.initialStateId = 1;

        app::State sActive{.id = 1, .name = QStringLiteral("Active"), .kind = app::StateKind::Normal};
        app::State sExact{.id = 2, .name = QStringLiteral("ExactDone"), .kind = app::StateKind::Normal};
        app::State sPrefix{.id = 3, .name = QStringLiteral("PrefixDone"), .kind = app::StateKind::Normal};
        app::State sUniversal{.id = 4, .name = QStringLiteral("UniversalDone"), .kind = app::StateKind::Normal};
        app::State sRoot{.id = 5, .name = QStringLiteral("RootDone"), .kind = app::StateKind::Normal};
        flatMachine.states = {sActive, sExact, sPrefix, sUniversal, sRoot};

        // Active state has:
        // 1. mouse.click (guarded) -> ExactDone (spec 200)
        // 2. mouse.* (unguarded) -> PrefixDone (spec 101)
        // 3. * (unguarded) -> UniversalDone (spec 0)
        app::Transition tExact{.id = 10, .from = 1, .to = 2, .event = QStringLiteral("mouse.click"), .guard = QStringLiteral("canClick")};
        app::Transition tPrefix{.id = 11, .from = 1, .to = 3, .event = QStringLiteral("mouse.*")};
        app::Transition tUniv{.id = 12, .from = 1, .to = 4, .event = QStringLiteral("*")};
        // Root fallback: * -> RootDone
        app::Transition tRoot{.id = 13, .from = 0, .to = 5, .event = QStringLiteral("*")};
        flatMachine.transitions = {tExact, tPrefix, tUniv, tRoot};

        const QVector<app::GeneratedFile> flatFiles = app::generate(flatMachine, QStringLiteral("app::generated"));
        QString flatCoreText;
        for (const auto& file : flatFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                flatCoreText = file.content;
            }
        }

        // Verify generated flat methods exist and chain fallbacks
        if (!flatCoreText.contains(QStringLiteral("void mouseClickRequested()")) ||
            !flatCoreText.contains(QStringLiteral("void mouseWildcardRequested()")) ||
            !flatCoreText.contains(QStringLiteral("void wildcardRequested()"))) {
            std::fprintf(stderr, "FAIL: flat wildcard codegen missing expected event methods\n");
            return 1;
        }

        // Inside mouseClickRequested():
        // Guard check for mouse.click first -> transitionTo ExactDone
        // Followed by mouse.* fallback -> transitionTo PrefixDone
        // Followed by * fallback -> transitionTo UniversalDone
        const int idxMouseClick = flatCoreText.indexOf(QStringLiteral("void mouseClickRequested()"));
        const int idxCanClick = flatCoreText.indexOf(QStringLiteral("guards_.get().canClick()"), idxMouseClick);
        const int idxExactDone = flatCoreText.indexOf(QStringLiteral("transitionTo(WildcardTwinFlatState::ExactDone)"), idxCanClick);
        const int idxPrefixDone = flatCoreText.indexOf(QStringLiteral("transitionTo(WildcardTwinFlatState::PrefixDone)"), idxExactDone);
        const int idxUniversalDone = flatCoreText.indexOf(QStringLiteral("transitionTo(WildcardTwinFlatState::UniversalDone)"), idxPrefixDone);

        if (idxMouseClick == -1 || idxCanClick == -1 || idxExactDone == -1 || idxPrefixDone == -1 || idxUniversalDone == -1 ||
            !(idxMouseClick < idxCanClick && idxCanClick < idxExactDone && idxExactDone < idxPrefixDone && idxPrefixDone < idxUniversalDone)) {
            std::fprintf(stderr, "FAIL: flat wildcard codegen mouseClick() did not chain candidates in specificity order\n");
            return 1;
        }

        // 2. Hierarchical machine wildcard matchAt_ and matchRoot_ chaining
        app::Machine hierMachine;
        hierMachine.name = QStringLiteral("WildcardTwinHier");
        hierMachine.initialStateId = 20;

        app::State hRoot{.id = 20, .name = QStringLiteral("Root"), .kind = app::StateKind::Normal};
        hRoot.initialChildId = 21;
        app::State hChild{.id = 21, .name = QStringLiteral("Child"), .kind = app::StateKind::Normal};
        hChild.parentId = 20;
        app::State hTarget{.id = 22, .name = QStringLiteral("Target"), .kind = app::StateKind::Normal};
        hTarget.parentId = 20;
        hierMachine.states = {hRoot, hChild, hTarget};

        app::Transition ht1{.id = 30, .from = 21, .to = 22, .event = QStringLiteral("action.*")};
        app::Transition ht2{.id = 31, .from = 20, .to = 22, .event = QStringLiteral("*")};
        hierMachine.transitions = {ht1, ht2};

        const QVector<app::GeneratedFile> hierFiles = app::generate(hierMachine, QStringLiteral("app::generated"));
        QString hierCoreText;
        for (const auto& file : hierFiles) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                hierCoreText = file.content;
            }
        }
        if (!hierCoreText.contains(QStringLiteral("int matchAt_actionWildcardRequested(int stateIndex) const")) ||
            !hierCoreText.contains(QStringLiteral("int matchAt_wildcardRequested(int stateIndex) const"))) {
            std::fprintf(stderr, "FAIL: hier wildcard codegen missing matchAt methods\n");
            return 1;
        }

        // 3. JIT simulation twin agreement with the declared flat machine
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: wildcard twin sim agents could not register\n");
            return 1;
        }
        doc->restore(flatMachine);

        // Case A: mouse.click with guard true -> lands on ExactDone (2)
        sim->run();
        sim->setGuardResult(QStringLiteral("canClick"), true);
        sim->sendEvent(QStringLiteral("mouse.click"));
        if (sim->configuration() != std::vector<quint64>{2}) {
            std::fprintf(stderr, "FAIL: wildcard twin sim: mouse.click with guard true did not land on ExactDone (got %llu)\n",
                         sim->configuration().empty() ? 0ULL : sim->configuration().front());
            return 1;
        }

        // Case B: mouse.click with guard false -> falls back to mouse.* -> lands on PrefixDone (3)
        sim->reset();
        sim->run();
        sim->setGuardResult(QStringLiteral("canClick"), false);
        sim->sendEvent(QStringLiteral("mouse.click"));
        if (sim->configuration() != std::vector<quint64>{3}) {
            std::fprintf(stderr, "FAIL: wildcard twin sim: mouse.click with guard false did not fall back to PrefixDone (got %llu)\n",
                         sim->configuration().empty() ? 0ULL : sim->configuration().front());
            return 1;
        }

        // Case C: mouse.move (unlisted exact) -> matches mouse.* -> lands on PrefixDone (3)
        sim->reset();
        sim->run();
        sim->sendEvent(QStringLiteral("mouse.move"));
        if (sim->configuration() != std::vector<quint64>{3}) {
            std::fprintf(stderr, "FAIL: wildcard twin sim: mouse.move did not match mouse.* to PrefixDone (got %llu)\n",
                         sim->configuration().empty() ? 0ULL : sim->configuration().front());
            return 1;
        }

        // Case D: key.down -> unlisted prefix -> matches * on Active -> lands on UniversalDone (4)
        sim->reset();
        sim->run();
        sim->sendEvent(QStringLiteral("key.down"));
        if (sim->configuration() != std::vector<quint64>{4}) {
            std::fprintf(stderr, "FAIL: wildcard twin sim: key.down did not match universal * to UniversalDone (got %llu)\n",
                         sim->configuration().empty() ? 0ULL : sim->configuration().front());
            return 1;
        }
    }

    // ==== part 12: multiple targets codegen & twin verification =================
    {
        // 1. Orthogonal parallel fork machine definition
        app::Machine multiMachine;
        multiMachine.name = QStringLiteral("multi_target_fork");
        multiMachine.initialStateId = 1;
        multiMachine.states.push_back(app::State{.id = 1, .name = QStringLiteral("Off"), .kind = app::StateKind::Normal});
        multiMachine.states.push_back(app::State{.id = 2, .name = QStringLiteral("Active"), .kind = app::StateKind::Parallel, .initialChildId = 3});
        multiMachine.states.push_back(app::State{.id = 3, .name = QStringLiteral("RegA"), .kind = app::StateKind::Normal, .parentId = 2, .initialChildId = 4});
        multiMachine.states.push_back(app::State{.id = 4, .name = QStringLiteral("A_Ready"), .kind = app::StateKind::Normal, .parentId = 3});
        multiMachine.states.push_back(app::State{.id = 5, .name = QStringLiteral("A_Running"), .kind = app::StateKind::Normal, .parentId = 3});
        multiMachine.states.push_back(app::State{.id = 6, .name = QStringLiteral("RegB"), .kind = app::StateKind::Normal, .parentId = 2, .initialChildId = 7});
        multiMachine.states.push_back(app::State{.id = 7, .name = QStringLiteral("B_Ready"), .kind = app::StateKind::Normal, .parentId = 6});
        multiMachine.states.push_back(app::State{.id = 8, .name = QStringLiteral("B_Running"), .kind = app::StateKind::Normal, .parentId = 6});

        // Fork transition: Off -> [5 (A_Running), 8 (B_Running)]
        multiMachine.transitions.push_back(app::Transition{
            .id = 1,
            .from = 1,
            .to = 5,
            .event = QStringLiteral("START"),
            .targets = {5, 8}
        });
        // Exit transition: Active -> Off
        multiMachine.transitions.push_back(app::Transition{
            .id = 2,
            .from = 2,
            .to = 1,
            .event = QStringLiteral("STOP")
        });

        // Machine validation check
        const QVector<app::Problem> problems = app::validate(multiMachine);
        for (const app::Problem& p : problems) {
            if (p.severity == app::ProblemSeverity::Error) {
                std::fprintf(stderr, "FAIL: multi-target codegen: machine validation error: %s\n",
                             p.text.toUtf8().constData());
                return 1;
            }
        }

        if (!app::isHierarchical(multiMachine)) {
            std::fprintf(stderr, "FAIL: multi-target codegen: machine should be recognized as hierarchical\n");
            return 1;
        }

        // 2. AOT Code generation shape check
        const QVector<app::GeneratedFile> files = app::generate(multiMachine, QStringLiteral("app::generated"));
        QString hierCoreText;
        for (const app::GeneratedFile& file : files) {
            if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                hierCoreText = file.content;
            }
        }
        if (hierCoreText.isEmpty()) {
            std::fprintf(stderr, "FAIL: multi-target codegen: generate() emitted no hier core file\n");
            return 1;
        }

        if (!hierCoreText.contains(QStringLiteral("void multiTargetEntrySequence(const int* targets, int targetCount, int lccaIndex, int* out,"))) {
            std::fprintf(stderr, "FAIL: multi-target codegen: missing multiTargetEntrySequence in emitted core\n");
            return 1;
        }

        if (!hierCoreText.contains(QStringLiteral("multiTargetEntrySequence(targets, 2,"))) {
            std::fprintf(stderr, "FAIL: multi-target codegen: missing multiTargetEntrySequence invocation in fireTransition\n");
            return 1;
        }

        // 3. JIT simulation twin agreement
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: multi-target codegen twin: could not register doc or sim agents\n");
            return 1;
        }

        doc->restore(multiMachine);
        sim->run();
        if (sim->configuration() != std::vector<quint64>{1}) {
            std::fprintf(stderr, "FAIL: multi-target twin: initial state should be Off (1)\n");
            return 1;
        }

        // START event fires fork transition to A_Running and B_Running
        sim->sendEvent(QStringLiteral("START"));
        const std::vector<quint64> expectedFork = {2, 3, 5, 6, 8};
        if (sim->configuration() != expectedFork) {
            std::fprintf(stderr, "FAIL: multi-target twin: START event did not enter fork configuration {2, 3, 5, 6, 8}\n");
            return 1;
        }

        // STOP event returns to Off
        sim->sendEvent(QStringLiteral("STOP"));
        if (sim->configuration() != std::vector<quint64>{1}) {
            std::fprintf(stderr, "FAIL: multi-target twin: STOP event did not exit parallel hierarchy to Off\n");
            return 1;
        }

        // 4. Partial targeting: specify only A_Running, RegB descends to initial child B_Ready
        app::Machine partialMachine = multiMachine;
        partialMachine.transitions[0].targets = {5};
        doc->restore(partialMachine);
        sim->reset();
        sim->run();
        sim->sendEvent(QStringLiteral("START"));
        const std::vector<quint64> expectedPartial = {2, 3, 5, 6, 7};
        if (sim->configuration() != expectedPartial) {
            std::fprintf(stderr, "FAIL: multi-target twin: partial target did not descend unspecified region to B_Ready\n");
            return 1;
        }

        // Emit and verify partial targeting codegen
        const QVector<app::GeneratedFile> partialFiles = app::generate(partialMachine, QStringLiteral("app::generated"));
        QString partialCoreText;
        for (const app::GeneratedFile& f : partialFiles) {
            if (f.relativePath.endsWith(QStringLiteral("_core.h"))) {
                partialCoreText = f.content;
            }
        }
        if (!partialCoreText.contains(QStringLiteral("entrySequence("))) {
            std::fprintf(stderr, "FAIL: multi-target codegen: partial single target missing entrySequence invocation\n");
            return 1;
        }
    }

    // ==== part 13: nested object context codegen + twin interpreter ===========
    // A ContextType::Object variable emits typed nested structs inside struct
    // Context, inlines member guards (context_.user.age >= 18LL) and member
    // assignments, and the interpreter matches step-for-step.
    {
        app::Machine objMachine;
        objMachine.name = QStringLiteral("nested_context_sample");
        objMachine.context = {
            app::ContextVariable{
                .name = QStringLiteral("user"),
                .type = app::ContextType::Object,
                .initialValue = QStringLiteral("{\"age\": 30, \"name\": \"Alice\", \"settings\": {\"notifications\": true}}")
            }
        };

        objMachine.initialStateId = 1;
        objMachine.states.push_back(app::State{
            .id = 1,
            .name = QStringLiteral("Idle"),
            .kind = app::StateKind::Normal
        });
        objMachine.states.push_back(app::State{
            .id = 2,
            .name = QStringLiteral("Active"),
            .kind = app::StateKind::Normal
        });

        objMachine.transitions.push_back(app::Transition{
            .id = 1,
            .from = 1,
            .to = 2,
            .event = QStringLiteral("LOGIN"),
            .guard = QStringLiteral("user.age >= 18 && user.settings.notifications"),
            .action = QStringLiteral("user.age = user.age + 1")
        });

        // 1. Validation check
        const QVector<app::Problem> problems = app::validate(objMachine);
        for (const app::Problem& p : problems) {
            if (p.severity == app::ProblemSeverity::Error) {
                std::fprintf(stderr, "FAIL: nested context codegen: validation error: %s\n",
                             p.text.toUtf8().constData());
                return 1;
            }
        }

        // 2. AOT Code generation shape check
        const QVector<app::GeneratedFile> files = app::generate(objMachine, QStringLiteral("app::generated"));
        QString hooksText;
        QString coreText;
        for (const app::GeneratedFile& file : files) {
            if (file.relativePath.endsWith(QStringLiteral("_hooks.h"))) {
                hooksText = file.content;
            } else if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                coreText = file.content;
            }
        }
        if (hooksText.isEmpty() || coreText.isEmpty()) {
            std::fprintf(stderr, "FAIL: nested context codegen: generate() missing hooks or core file\n");
            return 1;
        }

        // Check _hooks.h for nested struct Context definition
        if (!hooksText.contains(QStringLiteral("struct Context {"))) {
            std::fprintf(stderr, "FAIL: nested context codegen: missing struct Context in _hooks.h\n");
            return 1;
        }
        if (!hooksText.contains(QStringLiteral("struct User_t {"))) {
            std::fprintf(stderr, "FAIL: nested context codegen: missing struct User_t in _hooks.h\n");
            return 1;
        }
        if (!hooksText.contains(QStringLiteral("long long age = 30;"))) {
            std::fprintf(stderr, "FAIL: nested context codegen: missing long long age = 30 in _hooks.h\n");
            return 1;
        }
        if (!hooksText.contains(QStringLiteral("std::string name = \"Alice\";"))) {
            std::fprintf(stderr, "FAIL: nested context codegen: missing std::string name = \"Alice\" in _hooks.h\n");
            return 1;
        }
        if (!hooksText.contains(QStringLiteral("struct Settings_t {")) ||
            !hooksText.contains(QStringLiteral("bool notifications = true;")) ||
            !hooksText.contains(QStringLiteral("} settings;"))) {
            std::fprintf(stderr, "FAIL: nested context codegen: missing nested settings struct in _hooks.h\n");
            return 1;
        }
        if (!hooksText.contains(QStringLiteral("} user;"))) {
            std::fprintf(stderr, "FAIL: nested context codegen: missing } user; in _hooks.h\n");
            return 1;
        }
        if (!hooksText.contains(QStringLiteral("#include <string>"))) {
            std::fprintf(stderr, "FAIL: nested context codegen: missing #include <string> in _hooks.h\n");
            return 1;
        }

        // Check _core.h for inlined guard and action
        if (!coreText.contains(QStringLiteral("(context_.user.age >= 18LL) && context_.user.settings.notifications"))) {
            std::fprintf(stderr, "FAIL: nested context codegen: guard not inlined properly in _core.h\n");
            return 1;
        }
        if (!coreText.contains(QStringLiteral("context_.user.age = static_cast<long long>(context_.user.age + 1LL);"))) {
            std::fprintf(stderr, "FAIL: nested context codegen: action not inlined properly in _core.h\n");
            return 1;
        }

        // 3. JIT simulation twin agreement
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: nested context codegen twin: could not register doc or sim agents\n");
            return 1;
        }

        doc->restore(objMachine);
        sim->run();
        if (sim->configuration() != std::vector<quint64>{1}) {
            std::fprintf(stderr, "FAIL: nested context twin: initial state should be Idle (1)\n");
            return 1;
        }

        // Send LOGIN event -> should evaluate guard to true, mutate age to 31, transition to Active (2)
        sim->sendEvent(QStringLiteral("LOGIN"));
        if (sim->configuration() != std::vector<quint64>{2}) {
            std::fprintf(stderr, "FAIL: nested context twin: LOGIN event did not transition to Active (2)\n");
            return 1;
        }

        const QVariantMap userMap = sim->contextValues().value(QStringLiteral("user")).toMap();
        if (userMap.value(QStringLiteral("age")).toLongLong() != 31) {
            std::fprintf(stderr, "FAIL: nested context twin: user.age should be mutated to 31, got %lld\n",
                         userMap.value(QStringLiteral("age")).toLongLong());
            return 1;
        }
        if (userMap.value(QStringLiteral("name")).toString() != QStringLiteral("Alice")) {
            std::fprintf(stderr, "FAIL: nested context twin: user.name was corrupted\n");
            return 1;
        }
        const QVariantMap settingsMap = userMap.value(QStringLiteral("settings")).toMap();
        if (!settingsMap.value(QStringLiteral("notifications")).toBool()) {
            std::fprintf(stderr, "FAIL: nested context twin: user.settings.notifications was corrupted\n");
            return 1;
        }
    }

    // ==== part 14: STRUCT CONTEXT & C++ TYPE BINDING AOT CODEGEN & TWIN PINNING =====
    // Generates <Machine>_types.h (external headers, internal structs) included by
    // _events.h/_hooks.h/_core.h; typed event payload structs in _events.h;
    // const-reference payload parameters in _core.h and _commands.h; inlined
    // event.<field> and struct assignments. The interpreter twin includes Back replay.
    {
        app::Machine canMachine;
        canMachine.name = QStringLiteral("can_bus_decoder");

        // Types & external headers
        canMachine.externalHeaders.push_back(QStringLiteral("can_driver.h"));

        app::StructDefinition canMsgDef;
        canMsgDef.id = 1;
        canMsgDef.name = QStringLiteral("CanMessage");
        canMsgDef.fields.push_back(app::StructField{
            .name = QStringLiteral("id"),
            .type = app::FieldType::Int,
            .initialValue = QStringLiteral("0"),
        });
        canMsgDef.fields.push_back(app::StructField{
            .name = QStringLiteral("dlc"),
            .type = app::FieldType::Int,
            .initialValue = QStringLiteral("8"),
        });
        canMsgDef.fields.push_back(app::StructField{
            .name = QStringLiteral("data"),
            .type = app::FieldType::String,
            .initialValue = QStringLiteral("\"\""),
        });
        canMachine.types.push_back(canMsgDef);

        app::StructDefinition extDef;
        extDef.id = 2;
        extDef.name = QStringLiteral("ExternalTelemetry");
        extDef.external = true;
        extDef.headerPath = QStringLiteral("telemetry.h");
        canMachine.types.push_back(extDef);

        // Context variables
        app::ContextVariable lastFrameVar;
        lastFrameVar.id = 10;
        lastFrameVar.name = QStringLiteral("lastFrame");
        lastFrameVar.type = app::ContextType::Object;
        lastFrameVar.customTypeName = QStringLiteral("CanMessage");
        canMachine.context.push_back(lastFrameVar);

        app::ContextVariable rxCountVar;
        rxCountVar.id = 11;
        rxCountVar.name = QStringLiteral("rxCount");
        rxCountVar.type = app::ContextType::Int;
        rxCountVar.initialValue = QStringLiteral("0");
        canMachine.context.push_back(rxCountVar);

        // States: Idle (1, initial), Receiving (2)
        app::State idleState;
        idleState.id = 1;
        idleState.name = QStringLiteral("Idle");
        canMachine.states.push_back(idleState);

        app::State recvState;
        recvState.id = 2;
        recvState.name = QStringLiteral("Receiving");
        recvState.entryActions.push_back(QStringLiteral("rxCount = rxCount + 1"));
        canMachine.states.push_back(recvState);

        canMachine.initialStateId = 1;

        // Transitions:
        // 1: Idle -> Receiving on RX_FRAME with payloadType CanMessage, guard event.dlc >= 8 && event.id > 0,
        //    action lastFrame = event; Receiving entry action rxCount = rxCount + 1;
        app::Transition rxTrans;
        rxTrans.id = 100;
        rxTrans.from = 1;
        rxTrans.to = 2;
        rxTrans.event = QStringLiteral("RxFrame");
        rxTrans.payloadType = QStringLiteral("CanMessage");
        rxTrans.guard = QStringLiteral("event.dlc >= 8 && event.id > 0");
        rxTrans.action = QStringLiteral("lastFrame = event");
        canMachine.transitions.push_back(rxTrans);

        // 2: Receiving -> Idle on RESET
        app::Transition resetTrans;
        resetTrans.id = 101;
        resetTrans.from = 2;
        resetTrans.to = 1;
        resetTrans.event = QStringLiteral("RESET");
        canMachine.transitions.push_back(resetTrans);

        // Validate machine
        for (const app::Problem& p : app::validate(canMachine)) {
            if (p.severity == app::ProblemSeverity::Error) {
                std::fprintf(stderr, "FAIL: struct context codegen: validation failed: %s\n", p.text.toUtf8().constData());
                return 1;
            }
        }

        // 1. Generate AOT files
        const QVector<app::GeneratedFile> files = app::generate(canMachine, QStringLiteral("app::generated"));
        QString typesText, eventsText, hooksText, coreText, commandsText;
        for (const app::GeneratedFile& file : files) {
            if (file.relativePath.endsWith(QStringLiteral("_types.h"))) {
                typesText = file.content;
            } else if (file.relativePath.endsWith(QStringLiteral("_events.h"))) {
                eventsText = file.content;
            } else if (file.relativePath.endsWith(QStringLiteral("_hooks.h"))) {
                hooksText = file.content;
            } else if (file.relativePath.endsWith(QStringLiteral("_core.h"))) {
                coreText = file.content;
            } else if (file.relativePath.endsWith(QStringLiteral("_commands.h"))) {
                commandsText = file.content;
            }
        }

        if (typesText.isEmpty() || eventsText.isEmpty() || hooksText.isEmpty() || coreText.isEmpty() || commandsText.isEmpty()) {
            std::fprintf(stderr, "FAIL: struct context codegen: missing expected generated files\n");
            return 1;
        }

        // Check _types.h
        if (!typesText.contains(QStringLiteral("#include \"can_driver.h\"")) ||
            !typesText.contains(QStringLiteral("#include \"telemetry.h\""))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _types.h missing external header includes\n");
            return 1;
        }
        if (!typesText.contains(QStringLiteral("struct CanMessage {")) ||
            !typesText.contains(QStringLiteral("long long id = 0;")) ||
            !typesText.contains(QStringLiteral("long long dlc = 8;")) ||
            !typesText.contains(QStringLiteral("std::string data = \"\";")) ||
            !typesText.contains(QStringLiteral("bool operator==(const CanMessage&) const = default;"))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _types.h invalid CanMessage definition\n");
            return 1;
        }
        if (typesText.contains(QStringLiteral("struct ExternalTelemetry {"))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _types.h emitted duplicate struct for external type\n");
            return 1;
        }

        // Check _events.h
        if (!eventsText.contains(QStringLiteral("#include \"can_bus_decoder_types.h\""))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _events.h missing _types.h include\n");
            return 1;
        }
        if (!eventsText.contains(QStringLiteral("struct RxFrameRequested {")) ||
            !eventsText.contains(QStringLiteral("CanMessage payload{};"))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _events.h missing RxFrameRequested with payload\n");
            return 1;
        }

        // Check _hooks.h
        if (!hooksText.contains(QStringLiteral("#include \"can_bus_decoder_types.h\""))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _hooks.h missing _types.h include\n");
            return 1;
        }
        if (!hooksText.contains(QStringLiteral("CanMessage lastFrame{};")) ||
            !hooksText.contains(QStringLiteral("long long rxCount = 0;"))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _hooks.h missing Context members with CanMessage\n");
            return 1;
        }

        // Check _core.h
        if (!coreText.contains(QStringLiteral("#include \"can_bus_decoder_types.h\""))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _core.h missing _types.h include\n");
            return 1;
        }
        if (!coreText.contains(QStringLiteral("void rxFrameRequested(const CanMessage& event) {"))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _core.h missing rxFrameRequested(const CanMessage&)\n");
            return 1;
        }
        if (!coreText.contains(QStringLiteral("(event.dlc >= 8LL) && (event.id > 0LL)"))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _core.h guard not inlined with event.<field>\n");
            return 1;
        }
        if (!coreText.contains(QStringLiteral("context_.lastFrame = event;"))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _core.h action lastFrame = event not inlined\n");
            return 1;
        }
        if (!coreText.contains(QStringLiteral("context_.rxCount = static_cast<long long>(context_.rxCount + 1LL);"))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _core.h action rxCount not inlined\n");
            return 1;
        }

        // Check _commands.h
        if (!commandsText.contains(QStringLiteral("agent->core().rxFrameRequested(event.payload);"))) {
            std::fprintf(stderr, "FAIL: struct context codegen: _commands.h missing event.payload forwarding\n");
            return 1;
        }

        // 2. JIT Simulation twin agreement
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto sim = kernel.agentAs<app::SimulationAgent>(app::SimulationAgent::kName);
        if (!doc || !sim) {
            std::fprintf(stderr, "FAIL: struct context twin: could not register doc or sim agents\n");
            return 1;
        }

        doc->restore(canMachine);
        sim->run();
        if (sim->configuration() != std::vector<quint64>{1}) {
            std::fprintf(stderr, "FAIL: struct context twin: initial state should be Idle (1)\n");
            return 1;
        }

        // Send RxFrame with failing guard (dlc = 4 < 8)
        QVariantMap invalidPayload;
        invalidPayload[QStringLiteral("id")] = 0x100;
        invalidPayload[QStringLiteral("dlc")] = 4;
        invalidPayload[QStringLiteral("data")] = QStringLiteral("test");
        sim->sendEvent(QStringLiteral("RxFrame"), invalidPayload);
        if (sim->configuration() != std::vector<quint64>{1}) {
            std::fprintf(stderr, "FAIL: struct context twin: transition should not fire when guard fails\n");
            return 1;
        }

        // Send RxFrame with passing guard (dlc = 8, id = 0x123)
        QVariantMap validPayload;
        validPayload[QStringLiteral("id")] = 0x123;
        validPayload[QStringLiteral("dlc")] = 8;
        validPayload[QStringLiteral("data")] = QStringLiteral("payload-data");
        sim->sendEvent(QStringLiteral("RxFrame"), validPayload);
        if (sim->configuration() != std::vector<quint64>{2}) {
            std::fprintf(stderr, "FAIL: struct context twin: transition should fire to Receiving (2)\n");
            return 1;
        }

        // Verify context updated
        if (sim->contextValues().value(QStringLiteral("rxCount")).toLongLong() != 1) {
            std::fprintf(stderr, "FAIL: struct context twin: rxCount should be 1\n");
            return 1;
        }
        const QVariantMap recordedFrame = sim->contextValues().value(QStringLiteral("lastFrame")).toMap();
        if (recordedFrame.value(QStringLiteral("id")).toLongLong() != 0x123 ||
            recordedFrame.value(QStringLiteral("dlc")).toLongLong() != 8 ||
            recordedFrame.value(QStringLiteral("data")).toString() != QStringLiteral("payload-data")) {
            std::fprintf(stderr, "FAIL: struct context twin: lastFrame not updated accurately\n");
            return 1;
        }

        // Send RESET
        sim->sendEvent(QStringLiteral("RESET"));
        if (sim->configuration() != std::vector<quint64>{1}) {
            std::fprintf(stderr, "FAIL: struct context twin: RESET should return to Idle (1)\n");
            return 1;
        }

        // Test Back undo replay
        sim->back(); // undo RESET -> back to Receiving (2)
        if (sim->configuration() != std::vector<quint64>{2}) {
            std::fprintf(stderr, "FAIL: struct context twin: back() should restore Receiving (2)\n");
            return 1;
        }
        sim->back(); // undo RX_FRAME -> back to Idle (1), rxCount = 0
        if (sim->configuration() != std::vector<quint64>{1}) {
            std::fprintf(stderr, "FAIL: struct context twin: back() should restore Idle (1)\n");
            return 1;
        }
        if (sim->contextValues().value(QStringLiteral("rxCount")).toLongLong() != 0) {
            std::fprintf(stderr, "FAIL: struct context twin: back() should restore rxCount = 0\n");
            return 1;
        }
    }

    std::printf(
        "PASS: state-designer hierarchical codegen smoke (static tables + active_ + framework-free + inlined "
        "guard/action + flat shape untouched + canvas-interaction drift guard + gen-hier written + twin "
        "interpreter pinned + context-free byte shape + Context struct/inline guard/assign + gen-context "
        "written + context twin pinned + invoke Invocations/armInvocations/cancelInvocations shape + gen-invoke "
        "written + invoke twin pinned + hierarchical invoke armStateInvocation/cancelStateInvocation shape + "
        "gen-hier-invoke written + hierarchical invoke twin pinned + reenter & always codegen twin + raise microstep twin + "
        "actor communication twin + wildcard event codegen twin + multi-target fork codegen twin + nested object context twin + "
        "struct context AOT twin)\n");
    return 0;
}

