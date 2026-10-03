#include "view/shell/logic_panel.h"

#include "infra/logic_inventory.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/sim_agent.h"
#include "model/sim_events.h"

namespace app {

namespace {

// One row per declared invoke across all states, in document order.
QVector<ActorRow> deriveActorRows(const Machine& machine) {
    QVector<ActorRow> rows;
    for (const State& state : machine.states) {
        for (const Invocation& inv : state.effectiveInvocations()) {
            if (inv.src.isEmpty()) {
                continue;
            }
            ActorRow row;
            row.stateId = state.id;
            row.stateName = state.name;
            row.src = inv.src;
            row.effectiveId = effectiveInvocationId(inv);
            row.outputType = inv.outputType;
            rows.push_back(row);
        }
    }
    return rows;
}

}  // namespace

LogicAdapter::LogicAdapter(LogicPanel* panel, QString machineName)
    : Presenter(QStringLiteral("LogicAdapter"), panel), panel_(panel), machineName_(std::move(machineName)) {}

void LogicAdapter::onRegister() {
    panel_->setMachineName(machineName_);

    doc_ = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    sim_ = context().agentAs<SimulationAgent>(SimulationAgent::kName);  // live overlay source

    // The Context section's only input.
    subscribe<events::ContextVariableAdded>(&LogicAdapter::onContextVariableAdded);
    subscribe<events::ContextVariableRenamed>(&LogicAdapter::onContextVariableRenamed);
    subscribe<events::ContextTypeChanged>(&LogicAdapter::onContextTypeChanged);
    subscribe<events::ContextInitialValueChanged>(&LogicAdapter::onContextInitialValueChanged);
    subscribe<events::ContextVariableDeleted>(&LogicAdapter::onContextVariableDeleted);

    connect(panel_, &LogicPanel::contextAddClicked, this,
            [this] { context().send(events::AddContextVariableRequested{}); });
    connect(panel_, &LogicPanel::contextNameEditingFinished, this, &LogicAdapter::onContextNameEditingFinished);
    // Hook renames: the panel emits text pairs; the commands own the cascade
    // (Hook-class rows only, blank `after` refused) and undo as one step.
    connect(panel_, &LogicPanel::guardRenameRequested, this, [this](const QString& before, const QString& after) {
        context().send(events::RenameGuardRequested{.before = before, .after = after});
    });
    connect(panel_, &LogicPanel::actionRenameRequested, this, [this](const QString& before, const QString& after) {
        context().send(events::RenameActionRequested{.before = before, .after = after});
    });
    connect(panel_, &LogicPanel::contextTypeActivated, this, &LogicAdapter::onContextTypeActivated);
    connect(panel_, &LogicPanel::contextInitialValueEditingFinished, this,
            &LogicAdapter::onContextInitialValueEditingFinished);
    connect(panel_, &LogicPanel::contextDeleteClicked, this, &LogicAdapter::onContextDeleteClicked);

    // Facts that can move the inventory. Click-to-reveal has no connect() here:
    // transitionRowActivated/stateRowActivated only pick a canvas selection,
    // so MainWindow wires them directly.
    subscribe<events::TransitionGuardChanged>(&LogicAdapter::onTransitionGuardChanged);
    subscribe<events::TransitionActionChanged>(&LogicAdapter::onTransitionActionChanged);
    subscribe<events::EntryActionsChanged>(&LogicAdapter::onEntryActionsChanged);
    subscribe<events::ExitActionsChanged>(&LogicAdapter::onExitActionsChanged);
    subscribe<events::TransitionAdded>(&LogicAdapter::onTransitionAdded);
    subscribe<events::TransitionDeleted>(&LogicAdapter::onTransitionDeleted);
    subscribe<events::StateAdded>(&LogicAdapter::onStateAdded);
    subscribe<events::StateDeleted>(&LogicAdapter::onStateDeleted);
    subscribe<events::MachineSnapshotPublished>(&LogicAdapter::onMachineSnapshotPublished);
    // Invoke fields changing on a state that keeps existing.
    subscribe<events::InvokeSrcChanged>(&LogicAdapter::onInvokeSrcChanged);
    subscribe<events::InvokeIdChanged>(&LogicAdapter::onInvokeIdChanged);
    subscribe<events::InvokeOutputTypeChanged>(&LogicAdapter::onInvokeOutputTypeChanged);
    subscribe<events::InvocationsChanged>(&LogicAdapter::onInvocationsChanged);

    subscribe<events::ExternalHeadersChanged>(&LogicAdapter::onExternalHeadersChanged);
    subscribe<events::StructDefinitionAdded>(&LogicAdapter::onStructDefinitionAdded);
    subscribe<events::StructDefinitionChanged>(&LogicAdapter::onStructDefinitionChanged);
    subscribe<events::StructDefinitionDeleted>(&LogicAdapter::onStructDefinitionDeleted);
    subscribe<events::ContextCustomTypeNameChanged>(&LogicAdapter::onContextCustomTypeNameChanged);

    connect(panel_, &LogicPanel::contextCustomTypeNameActivated, this,
            &LogicAdapter::onContextCustomTypeNameActivated);
    connect(panel_, &LogicPanel::structAddClicked, this, &LogicAdapter::onStructAddClicked);
    connect(panel_, &LogicPanel::structDeleteClicked, this, &LogicAdapter::onStructDeleteClicked);
    connect(panel_, &LogicPanel::structChanged, this, &LogicAdapter::onStructChanged);
    connect(panel_, &LogicPanel::externalHeadersEditingFinished, this,
            &LogicAdapter::onExternalHeadersEditingFinished);

    refreshContext();  // initial paint on attach
    refreshTypes();
    // Live overlay: TraceAppended covers every assign (so every context value
    // change); the lifecycle facts cover entering and leaving a run.
    subscribe<events::TraceAppended>(&LogicAdapter::onTraceAppended);
    subscribe<events::SimulationStarted>(&LogicAdapter::onSimulationStarted);
    subscribe<events::SimulationReset>(&LogicAdapter::onSimulationReset);
    subscribe<events::TransitionFired>(&LogicAdapter::onTransitionFired);
    subscribe<events::ModeChanged>(&LogicAdapter::onModeChanged);
    subscribe<events::GuardResultChanged>(&LogicAdapter::onGuardResultChanged);

    refreshLogic();
    refreshLive();
}

void LogicAdapter::onContextVariableAdded(const events::ContextVariableAdded&) { refreshContext(); }
void LogicAdapter::onContextVariableRenamed(const events::ContextVariableRenamed&) { refreshContext(); }
void LogicAdapter::onContextTypeChanged(const events::ContextTypeChanged&) { refreshContext(); }
void LogicAdapter::onContextInitialValueChanged(const events::ContextInitialValueChanged&) { refreshContext(); }
void LogicAdapter::onContextVariableDeleted(const events::ContextVariableDeleted&) { refreshContext(); }

void LogicAdapter::onContextNameEditingFinished(quint64 id, QString name) {
    context().send(events::RenameContextVariableRequested{.id = id, .name = name});
}

void LogicAdapter::onContextTypeActivated(quint64 id, ContextType type) {
    context().send(events::SetContextTypeRequested{.id = id, .type = type});
}

void LogicAdapter::onContextInitialValueEditingFinished(quint64 id, QString initialValue) {
    context().send(events::SetContextInitialValueRequested{.id = id, .initialValue = initialValue});
}

void LogicAdapter::onContextDeleteClicked(quint64 id) {
    context().send(events::DeleteContextVariableRequested{.id = id});
}

void LogicAdapter::refreshContext() {
    if (doc_ == nullptr) {
        return;
    }
    panel_->setContextVariables(doc_->machine().context);
}

void LogicAdapter::onTransitionGuardChanged(const events::TransitionGuardChanged&) { refreshLogic(); }
void LogicAdapter::onTransitionActionChanged(const events::TransitionActionChanged&) { refreshLogic(); }
void LogicAdapter::onEntryActionsChanged(const events::EntryActionsChanged&) { refreshLogic(); }
void LogicAdapter::onExitActionsChanged(const events::ExitActionsChanged&) { refreshLogic(); }
void LogicAdapter::onTransitionAdded(const events::TransitionAdded&) { refreshLogic(); }
void LogicAdapter::onTransitionDeleted(const events::TransitionDeleted&) { refreshLogic(); }
void LogicAdapter::onStateAdded(const events::StateAdded&) { refreshLogic(); }
void LogicAdapter::onStateDeleted(const events::StateDeleted&) { refreshLogic(); }
void LogicAdapter::onMachineSnapshotPublished(const events::MachineSnapshotPublished&) {
    refreshLogic();
    refreshTypes();
}

void LogicAdapter::onInvokeSrcChanged(const events::InvokeSrcChanged&) { refreshLogic(); }
void LogicAdapter::onInvokeIdChanged(const events::InvokeIdChanged&) { refreshLogic(); }
void LogicAdapter::onInvokeOutputTypeChanged(const events::InvokeOutputTypeChanged&) { refreshLogic(); }
void LogicAdapter::onInvocationsChanged(const events::InvocationsChanged&) { refreshLogic(); }

void LogicAdapter::onTraceAppended(const events::TraceAppended&) { refreshLive(); }
void LogicAdapter::onSimulationStarted(const events::SimulationStarted&) { refreshLive(); }
void LogicAdapter::onSimulationReset(const events::SimulationReset&) { refreshLive(); }
void LogicAdapter::onTransitionFired(const events::TransitionFired&) { refreshLive(); }
void LogicAdapter::onModeChanged(const events::ModeChanged&) { refreshLive(); }
void LogicAdapter::onGuardResultChanged(const events::GuardResultChanged&) { refreshLive(); }

void LogicAdapter::refreshLive() {
    if (sim_ == nullptr) {
        return;  // defensive: SimulationAgent is always registered alongside the document in this app
    }
    const bool running = sim_->running();
    QHash<QString, LogicPanel::LiveGuardResult> results;
    // Keyed by guard source, as the inventory merges rows. Candidates sharing
    // one source within a macrostep show the last evaluation.
    for (const GuardEvaluation& evaluation : sim_->lastGuardEvaluations()) {
        if (evaluation.source.trimmed().isEmpty()) {
            continue;  // a blank guard is "no guard" -- it owns no Guards row to overlay
        }
        results.insert(evaluation.source.trimmed(),
                        LogicPanel::LiveGuardResult{.decided = evaluation.decided, .result = evaluation.result});
    }
    panel_->setLiveContextValues(sim_->contextValues(), running);
    panel_->setLiveGuardResults(results, running);
    // Already the effective-id set that ActorRow rows are keyed by.
    panel_->setLiveActorStates(sim_->liveInvocationIds(), running);
}

void LogicAdapter::refreshLogic() {
    if (doc_ == nullptr) {
        return;
    }
    const LogicInventory inventoryResult = inventory(doc_->machine());
    panel_->setGuardRows(inventoryResult.guards);
    panel_->setActionRows(inventoryResult.actions);
    panel_->setActorRows(deriveActorRows(doc_->machine()));
}

void LogicAdapter::onStructAddClicked() {
    if (doc_ == nullptr) return;
    int n = 1;
    QString name = QStringLiteral("Struct%1").arg(n);
    auto exists = [this](const QString& nm) {
        for (const auto& t : doc_->machine().types) {
            if (t.name == nm) return true;
        }
        return false;
    };
    while (exists(name)) {
        name = QStringLiteral("Struct%1").arg(++n);
    }
    StructDefinition def;
    def.name = name;
    context().send(events::AddStructDefinitionRequested{.definition = def});
}

void LogicAdapter::onStructDeleteClicked(quint64 id) {
    context().send(events::DeleteStructDefinitionRequested{.id = id});
}

void LogicAdapter::onStructChanged(StructDefinition def) {
    context().send(events::SetStructDefinitionRequested{.id = def.id, .definition = def});
}

void LogicAdapter::onExternalHeadersEditingFinished(QStringList headers) {
    context().send(events::SetExternalHeadersRequested{.headers = headers});
}

void LogicAdapter::onContextCustomTypeNameActivated(quint64 id, QString customTypeName) {
    context().send(events::SetContextCustomTypeNameRequested{.id = id, .customTypeName = customTypeName});
}

void LogicAdapter::onExternalHeadersChanged(const events::ExternalHeadersChanged&) { refreshTypes(); }
void LogicAdapter::onStructDefinitionAdded(const events::StructDefinitionAdded&) { refreshTypes(); refreshContext(); }
void LogicAdapter::onStructDefinitionChanged(const events::StructDefinitionChanged&) { refreshTypes(); refreshContext(); }
void LogicAdapter::onStructDefinitionDeleted(const events::StructDefinitionDeleted&) { refreshTypes(); refreshContext(); }
void LogicAdapter::onContextCustomTypeNameChanged(const events::ContextCustomTypeNameChanged&) { refreshContext(); }

void LogicAdapter::refreshTypes() {
    if (doc_ == nullptr) return;
    panel_->setTypes(doc_->machine().types, doc_->machine().externalHeaders);
}

}  // namespace app
