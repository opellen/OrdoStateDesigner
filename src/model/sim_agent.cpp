#include "model/sim_agent.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include "infra/expression.h"
#include "model/machine_doc.h"

namespace app {


void SimulationAgent::setMode(events::Mode mode) {
    if (mode_ == events::Mode::Simulate && mode == events::Mode::Design) {
        // Trace, macrostep history and guardResults_ are left as-is on purpose.
        running_ = false;
        armedTimers_.clear();
        // Live invocations end silently with the run (no "cancelled" trace line).
        activeInvocations_.clear();
        // Cleared, unlike the trace: a stale record would describe a run that no longer exists.
        lastGuardEvaluations_.clear();
        const bool hadActive = !configuration_.empty();
        const quint64 previousActive = activeStateId();
        if (hadActive) {
            context().send(events::ActiveStateChanged{.fromId = previousActive, .toId = 0, .viaTransitionId = 0});
        }
        configuration_.clear();
        internalQueue_.clear();
        context().send(events::ConfigurationChanged{.activeIds = {}});
    }
    mode_ = mode;
    context().send(events::ModeChanged{.mode = mode_});
}

void SimulationAgent::run() {
    if (running_) {
        return;  // already running -- "start or resume" has nothing to do
    }
    const bool wasEmpty = configuration_.empty();
    if (wasEmpty) {
        if (!activateInitialState(QStringLiteral("init"))) {
            return;  // no initial state set on the machine -- nothing to run
        }
    }
    // else: resuming from Pause; configuration, trace and armed countdowns are untouched.
    running_ = true;
    context().send(events::SimulationStarted{});

    if (wasEmpty) {
        for (quint64 id : configuration_) {
            if (breakpoints_.contains(id)) {
                running_ = false;
                auto doc = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
                const State* s = doc ? doc->findState(id) : nullptr;
                const QString sName = s ? s->name : QString::number(id);
                appendTrace(QStringLiteral("breakpoint: ") + sName + QStringLiteral(" hit, paused"));
                context().send(events::SimulationPaused{});
                context().send(events::BreakpointHit{.stateId = id});
                break;
            }
        }
        if (running_) {
            auto doc = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
            if (doc) {
                processMicrosteps(*doc);
            }
        }
    }
}

void SimulationAgent::pause() {
    if (!running_) {
        return;
    }
    running_ = false;
    context().send(events::SimulationPaused{});
}

void SimulationAgent::reset() {
    const bool ok = activateInitialState(QStringLiteral("reset"));
    if (ok) {
        running_ = true;
    } else {
        // Defensive fallback: no initial state set, so there is nothing to reset to.
        running_ = false;
        const bool hadActive = !configuration_.empty();
        const quint64 previousActive = activeStateId();
        if (hadActive) {
            context().send(events::ActiveStateChanged{.fromId = previousActive, .toId = 0, .viaTransitionId = 0});
        }
        configuration_.clear();
        trace_.clear();
        firedMacrosteps_.clear();
        armedTimers_.clear();
        activeInvocations_.clear();
        lastGuardEvaluations_.clear();
        context().send(events::ConfigurationChanged{.activeIds = {}});
    }
    context().send(events::SimulationReset{});
    if (ok && running_) {
        for (quint64 id : configuration_) {
            if (breakpoints_.contains(id)) {
                running_ = false;
                auto doc = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
                const State* s = doc ? doc->findState(id) : nullptr;
                const QString sName = s ? s->name : QString::number(id);
                appendTrace(QStringLiteral("breakpoint: ") + sName + QStringLiteral(" hit, paused"));
                context().send(events::SimulationPaused{});
                context().send(events::BreakpointHit{.stateId = id});
                break;
            }
        }
        if (running_) {
            auto doc = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
            if (doc) {
                processMicrosteps(*doc);
            }
        }
    }
}

void SimulationAgent::sendEvent(const QString& name, const QVariant& payload) {
    if (name.isEmpty() || configuration_.empty()) {
        return;
    }
    InvocationPayload ip;
    if (payload.isValid() && !payload.isNull()) {
        ip.name = QStringLiteral("event");
        ip.value = payload;
    }
    dispatchEvent(name, ip);
}

std::vector<const Transition*> SimulationAgent::selectTransitionsForEvent(
    const MachineDocAgent& doc, const QString& name, const InvocationPayload& payload) {
    // Phase 1: one candidate per atomic active state, against the pre-event configuration;
    // walk self then ancestors, first passing match wins (never crossing into the root group).
    std::vector<const Transition*> candidates;
    for (quint64 atomicId : atomicActives(doc)) {
        quint64 walk = atomicId;
        const Transition* found = nullptr;
        while (walk != 0) {
            found = firstMatchingHandler(doc, walk, name, payload);
            if (found) {
                break;
            }
            walk = parentOf(doc, walk);
        }
        if (found) {
            candidates.push_back(found);
        }
    }

    // Phase 2: drop any candidate whose source lies in an earlier survivor's exit set; this
    // also de-duplicates two atomics that bubbled up to the same ancestor handler.
    std::vector<const Transition*> survivors;
    std::vector<quint64> exitedSoFar;
    for (const Transition* candidate : candidates) {
        if (std::find(exitedSoFar.begin(), exitedSoFar.end(), candidate->from) != exitedSoFar.end()) {
            continue;  // this candidate's source was already claimed by an earlier survivor's exit set
        }
        survivors.push_back(candidate);
        if (candidate->to != 0 || candidate->isMultiTarget()) {  // targetless candidates have no exit set
            const quint64 lcca = candidate->isMultiTarget()
                ? lccaOf(doc, candidate->from, candidate->targets)
                : lccaOf(doc, candidate->from, candidate->to);
            const std::vector<quint64> exitSet = exitSequenceFor(doc, lcca);
            exitedSoFar.insert(exitedSoFar.end(), exitSet.begin(), exitSet.end());
        }
    }

    if (!survivors.empty()) {
        return survivors;
    }

    // Root fallback: only reached when no state-level candidate survived. Tried in
    // specificity order (Exact > Prefix > Universal), document order for ties; the first
    // passing guard fires.
    struct RootCandidate {
        const Transition* transition;
        int specificity;
        int originalOrder;
    };
    std::vector<RootCandidate> rootMatches;
    int rootOrder = 0;
    for (const Transition& transition : doc.machine().transitions) {
        if (transition.from != 0 || transition.event.isEmpty()) {
            continue;
        }
        if (eventMatches(transition.event, name)) {
            rootMatches.push_back(RootCandidate{
                .transition = &transition,
                .specificity = eventDescriptorSpecificity(transition.event),
                .originalOrder = rootOrder++,
            });
        }
    }

    std::stable_sort(rootMatches.begin(), rootMatches.end(), [](const RootCandidate& a, const RootCandidate& b) {
        if (a.specificity != b.specificity) {
            return a.specificity > b.specificity;
        }
        return a.originalOrder < b.originalOrder;
    });

    for (const RootCandidate& c : rootMatches) {
        if (guardPasses(c.transition->guard, c.transition->id, payload)) {
            return {c.transition};
        }
    }
    return {};
}

void SimulationAgent::dispatchEvent(const QString& name, const InvocationPayload& payload) {
    auto doc = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }

    // Cleared before phase 1, not only on a successful firing, so a call where nothing
    // fires still shows every guard it tried.
    lastGuardEvaluations_.clear();

    const std::vector<const Transition*> toFire = selectTransitionsForEvent(*doc, name, payload);
    if (!toFire.empty()) {
        fireMacrostep(*doc, toFire, payload);
        if (running_) {
            processMicrosteps(*doc);
        }
    }
}

void SimulationAgent::setGuardResult(const QString& name, bool result) {
    guardResults_[name] = result;
    context().send(events::GuardResultChanged{.name = name, .result = result});
}

void SimulationAgent::tick(int elapsedMs) {
    if (!running_ || armedTimers_.empty()) {
        return;  // Pause freezes: no countdown advances while !running_
    }
    for (ArmedTimer& timer : armedTimers_) {
        timer.remainingMs -= elapsedMs;
    }
    auto doc = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    while (!armedTimers_.empty()) {
        const auto dueIt = std::min_element(
            armedTimers_.begin(), armedTimers_.end(), [](const ArmedTimer& a, const ArmedTimer& b) {
                if (a.remainingMs != b.remainingMs) {
                    return a.remainingMs < b.remainingMs;
                }
                return a.transitionId < b.transitionId;
            });
        if (dueIt->remainingMs > 0) {
            break;  // nothing due yet
        }
        const quint64 transitionId = dueIt->transitionId;
        const quint64 ownerStateId = dueIt->ownerStateId;
        const int durationMs = dueIt->durationMs;
        const bool periodic = dueIt->periodic;
        armedTimers_.erase(dueIt);  // either fires (rearms wholesale below) or is cancelled -- either way, gone
        const Transition* transition = doc->findTransition(transitionId);
        if (!transition) {
            continue;  // defensive: should not happen (edits are rejected while Simulate)
        }
        // A due timer fires only if its owner is still active; root's owner (0) always counts.
        const bool ownerActive = ownerStateId == 0 ||
            std::find(configuration_.begin(), configuration_.end(), ownerStateId) != configuration_.end();
        if (!ownerActive) {
            continue;  // stale -- its state already left the configuration some other way
        }
        // Each due firing is its own macrostep, so it gets a fresh guard record (a single
        // tick() can sweep several).
        lastGuardEvaluations_.clear();
        if (!guardPasses(transition->guard, transitionId)) {
            continue;  // guard false at fire time cancels this arming; no re-arm until re-entry
        }
        fireMacrostep(*doc, {transition});  // each due firing is its own macrostep
        if (running_) {
            processMicrosteps(*doc);
        }
        if (transition->to == 0) {
            // Targetless: nothing left the configuration, so its OTHER
            // armed timers stay live -- keep sweeping for anything else
            // already due. If periodic, re-arm itself with its original interval.
            if (periodic && running_) {
                armedTimers_.push_back(ArmedTimer{
                    .transitionId = transitionId,
                    .remainingMs = durationMs,
                    .durationMs = durationMs,
                    .periodic = true,
                    .ownerStateId = ownerStateId,
                });
            }
            continue;
        }
        return;  // the configuration changed -- any other still-due old timer is already moot
    }
}

void SimulationAgent::back() {
    if (!running_) {
        return;
    }
    const std::vector<FiredMacrostep> macrosteps = firedMacrosteps_;  // snapshot: N macrosteps so far
    if (macrosteps.empty()) {
        return;
    }
    auto doc = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return;
    }
    if (!activateInitialState(QStringLiteral("init"))) {
        return;  // defensive: Run could not have started without an initial state either
    }
    for (std::size_t i = 0; i + 1 < macrosteps.size(); ++i) {
        std::vector<const Transition*> microsteps;
        bool ok = true;
        for (quint64 transitionId : macrosteps[i].transitionIds) {
            const Transition* transition = doc->findTransition(transitionId);
            if (!transition) {
                ok = false;
                break;  // defensive: should not happen (edits are rejected while Simulate)
            }
            microsteps.push_back(transition);
        }
        if (!ok) {
            break;
        }
        // Replays with the payload the macrostep originally fired with.
        fireMacrostep(*doc, microsteps, macrosteps[i].payload);  // guards deliberately NOT checked
    }
    internalQueue_.clear();
}

void SimulationAgent::completeInvocation(const QString& invokeId, bool ok, const QVariant& payload) {
    if (!running_ || invokeId.isEmpty()) {
        return;
    }
    // Liveness is re-checked here, at completion time. A dead invocation (state already
    // exited, or id never armed) is a silent no-op: no trace line, no dispatch.
    const bool live = std::any_of(activeInvocations_.begin(), activeInvocations_.end(),
                                   [&](const ActiveInvocation& invocation) { return invocation.invokeId == invokeId; });
    if (!live) {
        return;
    }
    const QString eventName =
        (ok ? QStringLiteral("done.invoke.") : QStringLiteral("error.platform.")) + invokeId;
    appendTrace(QStringLiteral("invoke: ") + invokeId + (ok ? QStringLiteral(" done") : QStringLiteral(" error")));
    dispatchEvent(eventName, InvocationPayload{.name = ok ? QStringLiteral("output") : QStringLiteral("error"),
                                                .value = payload});
}

void SimulationAgent::toggleBreakpoint(quint64 stateId) {
    if (stateId == 0) {
        return;
    }
    const bool wasSet = breakpoints_.contains(stateId);
    if (wasSet) {
        breakpoints_.remove(stateId);
    } else {
        breakpoints_.insert(stateId);
    }
    context().send(events::BreakpointToggled{.stateId = stateId, .enabled = !wasSet});
}

void SimulationAgent::addBreakpoint(quint64 stateId) {
    if (stateId == 0 || breakpoints_.contains(stateId)) {
        return;
    }
    breakpoints_.insert(stateId);
    context().send(events::BreakpointToggled{.stateId = stateId, .enabled = true});
}

void SimulationAgent::removeBreakpoint(quint64 stateId) {
    if (!breakpoints_.contains(stateId)) {
        return;
    }
    breakpoints_.remove(stateId);
    context().send(events::BreakpointToggled{.stateId = stateId, .enabled = false});
}

bool SimulationAgent::hasBreakpoint(quint64 stateId) const {
    return breakpoints_.contains(stateId);
}

bool SimulationAgent::activateInitialState(const QString& verb) {
    auto doc = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return false;
    }
    // initialStateId 0, or an id no state carries (a hand-edited file), means nothing to activate.
    const State* initial = doc->findState(doc->machine().initialStateId);
    if (!initial) {
        return false;
    }
    const quint64 fromId = activeStateId();  // flat-compat: whatever was active before (0 if nothing was)
    trace_.clear();
    firedMacrosteps_.clear();
    armedTimers_.clear();
    activeInvocations_.clear();
    historyRecords_.clear();  // a fresh activation has no history to redirect through
    configuration_.clear();
    internalQueue_.clear();
    lastGuardEvaluations_.clear();

    const std::vector<quint64> entrySet = entrySequenceFor(*doc, initial->id, 0);  // full descent from root
    configuration_.assign(entrySet.begin(), entrySet.end());
    std::sort(configuration_.begin(), configuration_.end(),
              [&](quint64 a, quint64 b) { return docOrderIndex(*doc, a) < docOrderIndex(*doc, b); });

    appendTrace(verb + QStringLiteral(" → ") + initial->name);

    // Seeding here is why back() needs no context rollback: its replay re-runs every assign.
    // After the verb line so a malformed initialValue's note follows the activation line.
    seedContext(*doc);

    for (quint64 id : entrySet) {
        armStateTimers(*doc, id);
        armStateInvocation(*doc, id);
    }
    armRootDelayedTransitions(*doc);  // the machine's own entry, once per Run/Reset/replay base

    context().send(events::ActiveStateChanged{.fromId = fromId, .toId = initial->id, .viaTransitionId = 0});
    context().send(events::ConfigurationChanged{.activeIds = configurationSnapshot()});
    return true;
}

void SimulationAgent::fireMicrostep(const MachineDocAgent& doc, const Transition& transition,
                                     const InvocationPayload& payload) {
    // Machine self-target: resolved at fire time to the current initial state, so with
    // from == 0 the whole configuration exits and the initial descent re-enters. An unset
    // initial resolves to 0 and takes the targetless early-out (action only).
    quint64 resolvedTo = transition.to;
    bool selfRestart = false;
    if (transition.from == 0 && transition.machineSelf) {
        resolvedTo = doc.machine().initialStateId;
        selfRestart = resolvedTo != 0;
    }

    if (transition.isMultiTarget()) {
        const QList<quint64>& targets = transition.targets;
        QStringList destNames;
        for (quint64 tid : targets) {
            const State* s = doc.findState(tid);
            destNames.push_back(s ? s->name : QString::number(tid));
        }
        const QString destName = QStringLiteral("[%1]").arg(destNames.join(QStringLiteral(", ")));
        appendTrace(transitionHeadline(transition, destName));

        const quint64 lcca = lccaOf(doc, transition.from, targets);
        const std::vector<quint64> exitSet = exitSequenceFor(doc, lcca);
        const std::vector<quint64> entrySet = entrySequenceFor(doc, targets, lcca);

        for (quint64 exitedId : exitSet) {
            if (const State* exited = doc.findState(exitedId)) {
                for (const QString& exitAction : exited->exitActions) {
                    runAction(doc, exitAction, payload);
                }
            }
        }
        if (!transition.action.isEmpty()) {
            runAction(doc, transition.action, payload);
        }
        for (quint64 enteredId : entrySet) {
            if (const State* entered = doc.findState(enteredId)) {
                for (const QString& entryAction : entered->entryActions) {
                    runAction(doc, entryAction, payload);
                }
            }
        }

        quint64 fromId = transition.from;
        if (transition.from == 0) {
            fromId = firstAtomicWithin(doc, exitSet);
        }

        for (quint64 exitedId : exitSet) {
            disarmStateTimers(exitedId);
            disarmStateInvocation(exitedId);
            const quint64 exitedParent = parentOf(doc, exitedId);
            if (exitedParent != 0) {
                historyRecords_[exitedParent] = exitedId;
            }
            configuration_.erase(std::remove(configuration_.begin(), configuration_.end(), exitedId),
                                 configuration_.end());
        }
        for (quint64 enteredId : entrySet) {
            configuration_.push_back(enteredId);
        }
        std::sort(configuration_.begin(), configuration_.end(),
                  [&](quint64 a, quint64 b) { return docOrderIndex(doc, a) < docOrderIndex(doc, b); });

        context().send(
            events::ActiveStateChanged{.fromId = fromId, .toId = targets.first(), .viaTransitionId = transition.id});
        context().send(events::TransitionFired{.transitionId = transition.id, .event = transition.event});

        for (quint64 enteredId : entrySet) {
            if (breakpoints_.contains(enteredId)) {
                hitBreakpointStateId_ = enteredId;
            }
            armStateTimers(doc, enteredId);
            armStateInvocation(doc, enteredId);
        }
        return;
    }

    // Targetless (to == 0): only the action fires; no exit/entry, no ActiveStateChanged,
    // armed countdowns untouched. It still counts as a firing (TransitionFired, macrostep history).
    if (resolvedTo == 0) {
        appendTrace(transitionHeadline(transition, QString()));
        if (!transition.action.isEmpty()) {
            runAction(doc, transition.action, payload);  // an assign on a targetless transition still writes context
        }
        context().send(events::TransitionFired{.transitionId = transition.id, .event = transition.event});
        return;
    }

    const State* dest = doc.findState(resolvedTo);
    // A self restart's headline names "#machine", never the resolved initial leaf.
    const QString destName = selfRestart ? QStringLiteral("#machine") : (dest ? dest->name : QString());

    // Internal self-transition (from == resolvedTo, !reenter): behaves like a targetless
    // one, only the action fires.
    if (transition.from != 0 && transition.from == resolvedTo && !transition.reenter) {
        appendTrace(transitionHeadline(transition, destName));
        if (!transition.action.isEmpty()) {
            runAction(doc, transition.action, payload);
        }
        context().send(events::TransitionFired{.transitionId = transition.id, .event = transition.event});
        return;
    }

    // History redirect: a History target H never enters configuration_. The entry domain
    // becomes H's parent compound C (the validator guarantees a non-zero parentId), with C's
    // descent overridden by historyRecords_. The headline still names H; an extra "history:"
    // trace line names the redirect.
    quint64 effectiveTargetId = resolvedTo;
    HistoryDescend historyPolicy = HistoryDescend::None;
    const bool isHistoryRedirect = dest && dest->kind == StateKind::History;
    if (isHistoryRedirect) {
        effectiveTargetId = dest->parentId;
        historyPolicy = dest->historyDeep ? HistoryDescend::AllLevels : HistoryDescend::OneLevel;
    }

    appendTrace(transitionHeadline(transition, destName));

    if (isHistoryRedirect) {
        const State* parent = doc.findState(effectiveTargetId);
        // Same first-hop lookup descendInto() does (record, else initialChildId); shallow and
        // deep agree here.
        const quint64 resolvedChildId = historyRecords_.value(effectiveTargetId, parent ? parent->initialChildId : 0);
        const State* resolvedChild = doc.findState(resolvedChildId);
        appendTrace(QStringLiteral("history: ") + (parent ? parent->name : QString()) + QStringLiteral(" -> ") +
                    (resolvedChild ? resolvedChild->name : QString()));
    }

    const quint64 lcca = lccaOf(doc, transition.from, effectiveTargetId);
    const std::vector<quint64> exitSet = exitSequenceFor(doc, lcca);  // deepest-first
    const std::vector<quint64> entrySet =
        entrySequenceFor(doc, effectiveTargetId, lcca, historyPolicy);  // parent-first

    // Firing order: exit actions child-first, transition action, entry actions parent-first.
    // Every position goes through runAction(), so an assign is visible to the destination's
    // entry actions but not to the guard that selected this transition.
    for (quint64 exitedId : exitSet) {
        if (const State* exited = doc.findState(exitedId)) {
            for (const QString& exitAction : exited->exitActions) {
                runAction(doc, exitAction, payload);
            }
        }
    }
    if (!transition.action.isEmpty()) {
        runAction(doc, transition.action, payload);
    }
    for (quint64 enteredId : entrySet) {
        if (const State* entered = doc.findState(enteredId)) {
            for (const QString& entryAction : entered->entryActions) {
                runAction(doc, entryAction, payload);
            }
        }
    }

    // ActiveStateChanged::fromId is transition.from as declared; a root firing uses the first
    // atomic state in the exit set, computed BEFORE that set leaves configuration_.
    quint64 fromId = transition.from;
    if (transition.from == 0) {
        fromId = firstAtomicWithin(doc, exitSet);
    }

    // Exit set out, entry set in (timers disarmed/armed per state), re-sorted into document
    // order. Each exited state is recorded in its parent's historyRecords_ slot, latest wins.
    for (quint64 exitedId : exitSet) {
        disarmStateTimers(exitedId);
        disarmStateInvocation(exitedId);
        const quint64 exitedParent = parentOf(doc, exitedId);
        if (exitedParent != 0) {
            historyRecords_[exitedParent] = exitedId;
        }
        configuration_.erase(std::remove(configuration_.begin(), configuration_.end(), exitedId),
                              configuration_.end());
    }
    for (quint64 enteredId : entrySet) {
        configuration_.push_back(enteredId);
    }
    std::sort(configuration_.begin(), configuration_.end(),
              [&](quint64 a, quint64 b) { return docOrderIndex(doc, a) < docOrderIndex(doc, b); });

    context().send(
        events::ActiveStateChanged{.fromId = fromId, .toId = resolvedTo, .viaTransitionId = transition.id});
    context().send(events::TransitionFired{.transitionId = transition.id, .event = transition.event});

    for (quint64 enteredId : entrySet) {
        if (breakpoints_.contains(enteredId)) {
            hitBreakpointStateId_ = enteredId;
        }
        armStateTimers(doc, enteredId);
        armStateInvocation(doc, enteredId);
    }
}

void SimulationAgent::fireMacrostep(const MachineDocAgent& doc, const std::vector<const Transition*>& transitions,
                                     const InvocationPayload& payload) {
    std::vector<quint64> firedIds;
    firedIds.reserve(transitions.size());
    for (const Transition* transition : transitions) {
        fireMicrostep(doc, *transition, payload);
        firedIds.push_back(transition->id);
    }
    // The payload is recorded too, so back()'s replay reproduces an invocation-driven assign.
    firedMacrosteps_.push_back(FiredMacrostep{.transitionIds = std::move(firedIds), .payload = payload});
    context().send(events::ConfigurationChanged{.activeIds = configurationSnapshot()});

    if (hitBreakpointStateId_ != 0) {
        const quint64 bpId = hitBreakpointStateId_;
        hitBreakpointStateId_ = 0;
        running_ = false;
        const State* s = doc.findState(bpId);
        const QString sName = s ? s->name : QString::number(bpId);
        appendTrace(QStringLiteral("breakpoint: ") + sName + QStringLiteral(" hit, paused"));
        context().send(events::SimulationPaused{});
        context().send(events::BreakpointHit{.stateId = bpId});
    }
}


const Transition* SimulationAgent::firstMatchingHandler(const MachineDocAgent& doc, quint64 stateId,
                                                          const QString& name, const InvocationPayload& payload) {
    // Collect all matching transitions, then sort by specificity (Exact > Prefix wildcard >
    // Universal wildcard), document order for ties.
    struct Candidate {
        const Transition* transition;
        int specificity;
        int originalOrder;
    };

    std::vector<Candidate> matches;
    int order = 0;
    for (const Transition& t : doc.machine().transitions) {
        if (t.from != stateId) {
            continue;
        }
        // Blank-event transitions are delay-only or always; SendEvent never matches them.
        if (t.event.isEmpty()) {
            continue;
        }
        if (eventMatches(t.event, name)) {
            matches.push_back(Candidate{
                .transition = &t,
                .specificity = eventDescriptorSpecificity(t.event),
                .originalOrder = order++,
            });
        }
    }

    if (matches.empty()) {
        return nullptr;
    }

    std::stable_sort(matches.begin(), matches.end(), [](const Candidate& a, const Candidate& b) {
        if (a.specificity != b.specificity) {
            return a.specificity > b.specificity;
        }
        return a.originalOrder < b.originalOrder;
    });

    for (const Candidate& c : matches) {
        if (guardPasses(c.transition->guard, c.transition->id, payload)) {
            return c.transition;
        }
    }

    return nullptr;
}

const Transition* SimulationAgent::firstMatchingAlwaysHandler(const MachineDocAgent& doc, quint64 stateId) {
    for (const Transition& t : doc.machine().transitions) {
        if (t.from != stateId) {
            continue;
        }
        if (!t.isAlways()) {
            continue;
        }
        if (!guardPasses(t.guard, t.id)) {
            continue;
        }
        return &t;
    }
    return nullptr;
}

std::vector<const Transition*> SimulationAgent::selectAlwaysTransitions(const MachineDocAgent& doc) {
    // Phase 1: one candidate per atomic active (walk self then ancestors)
    std::vector<const Transition*> candidates;
    for (quint64 atomicId : atomicActives(doc)) {
        quint64 walk = atomicId;
        const Transition* found = nullptr;
        while (walk != 0) {
            found = firstMatchingAlwaysHandler(doc, walk);
            if (found) {
                break;
            }
            walk = parentOf(doc, walk);
        }
        if (found) {
            candidates.push_back(found);
        }
    }

    // Phase 2: drop any candidate whose source lies in an earlier surviving candidate's exit set
    std::vector<const Transition*> survivors;
    std::vector<quint64> exitedSoFar;
    for (const Transition* candidate : candidates) {
        if (std::find(exitedSoFar.begin(), exitedSoFar.end(), candidate->from) != exitedSoFar.end()) {
            continue;
        }
        survivors.push_back(candidate);
        if (candidate->to != 0) {
            const quint64 lcca = lccaOf(doc, candidate->from, candidate->to);
            const std::vector<quint64> exitSet = exitSequenceFor(doc, lcca);
            exitedSoFar.insert(exitedSoFar.end(), exitSet.begin(), exitSet.end());
        }
    }

    if (!survivors.empty()) {
        return survivors;
    }

    // Root fallback: check machine-level (from == 0) always transitions
    for (const Transition& t : doc.machine().transitions) {
        if (t.from != 0 || !t.isAlways()) {
            continue;
        }
        if (!guardPasses(t.guard, t.id)) {
            continue;
        }
        return {&t};
    }

    return {};
}

void SimulationAgent::processMicrosteps(const MachineDocAgent& doc) {
    constexpr int kMaxMicrosteps = 100;
    int stepCount = 0;

    while (running_) {
        // Eventless (always) transitions run first
        const std::vector<const Transition*> alwaysTransitions = selectAlwaysTransitions(doc);
        if (!alwaysTransitions.empty()) {
            if (++stepCount > kMaxMicrosteps) {
                appendTrace(QStringLiteral("warning: always transition loop exceeded quota (100 steps)"));
                break;
            }
            fireMacrostep(doc, alwaysTransitions);
            continue;
        }

        // Otherwise drain the internal event queue
        if (!internalQueue_.empty()) {
            const QString internalEvent = internalQueue_.front();
            internalQueue_.pop_front();

            const std::vector<const Transition*> toFire = selectTransitionsForEvent(doc, internalEvent);
            if (!toFire.empty()) {
                if (++stepCount > kMaxMicrosteps) {
                    appendTrace(QStringLiteral("warning: microstep loop exceeded quota (100 steps)"));
                    break;
                }
                fireMacrostep(doc, toFire);
            }
            continue;
        }

        // Quiescence reached
        break;
    }
}

void SimulationAgent::processAlwaysTransitions(const MachineDocAgent& doc) {
    processMicrosteps(doc);
}

quint64 SimulationAgent::activeStateId() const {
    if (configuration_.empty()) {
        return 0;
    }
    auto doc = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!doc) {
        return configuration_.size() == 1 ? configuration_.front() : 0;
    }
    const std::vector<quint64> atomics = atomicActives(*doc);
    return atomics.size() == 1 ? atomics.front() : 0;
}

std::vector<quint64> SimulationAgent::firedTransitionIds() const {
    std::vector<quint64> flat;
    for (const FiredMacrostep& macrostep : firedMacrosteps_) {
        flat.insert(flat.end(), macrostep.transitionIds.begin(), macrostep.transitionIds.end());
    }
    return flat;
}

}  // namespace app
