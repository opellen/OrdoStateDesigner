#include "controller/undo_commands.h"

#include <optional>

#include "controller/undo_capture.h"
#include "model/machine_doc.h"
#include "model/sim_agent.h"
#include "model/undo_store.h"

namespace app {

namespace {

bool simulating(ordo::core::CommandContext& context) {
    auto sim = context.agentAs<SimulationAgent>(SimulationAgent::kName);
    return sim && sim->mode() == events::Mode::Simulate;
}

}  // namespace

void UndoCommand::execute(const events::UndoRequested&, ordo::core::CommandContext& context) {
    if (simulating(context)) {
        return;
    }
    auto undo = context.agentAs<UndoStore>(UndoStore::kName);
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!undo || !doc || !undo->canUndo()) {
        return;
    }
    const std::optional<TransactionDelta> delta = undo->takeUndo();
    if (!delta) {
        return;  // defensive -- canUndo() already checked
    }
    // Defensive: applyDelta requires the journal detached.
    doc->setJournal(nullptr);
    applyDelta(*doc, *delta, ApplyDirection::Backward);
}

void BeginUndoBatchCommand::execute(const events::BeginUndoBatchRequested&, ordo::core::CommandContext& context) {
    if (auto undo = context.agentAs<UndoStore>(UndoStore::kName)) {
        undo->beginBatch();
    }
}

void EndUndoBatchCommand::execute(const events::EndUndoBatchRequested&, ordo::core::CommandContext& context) {
    if (auto undo = context.agentAs<UndoStore>(UndoStore::kName)) {
        undo->endBatch();
    }
}

void RedoCommand::execute(const events::RedoRequested&, ordo::core::CommandContext& context) {
    if (simulating(context)) {
        return;
    }
    auto undo = context.agentAs<UndoStore>(UndoStore::kName);
    auto doc = context.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (!undo || !doc || !undo->canRedo()) {
        return;
    }
    const std::optional<TransactionDelta> delta = undo->takeRedo();
    if (!delta) {
        return;  // defensive -- canRedo() already checked
    }
    doc->setJournal(nullptr);
    applyDelta(*doc, *delta, ApplyDirection::Forward);
}

}  // namespace app
