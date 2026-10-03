#pragma once

#include <functional>

#include <QPoint>
#include <QPointF>
#include <QString>

#include "model/machine.h"  // StateKind

class QMenu;
class QWidget;

namespace app {

// The Inspector field a context-menu verb ("Add Entry Action", "Set Guard", ...)
// asks to focus.
enum class InspectorField {
    EntryActions,
    ExitActions,
    Description,
    Tags,
    TransitionGuard,
    TransitionAction,
    TransitionDelay,
};

// Canvas context menus. Each builder takes a context struct (what the menu
// shows) and a callbacks struct the presenter fills; no action here sends an
// intent directly.

struct StateMenuContext {
    StateKind currentKind = StateKind::Normal;
    bool isInitial = false;        // Machine::initialStateId == this state
    bool canHaveChildren = false;  // Normal/Parallel only (ReparentStateCommand's own policy)
    bool hasParent = false;        // parentId != 0 -- gates the Move Out One Level verb
    bool hasBreakpoint = false;
};

// onZoomToSelection, when set, is the first entry of every element menu.
struct StateMenuCallbacks {
    std::function<void()> onZoomToSelection;
    std::function<void()> onRename;
    std::function<void(StateKind)> onSetKind;
    std::function<void(bool checked)> onSetInitial;
    std::function<void()> onToggleBreakpoint;
    std::function<void()> onAddTransition;
    std::function<void()> onAddSelfTransition;
    std::function<void()> onAddTargetless;
    std::function<void()> onAddChildState;  // only reachable when ctx.canHaveChildren
    // Reparents to the grandparent (or root); only reachable when ctx.hasParent.
    std::function<void()> onMoveOutOneLevel;
    std::function<void(InspectorField)> onFocusField;
    std::function<void()> onDelete;
};

// Caller owns the returned menu.
QMenu* buildStateContextMenu(QWidget* parentForMenu, const StateMenuContext& ctx, const StateMenuCallbacks& cb);

struct TransitionMenuContext {
    bool hasTarget = false;      // transition.to != 0 -- gates the Reverse Direction verb
    bool hasBendpoints = false;  // !transition.manualBendpoints.isEmpty() -- gates Reset Route verb
    bool hasLabelOffset = false; // !transition.labelOffset.isNull() -- gates Reset Label Position verb
};

struct TransitionMenuCallbacks {
    std::function<void()> onZoomToSelection;
    std::function<void()> onSetEvent;
    std::function<void(InspectorField)> onFocusField;
    std::function<void()> onReverseDirection;    // only reachable when ctx.hasTarget
    std::function<void()> onResetRoute;          // only reachable when ctx.hasBendpoints
    std::function<void()> onResetLabelPosition;  // only reachable when ctx.hasLabelOffset
    std::function<void()> onDelete;
};

// Caller owns the returned menu.
QMenu* buildTransitionContextMenu(QWidget* parentForMenu, const TransitionMenuContext& ctx,
                                   const TransitionMenuCallbacks& cb);

// Shared by the "+" quick handle's dropdown and the state menu's add verbs.
struct AddTransitionCallbacks {
    std::function<void()> onAddTransition;
    std::function<void()> onAddSelfTransition;
    std::function<void()> onAddTargetless;
};
// Blocks in exec() at QCursor::pos().
void execQuickAddMenu(QWidget* parentForMenu, const AddTransitionCallbacks& cb);

struct EmptyCanvasCallbacks {
    std::function<void()> onAddStateHere;
    std::function<void()> onAddMachineEvent;
    std::function<void()> onAddNote;
    std::function<void()> onAutoLayout;  // Design menu only; omitted when unset
    // Set only when the click landed on something to zoom to; shown before Zoom to Fit.
    std::function<void()> onZoomToSelection;
    std::function<void()> onZoomToFit;
    std::function<void()> onResetView;
};
// Blocks in exec() at `globalPos`. With `design` false only the view verbs are
// shown and the add callbacks are never read.
void execEmptyCanvasMenu(QWidget* parentForMenu, QPoint globalPos, bool design, const EmptyCanvasCallbacks& cb);

}  // namespace app
