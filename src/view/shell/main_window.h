#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <QMainWindow>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include "infra/io_dispatcher.h"
#include "infra/project_io.h"  // Project -- held by value below (currentProject_)
#include "infra/recent_projects.h"  // RecentProjects -- held by value below (recentProjects_)
#include "infra/settings_store.h"
#include "model/sim_events.h"
#include "view/canvas/canvas_presenter.h"  // SelectionKind
#include "view/geometry/edge_router.h"       // EdgeStyle (View > Edge Style menu)
#include "view/shell/editor_group.h"      // EditorGroup + DropZone (handleMachineDrop)

class QAction;
class QMenu;
class QPoint;
class QSplitter;
class QLabel;
class QShowEvent;
class QTableWidget;
class QTabWidget;

namespace ordo::qt {
class ViewHost;
}

namespace app {

class ActivityRail;
class DocumentSession;
class EditorView;
class FloatingEditorWindow;
class MachinesPanel;
class LogicPanel;
class LogicAdapter;
class InspectorPanel;
class InspectorAdapter;
class TracePanel;
class TraceAdapter;
class MachineDocAgent;
class SettingsView;
class ProgressOverlayWidget;
class McpRuntimeServer;
struct Problem;

// The multi-kernel shell: a recursive QSplitter tiling of EditorGroups over the
// open DocumentSessions, plus icon rail, MACHINES explorer, menus and status bar.
// Sidebar clicks open-or-reveal in the FOCUSED group; the rail acts on its view.
// loginFlowSession_/trafficLightSession_ alias sessions_[0]/[1] for --gui-probe
// only; replaceSessions() nulls them. Teardown: ~MainWindow() destroys the group
// tree before sessions_, since EditorViews detach from their session on destruction.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // ---- Headless probe hooks (main.cpp's --gui-probe) -----------------------
    // Each does exactly what the matching UI affordance does -- not a
    // separate code path -- so a probe run exercises the shell's real
    // wiring, not a test double.
    EditorView* focusedView() const;
    DocumentSession* loginFlowSession() const { return loginFlowSession_; }
    DocumentSession* trafficLightSession() const { return trafficLightSession_; }
    // Same as the tab context menu's "Split Right": splits the FOCUSED
    // group; the new group (seeded with a view on the same session) becomes
    // focused, and its current view is returned (non-owning).
    EditorView* debugSplitRight();
    // Same as clicking `session`'s MACHINES row: open-or-reveal in the
    // focused group.
    void debugOpenMachine(DocumentSession* session);
    // Same as clicking inside `view`: focuses its group and activates its tab.
    void debugFocusView(EditorView* view);
    // Same as clicking `stateId`'s node on `view`'s canvas (Design mode only).
    void debugSelectState(EditorView* view, quint64 stateId);
    // Same as clicking the Inspector's Code tab, then picking the file
    // whose name contains `fileNameSuffix` in its file-name combo.
    void debugShowCodeTab(const QString& fileNameSuffix);
    // The shell-wide Inspector, and the Problems table's rows as
    // "Severity|Message" strings.
    InspectorPanel* debugInspector() const { return inspectorPanel_; }
    InspectorAdapter* debugInspectorAdapter() const { return inspectorAdapter_; }
    TracePanel* debugTracePanel() const { return tracePanel_; }
    MachinesPanel* debugMachinesPanel() const { return machinesPanel_; }
    QStringList debugProblemsTexts() const;
    // The Logic panel and its Appearance-toggle action (drive it via trigger()),
    // and a lever that closes the FOCUSED group's tabs one by one, reaching the
    // "no bound machine" focus state.
    LogicPanel* debugLogicPanel() const;
    QAction* debugLogicPanelToggleAction() const { return logicPanelToggleAction_; }
    void debugCloseAllTabsInFocusedGroup();
    // Closes every tab in every docked group: emptied splits collapse and the
    // last group remains as the one empty pane (no machine bound anywhere).
    void debugCloseAllTabs();
    // The Problems table as a plain widget, for single-widget captures.
    QWidget* debugProblemsWidget() const;
    // The dialog-free core of File > Import XState v5 JSON...: imports, then
    // adopts via adoptImportedSession(). Returns the adopted session
    // (non-owning), or nullptr with *error filled; *diagnostics receives the
    // import notes either way.
    DocumentSession* debugImportXState(const QString& path, QStringList* diagnostics, QString* error);
    DocumentSession* debugImportStateMachine(const QString& path, QStringList* diagnostics, QString* error);
    void debugRenameMachine(DocumentSession* session, const QString& newName);
    void debugDeleteMachine(DocumentSession* session);
    // View > Edge Style submenu's three checkable actions, in SmallFillet/
    // LargeFillet/Curved order.
    QVector<QAction*> debugEdgeStyleActions() const { return edgeStyleActions_; }
    SettingsView* debugOpenSettings();
    void debugCloseSettings();
    SettingsView* debugSettingsView() const { return settingsView_; }
    ProgressOverlayWidget* debugProgressOverlay() const { return progressOverlay_; }
    IoDispatcher& ioDispatcher() { return ioDispatcher_; }
    const IoDispatcher& ioDispatcher() const { return ioDispatcher_; }
    SettingsStore& settingsStore() { return settingsStore_; }
    const SettingsStore& settingsStore() const { return settingsStore_; }
    McpRuntimeServer* mcpServer() const { return mcpServer_.get(); }
    DocumentSession* createNewMachine(const QString& name = QString());
    DocumentSession* ensureActiveSession(const QString& fallbackName = QStringLiteral("Machine 1"));
    EditorView* ensureActiveView();
    bool closeProject(bool promptIfDirty = true);
    QAction* debugCloseProjectAction() const { return closeProjectAction_; }
    // Edit > Auto Layout...
    QAction* debugAutoLayoutAction() const { return autoLayoutAction_; }
    int debugSessionCount() const { return static_cast<int>(sessions_.size()); }

    // The recent-projects list, the File > Open Recent menu (rebuilt on
    // aboutToShow), and a dialog-free openProjectAt() that skips the
    // "replaces every open machine?" confirmation.
    RecentProjects& debugRecentProjects() { return recentProjects_; }
    QMenu* debugOpenRecentMenu() const { return openRecentMenu_; }
    void debugOpenProjectAt(const QString& path);

    // Reopens the last active project on startup if enabled and available.
    // Called from main.cpp's normal-run branch after window.show().
    void tryRestoreLastProject(bool noRestore = false);

protected:
    // Fits every already-open view once, the first time the window is visible:
    // CanvasView::centerAndFit() needs a non-zero viewport, which views bound
    // in the constructor lack. Views opened later fit themselves on bind.
    void showEvent(QShowEvent* event) override;

private:
    void setupCentralWidget();
    void setupToolbar();
    void setupMenuBar();
    void setupStatusBar();
    void setupShortcuts();

    EditorGroup* createGroup();
    // Splits `group`'s splitter slot in `orientation`; the new group is
    // seeded with a view on `seedSession` (a machine drop) or, when that is
    // null, on `group`'s current session (the context-menu verbs, VSCode
    // behavior) -- and becomes focused. `before` = Split Left/Up (insert
    // before `group`) vs. Split Right/Down (insert after).
    EditorGroup* splitGroup(EditorGroup* group, Qt::Orientation orientation, bool before,
                             DocumentSession* seedSession = nullptr);
    // A machine drag from the explorer dropped on `group` (see
    // EditorGroup::machineDropRequested): validates sessionTag against
    // sessions_ -- a stale token is a no-op -- then opens-or-reveals
    // (Center) or splits (edges).
    void handleMachineDrop(EditorGroup* group, quintptr sessionTag, DropZone zone);
    void setFocusedGroup(EditorGroup* group);
    // Collapses an emptied group out of the splitter tree (unless it is the
    // last one, which stays and shows its empty hint), unwrapping any
    // splitter left with a single child.
    void handleGroupEmptied(EditorGroup* group);
    void handleMachineActivated(DocumentSession* session);
    void showTabContextMenu(EditorGroup* group, int tabIndex, const QPoint& globalPos);
    void syncToolbarToFocusedView();
    void refreshStatusBar();

    // ---- Floating windows (tab context menu's "Move into New Window") -------
    // A float's group is wired like any other but never joins groups_ (the
    // rootSplitter_ tree only); windowOfGroup() tells the two kinds apart.
    void moveTabToNewWindow(EditorGroup* group, int tabIndex);
    void moveTabBackToMain(EditorGroup* floatingGroup, int tabIndex);
    void handleFloatingWindowClosing(FloatingEditorWindow* window);
    FloatingEditorWindow* windowOfGroup(EditorGroup* group) const;

    // Destroys the previous inspector/trace/logic hosts and, if the focused
    // group has a bound view, builds fresh ViewHosts and adapters on its
    // session's kernel (an adapter cannot be rebound to another kernel). Also
    // re-queries the view's currentSelection() so the Inspector starts on the
    // right tab. Called when focus, the current tab, or its session changes.
    void rebindPanelsToFocusedSession();

    void triggerSetMode(events::Mode mode);
    void triggerRun();
    void triggerPause();
    void triggerReset();
    void triggerBack();
    void triggerUndo();
    void triggerRedo();
    void triggerFit();
    // setEdgeStyle(style) is process-global, so this reroutes EVERY open
    // session by sending each kernel a MachineSnapshotRequested; attached
    // presenters rebuild their scenes from the resulting fact.
    void triggerSetEdgeStyle(EdgeStyle style);
    // Edit > Auto Layout... and the empty-canvas menu entry: the dialog opens
    // on the stored autoLayout.* options; Apply stores them and lays `view`'s
    // machine out as one undo step. No-op without a bound session or outside
    // Design mode.
    void runAutoLayoutDialog(EditorView* view);

    // Validates the focused machine and populates the Problems tab; any Error
    // writes nothing (status-bar message only), warnings alone proceed.
    // Never called from the headless --gui-probe/--codegen-check paths.
    void triggerGenerate();
    // Fills problemsTable_ from `problems`. `surfaceTab` also switches the
    // bottom tabs to Problems when non-empty; only the explicit Generate click
    // sets it, so live refreshes never pull the user off the Trace tab.
    void populateProblemsTable(MachineDocAgent& doc, const QVector<Problem>& problems, bool surfaceTab);
    // Re-validates the focused session's machine and repopulates the table in
    // place; driven by problemsChanged() and by every rebind.
    void refreshProblems();

    // ---- Project actions -----------------------------------------------------
    // File menu handlers -- dialogs live here, never in a headless path.
    void triggerNewMachine();
    void triggerOpenProject();
    // Open Project's dialog-free core, shared by the dialog path and Open
    // Recent entries. promptIfReplacing gates the "replaces every open
    // machine?" confirmation. Success touches recentProjects_; a failure for a
    // path that no longer exists removes it from recentProjects_.
    void openProjectAt(const QString& path, bool promptIfReplacing = true);
    // Rebuilds File > Open Recent on aboutToShow: numbered entries (full path
    // as tooltip), a separator, "Clear Recently Opened"; disabled
    // placeholders when the list is empty.
    void rebuildOpenRecentMenu();
    void triggerCloseProject();
    void triggerSaveProjectAs();
    void triggerSave();
    // XState v5 interop: file dialog and diagnostics box around the
    // dialog-free import/export cores. Import shows its diagnostics BEFORE
    // adopting the new session; Export acts on the focused machine.
    void triggerImportStateMachine();
    void triggerImportXState();
    void triggerExportStateMachine();
    void triggerExportXState();
    void exportSession(DocumentSession* session);
    void exportSessionToXState(DocumentSession* session);
    void renameMachine(DocumentSession* session);
    void deleteMachine(DocumentSession* session);
    void applyRenameMachine(DocumentSession* session, const QString& newName);
    void applyDeleteMachine(DocumentSession* session);
    void triggerOpenSettings();
    // The shared adopt-a-new-session plumbing (append to sessions_, sidebar
    // row, badge connection, open in the focused group) -- New Machine, the
    // XState import rim, and debugImportXState() all funnel through here.
    DocumentSession* adoptImportedSession(std::unique_ptr<DocumentSession> session);

    // Open Project's wholesale replacement: tears down panels, then every
    // group, THEN the old sessions_; rebuilds the sidebar, adopts
    // `projectPath`/`project`, and opens the first machine in the sole group.
    // `newSessions` must be non-empty.
    void replaceSessions(std::vector<std::unique_ptr<DocumentSession>> newSessions, QString projectPath,
                          Project project);

    // Declared before every widget pointer below -- see class comment.
    std::vector<std::unique_ptr<DocumentSession>> sessions_;
    // Non-owning; valid until a real project load (see class comment).
    DocumentSession* loginFlowSession_ = nullptr;
    DocumentSession* trafficLightSession_ = nullptr;
    IoDispatcher ioDispatcher_;
    SettingsStore settingsStore_;
    // Open Recent's backing store, load()ed in the constructor body.
    RecentProjects recentProjects_;

    // The open project; std::nullopt in the no-project boot state.
    // currentProjectPath_ is empty until a project is saved or opened.
    QString currentProjectPath_;
    std::optional<Project> currentProject_;

    MachinesPanel* machinesPanel_ = nullptr;
    QAction* logicPanelToggleAction_ = nullptr;
    QSplitter* rootSplitter_ = nullptr;
    SettingsView* settingsView_ = nullptr;
    ProgressOverlayWidget* progressOverlay_ = nullptr;
    std::vector<EditorGroup*> groups_;  // every group currently in rootSplitter_'s tree
    std::vector<FloatingEditorWindow*> floatingWindows_;  // "Move into New Window" hosts, one group each
    EditorGroup* focusedGroup_ = nullptr;  // may be a floating window's group -- see windowOfGroup()

    // One Inspector and one Trace panel for the whole shell, reflecting the
    // focused group's session (see rebindPanelsToFocusedSession()). The panel
    // pointers are Qt-parented; the ViewHosts own the adapters and destruct
    // before sessions_.
    InspectorPanel* inspectorPanel_ = nullptr;
    TracePanel* tracePanel_ = nullptr;
    std::unique_ptr<ordo::qt::ViewHost> inspectorHost_;
    InspectorAdapter* inspectorAdapter_ = nullptr;  // owned by inspectorHost_
    std::unique_ptr<ordo::qt::ViewHost> traceHost_;
    TraceAdapter* traceAdapter_ = nullptr;  // owned by traceHost_
    // Focus-bound like inspectorHost_/traceHost_.
    std::unique_ptr<ordo::qt::ViewHost> logicHost_;
    LogicAdapter* logicAdapter_ = nullptr;  // owned by logicHost_

    // The left icon rail; owns the shared QAction set, whose checked/enabled
    // state syncToolbarToFocusedView() drives from the focused view.
    ActivityRail* rail_ = nullptr;
    // Edge Style actions in SmallFillet/LargeFillet/Curved order; owned by the menu.
    QVector<QAction*> edgeStyleActions_;
    QAction* closeProjectAction_ = nullptr;
    QAction* autoLayoutAction_ = nullptr;  // Edit > Auto Layout...
    QMenu* openRecentMenu_ = nullptr;  // owned by the File menu
    QLabel* statusSummaryLabel_ = nullptr;
    // Permanent, right-aligned canvas-interaction cheat sheet in the status bar.
    QLabel* canvasHintLabel_ = nullptr;

    // Problems tab: a plain QTableWidget kept live by refreshProblems().
    // problemsConnection_ is the one connection to the FOCUSED session's
    // adapter; rebindPanelsToFocusedSession() remakes it on every focus change.
    QTabWidget* bottomTabs_ = nullptr;
    QTableWidget* problemsTable_ = nullptr;
    QMetaObject::Connection problemsConnection_;
    QString lastGeneratedDir_;  // QFileDialog::getExistingDirectory's remembered default

    std::unique_ptr<McpRuntimeServer> mcpServer_;

    bool shownOnce_ = false;  // guards showEvent()'s one-time initial fit pass
};

}  // namespace app
