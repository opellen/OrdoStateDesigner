#pragma once

#include <deque>
#include <vector>

#include <ordo/core/agent.h>

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>

#include "model/machine.h"
#include "model/sim_events.h"

namespace app {

class MachineDocAgent;

// One guard decision made by guardPasses(); hook guards are recorded like expression guards.
struct GuardEvaluation {
    quint64 transitionId = 0;
    // The guard string as authored; blank = no guard (still recorded).
    QString source;
    // False for an undecidable expression guard (parse/type error, missing context value,
    // non-Bool result); true for everything else, hook guards included.
    bool decided = true;
    // What guardPasses() returned; always false when !decided.
    bool result = false;
};

// Topology interpreter: walks MachineDocAgent's Machine directly and never executes
// generated code. configuration_ is the full active-state set, closed under
// State::parentId and kept in document order.
// A macrostep is one or more microsteps fired together (parallel survivors of one
// event); ConfigurationChanged publishes once per macrostep, and back() undoes one.
class SimulationAgent : public ordo::core::Agent {
public:
    static constexpr const char* kName = "simulation";

    SimulationAgent() : Agent(kName) {}

    // Leaving Simulate stops the run: running() false, configuration() empties (publishing
    // ActiveStateChanged{old, 0, 0} + ConfigurationChanged if something was active), timers
    // clear, and lastGuardEvaluations() clears. Trace, macrostep history and guard toggles stay.
    void setMode(events::Mode mode);
    events::Mode mode() const { return mode_; }

    // No-op if running(). From a fresh state (empty configuration) it activates
    // Machine::initialStateId, clears trace and macrostep history, arms timers, and publishes
    // ActiveStateChanged{0, initial, 0} + ConfigurationChanged. Resuming from Pause keeps
    // the position and armed countdowns. Either way running() becomes true.
    void run();
    // No-op unless running(). Timers freeze because tick() refuses to advance while !running().
    void pause();
    // Re-derives like a fresh Run and leaves running() true. SimulationReset always
    // publishes; with no initial state set, running() becomes false and everything clears.
    void reset();
    // No-op unless running(). Per atomic active state (document order), walks self then
    // ancestors for the first transition matching `name` whose guard passes. A candidate
    // whose source lies in an earlier survivor's exit set is dropped, and the survivors fire
    // as one macrostep. The root (from == 0) fallback fires once, only if no survivor
    // remains. An unmatched event is silently ignored: no trace line, no fact.
    void sendEvent(const QString& name, const QVariant& payload = QVariant());
    // Always publishes GuardResultChanged, even when the value did not change.
    void setGuardResult(const QString& name, bool result);
    // Advances armed delayed transitions (no-op while !running()). Each due firing (lowest
    // remaining time, ties by lowest transitionId) is its own one-microstep macrostep, fired only
    // if its owner state is still active (root owner 0 always counts). A targetless firing lets
    // the sweep continue; a normal one returns.
    void tick(int elapsedMs);
    // No-op unless running() and a macrostep has fired. Re-derives the initial activation and
    // replays the first N-1 macrosteps through fireMicrostep (guards are not re-checked), so
    // one back() undoes one macrostep.
    void back();
    // Resolves a live invocation as "done.invoke.<id>" (ok) or "error.platform.<id>" through
    // the same dispatchEvent() path as sendEvent(), with `payload` bound as "output"/"error"
    // for that one macrostep only. An id naming no live invocation is a silent no-op.
    void completeInvocation(const QString& invokeId, bool ok, const QVariant& payload = QVariant());

    void toggleBreakpoint(quint64 stateId);
    void addBreakpoint(quint64 stateId);
    void removeBreakpoint(quint64 stateId);
    bool hasBreakpoint(quint64 stateId) const;
    const QSet<quint64>& breakpoints() const { return breakpoints_; }

    bool running() const { return running_; }
    // The single atomic (no active child) member of configuration(), or 0 when nothing is
    // active or several regions are. Callers needing the full picture read configuration().
    quint64 activeStateId() const;
    const std::vector<quint64>& configuration() const { return configuration_; }
    const QStringList& trace() const { return trace_; }
    const std::deque<QString>& internalQueue() const { return internalQueue_; }
    // Every fired transition id, oldest first, macrostep grouping flattened.
    std::vector<quint64> firedTransitionIds() const;
    // The guard toggle table's value for a hook name, not a guard decision.
    bool guardResult(const QString& name) const { return hookGuardPasses(name); }
    // Live extended state, name -> value. Written only by the seed in activateInitialState()
    // and by assigns inside a firing.
    const QVariantMap& contextValues() const { return contextValues_; }
    // Guard decisions of the current macrostep only, in call order. Cleared at the start of
    // each sendEvent() and each due tick firing, in activateInitialState() (so back() leaves it
    // empty: replay checks no guards), and when leaving Simulate. Resuming from Pause keeps it.
    const QVector<GuardEvaluation>& lastGuardEvaluations() const { return lastGuardEvaluations_; }
    // Effective invoke ids of the invocations currently live.
    QSet<QString> liveInvocationIds() const;

private:
    struct ArmedTimer {
        quint64 transitionId = 0;
        int remainingMs = 0;
        int durationMs = 0;
        bool periodic = false;
        quint64 ownerStateId = 0;
    };

    // A live invocation, recorded while its state is active (this class runs no real
    // service). `invokeId` is the effective id captured at arm time.
    struct ActiveInvocation {
        quint64 stateId = 0;
        QString invokeId;
    };

    // The invoke-completion payload bound into one macrostep's guard/assign evaluation.
    // guardPasses()/runAction() merge it into a local copy of contextValues_, never into
    // contextValues_ itself. Blank `name` = no payload in scope (the default everywhere
    // except completeInvocation()).
    struct InvocationPayload {
        QString name;  // "output" or "error" -- blank = none
        QVariant value;
    };

    // One recorded macrostep: its fired transition ids plus the payload it was fired with,
    // so back()'s replay reproduces an onDone/onError transition's assign.
    struct FiredMacrostep {
        std::vector<quint64> transitionIds;
        InvocationPayload payload;
    };

    // Resolves Machine::initialStateId, clears trace, macrostep history, historyRecords_ and
    // lastGuardEvaluations_, reseeds contextValues_ from Machine::context, activates the
    // initial state (one trace line, ActiveStateChanged + ConfigurationChanged), and arms
    // timers and invocations. Returns false, changing nothing, if no initial state resolves.
    // The one shared start for run(), reset() and back()'s replay base.
    bool activateInitialState(const QString& verb);

    // Selects and fires one macrostep for event `name`; the requires-running() check is the
    // caller's. Shared by sendEvent() and completeInvocation().
    void dispatchEvent(const QString& name, const InvocationPayload& payload);

    // Firing order: headline trace line, exit actions (deepest first), the transition's own
    // action, entry actions (parent first). ActiveStateChanged::fromId is transition.from as
    // declared (for a root firing, the first atomic state in the exit set); toId is
    // transition.to as declared. A targetless transition (to == 0) runs only its action: no
    // exit/entry, no ActiveStateChanged, timers untouched, but TransitionFired still publishes.
    // A self-transition re-exits and re-enters the whole subtree.

    // Fires ONE microstep: trace lines, every action through runAction() (so an assign
    // executes only here, on every firing path including back()'s replay), the LCCA-bounded
    // exit/entry, configuration update, ActiveStateChanged + TransitionFired, timer and
    // invocation arm/disarm, and recording each exited state in its parent's historyRecords_.
    // A History target is redirected to its parent compound first. Never checks a guard.
    // Does not touch firedMacrosteps_ or publish ConfigurationChanged (fireMacrostep does).
    void fireMicrostep(const MachineDocAgent& doc, const Transition& transition,
                        const InvocationPayload& payload = InvocationPayload());

    // Runs `transitions` in order through fireMicrostep(), then closes the macrostep: one
    // FiredMacrostep entry (ids plus `payload`) and one ConfigurationChanged.
    void fireMacrostep(const MachineDocAgent& doc, const std::vector<const Transition*>& transitions,
                        const InvocationPayload& payload = InvocationPayload());

    // ---- topology helpers ------------------------------------------------

    // `stateId`'s index in doc.machine().states (document order), or -1 if unknown.
    int docOrderIndex(const MachineDocAgent& doc, quint64 stateId) const;
    // True if `ancestorId` is a proper ancestor of `descendantId`. Root (0) is an ancestor
    // of every state; a state is never its own ancestor.
    bool isAncestorOf(const MachineDocAgent& doc, quint64 ancestorId, quint64 descendantId) const;
    // `stateId`'s parentId; 0 for the root itself.
    quint64 parentOf(const MachineDocAgent& doc, quint64 stateId) const;
    // The transition domain: the closest proper ancestor of `sourceId` that has the target(s)
    // as proper descendants, ending at 0 (root). A self-transition resolves to
    // parentOf(source); a root-originated one (source 0) resolves to 0, so its exit set is
    // the whole configuration.
    quint64 lccaOf(const MachineDocAgent& doc, quint64 sourceId, quint64 targetId) const;
    quint64 lccaOf(const MachineDocAgent& doc, quint64 sourceId, const QList<quint64>& targetIds) const;
    // Direct children of `parentId` in document order, active or not.
    std::vector<quint64> childrenOf(const MachineDocAgent& doc, quint64 parentId) const;

    // ---- entry/exit sequencing -------------------------------------------

    // Every active proper descendant of `lcca`, deepest first by real parentId depth, reverse
    // document order among equals.
    std::vector<quint64> exitSequenceFor(const MachineDocAgent& doc, quint64 lcca) const;
    // parentId-chain length: 0 for a root-level state.
    int depthOf(const MachineDocAgent& doc, quint64 stateId) const;
    // How a History redirect overrides the default descent below its parent compound: None =
    // initialChildId (every child for Parallel); OneLevel = use the recorded child for the next
    // compound only (shallow); AllLevels = at every compound reached (deep). A Parallel state
    // always enters every region.
    enum class HistoryDescend { None, OneLevel, AllLevels };

    // The entry set for `targetId` under domain `lcca`: its not-yet-active ancestors below
    // lcca top-down (a Parallel ancestor on the path also enters its other children with the
    // default descent), then the descent below targetId honoring `historyPolicy`. Non-None
    // only when the original target was a History state and targetId is its parent compound.
    std::vector<quint64> entrySequenceFor(const MachineDocAgent& doc, quint64 targetId, quint64 lcca,
                                           HistoryDescend historyPolicy = HistoryDescend::None) const;
    std::vector<quint64> entrySequenceFor(const MachineDocAgent& doc, const QList<quint64>& targetIds,
                                           quint64 lcca) const;
    // Appends `stateId` then its default descent: every child for Parallel, else initialChildId
    // (recursively), or nothing for an atomic state; `policy` swaps in the recorded history child.
    void descendInto(const MachineDocAgent& doc, quint64 stateId, std::vector<quint64>& result,
                      HistoryDescend policy = HistoryDescend::None) const;

    // The atomic (no active child) members of configuration_, in document order.
    std::vector<quint64> atomicActives(const MachineDocAgent& doc) const;
    // Document-order-first id in `ids` with no child in `ids`; used for a root firing's
    // ActiveStateChanged::fromId, computed before the exit happens.
    quint64 firstAtomicWithin(const MachineDocAgent& doc, const std::vector<quint64>& ids) const;
    // The first transition leaving `stateId` (document order) whose event matches `name` and
    // whose guard passes. Non-const: an undecidable guard appends a trace line.
    const Transition* firstMatchingHandler(const MachineDocAgent& doc, quint64 stateId, const QString& name,
                                            const InvocationPayload& payload = InvocationPayload());
    // The first transition leaving `stateId` (document order) with isAlways() and a passing guard.
    const Transition* firstMatchingAlwaysHandler(const MachineDocAgent& doc, quint64 stateId);

    // Selects candidate always transitions across active configuration or root fallback.
    std::vector<const Transition*> selectAlwaysTransitions(const MachineDocAgent& doc);

    // Selects candidate transitions for an event across active configuration or root fallback.
    std::vector<const Transition*> selectTransitionsForEvent(const MachineDocAgent& doc, const QString& name,
                                                             const InvocationPayload& payload = InvocationPayload());

    // SCXML Run-To-Completion (RTC) microstep loop: evaluates always transitions
    // and drains internalQueue_ until quiescence or kMaxMicrosteps (100) is reached.
    void processMicrosteps(const MachineDocAgent& doc);

    // Eventless (always) transitions microstep loop: forwarded to processMicrosteps().
    void processAlwaysTransitions(const MachineDocAgent& doc);

    // A delayed transition has delayMs > 0 and a blank event (a non-blank event ignores
    // delayMs); it is owned by its source state. Arms `stateId`'s delayed transitions
    // (additive; several owners hold timers at once). The guard is checked at fire time, and
    // a false guard cancels that arming until the state is re-entered. Called per entered state.
    void armStateTimers(const MachineDocAgent& doc, quint64 stateId);
    // Cancels exactly `stateId`'s armed timers, never root's. Called per exited state.
    void disarmStateTimers(quint64 stateId);
    // Arms machine-level (from == 0) delayed transitions (owner 0), only from
    // activateInitialState(); ordinary state entry/exit never touches them.
    void armRootDelayedTransitions(const MachineDocAgent& doc);

    // Called wherever the timer pair is. Arm no-ops when the state declares no invoke, disarm
    // when it holds no live record; both append an "invoke: <id> started/cancelled" trace line.
    void armStateInvocation(const MachineDocAgent& doc, quint64 stateId);
    void disarmStateInvocation(quint64 stateId);

    // configuration_ as the QVector events::ConfigurationChanged carries.
    QVector<quint64> configurationSnapshot() const;

    void appendTrace(const QString& line);
    QString transitionHeadline(const Transition& transition, const QString& destName) const;

    // ---- extended state ----------------------------------------------------

    // The guard decision, recorded as one GuardEvaluation: blank -> true, a bare identifier ->
    // hookGuardPasses(), anything else is parsed, type-checked and evaluated over
    // contextValues_ (with `payload` merged into a local copy). An undecidable guard is false,
    // never true and never a throw, and traces "guard: <source> → false (<reason>)"; back()
    // does not reproduce that line.
    bool guardPasses(const QString& guardSource, quint64 transitionId,
                      const InvocationPayload& payload = InvocationPayload());
    // The named-hook half: blank -> true, else the toggle table (default true). Const, so
    // guardResult() cannot start evaluating expressions.
    bool hookGuardPasses(const QString& guardName) const;
    // Runs one action string. An assign form (expr::parseAssignForm) evaluates its RHS over
    // contextValues_, coerces it to the target's ContextType (Int/Double interconvert, a
    // Double into an Int truncates; other pairings refuse) and traces "assign: ..."; anything
    // else is a named hook. A refusal writes nothing and traces "assign skipped: ...". Actions
    // run before the target is entered, so an assign is visible to the destination's entry
    // actions but not to the guard that selected the transition.
    void runAction(const MachineDocAgent& doc, const QString& action,
                    const InvocationPayload& payload = InvocationPayload());
    // Reseeds contextValues_ from ContextVariable::initialValue per declared type (String
    // verbatim, Bool case-insensitive). A malformed value seeds the type's zero value and
    // traces one "context: ..." line; it never throws or aborts a run. Called only from
    // activateInitialState(), so a resume from Pause keeps the current values.
    void seedContext(const MachineDocAgent& doc);

    events::Mode mode_ = events::Mode::Design;
    bool running_ = false;
    // The active-state set, document order; empty = nothing active.
    std::vector<quint64> configuration_;
    std::deque<QString> internalQueue_;
    QStringList trace_;
    // One entry per macrostep; transitionIds has size > 1 only for parallel survivors.
    std::vector<FiredMacrostep> firedMacrosteps_;
    std::vector<ArmedTimer> armedTimers_;
    // Live invocations, armed and dropped alongside armedTimers_.
    std::vector<ActiveInvocation> activeInvocations_;
    // compoundId -> its last active direct child, written by fireMicrostep() for each exited
    // state under its parentId (latest wins; root exits are not recorded). A History
    // redirect reads it: shallow at its parent only, deep at every compound level below. A
    // compound with no record falls back to initialChildId. Cleared by activateInitialState().
    QMap<quint64, quint64> historyRecords_;
    QMap<QString, bool> guardResults_;
    // Context variable name -> value in expr::evaluate()'s QVariant vocabulary. Not named
    // context_ because ordo::core::Agent owns that name.
    QVariantMap contextValues_;
    QVector<GuardEvaluation> lastGuardEvaluations_;
    QSet<quint64> breakpoints_;
    quint64 hitBreakpointStateId_ = 0;
};

}  // namespace app
