#pragma once

#include <QString>
#include <QtGlobal>

#include "model/machine.h"

namespace app {

struct NodeCodeProjection {
    QString stateEnumSnippet;       // e.g. State Enum identifier or declaration snippet
    QString hooksSnippet;           // e.g. pure-virtual methods relevant to this entity
    QString coreHandlerSnippet;     // e.g. entry/exit execution and event switch branch
    QString fullSnippet;            // combined coherent snippet

    bool isEmpty() const {
        return stateEnumSnippet.isEmpty() && hooksSnippet.isEmpty() && coreHandlerSnippet.isEmpty();
    }
};

// Pure function: takes machine definition and target state ID, returns contextual C++ slice.
// If stateId is not found, returns an empty projection.
NodeCodeProjection projectStateCode(const Machine& machine, quint64 stateId,
                                    const QString& rootNamespace = QStringLiteral("app::generated"));

// Pure function: takes machine definition and target transition ID, returns contextual C++ slice.
// If transitionId is not found, returns an empty projection.
NodeCodeProjection projectTransitionCode(const Machine& machine, quint64 transitionId,
                                         const QString& rootNamespace = QStringLiteral("app::generated"));

}  // namespace app
