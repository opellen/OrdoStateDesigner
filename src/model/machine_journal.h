#pragma once

#include <optional>

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "model/machine.h"

namespace app {

// Non-owning observer on MachineDocAgent's mutation choke points. Every method
// defaults empty; one method per choke point, carrying the before/after images
// it has (an add has no "before", a delete no "after"). The id-preserving
// restoreState/restoreTransition notify stateAdded/transitionAdded, so a
// restore reads as an add. The wholesale restore(Machine) never notifies.
struct MachineJournal {
    virtual ~MachineJournal() = default;

    virtual void stateAdded(const State& state) {}
    virtual void stateRenamed(quint64 id, const QString& before, const QString& after) {}
    virtual void stateKindChanged(quint64 id, StateKind before, StateKind after) {}
    virtual void stateMoved(quint64 id, QPointF before, QPointF after) {}
    virtual void entryActionsChanged(quint64 id, const QStringList& before, const QStringList& after) {}
    virtual void exitActionsChanged(quint64 id, const QStringList& before, const QStringList& after) {}
    virtual void descriptionChanged(quint64 id, const QString& before, const QString& after) {}
    virtual void tagsChanged(quint64 id, const QStringList& before, const QStringList& after) {}
    virtual void historyDeepChanged(quint64 id, bool before, bool after) {}
    virtual void invokeSrcChanged(quint64 id, const QString& before, const QString& after) {}
    virtual void invokeIdChanged(quint64 id, const QString& before, const QString& after) {}
    virtual void invokeOutputTypeChanged(quint64 id, ContextType before, ContextType after) {}
    virtual void invocationsChanged(quint64 id, const QVector<Invocation>& before, const QVector<Invocation>& after) {}
    virtual void stateColorChanged(quint64 id, ElementColor before, ElementColor after) {}
    virtual void transitionColorChanged(quint64 id, ElementColor before, ElementColor after) {}
    virtual void noteColorChanged(quint64 id, ElementColor before, ElementColor after) {}
    virtual void machineSelfChanged(quint64 id, bool before, bool after) {}
    virtual void stateReparented(quint64 id, quint64 before, quint64 after) {}
    virtual void initialChildChanged(quint64 stateId, quint64 before, quint64 after) {}
    virtual void machineRenamed(const QString& before, const QString& after) {}
    virtual void initialStateChanged(quint64 before, quint64 after) {}

    virtual void transitionAdded(const Transition& transition) {}
    virtual void transitionEventChanged(quint64 id, const QString& before, const QString& after) {}
    virtual void transitionGuardChanged(quint64 id, const QString& before, const QString& after) {}
    virtual void transitionActionChanged(quint64 id, const QString& before, const QString& after) {}
    virtual void transitionDelayChanged(quint64 id, int before, int after) {}
    virtual void transitionRetargeted(quint64 id, quint64 fromBefore, quint64 toBefore, quint64 fromAfter,
                                       quint64 toAfter, const QList<quint64>& targetsBefore = {},
                                       const QList<quint64>& targetsAfter = {}) {}
    virtual void transitionLabelMoved(quint64 id, QPointF before, QPointF after) {}
    virtual void transitionLabelRatioChanged(quint64 id, std::optional<qreal> before, std::optional<qreal> after) {}
    virtual void transitionBendpointsChanged(quint64 id, const QVector<QPointF>& before, const QVector<QPointF>& after) {}
    virtual void transitionReenterChanged(quint64 id, bool before, bool after) {}
    virtual void transitionAlwaysChanged(quint64 id, bool before, bool after) {}
    virtual void transitionPayloadTypeChanged(quint64 id, const QString& before, const QString& after) {}

    // Before-image plus the erased vector index. The element is already gone from the
    // machine when this fires (the agent erases first, then notifies). The index lets undo
    // re-insert at the original position; otherwise reverse replay would invert document order.
    virtual void stateDeleted(const State& before, int index) {}
    virtual void transitionDeleted(const Transition& before, int index) {}

    virtual void noteAdded(const Note& note) {}
    virtual void noteMoved(quint64 id, QPointF before, QPointF after) {}
    virtual void noteTextChanged(quint64 id, const QString& before, const QString& after) {}
    virtual void noteDeleted(const Note& before, int index) {}

    virtual void contextVariableAdded(const ContextVariable& variable) {}
    virtual void contextVariableRenamed(quint64 id, const QString& before, const QString& after) {}
    virtual void contextTypeChanged(quint64 id, ContextType before, ContextType after) {}
    virtual void contextInitialValueChanged(quint64 id, const QString& before, const QString& after) {}
    virtual void contextCustomTypeNameChanged(quint64 id, const QString& before, const QString& after) {}
    virtual void contextVariableDeleted(const ContextVariable& before, int index) {}

    virtual void externalHeadersChanged(const QStringList& before, const QStringList& after) {}
    virtual void structDefinitionAdded(const StructDefinition& def) {}
    virtual void structDefinitionChanged(const StructDefinition& before, const StructDefinition& after) {}
    virtual void structDefinitionDeleted(const StructDefinition& before, int index) {}
};

}  // namespace app
