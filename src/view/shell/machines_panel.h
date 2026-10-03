#pragma once

#include <vector>

#include <QWidget>

class QTreeWidget;
class QTreeWidgetItem;

namespace app {

class DocumentSession;

// The left MACHINES sidebar as an explorer tree: a root row per machine (name +
// live status from StatusBadgeAdapter), its states, and each state's outgoing
// transition events. Never creates or destroys a DocumentSession, only lists
// those passed to addMachine().
class MachinesPanel : public QWidget {
    Q_OBJECT

public:
    explicit MachinesPanel(QWidget* parent = nullptr);

    // Appends one machine subtree for `session`, live-updated forever after
    // via its badge()/outlineAdapter() signals. `session` must outlive this
    // panel.
    void addMachine(DocumentSession* session);

    // Removes one machine subtree for `session`, disconnecting its badge
    // and outline adapter signals and deleting its tree item.
    void removeMachine(DocumentSession* session);

    // Refreshes the row label (name + status) for `session`.
    void refreshMachineRow(DocumentSession* session);

    // Removes every row (wholesale session replacement -- the sidebar is
    // rebuilt from scratch via addMachine() afterward), explicitly
    // disconnecting each row's adapter connections first.
    void clear();

    // Probe lever: the tree whose rows and separators the probe measures.
    QTreeWidget* debugTree() const { return tree_; }
    // Probe lever: `session`'s machine row (both ids 0), a state row, or a
    // transition row; nullptr when absent.
    QTreeWidgetItem* debugRow(DocumentSession* session, quint64 stateId, quint64 transitionId) const;

signals:
    // A row was clicked -- MainWindow decides what "activate" means (bind
    // the currently focused pane to this machine).
    void machineActivated(DocumentSession* session);
    // A state/event row was clicked: MainWindow selects the element on the
    // machine's canvas. Always emitted right after machineActivated for the
    // same click, so the machine is already revealed.
    void stateActivated(DocumentSession* session, quint64 stateId);
    void transitionActivated(DocumentSession* session, quint64 transitionId);
    // Zoom to Selection from the tree (row double-click, or any row's context
    // menu): both ids 0 means the whole machine (Zoom to Fit), otherwise the
    // state or the transition. Ids, not canvas selection -- works in Simulate.
    void zoomToSelectionRequested(DocumentSession* session, quint64 stateId, quint64 transitionId);
    void machineRenameRequested(DocumentSession* session);
    void machineDeleteRequested(DocumentSession* session);
    void machineExportRequested(DocumentSession* session);

private:
    void onItemClicked(QTreeWidgetItem* item);
    void requestZoomTo(QTreeWidgetItem* item);
    void onCustomContextMenuRequested(const QPoint& pos);
    void rebuildSubtree(DocumentSession* session);
    QString machineRowLabel(DocumentSession* session) const;

    struct Row {
        DocumentSession* session = nullptr;
        QTreeWidgetItem* item = nullptr;  // the machine's root row
    };

    QTreeWidget* tree_ = nullptr;
    std::vector<Row> rows_;
};

}  // namespace app
