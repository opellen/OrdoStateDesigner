#include <cstdio>
#include <QDir>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "infra/machine_validator.h"
#include "infra/project_io.h"
#include "model/machine.h"

#include "harness/harness.h"

int runValidatorSmoke() {
    // ---- no initial state -> Error, and the reachability BFS never runs ------
    {
        app::Machine machine;
        machine.name = QStringLiteral("NoInitial");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        machine.nextId = 2;  // initialStateId stays 0
        const QVector<app::Problem> problems = app::validate(machine);
        bool found = false;
        for (const app::Problem& problem : problems) {
            found = found || (problem.severity == app::ProblemSeverity::Error &&
                              problem.text.contains(QStringLiteral("no initial state")));
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not flag a machine with no initial state\n");
            return 1;
        }
    }

    // ---- dangling initialStateId -> Error (hand-edited .sdm only) -------------
    {
        app::Machine machine;
        machine.name = QStringLiteral("DanglingInitial");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        machine.nextId = 2;
        machine.initialStateId = 7;  // no state carries id 7
        const QVector<app::Problem> problems = app::validate(machine);
        bool found = false;
        for (const app::Problem& problem : problems) {
            found = found || (problem.severity == app::ProblemSeverity::Error &&
                              problem.text.contains(QStringLiteral("does not exist")));
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not flag a dangling initialStateId\n");
            return 1;
        }
    }

    // ---- unreachable state -> Warning -----------------------------------------
    {
        app::Machine machine;
        machine.name = QStringLiteral("Unreachable");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        machine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal});  // no incoming edge
        machine.nextId = 3;
        machine.initialStateId = 1;
        const QVector<app::Problem> problems = app::validate(machine);
        bool found = false;
        for (const app::Problem& problem : problems) {
            found = found || (problem.severity == app::ProblemSeverity::Warning && problem.stateId == 2);
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not warn that state 2 is unreachable from the initial state\n");
            return 1;
        }
    }

    // ---- check 3 default-entry-descent closure ----
    {
        auto countUnreachable = [](const QVector<app::Problem>& problems) {
            int n = 0;
            for (const app::Problem& p : problems) {
                if (p.severity == app::ProblemSeverity::Warning &&
                    p.text.contains(QStringLiteral("is unreachable from the initial state"))) {
                    ++n;
                }
            }
            return n;
        };

        // (d) P {A, B}: machine initial = P (a compound), P.initialChildId =
        // A, A -> B on an event, nothing else targets A. The BFS must follow
        // initialChildId, or A and B are falsely flagged unreachable.
        app::Machine compoundMachine;
        compoundMachine.name = QStringLiteral("CompoundInitialDescent");
        compoundMachine.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Normal, .initialChildId = 2});
        compoundMachine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 1});
        compoundMachine.states.push_back(
            app::State{.id = 3, .name = QStringLiteral("B"), .kind = app::StateKind::Normal, .parentId = 1});
        compoundMachine.initialStateId = 1;
        compoundMachine.transitions.push_back(
            app::Transition{.id = 10, .from = 2, .to = 3, .event = QStringLiteral("Next")});
        compoundMachine.nextId = 11;
        if (countUnreachable(app::validate(compoundMachine)) != 0) {
            std::fprintf(stderr, "FAIL: validate() still warns unreachable for P's initialChildId descent (A/B)\n");
            return 1;
        }

        // (e) A Parallel state with two regions, each with its own initial
        // child -- entering the Parallel enters EVERY region by default,
        // not just one.
        app::Machine parallelMachine;
        parallelMachine.name = QStringLiteral("ParallelRegionDescent");
        parallelMachine.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("Par"), .kind = app::StateKind::Parallel});
        parallelMachine.states.push_back(app::State{.id = 2,
                                                      .name = QStringLiteral("Region1"),
                                                      .kind = app::StateKind::Normal,
                                                      .parentId = 1,
                                                      .initialChildId = 4});
        parallelMachine.states.push_back(app::State{.id = 3,
                                                      .name = QStringLiteral("Region2"),
                                                      .kind = app::StateKind::Normal,
                                                      .parentId = 1,
                                                      .initialChildId = 5});
        parallelMachine.states.push_back(
            app::State{.id = 4, .name = QStringLiteral("R1Init"), .kind = app::StateKind::Normal, .parentId = 2});
        parallelMachine.states.push_back(
            app::State{.id = 5, .name = QStringLiteral("R2Init"), .kind = app::StateKind::Normal, .parentId = 3});
        parallelMachine.initialStateId = 1;
        parallelMachine.nextId = 6;
        if (countUnreachable(app::validate(parallelMachine)) != 0) {
            std::fprintf(stderr, "FAIL: validate() warns unreachable inside a Parallel state's regions\n");
            return 1;
        }

        // (f) A genuinely unreachable root-level state (not targeted, not the
        // initial state) must still warn exactly once.
        app::Machine trulyUnreachable;
        trulyUnreachable.name = QStringLiteral("GenuinelyUnreachable");
        trulyUnreachable.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        trulyUnreachable.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("Orphan"), .kind = app::StateKind::Normal});
        trulyUnreachable.initialStateId = 1;
        trulyUnreachable.nextId = 3;
        if (countUnreachable(app::validate(trulyUnreachable)) != 1) {
            std::fprintf(stderr,
                         "FAIL: validate() did not report exactly one unreachable Warning for a genuinely orphaned state\n");
            return 1;
        }

        // (g) A transition targets a History state directly. Entering History
        // redirects to its PARENT's default descent, so the Parallel parent and
        // BOTH regions' initial children are reachable although nothing targets
        // them; closeDefaultDescent must not stop at the History state.
        app::Machine historyRedirect;
        historyRedirect.name = QStringLiteral("HistoryRedirectDescent");
        historyRedirect.states.push_back(app::State{.id = 1, .name = QStringLiteral("Idle"), .kind = app::StateKind::Normal});
        historyRedirect.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("Proc"), .kind = app::StateKind::Parallel});
        historyRedirect.states.push_back(app::State{
            .id = 3, .name = QStringLiteral("RegionA"), .kind = app::StateKind::Normal, .parentId = 2, .initialChildId = 4});
        historyRedirect.states.push_back(
            app::State{.id = 4, .name = QStringLiteral("Leaf1"), .kind = app::StateKind::Normal, .parentId = 3});
        historyRedirect.states.push_back(app::State{
            .id = 5, .name = QStringLiteral("RegionB"), .kind = app::StateKind::Normal, .parentId = 2, .initialChildId = 6});
        historyRedirect.states.push_back(
            app::State{.id = 6, .name = QStringLiteral("Leaf2"), .kind = app::StateKind::Normal, .parentId = 5});
        historyRedirect.states.push_back(
            app::State{.id = 7, .name = QStringLiteral("Hist"), .kind = app::StateKind::History, .parentId = 2});
        historyRedirect.initialStateId = 1;
        historyRedirect.transitions.push_back(
            app::Transition{.id = 20, .from = 1, .to = 7, .event = QStringLiteral("Resume")});
        historyRedirect.nextId = 21;
        if (countUnreachable(app::validate(historyRedirect)) != 0) {
            std::fprintf(stderr,
                         "FAIL: validate() warns unreachable for a Parallel state (and its regions' initial children) "
                         "reached only via a History-target redirect\n");
            return 1;
        }

        // (h) An ancestor entered only THROUGH its child still fires its
        // own transitions: machine initial = A inside P, nothing targets P,
        // P -Done-> C. C is reachable.
        app::Machine ancestorEdge;
        ancestorEdge.name = QStringLiteral("AncestorEdgeWalk");
        ancestorEdge.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Normal, .initialChildId = 2});
        ancestorEdge.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("A"), .kind = app::StateKind::Normal, .parentId = 1});
        ancestorEdge.states.push_back(app::State{.id = 3, .name = QStringLiteral("C"), .kind = app::StateKind::Normal});
        ancestorEdge.initialStateId = 2;
        ancestorEdge.transitions.push_back(
            app::Transition{.id = 10, .from = 1, .to = 3, .event = QStringLiteral("Done")});
        ancestorEdge.nextId = 11;
        if (countUnreachable(app::validate(ancestorEdge)) != 0) {
            std::fprintf(stderr,
                         "FAIL: validate() warns unreachable for a state reached only by an ancestor's own transition\n");
            return 1;
        }
    }

    // ---- blank event + delayMs == 0 -> Error (fires never) --------------------
    {
        app::Machine machine;
        machine.name = QStringLiteral("DeadTransition");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        machine.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal});
        machine.transitions.push_back(app::Transition{.id = 3, .from = 1, .to = 2});  // blank event, delayMs 0
        machine.nextId = 4;
        machine.initialStateId = 1;
        const QVector<app::Problem> problems = app::validate(machine);
        bool found = false;
        for (const app::Problem& problem : problems) {
            found = found || (problem.severity == app::ProblemSeverity::Error && problem.transitionId == 3);
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not flag the blank-event/delayMs==0 transition\n");
            return 1;
        }
    }

    // ---- guard-aware candidate-fallback legality (both directions) ------------
    // XState v5 guarded-candidate arrays: document order is evaluation order,
    // and every row but the last needs a non-blank guard.
    {
        // Legal: guarded row first, bare (unguarded) fallback LAST -- zero
        // problems, same shape as runXStateInteropSmoke()'s Checkout.PAY pair.
        app::Machine machine;
        machine.name = QStringLiteral("GuardedFallback");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        machine.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Final});
        machine.states.push_back(app::State{.id = 3, .name = QStringLiteral("C"), .kind = app::StateKind::Final});
        machine.transitions.push_back(app::Transition{
            .id = 4, .from = 1, .to = 2, .event = QStringLiteral("go"), .guard = QStringLiteral("canGo")});
        machine.transitions.push_back(app::Transition{.id = 5, .from = 1, .to = 3, .event = QStringLiteral("go")});
        machine.nextId = 6;
        machine.initialStateId = 1;
        const QVector<app::Problem> legalProblems = app::validate(machine);
        if (!legalProblems.isEmpty()) {
            std::fprintf(stderr,
                         "FAIL: validate() flagged a legal guarded-then-bare-fallback candidate group (%d problem(s))\n",
                         static_cast<int>(legalProblems.size()));
            return 1;
        }

        // Illegal: swap the two rows -- the now-FIRST row is unguarded and
        // not last, so it shadows transition 5 (now last), which is an Error.
        app::Machine shadowed = machine;
        shadowed.name = QStringLiteral("UnguardedNotLast");
        shadowed.transitions[0].guard.clear();  // was "canGo" on transition id 4
        shadowed.transitions[1].guard = QStringLiteral("neverChecked");
        bool found = false;
        for (const app::Problem& problem : app::validate(shadowed)) {
            found = found || (problem.severity == app::ProblemSeverity::Error && problem.transitionId == 4 &&
                               problem.text.contains(QStringLiteral("shadows transition 5")));
        }
        if (!found) {
            std::fprintf(stderr,
                         "FAIL: validate() did not flag the unguarded, non-last transition 4 shadowing transition 5\n");
            return 1;
        }
    }

    // ---- identifier collision after sanitization -> Error ---------------------
    // "Logged Out" and "Logged-Out" both sanitize to "LoggedOut"
    // (sanitizeIdentifier() in infra/code_generator.h).
    {
        app::Machine machine;
        machine.name = QStringLiteral("Collision");
        machine.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("Logged Out"), .kind = app::StateKind::Normal});
        machine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("Logged-Out"), .kind = app::StateKind::Normal});
        machine.transitions.push_back(
            app::Transition{.id = 3, .from = 1, .to = 2, .event = QStringLiteral("go")});
        machine.nextId = 4;
        machine.initialStateId = 1;
        const QVector<app::Problem> problems = app::validate(machine);
        bool found = false;
        for (const app::Problem& problem : problems) {
            found = found || (problem.severity == app::ProblemSeverity::Error &&
                              problem.text.contains(QStringLiteral("sanitize to the same identifier")));
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not flag 'Logged Out'/'Logged-Out' colliding after sanitization\n");
            return 1;
        }
    }

    // ---- root transitions: legal source, reachability seed, duplicate group ----
    {
        app::Machine machine;
        machine.name = QStringLiteral("Rooted");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        machine.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal});  // only root-reachable
        machine.transitions.push_back(
            app::Transition{.id = 3, .from = 0, .to = 2, .event = QStringLiteral("JUMP")});
        machine.nextId = 4;
        machine.initialStateId = 1;
        if (!app::validate(machine).isEmpty()) {
            std::fprintf(stderr,
                         "FAIL: validate() flagged a machine whose state B is reachable via a root transition\n");
            return 1;
        }
        // Second JUMP in the group: id 3 (added above, unguarded) is now not
        // last and shadows this one (id 4) -- an Error.
        machine.transitions.push_back(
            app::Transition{.id = 4, .from = 0, .to = 1, .event = QStringLiteral("JUMP")});
        bool found = false;
        for (const app::Problem& problem : app::validate(machine)) {
            found = found || (problem.severity == app::ProblemSeverity::Error &&
                              problem.text.contains(QStringLiteral("shadows transition")));
        }
        if (!found) {
            std::fprintf(stderr, "FAIL: validate() did not flag the unguarded root JUMP shadowing group\n");
            return 1;
        }
    }

    // ---- a well-formed machine -> zero problems (validate() must not cry wolf) ----
    {
        app::Machine machine;
        machine.name = QStringLiteral("Clean");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        machine.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Final});
        machine.transitions.push_back(
            app::Transition{.id = 3, .from = 1, .to = 2, .event = QStringLiteral("go")});
        machine.nextId = 4;
        machine.initialStateId = 1;
        const QVector<app::Problem> problems = app::validate(machine);
        if (!problems.isEmpty()) {
            std::fprintf(stderr, "FAIL: validate() reported %d problem(s) on a well-formed machine:\n",
                         static_cast<int>(problems.size()));
            for (const app::Problem& problem : problems) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(problem.text));
            }
            return 1;
        }
    }

    // ---- always transitions: legality, self-loop guard, and candidate group ---
    {
        // Legal: A -> B always (blank event, delay 0)
        app::Machine machine;
        machine.name = QStringLiteral("CleanAlways");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        machine.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Final});
        machine.transitions.push_back(
            app::Transition{.id = 3, .from = 1, .to = 2, .always = true});
        machine.nextId = 4;
        machine.initialStateId = 1;
        const QVector<app::Problem> cleanProblems = app::validate(machine);
        if (!cleanProblems.isEmpty()) {
            std::fprintf(stderr, "FAIL: validate() flagged a legal always transition\n");
            return 1;
        }

        // Illegal: unguarded always self-transition
        app::Machine selfLoop = machine;
        selfLoop.name = QStringLiteral("AlwaysSelfLoop");
        selfLoop.transitions[0].to = 1;  // A -> A
        const QVector<app::Problem> loopProblems = app::validate(selfLoop);
        bool foundLoop = false;
        for (const app::Problem& problem : loopProblems) {
            foundLoop = foundLoop || (problem.severity == app::ProblemSeverity::Error &&
                                      problem.transitionId == 3 &&
                                      problem.text.contains(QStringLiteral("always self-transition has no guard")));
        }
        if (!foundLoop) {
            std::fprintf(stderr, "FAIL: validate() did not flag unguarded always self-loop\n");
            return 1;
        }

        // Legal: guarded always self-transition
        app::Machine guardedSelf;
        guardedSelf.name = QStringLiteral("GuardedSelf");
        guardedSelf.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        guardedSelf.transitions.push_back(app::Transition{
            .id = 2, .from = 1, .to = 1, .guard = QStringLiteral("canLoop"), .always = true
        });
        guardedSelf.nextId = 3;
        guardedSelf.initialStateId = 1;
        if (!app::validate(guardedSelf).isEmpty()) {
            std::fprintf(stderr, "FAIL: validate() flagged a guarded always self-loop\n");
            return 1;
        }

        // Always candidate group: guarded first, unguarded second -> legal
        app::Machine fallbackGroup = machine;
        fallbackGroup.transitions[0].guard = QStringLiteral("isReady");
        fallbackGroup.transitions.push_back(
            app::Transition{.id = 4, .from = 1, .to = 2, .always = true});  // unguarded fallback
        fallbackGroup.nextId = 5;
        if (!app::validate(fallbackGroup).isEmpty()) {
            std::fprintf(stderr, "FAIL: validate() flagged a legal always candidate group\n");
            return 1;
        }

        // Always candidate group: unguarded first, guarded second -> Error (shadowing)
        app::Machine shadowedAlways = fallbackGroup;
        shadowedAlways.transitions[0].guard.clear();
        shadowedAlways.transitions[1].guard = QStringLiteral("isReady");
        bool foundShadow = false;
        for (const app::Problem& problem : app::validate(shadowedAlways)) {
            foundShadow = foundShadow || (problem.severity == app::ProblemSeverity::Error &&
                                          problem.transitionId == 3 &&
                                          problem.text.contains(QStringLiteral("shadows transition 4")));
        }
        if (!foundShadow) {
            std::fprintf(stderr, "FAIL: validate() did not flag unguarded always transition shadowing candidate\n");
            return 1;
        }
    }

    // ---- 14. Check 14: Event descriptor wildcard syntax --------------------
    {
        app::Machine wm;
        wm.name = QStringLiteral("WildcardTest");
        wm.initialStateId = 1;
        wm.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        wm.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal});
        wm.transitions.push_back(app::Transition{.id = 3, .from = 1, .to = 2, .event = QStringLiteral("*")});
        wm.transitions.push_back(app::Transition{.id = 4, .from = 2, .to = 1, .event = QStringLiteral("mouse.*")});
        wm.nextId = 5;

        // Valid wildcards: "*" and "mouse.*" should produce no Check 14 problems
        bool hasWildcardError = false;
        for (const app::Problem& p : app::validate(wm)) {
            if (p.severity == app::ProblemSeverity::Error && p.text.contains(QStringLiteral("wildcard"))) {
                hasWildcardError = true;
            }
        }
        if (hasWildcardError) {
            std::fprintf(stderr, "FAIL: validate() reported error on valid wildcards '*' and 'mouse.*'\n");
            return 1;
        }

        // Invalid wildcard 1: "foo*bar" (middle star)
        wm.transitions[0].event = QStringLiteral("foo*bar");
        bool foundMiddleStar = false;
        for (const app::Problem& p : app::validate(wm)) {
            if (p.severity == app::ProblemSeverity::Error && p.transitionId == wm.transitions[0].id &&
                p.text.contains(QStringLiteral("invalid wildcard syntax"))) {
                foundMiddleStar = true;
            }
        }
        if (!foundMiddleStar) {
            std::fprintf(stderr, "FAIL: validate() did not flag invalid wildcard 'foo*bar'\n");
            return 1;
        }

        // Invalid wildcard 2: "**" (multiple stars)
        wm.transitions[0].event = QStringLiteral("**");
        bool foundMultiStar = false;
        for (const app::Problem& p : app::validate(wm)) {
            if (p.severity == app::ProblemSeverity::Error && p.transitionId == wm.transitions[0].id &&
                p.text.contains(QStringLiteral("invalid wildcard syntax"))) {
                foundMultiStar = true;
            }
        }
        if (!foundMultiStar) {
            std::fprintf(stderr, "FAIL: validate() did not flag invalid wildcard '**'\n");
            return 1;
        }

        // Invalid wildcard 3: ".*" (empty prefix before dot)
        wm.transitions[0].event = QStringLiteral(".*");
        bool foundEmptyPrefix = false;
        for (const app::Problem& p : app::validate(wm)) {
            if (p.severity == app::ProblemSeverity::Error && p.transitionId == wm.transitions[0].id &&
                p.text.contains(QStringLiteral("invalid prefix"))) {
                foundEmptyPrefix = true;
            }
        }
        if (!foundEmptyPrefix) {
            std::fprintf(stderr, "FAIL: validate() did not flag invalid wildcard '.*'\n");
            return 1;
        }
    }

    // ---- 17. multi-target transition validation (W3C SCXML §3.3.1) ------------
    {
        app::Machine mm;
        mm.name = QStringLiteral("MultiTargetValidation");
        mm.initialStateId = 1;
        mm.states.push_back(app::State{.id = 1, .name = QStringLiteral("Root"), .kind = app::StateKind::Parallel});
        mm.states.push_back(app::State{.id = 2, .name = QStringLiteral("RegA"), .kind = app::StateKind::Normal, .parentId = 1});
        mm.states.push_back(app::State{.id = 3, .name = QStringLiteral("A1"), .kind = app::StateKind::Normal, .parentId = 2});
        mm.states.push_back(app::State{.id = 4, .name = QStringLiteral("RegB"), .kind = app::StateKind::Normal, .parentId = 1});
        mm.states.push_back(app::State{.id = 5, .name = QStringLiteral("B1"), .kind = app::StateKind::Normal, .parentId = 4});

        // Valid multi-target: targeting A1 and B1 in orthogonal regions of Root (Parallel)
        mm.transitions.push_back(app::Transition{
            .id = 6,
            .from = 3,
            .to = 3,
            .event = QStringLiteral("FORK"),
            .targets = {3, 5}
        });

        QVector<app::Problem> problems = app::validate(mm);
        for (const app::Problem& p : problems) {
            if (p.severity == app::ProblemSeverity::Error && p.transitionId == 6) {
                std::fprintf(stderr, "FAIL: validate() flagged valid multi-target transition: %s\n",
                             p.text.toUtf8().constData());
                return 1;
            }
        }

        // Error case a: Non-existent target ID
        mm.transitions[0].targets = {3, 999};
        problems = app::validate(mm);
        bool foundNonExistent = false;
        for (const app::Problem& p : problems) {
            if (p.severity == app::ProblemSeverity::Error && p.transitionId == 6 &&
                p.text.contains(QStringLiteral("non-existent target id 999"))) {
                foundNonExistent = true;
            }
        }
        if (!foundNonExistent) {
            std::fprintf(stderr, "FAIL: validate() did not flag non-existent target id\n");
            return 1;
        }

        // Error case b: Duplicate target ID
        mm.transitions[0].targets = {3, 3};
        problems = app::validate(mm);
        bool foundDuplicate = false;
        for (const app::Problem& p : problems) {
            if (p.severity == app::ProblemSeverity::Error && p.transitionId == 6 &&
                p.text.contains(QStringLiteral("duplicate target id 3"))) {
                foundDuplicate = true;
            }
        }
        if (!foundDuplicate) {
            std::fprintf(stderr, "FAIL: validate() did not flag duplicate target id\n");
            return 1;
        }

        // Error case c: machineSelf conflict
        mm.transitions[0].targets = {3, 5};
        mm.transitions[0].machineSelf = true;
        problems = app::validate(mm);
        bool foundMachineSelf = false;
        for (const app::Problem& p : problems) {
            if (p.severity == app::ProblemSeverity::Error && p.transitionId == 6 &&
                p.text.contains(QStringLiteral("cannot target the machine itself"))) {
                foundMachineSelf = true;
            }
        }
        if (!foundMachineSelf) {
            std::fprintf(stderr, "FAIL: validate() did not flag multi-target machineSelf conflict\n");
            return 1;
        }
        mm.transitions[0].machineSelf = false;

        // Error case d: Ancestor-descendant conflict (RegA is parent of A1)
        mm.transitions[0].targets = {2, 3};
        problems = app::validate(mm);
        bool foundAncestorConflict = false;
        for (const app::Problem& p : problems) {
            if (p.severity == app::ProblemSeverity::Error && p.transitionId == 6 &&
                p.text.contains(QStringLiteral("ancestor-descendant conflict"))) {
                foundAncestorConflict = true;
            }
        }
        if (!foundAncestorConflict) {
            std::fprintf(stderr, "FAIL: validate() did not flag ancestor-descendant conflict\n");
            return 1;
        }

        // Error case e: Non-parallel LCCA
        // Change Root kind to Normal (Compound)
        mm.states[0].kind = app::StateKind::Normal;
        mm.transitions[0].targets = {3, 5};
        problems = app::validate(mm);
        bool foundNonParallelLcca = false;
        for (const app::Problem& p : problems) {
            if (p.severity == app::ProblemSeverity::Error && p.transitionId == 6 &&
                p.text.contains(QStringLiteral("do not have a parallel ancestor"))) {
                foundNonParallelLcca = true;
            }
        }
        if (!foundNonParallelLcca) {
            std::fprintf(stderr, "FAIL: validate() did not flag targets without parallel ancestor\n");
            return 1;
        }
    }

    // ---- Check 6 false-positive prevention: inline expression guards & assign actions ----
    {
        app::Machine machine;
        machine.name = QStringLiteral("InlineGuardsAndActions");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("S1"), .kind = app::StateKind::Normal});
        machine.states.push_back(app::State{.id = 2, .name = QStringLiteral("S2"), .kind = app::StateKind::Normal});
        machine.context.push_back(app::ContextVariable{
            .id = 10,
            .name = QStringLiteral("attempts"),
            .type = app::ContextType::Int,
            .initialValue = QStringLiteral("0"),
        });
        // Two transitions whose guards sanitize to the same identifier 'attempts3'
        // ("attempts >= 3" and "attempts < 3"), and whose actions sanitize to
        // the same identifier 'attemptsAttempts1' ("attempts = attempts + 1" and "attempts = attempts - 1").
        // Since both are inline expressions/assigns, Check 6 must NOT flag them as hook collisions.
        machine.transitions.push_back(app::Transition{
            .id = 3,
            .from = 1,
            .to = 2,
            .event = QStringLiteral("EV1"),
            .guard = QStringLiteral("attempts >= 3"),
            .action = QStringLiteral("attempts = attempts + 1"),
        });
        machine.transitions.push_back(app::Transition{
            .id = 4,
            .from = 1,
            .to = 2,
            .event = QStringLiteral("EV2"),
            .guard = QStringLiteral("attempts < 3"),
            .action = QStringLiteral("attempts = attempts - 1"),
        });
        machine.nextId = 5;
        machine.initialStateId = 1;

        const QVector<app::Problem> problems = app::validate(machine);
        for (const app::Problem& p : problems) {
            if (p.severity == app::ProblemSeverity::Error &&
                p.text.contains(QStringLiteral("both sanitize to the same identifier"))) {
                std::fprintf(stderr,
                             "FAIL: validate() falsely flagged inline guard/action as hook collision: %s\n",
                             qUtf8Printable(p.text));
                return 1;
            }
        }

        // But genuine hook collisions MUST still be caught:
        machine.transitions.push_back(app::Transition{
            .id = 5,
            .from = 1,
            .to = 2,
            .event = QStringLiteral("EV3"),
            .guard = QStringLiteral("is_active"),
            .action = QStringLiteral("do_save"),
        });
        machine.transitions.push_back(app::Transition{
            .id = 6,
            .from = 1,
            .to = 2,
            .event = QStringLiteral("EV4"),
            .guard = QStringLiteral("isActive"),
            .action = QStringLiteral("doSave"),
        });
        machine.nextId = 7;

        bool foundGuardHookCollision = false;
        bool foundActionHookCollision = false;
        for (const app::Problem& p : app::validate(machine)) {
            if (p.severity == app::ProblemSeverity::Error &&
                p.text.contains(QStringLiteral("both sanitize to the same identifier"))) {
                if (p.text.contains(QStringLiteral("Guard names 'is_active' and 'isActive'"))) {
                    foundGuardHookCollision = true;
                }
                if (p.text.contains(QStringLiteral("Action names 'do_save' and 'doSave'"))) {
                    foundActionHookCollision = true;
                }
            }
        }
        if (!foundGuardHookCollision) {
            std::fprintf(stderr, "FAIL: validate() did not flag colliding guard hooks 'is_active' and 'isActive'\n");
            return 1;
        }
        if (!foundActionHookCollision) {
            std::fprintf(stderr, "FAIL: validate() did not flag colliding action hooks 'do_save' and 'doSave'\n");
            return 1;
        }
    }

    // ---- 18 & 19. Struct schema, external headers, and payload type validation ----
    {
        app::Machine machine;
        machine.name = QStringLiteral("StructCheckMachine");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A")});
        machine.initialStateId = 1;

        // 1. Bad struct identifier
        machine.types.push_back(app::StructDefinition{
            .id = 2,
            .name = QStringLiteral("1InvalidStruct"),
        });
        bool foundBadStructIdent = false;
        for (const app::Problem& p : app::validate(machine)) {
            if (p.severity == app::ProblemSeverity::Error &&
                p.text.contains(QStringLiteral("struct '1InvalidStruct' is not a valid identifier"))) {
                foundBadStructIdent = true;
            }
        }
        if (!foundBadStructIdent) {
            std::fprintf(stderr, "FAIL: validate() did not flag invalid struct identifier '1InvalidStruct'\n");
            return 1;
        }
        machine.types.clear();

        // 2. Reserved word struct
        machine.types.push_back(app::StructDefinition{
            .id = 2,
            .name = QStringLiteral("class"),
        });
        bool foundReservedStruct = false;
        for (const app::Problem& p : app::validate(machine)) {
            if (p.severity == app::ProblemSeverity::Error &&
                p.text.contains(QStringLiteral("struct 'class' is a C++ reserved word"))) {
                foundReservedStruct = true;
            }
        }
        if (!foundReservedStruct) {
            std::fprintf(stderr, "FAIL: validate() did not flag C++ reserved word struct 'class'\n");
            return 1;
        }
        machine.types.clear();

        // 3. Duplicate struct definitions
        machine.types.push_back(app::StructDefinition{.id = 2, .name = QStringLiteral("Point")});
        machine.types.push_back(app::StructDefinition{.id = 3, .name = QStringLiteral("Point")});
        bool foundDuplicateStruct = false;
        for (const app::Problem& p : app::validate(machine)) {
            if (p.severity == app::ProblemSeverity::Error &&
                p.text.contains(QStringLiteral("two struct definitions share the name 'Point'"))) {
                foundDuplicateStruct = true;
            }
        }
        if (!foundDuplicateStruct) {
            std::fprintf(stderr, "FAIL: validate() did not flag duplicate struct 'Point'\n");
            return 1;
        }
        machine.types.clear();

        // 4. External struct missing header path
        machine.types.push_back(app::StructDefinition{
            .id = 2,
            .name = QStringLiteral("ExtType"),
            .external = true,
            .headerPath = QString(),
        });
        bool foundMissingHeader = false;
        for (const app::Problem& p : app::validate(machine)) {
            if (p.severity == app::ProblemSeverity::Error &&
                p.text.contains(QStringLiteral("external struct 'ExtType' must specify a header path"))) {
                foundMissingHeader = true;
            }
        }
        if (!foundMissingHeader) {
            std::fprintf(stderr, "FAIL: validate() did not flag external struct missing header path\n");
            return 1;
        }
        machine.types.clear();

        // 5. Bad field identifier, reserved word field, duplicate field, unknown custom type
        machine.types.push_back(app::StructDefinition{
            .id = 2,
            .name = QStringLiteral("SensorData"),
            .fields = {
                app::StructField{.name = QStringLiteral("3val"), .type = app::FieldType::Int},
                app::StructField{.name = QStringLiteral("const"), .type = app::FieldType::Int},
                app::StructField{.name = QStringLiteral("temp"), .type = app::FieldType::Double},
                app::StructField{.name = QStringLiteral("temp"), .type = app::FieldType::Double},
                app::StructField{.name = QStringLiteral("nested"), .type = app::FieldType::Custom, .customTypeName = QStringLiteral("NonExistent")},
                app::StructField{.name = QStringLiteral("buffer"), .type = app::FieldType::Int, .isArray = true, .arraySize = -5},
            },
        });
        bool foundBadFieldIdent = false;
        bool foundReservedField = false;
        bool foundDuplicateField = false;
        bool foundUnknownCustomField = false;
        bool foundNegativeArray = false;
        for (const app::Problem& p : app::validate(machine)) {
            if (p.severity == app::ProblemSeverity::Error) {
                if (p.text.contains(QStringLiteral("field '3val' in struct 'SensorData' is not a valid identifier"))) {
                    foundBadFieldIdent = true;
                }
                if (p.text.contains(QStringLiteral("field 'const' in struct 'SensorData' is a C++ reserved word"))) {
                    foundReservedField = true;
                }
                if (p.text.contains(QStringLiteral("two fields in struct 'SensorData' share the name 'temp'"))) {
                    foundDuplicateField = true;
                }
                if (p.text.contains(QStringLiteral("field 'nested' in struct 'SensorData' references unknown struct type 'NonExistent'"))) {
                    foundUnknownCustomField = true;
                }
                if (p.text.contains(QStringLiteral("field 'buffer' in struct 'SensorData' has invalid negative array size -5"))) {
                    foundNegativeArray = true;
                }
            }
        }
        if (!foundBadFieldIdent || !foundReservedField || !foundDuplicateField || !foundUnknownCustomField || !foundNegativeArray) {
            std::fprintf(stderr, "FAIL: validate() did not flag all field-level errors (badIdent:%d, reserved:%d, dup:%d, unknown:%d, negArray:%d)\n",
                         foundBadFieldIdent, foundReservedField, foundDuplicateField, foundUnknownCustomField, foundNegativeArray);
            return 1;
        }
        machine.types.clear();

        // 6. Circular struct dependency
        machine.types.push_back(app::StructDefinition{
            .id = 2,
            .name = QStringLiteral("NodeA"),
            .fields = {
                app::StructField{.name = QStringLiteral("b"), .type = app::FieldType::Custom, .customTypeName = QStringLiteral("NodeB")},
            },
        });
        machine.types.push_back(app::StructDefinition{
            .id = 3,
            .name = QStringLiteral("NodeB"),
            .fields = {
                app::StructField{.name = QStringLiteral("a"), .type = app::FieldType::Custom, .customTypeName = QStringLiteral("NodeA")},
            },
        });
        bool foundCycle = false;
        for (const app::Problem& p : app::validate(machine)) {
            if (p.severity == app::ProblemSeverity::Error &&
                p.text.contains(QStringLiteral("circular dependency detected in struct definitions"))) {
                foundCycle = true;
            }
        }
        if (!foundCycle) {
            std::fprintf(stderr, "FAIL: validate() did not flag circular struct dependency NodeA <-> NodeB\n");
            return 1;
        }
        machine.types.clear();

        // 7. Check 19: Transition payloadType validation
        machine.types.push_back(app::StructDefinition{
            .id = 2,
            .name = QStringLiteral("ValidPayload"),
            .fields = {app::StructField{.name = QStringLiteral("x"), .type = app::FieldType::Int}},
        });
        // Transition with valid struct payload
        machine.transitions.push_back(app::Transition{
            .id = 10,
            .from = 1,
            .to = 1,
            .event = QStringLiteral("EV1"),
            .payloadType = QStringLiteral("ValidPayload"),
        });
        // Transition with valid built-in payload
        machine.transitions.push_back(app::Transition{
            .id = 11,
            .from = 1,
            .to = 1,
            .event = QStringLiteral("EV2"),
            .payloadType = QStringLiteral("Int"),
        });
        // Transition with invalid payload
        machine.transitions.push_back(app::Transition{
            .id = 12,
            .from = 1,
            .to = 1,
            .event = QStringLiteral("EV3"),
            .payloadType = QStringLiteral("UnknownPayload"),
        });
        // Context variable with invalid custom type
        machine.context.push_back(app::ContextVariable{
            .id = 20,
            .name = QStringLiteral("badVar"),
            .type = app::ContextType::Object,
            .customTypeName = QStringLiteral("GhostType"),
        });
        // Context variable with valid custom type
        machine.context.push_back(app::ContextVariable{
            .id = 21,
            .name = QStringLiteral("goodVar"),
            .type = app::ContextType::Object,
            .customTypeName = QStringLiteral("ValidPayload"),
        });

        bool foundInvalidPayload = false;
        bool foundInvalidContextType = false;
        for (const app::Problem& p : app::validate(machine)) {
            if (p.severity == app::ProblemSeverity::Error) {
                if (p.text.contains(QStringLiteral("transition specifies unknown payloadType 'UnknownPayload'"))) {
                    foundInvalidPayload = true;
                }
                if (p.text.contains(QStringLiteral("context variable 'badVar' references unknown struct type 'GhostType'"))) {
                    foundInvalidContextType = true;
                }
            }
        }
        if (!foundInvalidPayload) {
            std::fprintf(stderr, "FAIL: validate() did not flag unknown transition payloadType 'UnknownPayload'\n");
            return 1;
        }
        if (!foundInvalidContextType) {
            std::fprintf(stderr, "FAIL: validate() did not flag context variable referencing unknown struct type 'GhostType'\n");
            return 1;
        }

        // Clean state with valid types should have zero errors
        machine.transitions.removeLast(); // remove EV3
        machine.context.removeFirst();     // remove badVar
        for (const app::Problem& p : app::validate(machine)) {
            if (p.severity == app::ProblemSeverity::Error) {
                std::fprintf(stderr, "FAIL: clean struct-context machine had unexpected error: %s\n",
                             qUtf8Printable(p.text));
                return 1;
            }
        }
    }

    // ---- 19b. Same-exact-event payloadType consistency ----
    {
        auto count19bErrors = [](const QVector<app::Problem>& problems) {
            int n = 0;
            for (const app::Problem& p : problems) {
                if (p.severity == app::ProblemSeverity::Error &&
                    p.text.contains(QStringLiteral("declares inconsistent payloadType"))) {
                    ++n;
                }
            }
            return n;
        };

        // (a) Two rows of event "Set" on DIFFERENT source states (no shared
        // check-5 group), payloadType Int vs Bool -> exactly one 19b Error,
        // transitionId on the first row (document order) that diverges.
        app::Machine machine;
        machine.name = QStringLiteral("SameEventPayloadMismatch");
        machine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        machine.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal});
        machine.initialStateId = 1;
        machine.transitions.push_back(app::Transition{
            .id = 10, .from = 1, .to = 2, .event = QStringLiteral("Set"), .payloadType = QStringLiteral("Int")});
        machine.transitions.push_back(app::Transition{
            .id = 11, .from = 2, .to = 1, .event = QStringLiteral("Set"), .payloadType = QStringLiteral("Bool")});
        machine.nextId = 12;

        const QVector<app::Problem> mismatchProblems = app::validate(machine);
        if (count19bErrors(mismatchProblems) != 1) {
            std::fprintf(stderr, "FAIL: validate() reported %d 19b error(s) for event 'Set' declaring Int and Bool, want exactly 1\n",
                         count19bErrors(mismatchProblems));
            return 1;
        }
        bool foundOnDivergingRow = false;
        for (const app::Problem& p : mismatchProblems) {
            if (p.severity == app::ProblemSeverity::Error &&
                p.text.contains(QStringLiteral("declares inconsistent payloadType"))) {
                foundOnDivergingRow = (p.transitionId == 11);
            }
        }
        if (!foundOnDivergingRow) {
            std::fprintf(stderr, "FAIL: 19b error's transitionId did not point at the first diverging row (id 11)\n");
            return 1;
        }

        // (b) Same shape, but "Int" vs "int" -- the four built-in scalar
        // names compare case-insensitively (model/machine.h's
        // builtInPayloadScalar) -- no 19b.
        app::Machine caseFold = machine;
        caseFold.name = QStringLiteral("SameEventPayloadCaseFold");
        caseFold.transitions[1].payloadType = QStringLiteral("int");
        if (count19bErrors(app::validate(caseFold)) != 0) {
            std::fprintf(stderr, "FAIL: validate() flagged 'Int' vs 'int' as a 19b payloadType mismatch\n");
            return 1;
        }

        // (c) All rows leave payloadType blank -- an untyped event is its
        // own consistent value ("no payload"), so no 19b either.
        app::Machine allBlank = machine;
        allBlank.name = QStringLiteral("SameEventPayloadAllBlank");
        allBlank.transitions[0].payloadType.clear();
        allBlank.transitions[1].payloadType.clear();
        if (count19bErrors(app::validate(allBlank)) != 0) {
            std::fprintf(stderr, "FAIL: validate() flagged an all-blank-payloadType event group as a 19b mismatch\n");
            return 1;
        }
    }

    // ---- 19c. Wildcard/blank-event rows may not declare a payloadType ----
    {
        auto count19cErrors = [](const QVector<app::Problem>& problems) {
            int n = 0;
            for (const app::Problem& p : problems) {
                if (p.severity == app::ProblemSeverity::Error && p.text.contains(QStringLiteral("to bind `event` to"))) {
                    ++n;
                }
            }
            return n;
        };

        // (d) A wildcard row ("data.*") declaring payloadType Int -- it can
        // match several distinct exact events, so there is no single value
        // to bind `event` to.
        app::Machine wildcardPayload;
        wildcardPayload.name = QStringLiteral("WildcardPayload");
        wildcardPayload.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        wildcardPayload.initialStateId = 1;
        wildcardPayload.transitions.push_back(app::Transition{
            .id = 10, .from = 1, .to = 1, .event = QStringLiteral("data.*"), .payloadType = QStringLiteral("Int")});
        wildcardPayload.nextId = 11;
        if (count19cErrors(app::validate(wildcardPayload)) != 1) {
            std::fprintf(stderr, "FAIL: validate() did not flag exactly one 19c error for typed wildcard row 'data.*'\n");
            return 1;
        }

        // (e) An `always` row (blank event) declaring payloadType Int -- it
        // matches no event at all, so again nothing to bind `event` to.
        app::Machine alwaysPayload;
        alwaysPayload.name = QStringLiteral("AlwaysPayload");
        alwaysPayload.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        alwaysPayload.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Final});
        alwaysPayload.initialStateId = 1;
        alwaysPayload.transitions.push_back(app::Transition{
            .id = 10, .from = 1, .to = 2, .always = true, .payloadType = QStringLiteral("Int")});
        alwaysPayload.nextId = 11;
        if (count19cErrors(app::validate(alwaysPayload)) != 1) {
            std::fprintf(stderr, "FAIL: validate() did not flag exactly one 19c error for a typed 'always' row\n");
            return 1;
        }
    }

    // ---- 19d. raise() of a payload event ----
    {
        auto count19dErrors = [](const QVector<app::Problem>& problems) {
            int n = 0;
            for (const app::Problem& p : problems) {
                if (p.severity == app::ProblemSeverity::Error &&
                    p.text.contains(QStringLiteral("raise carries no payload"))) {
                    ++n;
                }
            }
            return n;
        };

        // (a) A transition action raises "Set", which another (non-invoke)
        // transition declares payloadType Int for; raise() carries no payload,
        // so there is no value to bind `event` to.
        app::Machine transitionRaise;
        transitionRaise.name = QStringLiteral("RaisePayloadFromTransition");
        transitionRaise.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        transitionRaise.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal});
        transitionRaise.initialStateId = 1;
        transitionRaise.transitions.push_back(app::Transition{
            .id = 10, .from = 1, .to = 1, .event = QStringLiteral("Go"), .action = QStringLiteral("raise(Set)")});
        transitionRaise.transitions.push_back(app::Transition{
            .id = 11, .from = 1, .to = 2, .event = QStringLiteral("Set"), .payloadType = QStringLiteral("Int")});
        transitionRaise.nextId = 12;
        const QVector<app::Problem> transitionRaiseProblems = app::validate(transitionRaise);
        if (count19dErrors(transitionRaiseProblems) != 1) {
            std::fprintf(stderr,
                         "FAIL: validate() did not report exactly one 19d error for raise() of a payload event from a "
                         "transition action\n");
            return 1;
        }
        bool transitionScoped = false;
        for (const app::Problem& p : transitionRaiseProblems) {
            if (p.severity == app::ProblemSeverity::Error && p.text.contains(QStringLiteral("raise carries no payload"))) {
                transitionScoped = (p.transitionId == 10 && p.stateId == 0);
            }
        }
        if (!transitionScoped) {
            std::fprintf(stderr, "FAIL: 19d error from a transition action was not scoped to transitionId=10, stateId=0\n");
            return 1;
        }

        // (b) The same raise, from an ENTRY action instead -- stateId names
        // the state, transitionId stays 0.
        app::Machine entryRaise;
        entryRaise.name = QStringLiteral("RaisePayloadFromEntry");
        entryRaise.states.push_back(app::State{.id = 1,
                                                .name = QStringLiteral("A"),
                                                .kind = app::StateKind::Normal,
                                                .entryActions = {QStringLiteral("raise(Set)")}});
        entryRaise.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal});
        entryRaise.initialStateId = 1;
        entryRaise.transitions.push_back(app::Transition{
            .id = 10, .from = 1, .to = 2, .event = QStringLiteral("Set"), .payloadType = QStringLiteral("Int")});
        entryRaise.nextId = 11;
        const QVector<app::Problem> entryRaiseProblems = app::validate(entryRaise);
        if (count19dErrors(entryRaiseProblems) != 1) {
            std::fprintf(stderr,
                         "FAIL: validate() did not report exactly one 19d error for raise() of a payload event from an "
                         "entry action\n");
            return 1;
        }
        bool entryScoped = false;
        for (const app::Problem& p : entryRaiseProblems) {
            if (p.severity == app::ProblemSeverity::Error && p.text.contains(QStringLiteral("raise carries no payload"))) {
                entryScoped = (p.stateId == 1 && p.transitionId == 0);
            }
        }
        if (!entryScoped) {
            std::fprintf(stderr, "FAIL: 19d error from an entry action was not scoped to stateId=1, transitionId=0\n");
            return 1;
        }

        // (c) raise() of a payload-FREE event -- no 19d at all.
        app::Machine payloadFreeRaise;
        payloadFreeRaise.name = QStringLiteral("RaisePayloadFreeEvent");
        payloadFreeRaise.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        payloadFreeRaise.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal});
        payloadFreeRaise.initialStateId = 1;
        payloadFreeRaise.transitions.push_back(app::Transition{
            .id = 10, .from = 1, .to = 1, .event = QStringLiteral("Go"), .action = QStringLiteral("raise(Ping)")});
        payloadFreeRaise.transitions.push_back(
            app::Transition{.id = 11, .from = 1, .to = 2, .event = QStringLiteral("Ping")});  // no payloadType
        payloadFreeRaise.nextId = 12;
        if (count19dErrors(app::validate(payloadFreeRaise)) != 0) {
            std::fprintf(stderr, "FAIL: validate() flagged raise() of a payload-free event as a 19d error\n");
            return 1;
        }
    }

    // ---- 19b/19c exclusions: an onDone row's reserved payload is not an event payload ----
    {
        // (f) Two onDone rows off the SAME invocation declaring DIFFERENT
        // payloadType values on the shared "done.invoke.<id>" text. That
        // completion marker is not a domain event (its payload comes from the
        // invocation's outputType), so no 19b conflict.
        app::Machine invokeMachine;
        invokeMachine.name = QStringLiteral("OnDoneNotEventPayload");
        invokeMachine.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("Fetching"), .invokeSrc = QStringLiteral("fetchThing")});
        invokeMachine.states.push_back(app::State{.id = 2, .name = QStringLiteral("Done"), .kind = app::StateKind::Final});
        invokeMachine.initialStateId = 1;
        invokeMachine.transitions.push_back(app::Transition{.id = 10,
                                                              .from = 1,
                                                              .to = 2,
                                                              .event = QStringLiteral("done.invoke.fetchThing"),
                                                              .guard = QStringLiteral("proceedA"),
                                                              .payloadType = QStringLiteral("Int")});
        invokeMachine.transitions.push_back(app::Transition{.id = 11,
                                                              .from = 1,
                                                              .to = 2,
                                                              .event = QStringLiteral("done.invoke.fetchThing"),
                                                              .guard = QStringLiteral("proceedB"),
                                                              .payloadType = QStringLiteral("Bool")});
        invokeMachine.nextId = 12;

        int found19bOnInvoke = 0;
        for (const app::Problem& p : app::validate(invokeMachine)) {
            if (p.severity == app::ProblemSeverity::Error &&
                p.text.contains(QStringLiteral("declares inconsistent payloadType"))) {
                ++found19bOnInvoke;
            }
        }
        if (found19bOnInvoke != 0) {
            std::fprintf(stderr, "FAIL: validate() flagged a 19b conflict on an onDone row's reserved output payload\n");
            return 1;
        }
        // ...but a payloadType on a completion row is dead data in its own
        // right: check 19e flags each such row exactly once.
        QSet<quint64> flagged19e;
        for (const app::Problem& p : app::validate(invokeMachine)) {
            if (p.severity == app::ProblemSeverity::Error &&
                p.text.contains(QStringLiteral("onDone/onError transition declares payloadType"))) {
                flagged19e.insert(p.transitionId);
            }
        }
        if (flagged19e != QSet<quint64>{10, 11}) {
            std::fprintf(stderr, "FAIL: validate() did not flag both onDone rows' payloadType with check 19e\n");
            return 1;
        }

        // (g) Hierarchical machine: compound parent P with two children C1/
        // C2; a row on the CHILD and a row on the PARENT (a bubbling
        // fallback, canvas-interaction's "Session" shape) share one exact
        // event "Set", both typed Int -- consistent across hierarchy depth,
        // so the machine is entirely clean.
        app::Machine hier;
        hier.name = QStringLiteral("HierarchicalSameEventPayloadClean");
        hier.states.push_back(
            app::State{.id = 1, .name = QStringLiteral("P"), .kind = app::StateKind::Normal, .initialChildId = 2});
        hier.states.push_back(
            app::State{.id = 2, .name = QStringLiteral("C1"), .kind = app::StateKind::Normal, .parentId = 1});
        hier.states.push_back(
            app::State{.id = 3, .name = QStringLiteral("C2"), .kind = app::StateKind::Normal, .parentId = 1});
        hier.initialStateId = 2;  // starts nested in C1 -- check 3's hierarchy closure reaches P too
        hier.transitions.push_back(app::Transition{
            .id = 10, .from = 2, .to = 3, .event = QStringLiteral("Set"), .payloadType = QStringLiteral("Int")});  // C1's own handler
        hier.transitions.push_back(app::Transition{
            .id = 11, .from = 1, .to = 3, .event = QStringLiteral("Set"), .payloadType = QStringLiteral("Int")});  // P's bubbling fallback
        hier.nextId = 12;
        const QVector<app::Problem> hierProblems = app::validate(hier);
        if (!hierProblems.isEmpty()) {
            std::fprintf(stderr,
                         "FAIL: validate() reported %d problem(s) on a hierarchical machine with a consistent same-event payload type:\n",
                         static_cast<int>(hierProblems.size()));
            for (const app::Problem& p : hierProblems) {
                std::fprintf(stderr, "  - %s\n", qUtf8Printable(p.text));
            }
            return 1;
        }
    }

    // ---- Reference machines clean validation sweep (src/machines/*.sdm) ----
    {
        const QDir machinesDir(QStringLiteral("src/machines"));
        const QStringList sdmFiles = machinesDir.entryList(QStringList() << QStringLiteral("*.sdm"), QDir::Files, QDir::Name);
        if (sdmFiles.isEmpty()) {
            std::fprintf(stderr, "FAIL: no .sdm reference machines found in src/machines/\n");
            return 1;
        }
        for (const QString& fileName : sdmFiles) {
            const QString fullPath = machinesDir.filePath(fileName);
            app::Machine refMachine;
            QString loadErr;
            if (!app::loadMachine(fullPath, &refMachine, &loadErr)) {
                std::fprintf(stderr, "FAIL: could not load reference machine '%s': %s\n",
                             qUtf8Printable(fullPath), qUtf8Printable(loadErr));
                return 1;
            }
            const QVector<app::Problem> refProblems = app::validate(refMachine);
            for (const app::Problem& p : refProblems) {
                if (p.severity == app::ProblemSeverity::Error) {
                    std::fprintf(stderr, "FAIL: reference machine '%s' has validation Error: %s\n",
                                 qUtf8Printable(fullPath), qUtf8Printable(p.text));
                    return 1;
                }
            }
        }
    }

    // ---- 20. Dead-end / Deadlock state detection ----
    {
        // 1) Atomic state with no outgoing transitions and not Final -> Warning
        app::Machine deadlockMachine;
        deadlockMachine.name = QStringLiteral("DeadlockTest");
        deadlockMachine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        deadlockMachine.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal});
        deadlockMachine.transitions.push_back(app::Transition{.id = 3, .from = 1, .to = 2, .event = QStringLiteral("go")});
        deadlockMachine.nextId = 4;
        deadlockMachine.initialStateId = 1;
        const QVector<app::Problem> dlProblems = app::validate(deadlockMachine);
        bool foundDl = false;
        for (const app::Problem& p : dlProblems) {
            if (p.severity == app::ProblemSeverity::Warning && p.stateId == 2 &&
                p.text.contains(QStringLiteral("dead-end state"))) {
                foundDl = true;
                break;
            }
        }
        if (!foundDl) {
            std::fprintf(stderr, "FAIL: validate() did not flag dead-end state 2\n");
            return 1;
        }

        // 2) Marking state 2 as Final clears the warning
        deadlockMachine.states[1].kind = app::StateKind::Final;
        const QVector<app::Problem> finalProblems = app::validate(deadlockMachine);
        for (const app::Problem& p : finalProblems) {
            if (p.text.contains(QStringLiteral("dead-end state"))) {
                std::fprintf(stderr, "FAIL: validate() flagged Final state as dead-end: %s\n", qUtf8Printable(p.text));
                return 1;
            }
        }

        // 3) Parent compound state having an outgoing transition clears dead-end on child
        app::Machine hierDeadlock;
        hierDeadlock.name = QStringLiteral("HierDeadlockTest");
        hierDeadlock.states.push_back(app::State{.id = 1, .name = QStringLiteral("RootCompound"), .kind = app::StateKind::Normal});
        hierDeadlock.states.push_back(app::State{.id = 2, .name = QStringLiteral("ChildA"), .kind = app::StateKind::Normal, .parentId = 1});
        hierDeadlock.states.push_back(app::State{.id = 3, .name = QStringLiteral("Done"), .kind = app::StateKind::Final});
        hierDeadlock.states[0].initialChildId = 2;
        hierDeadlock.transitions.push_back(app::Transition{.id = 4, .from = 1, .to = 3, .event = QStringLiteral("TIMEOUT")});
        hierDeadlock.nextId = 5;
        hierDeadlock.initialStateId = 1;
        const QVector<app::Problem> hierProblems = app::validate(hierDeadlock);
        for (const app::Problem& p : hierProblems) {
            if (p.text.contains(QStringLiteral("dead-end state"))) {
                std::fprintf(stderr, "FAIL: validate() flagged child state as dead-end when parent has transition: %s\n", qUtf8Printable(p.text));
                return 1;
            }
        }

        // 4) Machine with root transition clears dead-end on all states
        app::Machine rootDlMachine;
        rootDlMachine.name = QStringLiteral("RootDlTest");
        rootDlMachine.states.push_back(app::State{.id = 1, .name = QStringLiteral("A"), .kind = app::StateKind::Normal});
        rootDlMachine.states.push_back(app::State{.id = 2, .name = QStringLiteral("B"), .kind = app::StateKind::Normal});
        rootDlMachine.transitions.push_back(app::Transition{.id = 3, .from = 0, .to = 1, .event = QStringLiteral("RESET")});
        rootDlMachine.nextId = 4;
        rootDlMachine.initialStateId = 1;
        const QVector<app::Problem> rootDlProblems = app::validate(rootDlMachine);
        for (const app::Problem& p : rootDlProblems) {
            if (p.text.contains(QStringLiteral("dead-end state"))) {
                std::fprintf(stderr, "FAIL: validate() flagged dead-end on machine with root transition: %s\n", qUtf8Printable(p.text));
                return 1;
            }
        }
    }

    std::printf("PASS: state-designer validator smoke (infra/machine_validator.h checks incl. root rules + multi-target rules + clean reference machine sweep)\n");
    return 0;
}
