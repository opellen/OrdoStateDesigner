#include "infra/logic_inventory.h"

#include <QHash>

#include "infra/expression.h"

namespace app {

namespace {

// Accumulates rows in first-appearance order; a repeated source text amends
// its existing row. The key is the TRIMMED text, matching the validator's
// check 6 and expr::isBareIdentifier, so all three agree on "the same" name.
struct RowBuilder {
    explicit RowBuilder(bool collectingActions) : isAction(collectingActions) {}

    bool isAction = false;
    QVector<LogicRow> rows;
    QHash<QString, int> index;  // trimmed source text -> rows[] slot

    void addTransitionRef(const QString& rawSource, quint64 transitionId) {
        const QString name = rawSource.trimmed();
        if (name.isEmpty()) {
            return;  // blank means "no guard"/"no action"
        }
        LogicRow& row = rowFor(name);
        if (!row.transitionIds.contains(transitionId)) {
            row.transitionIds.push_back(transitionId);
        }
    }

    void addStateRef(const QString& rawSource, quint64 stateId) {
        const QString name = rawSource.trimmed();
        if (name.isEmpty()) {
            return;
        }
        LogicRow& row = rowFor(name);
        if (!row.stateIds.contains(stateId)) {
            row.stateIds.push_back(stateId);
        }
    }

  private:
    LogicRow& rowFor(const QString& name) {
        const auto found = index.constFind(name);
        if (found != index.constEnd()) {
            return rows[found.value()];
        }
        LogicRow row;
        row.name = name;
        row.rowClass = isAction ? classifyAction(name) : classifyGuard(name);
        rows.push_back(row);
        index.insert(name, static_cast<int>(rows.size()) - 1);
        return rows.back();
    }

    // Guards and actions have separate classifiers, each mirroring the
    // generator's guardIsHook()/actionIsHook(): the panel must agree with
    // which hooks are actually emitted.
    static LogicRowClass classifyGuard(const QString& source) {
        // A hook only when the WHOLE string is a bare identifier.
        return expr::isBareIdentifier(source) ? LogicRowClass::Hook : LogicRowClass::InlineExpression;
    }

    static LogicRowClass classifyAction(const QString& source) {
        const expr::SendToForm sendTo = expr::parseSendToForm(source);
        if (sendTo.ok || !sendTo.message.isEmpty()) {
            return LogicRowClass::SendTo;
        }
        const expr::SendParentForm sendParent = expr::parseSendParentForm(source);
        if (sendParent.ok || !sendParent.message.isEmpty()) {
            return LogicRowClass::SendParent;
        }
        const expr::RaiseForm raise = expr::parseRaiseForm(source);
        if (raise.ok || !raise.message.isEmpty()) {
            return LogicRowClass::Raise;
        }
        const expr::AssignForm form = expr::parseAssignForm(source);
        if (form.ok || !form.message.isEmpty()) {
            return LogicRowClass::Assign;
        }
        return LogicRowClass::Hook;
    }
};

}  // namespace

LogicInventory inventory(const Machine& machine) {
    LogicInventory result;

    // Guards, in transition order; guard rows never carry stateIds.
    {
        RowBuilder builder(/*collectingActions=*/false);
        for (const Transition& transition : machine.transitions) {
            builder.addTransitionRef(transition.guard, transition.id);
        }
        result.guards = builder.rows;
    }

    // Actions: transition actions first, then each state's entry then exit
    // actions -- the same walk as the validator's check 6.
    {
        RowBuilder builder(/*collectingActions=*/true);
        for (const Transition& transition : machine.transitions) {
            builder.addTransitionRef(transition.action, transition.id);
        }
        for (const State& state : machine.states) {
            for (const QString& entryAction : state.entryActions) {
                builder.addStateRef(entryAction, state.id);
            }
            for (const QString& exitAction : state.exitActions) {
                builder.addStateRef(exitAction, state.id);
            }
        }
        result.actions = builder.rows;
    }

    return result;
}

}  // namespace app
