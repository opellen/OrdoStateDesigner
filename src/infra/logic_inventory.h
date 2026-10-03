#pragma once

// Guard/action inventory, DERIVED from the guard/action strings already on the
// document -- never a second stored registry. Pure: no widgets, no ordo/core,
// no model mutation, so --smoke can assert it without a QApplication.

#include <QString>
#include <QVector>
#include <QtGlobal>

#include "model/machine.h"

namespace app {

// Hook: the text is a bare identifier -- a C++ method the user implements.
// InlineExpression: a guard expression compiled inline. Assign/Raise/SendTo/
// SendParent: the action forms the generator emits inline.
enum class LogicRowClass { Hook, InlineExpression, Assign, Raise, SendTo, SendParent };

// One row: a distinct source text plus every element that references it.
// `name` is the trimmed text and doubles as the merge key (same trimming as
// the validator's check 6).
struct LogicRow {
    QString name;                    // hook name, or the source text for an expression
    LogicRowClass rowClass = LogicRowClass::Hook;
    QVector<quint64> transitionIds;  // every Transition::guard/action referencing this row, document order
    QVector<quint64> stateIds;       // every State whose entryActions/exitActions reference this row, document order
};

// Guards: every Transition::guard. Actions: transition actions plus state
// entry/exit actions in ONE shared vocabulary, so a name used in several
// places is one row carrying every referencing id.
struct LogicInventory {
    QVector<LogicRow> guards;
    QVector<LogicRow> actions;
};

// Recomputed on every call (no cache). Rows are in document order of first
// appearance, never sorted, so a panel rebuilt on each refresh stays stable.
LogicInventory inventory(const Machine& machine);

}  // namespace app
