#pragma once

#include <functional>

#include <QHash>
#include <QtGlobal>

#include "model/machine.h"

class QGraphicsScene;

namespace app {

class StateItem;
class TransitionItem;
class TransitionLabelItem;
class NoteItem;

// Owns the canvas's scene-item registries and their create/remove mechanics.
// Never sends intents: each create*() adds the item to the scene and registry,
// then hands it to a presenter-supplied configurator for gesture wiring.
// Removal cleanup beyond the item itself is the caller's job.
class ItemRegistryDelegate {
public:
    using StateItemConfigurator = std::function<void(StateItem*)>;
    using TransitionConfigurator = std::function<void(TransitionItem*, TransitionLabelItem*)>;
    using NoteItemConfigurator = std::function<void(NoteItem*)>;

    ItemRegistryDelegate(QGraphicsScene* scene, StateItemConfigurator onStateItemCreated,
                          TransitionConfigurator onTransitionCreated, NoteItemConfigurator onNoteItemCreated);

    // `design` sets the item's interactive flags for the current mode.
    StateItem* createStateItem(const State& state, bool design);
    void createTransitionVisual(const Transition& transition, bool design);
    NoteItem* createNoteItem(const Note& note, bool design);

    // Deletes the item(s); returns false if `id` was not registered.
    bool removeStateItem(quint64 id);
    bool removeTransitionVisual(quint64 id);
    bool removeNoteItem(quint64 id);

    // After the presenter's scene_->clear(): forget the now-dangling pointers.
    void clearAllItems();

    const QHash<quint64, StateItem*>& stateItems() const { return stateItems_; }
    const QHash<quint64, TransitionItem*>& transitionItems() const { return transitionItems_; }
    const QHash<quint64, TransitionLabelItem*>& transitionLabels() const { return transitionLabels_; }
    const QHash<quint64, NoteItem*>& noteItems() const { return noteItems_; }

private:
    QGraphicsScene* scene_;
    StateItemConfigurator onStateItemCreated_;
    TransitionConfigurator onTransitionCreated_;
    NoteItemConfigurator onNoteItemCreated_;

    QHash<quint64, StateItem*> stateItems_;
    QHash<quint64, TransitionItem*> transitionItems_;
    QHash<quint64, TransitionLabelItem*> transitionLabels_;
    QHash<quint64, NoteItem*> noteItems_;
};

}  // namespace app
