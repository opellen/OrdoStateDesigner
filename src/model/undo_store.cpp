#include "model/undo_store.h"

#include <iterator>
#include <utility>

#include <QLoggingCategory>

#include "model/undo_events.h"

// Every stack/batch mutation logs under "sd.undo"; silence with
// QT_LOGGING_RULES="sd.undo.debug=false".
Q_LOGGING_CATEGORY(sdUndo, "sd.undo")

namespace app {

UndoStore::UndoStore(std::size_t cap) : Agent(kName), cap_(cap) {}

void UndoStore::push(TransactionDelta delta) {
    if (batchDepth_ > 0) {
        // Merge into the open batch; it lands on the stack when the outermost endBatch() closes.
        qCDebug(sdUndo) << "push: merged" << delta.ops.size() << "op(s) into open batch (depth" << batchDepth_
                        << "), pending total" << (pendingBatch_.ops.size() + delta.ops.size());
        pendingBatch_.ops.insert(pendingBatch_.ops.end(), std::make_move_iterator(delta.ops.begin()),
                                  std::make_move_iterator(delta.ops.end()));
        return;
    }

    const bool undoBefore = canUndo();
    const bool redoBefore = canRedo();

    qCDebug(sdUndo) << "push: entry with" << delta.ops.size() << "op(s), undo depth becomes"
                    << (undoStack_.size() + 1);
    undoStack_.push_back(std::move(delta));
    if (undoStack_.size() > cap_) {
        undoStack_.erase(undoStack_.begin());  // evict oldest
    }
    redoStack_.clear();  // a newly committed mutation invalidates redo history

    dispatchIfStateChanged(undoBefore, redoBefore);
}

bool UndoStore::canUndo() const {
    return !undoStack_.empty();
}

bool UndoStore::canRedo() const {
    return !redoStack_.empty();
}

void UndoStore::beginBatch() {
    ++batchDepth_;
    qCDebug(sdUndo) << "beginBatch: depth" << batchDepth_;
}

void UndoStore::endBatch() {
    if (batchDepth_ == 0) {
        qCWarning(sdUndo) << "endBatch: UNBALANCED (depth already 0) -- ignored";
        return;  // unbalanced End -- defensive no-op (the RAII scope never produces this)
    }
    --batchDepth_;
    qCDebug(sdUndo) << "endBatch: depth" << batchDepth_ << ", pending" << pendingBatch_.ops.size() << "op(s)";
    if (batchDepth_ > 0) {
        return;  // an inner pair closed; the outermost owns the push
    }
    if (pendingBatch_.ops.empty()) {
        return;  // the batch captured nothing (every edit inside was rejected)
    }
    TransactionDelta merged = std::move(pendingBatch_);
    pendingBatch_ = TransactionDelta{};
    push(std::move(merged));  // depth is 0 again, so this is the normal push path
}

std::optional<TransactionDelta> UndoStore::takeUndo() {
    if (batchDepth_ > 0) {
        qCWarning(sdUndo) << "takeUndo: refused, batch open (depth" << batchDepth_ << ")";
        return std::nullopt;  // defensive: batches are synchronous, so the UI cannot reach this
    }
    if (undoStack_.empty()) {
        return std::nullopt;  // nothing to undo -- caller should have checked canUndo()
    }
    qCDebug(sdUndo) << "takeUndo: entry with" << undoStack_.back().ops.size() << "op(s), undo depth becomes"
                    << (undoStack_.size() - 1);

    const bool undoBefore = canUndo();
    const bool redoBefore = canRedo();

    TransactionDelta delta = std::move(undoStack_.back());
    undoStack_.pop_back();
    redoStack_.push_back(delta);  // the delta moves to the other stack

    dispatchIfStateChanged(undoBefore, redoBefore);
    return delta;
}

std::optional<TransactionDelta> UndoStore::takeRedo() {
    if (batchDepth_ > 0) {
        qCWarning(sdUndo) << "takeRedo: refused, batch open (depth" << batchDepth_ << ")";
        return std::nullopt;  // see takeUndo() -- defensive symmetry
    }
    if (redoStack_.empty()) {
        return std::nullopt;  // nothing to redo -- caller should have checked canRedo()
    }
    qCDebug(sdUndo) << "takeRedo: entry with" << redoStack_.back().ops.size() << "op(s)";

    const bool undoBefore = canUndo();
    const bool redoBefore = canRedo();

    TransactionDelta delta = std::move(redoStack_.back());
    redoStack_.pop_back();
    undoStack_.push_back(delta);  // no cap eviction on this transfer, same as takeUndo()'s reverse

    dispatchIfStateChanged(undoBefore, redoBefore);
    return delta;
}

void UndoStore::clearAll() {
    const bool undoBefore = canUndo();
    const bool redoBefore = canRedo();

    undoStack_.clear();
    redoStack_.clear();
    batchDepth_ = 0;
    pendingBatch_ = TransactionDelta{};

    dispatchIfStateChanged(undoBefore, redoBefore);
}

void UndoStore::dispatchIfStateChanged(bool undoBefore, bool redoBefore) {
    if (canUndo() != undoBefore || canRedo() != redoBefore) {
        send(events::UndoStateChanged{.canUndo = canUndo(), .canRedo = canRedo()});
    }
}

}  // namespace app
