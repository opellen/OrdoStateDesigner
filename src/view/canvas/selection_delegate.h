#pragma once

#include <functional>

#include <QRectF>
#include <QtGlobal>

#include "model/machine.h"

class QGraphicsScene;

namespace app {

class ItemRegistryDelegate;
class MachineFrameItem;

// What is selected on the canvas. Selection is view-local Qt state, never a
// kernel fact. Frame (the machine itself) and Multi (2+ items) carry id 0.
enum class SelectionKind { None, State, Transition, Frame, Multi, Note };

struct CanvasSelection {
    SelectionKind kind = SelectionKind::None;
    quint64 id = 0;
};

// Maps the scene selection to (kind, id), dedupes selectionChanged(), and
// derives the action box's inputs. The presenter emits the signal and decides
// whether the action box shows.
class SelectionDelegate {
public:
    struct ActionBoxInputs {
        bool actionable = false;  // the selection kind has a variant AND its item is present
        QRectF anchorRect;
        bool reverseEnabled = false;  // Transition variant only: has a real target
    };

    SelectionDelegate(QGraphicsScene* scene, const ItemRegistryDelegate& items,
                      std::function<const Machine*()> machine,
                      std::function<MachineFrameItem*()> frameItem);

    // Re-derived on every call, never cached.
    CanvasSelection derive() const;

    // True when the selection differs from the last one reported; the caller
    // then emits selectionChanged(last()).
    bool refreshLast();
    CanvasSelection last() const { return lastEmitted_; }

    ActionBoxInputs actionBoxInputsFor(const CanvasSelection& selection) const;

private:
    QGraphicsScene* scene_;
    const ItemRegistryDelegate& items_;
    std::function<const Machine*()> machine_;
    std::function<MachineFrameItem*()> frameItem_;

    CanvasSelection lastEmitted_;
};

}  // namespace app
