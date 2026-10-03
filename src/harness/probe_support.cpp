// Shared probe/smoke helpers with cross-translation-unit callers; see
// probe_support.h.

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

#include "constants/design_tokens.h"
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
#include "controller/setting_commands.h"
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

#include "harness/probe_support.h"

// Registers every controller/edit_commands.h command on `kernel`. Paired
// one-for-one with the removeCommand block at the end of runSmoke().
void registerEditCommands(ordo::core::Kernel& kernel) {
    kernel.registerCommand<app::events::AddStateRequested, app::AddStateCommand>();
    kernel.registerCommand<app::events::RenameStateRequested, app::RenameStateCommand>();
    kernel.registerCommand<app::events::SetStateKindRequested, app::SetStateKindCommand>();
    kernel.registerCommand<app::events::MoveStateRequested, app::MoveStateCommand>();
    kernel.registerCommand<app::events::ReparentStateRequested, app::ReparentStateCommand>();
    kernel.registerCommand<app::events::SetInitialChildRequested, app::SetInitialChildCommand>();
    kernel.registerCommand<app::events::SetEntryActionsRequested, app::SetEntryActionsCommand>();
    kernel.registerCommand<app::events::SetExitActionsRequested, app::SetExitActionsCommand>();
    kernel.registerCommand<app::events::SetDescriptionRequested, app::SetDescriptionCommand>();
    kernel.registerCommand<app::events::SetTagsRequested, app::SetTagsCommand>();
    kernel.registerCommand<app::events::SetHistoryDeepRequested, app::SetHistoryDeepCommand>();
    // Invoke declaration authoring.
    kernel.registerCommand<app::events::SetInvokeSrcRequested, app::SetInvokeSrcCommand>();
    kernel.registerCommand<app::events::SetInvokeIdRequested, app::SetInvokeIdCommand>();
    kernel.registerCommand<app::events::SetInvokeOutputTypeRequested, app::SetInvokeOutputTypeCommand>();
    kernel.registerCommand<app::events::SetInvocationsRequested, app::SetInvocationsCommand>();
    kernel.registerCommand<app::events::SetStateColorRequested, app::SetStateColorCommand>();
    kernel.registerCommand<app::events::SetTransitionColorRequested, app::SetTransitionColorCommand>();
    kernel.registerCommand<app::events::SetNoteColorRequested, app::SetNoteColorCommand>();
    kernel.registerCommand<app::events::SetMachineSelfRequested, app::SetMachineSelfCommand>();
    kernel.registerCommand<app::events::SetMachineNameRequested, app::SetMachineNameCommand>();
    kernel.registerCommand<app::events::SetInitialStateRequested, app::SetInitialStateCommand>();
    kernel.registerCommand<app::events::AddTransitionRequested, app::AddTransitionCommand>();
    kernel.registerCommand<app::events::SetTransitionEventRequested, app::SetTransitionEventCommand>();
    kernel.registerCommand<app::events::SetTransitionGuardRequested, app::SetTransitionGuardCommand>();
    kernel.registerCommand<app::events::SetTransitionActionRequested, app::SetTransitionActionCommand>();
    // Guard/action hook rename cascade.
    kernel.registerCommand<app::events::RenameGuardRequested, app::RenameGuardCommand>();
    kernel.registerCommand<app::events::RenameActionRequested, app::RenameActionCommand>();
    kernel.registerCommand<app::events::SetTransitionDelayRequested, app::SetTransitionDelayCommand>();
    kernel.registerCommand<app::events::SetTransitionReenterRequested, app::SetTransitionReenterCommand>();
    kernel.registerCommand<app::events::SetTransitionAlwaysRequested, app::SetTransitionAlwaysCommand>();
    kernel.registerCommand<app::events::SetTransitionPayloadTypeRequested, app::SetTransitionPayloadTypeCommand>();
    kernel.registerCommand<app::events::RetargetTransitionRequested, app::RetargetTransitionCommand>();
    kernel.registerCommand<app::events::MoveTransitionLabelRequested, app::MoveTransitionLabelCommand>();
    kernel.registerCommand<app::events::ApplyLayoutPlanRequested, app::ApplyLayoutPlanCommand>();
    kernel.registerCommand<app::events::SetTransitionBendpointsRequested, app::SetTransitionBendpointsCommand>();
    kernel.registerCommand<app::events::DeleteStateRequested, app::DeleteStateCommand>();
    kernel.registerCommand<app::events::DeleteTransitionRequested, app::DeleteTransitionCommand>();
    kernel.registerCommand<app::events::AddNoteRequested, app::AddNoteCommand>();
    kernel.registerCommand<app::events::MoveNoteRequested, app::MoveNoteCommand>();
    kernel.registerCommand<app::events::SetNoteTextRequested, app::SetNoteTextCommand>();
    kernel.registerCommand<app::events::DeleteNoteRequested, app::DeleteNoteCommand>();
    // Extended-state context vocabulary.
    kernel.registerCommand<app::events::AddContextVariableRequested, app::AddContextVariableCommand>();
    kernel.registerCommand<app::events::RenameContextVariableRequested, app::RenameContextVariableCommand>();
    kernel.registerCommand<app::events::SetContextTypeRequested, app::SetContextTypeCommand>();
    kernel.registerCommand<app::events::SetContextInitialValueRequested, app::SetContextInitialValueCommand>();
    kernel.registerCommand<app::events::DeleteContextVariableRequested, app::DeleteContextVariableCommand>();
    kernel.registerCommand<app::events::SetContextCustomTypeNameRequested, app::SetContextCustomTypeNameCommand>();
    kernel.registerCommand<app::events::SetExternalHeadersRequested, app::SetExternalHeadersCommand>();
    kernel.registerCommand<app::events::AddStructDefinitionRequested, app::AddStructDefinitionCommand>();
    kernel.registerCommand<app::events::SetStructDefinitionRequested, app::SetStructDefinitionCommand>();
    kernel.registerCommand<app::events::DeleteStructDefinitionRequested, app::DeleteStructDefinitionCommand>();
    kernel.registerCommand<app::events::MachineSnapshotRequested, app::MachineSnapshotCommand>();
    app::registerSettingCommands(kernel);
}

void removeEditCommands(ordo::core::Kernel& kernel) {
    kernel.removeCommand<app::events::AddStateRequested>();
    kernel.removeCommand<app::events::RenameStateRequested>();
    kernel.removeCommand<app::events::SetStateKindRequested>();
    kernel.removeCommand<app::events::MoveStateRequested>();
    kernel.removeCommand<app::events::ReparentStateRequested>();
    kernel.removeCommand<app::events::SetInitialChildRequested>();
    kernel.removeCommand<app::events::SetEntryActionsRequested>();
    kernel.removeCommand<app::events::SetExitActionsRequested>();
    kernel.removeCommand<app::events::SetDescriptionRequested>();
    kernel.removeCommand<app::events::SetTagsRequested>();
    kernel.removeCommand<app::events::SetHistoryDeepRequested>();
    kernel.removeCommand<app::events::SetInvokeSrcRequested>();
    kernel.removeCommand<app::events::SetInvokeIdRequested>();
    kernel.removeCommand<app::events::SetInvokeOutputTypeRequested>();
    kernel.removeCommand<app::events::SetInvocationsRequested>();
    kernel.removeCommand<app::events::SetStateColorRequested>();
    kernel.removeCommand<app::events::SetTransitionColorRequested>();
    kernel.removeCommand<app::events::SetNoteColorRequested>();
    kernel.removeCommand<app::events::SetMachineSelfRequested>();
    kernel.removeCommand<app::events::SetMachineNameRequested>();
    kernel.removeCommand<app::events::SetInitialStateRequested>();
    kernel.removeCommand<app::events::AddTransitionRequested>();
    kernel.removeCommand<app::events::SetTransitionEventRequested>();
    kernel.removeCommand<app::events::SetTransitionGuardRequested>();
    kernel.removeCommand<app::events::SetTransitionActionRequested>();
    kernel.removeCommand<app::events::RenameGuardRequested>();
    kernel.removeCommand<app::events::RenameActionRequested>();
    kernel.removeCommand<app::events::SetTransitionDelayRequested>();
    kernel.removeCommand<app::events::SetTransitionReenterRequested>();
    kernel.removeCommand<app::events::SetTransitionAlwaysRequested>();
    kernel.removeCommand<app::events::RetargetTransitionRequested>();
    kernel.removeCommand<app::events::MoveTransitionLabelRequested>();
    kernel.removeCommand<app::events::ApplyLayoutPlanRequested>();
    kernel.removeCommand<app::events::SetTransitionBendpointsRequested>();
    kernel.removeCommand<app::events::DeleteStateRequested>();
    kernel.removeCommand<app::events::DeleteTransitionRequested>();
    kernel.removeCommand<app::events::AddNoteRequested>();
    kernel.removeCommand<app::events::MoveNoteRequested>();
    kernel.removeCommand<app::events::SetNoteTextRequested>();
    kernel.removeCommand<app::events::DeleteNoteRequested>();
    kernel.removeCommand<app::events::AddContextVariableRequested>();
    kernel.removeCommand<app::events::RenameContextVariableRequested>();
    kernel.removeCommand<app::events::SetContextTypeRequested>();
    kernel.removeCommand<app::events::SetContextInitialValueRequested>();
    kernel.removeCommand<app::events::DeleteContextVariableRequested>();
    kernel.removeCommand<app::events::SetTransitionPayloadTypeRequested>();
    kernel.removeCommand<app::events::SetContextCustomTypeNameRequested>();
    kernel.removeCommand<app::events::SetExternalHeadersRequested>();
    kernel.removeCommand<app::events::AddStructDefinitionRequested>();
    kernel.removeCommand<app::events::SetStructDefinitionRequested>();
    kernel.removeCommand<app::events::DeleteStructDefinitionRequested>();
    kernel.removeCommand<app::events::MachineSnapshotRequested>();
    kernel.removeCommand<app::events::SetSettingRequested>();
    kernel.removeCommand<app::events::ResetSettingRequested>();
}

// Registers the whole simulation lane (controller/sim_commands.h) on `kernel`,
// plain: sim runtime state is never undo-captured. `clock` is held by
// std::ref and must outlive the registrations; call removeSimCommands() before
// it goes out of scope.
void registerSimCommands(ordo::core::Kernel& kernel, app::SimClock& clock) {
    kernel.registerCommand<app::events::SetModeRequested, app::SetModeCommand>();
    kernel.registerCommand<app::events::RunRequested, app::RunCommand>(std::ref(clock));
    kernel.registerCommand<app::events::PauseRequested, app::PauseCommand>(std::ref(clock));
    kernel.registerCommand<app::events::ResetRequested, app::ResetCommand>(std::ref(clock));
    kernel.registerCommand<app::events::SendEventRequested, app::SendEventCommand>();
    kernel.registerCommand<app::events::SetGuardResultRequested, app::SetGuardResultCommand>();
    kernel.registerCommand<app::events::TickElapsed, app::TickCommand>();
    kernel.registerCommand<app::events::BackRequested, app::BackCommand>();
    kernel.registerCommand<app::events::CompleteInvocationRequested, app::CompleteInvocationCommand>();
    kernel.registerCommand<app::events::ToggleBreakpointRequested, app::ToggleBreakpointCommand>();
    kernel.registerCommand<app::events::SetTimescaleRequested, app::SetTimescaleCommand>(std::ref(clock));
}

void removeSimCommands(ordo::core::Kernel& kernel) {
    kernel.removeCommand<app::events::SetModeRequested>();
    kernel.removeCommand<app::events::RunRequested>();
    kernel.removeCommand<app::events::PauseRequested>();
    kernel.removeCommand<app::events::ResetRequested>();
    kernel.removeCommand<app::events::SendEventRequested>();
    kernel.removeCommand<app::events::SetGuardResultRequested>();
    kernel.removeCommand<app::events::TickElapsed>();
    kernel.removeCommand<app::events::BackRequested>();
    kernel.removeCommand<app::events::CompleteInvocationRequested>();
    kernel.removeCommand<app::events::ToggleBreakpointRequested>();
    kernel.removeCommand<app::events::SetTimescaleRequested>();
}

// Registers the undo-phase command set: every edit intent wrapped in
// UndoCaptureCommand; MachineSnapshotRequested/SetModeRequested (reads / a
// runtime-state switch) and UndoRequested/RedoRequested stay plain. Paired
// with removeUndoPhaseCommands() below.
void registerUndoPhaseCommands(ordo::core::Kernel& kernel) {
    kernel.registerCommand<app::events::AddStateRequested,
                            app::UndoCaptureCommand<app::AddStateCommand, app::events::AddStateRequested>>();
    kernel.registerCommand<app::events::RenameStateRequested,
                            app::UndoCaptureCommand<app::RenameStateCommand, app::events::RenameStateRequested>>();
    kernel.registerCommand<app::events::SetStateKindRequested,
                            app::UndoCaptureCommand<app::SetStateKindCommand, app::events::SetStateKindRequested>>();
    kernel.registerCommand<app::events::MoveStateRequested,
                            app::UndoCaptureCommand<app::MoveStateCommand, app::events::MoveStateRequested>>();
    kernel.registerCommand<
        app::events::ReparentStateRequested,
        app::UndoCaptureCommand<app::ReparentStateCommand, app::events::ReparentStateRequested>>();
    kernel.registerCommand<
        app::events::SetInitialChildRequested,
        app::UndoCaptureCommand<app::SetInitialChildCommand, app::events::SetInitialChildRequested>>();
    kernel.registerCommand<
        app::events::SetEntryActionsRequested,
        app::UndoCaptureCommand<app::SetEntryActionsCommand, app::events::SetEntryActionsRequested>>();
    kernel.registerCommand<
        app::events::SetExitActionsRequested,
        app::UndoCaptureCommand<app::SetExitActionsCommand, app::events::SetExitActionsRequested>>();
    kernel.registerCommand<
        app::events::SetDescriptionRequested,
        app::UndoCaptureCommand<app::SetDescriptionCommand, app::events::SetDescriptionRequested>>();
    kernel.registerCommand<app::events::SetTagsRequested,
                            app::UndoCaptureCommand<app::SetTagsCommand, app::events::SetTagsRequested>>();
    kernel.registerCommand<
        app::events::SetHistoryDeepRequested,
        app::UndoCaptureCommand<app::SetHistoryDeepCommand, app::events::SetHistoryDeepRequested>>();
    // Invoke declaration authoring.
    kernel.registerCommand<
        app::events::SetInvokeSrcRequested,
        app::UndoCaptureCommand<app::SetInvokeSrcCommand, app::events::SetInvokeSrcRequested>>();
    kernel.registerCommand<
        app::events::SetInvokeIdRequested,
        app::UndoCaptureCommand<app::SetInvokeIdCommand, app::events::SetInvokeIdRequested>>();
    kernel.registerCommand<
        app::events::SetInvokeOutputTypeRequested,
        app::UndoCaptureCommand<app::SetInvokeOutputTypeCommand, app::events::SetInvokeOutputTypeRequested>>();
    kernel.registerCommand<
        app::events::SetInvocationsRequested,
        app::UndoCaptureCommand<app::SetInvocationsCommand, app::events::SetInvocationsRequested>>();
    kernel.registerCommand<
        app::events::SetStateColorRequested,
        app::UndoCaptureCommand<app::SetStateColorCommand, app::events::SetStateColorRequested>>();
    kernel.registerCommand<
        app::events::SetTransitionColorRequested,
        app::UndoCaptureCommand<app::SetTransitionColorCommand, app::events::SetTransitionColorRequested>>();
    kernel.registerCommand<
        app::events::SetNoteColorRequested,
        app::UndoCaptureCommand<app::SetNoteColorCommand, app::events::SetNoteColorRequested>>();
    kernel.registerCommand<
        app::events::SetMachineSelfRequested,
        app::UndoCaptureCommand<app::SetMachineSelfCommand, app::events::SetMachineSelfRequested>>();
    kernel.registerCommand<
        app::events::SetMachineNameRequested,
        app::UndoCaptureCommand<app::SetMachineNameCommand, app::events::SetMachineNameRequested>>();
    kernel.registerCommand<
        app::events::SetInitialStateRequested,
        app::UndoCaptureCommand<app::SetInitialStateCommand, app::events::SetInitialStateRequested>>();
    kernel.registerCommand<
        app::events::AddTransitionRequested,
        app::UndoCaptureCommand<app::AddTransitionCommand, app::events::AddTransitionRequested>>();
    kernel.registerCommand<
        app::events::SetTransitionEventRequested,
        app::UndoCaptureCommand<app::SetTransitionEventCommand, app::events::SetTransitionEventRequested>>();
    kernel.registerCommand<
        app::events::SetTransitionGuardRequested,
        app::UndoCaptureCommand<app::SetTransitionGuardCommand, app::events::SetTransitionGuardRequested>>();
    kernel.registerCommand<
        app::events::SetTransitionActionRequested,
        app::UndoCaptureCommand<app::SetTransitionActionCommand, app::events::SetTransitionActionRequested>>();
    // Guard/action hook rename cascade.
    kernel.registerCommand<app::events::RenameGuardRequested,
                            app::UndoCaptureCommand<app::RenameGuardCommand, app::events::RenameGuardRequested>>();
    kernel.registerCommand<
        app::events::RenameActionRequested,
        app::UndoCaptureCommand<app::RenameActionCommand, app::events::RenameActionRequested>>();
    kernel.registerCommand<
        app::events::SetTransitionDelayRequested,
        app::UndoCaptureCommand<app::SetTransitionDelayCommand, app::events::SetTransitionDelayRequested>>();
    kernel.registerCommand<
        app::events::SetTransitionReenterRequested,
        app::UndoCaptureCommand<app::SetTransitionReenterCommand, app::events::SetTransitionReenterRequested>>();
    kernel.registerCommand<
        app::events::SetTransitionAlwaysRequested,
        app::UndoCaptureCommand<app::SetTransitionAlwaysCommand, app::events::SetTransitionAlwaysRequested>>();
    kernel.registerCommand<
        app::events::SetTransitionPayloadTypeRequested,
        app::UndoCaptureCommand<app::SetTransitionPayloadTypeCommand, app::events::SetTransitionPayloadTypeRequested>>();
    kernel.registerCommand<
        app::events::RetargetTransitionRequested,
        app::UndoCaptureCommand<app::RetargetTransitionCommand, app::events::RetargetTransitionRequested>>();
    kernel.registerCommand<
        app::events::MoveTransitionLabelRequested,
        app::UndoCaptureCommand<app::MoveTransitionLabelCommand, app::events::MoveTransitionLabelRequested>>();
    kernel.registerCommand<
        app::events::ApplyLayoutPlanRequested,
        app::UndoCaptureCommand<app::ApplyLayoutPlanCommand, app::events::ApplyLayoutPlanRequested>>();
    kernel.registerCommand<
        app::events::SetTransitionBendpointsRequested,
        app::UndoCaptureCommand<app::SetTransitionBendpointsCommand, app::events::SetTransitionBendpointsRequested>>();
    kernel.registerCommand<app::events::DeleteStateRequested,
                            app::UndoCaptureCommand<app::DeleteStateCommand, app::events::DeleteStateRequested>>();
    kernel.registerCommand<
        app::events::DeleteTransitionRequested,
        app::UndoCaptureCommand<app::DeleteTransitionCommand, app::events::DeleteTransitionRequested>>();

    kernel.registerCommand<app::events::AddNoteRequested,
                            app::UndoCaptureCommand<app::AddNoteCommand, app::events::AddNoteRequested>>();
    kernel.registerCommand<app::events::MoveNoteRequested,
                            app::UndoCaptureCommand<app::MoveNoteCommand, app::events::MoveNoteRequested>>();
    kernel.registerCommand<
        app::events::SetNoteTextRequested,
        app::UndoCaptureCommand<app::SetNoteTextCommand, app::events::SetNoteTextRequested>>();
    kernel.registerCommand<app::events::DeleteNoteRequested,
                            app::UndoCaptureCommand<app::DeleteNoteCommand, app::events::DeleteNoteRequested>>();

    // Extended-state context vocabulary.
    kernel.registerCommand<
        app::events::AddContextVariableRequested,
        app::UndoCaptureCommand<app::AddContextVariableCommand, app::events::AddContextVariableRequested>>();
    kernel.registerCommand<
        app::events::RenameContextVariableRequested,
        app::UndoCaptureCommand<app::RenameContextVariableCommand, app::events::RenameContextVariableRequested>>();
    kernel.registerCommand<
        app::events::SetContextTypeRequested,
        app::UndoCaptureCommand<app::SetContextTypeCommand, app::events::SetContextTypeRequested>>();
    kernel.registerCommand<
        app::events::SetContextInitialValueRequested,
        app::UndoCaptureCommand<app::SetContextInitialValueCommand, app::events::SetContextInitialValueRequested>>();
    kernel.registerCommand<
        app::events::DeleteContextVariableRequested,
        app::UndoCaptureCommand<app::DeleteContextVariableCommand, app::events::DeleteContextVariableRequested>>();
    kernel.registerCommand<
        app::events::SetContextCustomTypeNameRequested,
        app::UndoCaptureCommand<app::SetContextCustomTypeNameCommand, app::events::SetContextCustomTypeNameRequested>>();
    kernel.registerCommand<
        app::events::SetExternalHeadersRequested,
        app::UndoCaptureCommand<app::SetExternalHeadersCommand, app::events::SetExternalHeadersRequested>>();
    kernel.registerCommand<
        app::events::AddStructDefinitionRequested,
        app::UndoCaptureCommand<app::AddStructDefinitionCommand, app::events::AddStructDefinitionRequested>>();
    kernel.registerCommand<
        app::events::SetStructDefinitionRequested,
        app::UndoCaptureCommand<app::SetStructDefinitionCommand, app::events::SetStructDefinitionRequested>>();
    kernel.registerCommand<
        app::events::DeleteStructDefinitionRequested,
        app::UndoCaptureCommand<app::DeleteStructDefinitionCommand, app::events::DeleteStructDefinitionRequested>>();

    kernel.registerCommand<app::events::MachineSnapshotRequested, app::MachineSnapshotCommand>();

    kernel.registerCommand<app::events::UndoRequested, app::UndoCommand>();
    kernel.registerCommand<app::events::RedoRequested, app::RedoCommand>();
    kernel.registerCommand<app::events::BeginUndoBatchRequested, app::BeginUndoBatchCommand>();
    kernel.registerCommand<app::events::EndUndoBatchRequested, app::EndUndoBatchCommand>();
}

void removeUndoPhaseCommands(ordo::core::Kernel& kernel) {
    kernel.removeCommand<app::events::AddStateRequested>();
    kernel.removeCommand<app::events::RenameStateRequested>();
    kernel.removeCommand<app::events::SetStateKindRequested>();
    kernel.removeCommand<app::events::MoveStateRequested>();
    kernel.removeCommand<app::events::ReparentStateRequested>();
    kernel.removeCommand<app::events::SetInitialChildRequested>();
    kernel.removeCommand<app::events::SetEntryActionsRequested>();
    kernel.removeCommand<app::events::SetExitActionsRequested>();
    kernel.removeCommand<app::events::SetDescriptionRequested>();
    kernel.removeCommand<app::events::SetTagsRequested>();
    kernel.removeCommand<app::events::SetHistoryDeepRequested>();
    kernel.removeCommand<app::events::SetInvokeSrcRequested>();
    kernel.removeCommand<app::events::SetInvokeIdRequested>();
    kernel.removeCommand<app::events::SetInvokeOutputTypeRequested>();
    kernel.removeCommand<app::events::SetInvocationsRequested>();
    kernel.removeCommand<app::events::SetStateColorRequested>();
    kernel.removeCommand<app::events::SetTransitionColorRequested>();
    kernel.removeCommand<app::events::SetNoteColorRequested>();
    kernel.removeCommand<app::events::SetMachineSelfRequested>();
    kernel.removeCommand<app::events::SetMachineNameRequested>();
    kernel.removeCommand<app::events::SetInitialStateRequested>();
    kernel.removeCommand<app::events::AddTransitionRequested>();
    kernel.removeCommand<app::events::SetTransitionEventRequested>();
    kernel.removeCommand<app::events::SetTransitionGuardRequested>();
    kernel.removeCommand<app::events::SetTransitionActionRequested>();
    kernel.removeCommand<app::events::RenameGuardRequested>();
    kernel.removeCommand<app::events::RenameActionRequested>();
    kernel.removeCommand<app::events::SetTransitionDelayRequested>();
    kernel.removeCommand<app::events::SetTransitionReenterRequested>();
    kernel.removeCommand<app::events::SetTransitionAlwaysRequested>();
    kernel.removeCommand<app::events::RetargetTransitionRequested>();
    kernel.removeCommand<app::events::MoveTransitionLabelRequested>();
    kernel.removeCommand<app::events::ApplyLayoutPlanRequested>();
    kernel.removeCommand<app::events::SetTransitionBendpointsRequested>();
    kernel.removeCommand<app::events::DeleteStateRequested>();
    kernel.removeCommand<app::events::DeleteTransitionRequested>();
    kernel.removeCommand<app::events::AddNoteRequested>();
    kernel.removeCommand<app::events::MoveNoteRequested>();
    kernel.removeCommand<app::events::SetNoteTextRequested>();
    kernel.removeCommand<app::events::DeleteNoteRequested>();
    kernel.removeCommand<app::events::AddContextVariableRequested>();
    kernel.removeCommand<app::events::RenameContextVariableRequested>();
    kernel.removeCommand<app::events::SetContextTypeRequested>();
    kernel.removeCommand<app::events::SetContextInitialValueRequested>();
    kernel.removeCommand<app::events::DeleteContextVariableRequested>();
    kernel.removeCommand<app::events::SetTransitionPayloadTypeRequested>();
    kernel.removeCommand<app::events::SetContextCustomTypeNameRequested>();
    kernel.removeCommand<app::events::SetExternalHeadersRequested>();
    kernel.removeCommand<app::events::AddStructDefinitionRequested>();
    kernel.removeCommand<app::events::SetStructDefinitionRequested>();
    kernel.removeCommand<app::events::DeleteStructDefinitionRequested>();
    kernel.removeCommand<app::events::MachineSnapshotRequested>();
    kernel.removeCommand<app::events::UndoRequested>();
    kernel.removeCommand<app::events::RedoRequested>();
    kernel.removeCommand<app::events::BeginUndoBatchRequested>();
    kernel.removeCommand<app::events::EndUndoBatchRequested>();
}

// ---- Scenario stage ---------------------------------------------------------
// Each scenario drives user-equivalent levers, settles with processEvents(),
// asserts, and saves a capture; any failed assertion fails the --gui-probe run.
// Captures are minimal: only the widget or scene region that evidences the
// assertion (the tour's canvas.png is the exception).

// Capture directory, resolved from the executable (build/ sits one level below
// the repo root). Created on first use because a fresh checkout has no such
// subtree; a write blocked by Controlled Folder Access still fails loudly at
// QImageWriter.
QString probeCaptureDir() {
    static const QString dir = [] {
        const QString d = QDir::cleanPath(QCoreApplication::applicationDirPath() +
                                          QStringLiteral("/../temp/captures/state-designer-probe"));
        QDir().mkpath(d);
        return d;
    }();
    return dir;
}

// Lossless WebP (quality 100) when the imageformats plugin is present, PNG
// otherwise. `baseName` carries no extension.
bool writeProbeImage(const QImage& image, const char* baseName) {
    if (image.isNull()) {
        std::fprintf(stderr, "FAIL: capture produced a null image (%s)\n", baseName);
        return false;
    }
    static const bool webpSupported = QImageWriter::supportedImageFormats().contains(QByteArrayLiteral("webp"));
    const QString path = probeCaptureDir() + QStringLiteral("/") + QLatin1String(baseName) +
                         (webpSupported ? QStringLiteral(".webp") : QStringLiteral(".png"));
    QImageWriter writer(path, webpSupported ? QByteArrayLiteral("webp") : QByteArrayLiteral("png"));
    if (webpSupported) {
        writer.setQuality(100);  // lossless in Qt's WebP handler
    }
    if (!writer.write(image)) {
        std::fprintf(stderr, "FAIL: QImageWriter failed for %s error=%s\n", qUtf8Printable(path),
                     qUtf8Printable(writer.errorString()));
        return false;
    }
    std::printf("wrote %s\n", qUtf8Printable(path));
    return true;
}

// One widget, exactly (QWidget::grab renders it even when it is not the
// visible tab).
bool saveWidgetCapture(QWidget* widget, const char* fileName) {
    QApplication::processEvents();
    if (widget == nullptr) {
        std::fprintf(stderr, "FAIL: no widget to capture (%s)\n", fileName);
        return false;
    }
    // The offscreen scenario stage disables window updates, and grab() (unlike
    // QGraphicsScene::render()) paints through that cycle and comes back blank.
    // Re-enable them for this shallow grab only.
    QWidget* topLevel = widget->window();
    const bool reenable = topLevel != nullptr && !topLevel->updatesEnabled();
    if (reenable) {
        topLevel->setUpdatesEnabled(true);
        QApplication::processEvents();
    }
    const QImage image = widget->grab().toImage();
    if (reenable) {
        topLevel->setUpdatesEnabled(false);
    }
    return writeProbeImage(image, fileName);
}

// The pane's scene content only (items bounding rect + margin, rendered
// 1:1 on the canvas background) -- compact and viewport-independent, for
// marker/loop/stub evidence.
bool saveSceneCapture(app::EditorView* pane, const char* fileName) {
    QApplication::processEvents();
    QGraphicsScene* scene = pane != nullptr && pane->canvasView() != nullptr ? pane->canvasView()->scene() : nullptr;
    if (scene == nullptr) {
        std::fprintf(stderr, "FAIL: no scene to capture (%s)\n", fileName);
        return false;
    }
    const QRectF rect = scene->itemsBoundingRect().adjusted(-24.0, -24.0, 24.0, 24.0);
    QImage image(static_cast<int>(std::ceil(rect.width())), static_cast<int>(std::ceil(rect.height())),
                 QImage::Format_ARGB32);
    image.fill(app::design::color(app::design::kCanvasBackground));  // so pills/edges read as on screen
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    scene->render(&painter, QRectF(image.rect()), rect);
    painter.end();
    return writeProbeImage(image, fileName);
}

// One scene region at 1:1: only the asserted edge/pill, never the whole scene.
bool saveSceneRegionCapture(app::EditorView* pane, const QRectF& region, const char* fileName) {
    QApplication::processEvents();
    QGraphicsScene* scene = pane != nullptr && pane->canvasView() != nullptr ? pane->canvasView()->scene() : nullptr;
    if (scene == nullptr || region.isEmpty()) {
        std::fprintf(stderr, "FAIL: no scene region to capture (%s)\n", fileName);
        return false;
    }
    QImage image(static_cast<int>(std::ceil(region.width())), static_cast<int>(std::ceil(region.height())),
                 QImage::Format_ARGB32);
    image.fill(app::design::color(app::design::kCanvasBackground));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    scene->render(&painter, QRectF(image.rect()), region);
    painter.end();
    return writeProbeImage(image, fileName);
}

// One widget region, for overlays that are not scene items (the minimap is a
// sibling widget of CanvasView, so a scene render cannot show it).
// QWidget::grab(rect) composites child-widget stacking as the screen does.
bool saveWidgetRegionCapture(QWidget* widget, const QRect& region, const char* fileName) {
    QApplication::processEvents();
    if (widget == nullptr || region.isEmpty()) {
        std::fprintf(stderr, "FAIL: no widget region to capture (%s)\n", fileName);
        return false;
    }
    return writeProbeImage(widget->grab(region).toImage(), fileName);
}

// The port invariant's perpendicularity, checked on the waypoints edge_router
// builds a drawn half from: the run leaves `from` along `fromDir`, arrives at
// `to` along `toDir`, and ends exactly at `to`.
bool portRunIsPerpendicular(QPointF from, QPointF fromDir, QPointF to, QPointF toDir) {
    const QVector<QPointF> run = app::orthogonalPortRun(from, fromDir, to, toDir);
    if (run.size() < 2) {
        return false;
    }
    if (std::hypot(run.front().x() - from.x(), run.front().y() - from.y()) > 0.01 ||
        std::hypot(run.back().x() - to.x(), run.back().y() - to.y()) > 0.01) {
        return false;
    }
    const auto unitOf = [](QPointF a, QPointF b) {
        const QPointF d = b - a;
        const qreal length = std::hypot(d.x(), d.y());
        return length > 1e-6 ? d / length : QPointF();
    };
    return QPointF::dotProduct(unitOf(run[0], run[1]), fromDir) > 0.999 &&
           QPointF::dotProduct(unitOf(run[run.size() - 2], run.back()), toDir) > 0.999;
}

// Every check one resolved assignment owes the invariant, independent of
// which rule row produced it: ports sit at their side's middle (offset by
// the straddle for a self pair), each half's run is perpendicular at both
// ends, and a normal transition's two lines never share a side.
bool portAssignmentHoldsInvariant(const app::PortAssignment& ports, const QRectF& pillRect, const char* what) {
    const qreal straddle = ports.cls == app::EdgeClass::Self ? app::kSelfStraddle : 0.0;
    // resolvePillPorts places each port at its side's middle, straddled by
    // kSelfStraddle for a self pair; the slack below is that straddle.
    constexpr qreal kPortBoundarySlack = app::kSelfStraddle;
    const auto portOnSideBoundary = [&](QPointF pt, app::PortSide side) {
        const QPointF middle = app::sidePortAnchor(pillRect, side, 0.0);
        switch (side) {
            case app::PortSide::Left:
            case app::PortSide::Right:
                return std::abs(pt.x() - middle.x()) < 0.01 &&
                       std::abs(pt.y() - middle.y()) <= kPortBoundarySlack + 0.01;
            case app::PortSide::Top:
            case app::PortSide::Bottom:
                return std::abs(pt.y() - middle.y()) < 0.01 &&
                       std::abs(pt.x() - middle.x()) <= kPortBoundarySlack + 0.01;
        }
        return false;
    };

    if (ports.hasEntry) {
        if (ports.cls == app::EdgeClass::Self) {
            const QPointF want = app::sidePortAnchor(pillRect, ports.entrySide, -straddle);
            if (std::hypot(ports.entryPoint.x() - want.x(), ports.entryPoint.y() - want.y()) > 0.01) {
                std::fprintf(stderr, "FAIL: %s entry port is not its side's straddle position\n", what);
                return false;
            }
        } else if (!portOnSideBoundary(ports.entryPoint, ports.entrySide)) {
            std::fprintf(stderr, "FAIL: %s entry port is not at its side's middle\n", what);
            return false;
        }
        if (!portRunIsPerpendicular(app::sourceHalfStart(ports.sourceAnchor, ports.sourceAnchorSide),
                                     app::outwardNormal(ports.sourceAnchorSide), ports.entryPoint,
                                     -app::outwardNormal(ports.entrySide))) {
            std::fprintf(stderr, "FAIL: %s source half is not perpendicular at both ends\n", what);
            return false;
        }
    }
    if (ports.hasExit) {
        if (ports.cls == app::EdgeClass::Self) {
            const QPointF want = app::sidePortAnchor(pillRect, ports.exitSide, straddle);
            if (std::hypot(ports.exitPoint.x() - want.x(), ports.exitPoint.y() - want.y()) > 0.01) {
                std::fprintf(stderr, "FAIL: %s exit port is not its side's straddle position\n", what);
                return false;
            }
        } else if (!portOnSideBoundary(ports.exitPoint, ports.exitSide)) {
            std::fprintf(stderr, "FAIL: %s exit port is not at its side's middle\n", what);
            return false;
        }
        if (!portRunIsPerpendicular(ports.exitPoint, app::outwardNormal(ports.exitSide),
                                     app::targetHalfStop(ports.targetAnchor, ports.targetAnchorSide),
                                     -app::outwardNormal(ports.targetAnchorSide))) {
            std::fprintf(stderr, "FAIL: %s target half is not perpendicular at both ends\n", what);
            return false;
        }
    }
    if (ports.cls == app::EdgeClass::Normal && ports.entrySide == ports.exitSide) {
        std::fprintf(stderr, "FAIL: %s put both lines on one pill side\n", what);
        return false;
    }
    return true;
}
// The one MachineFrameItem in a scene, or nullptr; it exists once any state
// does (created lazily with the first StateItem).
app::MachineFrameItem* findFrameItem(QGraphicsScene* scene) {
    for (QGraphicsItem* item : scene->items()) {
        if (item->type() == app::MachineFrameItem::Type) {
            return static_cast<app::MachineFrameItem*>(item);
        }
    }
    return nullptr;
}
// Blocks for `ms` of wall-clock time while pumping events; processEvents()
// alone does not advance timers (e.g. MinimapView's ~60ms throttled re-fit).
void pumpEventsFor(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(5);
    }
}
// Topology-only equality (name/states/transitions/notes/context), excluding
// Machine::nextId. nextId is a monotonic high-water mark that undo never rolls
// back (a fresh mint must not reuse an id a live redo entry refers to), so it
// differs from an earlier checkpoint even when the topology matches.
bool sameTopology(const app::Machine& a, const app::Machine& b) {
    return a.name == b.name && a.initialStateId == b.initialStateId && a.states == b.states &&
           a.transitions == b.transitions && a.notes == b.notes && a.context == b.context &&
           a.types == b.types && a.externalHeaders == b.externalHeaders;
}
// Finds `id`'s StateItem in `scene` by app::StateItem::Type over
// QGraphicsScene::items(), which returns nested children too.
app::StateItem* findStateItemById(QGraphicsScene* scene, quint64 id) {
    for (QGraphicsItem* item : scene->items()) {
        if (item->type() == app::StateItem::Type) {
            auto* stateItem = static_cast<app::StateItem*>(item);
            if (stateItem->id() == id) {
                return stateItem;
            }
        }
    }
    return nullptr;
}

// Finds `id`'s NoteItem in `scene`, same lookup as findStateItemById.
app::NoteItem* findNoteItemById(QGraphicsScene* scene, quint64 id) {
    for (QGraphicsItem* item : scene->items()) {
        if (item->type() == app::NoteItem::Type) {
            auto* noteItem = static_cast<app::NoteItem*>(item);
            if (noteItem->id() == id) {
                return noteItem;
            }
        }
    }
    return nullptr;
}
