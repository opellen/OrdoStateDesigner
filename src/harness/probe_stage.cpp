// The --gui-probe stage: runProbeScenarios(), the single ordered scenario call
// list, and runGuiProbe(), the shell tour that drives it.

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHash>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPair>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointF>
#include <QRect>
#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <ordo/core/kernel.h>
#include <ordo/qt/view_host.h>

#include "controller/edit_commands.h"
#include "controller/sim_commands.h"
#include "controller/undo_capture.h"
#include "controller/undo_commands.h"
#include "infra/code_generator.h"
#include "infra/machine_validator.h"
#include "infra/project_io.h"
#include "infra/sim_clock.h"
#include "infra/xstate_v5_io.h"
#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/sim_agent.h"
#include "model/sim_events.h"
#include "model/undo_events.h"
#include "model/undo_store.h"
#include "view/canvas/canvas_presenter.h"
#include "view/canvas/canvas_view.h"
#include "view/generated/canvas_interaction_core.h"  // drives the machine directly
#include "view/shell/document_session.h"
#include "view/shell/editor_view.h"
#include "view/shell/inspector_panel.h"
#include "view/shell/main_window.h"
#include "view/items/machine_frame_item.h"
#include "view/shell/minimap_view.h"
#include "view/items/note_item.h"
#include "view/geometry/pill_port_resolver.h"  // asserts the rule table 1:1
#include "view/items/state_item.h"
#include "view/shell/theme.h"
#include "view/shell/trace_panel.h"
#include "view/items/transition_item.h"

#include "harness/harness.h"
#include "harness/probe_scenarios.h"
#include "harness/probe_scene_index_check.h"

int runProbeScenarios(app::MainWindow& window, app::EditorView* loginPane, const QStringList& scenarioFilter) {
    if (loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: scenario stage has no login-flow pane\n");
        return 1;
    }
    int exitCode = 0;
    // --gui-probe-only runs just the scenarios whose function name contains a
    // filter term (case-insensitive). Skipping changes later scenarios'
    // baselines, so only the unfiltered run is a verification gate.
    int ran = 0;
    QStringList skipped;
    auto runSelected = [&](const char* name, const std::function<int()>& scenario) {
        const QString scenarioName = QString::fromLatin1(name);
        const bool selected =
            scenarioFilter.isEmpty() || std::any_of(scenarioFilter.begin(), scenarioFilter.end(), [&](const QString& term) {
                return scenarioName.contains(term, Qt::CaseInsensitive);
            });
        if (selected) {
            ++ran;
            exitCode |= scenario();
            // Checked after every scenario so a scene without NoIndex FAILs
            // under the scenario that introduced it.
            if (!probeSceneIndexCheckAllScenes(name)) {
                exitCode |= 1;
            }
        } else {
            skipped << scenarioName;
        }
    };
    // Undo-floor runs first: later scenarios' journal traffic saturates
    // UndoStore's 100-entry cap and evicts the bootstrap boundary it probes.
    runSelected("runUndoFloorScenario", [&] { return runUndoFloorScenario(window, loginPane); });
    runSelected("runGenerateEntryPointsScenario", [&] { return runGenerateEntryPointsScenario(window, loginPane); });
    runSelected("runInitialStateScenario", [&] { return runInitialStateScenario(window, loginPane); });
    runSelected("runKindWarningsScenario", [&] { return runKindWarningsScenario(window, loginPane); });
    runSelected("runLoopsTargetlessScenario", [&] { return runLoopsTargetlessScenario(window, loginPane); });
    runSelected("runStateMetadataScenario", [&] { return runStateMetadataScenario(window, loginPane); });
    runSelected("runInvokeAuthoringScenario", [&] { return runInvokeAuthoringScenario(window, loginPane); });
    runSelected("runContextVerbsScenario", [&] { return runContextVerbsScenario(window, loginPane); });
    runSelected("runCodeTabScenario", [&] { return runCodeTabScenario(window, loginPane); });
    runSelected("runPillReconnectScenario", [&] { return runPillReconnectScenario(window, loginPane); });
    runSelected("runRootEventsScenario", [&] { return runRootEventsScenario(window, loginPane); });
    runSelected("runEdgeStyleGateScenario", [&] { return runEdgeStyleGateScenario(window, loginPane); });
    runSelected("runSoftSnapScenario", [&] { return runSoftSnapScenario(window, loginPane); });
    runSelected("runEdgeFreedomScenario", [&] { return runEdgeFreedomScenario(window, loginPane); });
    runSelected("runPillPortsScenario", [&] { return runPillPortsScenario(window, loginPane); });
    runSelected("runMinBendRoutingScenario", [&] { return runMinBendRoutingScenario(window, loginPane); });
    runSelected("runPortSnapAlignmentScenario", [&] { return runPortSnapAlignmentScenario(window, loginPane); });
    runSelected("runSegmentDraggingScenario", [&] { return runSegmentDraggingScenario(window, loginPane); });
    runSelected("runMachineFrameScenario", [&] { return runMachineFrameScenario(window, loginPane); });
    runSelected("runRubberBandScenario", [&] { return runRubberBandScenario(window, loginPane); });
    runSelected("runNoteElementScenario", [&] { return runNoteElementScenario(window, loginPane); });
    runSelected("runMultilineNoteScenario", [&] { return runMultilineNoteScenario(window, loginPane); });
    runSelected("runActionBoxScenario", [&] { return runActionBoxScenario(window, loginPane); });
    runSelected("runHierarchyCanvasScenario", [&] { return runHierarchyCanvasScenario(window, loginPane); });
    runSelected("runReparentDragScenario", [&] { return runReparentDragScenario(window, loginPane); });
    runSelected("runNestedContainerDragScenario", [&] { return runNestedContainerDragScenario(window, loginPane); });
    runSelected("runCrossHierarchyEdgesScenario", [&] { return runCrossHierarchyEdgesScenario(window, loginPane); });
    runSelected("runHierarchySimulateScenario", [&] { return runHierarchySimulateScenario(window, loginPane); });
    runSelected("runCanvasContextBridgeScenario",
                [&] { return runCanvasContextBridgeScenario(window, loginPane); });
    runSelected("runAddChildStateScenario", [&] { return runAddChildStateScenario(window, loginPane); });
    runSelected("runGuardedGroupScenario", [&] { return runGuardedGroupScenario(window, loginPane); });
    runSelected("runHistoryDeepScenario", [&] { return runHistoryDeepScenario(window, loginPane); });
    runSelected("runAlignmentGuidesScenario", [&] { return runAlignmentGuidesScenario(window, loginPane); });
    runSelected("runEdgeStyleMenuScenario", [&] { return runEdgeStyleMenuScenario(window, loginPane); });
    runSelected("runMinimapScenario", [&] { return runMinimapScenario(window, loginPane); });
    runSelected("runContainerAuthoringScenario", [&] { return runContainerAuthoringScenario(window, loginPane); });
    runSelected("runCrossHighlightScenario", [&] { return runCrossHighlightScenario(window, loginPane); });
    runSelected("runSimulationBreakpointsScenario", [&] { return runSimulationBreakpointsScenario(window, loginPane); });
    runSelected("runReenterAlwaysScenario", [&] { return runReenterAlwaysScenario(window, loginPane); });
    runSelected("runRaiseMicrostepScenario", [&] { return runRaiseMicrostepScenario(window, loginPane); });
    runSelected("runActorCommunicationScenario", [&] { return runActorCommunicationScenario(window, loginPane); });
    runSelected("runWildcardEventsScenario", [&] { return runWildcardEventsScenario(window, loginPane); });
    runSelected("runMultipleTargetsScenario", [&] { return runMultipleTargetsScenario(window, loginPane); });

    runSelected("runLogicRowsScenario", [&] { return runLogicRowsScenario(window, loginPane); });
    runSelected("runGuardEditorScenario", [&] { return runGuardEditorScenario(window, loginPane); });
    runSelected("runExpressionEditorDialogScenario", [&] { return runExpressionEditorDialogScenario(window, loginPane); });
    runSelected("runHookRenameScenario", [&] { return runHookRenameScenario(window, loginPane); });
    runSelected("runLiveOverlayScenario", [&] { return runLiveOverlayScenario(window, loginPane); });
    runSelected("runActorsSectionScenario", [&] { return runActorsSectionScenario(window, loginPane); });
    runSelected("runInvokeCompletionScenario", [&] { return runInvokeCompletionScenario(window, loginPane); });
    runSelected("runFocusOutCommitScenario", [&] { return runFocusOutCommitScenario(window, loginPane); });
    runSelected("runContextSectionScenario", [&] { return runContextSectionScenario(window, loginPane); });
    runSelected("runTypesSectionScenario", [&] { return runTypesSectionScenario(window, loginPane); });
    runSelected("runSectionHeaderRhythmScenario", [&] { return runSectionHeaderRhythmScenario(window, loginPane); });
    runSelected("runFrameRightClickSelectionScenario", [&] { return runFrameRightClickSelectionScenario(window, loginPane); });
    runSelected("runMachinesSeparatorScenario", [&] { return runMachinesSeparatorScenario(window); });
    runSelected("runTypeRampBaseFontScenario", [&] { return runTypeRampBaseFontScenario(window); });
    runSelected("runAppIconScenario", [&] { return runAppIconScenario(window); });
    runSelected("runInspectorControlsFitScenario", [&] { return runInspectorControlsFitScenario(window, loginPane); });
    runSelected("runLogicPanelScenario", [&] { return runLogicPanelScenario(window, loginPane); });
    loginPane = window.focusedView();
    if (loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: scenario stage lost its login-flow pane after the Logic-panel scenario\n");
        return exitCode | 1;
    }

    runSelected("runSettingsViewScenario", [&] { return runSettingsViewScenario(window, loginPane); });
    loginPane = window.focusedView();
    if (loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: scenario stage lost its login-flow pane after the Settings-view scenario\n");
        return exitCode | 1;
    }

    runSelected("runProjectSettingsRoundTripScenario", [&] { return runProjectSettingsRoundTripScenario(window, loginPane); });
    loginPane = window.focusedView();
    if (loginPane == nullptr) {
        std::fprintf(stderr,
                     "FAIL: scenario stage lost its login-flow pane after the project-settings round-trip scenario\n");
        return exitCode | 1;
    }

    runSelected("runAsyncIoScenario", [&] { return runAsyncIoScenario(window, loginPane); });
    loginPane = window.focusedView();
    if (loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: scenario stage lost its login-flow pane after the Async-IO scenario\n");
        return exitCode | 1;
    }

    runSelected("runExportDialogScenario", [&] { return runExportDialogScenario(window, loginPane); });
    loginPane = window.focusedView();
    if (loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: scenario stage lost its login-flow pane after the Export-Dialog scenario\n");
        return exitCode | 1;
    }

    runSelected("runZoomToSelectionScenario", [&] { return runZoomToSelectionScenario(window, loginPane); });
    runSelected("runZoomCanvasExposureScenario", [&] { return runZoomCanvasExposureScenario(window, loginPane); });
    runSelected("runZoomMachinesTreeScenario", [&] { return runZoomMachinesTreeScenario(window, loginPane); });
    runSelected("runActionBoxIconSharpnessScenario", [&] { return runActionBoxIconSharpnessScenario(window, loginPane); });
    runSelected("runInitialAutoLayoutScenario", [&] { return runInitialAutoLayoutScenario(window, loginPane); });
    runSelected("runOnDemandAutoLayoutScenario", [&] { return runOnDemandAutoLayoutScenario(window, loginPane); });
    runSelected("runMachineRelationshipScenario", [&] { return runMachineRelationshipScenario(window, loginPane); });
    loginPane = window.focusedView();
    if (loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: scenario stage lost its login-flow pane after the Machine-Relationship scenario\n");
        return exitCode | 1;
    }
    runSelected("runXStateInteropScenario", [&] { return runXStateInteropScenario(window, loginPane); });  // must run last of the loginPane-based scenarios
    loginPane = window.focusedView();
    if (loginPane == nullptr) {
        std::fprintf(stderr, "FAIL: scenario stage lost its login-flow pane after the xstate-interop scenario\n");
        return exitCode | 1;
    }

    // Open Project load-failure never touches sessions_, so `loginPane` stays
    // valid; it runs before the session-wiping scenarios below.
    runSelected("runOpenProjectLoadFailureScenario",
                [&] { return runOpenProjectLoadFailureScenario(window, loginPane); });

    // Open-Recent wipes sessions_ (closeProject/reopen), so it runs after
    // everything that needs them.
    runSelected("runOpenRecentScenario", [&] { return runOpenRecentScenario(window, loginPane); });

    // Inspector empty-state also wipes sessions_ and mints its own session:
    // `loginPane` is stale after runOpenRecentScenario's closeProject() calls.
    runSelected("runInspectorEmptyStateScenario", [&] { return runInspectorEmptyStateScenario(window, loginPane); });
    runSelected("runMcpBatchPaintScenario", [&] { return runMcpBatchPaintScenario(window); });
    if (!scenarioFilter.isEmpty()) {
        std::printf("[PROBE] filtered run (--gui-probe-only %s): ran %d scenario(s), skipped %lld -- iteration "
                    "only, the baseline differs from a full run\n",
                    qUtf8Printable(scenarioFilter.join(QLatin1Char(','))), ran, static_cast<long long>(skipped.size()));
        if (ran == 0) {
            std::fprintf(stderr, "FAIL: --gui-probe-only %s matched no scenario\n",
                         qUtf8Printable(scenarioFilter.join(QLatin1Char(','))));
            exitCode |= 1;
        }
    }
    return exitCode;
}

// GUI probe tour: puts the traffic light in a second group in Simulate+Run via
// the debug* affordances, then captures the shell, the scene ~1200ms later, and
// the Inspector's State and Code tabs. Nothing sends to the kernel between the
// first two captures, so any change proves SimClock ticks on its own. Captures
// go under probeCaptureDir(), resolved from the exe's location.
int runGuiProbe(app::MainWindow& window, const QStringList& scenarioFilter) {
    if (window.loginFlowSession() == nullptr) {
        std::fprintf(stderr,
                     "FAIL: --gui-probe requires dev demo machines (built with ENABLE_DEV_DEMO=ON, e.g. SD-Run-Dev)\n");
        return 1;
    }

    QDir().mkpath(probeCaptureDir());

    window.show();

    int exitCode = 0;
    QTimer::singleShot(300, &window, [&] {
        app::EditorView* firstPane = window.focusedView();
        if (firstPane == nullptr) {
            std::fprintf(stderr, "FAIL: MainWindow has no focused view at startup\n");
            exitCode = 1;
            QApplication::quit();
            return;
        }

        // Split right, then open traffic-light in the new focused group: the
        // same DocumentSession::attachView() path a real split/open takes.
        app::EditorView* secondPane = window.debugSplitRight();
        if (secondPane == nullptr) {
            std::fprintf(stderr, "FAIL: debugSplitRight did not produce a second group\n");
            exitCode = 1;
            QApplication::quit();
            return;
        }
        window.debugOpenMachine(window.trafficLightSession());

        window.trafficLightSession()->kernel().send(
            app::events::SetModeRequested{.mode = app::events::Mode::Simulate});
        window.trafficLightSession()->kernel().send(app::events::RunRequested{});

        // Red's 1000ms delayed transition to Green has fired by the first
        // capture. The panes are captured by value: they are locals of this
        // lambda's frame, which has returned by the time the timers fire.
        QTimer::singleShot(1500, &window, [&, firstPane, secondPane] {
            // The one full-window capture: the shell layout has no single-widget
            // subject.
            if (!writeProbeImage(window.grab().toImage(), "canvas")) {
                exitCode = 1;
                QApplication::quit();
                return;
            }

            // No kernel sends here, only the event loop, so the next capture
            // can differ only because SimClock's own timer kept firing.
            QTimer::singleShot(1200, &window, [&, firstPane, secondPane] {
                // Scene only: compared against canvas.png's traffic pane.
                if (!saveSceneCapture(secondPane, "canvas-sim")) {
                    exitCode = 1;
                    QApplication::quit();
                    return;
                }

                // Focus the login-flow pane and select its Authenticating state;
                // the Inspector populates through the same selection route a
                // real click takes.
                window.debugFocusView(firstPane);
                window.debugSelectState(firstPane, kAuthenticatingStateId);

                QTimer::singleShot(200, &window, [&, firstPane] {
                    // Inspector panel only; the canvas is already in canvas.png.
                    if (!saveWidgetCapture(window.debugInspector(), "canvas-inspector")) {
                        exitCode = 1;
                        QApplication::quit();
                        return;
                    }

                    // Switch the Inspector to its Code tab and select
                    // login_flow_commands.h, as a real click chain would.
                    window.debugShowCodeTab(QStringLiteral("_commands.h"));

                    QTimer::singleShot(200, &window, [&, firstPane] {
                        // Inspector panel again -- now showing the Code tab.
                        if (!saveWidgetCapture(window.debugInspector(), "canvas-code")) {
                            exitCode = 1;
                        }
                        // The scenario stage runs after the tour so its edits do not
                        // contaminate the tour captures.
                        if (exitCode == 0) {
                            // Offscreen: disable window updates for the whole stage, or
                            // scene mutations overflow the stack in Qt's software sibling
                            // repaint recursion. saveWidgetCapture() re-enables them for its grab().
                            const bool suppressPaint =
                                QGuiApplication::platformName() == QLatin1String("offscreen");
                            if (suppressPaint) {
                                window.setUpdatesEnabled(false);
                            }
                            exitCode = runProbeScenarios(window, firstPane, scenarioFilter);
                            if (suppressPaint) {
                                window.setUpdatesEnabled(true);
                            }
                        }
                        QApplication::quit();
                    });
                });
            });
        });
    });

    QApplication::exec();
    return exitCode;
}
