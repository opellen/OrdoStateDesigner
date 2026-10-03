#pragma once

// Single declaration point for every --smoke phase and CLI-mode entry point.
// The --gui-probe scenario list lives in harness/probe_scenarios.h so adding a
// scenario does not recompile every includer. Shared capture/bootstrap helpers
// are in harness/probe_support.h. Forward declarations only for view types.

#include <QStringList>

#include "harness/probe_support.h"

class QString;

namespace app {
class EditorView;
class MainWindow;
}  // namespace app

// ---- --gui-probe stage (harness/probe_stage.cpp) ---------------------------
// scenarioFilter: --gui-probe-only terms; empty runs every scenario.
int runGuiProbe(app::MainWindow& window, const QStringList& scenarioFilter = {});

// ---- --smoke phases: domain + undo + hierarchy (harness/smoke_domain.cpp) -
int runUndoSmoke();
int runHierarchySmoke();
int runContextSmoke();
int runSmoke();

// ---- --smoke phase: guard expression unit (harness/smoke_expression.cpp) ---
// Own TU: infra/expression is standalone; no kernel, agent or document.
int runExpressionSmoke();
int runExpressionIntegrationSmoke();
int runExpressionActorsSmoke();

// ---- --smoke phases: sim interpreter family (harness/smoke_sim*.cpp) ------
int runSimSmoke();
int runTargetlessSelfSimSmoke();
int runRootEventSimSmoke();
int runHierarchySimSmoke();
int runHistorySimSmoke();
int runBackReplaySimSmoke();
int runSimFeaturesSmoke();

// ---- --smoke phases: codegen/validator/interop/shell ------------------------
int runCodegenCoreSmoke();
int runCodegenFeaturesSmoke();
int runCodegenProjectorSmoke();
int runHierarchicalCodegenSmoke();
int runNoteEditorMachineSmoke();
int runValidatorSmoke();
int runXStateInteropSmoke();
int runInteractionFsmSmoke();
int runPillPortSmoke();
int runAutoLayoutSmoke();
int runShellSmoke();
int runSettingsSmoke();
// infra/recent_projects unit smoke: pure list logic, no QApplication.
int runRecentProjectsSmoke();
int runIoSmoke();
int runScxmlSmoke();
int runMcpSmoke();
// Behavioural companion to runMcpSmoke() (which only proves every method
// dispatches): drives a real MainWindow/McpRuntimeServer through invocation
// authoring, a live run and complete_invocation, asserting on the JSON results.
// Owns its own QApplication; run after runShellSmoke(), never nested.
int runMcpInvokeSmoke();

// ---- CLI mode runners (harness/cli_modes.cpp) ------------------------------
int runCodegenCheck(const QString& outputDir);
int runImportMachine(const QString& formatId, const QString& inputPath, const QString& outputDir, const QString& savePath);
int runExportMachine(const QString& formatId, const QString& machinePath, const QString& outputPath);
int runImportXState(const QString& inputPath, const QString& outputDir, const QString& savePath);
int runExportXState(const QString& machinePath, const QString& outputPath);
int runProjectGenerate(const QString& machinePath, const QString& outputDir);
int runMachineDriftCheck(const QString& machinePath, const QString& committedDir);
int runMachineDocCheck(const QString& docPath, const QString& machinePath);
int runResaveMachine(const QString& inputPath, const QString& outputPath);
