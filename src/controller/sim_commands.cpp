#include "controller/sim_commands.h"

#include "model/sim_agent.h"

namespace app {

namespace {

bool inSimulate(ordo::core::CommandContext& context) {
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    return sim && sim->mode() == events::Mode::Simulate;
}

// Starts or stops `clock` to match `sim.running()` after a Run/Pause/Reset call.
void syncClock(SimulationAgent& sim, SimClock& clock) {
    if (sim.running()) {
        clock.start();
    } else {
        clock.stop();
    }
}

}  // namespace

void SetModeCommand::execute(const events::SetModeRequested& event, ordo::core::CommandContext& context) {
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim) {
        return;
    }
    sim->setMode(event.mode);
}

void RunCommand::execute(const events::RunRequested&, ordo::core::CommandContext& context) {
    if (!inSimulate(context)) {
        return;
    }
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim) {
        return;
    }
    sim->run();
    syncClock(*sim, clock_.get());
}

void PauseCommand::execute(const events::PauseRequested&, ordo::core::CommandContext& context) {
    if (!inSimulate(context)) {
        return;
    }
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim) {
        return;
    }
    sim->pause();
    syncClock(*sim, clock_.get());
}

void ResetCommand::execute(const events::ResetRequested&, ordo::core::CommandContext& context) {
    if (!inSimulate(context)) {
        return;
    }
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim) {
        return;
    }
    sim->reset();
    syncClock(*sim, clock_.get());
}

void SendEventCommand::execute(const events::SendEventRequested& event, ordo::core::CommandContext& context) {
    if (!inSimulate(context)) {
        return;
    }
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim || sim->configuration().empty()) {
        return;
    }
    sim->sendEvent(event.name, event.payload);
}

void SetGuardResultCommand::execute(const events::SetGuardResultRequested& event,
                                     ordo::core::CommandContext& context) {
    if (!inSimulate(context)) {
        return;
    }
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim) {
        return;
    }
    sim->setGuardResult(event.name, event.result);
}

void TickCommand::execute(const events::TickElapsed& event, ordo::core::CommandContext& context) {
    if (!inSimulate(context)) {
        return;
    }
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim) {
        return;
    }
    sim->tick(event.elapsedMs);
}

void BackCommand::execute(const events::BackRequested&, ordo::core::CommandContext& context) {
    if (!inSimulate(context)) {
        return;
    }
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim || !sim->running()) {
        return;
    }
    sim->back();
}

void CompleteInvocationCommand::execute(const events::CompleteInvocationRequested& event,
                                         ordo::core::CommandContext& context) {
    if (!inSimulate(context)) {
        return;
    }
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim || !sim->running()) {
        return;
    }
    sim->completeInvocation(event.invokeId, event.ok, event.payload);
}

void ToggleBreakpointCommand::execute(const events::ToggleBreakpointRequested& event,
                                      ordo::core::CommandContext& context) {
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    if (!sim) {
        return;
    }
    sim->toggleBreakpoint(event.stateId);
}

void SetTimescaleCommand::execute(const events::SetTimescaleRequested& event,
                                  ordo::core::CommandContext& context) {
    clock_.get().setTimeScale(event.scale);
    context.send(events::TimescaleChanged{.scale = event.scale});
}

}  // namespace app
