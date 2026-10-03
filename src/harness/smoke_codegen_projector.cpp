#include <cstdio>

#include "harness/harness.h"
#include "infra/node_code_projector.h"
#include "model/machine.h"

int runCodegenProjectorSmoke() {
    std::printf("[SMOKE] Running node code projector smoke...\n");

    app::Machine machine;
    machine.name = QStringLiteral("Coffee Machine");
    machine.initialStateId = 1;

    app::ContextVariable var;
    var.name = QStringLiteral("temp");
    var.type = app::ContextType::Int;
    var.initialValue = QStringLiteral("20");
    machine.context.push_back(var);

    app::State state1;
    state1.id = 1;
    state1.name = QStringLiteral("Heating Up");
    state1.entryActions = {QStringLiteral("startHeating"), QStringLiteral("showStatus")};
    state1.exitActions = {QStringLiteral("clearProcessing")};
    state1.description = QStringLiteral("Heating water tank to operating temperature");
    machine.states.push_back(state1);

    app::State state2;
    state2.id = 2;
    state2.name = QStringLiteral("Ready");
    machine.states.push_back(state2);

    app::Transition trans1;
    trans1.id = 101;
    trans1.from = 1;
    trans1.to = 2;
    trans1.event = QStringLiteral("TempReached");
    trans1.guard = QStringLiteral("isHot");
    trans1.action = QStringLiteral("beep");
    machine.transitions.push_back(trans1);

    // 1. Test projectStateCode for HeatingUp
    const app::NodeCodeProjection stateProj = app::projectStateCode(machine, 1);
    if (stateProj.isEmpty()) {
        std::fprintf(stderr, "FAIL: projectStateCode returned empty projection for valid state\n");
        return 1;
    }

    if (!stateProj.stateEnumSnippet.contains("CoffeeMachineState::HeatingUp")) {
        std::fprintf(stderr, "FAIL: stateEnumSnippet missing scoped state name: %s\n",
                     stateProj.stateEnumSnippet.toUtf8().constData());
        return 1;
    }

    if (!stateProj.hooksSnippet.contains("startHeating(Context& ctx)") ||
        !stateProj.hooksSnippet.contains("clearProcessing(Context& ctx)") ||
        !stateProj.hooksSnippet.contains("isHot(const Context& ctx)")) {
        std::fprintf(stderr, "FAIL: hooksSnippet missing expected methods: %s\n",
                     stateProj.hooksSnippet.toUtf8().constData());
        return 1;
    }

    if (!stateProj.coreHandlerSnippet.contains("case CoffeeMachineState::HeatingUp:") ||
        !stateProj.coreHandlerSnippet.contains("transitionTo(CoffeeMachineState::Ready);") ||
        !stateProj.coreHandlerSnippet.contains("Event::TempReached")) {
        std::fprintf(stderr, "FAIL: coreHandlerSnippet missing expected logic: %s\n",
                     stateProj.coreHandlerSnippet.toUtf8().constData());
        return 1;
    }

    // 2. Test projectTransitionCode for trans1
    const app::NodeCodeProjection transProj = app::projectTransitionCode(machine, 101);
    if (transProj.isEmpty()) {
        std::fprintf(stderr, "FAIL: projectTransitionCode returned empty projection for valid transition\n");
        return 1;
    }

    if (!transProj.stateEnumSnippet.contains("TempReached") ||
        !transProj.stateEnumSnippet.contains("HeatingUp") ||
        !transProj.stateEnumSnippet.contains("Ready")) {
        std::fprintf(stderr, "FAIL: transProj stateEnumSnippet missing header info: %s\n",
                     transProj.stateEnumSnippet.toUtf8().constData());
        return 1;
    }

    if (!transProj.hooksSnippet.contains("isHot") || !transProj.hooksSnippet.contains("beep")) {
        std::fprintf(stderr, "FAIL: transProj hooksSnippet missing isHot or beep: %s\n",
                     transProj.hooksSnippet.toUtf8().constData());
        return 1;
    }

    if (!transProj.coreHandlerSnippet.contains("state_ == CoffeeMachineState::HeatingUp") ||
        !transProj.coreHandlerSnippet.contains("transitionTo(CoffeeMachineState::Ready);")) {
        std::fprintf(stderr, "FAIL: transProj coreHandlerSnippet missing dispatch: %s\n",
                     transProj.coreHandlerSnippet.toUtf8().constData());
        return 1;
    }

    // 3. Test invalid IDs
    const app::NodeCodeProjection invalidState = app::projectStateCode(machine, 9999);
    if (!invalidState.isEmpty()) {
        std::fprintf(stderr, "FAIL: invalid state id returned non-empty projection\n");
        return 1;
    }

    const app::NodeCodeProjection invalidTrans = app::projectTransitionCode(machine, 9999);
    if (!invalidTrans.isEmpty()) {
        std::fprintf(stderr, "FAIL: invalid transition id returned non-empty projection\n");
        return 1;
    }

    std::printf("[SMOKE] PASS: node code projector smoke passed\n");
    return 0;
}
