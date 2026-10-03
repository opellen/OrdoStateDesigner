#include "view/shell/activity_rail.h"

#include <QAction>
#include <QActionGroup>
#include <QSizePolicy>
#include <QWidget>

#include "view/shell/icons.h"

namespace app {

ActivityRail::ActivityRail(QWidget* parent) : QToolBar(QStringLiteral("Activity"), parent) {
    setObjectName(QStringLiteral("activityRail"));  // the QSS hook
    setMovable(false);
    setFloatable(false);
    setIconSize(QSize(24, 24));
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    // No "hide this toolbar" context menu: the rail is fixed shell chrome.
    setContextMenuPolicy(Qt::PreventContextMenu);

    designAction_ = addAction(icons::icon(icons::IconId::Design), QStringLiteral("Design"));
    designAction_->setToolTip(QStringLiteral("Design mode (F8)"));
    designAction_->setCheckable(true);
    designAction_->setChecked(true);

    simulateAction_ = addAction(icons::icon(icons::IconId::Simulate), QStringLiteral("Simulate"));
    simulateAction_->setToolTip(QStringLiteral("Simulate mode"));
    simulateAction_->setCheckable(true);

    auto* modeGroup = new QActionGroup(this);
    modeGroup->setExclusive(true);
    modeGroup->addAction(designAction_);
    modeGroup->addAction(simulateAction_);

    addSeparator();

    runAction_ = addAction(icons::icon(icons::IconId::Run), QStringLiteral("Run"));
    runAction_->setToolTip(QStringLiteral("Run (F5)"));
    pauseAction_ = addAction(icons::icon(icons::IconId::Pause), QStringLiteral("Pause"));
    pauseAction_->setToolTip(QStringLiteral("Pause (F6)"));
    resetAction_ = addAction(icons::icon(icons::IconId::Reset), QStringLiteral("Reset"));
    resetAction_->setToolTip(QStringLiteral("Reset (F7)"));
    // Transport starts disabled: the boot mode is Design.
    runAction_->setEnabled(false);
    pauseAction_->setEnabled(false);
    resetAction_->setEnabled(false);

    addSeparator();

    fitAction_ = addAction(icons::icon(icons::IconId::Fit), QStringLiteral("Fit"));
    fitAction_->setToolTip(QStringLiteral("Fit the focused pane to its machine"));

    addSeparator();

    generateAction_ = addAction(icons::icon(icons::IconId::GenerateCpp), QStringLiteral("Generate C++..."));
    generateAction_->setToolTip(QStringLiteral("Validate, then write generated C++ for the focused machine"));

    auto* spacer = new QWidget(this);
    spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    addWidget(spacer);
}

}  // namespace app
