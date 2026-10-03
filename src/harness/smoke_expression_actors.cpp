// --smoke phase 2d (actors and payloads): Sections 13-16
// Covers:
// - Section 13: invoke actor payload
// - Section 14: raise action form and parser
// - Section 15: sendTo / sendParent
// - Section 16: parseRaiseForm and LogicRowClass::Raise

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include <cstdio>
#include <memory>

#include <ordo/core/kernel.h>

#include "harness/harness.h"
#include "infra/expression.h"
#include "infra/logic_inventory.h"
#include "infra/machine_validator.h"
#include "infra/project_io.h"
#include "infra/xstate_v5_io.h"
#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/sim_agent.h"
#include "model/undo_events.h"
#include "model/undo_store.h"

namespace {

bool expectEmitCpp(const QString& source, const QString& contextExpr, const QString& wanted) {
    const app::expr::ParseResult parsed = app::expr::parse(source);
    if (!parsed.ok) {
        std::fprintf(stderr, "FAIL: emitCpp fixture \"%s\" did not parse: %s (at %d)\n", qUtf8Printable(source),
                     qUtf8Printable(parsed.message), parsed.position);
        return false;
    }
    const QString actual = app::expr::emitCpp(parsed.ast, contextExpr);
    if (actual != wanted) {
        std::fprintf(stderr, "FAIL: emitCpp(parse(\"%s\"), contextExpr=\"%s\") = \"%s\" (want \"%s\")\n",
                     qUtf8Printable(source), qUtf8Printable(contextExpr), qUtf8Printable(actual),
                     qUtf8Printable(wanted));
        return false;
    }
    return true;
}

bool expectEmitCppBare(const QString& source, const QString& contextExpr, const QSet<QString>& bareIdentifiers,
                       const QString& wanted) {
    const app::expr::ParseResult parsed = app::expr::parse(source);
    if (!parsed.ok) {
        std::fprintf(stderr, "FAIL: emitCpp bare-set fixture \"%s\" did not parse: %s (at %d)\n",
                     qUtf8Printable(source), qUtf8Printable(parsed.message), parsed.position);
        return false;
    }
    const QString actual = app::expr::emitCpp(parsed.ast, contextExpr, bareIdentifiers);
    if (actual != wanted) {
        std::fprintf(stderr, "FAIL: emitCpp(parse(\"%s\"), contextExpr=\"%s\", bareIdentifiers=...) = \"%s\" (want \"%s\")\n",
                     qUtf8Printable(source), qUtf8Printable(contextExpr), qUtf8Printable(actual), qUtf8Printable(wanted));
        return false;
    }
    return true;
}

}  // namespace

int runExpressionActorsSmoke() {
    using app::expr::NodeKind;
    using app::expr::ParseResult;

    int failures = 0;
    // ---- 13. onDone/onError payloads meet context ------------------------------
    // `output`/`error` are ordinary Identifier nodes resolved against a
    // transition-scoped PayloadBinding. Proves the consumers agree: 13a
    // typeCheck() scope rule; 13b emitCpp() bare identifiers; 13c/13d validator
    // checks 9b/10/11; 13e xstate_v5_io `event.output`/`event.error` mapping.

    // 13a. typeCheck()/validateGuardSource() resolve `output`/`error` against
    // the injected PayloadBinding when in scope, else report the ordinary
    // unknown-identifier finding (also for cross-scope use, e.g. `error` under
    // PayloadBinding::forOnDone()).
    {
        const QVector<app::ContextVariable> noSchema;
        auto expectPayloadTypeProblems = [&](const QString& source, const app::expr::PayloadBinding& payload,
                                             int wantedProblems) {
            const app::expr::ParseResult parsed = app::expr::parse(source);
            if (!parsed.ok) {
                std::fprintf(stderr, "FAIL: payload typeCheck fixture \"%s\" did not parse: %s\n",
                             qUtf8Printable(source), qUtf8Printable(parsed.message));
                return false;
            }
            const QVector<app::expr::TypeProblem> problems = app::expr::typeCheck(parsed.ast, noSchema, payload);
            if (problems.size() != wantedProblems) {
                std::fprintf(stderr, "FAIL: typeCheck(\"%s\", payload=\"%s\") reported %d problem(s) (want %d)\n",
                             qUtf8Printable(source), qUtf8Printable(payload.name), static_cast<int>(problems.size()),
                             wantedProblems);
                return false;
            }
            return true;
        };

        // In scope: forOnDone(Int) resolves Int (comparable against an Int
        // literal), error() resolves String (comparable against a String
        // literal).
        failures += !expectPayloadTypeProblems(QStringLiteral("output > 0"),
                                                app::expr::PayloadBinding::forOnDone(app::ContextType::Int), 0);
        failures += !expectPayloadTypeProblems(QStringLiteral("error == 'TIMEOUT'"), app::expr::PayloadBinding::error(), 0);
        // Cross-scope: `error` while `output` is in scope, and vice versa,
        // is an unknown identifier, the same finding a plain typo gets.
        failures += !expectPayloadTypeProblems(QStringLiteral("error == 'x'"),
                                                app::expr::PayloadBinding::forOnDone(app::ContextType::Int), 1);
        failures += !expectPayloadTypeProblems(QStringLiteral("output > 0"), app::expr::PayloadBinding::error(), 1);
        // Neither is in scope on an ordinary transition (PayloadBinding::none()),
        // so both are unknown identifiers.
        failures += !expectPayloadTypeProblems(QStringLiteral("output > 0"), app::expr::PayloadBinding::none(), 1);
        failures += !expectPayloadTypeProblems(QStringLiteral("error == 'x'"), app::expr::PayloadBinding::none(), 1);

        // forOnDone(type) takes the declared type: the same guard source
        // `output == 'ok'` type-checks clean for String and reports a problem
        // for Int, proving the type comes from the argument.
        failures += !expectPayloadTypeProblems(QStringLiteral("output == 'ok'"),
                                                app::expr::PayloadBinding::forOnDone(app::ContextType::String), 0);
        failures += !expectPayloadTypeProblems(QStringLiteral("output == 'ok'"),
                                                app::expr::PayloadBinding::forOnDone(app::ContextType::Int), 1);

        // validateGuardSource() threads `payload` straight to typeCheck(), so it
        // must agree on the same rows.
        if (!app::expr::validateGuardSource(QStringLiteral("output > 0"), noSchema, nullptr,
                                            app::expr::PayloadBinding::forOnDone(app::ContextType::Int))
                 .isEmpty()) {
            std::fprintf(stderr,
                         "FAIL: validateGuardSource(\"output > 0\", payload=forOnDone(Int)) reported a problem\n");
            ++failures;
        }
        if (app::expr::validateGuardSource(QStringLiteral("output > 0"), noSchema, nullptr, app::expr::PayloadBinding::none())
                .isEmpty()) {
            std::fprintf(stderr,
                         "FAIL: validateGuardSource(\"output > 0\", payload=none()) did not report unknown identifier\n");
            ++failures;
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 13a (payload typeCheck/validateGuardSource) had %d failure(s)\n",
                     failures);
        return 1;
    }

    // 13b. emitCpp() emits `output`/`error` BARE when named in
    // `bareIdentifiers`, while a sibling context variable in the same
    // expression still gets the `<contextExpr>.` prefix. The default (empty)
    // bare set leaves emitCpp()'s output unchanged.
    failures += !expectEmitCppBare(QStringLiteral("output > 0"), QStringLiteral("context"),
                                   QSet<QString>{QStringLiteral("output")}, QStringLiteral("output > 0LL"));
    failures += !expectEmitCppBare(QStringLiteral("output > 0 && count > 3"), QStringLiteral("context"),
                                   QSet<QString>{QStringLiteral("output")},
                                   QStringLiteral("(output > 0LL) && (context.count > 3LL)"));
    failures += !expectEmitCppBare(QStringLiteral("error == 'TIMEOUT'"), QStringLiteral("context"),
                                   QSet<QString>{QStringLiteral("error")}, QStringLiteral("error == \"TIMEOUT\""));
    failures += !expectEmitCpp(QStringLiteral("output > 0"), QStringLiteral("context"),
                               QStringLiteral("context.output > 0LL"));
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 13b (emitCpp bare identifiers) had %d failure(s)\n",
                     failures);
        return 1;
    }

    // 13c. machine_validator.cpp check 9b rejects a context variable literally
    // named `output`/`error`: the payload binding resolves first, so such a
    // variable would be unreachable inside every onDone transition's guard/assign.
    {
        auto expectPayloadWordRejected = [&](const QString& name) {
            app::Machine machine;
            machine.name = QStringLiteral("PayloadReservedWord");
            machine.context = {app::ContextVariable{
                .id = 1, .name = name, .type = app::ContextType::Int, .initialValue = QStringLiteral("0")}};
            machine.states = {app::State{.id = 2, .name = QStringLiteral("A")}};
            machine.initialStateId = 2;
            machine.nextId = 100;
            for (const app::Problem& problem : app::validate(machine)) {
                if (problem.severity == app::ProblemSeverity::Error &&
                    problem.text.contains(QStringLiteral("reserved for the onDone/onError invoke completion payload"))) {
                    return true;
                }
            }
            std::fprintf(stderr, "FAIL: check 9b did not reject a context variable named '%s'\n", qUtf8Printable(name));
            return false;
        };
        failures += !expectPayloadWordRejected(QStringLiteral("output"));
        failures += !expectPayloadWordRejected(QStringLiteral("error"));
    }
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 13c (check 9b payload reserved words) had %d failure(s)\n",
                     failures);
        return 1;
    }

    // 13d. machine_validator.cpp checks 10/11 over a REAL onDone/onError
    // transition off a state that declares an invoke, so the structural
    // recognition (State::invokeSrc + the "done.invoke.<id>"/
    // "error.platform.<id>" event) is exercised end to end. Findings are
    // isolated by the "is ill-typed:" marker both checks share, since check 6
    // inspects the same guard/action strings regardless of whether they resolve.
    {
        auto findingCount = [](const app::Machine& machine, quint64 transitionId) {
            int count = 0;
            for (const app::Problem& problem : app::validate(machine)) {
                if (problem.transitionId != transitionId || problem.severity != app::ProblemSeverity::Error) {
                    continue;
                }
                if (problem.text.contains(QStringLiteral("is ill-typed:"))) {
                    ++count;
                }
            }
            return count;
        };
        auto expectFindingCount = [&](const app::Machine& machine, quint64 transitionId, int wanted,
                                      const char* label) {
            const int actual = findingCount(machine, transitionId);
            if (actual != wanted) {
                std::fprintf(stderr, "FAIL: %s: checks 10/11 reported %d finding(s) for transition %llu (want %d)\n",
                             label, actual, static_cast<unsigned long long>(transitionId), wanted);
                return false;
            }
            return true;
        };

        app::Machine machine;
        machine.name = QStringLiteral("InvokePayloadScope");
        machine.states = {
            app::State{.id = 1, .name = QStringLiteral("Fetching"), .invokeSrc = QStringLiteral("fetchUser")},
            app::State{.id = 2, .name = QStringLiteral("Done")},
        };
        machine.initialStateId = 1;
        machine.context = {app::ContextVariable{
            .id = 50, .name = QStringLiteral("result"), .type = app::ContextType::Int, .initialValue = QStringLiteral("0")}};
        machine.nextId = 100;
        machine.transitions = {
            // onDone (event == "done.invoke.fetchUser", off state 1's own
            // invokeSrc): `output` resolves; `error` does not. Guards 12/13
            // carry a harmless hook name ("proceed") rather than a blank one,
            // so check 5 sees four guarded candidates, not two unguarded ones
            // colliding.
            app::Transition{
                .id = 10, .from = 1, .to = 2, .event = QStringLiteral("done.invoke.fetchUser"), .guard = QStringLiteral("output > 0")},
            app::Transition{
                .id = 11, .from = 1, .to = 2, .event = QStringLiteral("done.invoke.fetchUser"), .guard = QStringLiteral("error == 'x'")},
            app::Transition{.id = 12,
                            .from = 1,
                            .to = 2,
                            .event = QStringLiteral("done.invoke.fetchUser"),
                            .guard = QStringLiteral("proceed"),
                            .action = QStringLiteral("result = output")},
            app::Transition{.id = 13,
                            .from = 1,
                            .to = 2,
                            .event = QStringLiteral("done.invoke.fetchUser"),
                            .guard = QStringLiteral("proceed"),
                            .action = QStringLiteral("result = error")},
            // onError (event == "error.platform.fetchUser"): the mirror image.
            app::Transition{.id = 14,
                            .from = 1,
                            .to = 2,
                            .event = QStringLiteral("error.platform.fetchUser"),
                            .guard = QStringLiteral("error == 'TIMEOUT'")},
            app::Transition{
                .id = 15, .from = 1, .to = 2, .event = QStringLiteral("error.platform.fetchUser"), .guard = QStringLiteral("output > 0")},
            // An ORDINARY transition off the SAME invoking state: neither
            // reserved word means anything here.
            app::Transition{.id = 16, .from = 1, .to = 2, .event = QStringLiteral("CANCEL"), .guard = QStringLiteral("output > 0")},
        };

        failures += !expectFindingCount(machine, 10, 0, "onDone guard using output");
        failures += !expectFindingCount(machine, 11, 1, "onDone guard using error (cross-scope)");
        failures += !expectFindingCount(machine, 12, 0, "onDone assign reading output");
        failures += !expectFindingCount(machine, 13, 1, "onDone assign reading error (cross-scope)");
        failures += !expectFindingCount(machine, 14, 0, "onError guard using error");
        failures += !expectFindingCount(machine, 15, 1, "onError guard using output (cross-scope)");
        failures += !expectFindingCount(machine, 16, 1, "ordinary transition using output");
    }
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 13d (checks 10/11 over a real invoke transition) had %d failure(s)\n",
                     failures);
        return 1;
    }

    // 13d-ii. payloadBindingForTransition() must build the onDone binding from
    // the invoking state's own invokeOutputType: two states invoking the same
    // service name with different output types, each with the identical guard
    // "output == 'ok'" on its onDone row, must report different finding counts.
    {
        auto findingCount = [](const app::Machine& machine, quint64 transitionId) {
            int count = 0;
            for (const app::Problem& problem : app::validate(machine)) {
                if (problem.transitionId == transitionId && problem.severity == app::ProblemSeverity::Error &&
                    problem.text.contains(QStringLiteral("is ill-typed:"))) {
                    ++count;
                }
            }
            return count;
        };

        app::Machine machine;
        machine.name = QStringLiteral("InvokeOutputTypePerState");
        machine.states = {
            app::State{.id = 1,
                       .name = QStringLiteral("FetchingString"),
                       .invokeSrc = QStringLiteral("fetchThing"),
                       .invokeOutputType = app::ContextType::String},
            app::State{.id = 2,
                       .name = QStringLiteral("FetchingInt"),
                       .invokeSrc = QStringLiteral("fetchOther"),
                       .invokeOutputType = app::ContextType::Int},
            app::State{.id = 3, .name = QStringLiteral("Done")},
        };
        machine.initialStateId = 1;
        machine.nextId = 100;
        machine.transitions = {
            app::Transition{.id = 10,
                            .from = 1,
                            .to = 3,
                            .event = QStringLiteral("done.invoke.fetchThing"),
                            .guard = QStringLiteral("output == 'ok'")},
            app::Transition{.id = 11,
                            .from = 2,
                            .to = 3,
                            .event = QStringLiteral("done.invoke.fetchOther"),
                            .guard = QStringLiteral("output == 'ok'")},
        };

        const int stringFindings = findingCount(machine, 10);
        const int intFindings = findingCount(machine, 11);
        if (stringFindings != 0) {
            std::fprintf(stderr,
                          "FAIL: check 10 reported %d finding(s) for \"output == 'ok'\" off a String-declared "
                          "invoke (want 0)\n",
                          stringFindings);
            ++failures;
        }
        if (intFindings != 1) {
            std::fprintf(stderr,
                          "FAIL: check 10 reported %d finding(s) for \"output == 'ok'\" off an Int-declared "
                          "invoke (want 1)\n",
                          intFindings);
            ++failures;
        }
    }
    if (failures != 0) {
        std::fprintf(stderr,
                      "FAIL: expression phase section 13d-ii (per-state invokeOutputType, checks 10/11) had %d "
                      "failure(s)\n",
                      failures);
        return 1;
    }

    // 13e. xstate_v5_io: `output`/`error` <-> `event.output`/`event.error`, both
    // directions, over a Machine whose State declares an invoke so
    // payloadNameForTransition() fires for real.
    {
        app::Machine machine;
        machine.name = QStringLiteral("InvokeRoundTrip");
        machine.states = {
            app::State{.id = 1, .name = QStringLiteral("Fetching"), .invokeSrc = QStringLiteral("fetchUser")},
            app::State{.id = 2, .name = QStringLiteral("Done")},
            app::State{.id = 3, .name = QStringLiteral("Failed")},
        };
        machine.initialStateId = 1;
        machine.nextId = 100;
        machine.transitions = {
            app::Transition{.id = 10,
                            .from = 1,
                            .to = 2,
                            .event = QStringLiteral("done.invoke.fetchUser"),
                            .guard = QStringLiteral("output == 'ok'")},
            app::Transition{.id = 11,
                            .from = 1,
                            .to = 3,
                            .event = QStringLiteral("error.platform.fetchUser"),
                            .guard = QStringLiteral("error == 'TIMEOUT'")},
        };

        const app::XStateExportResult exported = app::machineToXStateJson(machine);
        if (!exported.ok) {
            std::fprintf(stderr, "FAIL: invoke round-trip export refused: %s\n", qUtf8Printable(exported.error));
            return 1;
        }

        // The structural JSON reads `{"type": "event", "name": "output"}` on the
        // onDone guard's left operand: export picks the "event" reference shape,
        // never the "context" one. onDone/onError live under the state's own
        // `invoke.onDone`/`invoke.onError`, never a flat `on` entry.
        const QJsonObject fetchingState =
            exported.json[QStringLiteral("states")].toObject().value(QStringLiteral("Fetching")).toObject();
        const QJsonObject fetchingInvoke = fetchingState.value(QStringLiteral("invoke")).toObject();
        if (fetchingState.contains(QStringLiteral("on")) ||
            fetchingInvoke.value(QStringLiteral("src")).toString() != QStringLiteral("fetchUser") ||
            fetchingInvoke.contains(QStringLiteral("id"))) {
            std::fprintf(stderr,
                         "FAIL: Fetching should export invoke.src=fetchUser (no id, none set) with NO leftover "
                         "`on` map\n");
            return 1;
        }
        const QJsonObject doneGuardLeft = fetchingInvoke.value(QStringLiteral("onDone"))
                                              .toObject()
                                              .value(QStringLiteral("guard"))
                                              .toObject()
                                              .value(QStringLiteral("left"))
                                              .toObject();
        if (doneGuardLeft.value(QStringLiteral("type")).toString() != QStringLiteral("event") ||
            doneGuardLeft.value(QStringLiteral("name")).toString() != QStringLiteral("output")) {
            std::fprintf(stderr,
                         "FAIL: onDone guard's `output` did not export as {\"type\":\"event\",\"name\":\"output\"}\n");
            return 1;
        }
        const QJsonObject errorGuardLeft = fetchingInvoke.value(QStringLiteral("onError"))
                                               .toObject()
                                               .value(QStringLiteral("guard"))
                                               .toObject()
                                               .value(QStringLiteral("left"))
                                               .toObject();
        if (errorGuardLeft.value(QStringLiteral("type")).toString() != QStringLiteral("event") ||
            errorGuardLeft.value(QStringLiteral("name")).toString() != QStringLiteral("error")) {
            std::fprintf(stderr,
                         "FAIL: onError guard's `error` did not export as {\"type\":\"event\",\"name\":\"error\"}\n");
            return 1;
        }

        // Import: the guard text survives byte-for-byte, and Fetching's own
        // invokeSrc comes back too.
        const app::XStateImportResult imported = app::machineFromXStateJson(exported.json);
        const app::Transition* importedDone = nullptr;
        const app::Transition* importedError = nullptr;
        for (const app::Transition& transition : imported.machine.transitions) {
            if (transition.event == QStringLiteral("done.invoke.fetchUser")) {
                importedDone = &transition;
            } else if (transition.event == QStringLiteral("error.platform.fetchUser")) {
                importedError = &transition;
            }
        }
        const app::State* importedFetching = nullptr;
        for (const app::State& state : imported.machine.states) {
            if (state.name == QStringLiteral("Fetching")) {
                importedFetching = &state;
            }
        }
        if (importedDone == nullptr || importedError == nullptr || importedFetching == nullptr ||
            importedFetching->invokeSrc != QStringLiteral("fetchUser") ||
            importedDone->guard != QStringLiteral("output == 'ok'") ||
            importedError->guard != QStringLiteral("error == 'TIMEOUT'")) {
            std::fprintf(stderr,
                         "FAIL: invoke payload guard text and/or invokeSrc did not round-trip (done=\"%s\" "
                         "error=\"%s\")\n",
                         importedDone ? qUtf8Printable(importedDone->guard) : "<missing>",
                         importedError ? qUtf8Printable(importedError->guard) : "<missing>");
            return 1;
        }

        // Idempotency: re-export the reimported machine as-is;
        // toJson(fromJson(toJson(M))) == toJson(M).
        const app::XStateExportResult reExported = app::machineToXStateJson(imported.machine);
        if (!reExported.ok || reExported.json != exported.json) {
            std::fprintf(stderr, "FAIL: invoke round-trip re-export is not idempotent with the first export\n");
            return 1;
        }

        // Unknown invoke-payload identifier: `{"type":"event","name":"bogus"}`
        // is neither `output` nor `error`: diagnosed and dropped, never
        // silently imported.
        QJsonObject bogusGuard;
        bogusGuard[QStringLiteral("type")] = QStringLiteral("event");
        bogusGuard[QStringLiteral("name")] = QStringLiteral("bogus");
        QJsonObject bogusTransition;
        bogusTransition[QStringLiteral("target")] = QStringLiteral("Done");
        bogusTransition[QStringLiteral("guard")] = bogusGuard;
        QJsonObject bogusOn;
        bogusOn[QStringLiteral("BOGUS_EVENT")] = bogusTransition;
        QJsonObject bogusFetching;
        bogusFetching[QStringLiteral("on")] = bogusOn;
        QJsonObject bogusStates;
        bogusStates[QStringLiteral("Fetching")] = bogusFetching;
        bogusStates[QStringLiteral("Done")] = QJsonObject();
        QJsonObject bogusMachineJson;
        bogusMachineJson[QStringLiteral("id")] = QStringLiteral("Bogus");
        bogusMachineJson[QStringLiteral("initial")] = QStringLiteral("Fetching");
        bogusMachineJson[QStringLiteral("states")] = bogusStates;

        const app::XStateImportResult bogusImported = app::machineFromXStateJson(bogusMachineJson);
        bool sawBogusDiagnostic = false;
        for (const QString& diagnostic : bogusImported.diagnostics) {
            if (diagnostic.contains(QStringLiteral("is not a known invoke-payload identifier"))) {
                sawBogusDiagnostic = true;
            }
        }
        if (!sawBogusDiagnostic) {
            std::fprintf(stderr, "FAIL: importing {\"type\":\"event\",\"name\":\"bogus\"} did not diagnose an "
                                  "unknown invoke-payload identifier\n");
            return 1;
        }
    }

    // ---- 14. motion-controller.sdm sample machine loads, validates clean, and evaluates ----
    {
        app::Machine motionMachine;
        QString loadError;
        if (!app::loadMachine(QStringLiteral("src/machines/motion-controller.sdm"), &motionMachine, &loadError)) {
            std::fprintf(stderr, "FAIL: could not load src/machines/motion-controller.sdm: %s\n",
                         qUtf8Printable(loadError));
            return 1;
        }
        const QVector<app::Problem> problems = app::validate(motionMachine);
        if (!problems.isEmpty()) {
            std::fprintf(stderr, "FAIL: motion-controller.sdm has %d validation problem(s):\n", problems.size());
            for (const app::Problem& problem : problems) {
                std::fprintf(stderr, "  - [%s] %s (stateId=%llu, transitionId=%llu)\n",
                             problem.severity == app::ProblemSeverity::Error ? "Error" : "Warning",
                             qUtf8Printable(problem.text),
                             static_cast<unsigned long long>(problem.stateId),
                             static_cast<unsigned long long>(problem.transitionId));
            }
            return 1;
        }

        // Verify that complex expressions evaluate accurately on initial context
        QVariantMap initialContext;
        for (const app::ContextVariable& var : motionMachine.context) {
            if (var.type == app::ContextType::Double) {
                initialContext[var.name] = var.initialValue.toDouble();
            } else if (var.type == app::ContextType::Int) {
                initialContext[var.name] = var.initialValue.toLongLong();
            } else if (var.type == app::ContextType::Bool) {
                initialContext[var.name] = (var.initialValue == QStringLiteral("true"));
            } else {
                initialContext[var.name] = var.initialValue;
            }
        }
        // Test distance formula: sqrt(pow(targetX - x, 2.0) + pow(targetY - y, 2.0))
        const auto distParsed = app::expr::parse(QStringLiteral("sqrt(pow(targetX - x, 2.0) + pow(targetY - y, 2.0))"));
        bool evalOk = false;
        const app::expr::Value distVal = app::expr::evaluate(distParsed.ast, initialContext, &evalOk);
        if (!evalOk || std::abs(distVal.asDouble() - 111.80339887) > 0.001) {
            std::fprintf(stderr, "FAIL: motion-controller distance eval failed: ok=%d val=%f\n", evalOk, distVal.asDouble());
            return 1;
        }

        // Test math function with aliases: Math.sqrt(Math.pow(targetX - x, 2.0) + Math.pow(targetY - y, 2.0))
        const auto mathAliasParsed = app::expr::parse(QStringLiteral("Math.sqrt(Math.pow(targetX - x, 2.0) + Math.pow(targetY - y, 2.0))"));
        evalOk = false;
        const app::expr::Value aliasVal = app::expr::evaluate(mathAliasParsed.ast, initialContext, &evalOk);
        if (!evalOk || std::abs(aliasVal.asDouble() - 111.80339887) > 0.001) {
            std::fprintf(stderr, "FAIL: motion-controller math alias eval failed: ok=%d val=%f\n", evalOk, aliasVal.asDouble());
            return 1;
        }

        // Test modulo and constants
        const auto modParsed = app::expr::parse(QStringLiteral("stepCount % heartbeatInterval == 0 && headingAngle < 2.0 * Math.PI"));
        evalOk = false;
        const app::expr::Value modVal = app::expr::evaluate(modParsed.ast, initialContext, &evalOk);
        if (!evalOk || !modVal.boolValue) {
            std::fprintf(stderr, "FAIL: motion-controller modulo/constant eval failed: ok=%d val=%d\n", evalOk, modVal.boolValue);
            return 1;
        }
    }

    // Section 16: parseRaiseForm() and LogicRowClass::Raise:
    {
        const auto r1 = app::expr::parseRaiseForm(QStringLiteral("raise(Ready)"));
        if (!r1.ok || r1.event != QStringLiteral("Ready")) {
            std::fprintf(stderr, "FAIL: parseRaiseForm('raise(Ready)') failed: ok=%d event='%s'\n",
                         r1.ok, r1.event.toUtf8().constData());
            return 1;
        }

        const auto r2 = app::expr::parseRaiseForm(QStringLiteral("  raise (  done.invoke.fetch  )  "));
        if (!r2.ok || r2.event != QStringLiteral("done.invoke.fetch")) {
            std::fprintf(stderr, "FAIL: parseRaiseForm('raise(done.invoke.fetch)') failed: ok=%d event='%s'\n",
                         r2.ok, r2.event.toUtf8().constData());
            return 1;
        }

        const auto r3 = app::expr::parseRaiseForm(QStringLiteral("RAISE(Timeout)"));
        if (!r3.ok || r3.event != QStringLiteral("Timeout")) {
            std::fprintf(stderr, "FAIL: parseRaiseForm('RAISE(Timeout)') failed: ok=%d\n", r3.ok);
            return 1;
        }

        const auto nr1 = app::expr::parseRaiseForm(QStringLiteral("raise_flag"));
        if (nr1.ok || !nr1.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseRaiseForm('raise_flag') should be silent fallback, got ok=%d msg='%s'\n",
                         nr1.ok, nr1.message.toUtf8().constData());
            return 1;
        }

        const auto nr2 = app::expr::parseRaiseForm(QStringLiteral("count = count + 1"));
        if (nr2.ok || !nr2.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseRaiseForm('count = count + 1') should be silent fallback\n");
            return 1;
        }

        const auto bad1 = app::expr::parseRaiseForm(QStringLiteral("raise()"));
        if (bad1.ok || bad1.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseRaiseForm('raise()') should fail with message\n");
            return 1;
        }

        const auto bad2 = app::expr::parseRaiseForm(QStringLiteral("raise(Ready"));
        if (bad2.ok || bad2.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseRaiseForm('raise(Ready') should fail with missing ')' message\n");
            return 1;
        }

        const auto bad3 = app::expr::parseRaiseForm(QStringLiteral("raise(invalid event)"));
        if (bad3.ok || bad3.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseRaiseForm('raise(invalid event)') should fail with invalid char message\n");
            return 1;
        }

        app::Machine testMachine;
        app::Transition t;
        t.id = 1;
        t.from = 1;
        t.to = 2;
        t.event = QStringLiteral("Go");
        t.action = QStringLiteral("raise(NextStep)");
        testMachine.transitions.push_back(t);
        const auto inv = app::inventory(testMachine);
        if (inv.actions.size() != 1 || inv.actions[0].rowClass != app::LogicRowClass::Raise) {
            std::fprintf(stderr, "FAIL: inventory did not classify raise action as LogicRowClass::Raise\n");
            return 1;
        }
    }

    // Section 15: parseSendToForm(), parseSendParentForm(), and LogicRowClass::SendTo/SendParent
    {
        // 1. parseSendToForm positive cases
        const auto s1 = app::expr::parseSendToForm(QStringLiteral("sendTo(timerService, Tick)"));
        if (!s1.ok || s1.target != QStringLiteral("timerService") || s1.event != QStringLiteral("Tick")) {
            std::fprintf(stderr, "FAIL: parseSendToForm('sendTo(timerService, Tick)') failed: ok=%d target='%s' event='%s'\n",
                         s1.ok, s1.target.toUtf8().constData(), s1.event.toUtf8().constData());
            return 1;
        }

        const auto s2 = app::expr::parseSendToForm(QStringLiteral("sendTo(  my-actor.1  ,  NEXT_STEP  )"));
        if (!s2.ok || s2.target != QStringLiteral("my-actor.1") || s2.event != QStringLiteral("NEXT_STEP")) {
            std::fprintf(stderr, "FAIL: parseSendToForm with whitespace/dots failed: ok=%d\n", s2.ok);
            return 1;
        }

        // Silent fallbacks
        const auto sn1 = app::expr::parseSendToForm(QStringLiteral("sendTokens()"));
        if (sn1.ok || !sn1.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseSendToForm('sendTokens()') should be silent fallback\n");
            return 1;
        }

        const auto sn2 = app::expr::parseSendToForm(QStringLiteral("count = 1"));
        if (sn2.ok || !sn2.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseSendToForm('count = 1') should be silent fallback\n");
            return 1;
        }

        // Malformed sendTo forms
        const auto sbad1 = app::expr::parseSendToForm(QStringLiteral("sendTo()"));
        if (sbad1.ok || sbad1.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseSendToForm('sendTo()') should fail with message\n");
            return 1;
        }

        const auto sbad2 = app::expr::parseSendToForm(QStringLiteral("sendTo(actor)"));
        if (sbad2.ok || sbad2.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseSendToForm('sendTo(actor)') should fail with missing 2nd arg message\n");
            return 1;
        }

        const auto sbad3 = app::expr::parseSendToForm(QStringLiteral("sendTo(actor, )"));
        if (sbad3.ok || sbad3.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseSendToForm('sendTo(actor, )') should fail with empty event message\n");
            return 1;
        }

        // 2. parseSendParentForm positive cases
        const auto p1 = app::expr::parseSendParentForm(QStringLiteral("sendParent(Done)"));
        if (!p1.ok || p1.event != QStringLiteral("Done")) {
            std::fprintf(stderr, "FAIL: parseSendParentForm('sendParent(Done)') failed: ok=%d event='%s'\n",
                         p1.ok, p1.event.toUtf8().constData());
            return 1;
        }

        const auto p2 = app::expr::parseSendParentForm(QStringLiteral("sendParent(  CHILD_PROGRESS  )"));
        if (!p2.ok || p2.event != QStringLiteral("CHILD_PROGRESS")) {
            std::fprintf(stderr, "FAIL: parseSendParentForm with whitespace failed: ok=%d\n", p2.ok);
            return 1;
        }

        // Silent fallback
        const auto pn1 = app::expr::parseSendParentForm(QStringLiteral("sendParentalAdvice"));
        if (pn1.ok || !pn1.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseSendParentForm('sendParentalAdvice') should be silent fallback\n");
            return 1;
        }

        // Malformed sendParent forms
        const auto pbad1 = app::expr::parseSendParentForm(QStringLiteral("sendParent()"));
        if (pbad1.ok || pbad1.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseSendParentForm('sendParent()') should fail with message\n");
            return 1;
        }

        const auto pbad2 = app::expr::parseSendParentForm(QStringLiteral("sendParent(Done"));
        if (pbad2.ok || pbad2.message.isEmpty()) {
            std::fprintf(stderr, "FAIL: parseSendParentForm('sendParent(Done') should fail with missing ')'\n");
            return 1;
        }

        // 3. LogicInventory classification
        app::Machine testMachine;
        app::Transition t1;
        t1.id = 1;
        t1.from = 1;
        t1.to = 2;
        t1.event = QStringLiteral("Go");
        t1.action = QStringLiteral("sendTo(downloader, Pause)");
        testMachine.transitions.push_back(t1);

        app::Transition t2;
        t2.id = 2;
        t2.from = 2;
        t2.to = 3;
        t2.event = QStringLiteral("Finish");
        t2.action = QStringLiteral("sendParent(ChildComplete)");
        testMachine.transitions.push_back(t2);

        const auto inv = app::inventory(testMachine);
        if (inv.actions.size() != 2) {
            std::fprintf(stderr, "FAIL: inventory did not classify 2 actions, got %d\n", static_cast<int>(inv.actions.size()));
            return 1;
        }
        if (inv.actions[0].rowClass != app::LogicRowClass::SendTo) {
            std::fprintf(stderr, "FAIL: action 0 not classified as SendTo\n");
            return 1;
        }
        if (inv.actions[1].rowClass != app::LogicRowClass::SendParent) {
            std::fprintf(stderr, "FAIL: action 1 not classified as SendParent\n");
            return 1;
        }

        // 4. XState v5 IO export and import
        const QByteArray xstateJson = R"({
            "id": "ActorTest",
            "initial": "Idle",
            "states": {
                "Idle": {
                    "entry": [
                        { "type": "xstate.sendTo", "params": { "to": "worker1", "event": { "type": "START" } } },
                        { "type": "xstate.sendParent", "params": { "event": { "type": "CHILD_READY" } } }
                    ]
                }
            }
        })";
        const auto inDoc = QJsonDocument::fromJson(xstateJson);
        const auto impRes = app::machineFromXStateJson(inDoc.object());
        if (!impRes.ok || impRes.machine.states.size() != 1) {
            std::fprintf(stderr, "FAIL: machineFromXStateJson for sendTo/sendParent failed\n");
            return 1;
        }
        const auto& idleState = impRes.machine.states[0];
        if (idleState.entryActions.size() != 2) {
            std::fprintf(stderr, "FAIL: expected 2 entry actions, got %d\n", static_cast<int>(idleState.entryActions.size()));
            return 1;
        }
        if (idleState.entryActions[0] != QStringLiteral("sendTo(worker1, START)")) {
            std::fprintf(stderr, "FAIL: expected sendTo(worker1, START), got '%s'\n", idleState.entryActions[0].toUtf8().constData());
            return 1;
        }
        if (idleState.entryActions[1] != QStringLiteral("sendParent(CHILD_READY)")) {
            std::fprintf(stderr, "FAIL: expected sendParent(CHILD_READY), got '%s'\n", idleState.entryActions[1].toUtf8().constData());
            return 1;
        }

        // Re-export
        const auto expRes = app::machineToXStateJson(impRes.machine);
        if (!expRes.ok) {
            std::fprintf(stderr, "FAIL: machineToXStateJson failed: %s\n", expRes.error.toUtf8().constData());
            return 1;
        }
        const auto expEntry = expRes.json.value(QStringLiteral("states")).toObject()
                                    .value(QStringLiteral("Idle")).toObject()
                                    .value(QStringLiteral("entry")).toArray();
        if (expEntry.size() != 2) {
            std::fprintf(stderr, "FAIL: re-exported entry size not 2\n");
            return 1;
        }
        const auto sendToObj = expEntry.at(0).toObject();
        if (sendToObj.value(QStringLiteral("type")).toString() != QStringLiteral("xstate.sendTo") ||
            sendToObj.value(QStringLiteral("params")).toObject().value(QStringLiteral("to")).toString() != QStringLiteral("worker1") ||
            sendToObj.value(QStringLiteral("params")).toObject().value(QStringLiteral("event")).toObject().value(QStringLiteral("type")).toString() != QStringLiteral("START")) {
            std::fprintf(stderr, "FAIL: re-exported sendTo object mismatch\n");
            return 1;
        }
        const auto sendParentObj = expEntry.at(1).toObject();
        if (sendParentObj.value(QStringLiteral("type")).toString() != QStringLiteral("xstate.sendParent") ||
            sendParentObj.value(QStringLiteral("params")).toObject().value(QStringLiteral("event")).toObject().value(QStringLiteral("type")).toString() != QStringLiteral("CHILD_READY")) {
            std::fprintf(stderr, "FAIL: re-exported sendParent object mismatch\n");
            return 1;
        }

        // 5. Validator Check 11 tests for sendTo and sendParent
        app::Machine valMachine;
        app::State sActive;
        sActive.id = 10;
        sActive.name = QStringLiteral("Active");
        sActive.invokeSrc = QStringLiteral("myActor");
        sActive.invokeId = QStringLiteral("actor1");
        valMachine.states.push_back(sActive);

        app::Transition tValid;
        tValid.id = 1;
        tValid.from = 10;
        tValid.to = 10;
        tValid.event = QStringLiteral("Tick");
        tValid.action = QStringLiteral("sendTo(actor1, PING)");
        valMachine.transitions.push_back(tValid);

        app::Transition tParent;
        tParent.id = 2;
        tParent.from = 10;
        tParent.to = 10;
        tParent.event = QStringLiteral("Notify");
        tParent.action = QStringLiteral("sendParent(STATUS_UPDATE)");
        valMachine.transitions.push_back(tParent);

        // Valid actions should produce 0 warnings/errors for check 11
        auto problems = app::validate(valMachine);
        for (const auto& p : problems) {
            if (p.text.contains(QStringLiteral("sendTo")) || p.text.contains(QStringLiteral("sendParent"))) {
                std::fprintf(stderr, "FAIL: unexpected problem for valid sendTo/sendParent: %s\n", p.text.toUtf8().constData());
                return 1;
            }
        }

        // Unknown target actor produces a warning
        valMachine.transitions[0].action = QStringLiteral("sendTo(unknownWorker, PING)");
        problems = app::validate(valMachine);
        bool foundWarn = false;
        for (const auto& p : problems) {
            if (p.severity == app::ProblemSeverity::Warning && p.text.contains(QStringLiteral("not declared as an invokeId"))) {
                foundWarn = true;
                break;
            }
        }
        if (!foundWarn) {
            std::fprintf(stderr, "FAIL: expected warning for unknown sendTo target\n");
            return 1;
        }

        // Malformed sendTo syntax produces an error
        valMachine.transitions[0].action = QStringLiteral("sendTo(actor1)");
        problems = app::validate(valMachine);
        bool foundErr = false;
        for (const auto& p : problems) {
            if (p.severity == app::ProblemSeverity::Error && p.text.contains(QStringLiteral("looks like a sendTo but is not valid"))) {
                foundErr = true;
                break;
            }
        }
        if (!foundErr) {
            std::fprintf(stderr, "FAIL: expected error for malformed sendTo action\n");
            return 1;
        }

        // Malformed sendParent syntax produces an error
        valMachine.transitions[1].action = QStringLiteral("sendParent()");
        problems = app::validate(valMachine);
        bool foundParentErr = false;
        for (const auto& p : problems) {
            if (p.severity == app::ProblemSeverity::Error && p.text.contains(QStringLiteral("looks like a sendParent but is not valid"))) {
                foundParentErr = true;
                break;
            }
        }
        if (!foundParentErr) {
            std::fprintf(stderr, "FAIL: expected error for malformed sendParent action\n");
            return 1;
        }
    }

    return 0;
}
