#pragma once

#include <QToolBar>

class QAction;

namespace app {

// The left icon rail. Owns the shell's shared QAction set; menus reuse these
// instances so checked/enabled state cannot fork between surfaces. MainWindow
// connects them and drives their state.
// Layout: mode toggle (exclusive), Run/Pause/Reset, Fit, then Generate C++
// after a separator (the one action with filesystem side effects).
class ActivityRail : public QToolBar {
    Q_OBJECT

public:
    explicit ActivityRail(QWidget* parent = nullptr);

    QAction* designAction() const { return designAction_; }
    QAction* simulateAction() const { return simulateAction_; }
    QAction* runAction() const { return runAction_; }
    QAction* pauseAction() const { return pauseAction_; }
    QAction* resetAction() const { return resetAction_; }
    QAction* fitAction() const { return fitAction_; }
    QAction* generateAction() const { return generateAction_; }

private:
    QAction* designAction_ = nullptr;
    QAction* simulateAction_ = nullptr;
    QAction* runAction_ = nullptr;
    QAction* pauseAction_ = nullptr;
    QAction* resetAction_ = nullptr;
    QAction* fitAction_ = nullptr;
    QAction* generateAction_ = nullptr;
};

}  // namespace app
