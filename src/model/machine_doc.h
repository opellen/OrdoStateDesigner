#pragma once

#include <QPointF>
#include <QString>
#include <QStringList>

#include <ordo/core/agent.h>

#include "model/machine.h"

namespace app {

struct MachineJournal;  // model/machine_journal.h; only a nullable pointer is stored here

// Owns one machine document; Machine::nextId is minted here only. Every mutation goes
// through one narrow method below, which notifies the attached MachineJournal, so no
// mutation may bypass them. deleteState cascades through children (document order) and
// deleteTransition(), journaling in a fixed order that undo restores by reverse iteration.
// Policy (whether an edit is allowed) lives in controller/edit_commands.h.
class MachineDocAgent : public ordo::core::Agent {
public:
    static constexpr const char* kName = "machine";

    MachineDocAgent() : Agent(kName) {}

    // Attaches the journal notified from every choke method (nullptr detaches).
    // Last call wins. Non-owning: the caller must detach before destroying it.
    void setJournal(MachineJournal* journal) { journal_ = journal; }

    // Mints a new id, appends a State at `pos` (default name, Normal kind),
    // publishes StateAdded, and returns the new id. A non-zero parentId whose
    // parent has no initialChildId also becomes that parent's initial child
    // (journaled as [StateAddOp, InitialChildOp], so one undo reverts both).
    quint64 addState(QPointF pos, quint64 parentId = 0);
    void renameState(quint64 id, const QString& name);
    void setStateKind(quint64 id, StateKind kind);
    void moveState(quint64 id, QPointF pos);
    void setEntryActions(quint64 id, QStringList entryActions);
    void setExitActions(quint64 id, QStringList exitActions);
    void setDescription(quint64 id, const QString& description);
    void setTags(quint64 id, QStringList tags);
    void setHistoryDeep(quint64 id, bool deep);
    void setInvokeSrc(quint64 id, const QString& src);
    void setInvokeId(quint64 id, const QString& invokeId);
    void setInvokeOutputType(quint64 id, ContextType type);
    void setInvocations(quint64 id, const QVector<Invocation>& invocations);
    void setStateColor(quint64 id, ElementColor color);
    // reparentState is mechanical only (no cycle/kind checks) and calls
    // setInitialChild itself on the old and new parent, each through its own
    // choke point so undo replays it.
    void setInitialChild(quint64 stateId, quint64 childId);
    void reparentState(quint64 id, quint64 newParentId);
    void setMachineName(const QString& name);
    // 0 clears it. deleteState() calls this itself when it deletes the initial state.
    void setInitialState(quint64 id);

    // Mints a new id, appends a Transition from->to (blank event/guard/
    // action, delayMs 0), publishes TransitionAdded, and returns the new id.
    quint64 addTransition(quint64 from, quint64 to);
    void setTransitionEvent(quint64 id, const QString& event);
    void setTransitionGuard(quint64 id, const QString& guard);
    void setTransitionAction(quint64 id, const QString& action);
    void setTransitionDelay(quint64 id, int delayMs);
    void retargetTransition(quint64 id, quint64 from, quint64 to, const QList<quint64>& targets = {});
    void setTransitionLabelOffset(quint64 id, QPointF offset);
    // No-op (no journal entry, no fact) when the stored value already equals `ratio`,
    // so a re-layout that leaves a ratio unchanged captures a minimal undo delta.
    void setTransitionLabelRatio(quint64 id, std::optional<qreal> ratio);
    void setTransitionBendpoints(quint64 id, const QVector<QPointF>& bendpoints);
    void setTransitionColor(quint64 id, ElementColor color);
    void setMachineSelf(quint64 id, bool machineSelf);  // root #machine flag
    void setTransitionReenter(quint64 id, bool reenter);
    void setTransitionAlways(quint64 id, bool always);
    void setTransitionPayloadType(quint64 id, const QString& payloadType);

    // Cascades: removes every transition touching `id` via deleteTransition() (each
    // publishing its own TransitionDeleted) before removing the state and publishing StateDeleted.
    void deleteState(quint64 id);
    void deleteTransition(quint64 id);

    // Mints a new id, appends a Note at `pos` (blank text), publishes NoteAdded,
    // and returns the new id.
    quint64 addNote(QPointF pos);
    void moveNote(quint64 id, QPointF pos);
    void setNoteText(quint64 id, const QString& text);
    void setNoteColor(quint64 id, ElementColor color);
    void deleteNote(quint64 id);

    // Mints a new id, appends a ContextVariable named var<id> (type Int,
    // initialValue "0"), publishes ContextVariableAdded, and returns the new id.
    quint64 addContextVariable();
    void renameContextVariable(quint64 id, const QString& name);
    void setContextType(quint64 id, ContextType type);
    void setContextInitialValue(quint64 id, const QString& initialValue);
    void setContextCustomTypeName(quint64 id, const QString& customTypeName);
    void deleteContextVariable(quint64 id);

    void setExternalHeaders(const QStringList& headers);
    quint64 addStructDefinition(StructDefinition def);
    void setStructDefinition(quint64 id, const StructDefinition& def);
    void deleteStructDefinition(quint64 id);

    // Undo/redo's id-preserving counterparts to add*: insert with the given id, never touch
    // nextId, publish and journal like the matching add. `index` is the position to
    // re-insert at (-1 or out of range appends); document order is semantic, and reverse
    // replay of a multi-element delete would invert it if restores just appended.
    void restoreState(State state, int index = -1);
    void restoreTransition(Transition transition, int index = -1);
    void restoreNote(Note note, int index = -1);
    void restoreContextVariable(ContextVariable variable, int index = -1);
    void restoreStructDefinition(StructDefinition def, int index = -1);

    // Replaces the whole document (project load) and publishes MachineSnapshotPublished,
    // the same fact a MachineSnapshotRequested reply uses.
    // Does NOT notify the journal: undo never spans a wholesale restore, so callers
    // loading a project must call UndoStore::clearAll().
    void restore(Machine machine);

    const Machine& machine() const { return machine_; }
    const State* findState(quint64 id) const;
    const Transition* findTransition(quint64 id) const;
    const Note* findNote(quint64 id) const;
    const ContextVariable* findContextVariable(quint64 id) const;
    const StructDefinition* findStructDefinition(quint64 id) const;

private:
    Machine machine_;
    MachineJournal* journal_ = nullptr;
};

}  // namespace app
