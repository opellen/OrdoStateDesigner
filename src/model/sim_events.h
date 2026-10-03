#pragma once

#include <string_view>

#include <QString>
#include <QVariant>
#include <QVector>
#include <QtGlobal>

namespace app::events {

// The simulation lane's event vocabulary. SimulationAgent interprets
// MachineDocAgent's topology directly and never executes generated code.

enum class Mode { Design, Simulate };

// ---- Intents: mode switch --------------------------------------------------

struct SetModeRequested {
    static constexpr std::string_view eventName = "SetModeRequested";
    Mode mode = Mode::Design;
};

// ---- Intents: run/pause/reset/send-event/guard-toggle ----------------------
// All but SetModeRequested are rejected unless SimulationAgent::mode() == Simulate;
// SendEventRequested/BackRequested also need running(). A rejected intent is
// silent: no agent call, no fact.

struct RunRequested {
    static constexpr std::string_view eventName = "RunRequested";
};

struct PauseRequested {
    static constexpr std::string_view eventName = "PauseRequested";
};

struct ResetRequested {
    static constexpr std::string_view eventName = "ResetRequested";
};

struct SendEventRequested {
    static constexpr std::string_view eventName = "SendEventRequested";
    QString name;
    QVariant payload;
};

// SimulationAgent holds a name->bool map consulted at fire time; a name never
// toggled here defaults to true.
struct SetGuardResultRequested {
    static constexpr std::string_view eventName = "SetGuardResultRequested";
    QString name;
    bool result = true;
};

// Deterministic replay: re-walk the topology from the initial state through the
// first N-1 recorded firings, guards bypassed (the recording already proved the path).
struct BackRequested {
    static constexpr std::string_view eventName = "BackRequested";
};

// Explicit invoke completion; the sim runs no real service. `invokeId` is the
// effective id of the completing invocation, `ok` selects done.invoke vs
// error.platform, and `payload` is the "output"/"error" value for that one
// macrostep only (never written into the context). Rejected unless running().
// A completion for an invocation that is no longer live is a silent no-op.
struct CompleteInvocationRequested {
    static constexpr std::string_view eventName = "CompleteInvocationRequested";
    QString invokeId;
    bool ok = true;
    QVariant payload;
};

struct ToggleBreakpointRequested {
    static constexpr std::string_view eventName = "ToggleBreakpointRequested";
    quint64 stateId = 0;
};

struct SetTimescaleRequested {
    static constexpr std::string_view eventName = "SetTimescaleRequested";
    double scale = 1.0;
};

// ---- Internal event: the infrastructure's own intent -----------------------
// Never sent by user action: infra/sim_clock.h's SimClock posts it on the UI
// thread, from its real-mode timer or a manual advanceTicks(ms) call.
struct TickElapsed {
    static constexpr std::string_view eventName = "TickElapsed";
    int elapsedMs = 0;
};

// ---- Facts ------------------------------------------------------------------

struct ModeChanged {
    static constexpr std::string_view eventName = "ModeChanged";
    Mode mode = Mode::Design;
};

struct SimulationStarted {
    static constexpr std::string_view eventName = "SimulationStarted";
};

struct SimulationPaused {
    static constexpr std::string_view eventName = "SimulationPaused";
};

struct SimulationReset {
    static constexpr std::string_view eventName = "SimulationReset";
};

struct BreakpointToggled {
    static constexpr std::string_view eventName = "BreakpointToggled";
    quint64 stateId = 0;
    bool enabled = false;
};

struct BreakpointHit {
    static constexpr std::string_view eventName = "BreakpointHit";
    quint64 stateId = 0;
};

struct TimescaleChanged {
    static constexpr std::string_view eventName = "TimescaleChanged";
    double scale = 1.0;
};

// fromId/toId/viaTransitionId are 0 where not applicable (e.g. viaTransitionId
// on the very first activation out of Run/Reset, fromId on that same
// activation when nothing was active before it).
struct ActiveStateChanged {
    static constexpr std::string_view eventName = "ActiveStateChanged";
    quint64 fromId = 0;
    quint64 toId = 0;
    quint64 viaTransitionId = 0;
};

// The full active-state set in document order, mirroring
// SimulationAgent::configuration(). Published once per macrostep (not per
// microstep), on activation (Run fresh, Reset), and once more (empty) on
// leaving Simulate.
struct ConfigurationChanged {
    static constexpr std::string_view eventName = "ConfigurationChanged";
    QVector<quint64> activeIds;
};

struct TransitionFired {
    static constexpr std::string_view eventName = "TransitionFired";
    quint64 transitionId = 0;
    QString event;  // echoes Transition::event -- empty for a delayed firing
};

// One presentation-ready trace line, in the order SimulationAgent appended it.
struct TraceAppended {
    static constexpr std::string_view eventName = "TraceAppended";
    QString text;
};

struct GuardResultChanged {
    static constexpr std::string_view eventName = "GuardResultChanged";
    QString name;
    bool result = true;
};

}  // namespace app::events
