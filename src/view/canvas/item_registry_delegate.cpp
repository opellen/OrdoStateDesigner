#include "view/canvas/item_registry_delegate.h"

#include <utility>

#include <QGraphicsItem>
#include <QGraphicsScene>

#include "view/items/note_item.h"
#include "view/items/state_item.h"
#include "view/items/transition_item.h"

namespace app {

ItemRegistryDelegate::ItemRegistryDelegate(QGraphicsScene* scene, StateItemConfigurator onStateItemCreated,
                                            TransitionConfigurator onTransitionCreated,
                                            NoteItemConfigurator onNoteItemCreated)
    : scene_(scene),
      onStateItemCreated_(std::move(onStateItemCreated)),
      onTransitionCreated_(std::move(onTransitionCreated)),
      onNoteItemCreated_(std::move(onNoteItemCreated)) {}

StateItem* ItemRegistryDelegate::createStateItem(const State& state, bool design) {
    auto* item = new StateItem(state.id, state.name, state.kind, state.entryActions, state.exitActions);
    item->setPos(state.pos);
    item->setColor(state.color);
    item->setFlag(QGraphicsItem::ItemIsMovable, design);
    item->setFlag(QGraphicsItem::ItemIsSelectable, design);

    scene_->addItem(item);
    stateItems_.insert(state.id, item);
    if (onStateItemCreated_) {
        onStateItemCreated_(item);
    }
    return item;
}

bool ItemRegistryDelegate::removeStateItem(quint64 id) {
    if (StateItem* item = stateItems_.take(id)) {
        // Plain delete, never removeItem() first: removeItem() can leave a
        // misfiled scene-index entry behind; the destructor clears it.
        delete item;
        return true;
    }
    return false;
}

void ItemRegistryDelegate::createTransitionVisual(const Transition& transition, bool design) {
    auto* edgeItem = new TransitionItem(transition.id);
    edgeItem->setColor(transition.color);
    edgeItem->setFlag(QGraphicsItem::ItemIsSelectable, design);
    scene_->addItem(edgeItem);
    transitionItems_.insert(transition.id, edgeItem);

    auto* labelItem = new TransitionLabelItem();
    labelItem->setEdgeItem(edgeItem);
    labelItem->setInteractive(design);
    scene_->addItem(labelItem);
    transitionLabels_.insert(transition.id, labelItem);

    // The label paints its edge's selection, so repaint it whenever that flips.
    edgeItem->setOnSelectedChanged([labelItem](bool) { labelItem->update(); });

    if (onTransitionCreated_) {
        onTransitionCreated_(edgeItem, labelItem);
    }
}

bool ItemRegistryDelegate::removeTransitionVisual(quint64 id) {
    bool removed = false;
    if (TransitionItem* edge = transitionItems_.take(id)) {
        delete edge;  // not removeItem() first -- see removeStateItem
        removed = true;
    }
    if (TransitionLabelItem* label = transitionLabels_.take(id)) {
        delete label;
        removed = true;
    }
    return removed;
}

NoteItem* ItemRegistryDelegate::createNoteItem(const Note& note, bool design) {
    auto* item = new NoteItem(note.id, note.text);
    item->setPos(note.pos);
    item->setColor(note.color);
    item->setFlag(QGraphicsItem::ItemIsMovable, design);
    item->setFlag(QGraphicsItem::ItemIsSelectable, design);

    scene_->addItem(item);
    noteItems_.insert(note.id, item);
    if (onNoteItemCreated_) {
        onNoteItemCreated_(item);
    }
    return item;
}

bool ItemRegistryDelegate::removeNoteItem(quint64 id) {
    if (NoteItem* item = noteItems_.take(id)) {
        delete item;
        return true;
    }
    return false;
}

void ItemRegistryDelegate::clearAllItems() {
    stateItems_.clear();
    transitionItems_.clear();
    transitionLabels_.clear();
    noteItems_.clear();
}

}  // namespace app
