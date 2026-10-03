#pragma once

#include <optional>
#include <string_view>

#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include "model/machine.h"

namespace app::events {

// The machine document's edit vocabulary: every intent the canvas/inspector can
// send and the fact each one produces. MachineDocAgent mints every id; no intent
// supplies one for something that does not exist yet.

// ---- Intents: requests to perform an action --------------------------------

struct AddStateRequested {
    static constexpr std::string_view eventName = "AddStateRequested";
    QPointF pos;
    // The new state's immediate parent; 0 = direct child of the machine root.
    quint64 parentId = 0;
};

struct RenameStateRequested {
    static constexpr std::string_view eventName = "RenameStateRequested";
    quint64 id = 0;
    QString name;
};

struct SetStateKindRequested {
    static constexpr std::string_view eventName = "SetStateKindRequested";
    quint64 id = 0;
    StateKind kind = StateKind::Normal;
};

// Release-commit only: the canvas sends this once, on mouse release, not
// once per drag frame -- an in-progress drag is view-local state until then.
struct MoveStateRequested {
    static constexpr std::string_view eventName = "MoveStateRequested";
    quint64 id = 0;
    QPointF pos;
};

struct SetEntryActionsRequested {
    static constexpr std::string_view eventName = "SetEntryActionsRequested";
    quint64 id = 0;
    QStringList entryActions;
};

struct SetExitActionsRequested {
    static constexpr std::string_view eventName = "SetExitActionsRequested";
    quint64 id = 0;
    QStringList exitActions;
};

struct SetDescriptionRequested {
    static constexpr std::string_view eventName = "SetDescriptionRequested";
    quint64 id = 0;
    QString description;
};

struct SetTagsRequested {
    static constexpr std::string_view eventName = "SetTagsRequested";
    quint64 id = 0;
    QStringList tags;
};

struct SetHistoryDeepRequested {
    static constexpr std::string_view eventName = "SetHistoryDeepRequested";
    quint64 stateId = 0;
    bool deep = false;
};

// Changing the effective invoke id renames every "done.invoke.<id>" /
// "error.platform.<id>" event the state owns (done in the command).
struct SetInvokeSrcRequested {
    static constexpr std::string_view eventName = "SetInvokeSrcRequested";
    quint64 stateId = 0;
    QString src;
};

struct SetInvokeIdRequested {
    static constexpr std::string_view eventName = "SetInvokeIdRequested";
    quint64 stateId = 0;
    QString invokeId;
};

// The declared onDone payload type; no rename cascade, since it never appears in
// a Transition::event string.
struct SetInvokeOutputTypeRequested {
    static constexpr std::string_view eventName = "SetInvokeOutputTypeRequested";
    quint64 stateId = 0;
    ContextType type = ContextType::Int;
};

// Sets the full list of invocations for a state.
struct SetInvocationsRequested {
    static constexpr std::string_view eventName = "SetInvocationsRequested";
    quint64 stateId = 0;
    QVector<Invocation> invocations;
};

// Fired per element inside one undo batch, so a multi-selection recolor is a
// single undo step.
struct SetStateColorRequested {
    static constexpr std::string_view eventName = "SetStateColorRequested";
    quint64 id = 0;
    ElementColor color = ElementColor::Default;
};

struct SetTransitionColorRequested {
    static constexpr std::string_view eventName = "SetTransitionColorRequested";
    quint64 id = 0;
    ElementColor color = ElementColor::Default;
};

struct SetNoteColorRequested {
    static constexpr std::string_view eventName = "SetNoteColorRequested";
    quint64 id = 0;
    ElementColor color = ElementColor::Default;
};

// Only valid on a root transition (from == 0 && to == 0); retargeting onto a real
// state clears the flag in the same undo transaction.
struct SetMachineSelfRequested {
    static constexpr std::string_view eventName = "SetMachineSelfRequested";
    quint64 id = 0;
    bool machineSelf = false;
};

struct ReparentStateRequested {
    static constexpr std::string_view eventName = "ReparentStateRequested";
    quint64 id = 0;
    quint64 parentId = 0;
};

// Direct set only; MachineDocAgent also reassigns it automatically as children
// come and go, so most callers never send this.
struct SetInitialChildRequested {
    static constexpr std::string_view eventName = "SetInitialChildRequested";
    quint64 stateId = 0;
    quint64 initialChildId = 0;
};

struct SetMachineNameRequested {
    static constexpr std::string_view eventName = "SetMachineNameRequested";
    QString name;
};

// Sets Machine::initialStateId. id 0 clears it (the validator reports that as an
// Error; the intent is not rejected).
struct SetInitialStateRequested {
    static constexpr std::string_view eventName = "SetInitialStateRequested";
    quint64 id = 0;
};

// Never carries an event name -- a freshly added transition always starts
// blank; SetTransitionEventRequested names it afterward.
struct AddTransitionRequested {
    static constexpr std::string_view eventName = "AddTransitionRequested";
    quint64 from = 0;
    quint64 to = 0;
};

struct SetTransitionEventRequested {
    static constexpr std::string_view eventName = "SetTransitionEventRequested";
    quint64 id = 0;
    QString event;
};

struct SetTransitionGuardRequested {
    static constexpr std::string_view eventName = "SetTransitionGuardRequested";
    quint64 id = 0;
    QString guard;
};

struct SetTransitionActionRequested {
    static constexpr std::string_view eventName = "SetTransitionActionRequested";
    quint64 id = 0;
    QString action;
};

// Guard/action hook rename cascade, keyed by source text rather than id: a hook
// has no id, only the text shared by every referencing Transition::guard/action
// (and, for actions, State::entryActions/exitActions entry).
struct RenameGuardRequested {
    static constexpr std::string_view eventName = "RenameGuardRequested";
    QString before;
    QString after;
};

struct RenameActionRequested {
    static constexpr std::string_view eventName = "RenameActionRequested";
    QString before;
    QString after;
};

struct SetTransitionDelayRequested {
    static constexpr std::string_view eventName = "SetTransitionDelayRequested";
    quint64 id = 0;
    int delayMs = 0;
};

struct RetargetTransitionRequested {
    static constexpr std::string_view eventName = "RetargetTransitionRequested";
    quint64 id = 0;
    quint64 from = 0;
    quint64 to = 0;
    QList<quint64> targets = {};
};

// Release-commit only, like MoveStateRequested: sent once per finished pill drag.
struct MoveTransitionLabelRequested {
    static constexpr std::string_view eventName = "MoveTransitionLabelRequested";
    quint64 id = 0;
    QPointF offset;  // from the routed anchor; (0,0) resets
    // Opt-in: a pill move is offset-only and leaves the wire (bendpoints included)
    // stationary, so clearing bendpoints is never a default side effect.
    bool resetBendpoints = false;
};

// The plan is computed where the font metrics live (DocumentSession::runAutoLayout);
// the command applies it under one Transaction, so a whole auto layout is one undo
// step. Entries equal to the current machine are skipped.
struct ApplyLayoutPlanRequested {
    static constexpr std::string_view eventName = "ApplyLayoutPlanRequested";
    QVector<StatePlacement> states;
    QVector<LabelPlacement> labels;
};

struct SetTransitionBendpointsRequested {
    static constexpr std::string_view eventName = "SetTransitionBendpointsRequested";
    quint64 id = 0;
    QVector<QPointF> bendpoints = {};
};

struct SetTransitionReenterRequested {
    static constexpr std::string_view eventName = "SetTransitionReenterRequested";
    quint64 id = 0;
    bool reenter = false;
};

struct SetTransitionAlwaysRequested {
    static constexpr std::string_view eventName = "SetTransitionAlwaysRequested";
    quint64 id = 0;
    bool always = false;
};

struct SetTransitionPayloadTypeRequested {
    static constexpr std::string_view eventName = "SetTransitionPayloadTypeRequested";
    quint64 id = 0;
    QString payloadType;
};

// Cascades: MachineDocAgent::deleteState also removes every transition
// touching this state.
struct DeleteStateRequested {
    static constexpr std::string_view eventName = "DeleteStateRequested";
    quint64 id = 0;
};

struct DeleteTransitionRequested {
    static constexpr std::string_view eventName = "DeleteTransitionRequested";
    quint64 id = 0;
};

struct AddNoteRequested {
    static constexpr std::string_view eventName = "AddNoteRequested";
    QPointF pos;
};

// Release-commit only, like MoveStateRequested: sent once on drag release,
// never per drag frame.
struct MoveNoteRequested {
    static constexpr std::string_view eventName = "MoveNoteRequested";
    quint64 id = 0;
    QPointF pos;
};

struct SetNoteTextRequested {
    static constexpr std::string_view eventName = "SetNoteTextRequested";
    quint64 id = 0;
    QString text;
};

struct DeleteNoteRequested {
    static constexpr std::string_view eventName = "DeleteNoteRequested";
    quint64 id = 0;
};

// Carries no fields: the new variable gets a minted var<id>/Int/"0" default.
struct AddContextVariableRequested {
    static constexpr std::string_view eventName = "AddContextVariableRequested";
};

struct RenameContextVariableRequested {
    static constexpr std::string_view eventName = "RenameContextVariableRequested";
    quint64 id = 0;
    QString name;
};

struct SetContextTypeRequested {
    static constexpr std::string_view eventName = "SetContextTypeRequested";
    quint64 id = 0;
    ContextType type = ContextType::Int;
};

struct SetContextInitialValueRequested {
    static constexpr std::string_view eventName = "SetContextInitialValueRequested";
    quint64 id = 0;
    QString initialValue;
};

struct DeleteContextVariableRequested {
    static constexpr std::string_view eventName = "DeleteContextVariableRequested";
    quint64 id = 0;
};

struct SetContextCustomTypeNameRequested {
    static constexpr std::string_view eventName = "SetContextCustomTypeNameRequested";
    quint64 id = 0;
    QString customTypeName;
};

struct SetExternalHeadersRequested {
    static constexpr std::string_view eventName = "SetExternalHeadersRequested";
    QStringList headers;
};

struct AddStructDefinitionRequested {
    static constexpr std::string_view eventName = "AddStructDefinitionRequested";
    StructDefinition definition;
};

struct SetStructDefinitionRequested {
    static constexpr std::string_view eventName = "SetStructDefinitionRequested";
    quint64 id = 0;
    StructDefinition definition;
};

struct DeleteStructDefinitionRequested {
    static constexpr std::string_view eventName = "DeleteStructDefinitionRequested";
    quint64 id = 0;
};

struct MachineSnapshotRequested {
    static constexpr std::string_view eventName = "MachineSnapshotRequested";
};

// ---- Facts: notification that state has changed -----------------------------

struct StateAdded {
    static constexpr std::string_view eventName = "StateAdded";
    State state;
};

struct StateRenamed {
    static constexpr std::string_view eventName = "StateRenamed";
    quint64 id = 0;
    QString name;
};

struct StateKindChanged {
    static constexpr std::string_view eventName = "StateKindChanged";
    quint64 id = 0;
    StateKind kind = StateKind::Normal;
};

struct StateMoved {
    static constexpr std::string_view eventName = "StateMoved";
    quint64 id = 0;
    QPointF pos;
};

struct EntryActionsChanged {
    static constexpr std::string_view eventName = "EntryActionsChanged";
    quint64 stateId = 0;
    QStringList entryActions;
};

struct ExitActionsChanged {
    static constexpr std::string_view eventName = "ExitActionsChanged";
    quint64 stateId = 0;
    QStringList exitActions;
};

struct DescriptionChanged {
    static constexpr std::string_view eventName = "DescriptionChanged";
    quint64 stateId = 0;
    QString description;
};

struct TagsChanged {
    static constexpr std::string_view eventName = "TagsChanged";
    quint64 stateId = 0;
    QStringList tags;
};

struct HistoryDeepChanged {
    static constexpr std::string_view eventName = "HistoryDeepChanged";
    quint64 stateId = 0;
    bool deep = false;
};

struct InvokeSrcChanged {
    static constexpr std::string_view eventName = "InvokeSrcChanged";
    quint64 stateId = 0;
    QString src;
};

struct InvokeIdChanged {
    static constexpr std::string_view eventName = "InvokeIdChanged";
    quint64 stateId = 0;
    QString invokeId;
};

struct InvokeOutputTypeChanged {
    static constexpr std::string_view eventName = "InvokeOutputTypeChanged";
    quint64 stateId = 0;
    ContextType type = ContextType::Int;
};

struct InvocationsChanged {
    static constexpr std::string_view eventName = "InvocationsChanged";
    quint64 stateId = 0;
    QVector<Invocation> invocations;
};

struct StateColorChanged {
    static constexpr std::string_view eventName = "StateColorChanged";
    quint64 id = 0;
    ElementColor color = ElementColor::Default;
};

struct TransitionColorChanged {
    static constexpr std::string_view eventName = "TransitionColorChanged";
    quint64 id = 0;
    ElementColor color = ElementColor::Default;
};

struct NoteColorChanged {
    static constexpr std::string_view eventName = "NoteColorChanged";
    quint64 id = 0;
    ElementColor color = ElementColor::Default;
};

struct MachineSelfChanged {
    static constexpr std::string_view eventName = "MachineSelfChanged";
    quint64 id = 0;
    bool machineSelf = false;
};

struct StateReparented {
    static constexpr std::string_view eventName = "StateReparented";
    quint64 id = 0;
    quint64 parentId = 0;
};

struct InitialChildChanged {
    static constexpr std::string_view eventName = "InitialChildChanged";
    quint64 stateId = 0;
    quint64 initialChildId = 0;
};

struct MachineNameChanged {
    static constexpr std::string_view eventName = "MachineNameChanged";
    QString name;
};

struct InitialStateChanged {
    static constexpr std::string_view eventName = "InitialStateChanged";
    quint64 id = 0;  // 0 = no initial state set
};

struct TransitionAdded {
    static constexpr std::string_view eventName = "TransitionAdded";
    Transition transition;
};

struct TransitionEventChanged {
    static constexpr std::string_view eventName = "TransitionEventChanged";
    quint64 id = 0;
    QString event;
};

struct TransitionGuardChanged {
    static constexpr std::string_view eventName = "TransitionGuardChanged";
    quint64 id = 0;
    QString guard;
};

struct TransitionActionChanged {
    static constexpr std::string_view eventName = "TransitionActionChanged";
    quint64 id = 0;
    QString action;
};

// Carries before/after text, not an id (see RenameGuardRequested).
struct GuardRenamed {
    static constexpr std::string_view eventName = "GuardRenamed";
    QString before;
    QString after;
};

struct ActionRenamed {
    static constexpr std::string_view eventName = "ActionRenamed";
    QString before;
    QString after;
};

struct TransitionDelayChanged {
    static constexpr std::string_view eventName = "TransitionDelayChanged";
    quint64 id = 0;
    int delayMs = 0;
};

struct TransitionRetargeted {
    static constexpr std::string_view eventName = "TransitionRetargeted";
    quint64 id = 0;
    quint64 from = 0;
    quint64 to = 0;
    QList<quint64> targets = {};
};

struct TransitionLabelMoved {
    static constexpr std::string_view eventName = "TransitionLabelMoved";
    quint64 id = 0;
    QPointF offset;
};

struct TransitionLabelRatioChanged {
    static constexpr std::string_view eventName = "TransitionLabelRatioChanged";
    quint64 id = 0;
    std::optional<qreal> ratio;
};

struct TransitionBendpointsChanged {
    static constexpr std::string_view eventName = "TransitionBendpointsChanged";
    quint64 id = 0;
    QVector<QPointF> bendpoints = {};
};

struct TransitionReenterChanged {
    static constexpr std::string_view eventName = "TransitionReenterChanged";
    quint64 id = 0;
    bool reenter = false;
};

struct TransitionAlwaysChanged {
    static constexpr std::string_view eventName = "TransitionAlwaysChanged";
    quint64 id = 0;
    bool always = false;
};

struct TransitionPayloadTypeChanged {
    static constexpr std::string_view eventName = "TransitionPayloadTypeChanged";
    quint64 id = 0;
    QString payloadType;
};

// cascadedTransitionIds repeats what the per-transition TransitionDeleted facts
// carry, so a listener need not subscribe to both.
struct StateDeleted {
    static constexpr std::string_view eventName = "StateDeleted";
    quint64 id = 0;
    QVector<quint64> cascadedTransitionIds;
};

struct TransitionDeleted {
    static constexpr std::string_view eventName = "TransitionDeleted";
    quint64 id = 0;
};

struct NoteAdded {
    static constexpr std::string_view eventName = "NoteAdded";
    Note note;
};

struct NoteMoved {
    static constexpr std::string_view eventName = "NoteMoved";
    quint64 id = 0;
    QPointF pos;
};

struct NoteTextChanged {
    static constexpr std::string_view eventName = "NoteTextChanged";
    quint64 id = 0;
    QString text;
};

struct NoteDeleted {
    static constexpr std::string_view eventName = "NoteDeleted";
    quint64 id = 0;
};

struct ContextVariableAdded {
    static constexpr std::string_view eventName = "ContextVariableAdded";
    ContextVariable variable;
};

struct ContextVariableRenamed {
    static constexpr std::string_view eventName = "ContextVariableRenamed";
    quint64 id = 0;
    QString name;
};

struct ContextTypeChanged {
    static constexpr std::string_view eventName = "ContextTypeChanged";
    quint64 id = 0;
    ContextType type = ContextType::Int;
};

struct ContextInitialValueChanged {
    static constexpr std::string_view eventName = "ContextInitialValueChanged";
    quint64 id = 0;
    QString initialValue;
};

struct ContextVariableDeleted {
    static constexpr std::string_view eventName = "ContextVariableDeleted";
    quint64 id = 0;
};

struct ContextCustomTypeNameChanged {
    static constexpr std::string_view eventName = "ContextCustomTypeNameChanged";
    quint64 id = 0;
    QString customTypeName;
};

struct ExternalHeadersChanged {
    static constexpr std::string_view eventName = "ExternalHeadersChanged";
    QStringList headers;
};

struct StructDefinitionAdded {
    static constexpr std::string_view eventName = "StructDefinitionAdded";
    StructDefinition definition;
};

struct StructDefinitionChanged {
    static constexpr std::string_view eventName = "StructDefinitionChanged";
    quint64 id = 0;
    StructDefinition definition;
};

struct StructDefinitionDeleted {
    static constexpr std::string_view eventName = "StructDefinitionDeleted";
    quint64 id = 0;
};

struct MachineSnapshotPublished {
    static constexpr std::string_view eventName = "MachineSnapshotPublished";
    Machine machine;
};

}  // namespace app::events
