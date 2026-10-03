#pragma once

#include <vector>

#include <QMetaObject>
#include <QTabBar>
#include <QWidget>
#include <QtGlobal>

#include "view/canvas/canvas_presenter.h"  // SelectionKind (viewSelectionChanged signal)

class QDragEnterEvent;
class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QLabel;
class QPoint;
class QStackedWidget;

namespace app {

class DocumentSession;
class DropOverlay;
class EditorView;

// MIME type of the explorer -> editor-area machine drag. The payload is a
// serialized pointer value used only as an identity token: the receiver
// resolves it against its own session list, so a stale drop is a no-op.
inline constexpr char kMachineMimeType[] = "application/x-ordo-machine";

// Where a drag is hovering / a drop landed inside a group (VSCode's drop
// shading): the center opens a tab in the group itself; an edge band
// splits the group in that direction.
enum class DropZone { Center, Left, Right, Top, Bottom };

// The group's tab strip: a stock QTabBar plus two signals, a right-click's
// position and a click on the empty area past the last tab (a focus request).
class EditorTabBar : public QTabBar {
    Q_OBJECT

public:
    explicit EditorTabBar(QWidget* parent = nullptr);

signals:
    void tabContextMenuRequested(int index, const QPoint& globalPos);
    void barBackgroundClicked();

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
};

// One tile of the editor area's splitter tree: a tab strip over a stack of
// EditorViews, each tab one live DocumentSession binding. Inactive tabs keep
// their views (no rebind), so per-tab zoom/scroll survives tab switches.
//
// Owns tab/view mechanics only; focus, collapsing an emptied group and the
// context menu are MainWindow's, reached through the signals below.
class EditorGroup : public QWidget {
    Q_OBJECT

public:
    explicit EditorGroup(QWidget* parent = nullptr);

    // Appends a tab bound to `session`, makes it current, and returns its
    // view. The title/running-dot follow the session's badge from then on.
    EditorView* openView(DocumentSession* session);

    // Appends a custom widget tab (e.g. SettingsView), makes it current, and returns it.
    QWidget* openCustomTab(QWidget* widget, const QString& title, const QIcon& icon = {});
    bool revealWidget(QWidget* widget);
    int indexOfWidget(const QWidget* widget) const;

    // Activates the first existing tab bound to `session`, if any.
    // openView()+revealSession() together are VSCode's open-or-reveal.
    bool revealSession(DocumentSession* session);

    // Updates tab titles and badges for any tabs bound to `session`.
    void updateSessionTitle(DocumentSession* session);

    // Closes all tabs bound to `session` in this group.
    void closeTabsForSession(DocumentSession* session);

    void closeTab(int index);

    EditorView* currentView() const;
    DocumentSession* currentSession() const;
    int tabCount() const { return static_cast<int>(tabs_.size()); }
    EditorView* viewAt(int index) const;
    int indexOfView(const EditorView* view) const;

    // Focus ring at the group level (the old per-pane ring, one level up).
    void setFocusedGroup(bool focused);
    bool isFocusedGroup() const { return focused_; }

signals:
    void focusRequested(EditorGroup* group);
    void currentViewChanged(EditorGroup* group);
    void viewSelectionChanged(EditorGroup* group, SelectionKind kind, quint64 id);
    void viewInspectorFieldFocusRequested(EditorGroup* group, InspectorField field);
    // The empty-canvas menu's Auto Layout... on `view`; names the view because
    // the dialog lays out its session.
    void viewAutoLayoutRequested(EditorView* view);
    void emptied(EditorGroup* group);
    void tabContextMenuRequested(EditorGroup* group, int tabIndex, const QPoint& globalPos);
    // A machine drag from the MACHINES tree was dropped here. sessionTag is
    // the raw identity token from the MIME payload -- MainWindow validates
    // it against sessions_ before acting (see kMachineMimeType above).
    void machineDropRequested(EditorGroup* group, quintptr sessionTag, DropZone zone);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    struct Tab {
        EditorView* view = nullptr;
        DocumentSession* session = nullptr;  // non-owning
        QWidget* customWidget = nullptr;
        QString title;
        QMetaObject::Connection badgeConnection;
    };

    void refreshTabRow(int index);
    void syncStackToCurrent();
    DropZone dropZoneForPos(const QPoint& groupPos) const;

    EditorTabBar* tabBar_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* emptyHint_ = nullptr;      // stack page shown while no tab is open
    DropOverlay* dropOverlay_ = nullptr;  // translucent zone shading during a machine drag
    std::vector<Tab> tabs_;            // index-aligned with tabBar_'s tabs
    bool focused_ = false;
};

}  // namespace app
