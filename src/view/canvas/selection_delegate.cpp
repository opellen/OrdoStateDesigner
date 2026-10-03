#include "view/canvas/selection_delegate.h"

#include <QGraphicsItem>
#include <QGraphicsScene>

#include "view/canvas/item_registry_delegate.h"
#include "view/items/machine_frame_item.h"
#include "view/items/note_item.h"
#include "view/items/state_item.h"
#include "view/items/transition_item.h"

namespace app {

namespace {

const Transition* findTransition(const Machine& machine, quint64 id) {
    for (const Transition& transition : machine.transitions) {
        if (transition.id == id) {
            return &transition;
        }
    }
    return nullptr;
}

}  // namespace

SelectionDelegate::SelectionDelegate(QGraphicsScene* scene, const ItemRegistryDelegate& items,
                                     std::function<const Machine*()> machine,
                                     std::function<MachineFrameItem*()> frameItem)
    : scene_(scene), items_(items), machine_(std::move(machine)), frameItem_(std::move(frameItem)) {}

CanvasSelection SelectionDelegate::derive() const {
    const QList<QGraphicsItem*> selected = scene_->selectedItems();
    if (selected.size() == 1) {
        QGraphicsItem* item = selected.first();
        if (item->type() == StateItem::Type) {
            return CanvasSelection{SelectionKind::State, static_cast<StateItem*>(item)->id()};
        }
        if (item->type() == TransitionItem::Type) {
            return CanvasSelection{SelectionKind::Transition, static_cast<TransitionItem*>(item)->id()};
        }
        if (item->type() == MachineFrameItem::Type) {
            return CanvasSelection{SelectionKind::Frame, 0};  // the frame IS the machine -- no per-instance id
        }
        if (item->type() == NoteItem::Type) {
            return CanvasSelection{SelectionKind::Note, static_cast<NoteItem*>(item)->id()};
        }
    } else if (selected.size() >= 2) {
        return CanvasSelection{SelectionKind::Multi, 0};
    }
    return CanvasSelection{};
}

bool SelectionDelegate::refreshLast() {
    const CanvasSelection selection = derive();
    if (selection.kind == lastEmitted_.kind && selection.id == lastEmitted_.id) {
        return false;
    }
    lastEmitted_ = selection;
    return true;
}

SelectionDelegate::ActionBoxInputs SelectionDelegate::actionBoxInputsFor(const CanvasSelection& selection) const {
    ActionBoxInputs inputs;
    switch (selection.kind) {
        case SelectionKind::Multi: {
            // Anchor under the union of every selected item.
            QRectF unionRect;
            for (const QGraphicsItem* item : scene_->selectedItems()) {
                unionRect = unionRect.isNull() ? item->sceneBoundingRect()
                                               : unionRect.united(item->sceneBoundingRect());
            }
            inputs.actionable = !unionRect.isNull();
            inputs.anchorRect = unionRect;
            break;
        }
        case SelectionKind::Frame: {
            MachineFrameItem* frame = frameItem_();
            inputs.actionable = frame != nullptr && frame->isVisible();
            if (inputs.actionable) {
                inputs.anchorRect = frame->sceneFrameRect();
            }
            break;
        }
        case SelectionKind::State:
            if (StateItem* item = items_.stateItems().value(selection.id, nullptr)) {
                inputs.actionable = true;
                inputs.anchorRect = item->sceneRect();
            }
            break;
        case SelectionKind::Transition:
            if (TransitionLabelItem* label = items_.transitionLabels().value(selection.id, nullptr)) {
                inputs.actionable = true;
                inputs.anchorRect = label->sceneRect();
                const Machine* machine = machine_();
                const Transition* transition =
                    machine != nullptr ? findTransition(*machine, selection.id) : nullptr;
                inputs.reverseEnabled = transition != nullptr && transition->to != 0;
            }
            break;
        default:
            break;  // None/Note: no action box
    }
    return inputs;
}

}  // namespace app
