#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

#include "model/machine.h"
#include "infra/expression.h"

namespace app {

enum class ProblemSeverity { Error, Warning };

// One finding from validate() below. `stateId`/`transitionId` are 0 when
// not applicable to the finding (e.g. the machine-name check, which is
// about the whole document) -- never both non-zero at once.
struct Problem {
    ProblemSeverity severity = ProblemSeverity::Error;
    QString text;
    quint64 stateId = 0;
    quint64 transitionId = 0;
};

// The `output`/`error` binding an onDone/onError row's guard and actions see;
// none() for every other transition.
expr::PayloadBinding payloadBindingForTransition(const Machine& machine, const Transition& transition);

// Pure function: the pre-generate gate and the live Problems strip's data source.
// Error = generated code would not compile or would diverge from the simulator;
// Warning = compilable but suspicious. Findings are appended check by check
// (callers may re-sort); each check's rule is documented at its site in the .cpp.
QVector<Problem> validate(const Machine& machine);

}  // namespace app
