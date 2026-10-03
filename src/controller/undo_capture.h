#pragma once

#include <memory>
#include <vector>

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <ordo/core/command.h>
#include <ordo/core/command_context.h>

#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_journal.h"
#include "model/undo_store.h"

namespace app {

// Applies delta's ops to `doc` through its choke-point methods. Backward (undo) replays in
// reverse captured order with before-images, forward (redo) in original order with
// after-images; reverse order restores a cascade-deleted state before its transitions.
// Precondition: `doc`'s journal is detached, or the replay would re-record itself.
// Each choke call re-publishes its normal fact, which is the view-sync mechanism.
enum class ApplyDirection { Backward, Forward };

void applyDelta(MachineDocAgent& doc, const TransactionDelta& delta, ApplyDirection direction);

// Stack-local, one per mutating Command::execute() call (never an Agent). A no-op if
// MachineDocAgent/UndoStore is not registered.
class Transaction {
public:
    explicit Transaction(ordo::core::CommandContext& context);

    // Detaches the journal if neither commit() nor abort() ran, so no dangling pointer stays
    // registered on MachineDocAgent.
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    // Looks up MachineDocAgent/UndoStore and attaches a capturing journal to
    // the former. No-op if either agent is absent.
    void begin();

    // Detaches the journal and pushes the captured ops onto UndoStore (clearing redo) only if
    // the command mutated anything; a rejected edit commits nothing. No-op if begin() never ran.
    void commit();

    // Detaches the journal, then replays the captured ops backward, restoring the begin()-time
    // document. Nothing is pushed onto UndoStore. No-op if begin() never ran.
    void abort();

    bool isActive() const { return active_; }

private:
    class CapturingJournal : public MachineJournal {
    public:
        explicit CapturingJournal(Transaction& owner) : owner_(owner) {}

        void stateAdded(const State& state) override;
        void stateRenamed(quint64 id, const QString& before, const QString& after) override;
        void stateKindChanged(quint64 id, StateKind before, StateKind after) override;
        void stateMoved(quint64 id, QPointF before, QPointF after) override;
        void entryActionsChanged(quint64 id, const QStringList& before, const QStringList& after) override;
        void exitActionsChanged(quint64 id, const QStringList& before, const QStringList& after) override;
        void descriptionChanged(quint64 id, const QString& before, const QString& after) override;
        void tagsChanged(quint64 id, const QStringList& before, const QStringList& after) override;
        void historyDeepChanged(quint64 id, bool before, bool after) override;
        void invokeSrcChanged(quint64 id, const QString& before, const QString& after) override;
        void invokeIdChanged(quint64 id, const QString& before, const QString& after) override;
        void invokeOutputTypeChanged(quint64 id, ContextType before, ContextType after) override;
        void invocationsChanged(quint64 id, const QVector<Invocation>& before, const QVector<Invocation>& after) override;
        void stateColorChanged(quint64 id, ElementColor before, ElementColor after) override;
        void transitionColorChanged(quint64 id, ElementColor before, ElementColor after) override;
        void noteColorChanged(quint64 id, ElementColor before, ElementColor after) override;
        void machineSelfChanged(quint64 id, bool before, bool after) override;
        void stateReparented(quint64 id, quint64 before, quint64 after) override;
        void initialChildChanged(quint64 stateId, quint64 before, quint64 after) override;
        void machineRenamed(const QString& before, const QString& after) override;
        void initialStateChanged(quint64 before, quint64 after) override;
        void transitionAdded(const Transition& transition) override;
        void transitionEventChanged(quint64 id, const QString& before, const QString& after) override;
        void transitionGuardChanged(quint64 id, const QString& before, const QString& after) override;
        void transitionActionChanged(quint64 id, const QString& before, const QString& after) override;
        void transitionDelayChanged(quint64 id, int before, int after) override;
        void transitionRetargeted(quint64 id, quint64 fromBefore, quint64 toBefore, quint64 fromAfter,
                                   quint64 toAfter, const QList<quint64>& targetsBefore = {},
                                   const QList<quint64>& targetsAfter = {}) override;
        void transitionLabelMoved(quint64 id, QPointF before, QPointF after) override;
        void transitionLabelRatioChanged(quint64 id, std::optional<qreal> before, std::optional<qreal> after) override;
        void transitionBendpointsChanged(quint64 id, const QVector<QPointF>& before, const QVector<QPointF>& after) override;
        void transitionReenterChanged(quint64 id, bool before, bool after) override;
        void transitionAlwaysChanged(quint64 id, bool before, bool after) override;
        void transitionPayloadTypeChanged(quint64 id, const QString& before, const QString& after) override;
        void stateDeleted(const State& before, int index) override;
        void transitionDeleted(const Transition& before, int index) override;
        void noteAdded(const Note& note) override;
        void noteMoved(quint64 id, QPointF before, QPointF after) override;
        void noteTextChanged(quint64 id, const QString& before, const QString& after) override;
        void noteDeleted(const Note& before, int index) override;
        void contextVariableAdded(const ContextVariable& variable) override;
        void contextVariableRenamed(quint64 id, const QString& before, const QString& after) override;
        void contextTypeChanged(quint64 id, ContextType before, ContextType after) override;
        void contextInitialValueChanged(quint64 id, const QString& before, const QString& after) override;
        void contextCustomTypeNameChanged(quint64 id, const QString& before, const QString& after) override;
        void contextVariableDeleted(const ContextVariable& before, int index) override;
        void externalHeadersChanged(const QStringList& before, const QStringList& after) override;
        void structDefinitionAdded(const StructDefinition& def) override;
        void structDefinitionChanged(const StructDefinition& before, const StructDefinition& after) override;
        void structDefinitionDeleted(const StructDefinition& before, int index) override;

    private:
        Transaction& owner_;
    };

    ordo::core::CommandContext& context_;
    std::shared_ptr<MachineDocAgent> doc_;
    std::shared_ptr<UndoStore> undo_;
    CapturingJournal journal_;
    std::vector<UndoOp> ops_;
    bool active_ = false;
};

// Wraps RealCommand so dispatching EventT captures an undo step. A policy-rejected edit
// captures zero ops, so commit() pushes nothing. UndoCommand/RedoCommand are never wrapped.
template <typename RealCommand, typename EventT>
class UndoCaptureCommand : public ordo::core::Command<EventT> {
public:
    void execute(const EventT& event, ordo::core::CommandContext& context) override {
        Transaction txn(context);
        txn.begin();

        RealCommand{}.execute(event, context);

        txn.commit();
    }
};

}  // namespace app
