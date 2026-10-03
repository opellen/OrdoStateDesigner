#pragma once

#include <ordo/core/command.h>

#include "model/machine_events.h"

namespace app {

// One Command per intent in model/machine_events.h; policy lives here, never in
// MachineDocAgent. Every edit except MachineSnapshotRequested is rejected while simulating.
// A mutation leaving a (from, event) group with two or more unguarded members is rejected
// (a blank event is exempt). A rejection is silent unless noted: no fact, document untouched.

class AddStateCommand : public ordo::core::Command<events::AddStateRequested> {
public:
    void execute(const events::AddStateRequested& event, ordo::core::CommandContext& context) override;
};

class RenameStateCommand : public ordo::core::Command<events::RenameStateRequested> {
public:
    void execute(const events::RenameStateRequested& event, ordo::core::CommandContext& context) override;
};

class SetStateKindCommand : public ordo::core::Command<events::SetStateKindRequested> {
public:
    void execute(const events::SetStateKindRequested& event, ordo::core::CommandContext& context) override;
};

class MoveStateCommand : public ordo::core::Command<events::MoveStateRequested> {
public:
    void execute(const events::MoveStateRequested& event, ordo::core::CommandContext& context) override;
};

class SetEntryActionsCommand : public ordo::core::Command<events::SetEntryActionsRequested> {
public:
    void execute(const events::SetEntryActionsRequested& event, ordo::core::CommandContext& context) override;
};

class SetExitActionsCommand : public ordo::core::Command<events::SetExitActionsRequested> {
public:
    void execute(const events::SetExitActionsRequested& event, ordo::core::CommandContext& context) override;
};

class SetDescriptionCommand : public ordo::core::Command<events::SetDescriptionRequested> {
public:
    void execute(const events::SetDescriptionRequested& event, ordo::core::CommandContext& context) override;
};

class SetTagsCommand : public ordo::core::Command<events::SetTagsRequested> {
public:
    void execute(const events::SetTagsRequested& event, ordo::core::CommandContext& context) override;
};

// Refuses, with a qWarning (a caller bug), a target state whose kind is not History.
// Switching a History state's kind away later leaves historyDeep as a harmless residual.
class SetHistoryDeepCommand : public ordo::core::Command<events::SetHistoryDeepRequested> {
public:
    void execute(const events::SetHistoryDeepRequested& event, ordo::core::CommandContext& context) override;
};

// No state-kind gate: any state may declare an invoke. Both commands rename every
// "done.invoke.<id>" / "error.platform.<id>" Transition::event the state owns when the
// effective invoke id changes, calling setTransitionEvent directly (bypassing
// SetTransitionEventCommand's policy).
class SetInvokeSrcCommand : public ordo::core::Command<events::SetInvokeSrcRequested> {
public:
    void execute(const events::SetInvokeSrcRequested& event, ordo::core::CommandContext& context) override;
};

class SetInvokeIdCommand : public ordo::core::Command<events::SetInvokeIdRequested> {
public:
    void execute(const events::SetInvokeIdRequested& event, ordo::core::CommandContext& context) override;
};

// A missing id is a silent no-op; any state kind may carry this field. No rename cascade.
class SetInvokeOutputTypeCommand : public ordo::core::Command<events::SetInvokeOutputTypeRequested> {
public:
    void execute(const events::SetInvokeOutputTypeRequested& event, ordo::core::CommandContext& context) override;
};

class SetInvocationsCommand : public ordo::core::Command<events::SetInvocationsRequested> {
public:
    void execute(const events::SetInvocationsRequested& event, ordo::core::CommandContext& context) override;
};

// A missing id is a silent no-op (a multi-selection recolor may race a delete).
class SetStateColorCommand : public ordo::core::Command<events::SetStateColorRequested> {
public:
    void execute(const events::SetStateColorRequested& event, ordo::core::CommandContext& context) override;
};

class SetTransitionColorCommand : public ordo::core::Command<events::SetTransitionColorRequested> {
public:
    void execute(const events::SetTransitionColorRequested& event, ordo::core::CommandContext& context) override;
};

class SetNoteColorCommand : public ordo::core::Command<events::SetNoteColorRequested> {
public:
    void execute(const events::SetNoteColorRequested& event, ordo::core::CommandContext& context) override;
};

// Setting true is refused with a qWarning unless the transition is a root, targetless
// one (from == 0 && to == 0); clearing is accepted on any existing transition.
class SetMachineSelfCommand : public ordo::core::Command<events::SetMachineSelfRequested> {
public:
    void execute(const events::SetMachineSelfRequested& event, ordo::core::CommandContext& context) override;
};

// Rejects, each with a qWarning (a caller bug): a non-zero parentId that does not exist;
// parentId == id; a parentId inside id's own subtree (the chain walk has a visited guard
// against a pre-existing cycle); and a parentId of kind Final or History.
class ReparentStateCommand : public ordo::core::Command<events::ReparentStateRequested> {
public:
    void execute(const events::ReparentStateRequested& event, ordo::core::CommandContext& context) override;
};

// Policy: rejects a non-zero initialChildId that does not name a direct
// child of stateId (including one that names no state at all).
class SetInitialChildCommand : public ordo::core::Command<events::SetInitialChildRequested> {
public:
    void execute(const events::SetInitialChildRequested& event, ordo::core::CommandContext& context) override;
};

class SetMachineNameCommand : public ordo::core::Command<events::SetMachineNameRequested> {
public:
    void execute(const events::SetMachineNameRequested& event, ordo::core::CommandContext& context) override;
};

// Policy: a non-zero id must name an existing state.
class SetInitialStateCommand : public ordo::core::Command<events::SetInitialStateRequested> {
public:
    void execute(const events::SetInitialStateRequested& event, ordo::core::CommandContext& context) override;
};

// The unguarded-duplicate check is inert today (an added transition has a blank event).
class AddTransitionCommand : public ordo::core::Command<events::AddTransitionRequested> {
public:
    void execute(const events::AddTransitionRequested& event, ordo::core::CommandContext& context) override;
};

// Rejects an event change that would leave the (from, event) group with a second
// unguarded member.
class SetTransitionEventCommand : public ordo::core::Command<events::SetTransitionEventRequested> {
public:
    void execute(const events::SetTransitionEventRequested& event, ordo::core::CommandContext& context) override;
};

// Rejects only a guard clear (blank after trim) that would leave the (from, event) group
// with a second unguarded member.
class SetTransitionGuardCommand : public ordo::core::Command<events::SetTransitionGuardRequested> {
public:
    void execute(const events::SetTransitionGuardRequested& event, ordo::core::CommandContext& context) override;
};

class SetTransitionActionCommand : public ordo::core::Command<events::SetTransitionActionRequested> {
public:
    void execute(const events::SetTransitionActionRequested& event, ordo::core::CommandContext& context) override;
};

// Hook rename cascade: renames a hook (a guard/action source that is a bare identifier,
// matched whole-string after trim) everywhere it appears; an expression that merely contains
// it is untouched. Blank `before` or before == after is a silent no-op; blank `after` is
// refused with a qWarning (it would create an unguarded fallback past the policy gate).
// RenameGuardCommand rewrites Transition::guard; RenameActionCommand also rewrites every
// State::entryActions/exitActions entry. One fact, one undo step.
class RenameGuardCommand : public ordo::core::Command<events::RenameGuardRequested> {
public:
    void execute(const events::RenameGuardRequested& event, ordo::core::CommandContext& context) override;
};

class RenameActionCommand : public ordo::core::Command<events::RenameActionRequested> {
public:
    void execute(const events::RenameActionRequested& event, ordo::core::CommandContext& context) override;
};

class SetTransitionDelayCommand : public ordo::core::Command<events::SetTransitionDelayRequested> {
public:
    void execute(const events::SetTransitionDelayRequested& event, ordo::core::CommandContext& context) override;
};

class SetTransitionReenterCommand : public ordo::core::Command<events::SetTransitionReenterRequested> {
public:
    void execute(const events::SetTransitionReenterRequested& event, ordo::core::CommandContext& context) override;
};

class SetTransitionAlwaysCommand : public ordo::core::Command<events::SetTransitionAlwaysRequested> {
public:
    void execute(const events::SetTransitionAlwaysRequested& event, ordo::core::CommandContext& context) override;
};

class SetTransitionPayloadTypeCommand : public ordo::core::Command<events::SetTransitionPayloadTypeRequested> {
public:
    void execute(const events::SetTransitionPayloadTypeRequested& event, ordo::core::CommandContext& context) override;
};

// Rejects a from-change that would leave the joined (from, event) group with a second
// unguarded member. A root transition's from is never rewireable.
class RetargetTransitionCommand : public ordo::core::Command<events::RetargetTransitionRequested> {
public:
    void execute(const events::RetargetTransitionRequested& event, ordo::core::CommandContext& context) override;
};

class MoveTransitionLabelCommand : public ordo::core::Command<events::MoveTransitionLabelRequested> {
public:
    void execute(const events::MoveTransitionLabelRequested& event, ordo::core::CommandContext& context) override;
};

// Applies an auto-layout plan for the changed entries only, in one Transaction (one undo
// step; none when nothing differs). Entries naming a missing state or transition are skipped.
class ApplyLayoutPlanCommand : public ordo::core::Command<events::ApplyLayoutPlanRequested> {
public:
    void execute(const events::ApplyLayoutPlanRequested& event, ordo::core::CommandContext& context) override;
};

class SetTransitionBendpointsCommand : public ordo::core::Command<events::SetTransitionBendpointsRequested> {
public:
    void execute(const events::SetTransitionBendpointsRequested& event, ordo::core::CommandContext& context) override;
};

// Cascades via MachineDocAgent::deleteState -- no cascade logic here.
class DeleteStateCommand : public ordo::core::Command<events::DeleteStateRequested> {
public:
    void execute(const events::DeleteStateRequested& event, ordo::core::CommandContext& context) override;
};

class DeleteTransitionCommand : public ordo::core::Command<events::DeleteTransitionRequested> {
public:
    void execute(const events::DeleteTransitionRequested& event, ordo::core::CommandContext& context) override;
};

class AddNoteCommand : public ordo::core::Command<events::AddNoteRequested> {
public:
    void execute(const events::AddNoteRequested& event, ordo::core::CommandContext& context) override;
};

class MoveNoteCommand : public ordo::core::Command<events::MoveNoteRequested> {
public:
    void execute(const events::MoveNoteRequested& event, ordo::core::CommandContext& context) override;
};

class SetNoteTextCommand : public ordo::core::Command<events::SetNoteTextRequested> {
public:
    void execute(const events::SetNoteTextRequested& event, ordo::core::CommandContext& context) override;
};

class DeleteNoteCommand : public ordo::core::Command<events::DeleteNoteRequested> {
public:
    void execute(const events::DeleteNoteRequested& event, ordo::core::CommandContext& context) override;
};

// A missing id on rename/retype/re-initialise/delete is a silent no-op. A rename that
// collides with another variable or is not a valid identifier is not rejected here; the
// validator reports it.
class AddContextVariableCommand : public ordo::core::Command<events::AddContextVariableRequested> {
public:
    void execute(const events::AddContextVariableRequested& event, ordo::core::CommandContext& context) override;
};

class RenameContextVariableCommand : public ordo::core::Command<events::RenameContextVariableRequested> {
public:
    void execute(const events::RenameContextVariableRequested& event, ordo::core::CommandContext& context) override;
};

class SetContextTypeCommand : public ordo::core::Command<events::SetContextTypeRequested> {
public:
    void execute(const events::SetContextTypeRequested& event, ordo::core::CommandContext& context) override;
};

class SetContextInitialValueCommand : public ordo::core::Command<events::SetContextInitialValueRequested> {
public:
    void execute(const events::SetContextInitialValueRequested& event, ordo::core::CommandContext& context) override;
};

class DeleteContextVariableCommand : public ordo::core::Command<events::DeleteContextVariableRequested> {
public:
    void execute(const events::DeleteContextVariableRequested& event, ordo::core::CommandContext& context) override;
};

class SetContextCustomTypeNameCommand : public ordo::core::Command<events::SetContextCustomTypeNameRequested> {
public:
    void execute(const events::SetContextCustomTypeNameRequested& event, ordo::core::CommandContext& context) override;
};

class SetExternalHeadersCommand : public ordo::core::Command<events::SetExternalHeadersRequested> {
public:
    void execute(const events::SetExternalHeadersRequested& event, ordo::core::CommandContext& context) override;
};

class AddStructDefinitionCommand : public ordo::core::Command<events::AddStructDefinitionRequested> {
public:
    void execute(const events::AddStructDefinitionRequested& event, ordo::core::CommandContext& context) override;
};

class SetStructDefinitionCommand : public ordo::core::Command<events::SetStructDefinitionRequested> {
public:
    void execute(const events::SetStructDefinitionRequested& event, ordo::core::CommandContext& context) override;
};

class DeleteStructDefinitionCommand : public ordo::core::Command<events::DeleteStructDefinitionRequested> {
public:
    void execute(const events::DeleteStructDefinitionRequested& event, ordo::core::CommandContext& context) override;
};

// Not gated by the simulate-mode policy -- reading the current topology
// must work in either mode.
class MachineSnapshotCommand : public ordo::core::Command<events::MachineSnapshotRequested> {
public:
    void execute(const events::MachineSnapshotRequested&, ordo::core::CommandContext& context) override;
};

}  // namespace app
