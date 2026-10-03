#pragma once

#include <QWidget>

class QCloseEvent;

namespace app {

class EditorGroup;

// Top-level host for "Move into New Window": exactly one EditorGroup, no
// nested splitting. MainWindow creates and tracks it, and tears it down before
// sessions_ in ~MainWindow, as it does the docked group tree.
// Closing it (title-bar X, or its last tab closing) discards only the float;
// the machines live on elsewhere.
class FloatingEditorWindow : public QWidget {
    Q_OBJECT

public:
    // Adopts `group` as its sole child (Qt parent ownership). The window
    // title follows the group's current tab.
    explicit FloatingEditorWindow(EditorGroup* group);

    EditorGroup* group() const { return group_; }

signals:
    // Emitted from closeEvent -- MainWindow unregisters the window and
    // deletes it (deleteLater); the group and its views go with it, each
    // view unbinding from its still-alive session on the way down.
    void closing(FloatingEditorWindow* window);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void refreshTitle();

    EditorGroup* group_ = nullptr;
};

}  // namespace app
