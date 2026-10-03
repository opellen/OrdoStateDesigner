// --smoke integration sections 8-14: validator check 10 and rename cascade (8),
// logic inventory (9), hook rename cascade (10), parseAssignForm and validator
// check 11 (11), emitCpp (12), invoke actor payload (13), raise form (14).

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
// parseAssignForm()'s three outcomes, each its own helper so a FAIL line
// names exactly which contract broke rather than a generic "did not match".
// The RHS is asserted UNPARSED (see machine_validator.h check 11).
bool expectAssignOk(const QString& source, const QString& wantedTarget, const QString& wantedValueSource) {
    const app::expr::AssignForm form = app::expr::parseAssignForm(source);
    if (!form.ok || !form.message.isEmpty() || form.target != wantedTarget || form.valueSource != wantedValueSource) {
        std::fprintf(stderr,
                     "FAIL: parseAssignForm(\"%s\") = ok=%s target=\"%s\" value=\"%s\" message=\"%s\" (want ok "
                     "target=\"%s\" value=\"%s\")\n",
                     qUtf8Printable(source), form.ok ? "true" : "false", qUtf8Printable(form.target),
                     qUtf8Printable(form.valueSource), qUtf8Printable(form.message), qUtf8Printable(wantedTarget),
                     qUtf8Printable(wantedValueSource));
        return false;
    }
    return true;
}

// Not-an-assign with a BLANK message -- an ordinary named action hook, per
// the struct's own contract (see expression.h's AssignForm comment).
bool expectNotAnAssign(const QString& source) {
    const app::expr::AssignForm form = app::expr::parseAssignForm(source);
    if (form.ok || !form.message.isEmpty()) {
        std::fprintf(stderr,
                     "FAIL: parseAssignForm(\"%s\") = ok=%s message=\"%s\" (want ok=false with a BLANK message -- a "
                     "named action hook)\n",
                     qUtf8Printable(source), form.ok ? "true" : "false", qUtf8Printable(form.message));
        return false;
    }
    return true;
}

// Not-an-assign WITH a message -- a malformed assign, told apart from the
// hook case above by the message being non-blank.
bool expectMalformedAssign(const QString& source) {
    const app::expr::AssignForm form = app::expr::parseAssignForm(source);
    if (form.ok || form.message.isEmpty()) {
        std::fprintf(stderr,
                     "FAIL: parseAssignForm(\"%s\") = ok=%s message=\"%s\" (want ok=false WITH a message -- a "
                     "malformed assign)\n",
                     qUtf8Printable(source), form.ok ? "true" : "false", qUtf8Printable(form.message));
        return false;
    }
    return true;
}


// Section 12 helper. The FAIL line carries the source, the requested
// contextExpr, and both strings in full (no line number), so a mismatch is
// attributable from the log alone even when stdout is block-buffered.
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



// Section 9 helpers: a one-line-per-row dump, so a single expectRows() call
// asserts name, class, and both id lists together and the FAIL line shows
// every row that diverged.
QString logicRowClassLabel(app::LogicRowClass rowClass) {
    switch (rowClass) {
        case app::LogicRowClass::Hook: return QStringLiteral("Hook");
        case app::LogicRowClass::InlineExpression: return QStringLiteral("InlineExpression");
        case app::LogicRowClass::Assign: return QStringLiteral("Assign");
        case app::LogicRowClass::Raise: return QStringLiteral("Raise");
        case app::LogicRowClass::SendTo: return QStringLiteral("SendTo");
        case app::LogicRowClass::SendParent: return QStringLiteral("SendParent");
    }
    return QStringLiteral("?");
}

QString idList(const QVector<quint64>& ids) {
    QStringList parts;
    for (quint64 id : ids) {
        parts << QString::number(id);
    }
    return parts.join(QStringLiteral(","));
}

QString dumpLogicRow(const app::LogicRow& row) {
    return row.name + QStringLiteral("|") + logicRowClassLabel(row.rowClass) + QStringLiteral("|t:") +
           idList(row.transitionIds) + QStringLiteral("|s:") + idList(row.stateIds);
}

QString dumpLogicRows(const QVector<app::LogicRow>& rows) {
    QStringList parts;
    for (const app::LogicRow& row : rows) {
        parts << dumpLogicRow(row);
    }
    return parts.join(QStringLiteral("; "));
}

// `label` is a distinctive per-fixture tag, not a line number: stdout is
// block-buffered when redirected, so a FAIL must name the fixture in its text.
bool expectLogicRows(const QString& label, const QVector<app::LogicRow>& actual, const QStringList& wantedRows) {
    const QString actualDump = dumpLogicRows(actual);
    const QString wantedDump = wantedRows.join(QStringLiteral("; "));
    if (actualDump != wantedDump) {
        std::fprintf(stderr, "FAIL: %s = [%s] (want [%s])\n", qUtf8Printable(label), qUtf8Printable(actualDump),
                     qUtf8Printable(wantedDump));
        return false;
    }
    return true;
}

}  // namespace

int runExpressionIntegrationSmoke() {
    using app::expr::NodeKind;
    using app::expr::ParseResult;

    int failures = 0;
    // ---- 8a. validator check 10 over a hand-built Machine -------------------
    // check10FindingCount isolates check 10's findings by MESSAGE MARKER
    // ("does not parse:"/"is ill-typed:"), not by transitionId: check 6 also
    // sanitizes every non-blank guard, so a guard could pick up an unrelated
    // check-6 finding on the same transitionId.
    {
        auto check10FindingCount = [](const app::Machine& machine, quint64 transitionId) {
            int count = 0;
            for (const app::Problem& problem : app::validate(machine)) {
                if (problem.transitionId != transitionId || problem.severity != app::ProblemSeverity::Error) {
                    continue;
                }
                if (problem.text.contains(QStringLiteral("does not parse:")) ||
                    problem.text.contains(QStringLiteral("is ill-typed:"))) {
                    ++count;
                }
            }
            return count;
        };
        auto expectCheck10Count = [&](const app::Machine& machine, quint64 transitionId, int wanted) {
            const int actual = check10FindingCount(machine, transitionId);
            if (actual != wanted) {
                std::fprintf(stderr, "FAIL: check 10 reported %d finding(s) for transition %llu (want %d)\n", actual,
                             static_cast<unsigned long long>(transitionId), wanted);
                return false;
            }
            return true;
        };

        // Machine A: EMPTY context -- blank/bare-identifier guards skip check
        // 10; an unparseable guard is one Error; an unknown identifier is
        // ill-typed even with nothing in the schema (typeCheck does not
        // short-circuit on an empty Machine::context).
        app::Machine machineA;
        machineA.name = QStringLiteral("GuardCheckEmptyContext");
        machineA.states = {app::State{.id = 1, .name = QStringLiteral("A")},
                            app::State{.id = 2, .name = QStringLiteral("B")}};
        machineA.initialStateId = 1;
        machineA.nextId = 100;
        machineA.transitions = {
            app::Transition{.id = 10, .from = 1, .to = 2, .event = QStringLiteral("E1"), .guard = QString()},
            app::Transition{
                .id = 11, .from = 1, .to = 2, .event = QStringLiteral("E2"), .guard = QStringLiteral("ready")},
            app::Transition{
                .id = 12, .from = 1, .to = 2, .event = QStringLiteral("E3"), .guard = QStringLiteral("count > ")},
            app::Transition{
                .id = 13, .from = 1, .to = 2, .event = QStringLiteral("E4"), .guard = QStringLiteral("missing > 3")},
        };
        failures += !expectCheck10Count(machineA, 10, 0);  // blank -- no guard at all
        failures += !expectCheck10Count(machineA, 11, 0);  // bare identifier -- a named hook
        failures += !expectCheck10Count(machineA, 12, 1);  // unparseable -- one Error
        failures += !expectCheck10Count(machineA, 13, 1);  // unknown identifier, even against an empty schema

        // The unparseable transition's finding must carry the parse marker
        // (not the type-check one) and name the offending transition.
        bool sawParseMarker = false;
        for (const app::Problem& problem : app::validate(machineA)) {
            if (problem.transitionId == 12 && problem.text.contains(QStringLiteral("does not parse:"))) {
                sawParseMarker = true;
            }
        }
        if (!sawParseMarker) {
            std::fprintf(stderr, "FAIL: check 10 did not report a \"does not parse:\" finding for transition 12\n");
            ++failures;
        }

        // Machine B: a REAL schema -- a type mismatch (Int vs String) is
        // ill-typed even though every identifier resolves; a well-typed guard
        // against the SAME schema produces nothing at all.
        app::Machine machineB;
        machineB.name = QStringLiteral("GuardCheckRealSchema");
        machineB.states = {app::State{.id = 1, .name = QStringLiteral("A")},
                            app::State{.id = 2, .name = QStringLiteral("B")}};
        machineB.initialStateId = 1;
        machineB.context = {app::ContextVariable{.id = 50,
                                                   .name = QStringLiteral("count"),
                                                   .type = app::ContextType::Int,
                                                   .initialValue = QStringLiteral("0")},
                             app::ContextVariable{.id = 51,
                                                   .name = QStringLiteral("role"),
                                                   .type = app::ContextType::String,
                                                   .initialValue = QStringLiteral("guest")}};
        machineB.nextId = 100;
        machineB.transitions = {app::Transition{.id = 20,
                                                  .from = 1,
                                                  .to = 2,
                                                  .event = QStringLiteral("E1"),
                                                  .guard = QStringLiteral("count == role")},
                                 app::Transition{.id = 21,
                                                  .from = 1,
                                                  .to = 2,
                                                  .event = QStringLiteral("E2"),
                                                  .guard = QStringLiteral("count > 3")}};
        failures += !expectCheck10Count(machineB, 20, 1);  // Int vs String -- ill-typed against the real schema
        failures += !expectCheck10Count(machineB, 21, 0);  // well-typed against the same schema -- no finding
    }
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 8a (validator check 10) had %d failure(s)\n", failures);
        return 1;
    }

    // ---- 8b. the rename cascade, end to end through a kernel ----------------
    // RenameContextVariableCommand rewrites every referencing guard, and
    // rename + rewrites are ONE undo step.
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        kernel.registerAgent(std::make_shared<app::UndoStore>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto undo = kernel.agentAs<app::UndoStore>(app::UndoStore::kName);
        if (!doc || !undo) {
            std::fprintf(stderr, "FAIL: rename-cascade phase agents did not all register under kName\n");
            return 1;
        }
        registerUndoPhaseCommands(kernel);

        const quint64 kVar = doc->machine().nextId;
        kernel.send(app::events::AddContextVariableRequested{});
        kernel.send(app::events::RenameContextVariableRequested{.id = kVar, .name = QStringLiteral("counter")});

        const quint64 kStateA = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 kStateB = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});

        // Two transitions reference `counter`; a third (a bare-identifier
        // hook) does not -- the cascade must rewrite exactly the first two
        // and leave the third byte-for-byte untouched.
        const quint64 kT1 = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
        kernel.send(app::events::SetTransitionGuardRequested{.id = kT1, .guard = QStringLiteral("counter > 3")});
        const quint64 kT2 = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
        kernel.send(app::events::SetTransitionGuardRequested{.id = kT2, .guard = QStringLiteral("counter == 5")});
        const quint64 kT3 = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
        kernel.send(app::events::SetTransitionGuardRequested{.id = kT3, .guard = QStringLiteral("ready")});

        const app::Machine beforeRename = doc->machine();
        kernel.send(app::events::RenameContextVariableRequested{.id = kVar, .name = QStringLiteral("total")});

        if (doc->findContextVariable(kVar)->name != QStringLiteral("total") ||
            doc->findTransition(kT1)->guard != QStringLiteral("total > 3") ||
            doc->findTransition(kT2)->guard != QStringLiteral("total == 5") ||
            doc->findTransition(kT3)->guard != QStringLiteral("ready")) {
            std::fprintf(stderr,
                          "FAIL: rename cascade did not rewrite exactly the two referencing guards (var=\"%s\" "
                          "t1=\"%s\" t2=\"%s\" t3=\"%s\")\n",
                          qUtf8Printable(doc->findContextVariable(kVar)->name),
                          qUtf8Printable(doc->findTransition(kT1)->guard),
                          qUtf8Printable(doc->findTransition(kT2)->guard),
                          qUtf8Printable(doc->findTransition(kT3)->guard));
            return 1;
        }

        // ONE undo restores the name AND every rewritten guard together: the
        // undo capture records one Transaction per execute() call, however
        // many choke-point calls happen inside it.
        kernel.send(app::events::UndoRequested{});
        if (!(doc->machine() == beforeRename)) {
            std::fprintf(stderr,
                          "FAIL: ONE undo of the rename cascade did not deep-equal the pre-rename machine (name "
                          "and/or a guard was not restored together)\n");
            return 1;
        }
        kernel.send(app::events::RedoRequested{});
        if (doc->findContextVariable(kVar)->name != QStringLiteral("total") ||
            doc->findTransition(kT1)->guard != QStringLiteral("total > 3") ||
            doc->findTransition(kT2)->guard != QStringLiteral("total == 5") ||
            doc->findTransition(kT3)->guard != QStringLiteral("ready")) {
            std::fprintf(stderr, "FAIL: ONE redo of the rename cascade did not reproduce the rename and every "
                                  "rewritten guard together\n");
            return 1;
        }
    }

    // ---- 9. infra/logic_inventory.h: the pure guard/action derivation -------

    // Item 1: an empty machine yields empty guards and actions.
    {
        app::Machine machine;
        machine.name = QStringLiteral("Empty");
        const app::LogicInventory inv = app::inventory(machine);
        failures += !expectLogicRows(QStringLiteral("logic inventory: empty machine, guards"), inv.guards, {});
        failures += !expectLogicRows(QStringLiteral("logic inventory: empty machine, actions"), inv.actions, {});
    }

    // Item 2: blank guards and blank action entries (including whitespace-only
    // strings, per the trim decision) contribute nothing.
    {
        app::Machine machine;
        machine.name = QStringLiteral("BlankEntries");
        machine.states = {app::State{.id = 1,
                                      .name = QStringLiteral("A"),
                                      .entryActions = {QString(), QStringLiteral("   ")},
                                      .exitActions = {QStringLiteral("")}},
                          app::State{.id = 2, .name = QStringLiteral("B")}};
        machine.transitions = {app::Transition{
            .id = 10, .from = 1, .to = 2, .event = QStringLiteral("E"), .guard = QString(), .action = QStringLiteral("   ")}};
        machine.nextId = 100;
        const app::LogicInventory inv = app::inventory(machine);
        failures += !expectLogicRows(QStringLiteral("logic inventory: blank guard contributes nothing"), inv.guards, {});
        failures += !expectLogicRows(QStringLiteral("logic inventory: blank/whitespace-only action entries contribute nothing"),
                                     inv.actions, {});
    }

    // Item 3: a bare-identifier guard classifies Hook; an expression guard
    // classifies InlineExpression.
    {
        app::Machine machine;
        machine.name = QStringLiteral("GuardClassification");
        machine.states = {app::State{.id = 1, .name = QStringLiteral("A")}, app::State{.id = 2, .name = QStringLiteral("B")}};
        machine.transitions = {
            app::Transition{.id = 10, .from = 1, .to = 2, .event = QStringLiteral("E1"), .guard = QStringLiteral("ready")},
            app::Transition{
                .id = 11, .from = 1, .to = 2, .event = QStringLiteral("E2"), .guard = QStringLiteral("count > 3")}};
        machine.nextId = 100;
        const app::LogicInventory inv = app::inventory(machine);
        failures += !expectLogicRows(
            QStringLiteral("logic inventory: bare-identifier guard is Hook, expression guard is InlineExpression"),
            inv.guards,
            {QStringLiteral("ready|Hook|t:10|s:"), QStringLiteral("count > 3|InlineExpression|t:11|s:")});
    }

    // Item 4: the shared action vocabulary -- one name used as a transition action, a state's entry action, AND
    // another state's exit action merges into ONE row carrying the
    // transition id and both state ids.
    {
        app::Machine machine;
        machine.name = QStringLiteral("SharedActionVocabulary");
        machine.states = {app::State{.id = 1, .name = QStringLiteral("A"), .entryActions = {QStringLiteral("notify")}},
                          app::State{.id = 2, .name = QStringLiteral("B"), .exitActions = {QStringLiteral("notify")}}};
        machine.transitions = {app::Transition{
            .id = 10, .from = 1, .to = 2, .event = QStringLiteral("E"), .action = QStringLiteral("notify")}};
        machine.nextId = 100;
        const app::LogicInventory inv = app::inventory(machine);
        failures += !expectLogicRows(
            QStringLiteral("logic inventory: transition action + entry action + exit action merge into one row"),
            inv.actions, {QStringLiteral("notify|Hook|t:10|s:1,2")});
    }

    // Item 5: two transitions sharing a guard merge into one row with both
    // transition ids in document order. The second guard is written with
    // surrounding whitespace to prove the merge key is TRIMMED text.
    {
        app::Machine machine;
        machine.name = QStringLiteral("SharedGuard");
        machine.states = {app::State{.id = 1, .name = QStringLiteral("A")}, app::State{.id = 2, .name = QStringLiteral("B")}};
        machine.transitions = {
            app::Transition{
                .id = 10, .from = 1, .to = 2, .event = QStringLiteral("E1"), .guard = QStringLiteral("count > 3")},
            app::Transition{
                .id = 11, .from = 1, .to = 2, .event = QStringLiteral("E2"), .guard = QStringLiteral("  count > 3  ")}};
        machine.nextId = 100;
        const app::LogicInventory inv = app::inventory(machine);
        failures += !expectLogicRows(QStringLiteral("logic inventory: two transitions sharing a guard (modulo whitespace) merge"),
                                     inv.guards, {QStringLiteral("count > 3|InlineExpression|t:10,11|s:")});
    }

    // Item 6: two DIFFERENT guards that both reference the same context
    // variable stay two rows -- merging is by source text, not by referenced
    // identifier.
    {
        app::Machine machine;
        machine.name = QStringLiteral("DifferentGuardsSameVariable");
        machine.states = {app::State{.id = 1, .name = QStringLiteral("A")}, app::State{.id = 2, .name = QStringLiteral("B")}};
        machine.transitions = {
            app::Transition{
                .id = 10, .from = 1, .to = 2, .event = QStringLiteral("E1"), .guard = QStringLiteral("count > 3")},
            app::Transition{
                .id = 11, .from = 1, .to = 2, .event = QStringLiteral("E2"), .guard = QStringLiteral("count < 3")}};
        machine.nextId = 100;
        const app::LogicInventory inv = app::inventory(machine);
        failures += !expectLogicRows(
            QStringLiteral("logic inventory: different guard text over one context variable stays two rows"), inv.guards,
            {QStringLiteral("count > 3|InlineExpression|t:10|s:"), QStringLiteral("count < 3|InlineExpression|t:11|s:")});
    }

    // Item 7: determinism -- first-appearance DOCUMENT order (vector
    // position), never id order. Ids are out of vector order (99 then 1 for
    // states, 50 then 2 for transitions) so a by-id sort gives the opposite
    // sequence. Real machines hit this too: a restored element's id can be
    // larger than that of elements created after it.
    {
        app::Machine machine;
        machine.name = QStringLiteral("DeterministicOrder");
        machine.states = {app::State{.id = 99, .name = QStringLiteral("A")}, app::State{.id = 1, .name = QStringLiteral("B")}};
        machine.transitions = {
            app::Transition{.id = 50, .from = 99, .to = 1, .event = QStringLiteral("E1"), .guard = QStringLiteral("zeta")},
            app::Transition{.id = 2, .from = 99, .to = 1, .event = QStringLiteral("E2"), .guard = QStringLiteral("alpha")}};
        machine.nextId = 200;
        const app::LogicInventory inv = app::inventory(machine);
        failures += !expectLogicRows(QStringLiteral("logic inventory: row order follows vector position, not id order"),
                                     inv.guards, {QStringLiteral("zeta|Hook|t:50|s:"), QStringLiteral("alpha|Hook|t:2|s:")});
    }

    // Item 8: an expression guard and a hook guard with overlapping text
    // (`ready` vs `ready == true`) stay two rows with the right classes --
    // proof the merge key is exact-string, not a prefix or substring match.
    {
        app::Machine machine;
        machine.name = QStringLiteral("OverlappingGuardText");
        machine.states = {app::State{.id = 1, .name = QStringLiteral("A")}, app::State{.id = 2, .name = QStringLiteral("B")}};
        machine.transitions = {
            app::Transition{.id = 10, .from = 1, .to = 2, .event = QStringLiteral("E1"), .guard = QStringLiteral("ready")},
            app::Transition{
                .id = 11, .from = 1, .to = 2, .event = QStringLiteral("E2"), .guard = QStringLiteral("ready == true")}};
        machine.nextId = 100;
        const app::LogicInventory inv = app::inventory(machine);
        failures += !expectLogicRows(
            QStringLiteral("logic inventory: `ready` and `ready == true` stay two rows with distinct classes"), inv.guards,
            {QStringLiteral("ready|Hook|t:10|s:"), QStringLiteral("ready == true|InlineExpression|t:11|s:")});
    }

    // ---- 9i. guards and actions do NOT share a classifier ------------------
    // Regression pin: `beginLogin()` rendered in the Logic panel as an inline
    // expression while the code generator emitted a pure-virtual hook for it.
    // An ACTION is a hook unless it is an assign (actionIsHook); only a GUARD
    // uses the bare-identifier rule (guardIsHook). Pinned from both sides with
    // the SAME text.
    {
        app::Machine machine;
        machine.name = QStringLiteral("Classifier Asymmetry");
        machine.context = {app::ContextVariable{
            .id = 1, .name = QStringLiteral("count"), .type = app::ContextType::Int, .initialValue = QStringLiteral("0")}};
        machine.states = {app::State{.id = 2, .name = QStringLiteral("A")}, app::State{.id = 3, .name = QStringLiteral("B")}};
        machine.initialStateId = 2;
        machine.transitions = {
            // Same text in both roles: a hook name carrying call parentheses.
            app::Transition{.id = 10,
                            .from = 2,
                            .to = 3,
                            .event = QStringLiteral("E1"),
                            .guard = QStringLiteral("beginLogin()"),
                            .action = QStringLiteral("beginLogin()")},
            app::Transition{
                .id = 11, .from = 3, .to = 2, .event = QStringLiteral("E2"), .action = QStringLiteral("count = 3")},
            app::Transition{
                .id = 12, .from = 3, .to = 2, .event = QStringLiteral("E3"), .action = QStringLiteral("count + 1 = 2")}};
        machine.nextId = 100;
        const app::LogicInventory inv = app::inventory(machine);
        failures += !expectLogicRows(
            QStringLiteral("logic inventory: `beginLogin()` is an Expr GUARD but a Hook ACTION (the asymmetry)"),
            inv.guards, {QStringLiteral("beginLogin()|InlineExpression|t:10|s:")});
        failures += !expectLogicRows(
            QStringLiteral("logic inventory: actions classify by assign-form -- hook, assign, malformed assign"),
            inv.actions,
            {QStringLiteral("beginLogin()|Hook|t:10|s:"), QStringLiteral("count = 3|Assign|t:11|s:"),
             // A MALFORMED assign reads as Assign, never Hook: the generator
             // emits no hook for it, and check 11 explains why.
             QStringLiteral("count + 1 = 2|Assign|t:12|s:")});
    }

    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 9 (logic_inventory) had %d failure(s)\n", failures);
        return 1;
    }

    // ---- 10. RenameGuardCommand/RenameActionCommand -- the guard/action HOOK
    // rename cascade. Each fixture is its own kernel (section 8b's template),
    // with a different rewrite rule: whole-string bare-identifier match, never
    // expr::renameIdentifierInSource. Each block returns 1 on the first broken
    // assertion, since later sends would run against an already-wrong document.

    // ---- 10a. guard rename: a hook shared by TWO transitions renames in BOTH,
    // in ONE undo step; an overlapping EXPRESSION guard (`ready == true`) and a
    // longer HOOK name (`readyState`) are left untouched (no substring match).
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        kernel.registerAgent(std::make_shared<app::UndoStore>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        if (!doc) {
            std::fprintf(stderr, "FAIL: guard-rename fixture: MachineDocAgent did not register under kName\n");
            return 1;
        }
        registerUndoPhaseCommands(kernel);

        const quint64 kStateA = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 kStateB = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});

        const quint64 kT1 = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
        kernel.send(app::events::SetTransitionGuardRequested{.id = kT1, .guard = QStringLiteral("ready")});
        const quint64 kT2 = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
        kernel.send(app::events::SetTransitionGuardRequested{.id = kT2, .guard = QStringLiteral("ready")});
        // kT3: an EXPRESSION guard that merely CONTAINS "ready".
        // kT4: a DIFFERENT hook whose name merely STARTS WITH "ready".
        // Neither is a whole-string bare-identifier match, so neither may move.
        const quint64 kT3 = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
        kernel.send(app::events::SetTransitionGuardRequested{.id = kT3, .guard = QStringLiteral("ready == true")});
        const quint64 kT4 = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
        kernel.send(app::events::SetTransitionGuardRequested{.id = kT4, .guard = QStringLiteral("readyState")});

        const app::Machine beforeRename = doc->machine();
        kernel.send(
            app::events::RenameGuardRequested{.before = QStringLiteral("ready"), .after = QStringLiteral("prepared")});

        if (doc->findTransition(kT1)->guard != QStringLiteral("prepared") ||
            doc->findTransition(kT2)->guard != QStringLiteral("prepared") ||
            doc->findTransition(kT3)->guard != QStringLiteral("ready == true") ||
            doc->findTransition(kT4)->guard != QStringLiteral("readyState")) {
            std::fprintf(
                stderr,
                "FAIL: RenameGuardCommand(\"ready\"->\"prepared\") = t1=\"%s\" t2=\"%s\" t3=\"%s\" t4=\"%s\" (want "
                "t1/t2 renamed to \"prepared\", t3/t4 byte-for-byte untouched)\n",
                qUtf8Printable(doc->findTransition(kT1)->guard), qUtf8Printable(doc->findTransition(kT2)->guard),
                qUtf8Printable(doc->findTransition(kT3)->guard), qUtf8Printable(doc->findTransition(kT4)->guard));
            return 1;
        }

        // ONE undo restores BOTH renamed guards together.
        kernel.send(app::events::UndoRequested{});
        if (!(doc->machine() == beforeRename)) {
            std::fprintf(stderr,
                          "FAIL: ONE undo of the guard-rename cascade did not deep-equal the pre-rename machine "
                          "(t1 and/or t2 was not restored together)\n");
            return 1;
        }
    }

    // ---- 10b. action rename: the ONE shared vocabulary -- a transition
    // action, one state's entry action, and ANOTHER state's exit action are
    // all rewritten together, in ONE undo step. ------------------------------
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        kernel.registerAgent(std::make_shared<app::UndoStore>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        if (!doc) {
            std::fprintf(stderr, "FAIL: action-rename fixture: MachineDocAgent did not register under kName\n");
            return 1;
        }
        registerUndoPhaseCommands(kernel);

        const quint64 kStateA = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 kStateB = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});
        kernel.send(app::events::SetEntryActionsRequested{.id = kStateA, .entryActions = {QStringLiteral("notify")}});
        kernel.send(app::events::SetExitActionsRequested{.id = kStateB, .exitActions = {QStringLiteral("notify")}});

        const quint64 kT = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
        kernel.send(app::events::SetTransitionActionRequested{.id = kT, .action = QStringLiteral("notify")});

        const app::Machine beforeRename = doc->machine();
        kernel.send(
            app::events::RenameActionRequested{.before = QStringLiteral("notify"), .after = QStringLiteral("alert")});

        if (doc->findTransition(kT)->action != QStringLiteral("alert") ||
            doc->findState(kStateA)->entryActions != QStringList{QStringLiteral("alert")} ||
            doc->findState(kStateB)->exitActions != QStringList{QStringLiteral("alert")}) {
            std::fprintf(stderr,
                          "FAIL: RenameActionCommand(\"notify\"->\"alert\") = transition=\"%s\" entry=[%s] exit=[%s] "
                          "(want all three renamed to \"alert\")\n",
                          qUtf8Printable(doc->findTransition(kT)->action),
                          qUtf8Printable(doc->findState(kStateA)->entryActions.join(QStringLiteral(","))),
                          qUtf8Printable(doc->findState(kStateB)->exitActions.join(QStringLiteral(","))));
            return 1;
        }

        // ONE undo restores the transition action AND both state action
        // lists together.
        kernel.send(app::events::UndoRequested{});
        if (!(doc->machine() == beforeRename)) {
            std::fprintf(stderr,
                          "FAIL: ONE undo of the action-rename cascade did not restore the transition action AND "
                          "both state action lists together\n");
            return 1;
        }
    }

    // ---- 10c. policy: a blank `after` is REFUSED; before == after, and a
    // blank `before`, are silent no-ops. None of the three pushes an undo
    // entry: unwinding exactly the setup edits lands back on the pre-setup
    // TOPOLOGY (sameTopology(), not operator==: undoing an ADD never rolls
    // Machine::nextId back) with canUndo() false. -----------------------------
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        kernel.registerAgent(std::make_shared<app::UndoStore>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        auto undo = kernel.agentAs<app::UndoStore>(app::UndoStore::kName);
        if (!doc || !undo) {
            std::fprintf(stderr, "FAIL: refusal fixture: agents did not all register under kName\n");
            return 1;
        }
        registerUndoPhaseCommands(kernel);

        const app::Machine initialMachine = doc->machine();
        if (undo->canUndo()) {
            std::fprintf(stderr, "FAIL: refusal fixture: canUndo() was already true before any edit ran\n");
            return 1;
        }

        // Exactly 4 undo-worthy edits; unwinding exactly this many proves
        // nothing else was pushed.
        const quint64 kStateA = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});                                // edit 1
        const quint64 kStateB = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});                               // edit 2
        const quint64 kT = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});                  // edit 3
        kernel.send(app::events::SetTransitionGuardRequested{.id = kT, .guard = QStringLiteral("ready")});  // edit 4

        const app::Machine afterSetup = doc->machine();

        // Item 5: blank `after` is REFUSED -- nothing changes, nothing is
        // pushed.
        kernel.send(app::events::RenameGuardRequested{.before = QStringLiteral("ready"), .after = QString()});
        // Item 6: before == after, and a blank `before`, are silent no-ops --
        // the same "nothing pushed" contract as the refusal above.
        kernel.send(
            app::events::RenameGuardRequested{.before = QStringLiteral("ready"), .after = QStringLiteral("ready")});
        kernel.send(app::events::RenameGuardRequested{.before = QString(), .after = QStringLiteral("whatever")});

        if (!(doc->machine() == afterSetup)) {
            std::fprintf(stderr,
                          "FAIL: a refused/no-op RenameGuardRequested (blank after, before==after, or blank before) "
                          "mutated the document\n");
            return 1;
        }

        // Unwind exactly the 4 real edits. If any of the three calls above
        // had pushed a 5th undo entry, canUndo() would still read true here.
        for (int i = 0; i < 4; ++i) {
            kernel.send(app::events::UndoRequested{});
        }
        if (undo->canUndo() || !sameTopology(doc->machine(), initialMachine)) {
            std::fprintf(stderr,
                          "FAIL: 4 undos did not fully unwind to the pre-setup machine's topology (canUndo()=%s) -- "
                          "a refused or no-op rename pushed an undo entry it should not have\n",
                          undo->canUndo() ? "true" : "false");
            return 1;
        }
    }

    // ---- 10d. renaming to a name that ALREADY exists merges the rows: two
    // elements end up sharing one hook name; collision detection belongs to
    // validator check 6, not this command. ----------------------------------
    {
        ordo::core::Kernel kernel;
        kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
        kernel.registerAgent(std::make_shared<app::SimulationAgent>());
        kernel.registerAgent(std::make_shared<app::UndoStore>());
        auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
        if (!doc) {
            std::fprintf(stderr, "FAIL: merge fixture: MachineDocAgent did not register under kName\n");
            return 1;
        }
        registerUndoPhaseCommands(kernel);

        const quint64 kStateA = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(0, 0)});
        const quint64 kStateB = doc->machine().nextId;
        kernel.send(app::events::AddStateRequested{.pos = QPointF(200, 0)});

        const quint64 kT1 = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
        kernel.send(app::events::SetTransitionGuardRequested{.id = kT1, .guard = QStringLiteral("a")});
        const quint64 kT2 = doc->machine().nextId;
        kernel.send(app::events::AddTransitionRequested{.from = kStateA, .to = kStateB});
        kernel.send(app::events::SetTransitionGuardRequested{.id = kT2, .guard = QStringLiteral("b")});

        kernel.send(app::events::RenameGuardRequested{.before = QStringLiteral("a"), .after = QStringLiteral("b")});

        if (doc->findTransition(kT1)->guard != QStringLiteral("b") ||
            doc->findTransition(kT2)->guard != QStringLiteral("b")) {
            std::fprintf(stderr,
                          "FAIL: renaming guard \"a\" to the EXISTING guard \"b\" did not merge -- t1=\"%s\" "
                          "t2=\"%s\" (want both \"b\")\n",
                          qUtf8Printable(doc->findTransition(kT1)->guard),
                          qUtf8Printable(doc->findTransition(kT2)->guard));
            return 1;
        }
    }

    // ---- 11. parseAssignForm() and validator check 11 -- the assign-form
    // classification authority and its validator consumer.

    // ---- 11a. parseAssignForm: positives, not-an-assign (blank message),
    // malformed (message present) -------------------------------------------
    failures += !expectAssignOk(QStringLiteral("count = 3"), QStringLiteral("count"), QStringLiteral("3"));
    failures += !expectAssignOk(QStringLiteral("count = count + 1"), QStringLiteral("count"), QStringLiteral("count + 1"));
    failures += !expectAssignOk(QStringLiteral("ratio = round(ratio * 10.0) / 10.0"), QStringLiteral("ratio"), QStringLiteral("round(ratio * 10.0) / 10.0"));
    failures += !expectAssignOk(QStringLiteral("count=3"), QStringLiteral("count"), QStringLiteral("3"));
    failures += !expectAssignOk(QStringLiteral("  count = 3  "), QStringLiteral("count"), QStringLiteral("3"));
    failures += !expectAssignOk(QStringLiteral("locked = true"), QStringLiteral("locked"), QStringLiteral("true"));
    failures += !expectAssignOk(QStringLiteral("role = 'admin'"), QStringLiteral("role"), QStringLiteral("'admin'"));
    // Arithmetic is not in grammar v1 (tokenize() has no '+' case), so a
    // composed RHS instead exercises a COMPARISON -- which is also the shape
    // that proves an embedded '==' inside the RHS never confuses the '='
    // discrimination.
    failures +=
        !expectAssignOk(QStringLiteral("locked = a == b"), QStringLiteral("locked"), QStringLiteral("a == b"));
    // Same proof for '>=', the other multi-char operator that shares a
    // character with '='.
    failures +=
        !expectAssignOk(QStringLiteral("ready = x >= 10"), QStringLiteral("ready"), QStringLiteral("x >= 10"));
    failures +=
        !expectAssignOk(QStringLiteral("user.age = 25"), QStringLiteral("user.age"), QStringLiteral("25"));
    failures +=
        !expectAssignOk(QStringLiteral("user.profile.city = 'Seoul'"), QStringLiteral("user.profile.city"), QStringLiteral("'Seoul'"));

    // AssignForm member helper validation
    {
        const auto form = app::expr::parseAssignForm(QStringLiteral("user.profile.age = 25"));
        if (!form.ok || form.rootTarget() != QStringLiteral("user") ||
            form.memberPath() != QStringLiteral("profile.age") || !form.isMemberAssign()) {
            std::fprintf(stderr, "FAIL: AssignForm helpers failed on user.profile.age = 25\n");
            ++failures;
        }
        const auto scalarForm = app::expr::parseAssignForm(QStringLiteral("count = 1"));
        if (!scalarForm.ok || scalarForm.rootTarget() != QStringLiteral("count") ||
            !scalarForm.memberPath().isEmpty() || scalarForm.isMemberAssign()) {
            std::fprintf(stderr, "FAIL: AssignForm helpers failed on scalar count = 1\n");
            ++failures;
        }
    }

    failures += !expectNotAnAssign(QStringLiteral("doThing"));
    failures += !expectNotAnAssign(QStringLiteral("doThing()"));
    failures += !expectNotAnAssign(QStringLiteral("count == 3"));
    failures += !expectNotAnAssign(QStringLiteral("count != 3"));
    failures += !expectNotAnAssign(QStringLiteral("count >= 3"));
    failures += !expectNotAnAssign(QStringLiteral("count <= 3"));
    failures += !expectNotAnAssign(QString());

    failures += !expectMalformedAssign(QStringLiteral("= 3"));            // blank target
    failures += !expectMalformedAssign(QStringLiteral("count ="));        // blank RHS
    failures += !expectMalformedAssign(QStringLiteral("count + 1 = 2"));  // non-identifier target
    failures += !expectMalformedAssign(QStringLiteral("a = b = c"));      // a second top-level '='
    failures += !expectMalformedAssign(QStringLiteral("'lit' = 3"));      // a string literal, not an identifier, on the left
    failures += !expectMalformedAssign(QStringLiteral("user. = 10"));     // incomplete member path
    failures += !expectMalformedAssign(QStringLiteral("user..age = 10")); // double dot in member path
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 11a (parseAssignForm) had %d failure(s)\n", failures);
        return 1;
    }

    // ---- 11b. validator check 11 over a hand-built Machine (no kernel) ------
    // Findings are isolated by the "assign" marker every check-11 message
    // carries, not by id (see 8a); check 6's category label is "Action", so
    // the marker cannot collide with it.
    {
        auto check11FindingCount = [](const app::Machine& machine, quint64 stateId, quint64 transitionId) {
            int count = 0;
            for (const app::Problem& problem : app::validate(machine)) {
                if (problem.severity != app::ProblemSeverity::Error || problem.stateId != stateId ||
                    problem.transitionId != transitionId) {
                    continue;
                }
                if (problem.text.contains(QStringLiteral("assign"))) {
                    ++count;
                }
            }
            return count;
        };
        auto expectCheck11Count = [&](const app::Machine& machine, quint64 stateId, quint64 transitionId,
                                      int wanted) {
            const int actual = check11FindingCount(machine, stateId, transitionId);
            if (actual != wanted) {
                std::fprintf(stderr,
                             "FAIL: check 11 reported %d finding(s) for state %llu / transition %llu (want %d)\n",
                             actual, static_cast<unsigned long long>(stateId),
                             static_cast<unsigned long long>(transitionId), wanted);
                return false;
            }
            return true;
        };

        // `count` (Int) and `ratio` (Double) let one Machine cover the
        // unknown-target, malformed, unparseable-RHS, type-mismatch and
        // Int/Double-promotion rows, plus a well-typed assign in an entry list
        // AND an exit list.
        app::Machine machine;
        machine.name = QStringLiteral("AssignActionCheck");
        machine.states = {
            app::State{.id = 1, .name = QStringLiteral("A"), .entryActions = {QStringLiteral("count = 5")}},
            app::State{.id = 2, .name = QStringLiteral("B"), .exitActions = {QStringLiteral("count = 6")}},
        };
        machine.initialStateId = 1;
        machine.context = {app::ContextVariable{.id = 50,
                                                 .name = QStringLiteral("count"),
                                                 .type = app::ContextType::Int,
                                                 .initialValue = QStringLiteral("0")},
                            app::ContextVariable{.id = 51,
                                                 .name = QStringLiteral("ratio"),
                                                 .type = app::ContextType::Double,
                                                 .initialValue = QStringLiteral("0.0")},
                            app::ContextVariable{.id = 52,
                                                 .name = QStringLiteral("user"),
                                                 .type = app::ContextType::Object,
                                                 .initialValue = QStringLiteral("{\"age\": 30, \"name\": \"Alice\"}")}};
        machine.nextId = 100;
        machine.transitions = {
            app::Transition{
                .id = 10, .from = 1, .to = 2, .event = QStringLiteral("E1"), .action = QStringLiteral("doThing")},
            app::Transition{.id = 11,
                            .from = 1,
                            .to = 2,
                            .event = QStringLiteral("E2"),
                            .action = QStringLiteral("missing = 3")},
            app::Transition{.id = 12,
                            .from = 1,
                            .to = 2,
                            .event = QStringLiteral("E3"),
                            .action = QStringLiteral("count + 1 = 2")},
            app::Transition{
                .id = 13, .from = 1, .to = 2, .event = QStringLiteral("E4"), .action = QStringLiteral("count = (3")},
            app::Transition{.id = 14,
                            .from = 1,
                            .to = 2,
                            .event = QStringLiteral("E5"),
                            .action = QStringLiteral("count = 'text'")},
            app::Transition{
                .id = 15, .from = 1, .to = 2, .event = QStringLiteral("E6"), .action = QStringLiteral("count = 3.5")},
            app::Transition{
                .id = 16, .from = 1, .to = 2, .event = QStringLiteral("E7"), .action = QStringLiteral("ratio = 3")},
            app::Transition{
                .id = 17, .from = 1, .to = 2, .event = QStringLiteral("E8"), .action = QStringLiteral("user.age = 25")},
            app::Transition{
                .id = 18, .from = 1, .to = 2, .event = QStringLiteral("E9"), .action = QStringLiteral("user.name = 'Bob'")},
            app::Transition{
                .id = 19, .from = 1, .to = 2, .event = QStringLiteral("E10"), .action = QStringLiteral("user.age = 'text'")},
            app::Transition{
                .id = 20, .from = 1, .to = 2, .event = QStringLiteral("E11"), .action = QStringLiteral("user.unknownProp = 1")},
            app::Transition{
                .id = 21, .from = 1, .to = 2, .event = QStringLiteral("E12"), .action = QStringLiteral("count.age = 25")},
            app::Transition{
                .id = 22, .from = 1, .to = 2, .event = QStringLiteral("E13"), .action = QStringLiteral("missing.age = 25")},
        };

        failures += !expectCheck11Count(machine, 0, 10, 0);  // named action hook -- untouched
        failures += !expectCheck11Count(machine, 0, 11, 1);  // unknown target variable
        failures += !expectCheck11Count(machine, 0, 12, 1);  // malformed assign (non-identifier target)
        failures += !expectCheck11Count(machine, 0, 13, 1);  // unparseable RHS
        failures += !expectCheck11Count(machine, 0, 14, 1);  // type mismatch: Int variable assigned a String
        failures += !expectCheck11Count(machine, 0, 15, 0);  // Int variable assigned a Double -- ACCEPTED
        failures += !expectCheck11Count(machine, 0, 16, 0);  // Double variable assigned an Int -- ACCEPTED
        failures += !expectCheck11Count(machine, 0, 17, 0);  // well-typed member assign (Int to Int)
        failures += !expectCheck11Count(machine, 0, 18, 0);  // well-typed member assign (String to String)
        failures += !expectCheck11Count(machine, 0, 19, 1);  // member assign type mismatch (String to Int)
        failures += !expectCheck11Count(machine, 0, 20, 1);  // unknown member property in target
        failures += !expectCheck11Count(machine, 0, 21, 1);  // member assign on scalar (not an Object)
        failures += !expectCheck11Count(machine, 0, 22, 1);  // unknown root context variable
        failures += !expectCheck11Count(machine, 1, 0, 0);   // well-typed assign in an entry list
        failures += !expectCheck11Count(machine, 2, 0, 0);   // well-typed assign in an exit list

        // The malformed-target and unparseable-RHS transitions must carry
        // their OWN distinct marker text (the two paths are not conflated).
        bool sawMalformedMarker = false;
        bool sawParseMarker = false;
        for (const app::Problem& problem : app::validate(machine)) {
            if (problem.transitionId == 12 &&
                problem.text.contains(QStringLiteral("looks like an assign but is not valid:"))) {
                sawMalformedMarker = true;
            }
            if (problem.transitionId == 13 && problem.text.contains(QStringLiteral("does not parse:"))) {
                sawParseMarker = true;
            }
        }
        if (!sawMalformedMarker) {
            std::fprintf(stderr, "FAIL: check 11 did not report a malformed-assign finding for transition 12\n");
            ++failures;
        }
        if (!sawParseMarker) {
            std::fprintf(stderr, "FAIL: check 11 did not report a \"does not parse:\" finding for transition 13\n");
            ++failures;
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 11b (validator check 11) had %d failure(s)\n", failures);
        return 1;
    }

    // ---- 12. emitCpp() -- the AOT emitter. Pure text assertions only; nothing
    // is compiled here.

    // 12a. every operator, one row each. `a`/`b`/`c` stand in for Identifier operands so each
    // row isolates exactly one operator's C++ spelling.
    failures += !expectEmitCpp(QStringLiteral("a && b"), QStringLiteral("context"),
                                QStringLiteral("context.a && context.b"));
    failures += !expectEmitCpp(QStringLiteral("a || b"), QStringLiteral("context"),
                                QStringLiteral("context.a || context.b"));
    failures += !expectEmitCpp(QStringLiteral("!a"), QStringLiteral("context"), QStringLiteral("!context.a"));
    failures += !expectEmitCpp(QStringLiteral("a == b"), QStringLiteral("context"),
                                QStringLiteral("context.a == context.b"));
    failures += !expectEmitCpp(QStringLiteral("a != b"), QStringLiteral("context"),
                                QStringLiteral("context.a != context.b"));
    failures += !expectEmitCpp(QStringLiteral("a < b"), QStringLiteral("context"),
                                QStringLiteral("context.a < context.b"));
    failures += !expectEmitCpp(QStringLiteral("a <= b"), QStringLiteral("context"),
                                QStringLiteral("context.a <= context.b"));
    failures += !expectEmitCpp(QStringLiteral("a > b"), QStringLiteral("context"),
                                QStringLiteral("context.a > context.b"));
    failures += !expectEmitCpp(QStringLiteral("a >= b"), QStringLiteral("context"),
                                QStringLiteral("context.a >= context.b"));
    failures += !expectEmitCpp(QStringLiteral("-a"), QStringLiteral("context"), QStringLiteral("-context.a"));
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 12a (emitCpp operators) had %d failure(s)\n", failures);
        return 1;
    }

    // 12b. an Identifier emits `<contextExpr>.<name>` for contextExpr ==
    // "context", and honours a DIFFERENT contextExpr too (the generated hook
    // parameter is not always spelled "context").
    failures += !expectEmitCpp(QStringLiteral("count"), QStringLiteral("context"), QStringLiteral("context.count"));
    failures += !expectEmitCpp(QStringLiteral("count"), QStringLiteral("ctx_"), QStringLiteral("ctx_.count"));
    failures += !expectEmitCpp(QStringLiteral("count + 1"), QStringLiteral("context"), QStringLiteral("context.count + 1LL"));
    failures += !expectEmitCpp(QStringLiteral("pow(count, 2)"), QStringLiteral("context"), QStringLiteral("std::pow(context.count, 2LL)"));
    failures += !expectEmitCpp(QStringLiteral("sqrt(ratio)"), QStringLiteral("context"), QStringLiteral("std::sqrt(context.ratio)"));
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 12b (emitCpp identifier) had %d failure(s)\n", failures);
        return 1;
    }

    // 12c. each literal type: Bool, an Int with its LL suffix (the generated
    // Context member is `long long`), a Double that cannot be misread as an
    // integer ('.' or exponent marker always present), a String as an escaped
    // C++ literal, and a String containing BOTH a quote and a backslash (the
    // source string syntax has no escapes, so either can appear verbatim).
    failures += !expectEmitCpp(QStringLiteral("true"), QStringLiteral("context"), QStringLiteral("true"));
    failures += !expectEmitCpp(QStringLiteral("false"), QStringLiteral("context"), QStringLiteral("false"));
    failures += !expectEmitCpp(QStringLiteral("42"), QStringLiteral("context"), QStringLiteral("42LL"));
    failures += !expectEmitCpp(QStringLiteral("3.0"), QStringLiteral("context"), QStringLiteral("3.0"));
    failures += !expectEmitCpp(QStringLiteral("2.5"), QStringLiteral("context"), QStringLiteral("2.5"));
    failures += !expectEmitCpp(QStringLiteral("'admin'"), QStringLiteral("context"), QStringLiteral("\"admin\""));
    // Source DSL text: 'she said "hi" \ ok' -- a single-quoted string literal
    // whose body contains a literal double quote and a literal backslash,
    // neither escaped (v1's tokenizer has no escape syntax at all). The
    // wanted C++ output escapes both: \" for the quote, \\ for the backslash.
    failures += !expectEmitCpp(QStringLiteral("'she said \"hi\" \\ ok'"), QStringLiteral("context"),
                                QStringLiteral("\"she said \\\"hi\\\" \\\\ ok\""));
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 12c (emitCpp literals) had %d failure(s)\n", failures);
        return 1;
    }

    // 12d. precedence: `a || b && c` and `(a || b) && c` must emit DIFFERENT
    // text whose parenthesisation preserves each source's own grouping; both
    // are asserted so one being right by luck cannot pass. The parse trees are
    // pinned above (`a||b&&c` -> Or(a, And(b,c)); `(a||b)&&c` -> And(Or(a,b),
    // c)), so this checks only the C++-side parenthesisation of that shape.
    failures += !expectEmitCpp(QStringLiteral("a||b&&c"), QStringLiteral("context"),
                                QStringLiteral("context.a || (context.b && context.c)"));
    failures += !expectEmitCpp(QStringLiteral("(a||b)&&c"), QStringLiteral("context"),
                                QStringLiteral("(context.a || context.b) && context.c"));
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 12d (emitCpp precedence) had %d failure(s)\n", failures);
        return 1;
    }

    // 12e. a composed guard emits ONE line with the grouping intact. Parses as
    // Or(And(Greater(count, 3), Not(locked)), Equal(role, 'admin')): `&&`
    // binds tighter than `||` (grammar v1's ladder), so `!locked` sits inside
    // the `&&` and `role == 'admin'` is Or's other side.
    failures += !expectEmitCpp(
        QStringLiteral("count > 3 && !locked || role == 'admin'"), QStringLiteral("context"),
        QStringLiteral("((context.count > 3LL) && (!context.locked)) || (context.role == \"admin\")"));
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 12e (emitCpp composed guard) had %d failure(s)\n",
                     failures);
        return 1;
    }

    // 12f. unary minus, and a negative Int literal. `-(a && b)` exercises unary
    // minus over a NON-leaf operand (parenthesised); `-5` parses as
    // Negate(IntLiteral(5)), since the tokenizer never emits a negative
    // IntLiteral directly.
    failures += !expectEmitCpp(QStringLiteral("-(a && b)"), QStringLiteral("context"),
                                QStringLiteral("-(context.a && context.b)"));
    failures += !expectEmitCpp(QStringLiteral("-5"), QStringLiteral("context"), QStringLiteral("-5LL"));
    if (failures != 0) {
        std::fprintf(stderr, "FAIL: expression phase section 12f (emitCpp unary minus) had %d failure(s)\n",
                     failures);
        return 1;
    }


    return runExpressionActorsSmoke();
}
