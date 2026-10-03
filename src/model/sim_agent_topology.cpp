#include "model/sim_agent.h"

#include <algorithm>
#include <vector>

#include <QList>
#include <QSet>

#include "model/machine_doc.h"

namespace app {

int SimulationAgent::docOrderIndex(const MachineDocAgent& doc, quint64 stateId) const {
    const QVector<State>& states = doc.machine().states;
    for (int i = 0; i < states.size(); ++i) {
        if (states[i].id == stateId) {
            return i;
        }
    }
    return -1;
}

bool SimulationAgent::isAncestorOf(const MachineDocAgent& doc, quint64 ancestorId, quint64 descendantId) const {
    if (ancestorId == 0) {
        return true;  // root -- ancestor of every state, including top-level ones
    }
    quint64 cur = descendantId;
    while (cur != 0) {
        const State* s = doc.findState(cur);
        const quint64 parent = s ? s->parentId : 0;
        if (parent == ancestorId) {
            return true;
        }
        cur = parent;
    }
    return false;
}

quint64 SimulationAgent::parentOf(const MachineDocAgent& doc, quint64 stateId) const {
    if (stateId == 0) {
        return 0;  // root has no parent -- stays the sentinel
    }
    const State* s = doc.findState(stateId);
    return s ? s->parentId : 0;
}

quint64 SimulationAgent::lccaOf(const MachineDocAgent& doc, quint64 sourceId, quint64 targetId) const {
    quint64 anc = parentOf(doc, sourceId);
    while (!isAncestorOf(doc, anc, targetId)) {
        anc = parentOf(doc, anc);
    }
    return anc;
}

quint64 SimulationAgent::lccaOf(const MachineDocAgent& doc, quint64 sourceId, const QList<quint64>& targetIds) const {
    if (targetIds.isEmpty()) {
        return parentOf(doc, sourceId);
    }
    quint64 anc = lccaOf(doc, sourceId, targetIds.first());
    for (int i = 1; i < targetIds.size(); ++i) {
        while (!isAncestorOf(doc, anc, targetIds.at(i))) {
            anc = parentOf(doc, anc);
        }
    }
    return anc;
}

std::vector<quint64> SimulationAgent::childrenOf(const MachineDocAgent& doc, quint64 parentId) const {
    std::vector<quint64> children;
    for (const State& state : doc.machine().states) {  // doc.machine().states is already document order
        if (state.parentId == parentId) {
            children.push_back(state.id);
        }
    }
    return children;
}

std::vector<quint64> SimulationAgent::exitSequenceFor(const MachineDocAgent& doc, quint64 lcca) const {
    std::vector<quint64> exited;
    for (quint64 id : configuration_) {  // configuration_ is already document order
        if (isAncestorOf(doc, lcca, id)) {
            exited.push_back(id);
        }
    }
    // Deepest first by real depth, reverse document order among equals; reversed
    // document order is not depth order after a reparent.
    std::stable_sort(exited.begin(), exited.end(), [&](quint64 a, quint64 b) {
        const int depthA = depthOf(doc, a);
        const int depthB = depthOf(doc, b);
        if (depthA != depthB) {
            return depthA > depthB;
        }
        return docOrderIndex(doc, a) > docOrderIndex(doc, b);
    });
    return exited;
}

int SimulationAgent::depthOf(const MachineDocAgent& doc, quint64 stateId) const {
    int depth = 0;
    quint64 cur = parentOf(doc, stateId);
    while (cur != 0) {
        ++depth;
        cur = parentOf(doc, cur);
    }
    return depth;
}

void SimulationAgent::descendInto(const MachineDocAgent& doc, quint64 stateId, std::vector<quint64>& result,
                                   HistoryDescend policy) const {
    const State* s = doc.findState(stateId);
    if (!s) {
        return;
    }
    if (s->kind == StateKind::Parallel) {
        // A Parallel state always enters every region. AllLevels carries into each
        // region; OneLevel was already spent reaching this level.
        const HistoryDescend childPolicy =
            policy == HistoryDescend::AllLevels ? HistoryDescend::AllLevels : HistoryDescend::None;
        for (quint64 child : childrenOf(doc, stateId)) {
            result.push_back(child);
            descendInto(doc, child, result, childPolicy);
        }
        return;
    }
    // policy != None prefers the recorded history child over initialChildId, falling
    // back to it when nothing is recorded yet.
    quint64 nextChild = s->initialChildId;
    if (policy != HistoryDescend::None) {
        const auto it = historyRecords_.constFind(stateId);
        if (it != historyRecords_.constEnd()) {
            nextChild = it.value();
        }
    }
    if (nextChild != 0) {
        result.push_back(nextChild);
        // OneLevel applies here only; AllLevels keeps applying at every level below.
        descendInto(doc, nextChild, result,
                    policy == HistoryDescend::AllLevels ? HistoryDescend::AllLevels : HistoryDescend::None);
    }
    // else: atomic, or a compound with no initial child/record: nothing further to enter.
}

std::vector<quint64> SimulationAgent::entrySequenceFor(const MachineDocAgent& doc, quint64 targetId, quint64 lcca,
                                                         HistoryDescend historyPolicy) const {
    // The explicit path: targetId's ancestors below lcca, top-down.
    std::vector<quint64> chain;
    quint64 cur = targetId;
    while (cur != lcca) {
        chain.push_back(cur);
        const State* s = doc.findState(cur);
        cur = s ? s->parentId : 0;
        if (cur == 0 && lcca != 0) {
            break;  // defensive: lcca should always sit on targetId's ancestor chain
        }
    }
    std::reverse(chain.begin(), chain.end());  // chain[0] = direct child of lcca ... chain.back() = targetId

    std::vector<quint64> result;
    quint64 parent = lcca;
    for (quint64 id : chain) {
        const State* parentState = parent != 0 ? doc.findState(parent) : nullptr;
        if (parentState && parentState->kind == StateKind::Parallel) {
            // A Parallel ancestor on the path still enters every region: the on-path
            // child continues down the chain, every other child gets its default descent.
            for (quint64 child : childrenOf(doc, parent)) {
                result.push_back(child);
                if (child != id) {
                    descendInto(doc, child, result);
                }
            }
        } else {
            result.push_back(id);
        }
        parent = id;
    }
    // Whatever lies below targetId itself, honoring `historyPolicy`.
    descendInto(doc, targetId, result, historyPolicy);
    return result;
}

std::vector<quint64> SimulationAgent::entrySequenceFor(const MachineDocAgent& doc, const QList<quint64>& targetIds,
                                                       quint64 lcca) const {
    if (targetIds.size() == 1) {
        return entrySequenceFor(doc, targetIds.first(), lcca, HistoryDescend::None);
    }

    QSet<quint64> toEnter;
    // For each target: its ancestor chain below lcca, the target itself, and its default descent.
    for (quint64 tid : targetIds) {
        quint64 cur = tid;
        while (cur != lcca && cur != 0) {
            toEnter.insert(cur);
            const State* s = doc.findState(cur);
            cur = s ? s->parentId : 0;
        }
        std::vector<quint64> descent;
        descendInto(doc, tid, descent, HistoryDescend::None);
        for (quint64 d : descent) {
            toEnter.insert(d);
        }
    }

    // Every region of a Parallel state in toEnter must be entered: a region already
    // holding a member is covered by the explicit target, the rest use default descent.
    bool expanded = true;
    while (expanded) {
        expanded = false;
        const QSet<quint64> snapshot = toEnter;
        for (quint64 id : snapshot) {
            const State* s = doc.findState(id);
            if (s && s->kind == StateKind::Parallel) {
                for (quint64 child : childrenOf(doc, id)) {
                    const bool covered = std::any_of(toEnter.begin(), toEnter.end(), [&](quint64 member) {
                        return member == child || isAncestorOf(doc, child, member);
                    });
                    if (!covered) {
                        toEnter.insert(child);
                        std::vector<quint64> descent;
                        descendInto(doc, child, descent, HistoryDescend::None);
                        for (quint64 d : descent) {
                            toEnter.insert(d);
                        }
                        expanded = true;
                    }
                }
            }
        }
    }

    // Parent-first (shallowest first), then document order.
    std::vector<quint64> result(toEnter.begin(), toEnter.end());
    std::stable_sort(result.begin(), result.end(), [&](quint64 a, quint64 b) {
        const int depthA = depthOf(doc, a);
        const int depthB = depthOf(doc, b);
        if (depthA != depthB) {
            return depthA < depthB;
        }
        return docOrderIndex(doc, a) < docOrderIndex(doc, b);
    });
    return result;
}

std::vector<quint64> SimulationAgent::atomicActives(const MachineDocAgent& doc) const {
    std::vector<quint64> result;
    for (quint64 id : configuration_) {  // configuration_ is already document order
        const bool hasActiveChild = std::any_of(configuration_.begin(), configuration_.end(), [&](quint64 other) {
            const State* s = doc.findState(other);
            return s && s->parentId == id;
        });
        if (!hasActiveChild) {
            result.push_back(id);
        }
    }
    return result;
}

quint64 SimulationAgent::firstAtomicWithin(const MachineDocAgent& doc, const std::vector<quint64>& ids) const {
    for (quint64 id : configuration_) {  // configuration_'s own order -- document order
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            continue;
        }
        const bool hasChildInSet = std::any_of(ids.begin(), ids.end(), [&](quint64 other) {
            const State* s = doc.findState(other);
            return s && s->parentId == id;
        });
        if (!hasChildInSet) {
            return id;
        }
    }
    return 0;
}

}  // namespace app
