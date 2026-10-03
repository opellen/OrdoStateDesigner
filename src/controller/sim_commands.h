#pragma once

#include <functional>

#include <ordo/core/command.h>

#include "infra/sim_clock.h"
#include "model/sim_events.h"

namespace app {

// The simulation command family driving SimulationAgent. Policy:
//   1. Every intent except SetModeRequested requires SimulationAgent::mode() == Simulate;
//      otherwise it is rejected silently (no agent call, no fact).
//   2. SendEvent, Back and CompleteInvocation also require running().
// Sim state is never undo-captured, so these commands are registered unwrapped.
// Run/Pause/Reset also sync SimClock to SimulationAgent::running().
class SetModeCommand : public ordo::core::Command<events::SetModeRequested> {
public:
    void execute(const events::SetModeRequested& event, ordo::core::CommandContext& context) override;
};

class RunCommand : public ordo::core::Command<events::RunRequested> {
public:
    explicit RunCommand(SimClock& clock) : clock_(clock) {}
    void execute(const events::RunRequested& event, ordo::core::CommandContext& context) override;

private:
    std::reference_wrapper<SimClock> clock_;
};

class PauseCommand : public ordo::core::Command<events::PauseRequested> {
public:
    explicit PauseCommand(SimClock& clock) : clock_(clock) {}
    void execute(const events::PauseRequested& event, ordo::core::CommandContext& context) override;

private:
    std::reference_wrapper<SimClock> clock_;
};

class ResetCommand : public ordo::core::Command<events::ResetRequested> {
public:
    explicit ResetCommand(SimClock& clock) : clock_(clock) {}
    void execute(const events::ResetRequested& event, ordo::core::CommandContext& context) override;

private:
    std::reference_wrapper<SimClock> clock_;
};

class SendEventCommand : public ordo::core::Command<events::SendEventRequested> {
public:
    void execute(const events::SendEventRequested& event, ordo::core::CommandContext& context) override;
};

class SetGuardResultCommand : public ordo::core::Command<events::SetGuardResultRequested> {
public:
    void execute(const events::SetGuardResultRequested& event, ordo::core::CommandContext& context) override;
};

// Consumes TickElapsed, posted by SimClock (never by user action).
class TickCommand : public ordo::core::Command<events::TickElapsed> {
public:
    void execute(const events::TickElapsed& event, ordo::core::CommandContext& context) override;
};

class BackCommand : public ordo::core::Command<events::BackRequested> {
public:
    void execute(const events::BackRequested& event, ordo::core::CommandContext& context) override;
};

// Door onto SimulationAgent::completeInvocation().
class CompleteInvocationCommand : public ordo::core::Command<events::CompleteInvocationRequested> {
public:
    void execute(const events::CompleteInvocationRequested& event, ordo::core::CommandContext& context) override;
};

class ToggleBreakpointCommand : public ordo::core::Command<events::ToggleBreakpointRequested> {
public:
    void execute(const events::ToggleBreakpointRequested& event, ordo::core::CommandContext& context) override;
};

class SetTimescaleCommand : public ordo::core::Command<events::SetTimescaleRequested> {
public:
    explicit SetTimescaleCommand(SimClock& clock) : clock_(clock) {}
    void execute(const events::SetTimescaleRequested& event, ordo::core::CommandContext& context) override;

private:
    std::reference_wrapper<SimClock> clock_;
};

}  // namespace app
