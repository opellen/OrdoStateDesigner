#pragma once

// The --gui-probe scenario list: every scenario entry point, declared once.
// New scenarios are declared here and called from runProbeScenarios() in
// probe_stage.cpp.

#include <QStringList>

namespace app {
class EditorView;
class MainWindow;
}  // namespace app

// ---- --gui-probe scenarios (harness/probe_scenarios_shell.cpp) ------------
int runInitialStateScenario(app::MainWindow& window, app::EditorView* loginPane);
int runKindWarningsScenario(app::MainWindow& window, app::EditorView* loginPane);
int runLoopsTargetlessScenario(app::MainWindow& window, app::EditorView* loginPane);
int runStateMetadataScenario(app::MainWindow& window, app::EditorView* loginPane);
int runInvokeAuthoringScenario(app::MainWindow& window, app::EditorView* loginPane);
int runContextVerbsScenario(app::MainWindow& window, app::EditorView* loginPane);
int runGuardEditorScenario(app::MainWindow& window, app::EditorView* loginPane);
int runExpressionEditorDialogScenario(app::MainWindow& window, app::EditorView* loginPane);
int runHookRenameScenario(app::MainWindow& window, app::EditorView* loginPane);
int runLiveOverlayScenario(app::MainWindow& window, app::EditorView* loginPane);
int runActorsSectionScenario(app::MainWindow& window, app::EditorView* loginPane);
int runInvokeCompletionScenario(app::MainWindow& window, app::EditorView* loginPane);
int runFocusOutCommitScenario(app::MainWindow& window, app::EditorView* loginPane);
int runCodeTabScenario(app::MainWindow& window, app::EditorView* loginPane);
int runPillReconnectScenario(app::MainWindow& window, app::EditorView* loginPane);
int runLogicRowsScenario(app::MainWindow& window, app::EditorView* loginPane);
int runContextSectionScenario(app::MainWindow& window, app::EditorView* loginPane);
int runTypesSectionScenario(app::MainWindow& window, app::EditorView* loginPane);
int runSectionHeaderRhythmScenario(app::MainWindow& window, app::EditorView* loginPane);
int runFrameRightClickSelectionScenario(app::MainWindow& window, app::EditorView* loginPane);
int runMachinesSeparatorScenario(app::MainWindow& window);
int runTypeRampBaseFontScenario(app::MainWindow& window);
// Confirms the app icon is registered as a multi-size QIcon, a top-level
// window picks it up, and the 256px pixmap is the brand mark's shape.
int runAppIconScenario(app::MainWindow& window);
int runInspectorControlsFitScenario(app::MainWindow& window, app::EditorView* loginPane);
int runLogicPanelScenario(app::MainWindow& window, app::EditorView* loginPane);
int runUndoFloorScenario(app::MainWindow& window, app::EditorView* loginPane);
int runGenerateEntryPointsScenario(app::MainWindow& window, app::EditorView* loginPane);
int runProjectSettingsRoundTripScenario(app::MainWindow& window, app::EditorView* loginPane);
// A hand-broken .sdm with an out-of-set "color" value drives
// MainWindow::debugOpenProjectAt(); the load diagnostic must reach the user as
// a modal QMessageBox::warning, not a clipped status bar line. Leaves
// sessions_ untouched (the load never succeeds).
int runOpenProjectLoadFailureScenario(app::MainWindow& window, app::EditorView* loginPane);
// Save -> listed first -> close -> reopen via the real File > Open Recent
// QAction -> sessions restored; a missing-file entry is removed after a failed
// open. Wipes sessions_, so it must run last (see probe_stage.cpp).
int runOpenRecentScenario(app::MainWindow& window, app::EditorView* loginPane);
// Select a state -> Close Project (no-prompt path) -> the Inspector shows its
// empty-state page and the Trace panel has 0 rows -> binding a fresh machine
// leaves that page. Wipes sessions_, so it runs after runOpenRecentScenario;
// `loginPane` is stale by then, so the scenario mints its own session.
int runInspectorEmptyStateScenario(app::MainWindow& window, app::EditorView* loginPane);

// Unsettled hierarchical MCP batches painted at once, bulk-closed every other
// round. Runs last: it wipes every session.
int runMcpBatchPaintScenario(app::MainWindow& window);

// ---- --gui-probe scenarios (harness/probe_scenarios_canvas.cpp) -----------
int runRootEventsScenario(app::MainWindow& window, app::EditorView* loginPane);
int runEdgeStyleGateScenario(app::MainWindow& window, app::EditorView* loginPane);
int runSoftSnapScenario(app::MainWindow& window, app::EditorView* loginPane);
int runEdgeFreedomScenario(app::MainWindow& window, app::EditorView* loginPane);
int runPillPortsScenario(app::MainWindow& window, app::EditorView* loginPane);
int runMinBendRoutingScenario(app::MainWindow& window, app::EditorView* loginPane);
int runPortSnapAlignmentScenario(app::MainWindow& window, app::EditorView* loginPane);
int runSegmentDraggingScenario(app::MainWindow& window, app::EditorView* loginPane);
int runMachineFrameScenario(app::MainWindow& window, app::EditorView* loginPane);
int runRubberBandScenario(app::MainWindow& window, app::EditorView* loginPane);

// ---- --gui-probe scenarios (harness/probe_scenarios_hierarchy.cpp) --------
int runNoteElementScenario(app::MainWindow& window, app::EditorView* loginPane);
int runMultilineNoteScenario(app::MainWindow& window, app::EditorView* loginPane);
int runActionBoxScenario(app::MainWindow& window, app::EditorView* loginPane);
int runHierarchyCanvasScenario(app::MainWindow& window, app::EditorView* loginPane);
int runReparentDragScenario(app::MainWindow& window, app::EditorView* loginPane);
int runNestedContainerDragScenario(app::MainWindow& window, app::EditorView* loginPane);
int runCrossHierarchyEdgesScenario(app::MainWindow& window, app::EditorView* loginPane);
int runHierarchySimulateScenario(app::MainWindow& window, app::EditorView* loginPane);
// The canvas-interaction machine's own context (designMode/dropTargetId),
// asserted through fsm_ rather than the presenter's currentMode_ mirror:
// Design -> Simulate -> Design gesture gating, the drop-target verdict a live
// NodeDrag holds, and a second view opened mid-Simulate.
int runCanvasContextBridgeScenario(app::MainWindow& window, app::EditorView* loginPane);

// ---- --gui-probe scenarios (harness/probe_scenarios_polish.cpp) -----------
int runXStateInteropScenario(app::MainWindow& window, app::EditorView* loginPane);
int runAddChildStateScenario(app::MainWindow& window, app::EditorView* loginPane);
int runGuardedGroupScenario(app::MainWindow& window, app::EditorView* loginPane);
int runHistoryDeepScenario(app::MainWindow& window, app::EditorView* loginPane);
int runAlignmentGuidesScenario(app::MainWindow& window, app::EditorView* loginPane);
int runEdgeStyleMenuScenario(app::MainWindow& window, app::EditorView* loginPane);
int runMinimapScenario(app::MainWindow& window, app::EditorView* loginPane);
int runContainerAuthoringScenario(app::MainWindow& window, app::EditorView* loginPane);
int runCrossHighlightScenario(app::MainWindow& window, app::EditorView* loginPane);
int runSimulationBreakpointsScenario(app::MainWindow& window, app::EditorView* loginPane);
int runReenterAlwaysScenario(app::MainWindow& window, app::EditorView* loginPane);
int runRaiseMicrostepScenario(app::MainWindow& window, app::EditorView* loginPane);
int runActorCommunicationScenario(app::MainWindow& window, app::EditorView* loginPane);
int runWildcardEventsScenario(app::MainWindow& window, app::EditorView* loginPane);
int runMultipleTargetsScenario(app::MainWindow& window, app::EditorView* loginPane);
int runSettingsViewScenario(app::MainWindow& window, app::EditorView* loginPane);
int runAsyncIoScenario(app::MainWindow& window, app::EditorView* loginPane);
int runExportDialogScenario(app::MainWindow& window, app::EditorView* loginPane);
int runZoomToSelectionScenario(app::MainWindow& window, app::EditorView* loginPane);
int runZoomCanvasExposureScenario(app::MainWindow& window, app::EditorView* loginPane);
int runZoomMachinesTreeScenario(app::MainWindow& window, app::EditorView* loginPane);
int runActionBoxIconSharpnessScenario(app::MainWindow& window, app::EditorView* loginPane);
int runInitialAutoLayoutScenario(app::MainWindow& window, app::EditorView* loginPane);
int runOnDemandAutoLayoutScenario(app::MainWindow& window, app::EditorView* loginPane);
int runMachineRelationshipScenario(app::MainWindow& window, app::EditorView* loginPane);

// ---- the ordered scenario run (harness/probe_stage.cpp) ---------------------
// An empty filter runs every scenario; see --gui-probe-only.
int runProbeScenarios(app::MainWindow& window, app::EditorView* loginPane, const QStringList& scenarioFilter);
