#include "view/shell/document_session.h"

#include <algorithm>
#include <functional>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>

#include <ordo/qt/view_host.h>

#include "controller/edit_commands.h"
#include "controller/setting_commands.h"
#include "controller/sim_commands.h"
#include "controller/undo_capture.h"
#include "controller/undo_commands.h"
#include "infra/code_generator.h"
#include "infra/xstate_v5_io.h"
#include "model/machine.h"
#include "model/machine_events.h"
#include "model/undo_events.h"
#include "model/undo_store.h"
#include "view/canvas/canvas_presenter.h"
#include "view/canvas/canvas_view.h"
#include "view/geometry/auto_layout.h"
#include "view/items/state_item.h"
#include "view/items/transition_item.h"

namespace app {

namespace {

// The auto-layout's sizes (on load and on demand) come from throwaway canvas
// items: the same code that paints a node or a pill measures it, so nothing
// is placed smaller than it draws. Needs a GUI application (fonts) -- the
// caller checks.
AutoLayoutMetrics canvasItemMetrics() {
    AutoLayoutMetrics metrics;
    metrics.leafSize = [](const State& state) {
        const StateItem item(state.id, state.name, state.kind, state.entryActions, state.exitActions);
        return item.sceneRect().size();
    };
    metrics.containerHeaderHeight = [](const State& state) {
        const StateItem item(state.id, state.name, state.kind, state.entryActions, state.exitActions);
        return item.containerHeaderHeight();
    };
    metrics.labelSize = [](const Transition& transition) {
        TransitionLabelItem label;
        label.setLabelData(transitionPillEventText(transition), transition.guard, transition.action,
                           transition.isAlways());
        return label.sceneRect().size();
    };
    return metrics;
}

}  // namespace

// ---- StatusBadgeAdapter -----------------------------------------------------

StatusBadgeAdapter::StatusBadgeAdapter() : ordo::qt::ViewModel(QStringLiteral("StatusBadgeAdapter")) {}

void StatusBadgeAdapter::onRegister() {
    doc_ = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    sim_ = context().agentAs<SimulationAgent>(SimulationAgent::kName);

    subscribe<events::ActiveStateChanged>(&StatusBadgeAdapter::onActiveStateChanged);
    subscribe<events::SimulationStarted>(&StatusBadgeAdapter::onSimulationStarted);
    subscribe<events::SimulationPaused>(&StatusBadgeAdapter::onSimulationPaused);
    subscribe<events::SimulationReset>(&StatusBadgeAdapter::onSimulationReset);
    subscribe<events::ModeChanged>(&StatusBadgeAdapter::onModeChanged);
    // computeText() reads sim_->configuration(), so this is the fact that
    // carries the actual membership change.
    subscribe<events::ConfigurationChanged>(
        std::function<void(const events::ConfigurationChanged&)>([this](const events::ConfigurationChanged&) { refresh(); }));

    refresh();  // starting text ("idle") -- no sim fact has fired yet at construction time
}

void StatusBadgeAdapter::onActiveStateChanged(const events::ActiveStateChanged&) { refresh(); }
void StatusBadgeAdapter::onSimulationStarted(const events::SimulationStarted&) { refresh(); }
void StatusBadgeAdapter::onSimulationPaused(const events::SimulationPaused&) { refresh(); }
void StatusBadgeAdapter::onSimulationReset(const events::SimulationReset&) { refresh(); }
void StatusBadgeAdapter::onModeChanged(const events::ModeChanged&) { refresh(); }

QString StatusBadgeAdapter::computeText() const {
    const bool simulateRunning = sim_ != nullptr && sim_->mode() == events::Mode::Simulate && sim_->running();
    if (!simulateRunning || doc_ == nullptr) {
        return QStringLiteral("idle");
    }
    // The atomic (no active child) members of the configuration, in document
    // order; simultaneously active parallel regions join with " | ".
    const std::vector<quint64>& active = sim_->configuration();
    QStringList names;
    for (quint64 id : active) {
        bool hasActiveChild = false;
        for (quint64 other : active) {
            const State* otherState = doc_->findState(other);
            if (otherState != nullptr && otherState->parentId == id) {
                hasActiveChild = true;
                break;
            }
        }
        if (hasActiveChild) {
            continue;
        }
        const State* state = doc_->findState(id);
        names.push_back(state != nullptr ? state->name : QStringLiteral("?"));
    }
    if (names.isEmpty()) {
        return QStringLiteral("idle");
    }
    return QString(QChar(0x25CF)) + QStringLiteral(" ") + names.join(QStringLiteral(" | "));  // "● <name>[ | <name>...]"
}

void StatusBadgeAdapter::refresh() {
    const QString next = computeText();
    if (next != text_) {
        text_ = next;
        emit changed();
    }
}

// ---- MachineOutlineAdapter --------------------------------------------------

MachineOutlineAdapter::MachineOutlineAdapter() : ordo::qt::ViewModel(QStringLiteral("MachineOutlineAdapter")) {}

template <typename EventT>
void MachineOutlineAdapter::watch() {
    subscribe<EventT>(std::function<void(const EventT&)>([this](const EventT&) { emit outlineChanged(); }));
}

void MachineOutlineAdapter::onRegister() {
    doc_ = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    sim_ = context().agentAs<SimulationAgent>(SimulationAgent::kName);

    // Structure: every doc fact that can change what the tree shows --
    // states, transition endpoints, and the event/delay text on a
    // transition's tree row. Guard/action facts are deliberately not
    // watched: the outline doesn't display either.
    watch<events::StateAdded>();
    watch<events::StateRenamed>();
    watch<events::StateKindChanged>();
    watch<events::InitialStateChanged>();
    watch<events::StateDeleted>();
    watch<events::TransitionAdded>();
    watch<events::TransitionEventChanged>();
    watch<events::TransitionDelayChanged>();
    watch<events::TransitionAlwaysChanged>();
    watch<events::TransitionRetargeted>();
    watch<events::TransitionDeleted>();
    watch<events::MachineSnapshotPublished>();  // restore/undo/redo rebuild path

    // Live state: the badge's fact set. outline() reads sim_->configuration(),
    // so ConfigurationChanged carries the actual membership change.
    watch<events::ActiveStateChanged>();
    watch<events::SimulationStarted>();
    watch<events::SimulationPaused>();
    watch<events::SimulationReset>();
    watch<events::ModeChanged>();
    watch<events::ConfigurationChanged>();
}

QVector<MachineOutlineAdapter::OutlineState> MachineOutlineAdapter::outline() const {
    QVector<OutlineState> result;
    if (doc_ == nullptr) {
        return result;
    }
    const Machine& machine = doc_->machine();
    const bool simulateRunning = sim_ != nullptr && sim_->mode() == events::Mode::Simulate && sim_->running();

    result.reserve(machine.states.size());
    for (const State& state : machine.states) {
        OutlineState row;
        row.stateId = state.id;
        row.name = state.name.isEmpty() ? QStringLiteral("(unnamed)") : state.name;
        row.kind = state.kind;
        row.isInitial = state.id == machine.initialStateId;
        // Active dot per configuration member: every active state, containers included.
        row.active = false;
        if (simulateRunning) {
            for (quint64 activeId : sim_->configuration()) {
                if (activeId == state.id) {
                    row.active = true;
                    break;
                }
            }
        }
        for (const Transition& transition : machine.transitions) {
            if (transition.from != state.id) {
                continue;
            }
            OutlineEvent event;
            event.transitionId = transition.id;
            if (!transition.event.trimmed().isEmpty()) {
                event.label = transition.event;
            } else if (transition.always) {
                event.label = QStringLiteral("always");
            } else if (transition.delayMs > 0) {
                event.label = QStringLiteral("after %1ms").arg(transition.delayMs);
            } else {
                event.label = QStringLiteral("(unset)");
            }
            row.events.push_back(event);
        }
        result.push_back(row);
    }
    return result;
}

// ---- MachineProblemsAdapter -------------------------------------------------

MachineProblemsAdapter::MachineProblemsAdapter() : ordo::qt::ViewModel(QStringLiteral("MachineProblemsAdapter")) {}

template <typename EventT>
void MachineProblemsAdapter::watch() {
    subscribe<EventT>(std::function<void(const EventT&)>([this](const EventT&) { emit problemsChanged(); }));
}

void MachineProblemsAdapter::onRegister() {
    doc_ = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);

    // Exactly the facts validate() reads.
    watch<events::StateAdded>();
    watch<events::StateRenamed>();
    watch<events::StateKindChanged>();
    watch<events::InitialStateChanged>();
    watch<events::EntryActionsChanged>();
    watch<events::ExitActionsChanged>();
    watch<events::StateDeleted>();
    watch<events::TransitionAdded>();
    watch<events::TransitionEventChanged>();
    watch<events::TransitionGuardChanged>();
    watch<events::TransitionActionChanged>();
    watch<events::TransitionDelayChanged>();
    watch<events::TransitionRetargeted>();
    watch<events::TransitionDeleted>();
    watch<events::MachineNameChanged>();
    watch<events::MachineSnapshotPublished>();  // restore/undo/redo rebuild path
}

QVector<Problem> MachineProblemsAdapter::problems() const {
    if (doc_ == nullptr) {
        return {};
    }
    return validate(doc_->machine());
}

// ---- DocumentSession's command bootstrap -------------------------------------

namespace {

// Edit commands are UndoCapture-wrapped, the undo pair and sim family are
// plain; the sim family shares this session's SimClock via std::ref.
void registerSessionCommands(ordo::core::Kernel& kernel, SimClock& clock) {
    kernel.registerCommand<events::AddStateRequested, UndoCaptureCommand<AddStateCommand, events::AddStateRequested>>();
    kernel.registerCommand<events::RenameStateRequested,
                            UndoCaptureCommand<RenameStateCommand, events::RenameStateRequested>>();
    kernel.registerCommand<events::SetStateKindRequested,
                            UndoCaptureCommand<SetStateKindCommand, events::SetStateKindRequested>>();
    kernel.registerCommand<events::MoveStateRequested,
                            UndoCaptureCommand<MoveStateCommand, events::MoveStateRequested>>();
    kernel.registerCommand<events::ReparentStateRequested,
                            UndoCaptureCommand<ReparentStateCommand, events::ReparentStateRequested>>();
    kernel.registerCommand<events::SetInitialChildRequested,
                            UndoCaptureCommand<SetInitialChildCommand, events::SetInitialChildRequested>>();
    kernel.registerCommand<events::SetEntryActionsRequested,
                            UndoCaptureCommand<SetEntryActionsCommand, events::SetEntryActionsRequested>>();
    kernel.registerCommand<events::SetExitActionsRequested,
                            UndoCaptureCommand<SetExitActionsCommand, events::SetExitActionsRequested>>();
    kernel.registerCommand<events::SetDescriptionRequested,
                            UndoCaptureCommand<SetDescriptionCommand, events::SetDescriptionRequested>>();
    kernel.registerCommand<events::SetTagsRequested, UndoCaptureCommand<SetTagsCommand, events::SetTagsRequested>>();
    kernel.registerCommand<
        events::SetHistoryDeepRequested,
        UndoCaptureCommand<SetHistoryDeepCommand, events::SetHistoryDeepRequested>>();
    kernel.registerCommand<
        events::SetInvokeSrcRequested,
        UndoCaptureCommand<SetInvokeSrcCommand, events::SetInvokeSrcRequested>>();
    kernel.registerCommand<
        events::SetInvokeIdRequested,
        UndoCaptureCommand<SetInvokeIdCommand, events::SetInvokeIdRequested>>();
    kernel.registerCommand<
        events::SetInvokeOutputTypeRequested,
        UndoCaptureCommand<SetInvokeOutputTypeCommand, events::SetInvokeOutputTypeRequested>>();
    kernel.registerCommand<
        events::SetInvocationsRequested,
        UndoCaptureCommand<SetInvocationsCommand, events::SetInvocationsRequested>>();
    kernel.registerCommand<
        events::SetStateColorRequested,
        UndoCaptureCommand<SetStateColorCommand, events::SetStateColorRequested>>();
    kernel.registerCommand<
        events::SetTransitionColorRequested,
        UndoCaptureCommand<SetTransitionColorCommand, events::SetTransitionColorRequested>>();
    kernel.registerCommand<
        events::SetNoteColorRequested,
        UndoCaptureCommand<SetNoteColorCommand, events::SetNoteColorRequested>>();
    kernel.registerCommand<
        events::SetMachineSelfRequested,
        UndoCaptureCommand<SetMachineSelfCommand, events::SetMachineSelfRequested>>();
    kernel.registerCommand<events::SetMachineNameRequested,
                            UndoCaptureCommand<SetMachineNameCommand, events::SetMachineNameRequested>>();
    kernel.registerCommand<events::SetInitialStateRequested,
                            UndoCaptureCommand<SetInitialStateCommand, events::SetInitialStateRequested>>();
    kernel.registerCommand<events::AddTransitionRequested,
                            UndoCaptureCommand<AddTransitionCommand, events::AddTransitionRequested>>();
    kernel.registerCommand<events::SetTransitionEventRequested,
                            UndoCaptureCommand<SetTransitionEventCommand, events::SetTransitionEventRequested>>();
    kernel.registerCommand<events::SetTransitionGuardRequested,
                            UndoCaptureCommand<SetTransitionGuardCommand, events::SetTransitionGuardRequested>>();
    kernel.registerCommand<events::SetTransitionActionRequested,
                            UndoCaptureCommand<SetTransitionActionCommand, events::SetTransitionActionRequested>>();
    kernel.registerCommand<events::RenameGuardRequested,
                            UndoCaptureCommand<RenameGuardCommand, events::RenameGuardRequested>>();
    kernel.registerCommand<events::RenameActionRequested,
                            UndoCaptureCommand<RenameActionCommand, events::RenameActionRequested>>();
    kernel.registerCommand<events::SetTransitionDelayRequested,
                            UndoCaptureCommand<SetTransitionDelayCommand, events::SetTransitionDelayRequested>>();
    kernel.registerCommand<events::SetTransitionReenterRequested,
                            UndoCaptureCommand<SetTransitionReenterCommand, events::SetTransitionReenterRequested>>();
    kernel.registerCommand<events::SetTransitionAlwaysRequested,
                            UndoCaptureCommand<SetTransitionAlwaysCommand, events::SetTransitionAlwaysRequested>>();
    kernel.registerCommand<events::SetTransitionPayloadTypeRequested,
                            UndoCaptureCommand<SetTransitionPayloadTypeCommand, events::SetTransitionPayloadTypeRequested>>();
    kernel.registerCommand<events::RetargetTransitionRequested,
                            UndoCaptureCommand<RetargetTransitionCommand, events::RetargetTransitionRequested>>();
    kernel.registerCommand<events::MoveTransitionLabelRequested,
                            UndoCaptureCommand<MoveTransitionLabelCommand, events::MoveTransitionLabelRequested>>();
    // Auto layout: one plan, one Transaction, one undo step.
    kernel.registerCommand<events::ApplyLayoutPlanRequested,
                            UndoCaptureCommand<ApplyLayoutPlanCommand, events::ApplyLayoutPlanRequested>>();
    kernel.registerCommand<events::SetTransitionBendpointsRequested,
                            UndoCaptureCommand<SetTransitionBendpointsCommand, events::SetTransitionBendpointsRequested>>();
    kernel.registerCommand<events::DeleteStateRequested,
                            UndoCaptureCommand<DeleteStateCommand, events::DeleteStateRequested>>();
    kernel.registerCommand<events::DeleteTransitionRequested,
                            UndoCaptureCommand<DeleteTransitionCommand, events::DeleteTransitionRequested>>();

    kernel.registerCommand<events::AddNoteRequested, UndoCaptureCommand<AddNoteCommand, events::AddNoteRequested>>();
    kernel.registerCommand<events::MoveNoteRequested,
                            UndoCaptureCommand<MoveNoteCommand, events::MoveNoteRequested>>();
    kernel.registerCommand<events::SetNoteTextRequested,
                            UndoCaptureCommand<SetNoteTextCommand, events::SetNoteTextRequested>>();
    kernel.registerCommand<events::DeleteNoteRequested,
                            UndoCaptureCommand<DeleteNoteCommand, events::DeleteNoteRequested>>();

    kernel.registerCommand<
        events::AddContextVariableRequested,
        UndoCaptureCommand<AddContextVariableCommand, events::AddContextVariableRequested>>();
    kernel.registerCommand<
        events::RenameContextVariableRequested,
        UndoCaptureCommand<RenameContextVariableCommand, events::RenameContextVariableRequested>>();
    kernel.registerCommand<events::SetContextTypeRequested,
                            UndoCaptureCommand<SetContextTypeCommand, events::SetContextTypeRequested>>();
    kernel.registerCommand<
        events::SetContextInitialValueRequested,
        UndoCaptureCommand<SetContextInitialValueCommand, events::SetContextInitialValueRequested>>();
    kernel.registerCommand<
        events::DeleteContextVariableRequested,
        UndoCaptureCommand<DeleteContextVariableCommand, events::DeleteContextVariableRequested>>();
    kernel.registerCommand<
        events::SetContextCustomTypeNameRequested,
        UndoCaptureCommand<SetContextCustomTypeNameCommand, events::SetContextCustomTypeNameRequested>>();
    kernel.registerCommand<
        events::SetExternalHeadersRequested,
        UndoCaptureCommand<SetExternalHeadersCommand, events::SetExternalHeadersRequested>>();
    kernel.registerCommand<
        events::AddStructDefinitionRequested,
        UndoCaptureCommand<AddStructDefinitionCommand, events::AddStructDefinitionRequested>>();
    kernel.registerCommand<
        events::SetStructDefinitionRequested,
        UndoCaptureCommand<SetStructDefinitionCommand, events::SetStructDefinitionRequested>>();
    kernel.registerCommand<
        events::DeleteStructDefinitionRequested,
        UndoCaptureCommand<DeleteStructDefinitionCommand, events::DeleteStructDefinitionRequested>>();

    kernel.registerCommand<events::MachineSnapshotRequested, MachineSnapshotCommand>();

    kernel.registerCommand<events::UndoRequested, UndoCommand>();
    kernel.registerCommand<events::RedoRequested, RedoCommand>();
    kernel.registerCommand<events::BeginUndoBatchRequested, BeginUndoBatchCommand>();
    kernel.registerCommand<events::EndUndoBatchRequested, EndUndoBatchCommand>();

    kernel.registerCommand<events::SetModeRequested, SetModeCommand>();
    kernel.registerCommand<events::RunRequested, RunCommand>(std::ref(clock));
    kernel.registerCommand<events::PauseRequested, PauseCommand>(std::ref(clock));
    kernel.registerCommand<events::ResetRequested, ResetCommand>(std::ref(clock));
    kernel.registerCommand<events::SendEventRequested, SendEventCommand>();
    kernel.registerCommand<events::SetGuardResultRequested, SetGuardResultCommand>();
    kernel.registerCommand<events::TickElapsed, TickCommand>();
    kernel.registerCommand<events::BackRequested, BackCommand>();
    kernel.registerCommand<events::CompleteInvocationRequested, CompleteInvocationCommand>();
    kernel.registerCommand<events::ToggleBreakpointRequested, ToggleBreakpointCommand>();
    kernel.registerCommand<events::SetTimescaleRequested, SetTimescaleCommand>(std::ref(clock));
    registerSettingCommands(kernel);
}

void removeSessionCommands(ordo::core::Kernel& kernel) {
    kernel.removeCommand<events::AddStateRequested>();
    kernel.removeCommand<events::RenameStateRequested>();
    kernel.removeCommand<events::SetStateKindRequested>();
    kernel.removeCommand<events::MoveStateRequested>();
    kernel.removeCommand<events::ReparentStateRequested>();
    kernel.removeCommand<events::SetInitialChildRequested>();
    kernel.removeCommand<events::SetEntryActionsRequested>();
    kernel.removeCommand<events::SetExitActionsRequested>();
    kernel.removeCommand<events::SetDescriptionRequested>();
    kernel.removeCommand<events::SetTagsRequested>();
    kernel.removeCommand<events::SetHistoryDeepRequested>();
    kernel.removeCommand<events::SetInvokeSrcRequested>();
    kernel.removeCommand<events::SetInvokeIdRequested>();
    kernel.removeCommand<events::SetInvokeOutputTypeRequested>();
    kernel.removeCommand<events::SetInvocationsRequested>();
    kernel.removeCommand<events::SetStateColorRequested>();
    kernel.removeCommand<events::SetTransitionColorRequested>();
    kernel.removeCommand<events::SetNoteColorRequested>();
    kernel.removeCommand<events::SetMachineSelfRequested>();
    kernel.removeCommand<events::SetMachineNameRequested>();
    kernel.removeCommand<events::SetInitialStateRequested>();
    kernel.removeCommand<events::AddTransitionRequested>();
    kernel.removeCommand<events::SetTransitionEventRequested>();
    kernel.removeCommand<events::SetTransitionGuardRequested>();
    kernel.removeCommand<events::SetTransitionActionRequested>();
    kernel.removeCommand<events::RenameGuardRequested>();
    kernel.removeCommand<events::RenameActionRequested>();
    kernel.removeCommand<events::SetTransitionDelayRequested>();
    kernel.removeCommand<events::RetargetTransitionRequested>();
    kernel.removeCommand<events::MoveTransitionLabelRequested>();
    kernel.removeCommand<events::ApplyLayoutPlanRequested>();
    kernel.removeCommand<events::SetTransitionBendpointsRequested>();
    kernel.removeCommand<events::DeleteStateRequested>();
    kernel.removeCommand<events::DeleteTransitionRequested>();
    kernel.removeCommand<events::AddNoteRequested>();
    kernel.removeCommand<events::MoveNoteRequested>();
    kernel.removeCommand<events::SetNoteTextRequested>();
    kernel.removeCommand<events::DeleteNoteRequested>();
    kernel.removeCommand<events::AddContextVariableRequested>();
    kernel.removeCommand<events::RenameContextVariableRequested>();
    kernel.removeCommand<events::SetContextTypeRequested>();
    kernel.removeCommand<events::SetContextInitialValueRequested>();
    kernel.removeCommand<events::DeleteContextVariableRequested>();
    kernel.removeCommand<events::SetTransitionPayloadTypeRequested>();
    kernel.removeCommand<events::SetContextCustomTypeNameRequested>();
    kernel.removeCommand<events::SetExternalHeadersRequested>();
    kernel.removeCommand<events::AddStructDefinitionRequested>();
    kernel.removeCommand<events::SetStructDefinitionRequested>();
    kernel.removeCommand<events::DeleteStructDefinitionRequested>();
    kernel.removeCommand<events::MachineSnapshotRequested>();
    kernel.removeCommand<events::UndoRequested>();
    kernel.removeCommand<events::RedoRequested>();
    kernel.removeCommand<events::BeginUndoBatchRequested>();
    kernel.removeCommand<events::EndUndoBatchRequested>();
    kernel.removeCommand<events::SetModeRequested>();
    kernel.removeCommand<events::RunRequested>();
    kernel.removeCommand<events::PauseRequested>();
    kernel.removeCommand<events::ResetRequested>();
    kernel.removeCommand<events::SendEventRequested>();
    kernel.removeCommand<events::SetGuardResultRequested>();
    kernel.removeCommand<events::TickElapsed>();
    kernel.removeCommand<events::BackRequested>();
    kernel.removeCommand<events::CompleteInvocationRequested>();
    kernel.removeCommand<events::ToggleBreakpointRequested>();
    kernel.removeCommand<events::SetTimescaleRequested>();
    kernel.removeCommand<events::SetSettingRequested>();
    kernel.removeCommand<events::ResetSettingRequested>();
}

}  // namespace

// ---- DocumentSession ---------------------------------------------------------

DocumentSession::DocumentSession(QString machineName, bool manualClock)
    : DocumentSession(std::move(machineName), Machine{}, manualClock) {}

DocumentSession::DocumentSession(QString machineName, Machine machine, bool manualClock)
    : machineName_(std::move(machineName)) {
    kernel_.registerAgent(std::make_shared<MachineDocAgent>());
    kernel_.registerAgent(std::make_shared<SimulationAgent>());
    kernel_.registerAgent(std::make_shared<UndoStore>());

    // Restore happens before any command, badge or pane exists, so nothing
    // downstream is notified.
    if (machine.name.isEmpty() && !machineName_.isEmpty()) {
        machine.name = machineName_;
    }

    // A machine with no geometry (every state at the origin) is laid out here,
    // as part of the load: no undo entry, no dirty flag. Without a GUI
    // application there are no fonts to measure with, so positions stand.
    if (machineHasNoGeometry(machine) && qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr) {
        autoLayoutResidualCount_ = applyAutoLayout(&machine, canvasItemMetrics()).residuals.size();
    }

    auto doc = kernel_.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    doc->restore(std::move(machine));

    // Undo never crosses a document.
    auto undo = kernel_.agentAs<UndoStore>(UndoStore::kName);
    undo->clearAll();

    // Real mode: Run/Pause start and stop the clock's ~50ms QTimer, so the
    // machine advances in the background. manualClock makes start()/stop()
    // no-ops; advanceClock() is then the only way time moves.
    clock_.setManualMode(manualClock);
    clock_.onTick = [this](int elapsedMs) { kernel_.send(events::TickElapsed{.elapsedMs = elapsedMs}); };

    // Commands must be registered before badgeHost_ adds its adapters or any
    // attachView() runs.
    registerSessionCommands(kernel_, clock_);

    // Persistent mini ViewHost: lives as long as the session, independent of pane bindings.
    badgeHost_ = std::make_unique<ordo::qt::ViewHost>(kernel_);
    badge_ = badgeHost_->add<StatusBadgeAdapter>();
    outline_ = badgeHost_->add<MachineOutlineAdapter>();
    problems_ = badgeHost_->add<MachineProblemsAdapter>();
}

DocumentSession::~DocumentSession() {
    // View layer first. Panes normally detachView() beforehand, but clearing
    // here is safe regardless: ~CanvasPresenter only unsubscribes.
    viewBindings_.clear();
    badgeHost_.reset();

    removeSessionCommands(kernel_);
}

void DocumentSession::setMachineName(const QString& name) {
    machineName_ = name;
    auto doc = kernel_.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (doc != nullptr && doc->machine().name != name) {
        doc->setMachineName(name);
    }
}

// The plan is computed here because the metrics need fonts; the command only
// applies it, so the counts below are exactly the entries it will apply.
DocumentSession::AutoLayoutRun DocumentSession::runAutoLayout(const AutoLayoutOptions& options) {
    AutoLayoutRun run;
    auto doc = kernel_.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (doc == nullptr || qobject_cast<QGuiApplication*>(QCoreApplication::instance()) == nullptr) {
        return run;
    }
    const AutoLayoutPlan plan = computeAutoLayoutPlan(doc->machine(), canvasItemMetrics(), options);
    for (const StatePlacement& placement : plan.states) {
        const State* state = doc->findState(placement.id);
        if (state != nullptr && (state->pos.x() != placement.pos.x() || state->pos.y() != placement.pos.y())) {
            ++run.movedStates;
        }
    }
    for (const LabelPlacement& placement : plan.labels) {
        const Transition* transition = doc->findTransition(placement.id);
        // A label changes when its ratio moves OR a leftover drag offset is
        // about to be cleared (ApplyLayoutPlanCommand resets it).
        if (transition != nullptr &&
            (transition->labelRatio != placement.ratio || !transition->labelOffset.isNull())) {
            ++run.changedLabels;
        }
    }
    run.residuals = static_cast<int>(plan.result.residuals.size());
    run.ran = true;
    kernel_.send(events::ApplyLayoutPlanRequested{.states = plan.states, .labels = plan.labels});
    return run;
}

CanvasPresenter* DocumentSession::attachView(CanvasView* view) {
    auto scene = std::make_unique<QGraphicsScene>();
    // NoIndex: a graphics effect's margin changes item bounds per view zoom
    // without telling the BSP index, which left freed items in its leaves.
    scene->setItemIndexMethod(QGraphicsScene::NoIndex);
    view->setScene(scene.get());

    auto host = std::make_unique<ordo::qt::ViewHost>(kernel_);
    CanvasPresenter* presenter = host->add<CanvasPresenter>(scene.get(), view);

    viewBindings_.push_back(ViewBinding{view, std::move(scene), std::move(host)});
    return presenter;
}

void DocumentSession::detachView(CanvasView* view) {
    auto it = std::find_if(viewBindings_.begin(), viewBindings_.end(),
                            [view](const ViewBinding& binding) { return binding.view == view; });
    if (it == viewBindings_.end()) {
        return;
    }
    view->setScene(nullptr);
    viewBindings_.erase(it);  // ~host runs first (CanvasPresenter unsubscribes), then ~scene
}

bool DocumentSession::saveTo(const QString& osdPath, QString* error) {
    auto doc = kernel_.agentAs<MachineDocAgent>(MachineDocAgent::kName);
    if (doc == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("MachineDocAgent not registered");
        }
        return false;
    }
    return saveMachine(doc->machine(), osdPath, error);
}

// ---- Project-level persistence ----------------------------------------------

std::vector<std::unique_ptr<DocumentSession>> loadProjectSessions(const QString& ossPath, Project* projectOut,
                                                                    QString* error, bool manualClock) {
    auto loaded = readProjectFiles(ossPath, nullptr, error);
    if (!loaded.has_value()) {
        return {};
    }

    std::vector<std::unique_ptr<DocumentSession>> sessions;
    sessions.reserve(loaded->machines.size());
    for (auto& entry : loaded->machines) {
        auto session = std::make_unique<DocumentSession>(entry.name, std::move(entry.machine), manualClock);
        session->setRelativePath(entry.relativePath);
        sessions.push_back(std::move(session));
    }

    if (projectOut != nullptr) {
        *projectOut = loaded->project;
    }
    return sessions;
}

bool saveProjectSessions(const QString& ossPath, const std::vector<DocumentSession*>& sessions,
                          const QString& outputDir, const QString& rootNamespace, Project* projectOut,
                          QString* error) {
    std::vector<MachineFileEntry> entries;
    entries.reserve(sessions.size());
    Project project;
    project.name = QFileInfo(ossPath).completeBaseName();
    project.outputDir = outputDir;
    project.rootNamespace = rootNamespace;

    for (DocumentSession* session : sessions) {
        auto doc = session->kernel().agentAs<MachineDocAgent>(MachineDocAgent::kName);
        if (doc == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("MachineDocAgent not registered");
            }
            return false;
        }
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
        project.machineFiles.push_back(fileName);
    }

    if (!writeProjectFiles(ossPath, project, entries, nullptr, error)) {
        return false;
    }
    if (projectOut != nullptr) {
        *projectOut = project;
    }
    return true;
}

XStateSessionImport importXStateNewSession(const QString& jsonPath, bool manualClock) {
    XStateSessionImport result;
    XStateImportResult imported = importXStateFile(jsonPath);
    result.diagnostics = imported.diagnostics;
    if (!imported.ok) {
        result.error = imported.error;
        return result;  // session stays null
    }
    // The imported name is never empty (falls back to "Imported").
    const QString name = imported.machine.name;
    result.session = std::make_unique<DocumentSession>(name, std::move(imported.machine), manualClock);
    return result;
}

}  // namespace app
