#include "view/shell/main_window.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QWidget>

#include <ordo/qt/view_host.h>

#include "constants/app_version.h"
#include "constants/design_tokens.h"
#include "infra/code_generator.h"
#include "infra/machine_io.h"
#include "infra/machine_validator.h"
#include "infra/mcp_runtime_server.h"
#include "infra/recent_projects.h"
#include "infra/xstate_v5_io.h"
#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/sim_agent.h"
#include "model/undo_events.h"
#include "model/undo_store.h"
#include "view/shell/activity_rail.h"
#include "view/shell/auto_layout_dialog.h"
#include "view/canvas/canvas_view.h"
#include "view/shell/document_session.h"
#include "view/shell/editor_group.h"
#include "view/shell/editor_view.h"
#include "view/shell/export_dialog.h"
#include "view/shell/floating_window.h"
#include "view/shell/inspector_panel.h"
#include "view/shell/logic_panel.h"
#include "view/shell/machines_panel.h"
#include "view/shell/progress_overlay_widget.h"
#include "view/shell/settings_view.h"
#include "view/shell/trace_panel.h"
#include "infra/settings_store.h"

namespace app {

namespace {

void setMachineName(ordo::core::Kernel& kernel, const QString& name) {
    kernel.send(events::SetMachineNameRequested{.name = name});
}

// Login-flow demo. The "canLogin" guard on Login gives the Inspector's guard
// results and the codegen tests a guard to exercise; SimulationAgent defaults
// never-toggled guards to true, so Login still fires in Simulate.
#if defined(SD_ENABLE_DEV_DEMO) && (SD_ENABLE_DEV_DEMO != 0)
void buildLoginFlowMachine(ordo::core::Kernel& kernel) {
    kernel.send(events::AddStateRequested{.pos = QPointF(80, 200)});     // id 1: LoggedOut
    kernel.send(events::AddStateRequested{.pos = QPointF(320, 180)});    // id 2: Authenticating
    kernel.send(events::AddStateRequested{.pos = QPointF(560, 120)});    // id 3: LoggedIn
    kernel.send(events::AddStateRequested{.pos = QPointF(320, 360)});    // id 4: Error

    kernel.send(events::RenameStateRequested{.id = 1, .name = QStringLiteral("LoggedOut")});
    kernel.send(events::RenameStateRequested{.id = 2, .name = QStringLiteral("Authenticating")});
    kernel.send(events::RenameStateRequested{.id = 3, .name = QStringLiteral("LoggedIn")});
    kernel.send(events::RenameStateRequested{.id = 4, .name = QStringLiteral("Error")});

    kernel.send(events::SetInitialStateRequested{.id = 1});
    kernel.send(events::SetStateKindRequested{.id = 3, .kind = StateKind::Final});

    kernel.send(events::SetEntryActionsRequested{.id = 2, .entryActions = QStringList{QStringLiteral("beginLogin()")}});
    kernel.send(events::SetEntryActionsRequested{.id = 4, .entryActions = QStringList{QStringLiteral("showError()")}});

    kernel.send(events::AddTransitionRequested{.from = 1, .to = 2});  // id 5: Login
    kernel.send(events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: Success
    kernel.send(events::AddTransitionRequested{.from = 2, .to = 4});  // id 7: Failure
    kernel.send(events::AddTransitionRequested{.from = 4, .to = 1});  // id 8: Reset

    kernel.send(events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Login")});
    kernel.send(events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Success")});
    kernel.send(events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Failure")});
    kernel.send(events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("Reset")});
    kernel.send(events::SetTransitionGuardRequested{.id = 5, .guard = QStringLiteral("canLogin")});

    setMachineName(kernel, QStringLiteral("Login Flow"));

    // Bootstrap is a document-load boundary -- history must not cross it.
    auto undo = kernel.agentAs<UndoStore>(UndoStore::kName);
    undo->clearAll();
}

// Red -(1000ms)-> Green -(1000ms)-> Yellow -(600ms)-> Red: three pure delayed
// transitions (blank event, delayMs > 0), shown as the dim-cyan "waiting" pill.
void buildTrafficLightMachine(ordo::core::Kernel& kernel) {
    kernel.send(events::AddStateRequested{.pos = QPointF(80, 160)});   // id 1: Red
    kernel.send(events::AddStateRequested{.pos = QPointF(320, 160)});  // id 2: Green
    kernel.send(events::AddStateRequested{.pos = QPointF(560, 160)});  // id 3: Yellow

    kernel.send(events::RenameStateRequested{.id = 1, .name = QStringLiteral("Red")});
    kernel.send(events::RenameStateRequested{.id = 2, .name = QStringLiteral("Green")});
    kernel.send(events::RenameStateRequested{.id = 3, .name = QStringLiteral("Yellow")});

    kernel.send(events::SetInitialStateRequested{.id = 1});

    kernel.send(events::AddTransitionRequested{.from = 1, .to = 2});  // id 4: Red -> Green
    kernel.send(events::AddTransitionRequested{.from = 2, .to = 3});  // id 5: Green -> Yellow
    kernel.send(events::AddTransitionRequested{.from = 3, .to = 1});  // id 6: Yellow -> Red

    kernel.send(events::SetTransitionDelayRequested{.id = 4, .delayMs = 1000});
    kernel.send(events::SetTransitionDelayRequested{.id = 5, .delayMs = 1000});
    kernel.send(events::SetTransitionDelayRequested{.id = 6, .delayMs = 600});
    // Event stays blank: blank event + delayMs > 0 is a pure delayed transition.

    setMachineName(kernel, QStringLiteral("Traffic Light"));

    // Bootstrap is a document-load boundary -- history must not cross it.
    auto undo = kernel.agentAs<UndoStore>(UndoStore::kName);
    undo->clearAll();
}
#endif

// Bare active-state name for the status-bar chip; empty if there is none.
QString activeStateName(DocumentSession* session) {
    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    auto sim = session->kernel().agentAs<SimulationAgent>(SimulationAgent::kName);
    if (doc == nullptr || sim == nullptr) {
        return QString();
    }
    const State* state = doc->findState(sim->activeStateId());
    return state != nullptr ? state->name : QString();
}

bool isSimulateRunning(DocumentSession* session) {
    auto sim = session->kernel().agentAs<SimulationAgent>(SimulationAgent::kName);
    return sim != nullptr && sim->mode() == events::Mode::Simulate && sim->running();
}

// "Where" column text for a Problem: resolves its non-zero id (0 = n/a) to the
// state/transition name, falling back to a bare "state N"/"transition N".
QString problemLocationText(const MachineDocAgent& doc, const Problem& problem) {
    if (problem.stateId != 0) {
        const State* state = doc.findState(problem.stateId);
        return state != nullptr ? state->name : QStringLiteral("state %1").arg(problem.stateId);
    }
    if (problem.transitionId != 0) {
        const Transition* transition = doc.findTransition(problem.transitionId);
        if (transition != nullptr) {
            const QString event = transition->event.trimmed().isEmpty() ? QStringLiteral("(delayed)") : transition->event;
            return QStringLiteral("%1 (transition %2)").arg(event).arg(problem.transitionId);
        }
        return QStringLiteral("transition %1").arg(problem.transitionId);
    }
    return QStringLiteral("machine");
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), recentProjects_(RecentProjects::defaultPath()) {
    // The title carries the version so a stale build is visible at a glance.
    setWindowTitle(QStringLiteral("Ordo State Designer ") + QLatin1String(kAppVersion));
    resize(1400, 900);

#if defined(SD_ENABLE_DEV_DEMO) && (SD_ENABLE_DEV_DEMO != 0)
    // No-project boot state: the demos are sessions_[0]/[1], which the
    // loginFlowSession_/trafficLightSession_ hooks rely on.
    auto loginFlowSession = std::make_unique<DocumentSession>(QStringLiteral("Login Flow"));
    loginFlowSession->setRelativePath(QStringLiteral("login_flow.sdm"));
    buildLoginFlowMachine(loginFlowSession->kernel());
    loginFlowSession_ = loginFlowSession.get();
    sessions_.push_back(std::move(loginFlowSession));

    auto trafficLightSession = std::make_unique<DocumentSession>(QStringLiteral("Traffic Light"));
    trafficLightSession->setRelativePath(QStringLiteral("traffic_light.sdm"));
    buildTrafficLightMachine(trafficLightSession->kernel());
    trafficLightSession_ = trafficLightSession.get();
    sessions_.push_back(std::move(trafficLightSession));
#endif

    SettingsStore::setActiveStore(&settingsStore_);
    settingsStore_.loadUser();
    recentProjects_.load();

    // Order matters: the rail must exist before setupCentralWidget() focuses
    // the first group (syncToolbarToFocusedView() reads its actions), and the
    // menu bar's Appearance toggles capture panels that setupCentralWidget() creates.
    setupToolbar();
    setupCentralWidget();
    setupMenuBar();
    setupStatusBar();
    setupShortcuts();

    progressOverlay_ = new ProgressOverlayWidget(centralWidget());

    connect(&ioDispatcher_, &IoDispatcher::ioStarted, this, [this](const events::IoStarted& ev) {
        if (ev.isModal) {
            progressOverlay_->showOperation(ev.opId, ev.title, QStringLiteral("Please wait..."), /*cancelable=*/true);
        } else {
            statusBar()->showMessage(QStringLiteral("%1: %2").arg(ev.title, ev.targetPath));
        }
    });

    connect(&ioDispatcher_, &IoDispatcher::ioProgress, this, [this](const events::IoProgress& ev) {
        if (progressOverlay_->isVisible() && progressOverlay_->currentOpId() == ev.opId) {
            progressOverlay_->setProgress(ev.percentage, ev.statusMessage);
        } else if (!ev.statusMessage.isEmpty()) {
            statusBar()->showMessage(ev.statusMessage);
        }
    });

    connect(&ioDispatcher_, &IoDispatcher::ioCompleted, this, [this](const events::IoCompleted& ev) {
        if (progressOverlay_->currentOpId() == ev.opId) {
            progressOverlay_->hideOperation();
        }
    });

    connect(&ioDispatcher_, &IoDispatcher::ioFailed, this, [this](const events::IoFailed& ev) {
        if (progressOverlay_->currentOpId() == ev.opId) {
            progressOverlay_->hideOperation();
        }
    });

    connect(progressOverlay_, &ProgressOverlayWidget::cancelRequested, this, [this](quint64 opId) {
        ioDispatcher_.cancel(opId);
    });

    mcpServer_ = std::make_unique<McpRuntimeServer>(this);
    mcpServer_->start();
}

MainWindow::~MainWindow() {
    if (mcpServer_) {
        mcpServer_->stop();
    }
    settingsStore_.flushSync();
    if (SettingsStore::activeStore() == &settingsStore_) {
        SettingsStore::setActiveStore(nullptr);
    }

    // Panel hosts go first ("view layer first"); member order alone would
    // already be safe for them.
    traceHost_.reset();
    inspectorHost_.reset();
    logicHost_.reset();

    // Destroy every view while sessions_ is still alive, so each ~EditorView()
    // (-> unbind() -> session->detachView()) sees a live session. Floating
    // windows have no Qt parent, so no cascade would ever reach them.
    for (FloatingEditorWindow* window : floatingWindows_) {
        delete window;  // cascades into its group and that group's views
    }
    floatingWindows_.clear();
    delete rootSplitter_;  // cascades delete through every docked group's tabs' real ~EditorView() now
    groups_.clear();
    focusedGroup_ = nullptr;
    rootSplitter_ = nullptr;
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    if (shownOnce_) {
        return;
    }
    shownOnce_ = true;
    for (EditorGroup* group : groups_) {
        for (int i = 0; i < group->tabCount(); ++i) {
            CanvasView* view = group->viewAt(i)->canvasView();
            if (QGraphicsScene* scene = view->scene()) {
                view->centerAndFit(scene->itemsBoundingRect());
            }
        }
    }
}

void MainWindow::setupCentralWidget() {
    auto* central = new QWidget(this);
    auto* centralLayout = new QHBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    machinesPanel_ = new MachinesPanel(central);
    for (const auto& session : sessions_) {
        machinesPanel_->addMachine(session.get());
    }
    connect(machinesPanel_, &MachinesPanel::machineActivated, this, &MainWindow::handleMachineActivated);
    // Tree click: machineActivated has already open-or-revealed the machine
    // (the panel emits these second), so the focused view is the one to select
    // on. Selection is a no-op in Simulate mode. The element is then scrolled
    // into view in every mode, zoom unchanged, one loop turn later because a
    // freshly bound view has no laid-out viewport yet.
    auto revealLater = [this](DocumentSession* session, quint64 stateId, quint64 transitionId) {
        QTimer::singleShot(0, this, [this, session, stateId, transitionId] {
            EditorView* view = focusedView();
            if (view != nullptr && view->session() == session && view->presenter() != nullptr) {
                view->presenter()->revealElements(stateId != 0 ? QList<quint64>{stateId} : QList<quint64>{},
                                                  transitionId != 0 ? QList<quint64>{transitionId} : QList<quint64>{});
            }
        });
    };
    connect(machinesPanel_, &MachinesPanel::stateActivated, this,
            [this, revealLater](DocumentSession* session, quint64 stateId) {
                EditorView* view = focusedView();
                if (view != nullptr && view->session() == session && view->presenter() != nullptr) {
                    view->presenter()->selectState(stateId);
                    revealLater(session, stateId, 0);
                }
            });
    connect(machinesPanel_, &MachinesPanel::transitionActivated, this,
            [this, revealLater](DocumentSession* session, quint64 transitionId) {
                EditorView* view = focusedView();
                if (view != nullptr && view->session() == session && view->presenter() != nullptr) {
                    view->presenter()->selectTransition(transitionId);
                    revealLater(session, 0, transitionId);
                }
            });
    // Zoom to Selection from the tree: the same open/reveal a row click runs
    // (a context-menu request never clicked), then the zoom one loop turn
    // later -- a view just bound to the machine has no laid-out viewport yet,
    // and centerAndFit() is a no-op on one.
    connect(machinesPanel_, &MachinesPanel::zoomToSelectionRequested, this,
            [this](DocumentSession* session, quint64 stateId, quint64 transitionId) {
                handleMachineActivated(session);
                QTimer::singleShot(0, this, [this, session, stateId, transitionId] {
                    EditorView* view = focusedView();
                    if (view == nullptr || view->session() != session || view->presenter() == nullptr) {
                        return;
                    }
                    if (stateId == 0 && transitionId == 0) {
                        view->presenter()->zoomToFit();
                    } else {
                        view->presenter()->zoomToElements(stateId != 0 ? QList<quint64>{stateId} : QList<quint64>{},
                                                          transitionId != 0 ? QList<quint64>{transitionId}
                                                                            : QList<quint64>{});
                    }
                });
            });
    connect(machinesPanel_, &MachinesPanel::machineRenameRequested, this, &MainWindow::renameMachine);
    connect(machinesPanel_, &MachinesPanel::machineDeleteRequested, this, &MainWindow::deleteMachine);
    connect(machinesPanel_, &MachinesPanel::machineExportRequested, this, &MainWindow::exportSession);

    // Nesting: [pane splitter | inspector] over [that | trace], to the right of
    // the sidebar. Panels collapse via their splitter handles.
    rootSplitter_ = new QSplitter(Qt::Horizontal);  // pane-tiling splitter

    inspectorPanel_ = new InspectorPanel();

    auto* topSplitter = new QSplitter(Qt::Horizontal);
    topSplitter->addWidget(rootSplitter_);
    topSplitter->addWidget(inspectorPanel_);
    topSplitter->setStretchFactor(0, 1);
    topSplitter->setStretchFactor(1, 0);
    topSplitter->setSizes({1000, 300});  // inspector ~300px

    tracePanel_ = new TracePanel();

    // Problems tab: a plain QTableWidget with no Presenter, styled to match
    // TracePanel's table.
    problemsTable_ = new QTableWidget(0, 3);
    problemsTable_->setHorizontalHeaderLabels({QStringLiteral("Severity"), QStringLiteral("Where"), QStringLiteral("Message")});
    problemsTable_->horizontalHeader()->setStretchLastSection(true);
    problemsTable_->verticalHeader()->setVisible(false);
    problemsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    problemsTable_->setSelectionMode(QAbstractItemView::NoSelection);
    problemsTable_->setStyleSheet(design::resolveRoles(
        QStringLiteral("color: {text-primary}; background: {surface-1}; gridline-color: {outline};")));

    bottomTabs_ = new QTabWidget();
    bottomTabs_->addTab(tracePanel_, QStringLiteral("Trace"));
    bottomTabs_->addTab(problemsTable_, QStringLiteral("Problems"));

    auto* verticalSplitter = new QSplitter(Qt::Vertical);
    verticalSplitter->addWidget(topSplitter);
    verticalSplitter->addWidget(bottomTabs_);
    verticalSplitter->setStretchFactor(0, 1);
    verticalSplitter->setStretchFactor(1, 0);
    verticalSplitter->setSizes({750, 150});  // trace/problems tabs ~150px

    // Guards/Actions click-to-reveal: LogicPanel emits bare ids.
    if (inspectorPanel_ != nullptr && inspectorPanel_->logicPanel() != nullptr) {
        connect(inspectorPanel_->logicPanel(), &LogicPanel::transitionRowActivated, this, [this](quint64 transitionId) {
            EditorView* view = focusedView();
            if (view != nullptr && view->presenter() != nullptr) {
                view->presenter()->selectTransition(transitionId);
            }
        });
        connect(inspectorPanel_->logicPanel(), &LogicPanel::stateRowActivated, this, [this](quint64 stateId) {
            EditorView* view = focusedView();
            if (view != nullptr && view->presenter() != nullptr) {
                view->presenter()->selectState(stateId);
            }
        });
        // Clicking a variable toggles highlighting of the transitions that use it.
        connect(inspectorPanel_->logicPanel(), &LogicPanel::contextVariableHighlightRequested, this,
                [this](const QString& varName) {
                    EditorView* view = focusedView();
                    if (view == nullptr || view->presenter() == nullptr) {
                        return;
                    }
                    CanvasPresenter* presenter = view->presenter();
                    if (presenter->activeHighlightedVariable() == varName) {
                        presenter->clearVariableHighlight();
                    } else {
                        presenter->highlightTransitionsForVariable(varName);
                    }
                });
    }

    // The sidebar is a splitter child: resizable, and collapsible by dragging
    // its handle to zero.
    auto* sidebarSplitter = new QSplitter(Qt::Horizontal, central);
    sidebarSplitter->addWidget(machinesPanel_);
    sidebarSplitter->addWidget(verticalSplitter);
    sidebarSplitter->setStretchFactor(0, 0);
    sidebarSplitter->setStretchFactor(1, 1);
    sidebarSplitter->setSizes({220, 1180});

    centralLayout->addWidget(sidebarSplitter, 1);

    setCentralWidget(central);

    // Initial layout: one group with a single tab on login-flow.
    EditorGroup* firstGroup = createGroup();
    rootSplitter_->addWidget(firstGroup);
    groups_.push_back(firstGroup);
    if (!sessions_.empty()) {
        firstGroup->openView(sessions_.front().get());
    }
    setFocusedGroup(firstGroup);

    connect(&settingsStore_, &SettingsStore::settingChanged, this,
            [this](const QString& key, const QJsonValue&, const QJsonValue& newVal, auto) {
                if (key == QStringLiteral("canvas.edgeStyle")) {
                    const QString styleStr = newVal.toString();
                    if (styleStr == QStringLiteral("SmallFillet")) {
                        triggerSetEdgeStyle(EdgeStyle::SmallFillet);
                    } else if (styleStr == QStringLiteral("LargeFillet") || styleStr == QStringLiteral("Chamfer")) {
                        triggerSetEdgeStyle(EdgeStyle::LargeFillet);
                    } else if (styleStr == QStringLiteral("Straight") || styleStr == QStringLiteral("Bezier")) {
                        triggerSetEdgeStyle(EdgeStyle::Bezier);
                    }
                }
            });
}

void MainWindow::setupToolbar() {
    // The rail owns the QActions; this routes them to the trigger*() slots.
    // setChecked() emits toggled but not triggered, so programmatic check
    // updates cannot re-enter triggerSetMode().
    rail_ = new ActivityRail(this);
    addToolBar(Qt::LeftToolBarArea, rail_);

    connect(rail_->designAction(), &QAction::triggered, this, [this] { triggerSetMode(events::Mode::Design); });
    connect(rail_->simulateAction(), &QAction::triggered, this,
            [this] { triggerSetMode(events::Mode::Simulate); });
    connect(rail_->runAction(), &QAction::triggered, this, &MainWindow::triggerRun);
    connect(rail_->pauseAction(), &QAction::triggered, this, &MainWindow::triggerPause);
    connect(rail_->resetAction(), &QAction::triggered, this, &MainWindow::triggerReset);
    connect(rail_->fitAction(), &QAction::triggered, this, &MainWindow::triggerFit);
    connect(rail_->generateAction(), &QAction::triggered, this, &MainWindow::triggerGenerate);
}

// Every slot wired here is a dialog-carrying user path; the headless
// --smoke/--gui-probe/--codegen-check runs never trigger them.
void MainWindow::setupMenuBar() {
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));

    QAction* newMachineAction = fileMenu->addAction(QStringLiteral("New Machine..."));
    newMachineAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    connect(newMachineAction, &QAction::triggered, this, &MainWindow::triggerNewMachine);

    fileMenu->addSeparator();

    QAction* openProjectAction = fileMenu->addAction(QStringLiteral("Open Project..."));
    openProjectAction->setShortcut(QKeySequence::Open);
    connect(openProjectAction, &QAction::triggered, this, &MainWindow::triggerOpenProject);

    // Rebuilt on every show so it cannot drift from recentProjects_.
    openRecentMenu_ = fileMenu->addMenu(QStringLiteral("Open Recent"));
    connect(openRecentMenu_, &QMenu::aboutToShow, this, &MainWindow::rebuildOpenRecentMenu);

    closeProjectAction_ = fileMenu->addAction(QStringLiteral("Close Project"));
    closeProjectAction_->setShortcuts({
        QKeySequence(QStringLiteral("Ctrl+Shift+W")),
        QKeySequence(QStringLiteral("Ctrl+K, F")),
    });
    closeProjectAction_->setEnabled(!currentProjectPath_.isEmpty() || !sessions_.empty());
    connect(closeProjectAction_, &QAction::triggered, this, &MainWindow::triggerCloseProject);

    QAction* saveProjectAsAction = fileMenu->addAction(QStringLiteral("Save Project As..."));
    saveProjectAsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(saveProjectAsAction, &QAction::triggered, this, &MainWindow::triggerSaveProjectAs);

    QAction* saveAction = fileMenu->addAction(QStringLiteral("Save"));
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &MainWindow::triggerSave);

    fileMenu->addSeparator();

    QAction* importMachineAction = fileMenu->addAction(QStringLiteral("Import..."));
    importMachineAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+O")));
    connect(importMachineAction, &QAction::triggered, this, &MainWindow::triggerImportStateMachine);

    QAction* exportMachineAction = fileMenu->addAction(QStringLiteral("Export..."));
    connect(exportMachineAction, &QAction::triggered, this, &MainWindow::triggerExportStateMachine);

    fileMenu->addSeparator();

    // Shares the rail's Generate C++ QAction so enabled state cannot fork.
    fileMenu->addAction(rail_->generateAction());
    rail_->generateAction()->setShortcut(QKeySequence(Qt::Key_F9));

    QMenu* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));

    // Undo/Redo own their shortcuts here; a QShortcut and a menu QAction on
    // the same key make Qt flag the activation ambiguous and drop it.
    QAction* undoAction = editMenu->addAction(QStringLiteral("Undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    connect(undoAction, &QAction::triggered, this, &MainWindow::triggerUndo);
    QAction* redoAction = editMenu->addAction(QStringLiteral("Redo"));
    redoAction->setShortcuts(
        {QKeySequence(QStringLiteral("Ctrl+Y")), QKeySequence(QStringLiteral("Ctrl+Shift+Z"))});
    connect(redoAction, &QAction::triggered, this, &MainWindow::triggerRedo);

    editMenu->addSeparator();

    // Always enabled; runAutoLayoutDialog() gates on a bound session in Design mode.
    autoLayoutAction_ = editMenu->addAction(QStringLiteral("Auto Layout") + QChar(0x2026));
    autoLayoutAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    autoLayoutAction_->setStatusTip(
        QStringLiteral("Lay out the focused machine by direction and spacing (one undo step)"));
    connect(autoLayoutAction_, &QAction::triggered, this, [this] { runAutoLayoutDialog(focusedView()); });

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    QMenu* appearanceMenu = viewMenu->addMenu(QStringLiteral("Appearance"));

    QAction* fullScreenAction = appearanceMenu->addAction(QStringLiteral("Full Screen"));
    fullScreenAction->setShortcut(QKeySequence(Qt::Key_F11));
    fullScreenAction->setCheckable(true);
    connect(fullScreenAction, &QAction::toggled, this, [this](bool on) {
        if (on) {
            showFullScreen();
        } else {
            showNormal();
        }
    });

    appearanceMenu->addSeparator();

    // Panel visibility toggles. No Ctrl+B: it is the simulator's Back.
    struct Toggle {
        QString label;
        QWidget* widget;
    };
    const Toggle toggles[] = {
        {QStringLiteral("Primary Side Bar"), machinesPanel_},
        {QStringLiteral("Secondary Side Bar (Inspector)"), inspectorPanel_},
        {QStringLiteral("Panel (Trace / Problems)"), bottomTabs_},
        {QStringLiteral("Status Bar"), statusBar()},
    };
    for (const Toggle& toggle : toggles) {
        QAction* action = appearanceMenu->addAction(toggle.label);
        action->setCheckable(true);
        action->setChecked(true);
        QWidget* widget = toggle.widget;
        connect(action, &QAction::toggled, this, [widget](bool visible) { widget->setVisible(visible); });
    }
    if (debugLogicPanel() != nullptr) {
        logicPanelToggleAction_ = appearanceMenu->addAction(QStringLiteral("Machine Logic Section"));
        logicPanelToggleAction_->setCheckable(true);
        logicPanelToggleAction_->setChecked(true);
        LogicPanel* lp = debugLogicPanel();
        connect(logicPanelToggleAction_, &QAction::toggled, this, [lp](bool visible) { lp->setVisible(visible); });
    }

    // Exclusive, checkable; the checked item reflects edgeStyle() right now.
    QMenu* edgeStyleMenu = viewMenu->addMenu(QStringLiteral("Edge Style"));
    struct EdgeStyleOption {
        QString label;
        EdgeStyle style;
    };
    const EdgeStyleOption edgeStyleOptions[] = {
        {QStringLiteral("Small fillet"), EdgeStyle::SmallFillet},
        {QStringLiteral("Large fillet"), EdgeStyle::LargeFillet},
        {QStringLiteral("Curved"), EdgeStyle::Bezier},
    };
    auto* edgeStyleGroup = new QActionGroup(this);
    edgeStyleGroup->setExclusive(true);
    for (const EdgeStyleOption& option : edgeStyleOptions) {
        QAction* action = edgeStyleMenu->addAction(option.label);
        action->setCheckable(true);
        action->setChecked(edgeStyle() == option.style);
        edgeStyleGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, style = option.style] { triggerSetEdgeStyle(style); });
        edgeStyleActions_.push_back(action);
    }

    QMenu* prefMenu = menuBar()->addMenu(QStringLiteral("&Preferences"));
    QAction* settingsAction = prefMenu->addAction(QStringLiteral("Settings"));
    settingsAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(settingsAction, &QAction::triggered, this, &MainWindow::triggerOpenSettings);

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("About Ordo State Designer"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, QStringLiteral("About Ordo State Designer"),
            QStringLiteral("<b>Ordo State Designer</b> %1<br/><br/>"
                           "A visual state machine designer, live simulator, and C++ code "
                           "generator — the capstone worked example of the ordo application "
                           "framework: one independent kernel per open machine, a "
                           "VSCode-style multi-view shell.")
                .arg(QLatin1String(kAppVersion)));
    });
}

void MainWindow::setupStatusBar() {
    statusSummaryLabel_ = new QLabel(this);
    statusBar()->addWidget(statusSummaryLabel_, 1);

    // Permanent, right-aligned canvas cheat sheet.
    canvasHintLabel_ = new QLabel(this);
    canvasHintLabel_->setText(QStringLiteral(
        "Scroll to zoom · Middle-click to pan · Double-click to add state · Drag port to connect"));
    statusBar()->addPermanentWidget(canvasHintLabel_);

    for (const auto& session : sessions_) {
        connect(session->badge(), &StatusBadgeAdapter::changed, this, &MainWindow::refreshStatusBar);
    }
    refreshStatusBar();
}

void MainWindow::setupShortcuts() {
    // F5-F8/Ctrl+B act on the focused pane's session kernel. F5 sends only
    // RunRequested; there is no implicit mode switch.
    auto* runShortcut = new QShortcut(QKeySequence(Qt::Key_F5), this);
    connect(runShortcut, &QShortcut::activated, this, &MainWindow::triggerRun);
    auto* pauseShortcut = new QShortcut(QKeySequence(Qt::Key_F6), this);
    connect(pauseShortcut, &QShortcut::activated, this, &MainWindow::triggerPause);
    auto* resetShortcut = new QShortcut(QKeySequence(Qt::Key_F7), this);
    connect(resetShortcut, &QShortcut::activated, this, &MainWindow::triggerReset);
    auto* designShortcut = new QShortcut(QKeySequence(Qt::Key_F8), this);
    connect(designShortcut, &QShortcut::activated, this, [this] { triggerSetMode(events::Mode::Design); });
    auto* backShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+B")), this);
    connect(backShortcut, &QShortcut::activated, this, &MainWindow::triggerBack);

    // Undo/Redo keys live on the Edit menu's QActions; a QShortcut here would
    // make them ambiguous and Qt would drop them.
}

EditorGroup* MainWindow::createGroup() {
    auto* group = new EditorGroup();
    connect(group, &EditorGroup::focusRequested, this, &MainWindow::setFocusedGroup);
    connect(group, &EditorGroup::emptied, this, &MainWindow::handleGroupEmptied);
    connect(group, &EditorGroup::tabContextMenuRequested, this, &MainWindow::showTabContextMenu);
    connect(group, &EditorGroup::machineDropRequested, this, &MainWindow::handleMachineDrop);
    // The FOCUSED group's current tab drives the rail and the Inspector/
    // Trace panels -- an unfocused group switching tabs (possible later via
    // drag-drop) must not steal them.
    connect(group, &EditorGroup::currentViewChanged, this, [this](EditorGroup* source) {
        if (source == focusedGroup_) {
            syncToolbarToFocusedView();
            rebindPanelsToFocusedSession();
            refreshStatusBar();
        }
    });
    connect(group, &EditorGroup::viewSelectionChanged, this,
            [this](EditorGroup* source, SelectionKind kind, quint64 id) {
                if (source == focusedGroup_ && inspectorAdapter_ != nullptr) {
                    inspectorAdapter_->setSelection(kind, id);
                }
            });
    connect(group, &EditorGroup::viewInspectorFieldFocusRequested, this,
            [this](EditorGroup* source, InspectorField field) {
                // The verb already selected its state/transition, so the
                // Inspector is on the right tab by the time this arrives.
                if (source == focusedGroup_ && inspectorPanel_ != nullptr) {
                    inspectorPanel_->focusField(field);
                }
            });
    // The empty-canvas menu's Auto Layout... acts on the view it was opened
    // on, focused or not.
    connect(group, &EditorGroup::viewAutoLayoutRequested, this, &MainWindow::runAutoLayoutDialog);
    return group;
}

EditorGroup* MainWindow::splitGroup(EditorGroup* group, Qt::Orientation orientation, bool before,
                                     DocumentSession* seedSession) {
    auto* parentSplitter = qobject_cast<QSplitter*>(group->parentWidget());
    if (parentSplitter == nullptr) {
        return nullptr;  // defensive -- a docked group's parent is always a QSplitter by construction
    }

    EditorGroup* newGroup = createGroup();
    groups_.push_back(newGroup);

    // `before` turns Split Right/Down (insert after `group`) into Split
    // Left/Up (insert before) -- the only difference between the four menu
    // verbs.
    const int offset = before ? 0 : 1;
    if (parentSplitter->count() == 1 || parentSplitter->orientation() == orientation) {
        // Either the splitter has no established orientation preference yet
        // (exactly one child), or it already matches -- either way, just
        // insert the new group as a sibling of `group`.
        parentSplitter->setOrientation(orientation);
        const int index = parentSplitter->indexOf(group);
        parentSplitter->insertWidget(index + offset, newGroup);
    } else {
        // The existing splitter already holds >1 widget in the OTHER
        // orientation: wrap `group` alone in a fresh nested splitter of the
        // requested orientation -- the standard recursive-QSplitter-tiling
        // construction.
        const int index = parentSplitter->indexOf(group);
        auto* nested = new QSplitter(orientation);
        nested->addWidget(group);  // reparents `group` out of parentSplitter into `nested`
        nested->insertWidget(offset, newGroup);
        parentSplitter->insertWidget(index, nested);
    }

    // The new group starts with a view on the drop's machine, or -- for the
    // context-menu verbs -- on the same machine the source tab shows
    // (VSCode split behavior; nothing if the source group is empty).
    DocumentSession* seed = seedSession != nullptr ? seedSession : group->currentSession();
    if (seed != nullptr) {
        newGroup->openView(seed);
    }

    // The source group's current view just had its viewport shrunk by the
    // split -- re-fit it against the new size.
    if (EditorView* view = group->currentView()) {
        if (QGraphicsScene* scene = view->canvasView()->scene()) {
            view->canvasView()->centerAndFit(scene->itemsBoundingRect());
        }
    }

    setFocusedGroup(newGroup);
    return newGroup;
}

void MainWindow::setFocusedGroup(EditorGroup* group) {
    if (focusedGroup_ == group) {
        return;
    }
    if (focusedGroup_ != nullptr) {
        focusedGroup_->setFocusedGroup(false);
    }
    focusedGroup_ = group;
    if (focusedGroup_ != nullptr) {
        focusedGroup_->setFocusedGroup(true);
    }
    syncToolbarToFocusedView();
    rebindPanelsToFocusedSession();
}

void MainWindow::handleGroupEmptied(EditorGroup* group) {
    // A floating window's group emptying closes the whole window -- there
    // is no splitter slot to collapse and no reason to keep an empty float.
    if (FloatingEditorWindow* window = windowOfGroup(group)) {
        window->close();  // closeEvent -> closing() -> handleFloatingWindowClosing()
        return;
    }

    if (groups_.size() <= 1) {
        return;  // the last group stays, showing its empty hint
    }

    auto* parentSplitter = qobject_cast<QSplitter*>(group->parentWidget());
    groups_.erase(std::remove(groups_.begin(), groups_.end(), group), groups_.end());
    if (focusedGroup_ == group) {
        focusedGroup_ = nullptr;
    }
    group->setParent(nullptr);
    // Deferred -- this handler runs from the group's own emptied() signal,
    // whose call stack is still inside the group's close handling.
    group->deleteLater();

    // Unwrap any nested splitter the removal left holding a single child --
    // the inverse of splitGroup()'s wrap step, so the tree never accumulates
    // one-child splitters.
    while (parentSplitter != nullptr && parentSplitter != rootSplitter_ && parentSplitter->count() == 1) {
        auto* grandParent = qobject_cast<QSplitter*>(parentSplitter->parentWidget());
        if (grandParent == nullptr) {
            break;
        }
        const int index = grandParent->indexOf(parentSplitter);
        QWidget* orphan = parentSplitter->widget(0);
        grandParent->insertWidget(index, orphan);  // reparents out of the trivial splitter
        parentSplitter->setParent(nullptr);
        parentSplitter->deleteLater();
        parentSplitter = grandParent;
    }

    if (focusedGroup_ == nullptr && !groups_.empty()) {
        setFocusedGroup(groups_.front());
    }
}

void MainWindow::handleMachineActivated(DocumentSession* session) {
    if (focusedGroup_ == nullptr) {
        return;
    }
    // Open-or-reveal: activates the machine's existing tab, else opens one;
    // either way currentViewChanged resyncs the rail and panels.
    // Into an empty focused group, openView() selects the first tab inside
    // addTab(), before the view is bound, so the panels rebind to "no
    // session" and never correct; rebind once more.
    const bool wasEmpty = focusedGroup_->tabCount() == 0;
    if (!focusedGroup_->revealSession(session)) {
        focusedGroup_->openView(session);
        if (wasEmpty) {
            rebindPanelsToFocusedSession();
        }
    }
}

void MainWindow::handleMachineDrop(EditorGroup* group, quintptr sessionTag, DropZone zone) {
    // The payload is never trusted as a live pointer: resolve the token
    // against sessions_; a machine closed since the drag started resolves to nothing.
    DocumentSession* session = nullptr;
    for (const auto& candidate : sessions_) {
        if (reinterpret_cast<quintptr>(candidate.get()) == sessionTag) {
            session = candidate.get();
            break;
        }
    }
    if (session == nullptr) {
        return;
    }

    // A float never splits: any zone dropped on it means "open here".
    if (windowOfGroup(group) != nullptr) {
        zone = DropZone::Center;
    }

    switch (zone) {
        case DropZone::Center:
            setFocusedGroup(group);
            if (!group->revealSession(session)) {
                group->openView(session);
            }
            break;
        case DropZone::Left:
            splitGroup(group, Qt::Horizontal, /*before=*/true, session);
            break;
        case DropZone::Right:
            splitGroup(group, Qt::Horizontal, /*before=*/false, session);
            break;
        case DropZone::Top:
            splitGroup(group, Qt::Vertical, /*before=*/true, session);
            break;
        case DropZone::Bottom:
            splitGroup(group, Qt::Vertical, /*before=*/false, session);
            break;
    }
}

void MainWindow::showTabContextMenu(EditorGroup* group, int tabIndex, const QPoint& globalPos) {
    // Focus the group first so "focused" and "menu target" cannot diverge.
    setFocusedGroup(group);
    const bool isFloating = windowOfGroup(group) != nullptr;

    QMenu menu(this);
    QAction* closeAction = menu.addAction(QStringLiteral("Close"));
    QAction* closeOthersAction = menu.addAction(QStringLiteral("Close Others"));
    closeOthersAction->setEnabled(group->tabCount() > 1);
    menu.addSeparator();
    // No nested splitting inside a float: the split verbs stay visible but disabled.
    QAction* splitUpAction = menu.addAction(QStringLiteral("Split Up"));
    QAction* splitDownAction = menu.addAction(QStringLiteral("Split Down"));
    QAction* splitLeftAction = menu.addAction(QStringLiteral("Split Left"));
    QAction* splitRightAction = menu.addAction(QStringLiteral("Split Right"));
    for (QAction* action : {splitUpAction, splitDownAction, splitLeftAction, splitRightAction}) {
        action->setEnabled(!isFloating);
    }
    menu.addSeparator();
    QAction* moveAction = menu.addAction(isFloating ? QStringLiteral("Move back into Main Window")
                                                     : QStringLiteral("Move into New Window"));

    QAction* chosen = menu.exec(globalPos);
    if (chosen == nullptr) {
        return;
    }
    if (chosen == closeAction) {
        group->closeTab(tabIndex);
    } else if (chosen == closeOthersAction) {
        for (int i = group->tabCount() - 1; i >= 0; --i) {
            if (i != tabIndex) {
                group->closeTab(i);
            }
        }
    } else if (chosen == splitUpAction) {
        splitGroup(group, Qt::Vertical, /*before=*/true);
    } else if (chosen == splitDownAction) {
        splitGroup(group, Qt::Vertical, /*before=*/false);
    } else if (chosen == splitLeftAction) {
        splitGroup(group, Qt::Horizontal, /*before=*/true);
    } else if (chosen == splitRightAction) {
        splitGroup(group, Qt::Horizontal, /*before=*/false);
    } else if (chosen == moveAction) {
        if (isFloating) {
            moveTabBackToMain(group, tabIndex);
        } else {
            moveTabToNewWindow(group, tabIndex);
        }
    }
}

void MainWindow::moveTabToNewWindow(EditorGroup* group, int tabIndex) {
    EditorView* view = group->viewAt(tabIndex);
    DocumentSession* session = view != nullptr ? view->session() : nullptr;
    if (session == nullptr) {
        return;
    }

    // A fresh binding in the float, then close the source tab; each binding
    // derives everything from the shared kernel, so no view is reparented.
    EditorGroup* floatingGroup = createGroup();
    auto* window = new FloatingEditorWindow(floatingGroup);
    floatingWindows_.push_back(window);
    connect(window, &FloatingEditorWindow::closing, this, &MainWindow::handleFloatingWindowClosing);

    floatingGroup->openView(session);
    window->show();
    group->closeTab(tabIndex);  // may collapse the source group via emptied()
    setFocusedGroup(floatingGroup);
}

void MainWindow::moveTabBackToMain(EditorGroup* floatingGroup, int tabIndex) {
    EditorView* view = floatingGroup->viewAt(tabIndex);
    DocumentSession* session = view != nullptr ? view->session() : nullptr;
    if (session == nullptr) {
        return;
    }

    // Land in the focused main group, else the first docked one (the last
    // docked group never collapses).
    EditorGroup* target = focusedGroup_ != nullptr && windowOfGroup(focusedGroup_) == nullptr ? focusedGroup_
                          : !groups_.empty()                                                   ? groups_.front()
                                                                                                : nullptr;
    if (target == nullptr) {
        return;
    }

    setFocusedGroup(target);
    if (!target->revealSession(session)) {
        target->openView(session);
    }
    floatingGroup->closeTab(tabIndex);  // last tab -> emptied() -> the float closes itself
}

void MainWindow::handleFloatingWindowClosing(FloatingEditorWindow* window) {
    floatingWindows_.erase(std::remove(floatingWindows_.begin(), floatingWindows_.end(), window),
                            floatingWindows_.end());
    if (focusedGroup_ == window->group()) {
        focusedGroup_ = nullptr;
        if (!groups_.empty()) {
            setFocusedGroup(groups_.front());
        }
    }
    // Deferred -- this runs from the window's own closeEvent.
    window->deleteLater();
}

FloatingEditorWindow* MainWindow::windowOfGroup(EditorGroup* group) const {
    for (FloatingEditorWindow* window : floatingWindows_) {
        if (window->group() == group) {
            return window;
        }
    }
    return nullptr;
}

void MainWindow::syncToolbarToFocusedView() {
    DocumentSession* session = focusedGroup_ != nullptr ? focusedGroup_->currentSession() : nullptr;
    if (session == nullptr) {
        rail_->designAction()->setChecked(true);
        rail_->simulateAction()->setChecked(false);
        rail_->runAction()->setEnabled(false);
        rail_->pauseAction()->setEnabled(false);
        rail_->resetAction()->setEnabled(false);
        return;
    }
    auto sim = session->kernel().agentAs<SimulationAgent>(SimulationAgent::kName);
    if (sim == nullptr) {
        return;
    }
    const bool simulate = sim->mode() == events::Mode::Simulate;
    rail_->designAction()->setChecked(!simulate);
    rail_->simulateAction()->setChecked(simulate);
    // The sim transport is only meaningful in Simulate mode.
    rail_->runAction()->setEnabled(simulate);
    rail_->pauseAction()->setEnabled(simulate);
    rail_->resetAction()->setEnabled(simulate);
}

void MainWindow::refreshStatusBar() {
    // Also syncs the rail, so a mode flip that bypasses triggerSetMode()
    // (direct kernel sends) still updates it. Idempotent and cheap.
    syncToolbarToFocusedView();

    if (closeProjectAction_ != nullptr) {
        closeProjectAction_->setEnabled(!currentProjectPath_.isEmpty() || !sessions_.empty());
    }

    int simulatingCount = 0;
    QStringList chips;
    for (const auto& session : sessions_) {
        if (isSimulateRunning(session.get())) {
            ++simulatingCount;
            chips << session->machineName() + QStringLiteral(": ") + activeStateName(session.get());
        }
    }

    QString text = QStringLiteral("%1 %2 simulating")
                       .arg(simulatingCount)
                       .arg(simulatingCount == 1 ? QStringLiteral("machine") : QStringLiteral("machines"));
    if (!chips.isEmpty()) {
        text += QStringLiteral("   |   ") + chips.join(QStringLiteral("    "));
    }
    statusSummaryLabel_->setText(text);
}

void MainWindow::triggerSetMode(events::Mode mode) {
    DocumentSession* session = focusedGroup_ != nullptr ? focusedGroup_->currentSession() : nullptr;
    if (session == nullptr) {
        return;
    }
    session->kernel().send(events::SetModeRequested{.mode = mode});
    syncToolbarToFocusedView();
}

void MainWindow::triggerRun() {
    DocumentSession* session = focusedGroup_ != nullptr ? focusedGroup_->currentSession() : nullptr;
    if (session == nullptr) {
        return;
    }
    session->kernel().send(events::RunRequested{});
}

void MainWindow::triggerPause() {
    DocumentSession* session = focusedGroup_ != nullptr ? focusedGroup_->currentSession() : nullptr;
    if (session == nullptr) {
        return;
    }
    session->kernel().send(events::PauseRequested{});
}

void MainWindow::triggerReset() {
    DocumentSession* session = focusedGroup_ != nullptr ? focusedGroup_->currentSession() : nullptr;
    if (session == nullptr) {
        return;
    }
    session->kernel().send(events::ResetRequested{});
}

void MainWindow::triggerBack() {
    DocumentSession* session = focusedGroup_ != nullptr ? focusedGroup_->currentSession() : nullptr;
    if (session == nullptr) {
        return;
    }
    session->kernel().send(events::BackRequested{});
}

void MainWindow::triggerUndo() {
    DocumentSession* session = focusedGroup_ != nullptr ? focusedGroup_->currentSession() : nullptr;
    if (session == nullptr) {
        return;
    }
    session->kernel().send(events::UndoRequested{});
}

void MainWindow::triggerRedo() {
    DocumentSession* session = focusedGroup_ != nullptr ? focusedGroup_->currentSession() : nullptr;
    if (session == nullptr) {
        return;
    }
    session->kernel().send(events::RedoRequested{});
}

void MainWindow::runAutoLayoutDialog(EditorView* view) {
    DocumentSession* session = view != nullptr ? view->session() : nullptr;
    if (session == nullptr) {
        return;
    }
    // Refused while simulating; checked here because runAutoLayout() would
    // report counts for a plan the command then drops.
    auto sim = session->kernel().agentAs<SimulationAgent>(SimulationAgent::kName);
    if (sim != nullptr && sim->mode() == events::Mode::Simulate) {
        return;
    }
    AutoLayoutDialog dialog(AutoLayoutDialog::loadOptions(&settingsStore_), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const AutoLayoutOptions options = dialog.options();
    AutoLayoutDialog::storeOptions(&settingsStore_, options);
    const DocumentSession::AutoLayoutRun run = session->runAutoLayout(options);
    if (!run.ran) {
        statusBar()->showMessage(QStringLiteral("Auto Layout could not run"), 3000);
        return;
    }
    QString message = QStringLiteral("Auto Layout: %1 state(s) moved, %2 label(s) changed")
                          .arg(run.movedStates)
                          .arg(run.changedLabels);
    if (run.residuals > 0) {
        message += QStringLiteral(", %1 label(s) could not be cleared").arg(run.residuals);
    }
    statusBar()->showMessage(message, 3000);
}

void MainWindow::triggerFit() {
    EditorView* view = focusedView();
    if (view == nullptr) {
        return;
    }
    CanvasView* canvas = view->canvasView();
    if (canvas->scene() != nullptr) {
        canvas->centerAndFit(canvas->scene()->itemsBoundingRect());
    }
}

void MainWindow::triggerSetEdgeStyle(EdgeStyle style) {
    setEdgeStyle(style);
    // The style is process-global: reroute EVERY open session. Each attached
    // CanvasPresenter rebuilds on the resulting MachineSnapshotPublished.
    for (const auto& session : sessions_) {
        session->kernel().send(events::MachineSnapshotRequested{});
    }
}

void MainWindow::triggerGenerate() {
    DocumentSession* session = focusedGroup_ != nullptr ? focusedGroup_->currentSession() : nullptr;
    if (session == nullptr) {
        return;
    }
    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (doc == nullptr) {
        return;
    }
    const Machine& machine = doc->machine();

    const QVector<Problem> problems = validate(machine);
    populateProblemsTable(*doc, problems, /*surfaceTab=*/true);

    bool hasError = false;
    for (const Problem& problem : problems) {
        if (problem.severity == ProblemSeverity::Error) {
            hasError = true;
            break;
        }
    }
    if (hasError) {
        statusBar()->showMessage(
            QStringLiteral("Generate C++ failed -- %1 problem(s) found, see the Problems tab").arg(problems.size()),
            6000);
        return;  // write nothing
    }

    // Warnings alone proceed. With a project whose outputDir is non-empty,
    // write straight there (relative to the project file's directory, like
    // machine paths) without a dialog; otherwise ask for a directory.
    QString rootNamespace = QStringLiteral("app::generated");
    QString outputBaseDir;
    if (currentProject_.has_value() && !currentProject_->rootNamespace.isEmpty()) {
        rootNamespace = currentProject_->rootNamespace;
    }
    if (currentProject_.has_value() && !currentProject_->outputDir.isEmpty() && !currentProjectPath_.isEmpty()) {
        outputBaseDir = QFileInfo(currentProjectPath_).dir().filePath(currentProject_->outputDir);
    } else {
        const QString startDir = !lastGeneratedDir_.isEmpty()
                                      ? lastGeneratedDir_
                                      : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        const QString chosenDir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose an output directory for generated C++"), startDir);
        if (chosenDir.isEmpty()) {
            return;  // user canceled
        }
        lastGeneratedDir_ = chosenDir;
        outputBaseDir = chosenDir;
    }

    struct GenerateResult {
        int fileCount = 0;
        int stubsWritten = 0;
        QString machineDir;
    };

    statusBar()->showMessage(QStringLiteral("Generating C++ code for %1...").arg(machine.name));

    ioDispatcher_.submitTask<GenerateResult>(
        events::IoOperationKind::GenerateCode,
        outputBaseDir,
        QStringLiteral("Generate C++"),
        /*isModal=*/false,
        [machine, rootNamespace, outputBaseDir](const std::atomic<bool>& cancelToken, IoDispatcher::ProgressCallback progress, QString* error) -> std::optional<GenerateResult> {
            if (progress) progress(20, QStringLiteral("Generating C++ AST and files..."));
            const QVector<GeneratedFile> files = generate(machine, rootNamespace);
            const QString machineDir = outputBaseDir + QStringLiteral("/generated/") + sanitizeSnakeCase(machine.name);

            if (progress) progress(50, QStringLiteral("Writing generated header and source files..."));
            if (!writeGeneratedFiles(machineDir, files, error)) {
                return std::nullopt;
            }

            if (progress) progress(80, QStringLiteral("Writing domain stubs..."));
            int stubsWritten = 0;
            if (!writeDomainStubsIfAbsent(machineDir, generateDomainStubs(machine, rootNamespace), &stubsWritten, error)) {
                return std::nullopt;
            }

            if (progress) progress(100, QStringLiteral("Generation completed"));
            return GenerateResult{
                .fileCount = static_cast<int>(files.size()),
                .stubsWritten = stubsWritten,
                .machineDir = machineDir
            };
        },
        [this](GenerateResult res) {
            statusBar()->showMessage(QStringLiteral("Generate C++ -> wrote %1 file(s) to %2 (%3)")
                                          .arg(res.fileCount)
                                          .arg(res.machineDir)
                                          .arg(res.stubsWritten > 0
                                                   ? QStringLiteral("+ %1 domain/ stub(s)").arg(res.stubsWritten)
                                                   : QStringLiteral("domain/ stubs kept")),
                                      6000);
        },
        [this](const QString& error) {
            statusBar()->showMessage(QStringLiteral("Generate C++ failed: %1").arg(error), 6000);
        }
    );
}

void MainWindow::triggerNewMachine() {
    // The new machine starts empty: no states, no fabricated Initial state.
    const QString defaultName = QStringLiteral("Machine %1").arg(sessions_.size() + 1);
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New Machine"), QStringLiteral("Machine name:"),
                                                QLineEdit::Normal, defaultName, &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;  // user canceled or cleared the field
    }

    adoptImportedSession(std::make_unique<DocumentSession>(name));
}

void MainWindow::renameMachine(DocumentSession* session) {
    if (session == nullptr) {
        return;
    }
    bool ok = false;
    const QString currentName = session->machineName();
    const QString newName = QInputDialog::getText(
        this, QStringLiteral("Rename Machine"), QStringLiteral("Machine name:"),
        QLineEdit::Normal, currentName, &ok);
    if (!ok || newName.trimmed().isEmpty() || newName.trimmed() == currentName) {
        return;
    }
    applyRenameMachine(session, newName.trimmed());
}

void MainWindow::applyRenameMachine(DocumentSession* session, const QString& newName) {
    if (session == nullptr || newName.isEmpty()) {
        return;
    }
    session->kernel().send(events::SetMachineNameRequested{.name = newName});
    session->setMachineName(newName);
    if (machinesPanel_ != nullptr) {
        machinesPanel_->refreshMachineRow(session);
    }
    for (EditorGroup* group : groups_) {
        group->updateSessionTitle(session);
    }
    for (FloatingEditorWindow* window : floatingWindows_) {
        window->group()->updateSessionTitle(session);
    }
    statusBar()->showMessage(QStringLiteral("Renamed machine to '%1'").arg(newName), 3000);
    refreshStatusBar();
}

void MainWindow::deleteMachine(DocumentSession* session) {
    if (session == nullptr) {
        return;
    }
    if (sessions_.size() <= 1) {
        QMessageBox::warning(
            this, QStringLiteral("Delete Machine"),
            QStringLiteral("Cannot delete the only open machine in the workspace."));
        return;
    }
    const auto res = QMessageBox::question(
        this, QStringLiteral("Delete Machine"),
        QStringLiteral("Are you sure you want to delete machine '%1'?").arg(session->machineName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (res != QMessageBox::Yes) {
        return;
    }
    applyDeleteMachine(session);
}

void MainWindow::applyDeleteMachine(DocumentSession* session) {
    if (session == nullptr || sessions_.size() <= 1) {
        return;
    }

    const QString deletedName = session->machineName();
    const QString relPath = session->relativePath();

    // 1. Close tabs in all docked groups
    for (EditorGroup* group : groups_) {
        group->closeTabsForSession(session);
    }
    // and floating windows
    auto floats = floatingWindows_;
    for (FloatingEditorWindow* window : floats) {
        window->group()->closeTabsForSession(session);
    }

    // 2. Clear debug probe pointers if deleting boot demo sessions
    if (loginFlowSession_ == session) {
        loginFlowSession_ = nullptr;
    }
    if (trafficLightSession_ == session) {
        trafficLightSession_ = nullptr;
    }

    // 3. If focused group has no session active, open the first remaining session
    if (focusedGroup_ != nullptr && focusedGroup_->currentSession() == nullptr) {
        for (const auto& candidate : sessions_) {
            if (candidate.get() != session) {
                focusedGroup_->openView(candidate.get());
                break;
            }
        }
    }

    // 4. Rebind panels
    rebindPanelsToFocusedSession();

    // 5. Remove from MACHINES sidebar panel
    if (machinesPanel_ != nullptr) {
        machinesPanel_->removeMachine(session);
    }

    // 6. Prune from currentProject_->machineFiles
    if (currentProject_.has_value() && !relPath.isEmpty()) {
        currentProject_->machineFiles.removeAll(relPath);
    }

    // 7. Locate and erase session from sessions_
    auto it = std::find_if(sessions_.begin(), sessions_.end(),
                           [session](const std::unique_ptr<DocumentSession>& ptr) {
                               return ptr.get() == session;
                           });
    if (it != sessions_.end()) {
        sessions_.erase(it);
    }

    // 8. Update status bar and summary
    statusBar()->showMessage(QStringLiteral("Deleted machine '%1'").arg(deletedName), 3000);
    refreshStatusBar();
}

DocumentSession* MainWindow::adoptImportedSession(std::unique_ptr<DocumentSession> session) {
    DocumentSession* raw = session.get();

    // Ensure collision-free relativePath is allocated
    if (raw->relativePath().isEmpty()) {
        QString baseName = sanitizeSnakeCase(raw->machineName());
        if (baseName.isEmpty()) {
            baseName = QStringLiteral("machine_%1").arg(sessions_.size() + 1);
        }
        QString candidate = baseName + QStringLiteral(".sdm");
        int suffix = 1;
        auto collides = [this](const QString& path) {
            for (const auto& s : sessions_) {
                if (s->relativePath().compare(path, Qt::CaseInsensitive) == 0) return true;
            }
            if (currentProject_.has_value()) {
                for (const auto& f : currentProject_->machineFiles) {
                    if (f.compare(path, Qt::CaseInsensitive) == 0) return true;
                }
            }
            return false;
        };
        while (collides(candidate)) {
            candidate = QStringLiteral("%1_%2.sdm").arg(baseName).arg(suffix++);
        }
        raw->setRelativePath(candidate);
    }

    // Register in current project manifest if a project is loaded
    if (currentProject_.has_value()) {
        if (!currentProject_->machineFiles.contains(raw->relativePath())) {
            currentProject_->machineFiles.push_back(raw->relativePath());
        }
    }

    sessions_.push_back(std::move(session));

    machinesPanel_->addMachine(raw);
    connect(raw->badge(), &StatusBadgeAdapter::changed, this, &MainWindow::refreshStatusBar);

    // Same call a MACHINES row click makes: open in the focused group.
    handleMachineActivated(raw);
    refreshStatusBar();
    return raw;
}

void MainWindow::triggerImportStateMachine() {
    const QString filters = QStringLiteral("Supported State Machines (*.sdm *.json);;State Designer Machine (*.sdm);;") +
                            MachineIoRegistry::instance().importFilterString();
    QString selectedFilter;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import State Machine"), QString(), filters, &selectedFilter);
    if (path.isEmpty()) {
        return;  // user canceled
    }

    struct MachineImportTaskResult {
        Machine machine;
        QString machineName;
        QStringList diagnostics;
        QString formatName;
    };

    statusBar()->showMessage(QStringLiteral("Importing machine from %1...").arg(path));

    ioDispatcher_.submitTask<MachineImportTaskResult>(
        events::IoOperationKind::ImportMachine,
        path,
        QStringLiteral("Import Machine"),
        /*isModal=*/true,
        [path, selectedFilter](const std::atomic<bool>& cancelToken, IoDispatcher::ProgressCallback progress, QString* error) -> std::optional<MachineImportTaskResult> {
            if (progress) progress(20, QStringLiteral("Reading machine file..."));
            if (cancelToken.load(std::memory_order_relaxed)) return std::nullopt;

            auto adapter = MachineIoRegistry::instance().formatForFilter(selectedFilter);
            if (!adapter) {
                adapter = MachineIoRegistry::instance().findAdapterByExtension(QFileInfo(path).suffix());
            }

            if (adapter) {
                const QString fmtName = adapter->descriptor().name;
                if (progress) progress(50, QStringLiteral("Parsing %1...").arg(fmtName));
                MachineImportResult res = adapter->importFile(path);
                if (!res.ok) {
                    if (error) *error = res.error;
                    return std::nullopt;
                }
                const QString name = !res.machine.name.isEmpty() ? res.machine.name : QFileInfo(path).completeBaseName();
                if (progress) progress(100, QStringLiteral("Machine parsed"));
                return MachineImportTaskResult{
                    .machine = std::move(res.machine),
                    .machineName = name,
                    .diagnostics = std::move(res.diagnostics),
                    .formatName = fmtName
                };
            } else {
                if (progress) progress(50, QStringLiteral("Parsing State Designer machine..."));
                Machine machine;
                if (!loadMachine(path, &machine, error)) {
                    return std::nullopt;
                }
                const QString name = !machine.name.isEmpty() ? machine.name : QFileInfo(path).completeBaseName();
                if (progress) progress(100, QStringLiteral("Machine parsed"));
                return MachineImportTaskResult{
                    .machine = std::move(machine),
                    .machineName = name,
                    .diagnostics = {},
                    .formatName = QStringLiteral("State Designer Machine")
                };
            }
        },
        [this, path](MachineImportTaskResult res) {
            if (!res.diagnostics.isEmpty()) {
                QMessageBox::information(
                    this, QStringLiteral("Import State Machine"),
                    QStringLiteral("Imported '%1' (%2) with %3 note(s):\n\n- %4")
                        .arg(res.machineName, res.formatName)
                        .arg(res.diagnostics.size())
                        .arg(res.diagnostics.join(QStringLiteral("\n- "))));
            }
            auto session = std::make_unique<DocumentSession>(res.machineName, std::move(res.machine));
            DocumentSession* raw = adoptImportedSession(std::move(session));
            statusBar()->showMessage(QStringLiteral("Imported %1 from %2").arg(raw->machineName(), path), 4000);
        },
        [this](const QString& error) {
            QMessageBox::warning(this, QStringLiteral("Import State Machine"),
                                 QStringLiteral("Import failed: %1").arg(error));
        }
    );
}

void MainWindow::triggerImportXState() {
    triggerImportStateMachine();
}

void MainWindow::triggerExportStateMachine() {
    exportSession(focusedGroup_ != nullptr ? focusedGroup_->currentSession() : nullptr);
}

void MainWindow::triggerExportXState() {
    triggerExportStateMachine();
}

void MainWindow::exportSessionToXState(DocumentSession* session) {
    exportSession(session);
}

void MainWindow::exportSession(DocumentSession* session) {
    if (session == nullptr) {
        statusBar()->showMessage(QStringLiteral("Export: no machine is focused"), 4000);
        return;
    }
    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (doc == nullptr) {
        return;
    }

    ExportDialogConfig cfg;
    cfg.activeMachine = &doc->machine();
    cfg.activeMachineName = session->machineName();
    if (cfg.activeMachineName.isEmpty()) {
        cfg.activeMachineName = doc->machine().name;
    }
    if (cfg.activeMachineName.isEmpty()) {
        cfg.activeMachineName = QStringLiteral("machine");
    }

    // Collect all machines in open sessions
    for (const auto& s : sessions_) {
        if (!s) continue;
        auto sDoc = s->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
        if (sDoc) {
            cfg.allMachines.append(&sDoc->machine());
        }
    }

    if (!currentProjectPath_.isEmpty()) {
        cfg.defaultDirectory = QFileInfo(currentProjectPath_).absolutePath();
    } else {
        cfg.defaultDirectory = QDir::currentPath();
    }

    ExportDialog dlg(cfg, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;  // user canceled
    }

    ExportJob job = dlg.exportJob();
    if (!job.adapter) {
        return;
    }

    const QString formatName = job.adapter->descriptor().name;
    const QString destPath = job.destinationPath;

    statusBar()->showMessage(QStringLiteral("Exporting %1 to %2...").arg(cfg.activeMachineName, formatName));

    executeExportJobAsync(
        ioDispatcher_,
        job,
        /*isModal=*/false,
        [this, formatName, destPath](const ExportJobResult& res) {
            if (res.ok) {
                if (!res.diagnostics.isEmpty()) {
                    QMessageBox::information(
                        this, QStringLiteral("Export Machine"),
                        QStringLiteral("Exported to '%1' with %2 note(s):\n\n- %3")
                            .arg(destPath)
                            .arg(res.diagnostics.size())
                            .arg(res.diagnostics.join(QStringLiteral("\n- "))));
                }
                statusBar()->showMessage(QStringLiteral("Successfully exported %1 to %2").arg(formatName, destPath), 5000);
            } else {
                QMessageBox::warning(this, QStringLiteral("Export Machine"),
                                     QStringLiteral("Export to %1 failed: %2").arg(formatName, res.error));
            }
        }
    );
}

DocumentSession* MainWindow::debugImportStateMachine(const QString& path, QStringList* diagnostics, QString* error) {
    if (path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        return debugImportXState(path, diagnostics, error);
    }
    Machine machine;
    if (!loadMachine(path, &machine, error)) {
        return nullptr;
    }
    const QString name = !machine.name.isEmpty() ? machine.name : QFileInfo(path).completeBaseName();
    return adoptImportedSession(std::make_unique<DocumentSession>(name, std::move(machine)));
}

DocumentSession* MainWindow::debugImportXState(const QString& path, QStringList* diagnostics, QString* error) {
    XStateSessionImport imported = importXStateNewSession(path);
    if (diagnostics != nullptr) {
        *diagnostics = imported.diagnostics;
    }
    if (imported.session == nullptr) {
        if (error != nullptr) {
            *error = imported.error;
        }
        return nullptr;
    }
    return adoptImportedSession(std::move(imported.session));
}

void MainWindow::debugRenameMachine(DocumentSession* session, const QString& newName) {
    applyRenameMachine(session, newName);
}

void MainWindow::debugDeleteMachine(DocumentSession* session) {
    applyDeleteMachine(session);
}

void MainWindow::triggerOpenProject() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Project"), QString(),
                                                        QStringLiteral("State Designer Project (*.sdp)"));
    if (path.isEmpty()) {
        return;  // user canceled
    }
    openProjectAt(path);
}

void MainWindow::debugOpenProjectAt(const QString& path) {
    openProjectAt(path, /*promptIfReplacing=*/false);
}

void MainWindow::tryRestoreLastProject(bool noRestore) {
    if (noRestore) {
        return;
    }
    const bool restoreEnabled =
        settingsStore_.get(QStringLiteral("editor.restoreLastProject"), true).toBool();
    if (!restoreEnabled) {
        return;
    }
    const QString lastProject = recentProjects_.lastActiveProject();
    if (lastProject.isEmpty()) {
        return;
    }
    openProjectAt(lastProject, /*promptIfReplacing=*/false);
}

void MainWindow::openProjectAt(const QString& path, bool promptIfReplacing) {
    if (path.isEmpty()) {
        return;
    }

    if (promptIfReplacing && !sessions_.empty()) {
        if (QMessageBox::question(this, QStringLiteral("Open Project"),
                                   QStringLiteral("Opening this project replaces every currently open machine. "
                                                  "Continue?")) != QMessageBox::Yes) {
            return;
        }
    }

    statusBar()->showMessage(QStringLiteral("Opening project %1...").arg(path));

    ioDispatcher_.submitTask<LoadedProjectData>(
        events::IoOperationKind::LoadProject,
        path,
        QStringLiteral("Open Project"),
        /*isModal=*/true,
        [path](const std::atomic<bool>& cancelToken, IoDispatcher::ProgressCallback progress, QString* error) -> std::optional<LoadedProjectData> {
            return readProjectFiles(path, progress, error);
        },
        [this, path](LoadedProjectData loaded) {
            std::vector<std::unique_ptr<DocumentSession>> sessions;
            sessions.reserve(loaded.machines.size());
            for (auto& entry : loaded.machines) {
                auto session = std::make_unique<DocumentSession>(entry.name, std::move(entry.machine), /*manualClock=*/false);
                session->setRelativePath(entry.relativePath);
                sessions.push_back(std::move(session));
            }
            const int count = static_cast<int>(sessions.size());
            replaceSessions(std::move(sessions), path, loaded.project);
            recentProjects_.touch(path);
            recentProjects_.setLastActiveProject(path);
            statusBar()->showMessage(QStringLiteral("Opened project %1 (%2 machine(s))").arg(path).arg(count), 4000);
        },
        [this, path](const QString& error) {
            if (recentProjects_.isLastActiveProject(path)) {
                recentProjects_.setLastActiveProject(QString());
            }
            // A missing file is a dead entry: drop it from Open Recent. Any
            // other failure keeps the entry, since a retry may succeed.
            if (!QFileInfo::exists(path)) {
                recentProjects_.remove(path);
                statusBar()->showMessage(
                    QStringLiteral("Project not found — removed from Open Recent: %1").arg(path), 6000);
            } else {
                // Modal: load errors can be multi-line and clip in the status bar.
                QMessageBox::warning(this, QStringLiteral("Open Project"),
                                     QStringLiteral("Open Project failed: %1").arg(error));
            }
        }
    );
}

void MainWindow::rebuildOpenRecentMenu() {
    openRecentMenu_->clear();

    const QStringList paths = recentProjects_.paths();
    if (paths.isEmpty()) {
        QAction* none = openRecentMenu_->addAction(QStringLiteral("No Recent Projects"));
        none->setEnabled(false);
        openRecentMenu_->addSeparator();
        QAction* clear = openRecentMenu_->addAction(QStringLiteral("Clear Recently Opened"));
        clear->setEnabled(false);
        return;
    }

    const QString home = QDir::homePath();
    int index = 1;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        const QString name = info.completeBaseName();
        QString folder = info.absolutePath();
        if (!home.isEmpty() && folder.startsWith(home)) {
            folder = QStringLiteral("~") + folder.mid(home.length());
        }
        QAction* action =
            openRecentMenu_->addAction(QStringLiteral("&%1 %2  —  %3").arg(index).arg(name, folder));
        action->setToolTip(path);
        action->setStatusTip(path);
        connect(action, &QAction::triggered, this, [this, path] { openProjectAt(path); });
        ++index;
    }

    openRecentMenu_->addSeparator();
    QAction* clearAction = openRecentMenu_->addAction(QStringLiteral("Clear Recently Opened"));
    connect(clearAction, &QAction::triggered, this, [this] { recentProjects_.clear(); });
}

void MainWindow::triggerCloseProject() {
    closeProject(/*promptIfDirty=*/true);
}

bool MainWindow::closeProject(bool promptIfDirty) {
    if (sessions_.empty() && currentProjectPath_.isEmpty()) {
        return true;
    }

    if (promptIfDirty) {
        const QString message = !currentProjectPath_.isEmpty()
            ? QStringLiteral("Close project '%1' and all open machines? Any unsaved changes will be lost.")
                  .arg(QFileInfo(currentProjectPath_).fileName())
            : QStringLiteral("Close all open machines? Any unsaved changes will be lost.");

        if (QMessageBox::question(this, QStringLiteral("Close Project"), message,
                                  QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
            return false;
        }
    }

    replaceSessions({}, QString(), Project());
    recentProjects_.setLastActiveProject(QString());
    setWindowTitle(QStringLiteral("Ordo State Designer ") + QLatin1String(kAppVersion));
    statusBar()->showMessage(QStringLiteral("Closed project"), 3000);
    return true;
}

void MainWindow::triggerSaveProjectAs() {
    const QString startPath = !currentProjectPath_.isEmpty() ? currentProjectPath_ : QString();
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save Project"), startPath,
                                                        QStringLiteral("State Designer Project (*.sdp)"));
    if (path.isEmpty()) {
        return;  // user canceled
    }

    std::vector<MachineFileEntry> entries;
    entries.reserve(sessions_.size());
    for (const auto& session : sessions_) {
        auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
        if (!doc) continue;
        QString fileName = session->relativePath();
        if (fileName.isEmpty()) {
            fileName = sanitizeSnakeCase(session->machineName()) + QStringLiteral(".sdm");
            session->setRelativePath(fileName);
        }
        if (doc->machine().name != session->machineName()) {
            doc->setMachineName(session->machineName());
        }
        entries.push_back(MachineFileEntry{
            .relativePath = fileName,
            .name = session->machineName(),
            .machine = doc->machine()
        });
    }

    // An open project keeps its outputDir/rootNamespace across Save As; a new
    // one gets triggerGenerate()'s default namespace and a blank outputDir.
    const QString outputDir = currentProject_.has_value() ? currentProject_->outputDir : QString();
    const QString rootNamespace =
        currentProject_.has_value() && !currentProject_->rootNamespace.isEmpty() ? currentProject_->rootNamespace
                                                                                  : QStringLiteral("app::generated");

    Project project;
    project.name = QFileInfo(path).completeBaseName();
    project.outputDir = outputDir;
    project.rootNamespace = rootNamespace;
    for (const auto& entry : entries) {
        project.machineFiles.push_back(entry.relativePath);
    }

    statusBar()->showMessage(QStringLiteral("Saving project to %1...").arg(path));

    ioDispatcher_.submitAction(
        events::IoOperationKind::SaveProject,
        path,
        QStringLiteral("Save Project"),
        /*isModal=*/false,
        [path, project, entries = std::move(entries)](const std::atomic<bool>& cancelToken, IoDispatcher::ProgressCallback progress, QString* error) {
            return writeProjectFiles(path, project, entries, progress, error);
        },
        [this, path, project]() {
            currentProjectPath_ = path;
            currentProject_ = project;
            recentProjects_.touch(path);
            recentProjects_.setLastActiveProject(path);
            statusBar()->showMessage(QStringLiteral("Saved project to %1").arg(path), 4000);
        },
        [this](const QString& error) {
            statusBar()->showMessage(QStringLiteral("Save Project failed: %1").arg(error), 6000);
        }
    );
}

void MainWindow::triggerSave() {
    if (currentProjectPath_.isEmpty()) {
        // Never saved or opened: ask for a path once, then remember it.
        triggerSaveProjectAs();
        return;
    }

    std::vector<MachineFileEntry> entries;
    entries.reserve(sessions_.size());
    for (const auto& session : sessions_) {
        auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
        if (!doc) continue;
        QString fileName = session->relativePath();
        if (fileName.isEmpty()) {
            fileName = sanitizeSnakeCase(session->machineName()) + QStringLiteral(".sdm");
            session->setRelativePath(fileName);
        }
        if (doc->machine().name != session->machineName()) {
            doc->setMachineName(session->machineName());
        }
        entries.push_back(MachineFileEntry{
            .relativePath = fileName,
            .name = session->machineName(),
            .machine = doc->machine()
        });
    }

    const QString outputDir = currentProject_.has_value() ? currentProject_->outputDir : QString();
    const QString rootNamespace =
        currentProject_.has_value() && !currentProject_->rootNamespace.isEmpty() ? currentProject_->rootNamespace
                                                                                  : QStringLiteral("app::generated");

    Project project;
    project.name = QFileInfo(currentProjectPath_).completeBaseName();
    project.outputDir = outputDir;
    project.rootNamespace = rootNamespace;
    for (const auto& entry : entries) {
        project.machineFiles.push_back(entry.relativePath);
    }

    const QString path = currentProjectPath_;
    statusBar()->showMessage(QStringLiteral("Saving project to %1...").arg(path));

    ioDispatcher_.submitAction(
        events::IoOperationKind::SaveProject,
        path,
        QStringLiteral("Save Project"),
        /*isModal=*/false,
        [path, project, entries = std::move(entries)](const std::atomic<bool>& cancelToken, IoDispatcher::ProgressCallback progress, QString* error) {
            return writeProjectFiles(path, project, entries, progress, error);
        },
        [this, path, project]() {
            currentProject_ = project;
            statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 4000);
        },
        [this](const QString& error) {
            statusBar()->showMessage(QStringLiteral("Save failed: %1").arg(error), 6000);
        }
    );
}

void MainWindow::replaceSessions(std::vector<std::unique_ptr<DocumentSession>> newSessions, QString projectPath,
                                  Project project) {
    // View layer first: the panels reference the focused session's kernel,
    // which is about to be destroyed with every other current session.
    inspectorHost_.reset();
    inspectorAdapter_ = nullptr;
    traceHost_.reset();
    traceAdapter_ = nullptr;
    logicHost_.reset();
    logicAdapter_ = nullptr;
    focusedGroup_ = nullptr;

    // Every group, docked or floating, collapses to one fresh empty group; the
    // old tiling is meaningless once all its sessions die.
    for (FloatingEditorWindow* window : floatingWindows_) {
        delete window;
    }
    floatingWindows_.clear();
    while (rootSplitter_->count() > 0) {
        delete rootSplitter_->widget(0);  // cascades into nested splitters and every tab's EditorView
    }
    groups_.clear();

    machinesPanel_->clear();

    // sessions_ dies here; the probe pointers must not dangle past it.
    loginFlowSession_ = nullptr;
    trafficLightSession_ = nullptr;
    sessions_ = std::move(newSessions);

    for (const auto& session : sessions_) {
        machinesPanel_->addMachine(session.get());
        connect(session->badge(), &StatusBadgeAdapter::changed, this, &MainWindow::refreshStatusBar);
    }

    currentProjectPath_ = std::move(projectPath);
    currentProject_ = std::move(project);
    if (!currentProjectPath_.isEmpty()) {
        const QString ws = QFileInfo(currentProjectPath_).dir().filePath(QStringLiteral(".sd/settings.json"));
        settingsStore_.setWorkspacePath(ws);
        settingsStore_.loadWorkspace();
    } else {
        settingsStore_.setWorkspacePath(QString());
    }

    // The sole group opens the first machine (openView() fits on bind);
    // setFocusedGroup() rebinds the panels to it.
    EditorGroup* group = createGroup();
    rootSplitter_->addWidget(group);
    groups_.push_back(group);
    if (!sessions_.empty()) {
        group->openView(sessions_.front().get());
    }
    setFocusedGroup(group);
    refreshStatusBar();
}

EditorView* MainWindow::focusedView() const {
    return focusedGroup_ != nullptr ? focusedGroup_->currentView() : nullptr;
}

DocumentSession* MainWindow::createNewMachine(const QString& name) {
    if (focusedGroup_ == nullptr && !groups_.empty()) {
        setFocusedGroup(groups_.front());
    }
    QString finalName = name.trimmed();
    if (finalName.isEmpty()) {
        finalName = QStringLiteral("Machine %1").arg(sessions_.size() + 1);
    }
    return adoptImportedSession(std::make_unique<DocumentSession>(finalName));
}

DocumentSession* MainWindow::ensureActiveSession(const QString& fallbackName) {
    if (focusedGroup_ == nullptr && !groups_.empty()) {
        setFocusedGroup(groups_.front());
    }
    EditorView* view = focusedView();
    if (view != nullptr && view->session() != nullptr) {
        return view->session();
    }
    if (!sessions_.empty()) {
        handleMachineActivated(sessions_.front().get());
        view = focusedView();
        if (view != nullptr && view->session() != nullptr) {
            return view->session();
        }
    }
    return createNewMachine(fallbackName);
}

EditorView* MainWindow::ensureActiveView() {
    ensureActiveSession();
    return focusedView();
}

EditorView* MainWindow::debugSplitRight() {
    if (focusedGroup_ == nullptr) {
        return nullptr;
    }
    EditorGroup* newGroup = splitGroup(focusedGroup_, Qt::Horizontal, /*before=*/false);
    return newGroup != nullptr ? newGroup->currentView() : nullptr;
}

void MainWindow::debugOpenMachine(DocumentSession* session) {
    if (session != nullptr) {
        handleMachineActivated(session);
    }
}

void MainWindow::debugFocusView(EditorView* view) {
    if (view == nullptr) {
        return;
    }
    for (EditorGroup* group : groups_) {
        const int index = group->indexOfView(view);
        if (index >= 0) {
            setFocusedGroup(group);
            group->revealSession(view->session());
            return;
        }
    }
}

void MainWindow::debugSelectState(EditorView* view, quint64 stateId) {
    if (view == nullptr || view->presenter() == nullptr) {
        return;
    }
    view->presenter()->debugSelectState(stateId);
}

void MainWindow::debugShowCodeTab(const QString& fileNameSuffix) {
    if (inspectorPanel_ != nullptr) {
        inspectorPanel_->showCodeTabForFile(fileNameSuffix);
    }
}

LogicPanel* MainWindow::debugLogicPanel() const {
    return inspectorPanel_ != nullptr ? inspectorPanel_->logicPanel() : nullptr;
}

// Empties the focused group tab by tab. Only when it is the sole surviving
// group does this leave a standing "no bound machine" focus state; otherwise
// the emptied group collapses and focus moves on.
void MainWindow::debugCloseAllTabsInFocusedGroup() {
    // Pin the target once: closing its last tab can collapse it and move
    // focus, and re-reading focusedGroup_ would close the next group's tabs.
    const QPointer<EditorGroup> group = focusedGroup_;
    while (group != nullptr && group->tabCount() > 0) {
        group->closeTab(0);
    }
}

void MainWindow::debugCloseAllTabs() {
    // Walk a copy: each emptied split collapses and edits groups_.
    const std::vector<QPointer<EditorGroup>> targets(groups_.begin(), groups_.end());
    for (const QPointer<EditorGroup>& group : targets) {
        while (group != nullptr && group->tabCount() > 0) {
            group->closeTab(0);
        }
    }
}

void MainWindow::rebindPanelsToFocusedSession() {
    // Destroy-then-rebuild, never mutate in place.
    inspectorHost_.reset();
    inspectorAdapter_ = nullptr;
    traceHost_.reset();
    traceAdapter_ = nullptr;
    logicHost_.reset();
    logicAdapter_ = nullptr;
    QObject::disconnect(problemsConnection_);

    EditorView* view = focusedView();
    DocumentSession* session = view != nullptr ? view->session() : nullptr;
    if (session == nullptr) {
        if (problemsTable_ != nullptr) {
            problemsTable_->setRowCount(0);  // no focused machine -- nothing to validate
        }
        if (debugLogicPanel() != nullptr) {
            debugLogicPanel()->clearMachine();
        }
        // Every panel shows an explicit empty state, never stale content.
        if (inspectorPanel_ != nullptr) {
            inspectorPanel_->clearMachine();
        }
        if (tracePanel_ != nullptr) {
            tracePanel_->clearRows();
        }
        return;  // no focused group, or its current tab is empty -- nothing to attach to
    }

    inspectorHost_ = std::make_unique<ordo::qt::ViewHost>(session->kernel());
    inspectorAdapter_ = inspectorHost_->add<InspectorAdapter>(inspectorPanel_, session->machineName());

    traceHost_ = std::make_unique<ordo::qt::ViewHost>(session->kernel());
    traceAdapter_ = traceHost_->add<TraceAdapter>(tracePanel_, session->machineName());

    if (debugLogicPanel() != nullptr) {
        logicHost_ = std::make_unique<ordo::qt::ViewHost>(session->kernel());
        logicAdapter_ = logicHost_->add<LogicAdapter>(debugLogicPanel(), session->machineName());
    }

    // Live Problems: the adapter outlives this connection (session-lifetime
    // host), and the connection auto-drops if the session dies first.
    problemsConnection_ =
        connect(session->problemsAdapter(), &MachineProblemsAdapter::problemsChanged, this,
                &MainWindow::refreshProblems);
    refreshProblems();

    // Re-query the canvas selection; a fresh InspectorAdapter otherwise starts
    // on the Machine tab even with a state/transition already selected.
    CanvasPresenter::Selection selection;
    if (view->presenter() != nullptr) {
        selection = view->presenter()->currentSelection();
        if (inspectorPanel_ != nullptr && inspectorPanel_->logicPanel() != nullptr) {
            inspectorPanel_->logicPanel()->setHighlightedVariable(view->presenter()->activeHighlightedVariable());
            connect(view->presenter(), &CanvasPresenter::variableHighlightChanged,
                    inspectorPanel_->logicPanel(), &LogicPanel::setHighlightedVariable, Qt::UniqueConnection);
        }
    }
    inspectorAdapter_->setSelection(selection.kind, selection.id);
}

void MainWindow::populateProblemsTable(MachineDocAgent& doc, const QVector<Problem>& problems, bool surfaceTab) {
    problemsTable_->setRowCount(0);
    for (const Problem& problem : problems) {
        const int row = problemsTable_->rowCount();
        problemsTable_->insertRow(row);
        problemsTable_->setItem(
            row, 0,
            new QTableWidgetItem(problem.severity == ProblemSeverity::Error ? QStringLiteral("Error")
                                                                             : QStringLiteral("Warning")));
        problemsTable_->setItem(row, 1, new QTableWidgetItem(problemLocationText(doc, problem)));
        problemsTable_->setItem(row, 2, new QTableWidgetItem(problem.text));
    }
    // Only the explicit Generate click surfaces the tab, and only with rows;
    // a live refresh never steals it.
    if (surfaceTab && !problems.isEmpty() && bottomTabs_ != nullptr) {
        bottomTabs_->setCurrentWidget(problemsTable_);
    }
}

QWidget* MainWindow::debugProblemsWidget() const { return problemsTable_; }

QStringList MainWindow::debugProblemsTexts() const {
    QStringList rows;
    for (int row = 0; row < problemsTable_->rowCount(); ++row) {
        rows.push_back(problemsTable_->item(row, 0)->text() + QStringLiteral("|") +
                       problemsTable_->item(row, 2)->text());
    }
    return rows;
}

void MainWindow::refreshProblems() {
    DocumentSession* session = focusedGroup_ != nullptr ? focusedGroup_->currentSession() : nullptr;
    if (session == nullptr || problemsTable_ == nullptr) {
        return;
    }
    auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (doc == nullptr) {
        return;
    }
    populateProblemsTable(*doc, session->problemsAdapter()->problems(), /*surfaceTab=*/false);
}

void MainWindow::triggerOpenSettings() {
    if (settingsView_ != nullptr) {
        for (EditorGroup* group : groups_) {
            if (group->revealWidget(settingsView_)) {
                setFocusedGroup(group);
                return;
            }
        }
    }

    // codegen.outputDir/rootNamespace are schema-only in SettingsRegistry; their
    // values live in Project (the .sdp), which triggerGenerate() reads.
    ProjectSettingsAccess projectAccess;
    projectAccess.isProjectOpen = [this] { return currentProject_.has_value(); };
    projectAccess.read = [this](const QString& key) -> QJsonValue {
        if (!currentProject_.has_value()) {
            return QJsonValue();
        }
        if (key == QStringLiteral("codegen.outputDir")) {
            return currentProject_->outputDir;
        }
        if (key == QStringLiteral("codegen.rootNamespace")) {
            return currentProject_->rootNamespace;
        }
        return QJsonValue();
    };
    projectAccess.write = [this](const QString& key, const QJsonValue& value) {
        if (!currentProject_.has_value() || currentProjectPath_.isEmpty()) {
            return;  // no project open -- nothing to persist
        }
        if (key == QStringLiteral("codegen.outputDir")) {
            currentProject_->outputDir = value.toString();
        } else if (key == QStringLiteral("codegen.rootNamespace")) {
            currentProject_->rootNamespace = value.toString();
        } else {
            return;
        }
        // Manifest only; machine files are untouched.
        QString error;
        if (!saveProject(*currentProject_, currentProjectPath_, &error)) {
            statusBar()->showMessage(QStringLiteral("Failed to save project settings: %1").arg(error), 6000);
        }
    };

    settingsView_ = new SettingsView(settingsStore_, std::move(projectAccess));
    connect(settingsView_, &QObject::destroyed, this, [this] { settingsView_ = nullptr; });

    EditorGroup* target = focusedGroup_ != nullptr ? focusedGroup_ : (!groups_.empty() ? groups_.front() : nullptr);
    if (target != nullptr) {
        target->openCustomTab(settingsView_, QStringLiteral("Settings"));
        setFocusedGroup(target);
    }
}

SettingsView* MainWindow::debugOpenSettings() {
    triggerOpenSettings();
    return settingsView_;
}

void MainWindow::debugCloseSettings() {
    if (settingsView_ == nullptr) {
        return;
    }
    for (EditorGroup* group : groups_) {
        const int idx = group->indexOfWidget(settingsView_);
        if (idx >= 0) {
            group->closeTab(idx);
            break;
        }
    }
    settingsView_ = nullptr;
}

}  // namespace app
