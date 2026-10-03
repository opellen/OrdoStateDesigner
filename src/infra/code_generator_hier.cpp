#include "infra/code_generator_internal.h"

namespace app {

namespace {

QString hierHistoryPolicyLiteral(HierHistoryRedirect policy) {
    switch (policy) {
        case HierHistoryRedirect::None:
            return QStringLiteral("HistoryPolicy::None");
        case HierHistoryRedirect::OneLevel:
            return QStringLiteral("HistoryPolicy::OneLevel");
        case HierHistoryRedirect::AllLevels:
            return QStringLiteral("HistoryPolicy::AllLevels");
    }
    return QStringLiteral("HistoryPolicy::None");
}

// The private topology and per-transition tables. Document order == array
// index == the State enum value, so an index and static_cast<State> of it name
// the same state. kStateCount is emitted earlier: signatures must see it.
void appendHierTables(QString& text, const GenModel& model, const Machine& machine) {
    text += QStringLiteral(
        "    // History-redirect descent policy -- see descendInto()'s\n"
        "    // own comment below for exactly how far OneLevel/AllLevels each\n"
        "    // propagate.\n"
        "    enum class HistoryPolicy { None, OneLevel, AllLevels };\n\n");
    text += QStringLiteral("    static constexpr int kTransitionCount = %1;\n\n").arg(machine.transitions.size());

    QStringList parentEntries, depthEntries, initialChildEntries, parallelEntries;
    for (const HierState& hs : model.hierStates) {
        parentEntries.push_back(QString::number(hs.parentIndex));
        depthEntries.push_back(QString::number(hs.depth));
        initialChildEntries.push_back(QString::number(hs.initialChildIndex));
        parallelEntries.push_back(hs.isParallel ? QStringLiteral("true") : QStringLiteral("false"));
    }
    text += QStringLiteral("    static constexpr int kParentIndex[kStateCount] = {%1};  // -1 = machine root\n")
                .arg(parentEntries.join(QStringLiteral(", ")));
    text +=
        QStringLiteral("    static constexpr int kDepth[kStateCount] = {%1};  // parentId-chain length\n")
            .arg(depthEntries.join(QStringLiteral(", ")));
    text += QStringLiteral("    static constexpr int kInitialChildIndex[kStateCount] = {%1};  // -1 = none\n")
                .arg(initialChildEntries.join(QStringLiteral(", ")));
    text += QStringLiteral("    static constexpr bool kIsParallel[kStateCount] = {%1};\n\n")
                .arg(parallelEntries.join(QStringLiteral(", ")));

    text += QStringLiteral(
        "    // The machine's initial state's document-order index -- start() below\n"
        "    // descends from the machine root through this state.\n");
    text += QStringLiteral("    static constexpr int kInitialStateIndex = %1;\n\n").arg(model.initialStateIndex);

    text += QStringLiteral(
        "    // Per-transition domain, fully resolved at GENERATION time (topology\n"
        "    // is static, so this needs no runtime counterpart): source/declared-\n"
        "    // target index (-1 = root/targetless; the declared target is what\n"
        "    // setOnStateChanged() reports as `next` -- \"never the resolved\n"
        "    // leaf\"), the ENTRY DOMAIN target (History-redirected to the parent\n"
        "    // compound, meaningless when the declared target is -1), the LCCA\n"
        "    // index (-1 = root domain), and the History policy the redirect\n"
        "    // carries (None for an ordinary, non-History-redirected target).\n");
    QStringList sourceEntries, declaredTargetEntries, targetEntries, lccaEntries, policyEntries;
    for (const HierTransition& ht : model.hierTransitions) {
        sourceEntries.push_back(QString::number(ht.sourceIndex));
        declaredTargetEntries.push_back(QString::number(ht.declaredTargetIndex));
        targetEntries.push_back(QString::number(ht.targetIndex));
        lccaEntries.push_back(QString::number(ht.lccaIndex));
        policyEntries.push_back(hierHistoryPolicyLiteral(ht.historyPolicy));
    }
    text += QStringLiteral("    static constexpr int kTransSource[kTransitionCount] = {%1};\n")
                .arg(sourceEntries.join(QStringLiteral(", ")));
    text += QStringLiteral("    static constexpr int kTransDeclaredTarget[kTransitionCount] = {%1};\n")
                .arg(declaredTargetEntries.join(QStringLiteral(", ")));
    text += QStringLiteral("    static constexpr int kTransTarget[kTransitionCount] = {%1};\n")
                .arg(targetEntries.join(QStringLiteral(", ")));
    text += QStringLiteral("    static constexpr int kTransLcca[kTransitionCount] = {%1};\n")
                .arg(lccaEntries.join(QStringLiteral(", ")));
    text += QStringLiteral("    static constexpr HistoryPolicy kTransHistoryPolicy[kTransitionCount] = {%1};\n\n")
                .arg(policyEntries.join(QStringLiteral(", ")));
}

// Initial entry descent through kInitialStateIndex, run once from the
// constructor, so the core is in its initial configuration as soon as it exists.
void appendHierStart(QString& text, const GenModel& model) {
    text += QStringLiteral(
        "    void start() {\n"
        "        if (kInitialStateIndex < 0) {\n"
        "            return;  // no initial state set -- nothing to activate\n"
        "        }\n"
        "        int entryBuf[kStateCount];\n"
        "        int entryCount = 0;\n"
        "        entrySequence(kInitialStateIndex, -1, HistoryPolicy::None, entryBuf, entryCount);\n"
        "        for (int i = 0; i < entryCount; ++i) {\n"
        "            active_[entryBuf[i]] = true;\n"
        "        }\n");
    if (model.hasScheduler) {
        text += QStringLiteral(
            "        for (int i = 0; i < entryCount; ++i) {\n"
            "            armState(entryBuf[i]);\n"
            "        }\n");
        if (model.delayedBySource.contains(0)) {
            text += QStringLiteral("        armRoot();\n");
        }
    }
    if (model.hasInvoke) {
        text += QStringLiteral(
            "        for (int i = 0; i < entryCount; ++i) {\n"
            "            armStateInvocation(entryBuf[i]);\n"
            "        }\n");
    }
    if (model.hasAlways || model.hasRaise) {
        text += QStringLiteral("        checkAlwaysTransitions();\n");
    }
    text += QStringLiteral("    }\n\n");
}

// The generic walker engine: identical text for every hierarchical machine,
// reading the static tables against active_/historyRecord_. Mirrors the
// simulator's walkers (sim_agent.cpp) rule for rule. No heap: every buffer is
// a fixed `int[kStateCount]` local.
void appendHierWalkerEngine(QString& text, const GenModel& model) {
    text += QStringLiteral(
        "    // Direct children of `parentIndex` (-1 = the machine root),\n"
        "    // document order -- a plain scan of kParentIndex is already\n"
        "    // document order (it is indexed BY document-order position), so\n"
        "    // no separate span table is needed.\n"
        "    void childrenOf(int parentIndex, int* out, int& count) const {\n"
        "        count = 0;\n"
        "        for (int i = 0; i < kStateCount; ++i) {\n"
        "            if (kParentIndex[i] == parentIndex) {\n"
        "                out[count++] = i;\n"
        "            }\n"
        "        }\n"
        "    }\n\n"
        "    // True if `ancestorIndex` is a PROPER ancestor of `stateIndex` --\n"
        "    // -1 (root) counts as an ancestor of every state.\n"
        "    bool isDescendantOf(int stateIndex, int ancestorIndex) const {\n"
        "        if (ancestorIndex == -1) {\n"
        "            return true;\n"
        "        }\n"
        "        int cur = kParentIndex[stateIndex];\n"
        "        while (cur != -1) {\n"
        "            if (cur == ancestorIndex) {\n"
        "                return true;\n"
        "            }\n"
        "            cur = kParentIndex[cur];\n"
        "        }\n"
        "        return false;\n"
        "    }\n\n"
        "    // Exit set: every currently ACTIVE proper descendant of `lccaIndex`,\n"
        "    // deepest-first by real depth, higher index first among equal\n"
        "    // depth (index == document order -- NOT a plain reversal; a state\n"
        "    // reparented under a later-created parent sits at a LOWER index\n"
        "    // than that parent).\n"
        "    void exitSequence(int lccaIndex, int* out, int& count) const {\n"
        "        count = 0;\n"
        "        for (int i = 0; i < kStateCount; ++i) {\n"
        "            if (active_[i] && isDescendantOf(i, lccaIndex)) {\n"
        "                out[count++] = i;\n"
        "            }\n"
        "        }\n"
        "        std::sort(out, out + count, [](int a, int b) {\n"
        "            if (kDepth[a] != kDepth[b]) {\n"
        "                return kDepth[a] > kDepth[b];\n"
        "            }\n"
        "            return a > b;\n"
        "        });\n"
        "    }\n\n"
        "    // Default entry descent below `stateIndex`: every child\n"
        "    // (recursively) if Parallel, else its initial child\n"
        "    // (recursively) -- unless `policy` overrides a Normal compound's\n"
        "    // own choice with historyRecord_'s recorded child instead; a\n"
        "    // Parallel state always enters every region regardless of\n"
        "    // `policy` (AllLevels still applies region-internally).\n"
        "    void descendInto(int stateIndex, int* out, int& count, HistoryPolicy policy) const {\n"
        "        if (kIsParallel[stateIndex]) {\n"
        "            int children[kStateCount];\n"
        "            int childCount = 0;\n"
        "            childrenOf(stateIndex, children, childCount);\n"
        "            const HistoryPolicy childPolicy =\n"
        "                policy == HistoryPolicy::AllLevels ? HistoryPolicy::AllLevels : HistoryPolicy::None;\n"
        "            for (int i = 0; i < childCount; ++i) {\n"
        "                out[count++] = children[i];\n"
        "                descendInto(children[i], out, count, childPolicy);\n"
        "            }\n"
        "            return;\n"
        "        }\n"
        "        int nextChild = kInitialChildIndex[stateIndex];\n"
        "        if (policy != HistoryPolicy::None && historyRecord_[stateIndex] != -1) {\n"
        "            nextChild = historyRecord_[stateIndex];\n"
        "        }\n"
        "        if (nextChild != -1) {\n"
        "            out[count++] = nextChild;\n"
        "            descendInto(nextChild, out, count,\n"
        "                         policy == HistoryPolicy::AllLevels ? HistoryPolicy::AllLevels : HistoryPolicy::None);\n"
        "        }\n"
        "    }\n\n"
        "    // Entry set: `targetIndex`'s not-yet-active ancestors down from\n"
        "    // `lccaIndex`, parent-first, then the default descent below\n"
        "    // `targetIndex` itself (honoring `historyPolicy`). A\n"
        "    // Parallel ancestor along the explicit path also enters its\n"
        "    // OTHER children, at the default None policy (off-path siblings\n"
        "    // are never part of a History redirect's own subtree).\n"
        "    void entrySequence(int targetIndex, int lccaIndex, HistoryPolicy historyPolicy, int* out,\n"
        "                        int& count) const {\n"
        "        int chain[kStateCount];\n"
        "        int chainCount = 0;\n"
        "        int cur = targetIndex;\n"
        "        while (cur != lccaIndex) {\n"
        "            if (cur == -1) {\n"
        "                break;  // defensive: lccaIndex should always sit on targetIndex's ancestor chain\n"
        "            }\n"
        "            chain[chainCount++] = cur;\n"
        "            cur = kParentIndex[cur];\n"
        "        }\n"
        "        for (int i = 0; i < chainCount / 2; ++i) {\n"
        "            const int tmp = chain[i];\n"
        "            chain[i] = chain[chainCount - 1 - i];\n"
        "            chain[chainCount - 1 - i] = tmp;\n"
        "        }\n"
        "\n"
        "        count = 0;\n"
        "        int parent = lccaIndex;\n"
        "        for (int i = 0; i < chainCount; ++i) {\n"
        "            const int id = chain[i];\n"
        "            if (parent != -1 && kIsParallel[parent]) {\n"
        "                int children[kStateCount];\n"
        "                int childCount = 0;\n"
        "                childrenOf(parent, children, childCount);\n"
        "                for (int c = 0; c < childCount; ++c) {\n"
        "                    out[count++] = children[c];\n"
        "                    if (children[c] != id) {\n"
        "                        descendInto(children[c], out, count, HistoryPolicy::None);\n"
        "                    }\n"
        "                }\n"
        "            } else {\n"
        "                out[count++] = id;\n"
        "            }\n"
        "            parent = id;\n"
        "        }\n"
        "        descendInto(targetIndex, out, count, historyPolicy);\n    }\n\n");
    if (model.hasMultiTarget) {
        text += QStringLiteral(
            "    void multiTargetEntrySequence(const int* targets, int targetCount, int lccaIndex, int* out,\n"
            "                                  int& count) const {\n"
            "        if (targetCount == 1) {\n"
            "            entrySequence(targets[0], lccaIndex, HistoryPolicy::None, out, count);\n"
            "            return;\n"
            "        }\n"
            "        bool inEnter[kStateCount] = {false};\n"
            "        for (int t = 0; t < targetCount; ++t) {\n"
            "            int cur = targets[t];\n"
            "            while (cur != lccaIndex && cur != -1) {\n"
            "                inEnter[cur] = true;\n"
            "                cur = kParentIndex[cur];\n"
            "            }\n"
            "            int descBuf[kStateCount];\n"
            "            int descCount = 0;\n"
            "            descendInto(targets[t], descBuf, descCount, HistoryPolicy::None);\n"
            "            for (int d = 0; d < descCount; ++d) {\n"
            "                inEnter[descBuf[d]] = true;\n"
            "            }\n"
            "        }\n"
            "        bool expanded = true;\n"
            "        while (expanded) {\n"
            "            expanded = false;\n"
            "            for (int s = 0; s < kStateCount; ++s) {\n"
            "                if (inEnter[s] && kIsParallel[s]) {\n"
            "                    int children[kStateCount];\n"
            "                    int childCount = 0;\n"
            "                    childrenOf(s, children, childCount);\n"
            "                    for (int c = 0; c < childCount; ++c) {\n"
            "                        const int child = children[c];\n"
            "                        bool regionCovered = false;\n"
            "                        for (int other = 0; other < kStateCount; ++other) {\n"
            "                            if (inEnter[other] && (other == child || isDescendantOf(other, child))) {\n"
            "                                regionCovered = true;\n"
            "                                break;\n"
            "                            }\n"
            "                        }\n"
            "                        if (!regionCovered) {\n"
            "                            inEnter[child] = true;\n"
            "                            int descBuf[kStateCount];\n"
            "                            int descCount = 0;\n"
            "                            descendInto(child, descBuf, descCount, HistoryPolicy::None);\n"
            "                            for (int d = 0; d < descCount; ++d) {\n"
            "                                inEnter[descBuf[d]] = true;\n"
            "                            }\n"
            "                            expanded = true;\n"
            "                        }\n"
            "                    }\n"
            "                }\n"
            "            }\n"
            "        }\n"
            "        count = 0;\n"
            "        for (int d = 0; d < kStateCount; ++d) {\n"
            "            for (int s = 0; s < kStateCount; ++s) {\n"
            "                if (inEnter[s] && kDepth[s] == d) {\n"
            "                    out[count++] = s;\n"
            "                }\n"
            "            }\n"
            "        }\n"
            "    }\n\n");
    }
    text += QStringLiteral(
        "    // The ATOMIC (no active child) members of active_,\n"
        "    // document order -- one candidate slot per atomic during event\n"
        "    // selection.\n"
        "    void atomicActiveIndices(int* out, int& count) const {\n"
        "        count = 0;\n"
        "        for (int i = 0; i < kStateCount; ++i) {\n"
        "            if (!active_[i]) {\n"
        "                continue;\n"
        "            }\n"
        "            bool hasActiveChild = false;\n"
        "            for (int j = 0; j < kStateCount; ++j) {\n"
        "                if (active_[j] && kParentIndex[j] == i) {\n"
        "                    hasActiveChild = true;\n"
        "                    break;\n"
        "                }\n"
        "            }\n"
        "            if (!hasActiveChild) {\n"
        "                out[count++] = i;\n"
        "            }\n"
        "        }\n"
        "    }\n\n"
        "    // The `previous` state a root-originated firing reports: the\n"
        "    // document-order-first atomic index within `ids` (evaluated against\n"
        "    // `ids` BEFORE it actually leaves active_).\n"
        "    int firstAtomicWithin(const int* ids, int idsCount) const {\n"
        "        for (int i = 0; i < kStateCount; ++i) {\n"
        "            if (!active_[i]) {\n"
        "                continue;\n"
        "            }\n"
        "            bool inIds = false;\n"
        "            for (int k = 0; k < idsCount; ++k) {\n"
        "                if (ids[k] == i) {\n"
        "                    inIds = true;\n"
        "                    break;\n"
        "                }\n"
        "            }\n"
        "            if (!inIds) {\n"
        "                continue;\n"
        "            }\n"
        "            bool hasChildInSet = false;\n"
        "            for (int k = 0; k < idsCount; ++k) {\n"
        "                if (kParentIndex[ids[k]] == i) {\n"
        "                    hasChildInSet = true;\n"
        "                    break;\n"
        "                }\n"
        "            }\n"
        "            if (!hasChildInSet) {\n"
        "                return i;\n"
        "            }\n"
        "        }\n"
        "        return -1;  // defensive -- ids is never empty when this is called\n"
        "    }\n\n"
        "    // Conflict resolution, shared by every event method: `candidateOf`\n"
        "    // holds one transition index (or -1) per ATOMIC state index (as\n"
        "    // returned by atomicActiveIndices() into `atomics`/`atomicCount`)\n"
        "    // -- drops a candidate whose source lies in an earlier survivor's\n"
        "    // exit set and writes the survivors (ONE macrostep) into `toFire`,\n"
        "    // else the root candidate (if any) as the fallback. Chooses only;\n"
        "    // returns how many indices it wrote.\n"
        "    int selectToFire(const int* candidateOf, const int* atomics, int atomicCount, int rootCandidate,\n"
        "                     int* toFire) const {\n"
        "        int survivorCount = 0;\n"
        "        int exitedSoFar[kStateCount];\n"
        "        int exitedCount = 0;\n"
        "        for (int a = 0; a < atomicCount; ++a) {\n"
        "            const int candidate = candidateOf[atomics[a]];\n"
        "            if (candidate == -1) {\n"
        "                continue;\n"
        "            }\n"
        "            const int source = kTransSource[candidate];\n"
        "            bool claimed = false;\n"
        "            for (int k = 0; k < exitedCount; ++k) {\n"
        "                if (exitedSoFar[k] == source) {\n"
        "                    claimed = true;\n"
        "                    break;\n"
        "                }\n"
        "            }\n"
        "            if (claimed) {\n"
        "                continue;\n"
        "            }\n"
        "            toFire[survivorCount++] = candidate;\n"
        "            if (kTransDeclaredTarget[candidate] != -1) {\n"
        "                int es[kStateCount];\n"
        "                int esCount = 0;\n"
        "                exitSequence(kTransLcca[candidate], es, esCount);\n"
        "                for (int k = 0; k < esCount; ++k) {\n"
        "                    exitedSoFar[exitedCount++] = es[k];\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        if (survivorCount > 0) {\n"
        "            return survivorCount;\n"
        "        }\n"
        "        if (rootCandidate != -1) {\n"
        "            toFire[0] = rootCandidate;\n"
        "            return 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n\n"
        "    // selectToFire()'s choice, fired through the generic index switch --\n"
        "    // the tail of every payload-free event method.\n"
        "    void selectAndFire(const int* candidateOf, const int* atomics, int atomicCount, int rootCandidate) {\n"
        "        int toFire[kStateCount];\n"
        "        const int fireCount = selectToFire(candidateOf, atomics, atomicCount, rootCandidate, toFire);\n"
        "        for (int i = 0; i < fireCount; ++i) {\n"
        "            fireTransition(toFire[i]);\n"
        "        }\n"
        "    }\n\n");
}

// One public method per event: each atomic active state walks up its ancestors
// for the first passing candidate, then selectAndFire() fires the set. A typed
// payload event takes `const T& event` and fires through its own fire_<event>()
// switch, since fireTransition(int) has no payload to pass.
void appendHierPublicEventMethods(QString& text, const GenModel& model) {
    for (const QString& rawEvent : model.eventOrder) {
        const QString methodName = model.eventMethodByRaw.value(rawEvent);
        const QString matchAtName = QStringLiteral("matchAt_%1").arg(methodName);
        const QString matchRootName = QStringLiteral("matchRoot_%1").arg(methodName);
        const QString payloadType = model.eventPayloadTypeByRaw.value(rawEvent);
        const bool hasPayload = !payloadType.isEmpty();
        text += QStringLiteral("    // \"%1\" -- one method per distinct event on the diagram.\n").arg(rawEvent);
        if (hasPayload) {
            text += QStringLiteral("    void %1(const %2& event) {\n").arg(methodName, cppPayloadTypeSpelling(payloadType));
        } else {
            text += QStringLiteral("    void %1() {\n").arg(methodName);
        }
        text += QStringLiteral(
            "        int atomics[kStateCount];\n"
            "        int atomicCount = 0;\n"
            "        atomicActiveIndices(atomics, atomicCount);\n"
            "        int candidateOf[kStateCount];\n"
            "        for (int i = 0; i < kStateCount; ++i) {\n"
            "            candidateOf[i] = -1;\n"
            "        }\n"
            "        for (int a = 0; a < atomicCount; ++a) {\n"
            "            int walk = atomics[a];\n"
            "            while (walk != -1) {\n");
        text += QStringLiteral("                const int found = %1(walk%2);\n")
                    .arg(matchAtName, hasPayload ? QStringLiteral(", event") : QString());
        text += QStringLiteral(
            "                if (found != -1) {\n"
            "                    candidateOf[atomics[a]] = found;\n"
            "                    break;\n"
            "                }\n"
            "                walk = kParentIndex[walk];\n"
            "            }\n"
            "        }\n");
        if (hasPayload) {
            text += QStringLiteral(
                        "        int toFire[kStateCount];\n"
                        "        const int fireCount = selectToFire(candidateOf, atomics, atomicCount, %1(event), toFire);\n"
                        "        for (int i = 0; i < fireCount; ++i) {\n"
                        "            fire_%2(toFire[i], event);\n"
                        "        }\n")
                        .arg(matchRootName, methodName);
        } else {
            text += QStringLiteral("        selectAndFire(candidateOf, atomics, atomicCount, %1());\n").arg(matchRootName);
        }
        if (model.hasAlways || model.hasRaise) {
            text += QStringLiteral("        checkAlwaysTransitions();\n");
        }
        text += QStringLiteral("    }\n\n");
    }
    if (model.hasAlways || model.hasRaise) {
        if (model.hasAlways && !model.hasRaise) {
            text += QStringLiteral(
                "    // Microstep quiescence loop for eventless (always) transitions (up to 100 steps).\n"
                "    void checkAlwaysTransitions() {\n"
                "        constexpr int kMaxAlwaysSteps = 100;\n"
                "        int stepCount = 0;\n"
                "        while (stepCount < kMaxAlwaysSteps) {\n"
                "            if (!stepAlwaysTransitions()) {\n"
                "                break;\n"
                "            }\n"
                "            ++stepCount;\n"
                "        }\n"
                "    }\n\n");
        } else {
            text += QStringLiteral(
                "    // Microstep quiescence loop for %1 (up to 100 steps).\n"
                "    void checkAlwaysTransitions() {\n"
                "        if (processingMicrosteps_) {\n"
                "            return;\n"
                "        }\n"
                "        processingMicrosteps_ = true;\n"
                "        constexpr int kMaxMicrosteps = 100;\n"
                "        int stepCount = 0;\n"
                "        while (stepCount < kMaxMicrosteps) {\n")
                .arg(model.hasAlways ? QStringLiteral("always transitions and internal event queue")
                                     : QStringLiteral("internal event queue"));
            if (model.hasAlways) {
                text += QStringLiteral(
                    "            if (stepAlwaysTransitions()) {\n"
                    "                ++stepCount;\n"
                    "                continue;\n"
                    "            }\n");
            }
            if (model.hasRaise) {
                text += QStringLiteral(
                    "            if (!internalQueue_.empty()) {\n"
                    "                const Event event = internalQueue_.front();\n"
                    "                internalQueue_.pop_front();\n"
                    "                dispatchInternal(event);\n"
                    "                ++stepCount;\n"
                    "                continue;\n"
                    "            }\n");
            }
            text += QStringLiteral(
                "            break;\n"
                "        }\n"
                "        processingMicrosteps_ = false;\n"
                "    }\n\n");
        }
    }
}

// matchAt_<event>(stateIndex): one state's candidates, first passing guard wins
// (the validator keeps an unguarded candidate last). matchRoot_<event>(): the
// root group, never reached by the ancestor walk. A payload is visible only to
// the guards of rows that bind it.
void appendHierMatchFunctions(QString& text, const GenModel& model, const Machine& machine) {
    const Transition* base = machine.transitions.constData();
    for (const QString& rawEvent : model.eventOrder) {
        const QString methodName = model.eventMethodByRaw.value(rawEvent);

        struct HierCandidate {
            int transitionIndex;
            int specificity;
            int originalOrder;
        };

        auto collectHierMatches = [&](int requiredSourceIdx) {
            std::vector<HierCandidate> matches;
            for (int tIdx = 0; tIdx < machine.transitions.size(); ++tIdx) {
                const Transition& t = machine.transitions.at(tIdx);
                const int sourceIdx = model.hierTransitions.at(tIdx).sourceIndex;
                const QString ev = t.event.trimmed();
                if (sourceIdx == requiredSourceIdx && !ev.isEmpty() && eventMatches(ev, rawEvent)) {
                    matches.push_back({
                        .transitionIndex = tIdx,
                        .specificity = eventDescriptorSpecificity(ev),
                        .originalOrder = tIdx,
                    });
                }
            }
            std::stable_sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
                if (a.specificity != b.specificity) {
                    return a.specificity > b.specificity;
                }
                return a.originalOrder < b.originalOrder;
            });
            QVector<int> res;
            res.reserve(static_cast<int>(matches.size()));
            for (const auto& m : matches) {
                res.push_back(m.transitionIndex);
            }
            return res;
        };

        QVector<int> fromOrder;
        QHash<int, QVector<int>> byFrom;
        for (int stateIdx = 0; stateIdx < machine.states.size(); ++stateIdx) {
            const QVector<int> candidates = collectHierMatches(stateIdx);
            if (!candidates.isEmpty()) {
                fromOrder.push_back(stateIdx);
                byFrom.insert(stateIdx, candidates);
            }
        }
        const QVector<int> rootCandidates = collectHierMatches(-1);

        const QString payloadType = model.eventPayloadTypeByRaw.value(rawEvent);
        const QString payloadParam = payloadType.isEmpty()
                                         ? QString()
                                         : QStringLiteral("[[maybe_unused]] const %1& event")
                                               .arg(cppPayloadTypeSpelling(payloadType));
        // Per-row binding; the parameter is [[maybe_unused]] because the
        // user's build may use -Wextra -Werror.
        const auto rowBareIdentifiers = [&](int tIdx) {
            return eventPayloadCppTypeForTransition(machine, machine.transitions.at(tIdx)).isEmpty()
                       ? QSet<QString>()
                       : QSet<QString>{QStringLiteral("event")};
        };

        text += QStringLiteral("    int matchAt_%1(int stateIndex%2) const {\n        switch (stateIndex) {\n")
                    .arg(methodName, payloadParam.isEmpty() ? QString() : QStringLiteral(", ") + payloadParam);
        for (int sourceIdx : fromOrder) {
            text += QStringLiteral("            case %1:\n").arg(sourceIdx);
            bool endedUnconditional = false;
            for (int tIdx : byFrom.value(sourceIdx)) {
                const QString guard = machine.transitions.at(tIdx).guard.trimmed();
                if (!guard.isEmpty()) {
                    text += QStringLiteral("                if (%1) {\n                    return %2;\n                }\n")
                                .arg(guardCondition(model, guard, /*negated=*/false, rowBareIdentifiers(tIdx)),
                                     QString::number(tIdx));
                } else {
                    text += QStringLiteral("                return %1;\n").arg(tIdx);
                    endedUnconditional = true;
                    break;
                }
            }
            if (!endedUnconditional) {
                text += QStringLiteral("                return -1;\n");
            }
        }
        text += QStringLiteral("            default:\n                return -1;\n        }\n    }\n\n");

        text += QStringLiteral("    int matchRoot_%1(%2) const {\n").arg(methodName, payloadParam);
        bool endedUnconditional = false;
        for (int tIdx : rootCandidates) {
            const QString guard = machine.transitions.at(tIdx).guard.trimmed();
            if (!guard.isEmpty()) {
                text += QStringLiteral("        if (%1) {\n            return %2;\n        }\n")
                            .arg(guardCondition(model, guard, /*negated=*/false, rowBareIdentifiers(tIdx)),
                                 QString::number(tIdx));
            } else {
                text += QStringLiteral("        return %1;\n").arg(tIdx);
                endedUnconditional = true;
                break;
            }
        }
        if (!endedUnconditional) {
            text += QStringLiteral("        return -1;\n");
        }
        text += QStringLiteral("    }\n\n");
    }

    if (model.hasAlways) {
        QVector<int> fromOrder;
        QHash<int, QVector<int>> byFrom;
        QVector<int> rootCandidates;
        for (int i = 0; i < machine.transitions.size(); ++i) {
            const Transition& t = machine.transitions.at(i);
            if (!t.isAlways()) {
                continue;
            }
            const int sourceIdx = model.hierTransitions.at(i).sourceIndex;
            if (sourceIdx == -1) {
                rootCandidates.push_back(i);
                continue;
            }
            if (!byFrom.contains(sourceIdx)) {
                fromOrder.push_back(sourceIdx);
            }
            byFrom[sourceIdx].push_back(i);
        }

        text += QStringLiteral("    int matchAt_always(int stateIndex) const {\n        switch (stateIndex) {\n");
        for (int sourceIdx : fromOrder) {
            text += QStringLiteral("            case %1:\n").arg(sourceIdx);
            bool endedUnconditional = false;
            for (int tIdx : byFrom.value(sourceIdx)) {
                const QString guard = machine.transitions.at(tIdx).guard.trimmed();
                if (!guard.isEmpty()) {
                    text += QStringLiteral("                if (%1) {\n                    return %2;\n                }\n")
                                .arg(guardCondition(model, guard, /*negated=*/false), QString::number(tIdx));
                } else {
                    text += QStringLiteral("                return %1;\n").arg(tIdx);
                    endedUnconditional = true;
                    break;
                }
            }
            if (!endedUnconditional) {
                text += QStringLiteral("                return -1;\n");
            }
        }
        text += QStringLiteral("            default:\n                return -1;\n        }\n    }\n\n");

        text += QStringLiteral("    int matchRoot_always() const {\n");
        bool endedUnconditional = false;
        for (int tIdx : rootCandidates) {
            const QString guard = machine.transitions.at(tIdx).guard.trimmed();
            if (!guard.isEmpty()) {
                text += QStringLiteral("        if (%1) {\n            return %2;\n        }\n")
                            .arg(guardCondition(model, guard, /*negated=*/false), QString::number(tIdx));
            } else {
                text += QStringLiteral("        return %1;\n").arg(tIdx);
                endedUnconditional = true;
                break;
            }
        }
        if (!endedUnconditional) {
            text += QStringLiteral("        return -1;\n");
        }
        text += QStringLiteral("    }\n\n");
    }
}

// One fireTransitionN() per transition with its guard-free firing sequence
// (the caller already checked the guard); a targetless one runs its action
// only. fireTransition(index) dispatches the ordinary rows. Payload rows take
// the payload as a parameter and are called by name instead: onDone/onError
// from armStateInvocation(), typed-event rows from fire_<event>().
void appendHierFireTransitions(QString& text, const GenModel& model, const Machine& machine) {
    const auto bindsEvent = [&](int tIdx) {
        return !eventPayloadCppTypeForTransition(machine, machine.transitions.at(tIdx)).isEmpty();
    };
    text += QStringLiteral("    void fireTransition(int transitionIndex) {\n        switch (transitionIndex) {\n");
    for (int i = 0; i < machine.transitions.size(); ++i) {
        if (invokePayloadKindForTransition(machine, machine.transitions.at(i)) != InvokePayloadKind::None ||
            bindsEvent(i)) {
            continue;  // payload row: called by name
        }
        text += QStringLiteral("            case %1:\n                fireTransition%1();\n                return;\n").arg(i);
    }
    text += QStringLiteral("            default:\n                return;\n        }\n    }\n\n");

    for (const QString& rawEvent : model.eventOrder) {
        const QString payloadType = model.eventPayloadTypeByRaw.value(rawEvent);
        if (payloadType.isEmpty()) {
            continue;
        }
        const QString methodName = model.eventMethodByRaw.value(rawEvent);
        text += QStringLiteral("    // Fires one of selectToFire()'s picks for \"%1\", payload in hand.\n").arg(rawEvent);
        text += QStringLiteral("    void fire_%1(int transitionIndex, const %2& event) {\n        switch (transitionIndex) {\n")
                    .arg(methodName, cppPayloadTypeSpelling(payloadType));
        for (int i = 0; i < machine.transitions.size(); ++i) {
            const Transition& t = machine.transitions.at(i);
            const QString ev = t.event.trimmed();
            if (ev.isEmpty() || !eventMatches(ev, rawEvent) ||
                invokePayloadKindForTransition(machine, t) != InvokePayloadKind::None) {
                continue;
            }
            text += QStringLiteral("            case %1:\n                fireTransition%1(%2);\n                return;\n")
                        .arg(QString::number(i), bindsEvent(i) ? QStringLiteral("event") : QString());
        }
        text += QStringLiteral("            default:\n                return;\n        }\n    }\n\n");
    }

    for (int i = 0; i < machine.transitions.size(); ++i) {
        const Transition& transition = machine.transitions.at(i);
        const HierTransition& ht = model.hierTransitions.at(i);
        const QString action = transition.action.trimmed();
        // This row's payload identifier, in scope for its own action only.
        const InvokePayloadKind payloadKind = invokePayloadKindForTransition(machine, transition);
        const QString eventPayloadCppType = eventPayloadCppTypeForTransition(machine, transition);
        const QSet<QString> bareIdentifiers = payloadKind == InvokePayloadKind::Output
                                                   ? QSet<QString>{QStringLiteral("output")}
                                                   : (payloadKind == InvokePayloadKind::Error
                                                          ? QSet<QString>{QStringLiteral("error")}
                                                          : (!eventPayloadCppType.isEmpty()
                                                                 ? QSet<QString>{QStringLiteral("event")}
                                                                 : QSet<QString>()));

        // A payload row declares its payload as a parameter so the emitted
        // action can name it; ordinary rows take none.
        const QString param = payloadKind == InvokePayloadKind::Output
                                   ? QStringLiteral("[[maybe_unused]] %1 output").arg(contextTypeSpelling(
                                         model.invokeOutputTypeByEffectiveId.value(
                                             model.invokeEffectiveIdByStateId.value(transition.from))))
                                   : (payloadKind == InvokePayloadKind::Error
                                          ? QStringLiteral("[[maybe_unused]] const std::string& error")
                                          : (!eventPayloadCppType.isEmpty()
                                                 ? QStringLiteral("[[maybe_unused]] const %1& event")
                                                       .arg(eventPayloadCppType)
                                                 : QString()));

        text += QStringLiteral("    void fireTransition%1(%2) {\n").arg(QString::number(i), param);
        const bool isInternalSelf = (ht.sourceIndex != -1 && ht.sourceIndex == ht.declaredTargetIndex && !transition.reenter);
        if (ht.declaredTargetIndex == -1 || isInternalSelf) {
            if (isInternalSelf) {
                text += QStringLiteral("        // internal self-transition (reenter == false): action only, skip exit/entry sequences\n");
            }
            if (!action.isEmpty()) {
                appendAction(text, model, QStringLiteral("        "), action, bareIdentifiers);
            }
            text += QStringLiteral("    }\n\n");
            continue;
        }
        text += QStringLiteral(
                    "        int exitBuf[kStateCount];\n"
                    "        int exitCount = 0;\n"
                    "        exitSequence(%1, exitBuf, exitCount);\n"
                    "        for (int i = 0; i < exitCount; ++i) {\n")
                    .arg(ht.lccaIndex);
        if (model.hasInvoke) {
            // Cancel before exit actions, as in the flat core.
            text += QStringLiteral("            cancelStateInvocation(exitBuf[i]);\n");
        }
        text += QStringLiteral(
                    "            runExitActions(exitBuf[i]);\n"
                    "        }\n");
        if (!action.isEmpty()) {
            appendAction(text, model, QStringLiteral("        "), action, bareIdentifiers);
        }
        text += QStringLiteral(
                    "        int entryBuf[kStateCount];\n"
                    "        int entryCount = 0;\n");
        if (ht.isMultiTarget) {
            QStringList targetStrs;
            for (int tIdx : ht.targetIndices) {
                targetStrs.push_back(QString::number(tIdx));
            }
            text += QStringLiteral(
                        "        const int targets[] = {%1};\n"
                        "        multiTargetEntrySequence(targets, %2, %3, entryBuf, entryCount);\n")
                        .arg(targetStrs.join(QStringLiteral(", ")),
                             QString::number(ht.targetIndices.size()),
                             QString::number(ht.lccaIndex));
        } else {
            text += QStringLiteral(
                        "        entrySequence(%1, %2, %3, entryBuf, entryCount);\n")
                        .arg(QString::number(ht.targetIndex), QString::number(ht.lccaIndex),
                             hierHistoryPolicyLiteral(ht.historyPolicy));
        }
        text += QStringLiteral(
                    "        for (int i = 0; i < entryCount; ++i) {\n"
                    "            runEntryActions(entryBuf[i]);\n"
                    "        }\n");

        // `from` is the declared source; a root firing uses the first exited
        // atomic, captured BEFORE the loop below clears active_.
        const QString fromExpr = (ht.sourceIndex == -1)
                                      ? QStringLiteral("static_cast<%1State>(fromIdx)").arg(model.machinePascal)
                                      : QStringLiteral("%1State::%2").arg(model.machinePascal, stateIdent(model, transition.from));
        if (ht.sourceIndex == -1) {
            text += QStringLiteral("        const int fromIdx = firstAtomicWithin(exitBuf, exitCount);\n");
        }

        text += QStringLiteral(
                    "        for (int i = 0; i < exitCount; ++i) {\n"
                    "            const int p = kParentIndex[exitBuf[i]];\n"
                    "            if (p != -1) {\n"
                    "                historyRecord_[p] = exitBuf[i];\n"
                    "            }\n"
                    "            active_[exitBuf[i]] = false;\n"
                    "        }\n"
                    "        for (int i = 0; i < entryCount; ++i) {\n"
                    "            active_[entryBuf[i]] = true;\n"
                    "        }\n");
        text += QStringLiteral(
                    "        if (onStateChanged_) {\n"
                    "            onStateChanged_(%1, %2State::%3);\n"
                    "        }\n")
                    .arg(fromExpr, model.machinePascal, stateIdent(model, transition.to));
        if (model.hasScheduler) {
            text += QStringLiteral(
                "        for (int i = 0; i < entryCount; ++i) {\n"
                "            armState(entryBuf[i]);\n"
                "        }\n");
        }
        if (model.hasInvoke) {
            text += QStringLiteral(
                "        for (int i = 0; i < entryCount; ++i) {\n"
                "            armStateInvocation(entryBuf[i]);\n"
                "        }\n");
        }
        text += QStringLiteral("    }\n\n");
    }
}

// runExitActions/runEntryActions(stateIndex): a case per state with actions.
void appendHierActionSwitch(QString& text, const GenModel& model, const Machine& machine, const QString& methodName,
                             bool useExitActions) {
    text += QStringLiteral("    void %1(int stateIndex) {\n        switch (stateIndex) {\n").arg(methodName);
    for (int i = 0; i < machine.states.size(); ++i) {
        const State& state = machine.states.at(i);
        const QStringList& rawActions = useExitActions ? state.exitActions : state.entryActions;
        QStringList trimmed;
        for (const QString& raw : rawActions) {
            const QString t = raw.trimmed();
            if (!t.isEmpty()) {
                trimmed.push_back(t);
            }
        }
        if (trimmed.isEmpty()) {
            continue;
        }
        text += QStringLiteral("            case %1:\n").arg(i);
        for (const QString& a : trimmed) {
            appendAction(text, model, QStringLiteral("                "), a);
        }
        text += QStringLiteral("                return;\n");
    }
    text += QStringLiteral("            default:\n                return;\n        }\n    }\n\n");
}

// armState(stateIndex)/armRoot(); only when hasScheduler. No disarm: each
// state's fire lambda checks active_[owner] at fire time, so a stale callback
// is a no-op (root lambdas need no check).
void appendHierArmMethods(QString& text, const GenModel& model, const Machine& machine) {
    text += QStringLiteral("    void armState(int stateIndex) {\n        switch (stateIndex) {\n");
    for (int i = 0; i < machine.states.size(); ++i) {
        const State& state = machine.states.at(i);
        const auto it = model.delayedBySource.constFind(state.id);
        if (it == model.delayedBySource.constEnd()) {
            continue;
        }
        text += QStringLiteral("            case %1:\n").arg(i);
        for (const Transition* t : it.value()) {
            const int tIdx = static_cast<int>(t - machine.transitions.constData());
            const QString guard = t->guard.trimmed();
            const auto tt = expr::parseTimeTrigger(t->event);
            const int effectiveDelay = tt.ok ? tt.durationMs : t->delayMs;
            const bool isPeriodic = t->periodic || (tt.ok && tt.isEvery());
            text += QStringLiteral("                scheduler_.get().scheduleAfter(%1, [this] {\n").arg(effectiveDelay);
            text += QStringLiteral(
                        "                    if (!active_[%1]) {\n"
                        "                        return;  // superseded -- this state already exited\n"
                        "                    }\n")
                        .arg(i);
            if (!guard.isEmpty()) {
                text += QStringLiteral(
                            "                    if (%1) {\n"
                            "                        return;\n"
                            "                    }\n")
                            .arg(guardCondition(model, guard, /*negated=*/true));
            }
            text += QStringLiteral("                    fireTransition(%1);\n").arg(tIdx);
            if (model.hasAlways || model.hasRaise) {
                text += QStringLiteral("                    checkAlwaysTransitions();\n");
            }
            if (isPeriodic) {
                text += QStringLiteral("                    if (active_[%1]) {\n").arg(i);
                text += QStringLiteral("                        armState(%1);\n").arg(i);
                text += QStringLiteral("                    }\n");
            }
            text += QStringLiteral("                });\n");
        }
        text += QStringLiteral("                return;\n");
    }
    text += QStringLiteral("            default:\n                return;\n        }\n    }\n\n");

    const QVector<const Transition*> rootDelayed = model.delayedBySource.value(0);
    if (rootDelayed.isEmpty()) {
        return;
    }
    text += QStringLiteral(
        "    // The machine's own root `after` countdowns -- armed ONCE by\n"
        "    // start(), never re-armed on ordinary state entry (root's owner\n"
        "    // always counts, so no liveness check is needed here).\n");
    text += QStringLiteral("    void armRoot() {\n");
    for (const Transition* t : rootDelayed) {
        const int tIdx = static_cast<int>(t - machine.transitions.constData());
        const QString guard = t->guard.trimmed();
        const auto tt = expr::parseTimeTrigger(t->event);
        const int effectiveDelay = tt.ok ? tt.durationMs : t->delayMs;
        const bool isPeriodic = t->periodic || (tt.ok && tt.isEvery());
        text += QStringLiteral("        scheduler_.get().scheduleAfter(%1, [this] {\n").arg(effectiveDelay);
        if (!guard.isEmpty()) {
            text += QStringLiteral(
                        "            if (%1) {\n"
                        "                return;\n"
                        "            }\n")
                        .arg(guardCondition(model, guard, /*negated=*/true));
        }
        text += QStringLiteral("            fireTransition(%1);\n").arg(tIdx);
        if (model.hasAlways || model.hasRaise) {
            text += QStringLiteral("            checkAlwaysTransitions();\n");
        }
        if (isPeriodic) {
            text += QStringLiteral("            armRoot();\n");
        }
        text += QStringLiteral("        });\n");
    }
    text += QStringLiteral("    }\n\n");
}

// Picks which of one state's onDone (or onError) rows fires: document order,
// first passing guard wins. Calls the winner by name, forwarding the
// completion lambda's `payloadArg`.
void appendHierInvokeDispatch(QString& text, const GenModel& model, const Machine& machine,
                               const QVector<const Transition*>& candidates, const QSet<QString>& bareIdentifiers,
                               const QString& payloadArg) {
    const Transition* base = machine.transitions.constData();
    for (const Transition* transition : candidates) {
        const QString guard = transition->guard.trimmed();
        const int tIdx = static_cast<int>(transition - base);
        if (guard.isEmpty()) {
            // Unguarded is always last in its group (validator-enforced).
            text += QStringLiteral("                        fireTransition%1(%2);\n").arg(QString::number(tIdx), payloadArg);
            if (model.hasAlways || model.hasRaise) {
                text += QStringLiteral("                        checkAlwaysTransitions();\n");
            }
            text += QStringLiteral("                        return;\n");
            break;
        }
        text += QStringLiteral("                        if (%1) {\n"
                                "                            fireTransition%2(%3);\n")
                    .arg(guardCondition(model, guard, /*negated=*/false, bareIdentifiers), QString::number(tIdx),
                         payloadArg);
        if (model.hasAlways || model.hasRaise) {
            text += QStringLiteral("                            checkAlwaysTransitions();\n");
        }
        text += QStringLiteral("                            return;\n"
                                "                        }\n");
    }
}

// armStateInvocation/cancelStateInvocation(stateIndex); only when hasInvoke.
// Completions re-check active_[stateIndex] at fire time: with Parallel regions
// only the active set can say whether this state was left.
void appendHierInvokeMethods(QString& text, const GenModel& model, const Machine& machine) {
    text += QStringLiteral(
        "    // Requests cooperative cancellation of `stateIndex`'s own invoke, if\n"
        "    // it has one -- called for every exited index in fireTransitionN()'s\n"
        "    // own exit loop, the hierarchical (stateIndex-parameterized) shape of\n"
        "    // the same idea a flat machine's core carries as a scalar-state_\n"
        "    // switch in its cancelInvocations().\n");
    text += QStringLiteral("    void cancelStateInvocation(int stateIndex) {\n        switch (stateIndex) {\n");
    for (int i = 0; i < machine.states.size(); ++i) {
        const State& state = machine.states.at(i);
        const auto effIds = model.invokeEffectiveIdsByStateId.value(state.id);
        if (effIds.isEmpty()) {
            continue;
        }
        text += QStringLiteral("            case %1:\n").arg(i);
        for (const QString& effId : effIds) {
            text += QStringLiteral("                %1.request_stop();\n").arg(invokeStopSourceMember(effId));
        }
        text += QStringLiteral("                return;\n");
    }
    text += QStringLiteral("            default:\n                return;\n        }\n    }\n\n");

    text += QStringLiteral(
        "    // Starts `stateIndex`'s own invoke, if it has one -- called from\n"
        "    // start() (the initial entry set) and from every fireTransitionN()'s\n"
        "    // own entry loop, the same re-arm-on-entry discipline a Scheduler's\n"
        "    // own armState() uses (see that method's own comment when this\n"
        "    // machine declares one). Each completion lambda's FIRST line is the\n"
        "    // fire-time re-check documented above cancelStateInvocation(); its SECOND is\n"
        "    // the guarded dispatch over this state's own onDone/onError\n"
        "    // transitions (document order, first passing guard wins), which\n"
        "    // calls the WINNING candidate's fireTransitionN() directly BY NAME,\n"
        "    // passing this lambda's own payload as its argument -- never the\n"
        "    // generic fireTransition(int) dispatch, which carries no\n"
        "    // payload at all. `output`/`error` are in scope for the transition's\n"
        "    // own action ONLY because this call site hands them in this way; an\n"
        "    // ordinary transition's fireTransitionN() declares no such parameter.\n");
    text += QStringLiteral("    void armStateInvocation(int stateIndex) {\n        switch (stateIndex) {\n");
    for (int i = 0; i < machine.states.size(); ++i) {
        const State& state = machine.states.at(i);
        const auto effIds = model.invokeEffectiveIdsByStateId.value(state.id);
        if (effIds.isEmpty()) {
            continue;
        }
        text += QStringLiteral("            case %1:\n").arg(i);
        for (const QString& effectiveId : effIds) {
            const QString stopMember = invokeStopSourceMember(effectiveId);
            const QString outputSpelling = contextTypeSpelling(model.invokeOutputTypeByEffectiveId.value(effectiveId));
            text += QStringLiteral("                %1 = std::stop_source{};\n").arg(stopMember);
            text += QStringLiteral("                invocations_.get().%1(\n").arg(invokeHookMethodName(effectiveId));
            text += QStringLiteral("                    %1.get_token(),\n").arg(stopMember);
            text += QStringLiteral("                    [this](%1 output) {\n").arg(outputSpelling);
            text += QStringLiteral("                        if (!active_[%1]) return;\n").arg(i);
            QVector<const Transition*> matchingDone;
            for (const Transition* t : model.onDoneByState.value(state.id)) {
                if (t->event == QStringLiteral("done.invoke.") + effectiveId) {
                    matchingDone.push_back(t);
                }
            }
            appendHierInvokeDispatch(text, model, machine, matchingDone,
                                      QSet<QString>{QStringLiteral("output")}, QStringLiteral("output"));
            text += QStringLiteral("                    },\n");
            text += QStringLiteral("                    [this](const std::string& error) {\n");
            text += QStringLiteral("                        if (!active_[%1]) return;\n").arg(i);
            QVector<const Transition*> matchingError;
            for (const Transition* t : model.onErrorByState.value(state.id)) {
                if (t->event == QStringLiteral("error.platform.") + effectiveId) {
                    matchingError.push_back(t);
                }
            }
            appendHierInvokeDispatch(text, model, machine, matchingError,
                                      QSet<QString>{QStringLiteral("error")}, QStringLiteral("error"));
            text += QStringLiteral("                    });\n");
        }
        text += QStringLiteral("                return;\n");
    }
    text += QStringLiteral("            default:\n                return;\n        }\n    }\n\n");
}

void appendHierAlwaysMethods(QString& text, const GenModel& model) {
    if (!model.hasAlways) {
        return;
    }
    text += QStringLiteral(
        "    // One eventless microstep: the SAME selection as every event method\n"
        "    // (selectToFire), reporting whether anything fired so the quiescence\n"
        "    // loop knows when to stop.\n"
        "    bool selectAndFireAlways(const int* candidateOf, const int* atomics, int atomicCount, int rootCandidate) {\n"
        "        int toFire[kStateCount];\n"
        "        const int fireCount = selectToFire(candidateOf, atomics, atomicCount, rootCandidate, toFire);\n"
        "        for (int i = 0; i < fireCount; ++i) {\n"
        "            fireTransition(toFire[i]);\n"
        "        }\n"
        "        return fireCount > 0;\n"
        "    }\n\n"
        "    bool stepAlwaysTransitions() {\n"
        "        int atomics[kStateCount];\n"
        "        int atomicCount = 0;\n"
        "        atomicActiveIndices(atomics, atomicCount);\n"
        "        int candidateOf[kStateCount];\n"
        "        for (int i = 0; i < kStateCount; ++i) {\n"
        "            candidateOf[i] = -1;\n"
        "        }\n"
        "        for (int a = 0; a < atomicCount; ++a) {\n"
        "            int walk = atomics[a];\n"
        "            while (walk != -1) {\n"
        "                const int found = matchAt_always(walk);\n"
        "                if (found != -1) {\n"
        "                    candidateOf[atomics[a]] = found;\n"
        "                    break;\n"
        "                }\n"
        "                walk = kParentIndex[walk];\n"
        "            }\n"
        "        }\n"
        "        return selectAndFireAlways(candidateOf, atomics, atomicCount, matchRoot_always());\n"
        "    }\n\n");
}



}  // namespace

GeneratedFile buildHierarchicalCoreFile(const Machine& machine, const GenModel& model) {
    QString text =
        bannerLines(QStringLiteral("%1Core -- the machine's whole HIERARCHICAL transition contract, ordo-free.")
                        .arg(model.machinePascal));
    QString extraHeaders;
    if (model.hasRaise) {
        extraHeaders += QStringLiteral("#include <deque>\n");
    }
    if (model.hasInvoke) {
        extraHeaders += QStringLiteral("#include <stop_token>\n");
    }
    if (model.hasContext) {
        extraHeaders += QStringLiteral("#include <cmath>\n");
    }
    if (model.hasTypes) {
        extraHeaders += QStringLiteral("#include \"%1_types.h\"\n").arg(model.machineNs);
    }
    text += QStringLiteral("#pragma once\n\n#include <algorithm>\n#include <array>\n#include <functional>\n%1#include "
                            "<utility>\n\n#include \"%2_hooks.h\"\n#include \"%2_state.h\"\n\n")
                .arg(extraHeaders, model.machineNs);
    text += openNamespace(model.nsJoined) + QStringLiteral("\n");

    text += QStringLiteral(
                "// Standalone HIERARCHICAL transition core: the C++ standard library only\n"
                "// -- no ordo, no Qt -- same embedding contract as the flat %1Core shape\n"
                "// (see that variant's own header comment when this machine is flat) with\n"
                "// one structural difference: active_ is a SET (any number of states can\n"
                "// be active at once under a Parallel region), so there is no single\n"
                "// state() accessor -- isActive()/configuration() below replace it.\n"
                "//\n"
                "// Full per-transition sequence UNROLLING is impossible here -- the exit set\n"
                "// depends on the RUNTIME configuration (which Parallel descendants are\n"
                "// active, which history records exist), not on (from,to) alone. Instead\n"
                "// this core carries STATIC topology/per-transition tables (below) plus\n"
                "// small, fixed, allocation-free WALKER methods that read them against\n"
                "// active_ at firing time, applying the same statechart rules as the\n"
                "// designer's simulator. Guard/action calls stay INLINED per transition\n"
                "// (fireTransitionN() below, one per transition) -- only the SELECTION\n"
                "// walk (which region's candidate wins) is generic.\n"
                "//\n"
                "// `guards`/`actions`%2 are held by reference and must outlive this core.\n")
                .arg(model.machinePascal, extraRefComment(model));
    if (model.hasInvoke) {
        text += QStringLiteral(
            "//\n"
            "// Invoke: cancelStateInvocation()/armStateInvocation() are the generic\n"
            "// (stateIndex-parameterized) mirror of a FLAT machine's\n"
            "// cancelInvocations()/armInvocations() -- the fire-time re-check inside\n"
            "// a completion lambda tests active_[stateIndex] (the SET), never a\n"
            "// scalar, because a Parallel region can hold it active alongside\n"
            "// others. See %1Invocations's own header comment (%2_hooks.h) for the\n"
            "// threading contract onDone/onError must honor.\n")
            .arg(model.machinePascal, model.machineNs);
    }
    if (model.hasContext) {
        text += QStringLiteral(
            "//\n"
            "// Extended state: this core OWNS the one Context instance (declared in\n"
            "// the hooks header) and is the only writer of it -- an assign-form action\n"
            "// compiled to a plain assignment in fireTransitionN()/runEntryActions()/\n"
            "// runExitActions(), plus whatever a Context&-taking action hook chooses to\n"
            "// change. Expression guards are compiled inline against it inside the\n"
            "// const matchAt_*/matchRoot_* selectors, which is why a guard only ever\n"
            "// READS it.\n");
    }
    text += QStringLiteral("class %1Core {\n").arg(model.machinePascal);
    text += QStringLiteral("public:\n");
    text += QStringLiteral(
                "    // Hoisted ahead of everything else (private, below) -- configuration()'s\n"
                "    // own return type needs to name it, and a member function's SIGNATURE\n"
                "    // (unlike its body) is not complete-class context.\n");
    text += QStringLiteral("    static constexpr int kStateCount = %1;\n\n").arg(machine.states.size());
    text += QStringLiteral("    using StateChangedCallback = std::function<void(%1State previous, %1State next)>;\n\n")
                .arg(model.machinePascal);

    if (model.hasRaise) {
        text += QStringLiteral("    enum class Event {\n");
        for (int i = 0; i < model.eventOrder.size(); ++i) {
            const QString rawEvent = model.eventOrder.at(i);
            const QString eventIdent = model.eventEnumIdentByRaw.value(rawEvent);
            const QString comma = (i + 1 < model.eventOrder.size()) ? QStringLiteral(",") : QString();
            text += QStringLiteral("        %1%2\n").arg(eventIdent, comma);
        }
        text += QStringLiteral("    };\n\n");
        text += QStringLiteral("    void raise(Event event) { internalQueue_.push_back(event); }\n");
        text += QStringLiteral("    const std::deque<Event>& internalQueue() const { return internalQueue_; }\n\n");
        text += QStringLiteral("    void dispatchInternal(Event event) {\n");
        text += QStringLiteral("        switch (event) {\n");
        for (const QString& rawEvent : model.eventOrder) {
            const QString payloadType = model.eventPayloadTypeByRaw.value(rawEvent);
            text += QStringLiteral("            case Event::%1:\n")
                        .arg(model.eventEnumIdentByRaw.value(rawEvent));
            if (!payloadType.isEmpty()) {
                text += QStringLiteral("                %1({});\n")
                            .arg(model.eventMethodByRaw.value(rawEvent));
            } else {
                text += QStringLiteral("                %1();\n")
                            .arg(model.eventMethodByRaw.value(rawEvent));
            }
            text += QStringLiteral("                break;\n");
        }
        text += QStringLiteral("            default:\n");
        text += QStringLiteral("                break;\n");
        text += QStringLiteral("        }\n");
        text += QStringLiteral("    }\n\n");
    }

    text += QStringLiteral("    %1Core(%1Guards& guards, %1Actions& actions%2)\n")
                .arg(model.machinePascal, extraCtorParams(model));
    text += QStringLiteral("        : guards_(guards), actions_(actions)%1 {\n").arg(extraCtorInit(model));
    text += QStringLiteral(
        "        historyRecord_.fill(-1);\n"
        "        start();  // full initial entry descent -- see start()'s own comment below\n"
        "    }\n\n");

    text += QStringLiteral("    bool isActive(%1State state) const { return active_[static_cast<std::size_t>(state)]; }\n\n")
                .arg(model.machinePascal);
    text += QStringLiteral(
        "    // Fixed-size active-state snapshot (document order == index ==\n"
        "    // the %1State value) -- true at index i means %1State(i) is\n"
        "    // active. No heap: a const reference to the core's own array.\n")
                .arg(model.machinePascal);
    text += QStringLiteral("    const std::array<bool, kStateCount>& configuration() const { return active_; }\n\n");

    if (model.hasContext) {
        text += contextAccessorLines();
    }

    text += QStringLiteral(
        "    // Invoked on every COMMITTED microstep -- never for a guard-rejected\n"
        "    // candidate and never for a targetless firing, matching the designer's\n"
        "    // simulator: `next` is the\n"
        "    // transition's DECLARED target (never the resolved leaf a compound/\n"
        "    // Parallel/History descent reaches); `previous` is the declared\n"
        "    // source, except a root-originated firing generalizes it to the\n"
        "    // document-order-first atomic state within the exit set.\n");
    text += QStringLiteral(
        "    void setOnStateChanged(StateChangedCallback callback) { onStateChanged_ = std::move(callback); }\n\n");

    appendHierPublicEventMethods(text, model);

    text += QStringLiteral("private:\n");
    appendHierTables(text, model, machine);
    appendHierStart(text, model);
    appendHierWalkerEngine(text, model);
    appendHierMatchFunctions(text, model, machine);
    appendHierFireTransitions(text, model, machine);
    appendHierActionSwitch(text, model, machine, QStringLiteral("runExitActions"), /*useExitActions=*/true);
    appendHierActionSwitch(text, model, machine, QStringLiteral("runEntryActions"), /*useExitActions=*/false);
    if (model.hasScheduler) {
        appendHierArmMethods(text, model, machine);
    }
    if (model.hasInvoke) {
        appendHierInvokeMethods(text, model, machine);
    }
    if (model.hasAlways) {
        appendHierAlwaysMethods(text, model);
    }

    text += QStringLiteral("    std::reference_wrapper<%1Guards> guards_;\n").arg(model.machinePascal);
    text += QStringLiteral("    std::reference_wrapper<%1Actions> actions_;\n").arg(model.machinePascal);
    if (model.hasScheduler) {
        text += QStringLiteral("    std::reference_wrapper<%1Scheduler> scheduler_;\n").arg(model.machinePascal);
    }
    if (model.hasInvoke) {
        text += QStringLiteral("    std::reference_wrapper<%1Invocations> invocations_;\n").arg(model.machinePascal);
        for (const QString& effectiveId : model.invokeIdOrder) {
            text += QStringLiteral("    std::stop_source %1;\n").arg(invokeStopSourceMember(effectiveId));
        }
    }
    if (model.hasContext) {
        text += contextMemberLine();
    }
    text += QStringLiteral(
        "    std::array<bool, kStateCount> active_{};       // document-order indexed\n"
        "    std::array<int, kStateCount> historyRecord_{};  // -1 = no record, filled in the ctor body\n"
        "    StateChangedCallback onStateChanged_;\n");
    if (model.hasRaise) {
        text += QStringLiteral("    std::deque<Event> internalQueue_;\n");
        text += QStringLiteral("    bool processingMicrosteps_ = false;\n");
    }
    text += QStringLiteral("};\n\n");
    text += closeNamespace(model.nsJoined);

    return GeneratedFile{.relativePath = model.machineNs + QStringLiteral("_core.h"), .content = text};
}

}  // namespace app
