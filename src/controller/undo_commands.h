#pragma once

#include <ordo/core/command.h>

#include "model/undo_events.h"

namespace app {

// Pop the top delta off UndoStore and replay it through applyDelta with the journal
// detached, so the replay cannot re-record itself. Never wrapped in UndoCaptureCommand.
// Rejected while simulating: the simulator walks the live document, and an undo could
// delete what its active state depends on.
class UndoCommand : public ordo::core::Command<events::UndoRequested> {
public:
    void execute(const events::UndoRequested& event, ordo::core::CommandContext& context) override;
};

class RedoCommand : public ordo::core::Command<events::RedoRequested> {
public:
    void execute(const events::RedoRequested& event, ordo::core::CommandContext& context) override;
};

// Forward Begin/EndUndoBatchRequested to UndoStore::beginBatch()/endBatch(). Unwrapped, and
// no Simulate gate: the edits inside a batch are gated individually, so a batch around
// rejected edits pushes nothing.
class BeginUndoBatchCommand : public ordo::core::Command<events::BeginUndoBatchRequested> {
public:
    void execute(const events::BeginUndoBatchRequested& event, ordo::core::CommandContext& context) override;
};

class EndUndoBatchCommand : public ordo::core::Command<events::EndUndoBatchRequested> {
public:
    void execute(const events::EndUndoBatchRequested& event, ordo::core::CommandContext& context) override;
};

}  // namespace app
