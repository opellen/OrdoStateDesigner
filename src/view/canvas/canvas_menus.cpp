#include "view/canvas/canvas_menus.h"

#include <utility>

#include <QAction>
#include <QCursor>
#include <QMenu>
#include <QWidget>

namespace app {

namespace {

// The shared first entry of the element menus (see canvas_menus.h).
void addZoomToSelection(QMenu* menu, const std::function<void()>& onZoomToSelection) {
    if (!onZoomToSelection) {
        return;
    }
    QObject::connect(menu->addAction(QStringLiteral("Zoom to Selection")), &QAction::triggered, menu,
                      [onZoomToSelection] { onZoomToSelection(); });
    menu->addSeparator();
}

// Both empty-canvas menus end in the same view-verb group.
void addViewVerbs(QMenu& menu, const EmptyCanvasCallbacks& cb) {
    if (cb.onZoomToSelection) {
        QObject::connect(menu.addAction(QStringLiteral("Zoom to Selection")), &QAction::triggered, &menu,
                          [cb] { cb.onZoomToSelection(); });
    }
    QObject::connect(menu.addAction(QStringLiteral("Zoom to Fit")), &QAction::triggered, &menu,
                      [cb] { if (cb.onZoomToFit) cb.onZoomToFit(); });
    QObject::connect(menu.addAction(QStringLiteral("Reset View")), &QAction::triggered, &menu,
                      [cb] { if (cb.onResetView) cb.onResetView(); });
}

}  // namespace

QMenu* buildStateContextMenu(QWidget* parentForMenu, const StateMenuContext& ctx, const StateMenuCallbacks& cb) {
    auto* menu = new QMenu(parentForMenu);
    addZoomToSelection(menu, cb.onZoomToSelection);

    QObject::connect(menu->addAction(QStringLiteral("Rename")), &QAction::triggered, menu,
                      [cb] { if (cb.onRename) cb.onRename(); });
    menu->addSeparator();

    QMenu* typeMenu = menu->addMenu(QStringLiteral("State Type"));
    const std::pair<const char*, StateKind> kinds[] = {
        {"Normal", StateKind::Normal},
        {"Parallel", StateKind::Parallel},
        {"Final", StateKind::Final},
        {"History", StateKind::History},
    };
    for (const auto& [label, kind] : kinds) {
        QAction* action = typeMenu->addAction(QLatin1String(label));
        action->setCheckable(true);
        action->setChecked(ctx.currentKind == kind);
        QObject::connect(action, &QAction::triggered, menu,
                          [cb, kind = kind] { if (cb.onSetKind) cb.onSetKind(kind); });
    }
    QAction* initialAction = menu->addAction(QStringLiteral("Initial State"));
    initialAction->setCheckable(true);
    initialAction->setChecked(ctx.isInitial);
    QObject::connect(initialAction, &QAction::triggered, menu,
                      [cb](bool checked) { if (cb.onSetInitial) cb.onSetInitial(checked); });

    QAction* bpAction = menu->addAction(QStringLiteral("Breakpoint"));
    bpAction->setCheckable(true);
    bpAction->setChecked(ctx.hasBreakpoint);
    QObject::connect(bpAction, &QAction::triggered, menu,
                      [cb] { if (cb.onToggleBreakpoint) cb.onToggleBreakpoint(); });
    menu->addSeparator();

    QObject::connect(menu->addAction(QStringLiteral("Add Transition")), &QAction::triggered, menu,
                      [cb] { if (cb.onAddTransition) cb.onAddTransition(); });
    QObject::connect(menu->addAction(QStringLiteral("Add Self-Transition")), &QAction::triggered, menu,
                      [cb] { if (cb.onAddSelfTransition) cb.onAddSelfTransition(); });
    QObject::connect(menu->addAction(QStringLiteral("Add Targetless Transition")), &QAction::triggered, menu,
                      [cb] { if (cb.onAddTargetless) cb.onAddTargetless(); });
    // Inapplicable hierarchy verbs are omitted, not disabled.
    if (ctx.canHaveChildren) {
        QObject::connect(menu->addAction(QStringLiteral("Add Child State")), &QAction::triggered, menu,
                          [cb] { if (cb.onAddChildState) cb.onAddChildState(); });
    }
    if (ctx.hasParent) {
        QObject::connect(menu->addAction(QStringLiteral("Move Out One Level")), &QAction::triggered, menu,
                          [cb] { if (cb.onMoveOutOneLevel) cb.onMoveOutOneLevel(); });
    }
    menu->addSeparator();

    const std::pair<const char*, InspectorField> fields[] = {
        {"Add Entry Action", InspectorField::EntryActions},
        {"Add Exit Action", InspectorField::ExitActions},
        {"Add Description", InspectorField::Description},
        {"Add Tag", InspectorField::Tags},
    };
    for (const auto& [label, field] : fields) {
        QObject::connect(menu->addAction(QLatin1String(label)), &QAction::triggered, menu,
                          [cb, field = field] { if (cb.onFocusField) cb.onFocusField(field); });
    }
    menu->addSeparator();

    QObject::connect(menu->addAction(QStringLiteral("Delete")), &QAction::triggered, menu,
                      [cb] { if (cb.onDelete) cb.onDelete(); });
    return menu;
}

// Also serves the action box's "..." button, so both show the same verbs.
QMenu* buildTransitionContextMenu(QWidget* parentForMenu, const TransitionMenuContext& ctx,
                                   const TransitionMenuCallbacks& cb) {
    auto* menu = new QMenu(parentForMenu);
    addZoomToSelection(menu, cb.onZoomToSelection);
    QObject::connect(menu->addAction(QStringLiteral("Set Event")), &QAction::triggered, menu,
                      [cb] { if (cb.onSetEvent) cb.onSetEvent(); });
    const std::pair<const char*, InspectorField> fields[] = {
        {"Set Guard", InspectorField::TransitionGuard},
        {"Set Action", InspectorField::TransitionAction},
        {"Set Delay", InspectorField::TransitionDelay},
    };
    for (const auto& [label, field] : fields) {
        QObject::connect(menu->addAction(QLatin1String(label)), &QAction::triggered, menu,
                          [cb, field = field] { if (cb.onFocusField) cb.onFocusField(field); });
    }
    menu->addSeparator();
    // Reversal has no meaning without a target -- targetless stubs skip it.
    if (ctx.hasTarget) {
        QObject::connect(menu->addAction(QStringLiteral("Reverse Direction")), &QAction::triggered, menu,
                          [cb] { if (cb.onReverseDirection) cb.onReverseDirection(); });
        menu->addSeparator();
    }
    if (ctx.hasBendpoints) {
        QObject::connect(menu->addAction(QStringLiteral("Reset Route")), &QAction::triggered, menu,
                          [cb] { if (cb.onResetRoute) cb.onResetRoute(); });
        menu->addSeparator();
    }
    if (ctx.hasLabelOffset) {
        QObject::connect(menu->addAction(QStringLiteral("Reset Label Position")), &QAction::triggered, menu,
                          [cb] { if (cb.onResetLabelPosition) cb.onResetLabelPosition(); });
        menu->addSeparator();
    }
    QObject::connect(menu->addAction(QStringLiteral("Delete")), &QAction::triggered, menu,
                      [cb] { if (cb.onDelete) cb.onDelete(); });
    return menu;
}

void execQuickAddMenu(QWidget* parentForMenu, const AddTransitionCallbacks& cb) {
    QMenu menu(parentForMenu);
    QObject::connect(menu.addAction(QStringLiteral("Add Transition")), &QAction::triggered, &menu,
                      [cb] { if (cb.onAddTransition) cb.onAddTransition(); });
    QObject::connect(menu.addAction(QStringLiteral("Add Self-Transition")), &QAction::triggered, &menu,
                      [cb] { if (cb.onAddSelfTransition) cb.onAddSelfTransition(); });
    QObject::connect(menu.addAction(QStringLiteral("Add Targetless Transition")), &QAction::triggered, &menu,
                      [cb] { if (cb.onAddTargetless) cb.onAddTargetless(); });
    menu.exec(QCursor::pos());
}

void execEmptyCanvasMenu(QWidget* parentForMenu, QPoint globalPos, bool design, const EmptyCanvasCallbacks& cb) {
    // Edit verbs are rejected while simulating, so offer only the view verbs.
    if (!design) {
        QMenu menu(parentForMenu);
        addViewVerbs(menu, cb);
        menu.exec(globalPos);
        return;
    }

    QMenu menu(parentForMenu);
    QObject::connect(menu.addAction(QStringLiteral("Add State Here")), &QAction::triggered, &menu,
                      [cb] { if (cb.onAddStateHere) cb.onAddStateHere(); });
    // A root transition (from == 0, targetless) whose pill sits on the frame's right edge.
    QObject::connect(menu.addAction(QStringLiteral("Add Machine Event")), &QAction::triggered, &menu,
                      [cb] { if (cb.onAddMachineEvent) cb.onAddMachineEvent(); });
    QObject::connect(menu.addAction(QStringLiteral("Add Note")), &QAction::triggered, &menu,
                      [cb] { if (cb.onAddNote) cb.onAddNote(); });
    menu.addSeparator();
    if (cb.onAutoLayout) {
        QObject::connect(menu.addAction(QStringLiteral("Auto Layout") + QChar(0x2026)), &QAction::triggered, &menu,
                          [cb] { cb.onAutoLayout(); });
        menu.addSeparator();
    }
    addViewVerbs(menu, cb);
    menu.exec(globalPos);
}

}  // namespace app
