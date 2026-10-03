#include "controller/undo_capture.h"

#include <type_traits>
#include <utility>
#include <variant>

namespace app {

// ---- Transaction::CapturingJournal -----------------------------------------
// Each override wraps its before/after images in the matching UndoOp and appends it to
// the owning Transaction's ops_, in capture order.

void Transaction::CapturingJournal::stateAdded(const State& state) {
    owner_.ops_.push_back(StateAddOp{.state = state});
}

void Transaction::CapturingJournal::stateRenamed(quint64 id, const QString& before, const QString& after) {
    owner_.ops_.push_back(StateRenameOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::stateKindChanged(quint64 id, StateKind before, StateKind after) {
    owner_.ops_.push_back(StateKindOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::stateMoved(quint64 id, QPointF before, QPointF after) {
    owner_.ops_.push_back(StateMoveOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::entryActionsChanged(quint64 id, const QStringList& before,
                                                          const QStringList& after) {
    owner_.ops_.push_back(EntryActionsOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::exitActionsChanged(quint64 id, const QStringList& before,
                                                         const QStringList& after) {
    owner_.ops_.push_back(ExitActionsOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::descriptionChanged(quint64 id, const QString& before, const QString& after) {
    owner_.ops_.push_back(DescriptionOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::tagsChanged(quint64 id, const QStringList& before, const QStringList& after) {
    owner_.ops_.push_back(TagsOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::historyDeepChanged(quint64 id, bool before, bool after) {
    owner_.ops_.push_back(HistoryDeepOp{.stateId = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::invokeSrcChanged(quint64 id, const QString& before, const QString& after) {
    owner_.ops_.push_back(InvokeSrcOp{.stateId = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::invokeIdChanged(quint64 id, const QString& before, const QString& after) {
    owner_.ops_.push_back(InvokeIdOp{.stateId = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::invokeOutputTypeChanged(quint64 id, ContextType before, ContextType after) {
    owner_.ops_.push_back(InvokeOutputTypeOp{.stateId = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::invocationsChanged(quint64 id, const QVector<Invocation>& before, const QVector<Invocation>& after) {
    owner_.ops_.push_back(InvocationsOp{.stateId = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::stateColorChanged(quint64 id, ElementColor before, ElementColor after) {
    owner_.ops_.push_back(StateColorOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::transitionColorChanged(quint64 id, ElementColor before, ElementColor after) {
    owner_.ops_.push_back(TransitionColorOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::noteColorChanged(quint64 id, ElementColor before, ElementColor after) {
    owner_.ops_.push_back(NoteColorOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::machineSelfChanged(quint64 id, bool before, bool after) {
    owner_.ops_.push_back(MachineSelfOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::stateReparented(quint64 id, quint64 before, quint64 after) {
    owner_.ops_.push_back(StateReparentOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::initialChildChanged(quint64 stateId, quint64 before, quint64 after) {
    owner_.ops_.push_back(InitialChildOp{.stateId = stateId, .before = before, .after = after});
}

void Transaction::CapturingJournal::machineRenamed(const QString& before, const QString& after) {
    owner_.ops_.push_back(MachineRenameOp{.before = before, .after = after});
}

void Transaction::CapturingJournal::initialStateChanged(quint64 before, quint64 after) {
    owner_.ops_.push_back(InitialStateOp{.before = before, .after = after});
}

void Transaction::CapturingJournal::transitionAdded(const Transition& transition) {
    owner_.ops_.push_back(TransitionAddOp{.transition = transition});
}

void Transaction::CapturingJournal::transitionEventChanged(quint64 id, const QString& before, const QString& after) {
    owner_.ops_.push_back(TransitionEventOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::transitionGuardChanged(quint64 id, const QString& before, const QString& after) {
    owner_.ops_.push_back(TransitionGuardOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::transitionActionChanged(quint64 id, const QString& before, const QString& after) {
    owner_.ops_.push_back(TransitionActionOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::transitionDelayChanged(quint64 id, int before, int after) {
    owner_.ops_.push_back(TransitionDelayOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::transitionRetargeted(quint64 id, quint64 fromBefore, quint64 toBefore,
                                                           quint64 fromAfter, quint64 toAfter,
                                                           const QList<quint64>& targetsBefore,
                                                           const QList<quint64>& targetsAfter) {
    owner_.ops_.push_back(TransitionRetargetOp{
        .id = id,
        .fromBefore = fromBefore,
        .toBefore = toBefore,
        .fromAfter = fromAfter,
        .toAfter = toAfter,
        .targetsBefore = targetsBefore,
        .targetsAfter = targetsAfter,
    });
}

void Transaction::CapturingJournal::transitionLabelMoved(quint64 id, QPointF before, QPointF after) {
    owner_.ops_.push_back(TransitionLabelOffsetOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::transitionLabelRatioChanged(quint64 id, std::optional<qreal> before,
                                                                  std::optional<qreal> after) {
    owner_.ops_.push_back(TransitionLabelRatioOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::transitionBendpointsChanged(quint64 id, const QVector<QPointF>& before, const QVector<QPointF>& after) {
    owner_.ops_.push_back(TransitionBendpointsOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::transitionReenterChanged(quint64 id, bool before, bool after) {
    owner_.ops_.push_back(TransitionReenterOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::transitionAlwaysChanged(quint64 id, bool before, bool after) {
    owner_.ops_.push_back(TransitionAlwaysOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::transitionPayloadTypeChanged(quint64 id, const QString& before, const QString& after) {
    owner_.ops_.push_back(TypesOp{
        .kind = TypesOp::Kind::TransitionPayloadType,
        .id = id,
        .stringBefore = before,
        .stringAfter = after,
    });
}

void Transaction::CapturingJournal::stateDeleted(const State& before, int index) {
    owner_.ops_.push_back(StateDeleteOp{.before = before, .index = index});
}

void Transaction::CapturingJournal::transitionDeleted(const Transition& before, int index) {
    owner_.ops_.push_back(TransitionDeleteOp{.before = before, .index = index});
}

void Transaction::CapturingJournal::noteAdded(const Note& note) { owner_.ops_.push_back(NoteAddOp{.note = note}); }

void Transaction::CapturingJournal::noteMoved(quint64 id, QPointF before, QPointF after) {
    owner_.ops_.push_back(NoteMoveOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::noteTextChanged(quint64 id, const QString& before, const QString& after) {
    owner_.ops_.push_back(NoteTextOp{.id = id, .before = before, .after = after});
}

void Transaction::CapturingJournal::noteDeleted(const Note& before, int index) {
    owner_.ops_.push_back(NoteDeleteOp{.before = before, .index = index});
}

void Transaction::CapturingJournal::contextVariableAdded(const ContextVariable& variable) {
    owner_.ops_.push_back(ContextOp{
        .kind = ContextOp::Kind::Add,
        .variable = variable,
    });
}

void Transaction::CapturingJournal::contextVariableRenamed(quint64 id, const QString& before, const QString& after) {
    owner_.ops_.push_back(ContextOp{
        .kind = ContextOp::Kind::Rename,
        .id = id,
        .stringBefore = before,
        .stringAfter = after,
    });
}

void Transaction::CapturingJournal::contextTypeChanged(quint64 id, ContextType before, ContextType after) {
    owner_.ops_.push_back(ContextOp{
        .kind = ContextOp::Kind::Type,
        .id = id,
        .typeBefore = before,
        .typeAfter = after,
    });
}

void Transaction::CapturingJournal::contextInitialValueChanged(quint64 id, const QString& before,
                                                                 const QString& after) {
    owner_.ops_.push_back(ContextOp{
        .kind = ContextOp::Kind::InitialValue,
        .id = id,
        .stringBefore = before,
        .stringAfter = after,
    });
}

void Transaction::CapturingJournal::contextVariableDeleted(const ContextVariable& before, int index) {
    owner_.ops_.push_back(ContextOp{
        .kind = ContextOp::Kind::Delete,
        .index = index,
        .variable = before,
    });
}

void Transaction::CapturingJournal::contextCustomTypeNameChanged(quint64 id, const QString& before, const QString& after) {
    owner_.ops_.push_back(ContextOp{
        .kind = ContextOp::Kind::CustomTypeName,
        .id = id,
        .stringBefore = before,
        .stringAfter = after,
    });
}

void Transaction::CapturingJournal::externalHeadersChanged(const QStringList& before, const QStringList& after) {
    owner_.ops_.push_back(TypesOp{
        .kind = TypesOp::Kind::ExternalHeaders,
        .headersBefore = before,
        .headersAfter = after,
    });
}

void Transaction::CapturingJournal::structDefinitionAdded(const StructDefinition& def) {
    owner_.ops_.push_back(TypesOp{
        .kind = TypesOp::Kind::StructAdd,
        .structAfter = def,
    });
}

void Transaction::CapturingJournal::structDefinitionChanged(const StructDefinition& before, const StructDefinition& after) {
    owner_.ops_.push_back(TypesOp{
        .kind = TypesOp::Kind::StructChange,
        .structBefore = before,
        .structAfter = after,
    });
}

void Transaction::CapturingJournal::structDefinitionDeleted(const StructDefinition& before, int index) {
    owner_.ops_.push_back(TypesOp{
        .kind = TypesOp::Kind::StructDelete,
        .index = index,
        .structBefore = before,
    });
}

// ---- Transaction ------------------------------------------------------------

Transaction::Transaction(ordo::core::CommandContext& context) : context_(context), journal_(*this) {}

Transaction::~Transaction() {
    if (active_) {
        doc_->setJournal(nullptr);
    }
}

void Transaction::begin() {
    doc_ = context_.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    undo_ = context_.agentAs<UndoStore>(UndoStore::kName);
    if (!doc_ || !undo_) {
        return;  // degrade to a no-op
    }
    ops_.clear();
    doc_->setJournal(&journal_);
    active_ = true;
}

void Transaction::commit() {
    if (!active_) {
        return;
    }
    doc_->setJournal(nullptr);
    active_ = false;
    if (!ops_.empty()) {
        undo_->push(TransactionDelta{.ops = std::move(ops_)});
    }
}

void Transaction::abort() {
    if (!active_) {
        return;
    }
    doc_->setJournal(nullptr);  // detach first -- backward replay must not re-record itself
    active_ = false;
    TransactionDelta delta{.ops = std::move(ops_)};
    applyDelta(*doc_, delta, ApplyDirection::Backward);
}

// ---- applyDelta ---------------------------------------------------------------

namespace {

// One op, backward (undo): call the choke method that reverses it, with the op's
// before-image (an add/delete pair uses the opposite method).
void applyOpBackward(MachineDocAgent& doc, const UndoOp& op) {
    std::visit(
        [&doc](const auto& o) {
            using T = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<T, StateAddOp>) {
                doc.deleteState(o.state.id);
            } else if constexpr (std::is_same_v<T, StateRenameOp>) {
                doc.renameState(o.id, o.before);
            } else if constexpr (std::is_same_v<T, StateKindOp>) {
                doc.setStateKind(o.id, o.before);
            } else if constexpr (std::is_same_v<T, StateMoveOp>) {
                doc.moveState(o.id, o.before);
            } else if constexpr (std::is_same_v<T, EntryActionsOp>) {
                doc.setEntryActions(o.id, o.before);
            } else if constexpr (std::is_same_v<T, ExitActionsOp>) {
                doc.setExitActions(o.id, o.before);
            } else if constexpr (std::is_same_v<T, DescriptionOp>) {
                doc.setDescription(o.id, o.before);
            } else if constexpr (std::is_same_v<T, TagsOp>) {
                doc.setTags(o.id, o.before);
            } else if constexpr (std::is_same_v<T, HistoryDeepOp>) {
                doc.setHistoryDeep(o.stateId, o.before);
            } else if constexpr (std::is_same_v<T, InvokeSrcOp>) {
                doc.setInvokeSrc(o.stateId, o.before);
            } else if constexpr (std::is_same_v<T, InvokeIdOp>) {
                doc.setInvokeId(o.stateId, o.before);
            } else if constexpr (std::is_same_v<T, InvokeOutputTypeOp>) {
                doc.setInvokeOutputType(o.stateId, o.before);
            } else if constexpr (std::is_same_v<T, InvocationsOp>) {
                doc.setInvocations(o.stateId, o.before);
            } else if constexpr (std::is_same_v<T, StateColorOp>) {
                doc.setStateColor(o.id, o.before);
            } else if constexpr (std::is_same_v<T, TransitionColorOp>) {
                doc.setTransitionColor(o.id, o.before);
            } else if constexpr (std::is_same_v<T, NoteColorOp>) {
                doc.setNoteColor(o.id, o.before);
            } else if constexpr (std::is_same_v<T, MachineSelfOp>) {
                doc.setMachineSelf(o.id, o.before);
            } else if constexpr (std::is_same_v<T, StateReparentOp>) {
                doc.reparentState(o.id, o.before);
            } else if constexpr (std::is_same_v<T, InitialChildOp>) {
                doc.setInitialChild(o.stateId, o.before);
            } else if constexpr (std::is_same_v<T, MachineRenameOp>) {
                doc.setMachineName(o.before);
            } else if constexpr (std::is_same_v<T, InitialStateOp>) {
                doc.setInitialState(o.before);
            } else if constexpr (std::is_same_v<T, StateDeleteOp>) {
                doc.restoreState(o.before, o.index);
            } else if constexpr (std::is_same_v<T, TransitionAddOp>) {
                doc.deleteTransition(o.transition.id);
            } else if constexpr (std::is_same_v<T, TransitionEventOp>) {
                doc.setTransitionEvent(o.id, o.before);
            } else if constexpr (std::is_same_v<T, TransitionGuardOp>) {
                doc.setTransitionGuard(o.id, o.before);
            } else if constexpr (std::is_same_v<T, TransitionActionOp>) {
                doc.setTransitionAction(o.id, o.before);
            } else if constexpr (std::is_same_v<T, TransitionDelayOp>) {
                doc.setTransitionDelay(o.id, o.before);
            } else if constexpr (std::is_same_v<T, TransitionRetargetOp>) {
                doc.retargetTransition(o.id, o.fromBefore, o.toBefore, o.targetsBefore);
            } else if constexpr (std::is_same_v<T, TransitionLabelOffsetOp>) {
                doc.setTransitionLabelOffset(o.id, o.before);
            } else if constexpr (std::is_same_v<T, TransitionLabelRatioOp>) {
                doc.setTransitionLabelRatio(o.id, o.before);
            } else if constexpr (std::is_same_v<T, TransitionBendpointsOp>) {
                doc.setTransitionBendpoints(o.id, o.before);
            } else if constexpr (std::is_same_v<T, TransitionReenterOp>) {
                doc.setTransitionReenter(o.id, o.before);
            } else if constexpr (std::is_same_v<T, TransitionAlwaysOp>) {
                doc.setTransitionAlways(o.id, o.before);
            } else if constexpr (std::is_same_v<T, TransitionDeleteOp>) {
                doc.restoreTransition(o.before, o.index);
            } else if constexpr (std::is_same_v<T, NoteAddOp>) {
                doc.deleteNote(o.note.id);
            } else if constexpr (std::is_same_v<T, NoteMoveOp>) {
                doc.moveNote(o.id, o.before);
            } else if constexpr (std::is_same_v<T, NoteTextOp>) {
                doc.setNoteText(o.id, o.before);
            } else if constexpr (std::is_same_v<T, NoteDeleteOp>) {
                doc.restoreNote(o.before, o.index);
            } else if constexpr (std::is_same_v<T, ContextOp>) {
                switch (o.kind) {
                case ContextOp::Kind::Add:
                    doc.deleteContextVariable(o.variable.id);
                    break;
                case ContextOp::Kind::Rename:
                    doc.renameContextVariable(o.id, o.stringBefore);
                    break;
                case ContextOp::Kind::Type:
                    doc.setContextType(o.id, o.typeBefore);
                    break;
                case ContextOp::Kind::InitialValue:
                    doc.setContextInitialValue(o.id, o.stringBefore);
                    break;
                case ContextOp::Kind::CustomTypeName:
                    doc.setContextCustomTypeName(o.id, o.stringBefore);
                    break;
                case ContextOp::Kind::Delete:
                    doc.restoreContextVariable(o.variable, o.index);
                    break;
                }
            } else if constexpr (std::is_same_v<T, TypesOp>) {
                switch (o.kind) {
                case TypesOp::Kind::TransitionPayloadType:
                    doc.setTransitionPayloadType(o.id, o.stringBefore);
                    break;
                case TypesOp::Kind::ExternalHeaders:
                    doc.setExternalHeaders(o.headersBefore);
                    break;
                case TypesOp::Kind::StructAdd:
                    doc.deleteStructDefinition(o.structAfter.id);
                    break;
                case TypesOp::Kind::StructChange:
                    doc.setStructDefinition(o.structBefore.id, o.structBefore);
                    break;
                case TypesOp::Kind::StructDelete:
                    doc.restoreStructDefinition(o.structBefore, o.index);
                    break;
                }
            } else {
                static_assert(sizeof(T) == 0, "unhandled UndoOp alternative in applyOpBackward");
            }
        },
        op);
}

// One op, forward (redo): call the choke method that reproduces it, using
// the op's after-image.
void applyOpForward(MachineDocAgent& doc, const UndoOp& op) {
    std::visit(
        [&doc](const auto& o) {
            using T = std::decay_t<decltype(o)>;
            if constexpr (std::is_same_v<T, StateAddOp>) {
                doc.restoreState(o.state);
            } else if constexpr (std::is_same_v<T, StateRenameOp>) {
                doc.renameState(o.id, o.after);
            } else if constexpr (std::is_same_v<T, StateKindOp>) {
                doc.setStateKind(o.id, o.after);
            } else if constexpr (std::is_same_v<T, StateMoveOp>) {
                doc.moveState(o.id, o.after);
            } else if constexpr (std::is_same_v<T, EntryActionsOp>) {
                doc.setEntryActions(o.id, o.after);
            } else if constexpr (std::is_same_v<T, ExitActionsOp>) {
                doc.setExitActions(o.id, o.after);
            } else if constexpr (std::is_same_v<T, DescriptionOp>) {
                doc.setDescription(o.id, o.after);
            } else if constexpr (std::is_same_v<T, TagsOp>) {
                doc.setTags(o.id, o.after);
            } else if constexpr (std::is_same_v<T, HistoryDeepOp>) {
                doc.setHistoryDeep(o.stateId, o.after);
            } else if constexpr (std::is_same_v<T, InvokeSrcOp>) {
                doc.setInvokeSrc(o.stateId, o.after);
            } else if constexpr (std::is_same_v<T, InvokeIdOp>) {
                doc.setInvokeId(o.stateId, o.after);
            } else if constexpr (std::is_same_v<T, InvokeOutputTypeOp>) {
                doc.setInvokeOutputType(o.stateId, o.after);
            } else if constexpr (std::is_same_v<T, InvocationsOp>) {
                doc.setInvocations(o.stateId, o.after);
            } else if constexpr (std::is_same_v<T, StateColorOp>) {
                doc.setStateColor(o.id, o.after);
            } else if constexpr (std::is_same_v<T, TransitionColorOp>) {
                doc.setTransitionColor(o.id, o.after);
            } else if constexpr (std::is_same_v<T, NoteColorOp>) {
                doc.setNoteColor(o.id, o.after);
            } else if constexpr (std::is_same_v<T, MachineSelfOp>) {
                doc.setMachineSelf(o.id, o.after);
            } else if constexpr (std::is_same_v<T, StateReparentOp>) {
                doc.reparentState(o.id, o.after);
            } else if constexpr (std::is_same_v<T, InitialChildOp>) {
                doc.setInitialChild(o.stateId, o.after);
            } else if constexpr (std::is_same_v<T, MachineRenameOp>) {
                doc.setMachineName(o.after);
            } else if constexpr (std::is_same_v<T, InitialStateOp>) {
                doc.setInitialState(o.after);
            } else if constexpr (std::is_same_v<T, StateDeleteOp>) {
                doc.deleteState(o.before.id);
            } else if constexpr (std::is_same_v<T, TransitionAddOp>) {
                doc.restoreTransition(o.transition);
            } else if constexpr (std::is_same_v<T, TransitionEventOp>) {
                doc.setTransitionEvent(o.id, o.after);
            } else if constexpr (std::is_same_v<T, TransitionGuardOp>) {
                doc.setTransitionGuard(o.id, o.after);
            } else if constexpr (std::is_same_v<T, TransitionActionOp>) {
                doc.setTransitionAction(o.id, o.after);
            } else if constexpr (std::is_same_v<T, TransitionDelayOp>) {
                doc.setTransitionDelay(o.id, o.after);
            } else if constexpr (std::is_same_v<T, TransitionRetargetOp>) {
                doc.retargetTransition(o.id, o.fromAfter, o.toAfter, o.targetsAfter);
            } else if constexpr (std::is_same_v<T, TransitionLabelOffsetOp>) {
                doc.setTransitionLabelOffset(o.id, o.after);
            } else if constexpr (std::is_same_v<T, TransitionLabelRatioOp>) {
                doc.setTransitionLabelRatio(o.id, o.after);
            } else if constexpr (std::is_same_v<T, TransitionBendpointsOp>) {
                doc.setTransitionBendpoints(o.id, o.after);
            } else if constexpr (std::is_same_v<T, TransitionReenterOp>) {
                doc.setTransitionReenter(o.id, o.after);
            } else if constexpr (std::is_same_v<T, TransitionAlwaysOp>) {
                doc.setTransitionAlways(o.id, o.after);
            } else if constexpr (std::is_same_v<T, TransitionDeleteOp>) {
                doc.deleteTransition(o.before.id);
            } else if constexpr (std::is_same_v<T, NoteAddOp>) {
                doc.restoreNote(o.note);
            } else if constexpr (std::is_same_v<T, NoteMoveOp>) {
                doc.moveNote(o.id, o.after);
            } else if constexpr (std::is_same_v<T, NoteTextOp>) {
                doc.setNoteText(o.id, o.after);
            } else if constexpr (std::is_same_v<T, NoteDeleteOp>) {
                doc.deleteNote(o.before.id);
            } else if constexpr (std::is_same_v<T, ContextOp>) {
                switch (o.kind) {
                case ContextOp::Kind::Add:
                    doc.restoreContextVariable(o.variable);
                    break;
                case ContextOp::Kind::Rename:
                    doc.renameContextVariable(o.id, o.stringAfter);
                    break;
                case ContextOp::Kind::Type:
                    doc.setContextType(o.id, o.typeAfter);
                    break;
                case ContextOp::Kind::InitialValue:
                    doc.setContextInitialValue(o.id, o.stringAfter);
                    break;
                case ContextOp::Kind::CustomTypeName:
                    doc.setContextCustomTypeName(o.id, o.stringAfter);
                    break;
                case ContextOp::Kind::Delete:
                    doc.deleteContextVariable(o.variable.id);
                    break;
                }
            } else if constexpr (std::is_same_v<T, TypesOp>) {
                switch (o.kind) {
                case TypesOp::Kind::TransitionPayloadType:
                    doc.setTransitionPayloadType(o.id, o.stringAfter);
                    break;
                case TypesOp::Kind::ExternalHeaders:
                    doc.setExternalHeaders(o.headersAfter);
                    break;
                case TypesOp::Kind::StructAdd:
                    doc.restoreStructDefinition(o.structAfter);
                    break;
                case TypesOp::Kind::StructChange:
                    doc.setStructDefinition(o.structAfter.id, o.structAfter);
                    break;
                case TypesOp::Kind::StructDelete:
                    doc.deleteStructDefinition(o.structBefore.id);
                    break;
                }
            } else {
                static_assert(sizeof(T) == 0, "unhandled UndoOp alternative in applyOpForward");
            }
        },
        op);
}

}  // namespace

void applyDelta(MachineDocAgent& doc, const TransactionDelta& delta, ApplyDirection direction) {
    if (direction == ApplyDirection::Backward) {
        for (auto it = delta.ops.rbegin(); it != delta.ops.rend(); ++it) {
            applyOpBackward(doc, *it);
        }
    } else {
        for (const auto& op : delta.ops) {
            applyOpForward(doc, op);
        }
    }
}

}  // namespace app
