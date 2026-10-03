#pragma once

#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <ordo/core/agent.h>

#include "model/machine.h"

namespace app {

// The payload of UndoStore's stacks: one op per MachineDocAgent choke-point call a
// captured Transaction touched. Each op is a before/after image pair, so applyDelta
// replays it through the same choke method, which re-publishes its normal fact.
// Defined here (not in controller/) so this model header does not depend on the
// controller layer.

struct StateAddOp {
    State state;  // undo: doc.deleteState(state.id); redo: doc.restoreState(state)
};
struct StateRenameOp {
    quint64 id = 0;
    QString before;
    QString after;
};
struct StateKindOp {
    quint64 id = 0;
    StateKind before = StateKind::Normal;
    StateKind after = StateKind::Normal;
};
struct StateMoveOp {
    quint64 id = 0;
    QPointF before;
    QPointF after;
};
struct EntryActionsOp {
    quint64 id = 0;
    QStringList before;
    QStringList after;
};
struct ExitActionsOp {
    quint64 id = 0;
    QStringList before;
    QStringList after;
};
struct DescriptionOp {
    quint64 id = 0;
    QString before;
    QString after;
};
struct TagsOp {
    quint64 id = 0;
    QStringList before;
    QStringList after;
};
struct HistoryDeepOp {
    quint64 stateId = 0;
    bool before = false;
    bool after = false;
};
struct InvokeSrcOp {
    quint64 stateId = 0;
    QString before;
    QString after;
};
struct InvokeIdOp {
    quint64 stateId = 0;
    QString before;
    QString after;
};
struct InvokeOutputTypeOp {
    quint64 stateId = 0;
    ContextType before = ContextType::Int;
    ContextType after = ContextType::Int;
};
struct InvocationsOp {
    quint64 stateId = 0;
    QVector<Invocation> before;
    QVector<Invocation> after;
};
struct StateColorOp {
    quint64 id = 0;
    ElementColor before = ElementColor::Default;
    ElementColor after = ElementColor::Default;
};
struct TransitionColorOp {
    quint64 id = 0;
    ElementColor before = ElementColor::Default;
    ElementColor after = ElementColor::Default;
};
struct NoteColorOp {
    quint64 id = 0;
    ElementColor before = ElementColor::Default;
    ElementColor after = ElementColor::Default;
};
struct MachineSelfOp {
    quint64 id = 0;
    bool before = false;
    bool after = false;
};
struct StateReparentOp {
    quint64 id = 0;
    quint64 before = 0;
    quint64 after = 0;
};
struct InitialChildOp {
    quint64 stateId = 0;
    quint64 before = 0;
    quint64 after = 0;
};
struct MachineRenameOp {
    QString before;
    QString after;
};
struct InitialStateOp {
    quint64 before = 0;
    quint64 after = 0;
};
struct StateDeleteOp {
    State before;    // undo: doc.restoreState(before, index); redo: doc.deleteState(before.id)
    int index = -1;  // erased position in Machine::states; undo re-inserts there so a
                     // reverse-replayed cascade keeps document order
};

struct TransitionAddOp {
    Transition transition;  // undo: doc.deleteTransition(transition.id); redo: doc.restoreTransition(transition)
};
struct TransitionEventOp {
    quint64 id = 0;
    QString before;
    QString after;
};
struct TransitionGuardOp {
    quint64 id = 0;
    QString before;
    QString after;
};
struct TransitionActionOp {
    quint64 id = 0;
    QString before;
    QString after;
};
struct TransitionDelayOp {
    quint64 id = 0;
    int before = 0;
    int after = 0;
};
struct TransitionRetargetOp {
    quint64 id = 0;
    quint64 fromBefore = 0;
    quint64 toBefore = 0;
    quint64 fromAfter = 0;
    quint64 toAfter = 0;
    QList<quint64> targetsBefore = {};
    QList<quint64> targetsAfter = {};
};
struct TransitionLabelOffsetOp {
    quint64 id = 0;
    QPointF before;
    QPointF after;
};
struct TransitionLabelRatioOp {
    quint64 id = 0;
    std::optional<qreal> before;
    std::optional<qreal> after;
};
struct TransitionBendpointsOp {
    quint64 id = 0;
    QVector<QPointF> before;
    QVector<QPointF> after;
};
struct TransitionReenterOp {
    quint64 id = 0;
    bool before = false;
    bool after = false;
};
struct TransitionAlwaysOp {
    quint64 id = 0;
    bool before = false;
    bool after = false;
};
struct TransitionDeleteOp {
    Transition before;  // undo: doc.restoreTransition(before, index); redo: doc.deleteTransition(before.id)
    int index = -1;     // erased position in Machine::transitions (see StateDeleteOp)
};

struct NoteAddOp {
    Note note;  // undo: doc.deleteNote(note.id); redo: doc.restoreNote(note)
};
struct NoteMoveOp {
    quint64 id = 0;
    QPointF before;
    QPointF after;
};
struct NoteTextOp {
    quint64 id = 0;
    QString before;
    QString after;
};
struct NoteDeleteOp {
    Note before;     // undo: doc.restoreNote(before, index); redo: doc.deleteNote(before.id)
    int index = -1;  // erased position in Machine::notes (see StateDeleteOp)
};

struct ContextOp {
    enum class Kind {
        Add,
        Rename,
        Type,
        InitialValue,
        CustomTypeName,
        Delete
    };
    Kind kind = Kind::Add;
    quint64 id = 0;
    int index = -1;
    ContextType typeBefore = ContextType::Int;
    ContextType typeAfter = ContextType::Int;
    QString stringBefore;
    QString stringAfter;
    ContextVariable variable;
};

// Struct definitions, external headers, and typed payload operations.
struct TypesOp {
    enum class Kind {
        TransitionPayloadType,
        ExternalHeaders,
        StructAdd,
        StructChange,
        StructDelete
    };
    Kind kind = Kind::TransitionPayloadType;
    quint64 id = 0;
    int index = -1;
    QString stringBefore;
    QString stringAfter;
    QStringList headersBefore;
    QStringList headersAfter;
    StructDefinition structBefore;
    StructDefinition structAfter;
};

using UndoOp = std::variant<StateAddOp, StateRenameOp, StateKindOp, StateMoveOp, EntryActionsOp, ExitActionsOp,
                             DescriptionOp, TagsOp, HistoryDeepOp, InvokeSrcOp, InvokeIdOp, InvokeOutputTypeOp, InvocationsOp,
                             StateColorOp, TransitionColorOp, NoteColorOp,
                             MachineSelfOp, StateReparentOp, InitialChildOp, MachineRenameOp,
                             InitialStateOp, StateDeleteOp, TransitionAddOp, TransitionEventOp, TransitionGuardOp,
                             TransitionActionOp, TransitionDelayOp, TransitionRetargetOp, TransitionLabelOffsetOp,
                             TransitionLabelRatioOp,
                             TransitionBendpointsOp,
                             TransitionReenterOp, TransitionAlwaysOp,
                             TransitionDeleteOp, NoteAddOp, NoteMoveOp, NoteTextOp, NoteDeleteOp,
                             ContextOp, TypesOp>;

// One committed undo/redo step: the ordered op list one Transaction captured.
// Undo replays it in reverse (before-images), redo in order (after-images).
struct TransactionDelta {
    std::vector<UndoOp> ops;
};

// Owns the undo/redo delta stacks for one machine document's kernel; never shared
// across documents. Capture (journal attach, op list) is controller/undo_capture.h's
// Transaction; this class only holds the stacks and reports canUndo()/canRedo().
class UndoStore : public ordo::core::Agent {
public:
    static constexpr const char* kName = "undo";

    // cap: max entries retained in undoStack_; a push beyond it evicts the oldest.
    // redoStack_ is uncapped (bounded by undoStack_'s history).
    explicit UndoStore(std::size_t cap = 100);

    // Pushes delta onto undoStack_ (evicting the oldest entry past cap) and
    // clears redoStack_ (a newly committed edit invalidates redo history).
    // Sends events::UndoStateChanged only if canUndo()/canRedo() actually
    // changed.
    void push(TransactionDelta delta);

    bool canUndo() const;
    bool canRedo() const;

    // Pops undoStack_'s top and pushes a COPY of it onto redoStack_ -- the
    // caller applies the returned delta BACKWARD (before-images).
    // std::nullopt if undoStack_ was empty (caller should check canUndo()
    // first). Sends events::UndoStateChanged if canUndo()/canRedo() changed.
    std::optional<TransactionDelta> takeUndo();

    // Symmetric to takeUndo(): pops redoStack_'s top, pushes a copy onto
    // undoStack_ (no cap eviction on this transfer), returns it for the
    // caller to apply FORWARD (after-images). std::nullopt if redoStack_
    // was empty.
    std::optional<TransactionDelta> takeRedo();

    // ---- Batch grouping (one undo step for a multi-command gesture) ---------
    // While a batch is open (depth > 0), push() appends its ops to a pending merged
    // delta; the outermost endBatch() pushes it as one entry (clearing redo once).
    // Nesting is depth-counted. Atomicity relies on synchronous single-threaded
    // dispatch: a batch is opened and closed within one call stack, so no unrelated
    // command can commit while it is open.
    void beginBatch();
    void endBatch();  // unbalanced call (depth already 0) is a defensive no-op
    bool batchOpen() const { return batchDepth_ > 0; }

    // Empties both stacks (call on project load) and drops any open batch.
    void clearAll();

private:
    // Sends events::UndoStateChanged if canUndo()/canRedo() differ from the before-snapshot.
    void dispatchIfStateChanged(bool undoBefore, bool redoBefore);

    std::vector<TransactionDelta> undoStack_;
    std::vector<TransactionDelta> redoStack_;
    std::size_t cap_;
    int batchDepth_ = 0;            // see beginBatch() above
    TransactionDelta pendingBatch_;  // ops merged so far while batchDepth_ > 0
};

}  // namespace app
