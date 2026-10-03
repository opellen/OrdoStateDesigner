// InspectorAdapter: the ordo Presenter half of the Inspector. The widget half is
// in inspector_panel.cpp; helpers both share are in inspector_panel_internal.h.
#include "view/shell/inspector_panel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringListModel>
#include <QSyntaxHighlighter>
#include <QTabWidget>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include "infra/code_generator.h"
#include "infra/node_code_projector.h"
#include "infra/expression.h"        // single-string guard validation
#include "infra/logic_inventory.h"   // guard suggestions, derived not stored
#include "infra/machine_validator.h"

#include "view/shell/expression_editor_dialog.h"
#include "view/shell/inspector_panel_internal.h"

namespace app {

using namespace inspector_detail;

// ---- InspectorAdapter --------------------------------------------------------

InspectorAdapter::InspectorAdapter(InspectorPanel* panel, QString machineName)
    : Presenter(QStringLiteral("InspectorAdapter"), panel), panel_(panel), machineName_(std::move(machineName)) {}

void InspectorAdapter::onRegister() {
    doc_ = context().agentAs<MachineDocAgent>(MachineDocAgent::kName);
    sim_ = context().agentAs<SimulationAgent>(SimulationAgent::kName);

    subscribe<events::StateAdded>(&InspectorAdapter::onStateAdded);
    subscribe<events::StateRenamed>(&InspectorAdapter::onStateRenamed);
    subscribe<events::StateKindChanged>(&InspectorAdapter::onStateKindChanged);
    subscribe<events::InitialStateChanged>(&InspectorAdapter::onInitialStateChanged);
    subscribe<events::StateMoved>(&InspectorAdapter::onStateMoved);
    subscribe<events::EntryActionsChanged>(&InspectorAdapter::onEntryActionsChanged);
    subscribe<events::ExitActionsChanged>(&InspectorAdapter::onExitActionsChanged);
    subscribe<events::DescriptionChanged>(&InspectorAdapter::onDescriptionChanged);
    subscribe<events::TagsChanged>(&InspectorAdapter::onTagsChanged);
    subscribe<events::HistoryDeepChanged>(&InspectorAdapter::onHistoryDeepChanged);
    subscribe<events::InvokeSrcChanged>(&InspectorAdapter::onInvokeSrcChanged);
    subscribe<events::InvokeIdChanged>(&InspectorAdapter::onInvokeIdChanged);
    subscribe<events::InvokeOutputTypeChanged>(&InspectorAdapter::onInvokeOutputTypeChanged);
    subscribe<events::InvocationsChanged>(&InspectorAdapter::onInvocationsChanged);
    subscribe<events::StateDeleted>(&InspectorAdapter::onStateDeleted);
    subscribe<events::TransitionAdded>(&InspectorAdapter::onTransitionAdded);
    subscribe<events::TransitionEventChanged>(&InspectorAdapter::onTransitionEventChanged);
    subscribe<events::TransitionGuardChanged>(&InspectorAdapter::onTransitionGuardChanged);
    subscribe<events::TransitionActionChanged>(&InspectorAdapter::onTransitionActionChanged);
    subscribe<events::TransitionDelayChanged>(&InspectorAdapter::onTransitionDelayChanged);
    subscribe<events::TransitionReenterChanged>(&InspectorAdapter::onTransitionReenterChanged);
    subscribe<events::TransitionAlwaysChanged>(&InspectorAdapter::onTransitionAlwaysChanged);
    subscribe<events::TransitionRetargeted>(&InspectorAdapter::onTransitionRetargeted);
    subscribe<events::TransitionDeleted>(&InspectorAdapter::onTransitionDeleted);
    subscribe<events::TransitionPayloadTypeChanged>(&InspectorAdapter::onTransitionPayloadTypeChanged);
    subscribe<events::StructDefinitionAdded>(&InspectorAdapter::onStructDefinitionAdded);
    subscribe<events::StructDefinitionChanged>(&InspectorAdapter::onStructDefinitionChanged);
    subscribe<events::StructDefinitionDeleted>(&InspectorAdapter::onStructDefinitionDeleted);
    subscribe<events::StateColorChanged>(&InspectorAdapter::onStateColorChanged);
    subscribe<events::TransitionColorChanged>(&InspectorAdapter::onTransitionColorChanged);
    subscribe<events::NoteColorChanged>(&InspectorAdapter::onNoteColorChanged);
    subscribe<events::NoteTextChanged>(&InspectorAdapter::onNoteTextChanged);
    subscribe<events::NoteDeleted>(&InspectorAdapter::onNoteDeleted);
    subscribe<events::MachineSnapshotPublished>(&InspectorAdapter::onSnapshotPublished);
    subscribe<events::ModeChanged>(&InspectorAdapter::onModeChanged);
    subscribe<events::ActiveStateChanged>(&InspectorAdapter::onActiveStateChanged);
    subscribe<events::SimulationStarted>(&InspectorAdapter::onSimulationStarted);
    subscribe<events::SimulationPaused>(&InspectorAdapter::onSimulationPaused);
    subscribe<events::SimulationReset>(&InspectorAdapter::onSimulationReset);
    subscribe<events::TimescaleChanged>(&InspectorAdapter::onTimescaleChanged);
    subscribe<events::BreakpointHit>(&InspectorAdapter::onBreakpointHit);
    subscribe<events::GuardResultChanged>(&InspectorAdapter::onGuardResultChanged);
    subscribe<events::TraceAppended>(&InspectorAdapter::onTraceAppended);

    connect(panel_, &InspectorPanel::stateNameEditingFinished, this, &InspectorAdapter::onNameEditingFinished);
    connect(panel_, &InspectorPanel::stateKindEdited, this, &InspectorAdapter::onKindEdited);
    connect(panel_, &InspectorPanel::stateInitialToggled, this, &InspectorAdapter::onInitialToggled);
    connect(panel_, &InspectorPanel::stateColorEdited, this, [this](ElementColor c) {
        if (selectionKind_ == SelectionKind::State && selectionId_ != 0) {
            context().send(events::SetStateColorRequested{selectionId_, c});
        }
    });
    connect(panel_, &InspectorPanel::entryActionsEditingFinished, this,
            &InspectorAdapter::onEntryActionsEditingFinished);
    connect(panel_, &InspectorPanel::exitActionsEditingFinished, this,
            &InspectorAdapter::onExitActionsEditingFinished);
    connect(panel_, &InspectorPanel::descriptionEditingFinished, this,
            &InspectorAdapter::onDescriptionEditingFinished);
    connect(panel_, &InspectorPanel::tagsEditingFinished, this, &InspectorAdapter::onTagsEditingFinished);
    connect(panel_, &InspectorPanel::historyDeepToggled, this, &InspectorAdapter::onHistoryDeepToggled);
    connect(panel_, &InspectorPanel::invokeSrcEditingFinished, this, &InspectorAdapter::onInvokeSrcEditingFinished);
    connect(panel_, &InspectorPanel::invokeIdEditingFinished, this, &InspectorAdapter::onInvokeIdEditingFinished);
    connect(panel_, &InspectorPanel::invokeOutputTypeEdited, this, &InspectorAdapter::onInvokeOutputTypeEdited);
    connect(panel_, &InspectorPanel::invocationsEdited, this, &InspectorAdapter::onInvocationsEdited);
    connect(panel_, &InspectorPanel::invocationDoneClicked, this, &InspectorAdapter::onInvocationDoneClicked);
    connect(panel_, &InspectorPanel::invocationErrorClicked, this, &InspectorAdapter::onInvocationErrorClicked);
    connect(panel_, &InspectorPanel::transitionEventEditingFinished, this, &InspectorAdapter::onEventEditingFinished);
    connect(panel_, &InspectorPanel::transitionEventTextChanged, this, &InspectorAdapter::onEventTextChanged);
    connect(panel_, &InspectorPanel::transitionGuardEditingFinished, this, &InspectorAdapter::onGuardEditingFinished);
    connect(panel_, &InspectorPanel::transitionGuardTextChanged, this, &InspectorAdapter::onGuardTextChanged);
    connect(panel_, &InspectorPanel::transitionGuardExpandRequested, this, &InspectorAdapter::onGuardExpandRequested);
    connect(panel_, &InspectorPanel::transitionActionEditingFinished, this,
            &InspectorAdapter::onActionEditingFinished);
    connect(panel_, &InspectorPanel::transitionActionTextChanged, this,
            &InspectorAdapter::onActionTextChanged);
    connect(panel_, &InspectorPanel::transitionActionExpandRequested, this, &InspectorAdapter::onActionExpandRequested);
    connect(panel_, &InspectorPanel::transitionDelayEdited, this, &InspectorAdapter::onDelayEdited);
    connect(panel_, &InspectorPanel::transitionReenterToggled, this, &InspectorAdapter::onReenterToggled);
    connect(panel_, &InspectorPanel::transitionAlwaysToggled, this, &InspectorAdapter::onAlwaysToggled);
    connect(panel_, &InspectorPanel::transitionColorEdited, this, [this](ElementColor c) {
        if (selectionKind_ == SelectionKind::Transition && selectionId_ != 0) {
            context().send(events::SetTransitionColorRequested{selectionId_, c});
        }
    });
    connect(panel_, &InspectorPanel::transitionTargetEditingFinished, this,
            &InspectorAdapter::onTargetEditingFinished);
    connect(panel_, &InspectorPanel::transitionTargetTextChanged, this,
            &InspectorAdapter::onTargetTextChanged);
    connect(panel_, &InspectorPanel::transitionPayloadTypeEditingFinished, this,
            &InspectorAdapter::onPayloadTypeEditingFinished);
    connect(panel_, &InspectorPanel::noteColorEdited, this, [this](ElementColor c) {
        if (selectionKind_ == SelectionKind::Note && selectionId_ != 0) {
            context().send(events::SetNoteColorRequested{selectionId_, c});
        }
    });
    connect(panel_, &InspectorPanel::noteTextEditingFinished, this, [this](const QString& text) {
        if (selectionKind_ == SelectionKind::Note && selectionId_ != 0) {
            context().send(events::SetNoteTextRequested{selectionId_, text});
        }
    });
    connect(panel_, &InspectorPanel::runClicked, this, [this] { context().send(events::RunRequested{}); });
    connect(panel_, &InspectorPanel::pauseClicked, this, [this] { context().send(events::PauseRequested{}); });
    connect(panel_, &InspectorPanel::resetClicked, this, [this] { context().send(events::ResetRequested{}); });
    connect(panel_, &InspectorPanel::backClicked, this, [this] { context().send(events::BackRequested{}); });
    connect(panel_, &InspectorPanel::timescaleChanged, this, &InspectorAdapter::onTimescaleSelected);
    connect(panel_, &InspectorPanel::sendEventClicked, this, &InspectorAdapter::onSendEventClicked);
    connect(panel_, &InspectorPanel::guardToggled, this, &InspectorAdapter::onGuardToggled);

    // No selection yet: default to the Machine tab so no frame shows stale State/Transition fields.
    refreshMachineTab();
    refreshCodeTab();
    panel_->showTabForSelection(SelectionKind::None);
}

void InspectorAdapter::setSelection(SelectionKind kind, quint64 id) {
    selectionKind_ = kind;
    selectionId_ = id;
    if (kind == SelectionKind::State) {
        refreshStateTab();
    } else if (kind == SelectionKind::Transition) {
        refreshTransitionTab();
    } else if (kind == SelectionKind::Note) {
        refreshNoteTab();
    } else {
        refreshMachineTab();
    }
    panel_->showTabForSelection(selectionKind_);  // refresh*Tab() above may have fallen back to None
}

void InspectorAdapter::onStateAdded(const events::StateAdded&) {
    refreshMachineTab();
    refreshCodeTab();
}
void InspectorAdapter::onTransitionAdded(const events::TransitionAdded&) {
    refreshMachineTab();
    refreshCodeTab();
}

void InspectorAdapter::onStateRenamed(const events::StateRenamed& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.id) {
        refreshStateTab();
    }
    refreshCodeTab();  // the state's enum identifier changed regardless of selection
}

void InspectorAdapter::onStateKindChanged(const events::StateKindChanged& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.id) {
        refreshStateTab();
    }
    // No refreshCodeTab(): kind does not feed generated output.
}

void InspectorAdapter::onInitialStateChanged(const events::InitialStateChanged&) {
    if (selectionKind_ == SelectionKind::State) {
        refreshStateTab();  // the checkbox on whichever state is inspected
    }
    refreshCodeTab();  // the generated agent's default state changed
}

void InspectorAdapter::onStateMoved(const events::StateMoved& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.id) {
        refreshStateTab();  // Position display tracks a live canvas drag
    }
    // No refreshCodeTab(): geometry is not emitted into generated code, so skip per-drag-frame work.
}

void InspectorAdapter::onEntryActionsChanged(const events::EntryActionsChanged& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.stateId) {
        refreshStateTab();
    }
    refreshCodeTab();  // the Actions interface / destination entry-action calls may have changed
}

void InspectorAdapter::onExitActionsChanged(const events::ExitActionsChanged& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.stateId) {
        refreshStateTab();
    }
    refreshCodeTab();  // exit actions feed the Actions interface / exit calls
}

void InspectorAdapter::onDescriptionChanged(const events::DescriptionChanged& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.stateId) {
        refreshStateTab();
    }
}

void InspectorAdapter::onTagsChanged(const events::TagsChanged& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.stateId) {
        refreshStateTab();
    }
}

void InspectorAdapter::onHistoryDeepChanged(const events::HistoryDeepChanged& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.stateId) {
        refreshStateTab();
    }
}

// Invoke declaration facts: the value can change under the inspected state via
// undo/redo or project load, not only this panel's own edit, so refresh on the fact.
void InspectorAdapter::onInvokeSrcChanged(const events::InvokeSrcChanged& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.stateId) {
        refreshStateTab();
    }
    refreshCodeTab();  // the generated Invocations interface may have gained/lost a method
}

void InspectorAdapter::onInvokeIdChanged(const events::InvokeIdChanged& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.stateId) {
        refreshStateTab();
    }
    refreshCodeTab();
}

void InspectorAdapter::onInvokeOutputTypeChanged(const events::InvokeOutputTypeChanged& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.stateId) {
        refreshStateTab();
    }
    refreshCodeTab();  // the declared onDone payload type feeds the Invocations interface's signature
}

void InspectorAdapter::onInvocationsChanged(const events::InvocationsChanged& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.stateId) {
        refreshStateTab();
    }
    refreshCodeTab();
}

void InspectorAdapter::onStateDeleted(const events::StateDeleted& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.id) {
        fallBackToMachineTab();
    }
    refreshMachineTab();  // topology counts changed regardless
    refreshCodeTab();
}

void InspectorAdapter::onTransitionEventChanged(const events::TransitionEventChanged& fact) {
    if (selectionKind_ == SelectionKind::Transition && selectionId_ == fact.id) {
        refreshTransitionTab();
    }
    refreshMachineTab();  // event vocabulary may have changed
    refreshCodeTab();
}

void InspectorAdapter::onTransitionGuardChanged(const events::TransitionGuardChanged& fact) {
    if (selectionKind_ == SelectionKind::Transition && selectionId_ == fact.id) {
        refreshTransitionTab();
    }
    refreshMachineTab();  // guard vocabulary may have changed
    refreshCodeTab();
}

void InspectorAdapter::onTransitionActionChanged(const events::TransitionActionChanged& fact) {
    if (selectionKind_ == SelectionKind::Transition && selectionId_ == fact.id) {
        refreshTransitionTab();
    }
    refreshCodeTab();  // the Actions interface may have changed
}

void InspectorAdapter::onTransitionDelayChanged(const events::TransitionDelayChanged& fact) {
    if (selectionKind_ == SelectionKind::Transition && selectionId_ == fact.id) {
        refreshTransitionTab();
    }
    refreshCodeTab();  // may flip whether this machine has a Scheduler at all
}

void InspectorAdapter::onTransitionReenterChanged(const events::TransitionReenterChanged& fact) {
    if (selectionKind_ == SelectionKind::Transition && selectionId_ == fact.id) {
        refreshTransitionTab();
    }
    refreshCodeTab();
}

void InspectorAdapter::onTransitionAlwaysChanged(const events::TransitionAlwaysChanged& fact) {
    if (selectionKind_ == SelectionKind::Transition && selectionId_ == fact.id) {
        refreshTransitionTab();
    }
    refreshMachineTab();
    refreshCodeTab();
}

void InspectorAdapter::onTransitionRetargeted(const events::TransitionRetargeted& fact) {
    if (selectionKind_ == SelectionKind::Transition && selectionId_ == fact.id) {
        refreshTransitionTab();  // source/target display names changed
    }
    refreshCodeTab();  // the command's per-source-state branch changed
}

void InspectorAdapter::onTransitionDeleted(const events::TransitionDeleted& fact) {
    if (selectionKind_ == SelectionKind::Transition && selectionId_ == fact.id) {
        fallBackToMachineTab();
    }
    refreshMachineTab();
    refreshCodeTab();
}

void InspectorAdapter::onStateColorChanged(const events::StateColorChanged& fact) {
    if (selectionKind_ == SelectionKind::State && selectionId_ == fact.id) {
        refreshStateTab();
    }
}

void InspectorAdapter::onTransitionColorChanged(const events::TransitionColorChanged& fact) {
    if (selectionKind_ == SelectionKind::Transition && selectionId_ == fact.id) {
        refreshTransitionTab();
    }
}

void InspectorAdapter::onNoteColorChanged(const events::NoteColorChanged& fact) {
    if (selectionKind_ == SelectionKind::Note && selectionId_ == fact.id) {
        refreshNoteTab();
    }
}

void InspectorAdapter::onNoteTextChanged(const events::NoteTextChanged& fact) {
    if (selectionKind_ == SelectionKind::Note && selectionId_ == fact.id) {
        refreshNoteTab();
    }
}

void InspectorAdapter::onNoteDeleted(const events::NoteDeleted& fact) {
    if (selectionKind_ == SelectionKind::Note && selectionId_ == fact.id) {
        fallBackToMachineTab();
    }
}

void InspectorAdapter::onSnapshotPublished(const events::MachineSnapshotPublished&) {
    if (selectionKind_ == SelectionKind::State) {
        refreshStateTab();
    } else if (selectionKind_ == SelectionKind::Transition) {
        refreshTransitionTab();
    } else if (selectionKind_ == SelectionKind::Note) {
        refreshNoteTab();
    }
    refreshMachineTab();
    refreshCodeTab();  // wholesale machine replace (e.g. project load)
}

// Each of these can change running() or the live-invocation set; refreshMachineTab()
// re-derives and pushes the live set.
void InspectorAdapter::onModeChanged(const events::ModeChanged&) {
    breakpointHitStateId_ = 0;
    refreshMachineTab();
}
void InspectorAdapter::onActiveStateChanged(const events::ActiveStateChanged&) { refreshMachineTab(); }
void InspectorAdapter::onSimulationStarted(const events::SimulationStarted&) {
    breakpointHitStateId_ = 0;
    refreshMachineTab();
}
void InspectorAdapter::onSimulationPaused(const events::SimulationPaused&) { refreshMachineTab(); }
void InspectorAdapter::onSimulationReset(const events::SimulationReset&) {
    breakpointHitStateId_ = 0;
    refreshMachineTab();
}
void InspectorAdapter::onTimescaleChanged(const events::TimescaleChanged& fact) {
    panel_->setTimescale(fact.scale);
}
void InspectorAdapter::onBreakpointHit(const events::BreakpointHit& fact) {
    breakpointHitStateId_ = fact.stateId;
    refreshMachineTab();
}
void InspectorAdapter::onTimescaleSelected(double scale) {
    context().send(events::SetTimescaleRequested{.scale = scale});
}
void InspectorAdapter::onGuardResultChanged(const events::GuardResultChanged&) { refreshMachineTab(); }
// TraceAppended, not ActiveStateChanged: it lands after the new live invocation is recorded.
void InspectorAdapter::onTraceAppended(const events::TraceAppended&) { refreshLiveInvocations(); }

void InspectorAdapter::onNameEditingFinished(QString name) {
    if (selectionKind_ != SelectionKind::State || doc_ == nullptr) {
        return;
    }
    const State* state = doc_->findState(selectionId_);
    if (state == nullptr || state->name == name) {
        return;  // skip if unchanged; also avoids a spurious no-op undo step
    }
    context().send(events::RenameStateRequested{.id = selectionId_, .name = name});
    refreshStateTab();  // re-sync from the agent's actual post-send value
}

void InspectorAdapter::onKindEdited(StateKind kind) {
    if (selectionKind_ != SelectionKind::State) {
        return;
    }
    context().send(events::SetStateKindRequested{.id = selectionId_, .kind = kind});
    refreshStateTab();
}

void InspectorAdapter::onInitialToggled(bool checked) {
    if (selectionKind_ != SelectionKind::State) {
        return;
    }
    context().send(events::SetInitialStateRequested{.id = checked ? selectionId_ : 0});
    refreshStateTab();  // snap-back if the command rejected (e.g. Simulate mode)
}

void InspectorAdapter::onEntryActionsEditingFinished(QStringList actions) {
    if (selectionKind_ != SelectionKind::State) {
        return;
    }
    context().send(events::SetEntryActionsRequested{.id = selectionId_, .entryActions = actions});
    refreshStateTab();
}

void InspectorAdapter::onExitActionsEditingFinished(QStringList actions) {
    if (selectionKind_ != SelectionKind::State) {
        return;
    }
    context().send(events::SetExitActionsRequested{.id = selectionId_, .exitActions = actions});
    refreshStateTab();
}

void InspectorAdapter::onDescriptionEditingFinished(QString description) {
    if (selectionKind_ != SelectionKind::State) {
        return;
    }
    context().send(events::SetDescriptionRequested{.id = selectionId_, .description = description});
    refreshStateTab();
}

void InspectorAdapter::onTagsEditingFinished(QStringList tags) {
    if (selectionKind_ != SelectionKind::State) {
        return;
    }
    context().send(events::SetTagsRequested{.id = selectionId_, .tags = tags});
    refreshStateTab();
}

void InspectorAdapter::onHistoryDeepToggled(bool checked) {
    if (selectionKind_ != SelectionKind::State) {
        return;
    }
    context().send(events::SetHistoryDeepRequested{.stateId = selectionId_, .deep = checked});
    refreshStateTab();  // snap-back if the command rejected (target not History-kind)
}

void InspectorAdapter::onInvokeSrcEditingFinished(QString src) {
    if (selectionKind_ != SelectionKind::State) {
        return;
    }
    context().send(events::SetInvokeSrcRequested{.stateId = selectionId_, .src = src});
    refreshStateTab();
    refreshCodeTab();  // the generated Invocations interface may have gained/lost a method
}

void InspectorAdapter::onInvokeIdEditingFinished(QString invokeId) {
    if (selectionKind_ != SelectionKind::State) {
        return;
    }
    context().send(events::SetInvokeIdRequested{.stateId = selectionId_, .invokeId = invokeId});
    refreshStateTab();
    refreshCodeTab();
}

void InspectorAdapter::onInvokeOutputTypeEdited(ContextType type) {
    if (selectionKind_ != SelectionKind::State) {
        return;
    }
    context().send(events::SetInvokeOutputTypeRequested{.stateId = selectionId_, .type = type});
    refreshStateTab();
    refreshCodeTab();  // the declared onDone payload type feeds the Invocations interface's signature
}

void InspectorAdapter::onInvocationsEdited(QVector<Invocation> invocations) {
    if (selectionKind_ != SelectionKind::State) {
        return;
    }
    context().send(events::SetInvocationsRequested{.stateId = selectionId_, .invocations = invocations});
    refreshStateTab();
    refreshCodeTab();
}

// `invokeId` arrives already resolved (the entry's effectiveId). No selection
// guard is needed: CompleteInvocationCommand silently rejects a stale send
// (not running, or the id is no longer live).
void InspectorAdapter::onInvocationDoneClicked(QString invokeId, QVariant payload) {
    context().send(events::CompleteInvocationRequested{.invokeId = invokeId, .ok = true, .payload = payload});
    // Re-sync the entries now; the completion likely dropped this invocation's
    // liveness. Redundant with the synchronous ActiveStateChanged refresh, but harmless.
    refreshMachineTab();
}

void InspectorAdapter::onInvocationErrorClicked(QString invokeId, QString message) {
    context().send(events::CompleteInvocationRequested{.invokeId = invokeId, .ok = false, .payload = message});
    refreshMachineTab();
}

void InspectorAdapter::onEventEditingFinished(QString event) {
    if (selectionKind_ != SelectionKind::Transition) {
        return;
    }
    context().send(events::SetTransitionEventRequested{.id = selectionId_, .event = event});
    // The duplicate-event-per-source policy may reject the edit. send() is
    // synchronous, so re-reading here snaps the field back to the kept value.
    refreshTransitionTab();
}

void InspectorAdapter::onGuardEditingFinished(QString guard) {
    if (selectionKind_ != SelectionKind::Transition) {
        return;
    }
    context().send(events::SetTransitionGuardRequested{.id = selectionId_, .guard = guard});
    refreshTransitionTab();
    refreshMachineTab();  // guard vocabulary may have gained/lost an entry
}

void InspectorAdapter::onGuardExpandRequested() {
    if (doc_ == nullptr || selectionKind_ != SelectionKind::Transition || selectionId_ == 0) {
        return;
    }
    const Transition* transition = doc_->findTransition(selectionId_);
    if (transition == nullptr) {
        return;
    }
    const expr::PayloadBinding payload = payloadBindingForTransition(doc_->machine(), *transition);

    std::optional<QString> result;
    if (expressionDialogRunner_) {
        result = expressionDialogRunner_(
            ExpressionEditorDialog::Kind::Guard,
            transition->guard,
            doc_->machine().context,
            doc_->machine().types,
            payload,
            panel_);
    } else {
        result = ExpressionEditorDialog::editExpression(
            ExpressionEditorDialog::Kind::Guard,
            transition->guard,
            doc_->machine().context,
            doc_->machine().types,
            payload,
            panel_);
    }

    if (result.has_value()) {
        context().send(events::SetTransitionGuardRequested{.id = selectionId_, .guard = *result});
        refreshTransitionTab();
        refreshMachineTab();
    }
}

void InspectorAdapter::onActionEditingFinished(QString action) {
    if (selectionKind_ != SelectionKind::Transition) {
        return;
    }
    context().send(events::SetTransitionActionRequested{.id = selectionId_, .action = action});
    refreshTransitionTab();
}

void InspectorAdapter::onActionExpandRequested() {
    if (doc_ == nullptr || selectionKind_ != SelectionKind::Transition || selectionId_ == 0) {
        return;
    }
    const Transition* transition = doc_->findTransition(selectionId_);
    if (transition == nullptr) {
        return;
    }
    const expr::PayloadBinding payload = payloadBindingForTransition(doc_->machine(), *transition);

    std::optional<QString> result;
    if (expressionDialogRunner_) {
        result = expressionDialogRunner_(
            ExpressionEditorDialog::Kind::Action,
            transition->action,
            doc_->machine().context,
            doc_->machine().types,
            payload,
            panel_);
    } else {
        result = ExpressionEditorDialog::editExpression(
            ExpressionEditorDialog::Kind::Action,
            transition->action,
            doc_->machine().context,
            doc_->machine().types,
            payload,
            panel_);
    }

    if (result.has_value()) {
        context().send(events::SetTransitionActionRequested{.id = selectionId_, .action = *result});
        refreshTransitionTab();
    }
}

void InspectorAdapter::onDelayEdited(int delayMs) {
    if (selectionKind_ != SelectionKind::Transition) {
        return;
    }
    context().send(events::SetTransitionDelayRequested{.id = selectionId_, .delayMs = delayMs});
    refreshTransitionTab();
}

void InspectorAdapter::onReenterToggled(bool checked) {
    if (selectionKind_ != SelectionKind::Transition) {
        return;
    }
    context().send(events::SetTransitionReenterRequested{.id = selectionId_, .reenter = checked});
    refreshTransitionTab();
}

void InspectorAdapter::onAlwaysToggled(bool checked) {
    if (selectionKind_ != SelectionKind::Transition) {
        return;
    }
    context().send(events::SetTransitionAlwaysRequested{.id = selectionId_, .always = checked});
    refreshTransitionTab();
    refreshMachineTab();
}

namespace {

const State* findStateByName(const Machine& machine, const QString& name) {
    for (const State& s : machine.states) {
        if (s.name.trimmed().compare(name.trimmed(), Qt::CaseInsensitive) == 0 ||
            s.name.trimmed() == name.trimmed()) {
            return &s;
        }
    }
    return nullptr;
}

bool isAncestorState(const Machine& machine, quint64 ancestorId, quint64 descendantId) {
    if (ancestorId == 0 || descendantId == 0) {
        return false;
    }
    quint64 curr = descendantId;
    while (curr != 0) {
        const State* s = nullptr;
        for (const State& candidate : machine.states) {
            if (candidate.id == curr) {
                s = &candidate;
                break;
            }
        }
        if (!s) {
            break;
        }
        if (s->parentId == ancestorId) {
            return true;
        }
        curr = s->parentId;
    }
    return false;
}

quint64 findLccaState(const Machine& machine, quint64 s1, quint64 s2) {
    const State* state1 = nullptr;
    for (const State& s : machine.states) {
        if (s.id == s1) {
            state1 = &s;
            break;
        }
    }
    if (!state1) {
        return 0;
    }
    quint64 anc = state1->parentId;
    while (anc != 0 && !isAncestorState(machine, anc, s2)) {
        const State* ancState = nullptr;
        for (const State& s : machine.states) {
            if (s.id == anc) {
                ancState = &s;
                break;
            }
        }
        anc = ancState ? ancState->parentId : 0;
    }
    return anc;
}

// One row per invocation with a non-empty src whose effective id is in `liveIds`
// (SimulationAgent::liveInvocationIds()), in Machine::states document order.
QVector<InspectorPanel::LiveInvocationRow> deriveLiveInvocationRows(const Machine& machine,
                                                                     const QSet<QString>& liveIds) {
    QVector<InspectorPanel::LiveInvocationRow> rows;
    for (const State& state : machine.states) {
        for (const Invocation& inv : state.effectiveInvocations()) {
            if (inv.src.isEmpty()) {
                continue;
            }
            const QString effectiveId = effectiveInvocationId(inv);
            if (!liveIds.contains(effectiveId)) {
                continue;
            }
            InspectorPanel::LiveInvocationRow row;
            row.stateId = state.id;
            row.stateName = state.name;
            row.src = inv.src;
            row.effectiveId = effectiveId;
            row.outputType = inv.outputType;
            rows.push_back(row);
        }
    }
    return rows;
}

}  // namespace

void InspectorAdapter::onTargetTextChanged(QString text) {
    if (doc_ == nullptr || selectionKind_ != SelectionKind::Transition) {
        return;
    }
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        panel_->setTargetFeedback(InspectorPanel::TargetFeedback::None, QString());
        return;
    }
    const QStringList parts = trimmed.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        panel_->setTargetFeedback(InspectorPanel::TargetFeedback::None, QString());
        return;
    }

    const Machine& machine = doc_->machine();
    QList<quint64> targetIds;
    for (const QString& rawPart : parts) {
        const QString name = rawPart.trimmed();
        const State* s = findStateByName(machine, name);
        if (!s) {
            panel_->setTargetFeedback(InspectorPanel::TargetFeedback::Error,
                                     QStringLiteral("Unknown target state '%1'").arg(name));
            return;
        }
        if (targetIds.contains(s->id)) {
            panel_->setTargetFeedback(InspectorPanel::TargetFeedback::Error,
                                     QStringLiteral("Duplicate target state '%1'").arg(name));
            return;
        }
        targetIds.push_back(s->id);
    }

    if (targetIds.size() <= 1) {
        panel_->setTargetFeedback(InspectorPanel::TargetFeedback::None, QString());
        return;
    }

    for (int i = 0; i < targetIds.size(); ++i) {
        for (int j = i + 1; j < targetIds.size(); ++j) {
            const quint64 t1 = targetIds[i];
            const quint64 t2 = targetIds[j];
            if (isAncestorState(machine, t1, t2) || isAncestorState(machine, t2, t1)) {
                panel_->setTargetFeedback(InspectorPanel::TargetFeedback::Error,
                                         QStringLiteral("Ancestor-descendant conflict between '%1' and '%2'")
                                             .arg(stateName(t1), stateName(t2)));
                return;
            }
            const quint64 lcca = findLccaState(machine, t1, t2);
            const State* lccaState = doc_->findState(lcca);
            if (!lccaState || lccaState->kind != StateKind::Parallel) {
                panel_->setTargetFeedback(InspectorPanel::TargetFeedback::Error,
                                         QStringLiteral("Targets '%1' and '%2' must reside in orthogonal regions of a Parallel state")
                                             .arg(stateName(t1), stateName(t2)));
                return;
            }
        }
    }

    panel_->setTargetFeedback(InspectorPanel::TargetFeedback::Note,
                             QStringLiteral("Multiple targets: %1 orthogonal regions").arg(targetIds.size()));
}

void InspectorAdapter::onTargetEditingFinished(QString text) {
    if (selectionKind_ != SelectionKind::Transition || doc_ == nullptr) {
        return;
    }
    const Transition* transition = doc_->findTransition(selectionId_);
    if (!transition) {
        return;
    }
    const QString trimmed = text.trimmed();
    QList<quint64> targetIds;
    if (!trimmed.isEmpty()) {
        const QStringList parts = trimmed.split(QLatin1Char(','), Qt::SkipEmptyParts);
        const Machine& machine = doc_->machine();
        for (const QString& rawPart : parts) {
            const QString name = rawPart.trimmed();
            const State* s = findStateByName(machine, name);
            if (!s || targetIds.contains(s->id)) {
                refreshTransitionTab();  // Snap back on error
                return;
            }
            targetIds.push_back(s->id);
        }
    }

    if (targetIds.size() > 1) {
        const Machine& machine = doc_->machine();
        for (int i = 0; i < targetIds.size(); ++i) {
            for (int j = i + 1; j < targetIds.size(); ++j) {
                const quint64 t1 = targetIds[i];
                const quint64 t2 = targetIds[j];
                if (isAncestorState(machine, t1, t2) || isAncestorState(machine, t2, t1)) {
                    refreshTransitionTab();
                    return;
                }
                const quint64 lcca = findLccaState(machine, t1, t2);
                const State* lccaState = doc_->findState(lcca);
                if (!lccaState || lccaState->kind != StateKind::Parallel) {
                    refreshTransitionTab();
                    return;
                }
            }
        }
    }

    const quint64 primaryTo = targetIds.isEmpty() ? 0 : targetIds.first();
    context().send(events::RetargetTransitionRequested{
        .id = selectionId_,
        .from = transition->from,
        .to = primaryTo,
        .targets = targetIds,
    });
    refreshTransitionTab();
}

void InspectorAdapter::onPayloadTypeEditingFinished(QString payloadType) {
    if (doc_ == nullptr || selectionKind_ != SelectionKind::Transition || selectionId_ == 0) {
        return;
    }
    context().send(events::SetTransitionPayloadTypeRequested{.id = selectionId_, .payloadType = payloadType});
}

void InspectorAdapter::onSendEventClicked(QString name, QVariant payload) {
    context().send(events::SendEventRequested{.name = name, .payload = payload});
}

void InspectorAdapter::onTransitionPayloadTypeChanged(const events::TransitionPayloadTypeChanged& fact) {
    if (selectionKind_ == SelectionKind::Transition && selectionId_ == fact.id) {
        refreshTransitionTab();
    }
    refreshCodeTab();
}

void InspectorAdapter::onStructDefinitionAdded(const events::StructDefinitionAdded&) {
    if (selectionKind_ == SelectionKind::Transition) {
        refreshTransitionTab();
    }
    refreshCodeTab();
}

void InspectorAdapter::onStructDefinitionChanged(const events::StructDefinitionChanged&) {
    if (selectionKind_ == SelectionKind::Transition) {
        refreshTransitionTab();
    }
    refreshCodeTab();
}

void InspectorAdapter::onStructDefinitionDeleted(const events::StructDefinitionDeleted&) {
    if (selectionKind_ == SelectionKind::Transition) {
        refreshTransitionTab();
    }
    refreshCodeTab();
}

void InspectorAdapter::onGuardToggled(QString name, bool result) {
    context().send(events::SetGuardResultRequested{.name = name, .result = result});
}

void InspectorAdapter::refreshStateTab() {
    if (doc_ == nullptr) {
        return;
    }
    const State* state = doc_->findState(selectionId_);
    if (state == nullptr) {
        fallBackToMachineTab();
        return;
    }
    panel_->setStateFields(state->name, state->kind, doc_->machine().initialStateId == state->id,
                            state->entryActions, state->exitActions, state->description, state->tags,
                            state->historyDeep, state->pos, state->invokeSrc, state->invokeId,
                            state->invokeOutputType, state->effectiveInvocations(),
                            state->color);
    const NodeCodeProjection proj = projectStateCode(doc_->machine(), selectionId_);
    panel_->setStateCodeProjection(proj);
}

void InspectorAdapter::refreshTransitionTab() {
    if (doc_ == nullptr) {
        return;
    }
    const Transition* transition = doc_->findTransition(selectionId_);
    if (transition == nullptr) {
        fallBackToMachineTab();
        return;
    }
    // A root transition's source is the machine itself (from == 0); stateName(0) reads blank.
    // Set suggestions before the fields: setTransitionFields' setText emits
    // textChanged, whose guard feedback needs the completer model current.
    QStringList guardSuggestions;
    for (const LogicRow& row : inventory(doc_->machine()).guards) {
        guardSuggestions.push_back(row.name);
    }
    for (const ContextVariable& var : doc_->machine().context) {
        if (var.type == ContextType::Object) {
            QJsonParseError err;
            const QJsonDocument jsonDoc = QJsonDocument::fromJson(var.initialValue.trimmed().toUtf8(), &err);
            if (!jsonDoc.isNull() && jsonDoc.isObject()) {
                const std::function<void(const QString&, const QJsonObject&)> collectMembers =
                    [&collectMembers, &guardSuggestions](const QString& prefix, const QJsonObject& obj) {
                        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
                            const QString path = prefix + QLatin1Char('.') + it.key();
                            guardSuggestions.push_back(path);
                            if (it.value().isObject()) {
                                collectMembers(path, it.value().toObject());
                            }
                        }
                    };
                collectMembers(var.name, jsonDoc.object());
            }
        }
    }
    panel_->setGuardSuggestions(guardSuggestions);

    QStringList typeSuggestions{QStringLiteral("bool"), QStringLiteral("int"),
                                QStringLiteral("double"), QStringLiteral("string")};
    for (const StructDefinition& def : doc_->machine().types) {
        if (!def.name.isEmpty()) {
            typeSuggestions.push_back(def.name);
        }
    }
    panel_->setPayloadTypeSuggestions(typeSuggestions);

    QString targetText;
    if (transition->isMultiTarget()) {
        QStringList names;
        for (quint64 tid : transition->targets) {
            names.push_back(stateName(tid));
        }
        targetText = names.join(QStringLiteral(", "));
    } else if (transition->to != 0) {
        targetText = stateName(transition->to);
    }

    panel_->setTransitionFields(transition->event, transition->guard, transition->action, transition->delayMs,
                                 transition->from == 0 ? QStringLiteral("Machine") : stateName(transition->from),
                                 targetText,
                                 transition->reenter, transition->isAlways(), transition->payloadType,
                                 transition->color);
    onEventTextChanged(transition->event);
    onActionTextChanged(transition->action);
    onTargetTextChanged(targetText);

    const NodeCodeProjection proj = projectTransitionCode(doc_->machine(), selectionId_);
    panel_->setTransitionCodeProjection(proj);
}

void InspectorAdapter::refreshNoteTab() {
    if (doc_ == nullptr) {
        return;
    }
    const Note* note = doc_->findNote(selectionId_);
    if (note == nullptr) {
        fallBackToMachineTab();
        return;
    }
    panel_->setNoteFields(note->text, note->color);
}

void InspectorAdapter::onEventTextChanged(QString event) {
    if (doc_ == nullptr) {
        return;
    }
    const QString trimmed = event.trimmed();
    if (trimmed.isEmpty()) {
        panel_->setEventFeedback(InspectorPanel::EventFeedback::None, QString());
        return;
    }
    if (trimmed == QStringLiteral("*")) {
        panel_->setEventFeedback(InspectorPanel::EventFeedback::Note,
                                 QStringLiteral("Universal wildcard: matches any event"));
        return;
    }
    if (trimmed.contains(QLatin1Char('*'))) {
        if (trimmed.endsWith(QStringLiteral(".*"))) {
            const QString prefix = trimmed.left(trimmed.size() - 2);
            if (prefix.isEmpty()) {
                panel_->setEventFeedback(InspectorPanel::EventFeedback::Error,
                                         QStringLiteral("Invalid wildcard: prefix cannot be empty (use '*' or 'prefix.*')"));
                return;
            }
            if (prefix.contains(QLatin1Char('*'))) {
                panel_->setEventFeedback(InspectorPanel::EventFeedback::Error,
                                         QStringLiteral("Invalid wildcard: multiple asterisks are not allowed"));
                return;
            }
            panel_->setEventFeedback(InspectorPanel::EventFeedback::Note,
                                     QStringLiteral("Prefix wildcard: matches '%1' and all '%1.<subevent>'").arg(prefix));
            return;
        }
        panel_->setEventFeedback(InspectorPanel::EventFeedback::Error,
                                 QStringLiteral("Invalid wildcard: '*' must be universal ('*') or follow a dot prefix ('prefix.*')"));
        return;
    }
    panel_->setEventFeedback(InspectorPanel::EventFeedback::None, QString());
}

void InspectorAdapter::onGuardTextChanged(QString guard) {
    if (doc_ == nullptr) {
        return;
    }
    const QString trimmed = guard.trimmed();
    if (trimmed.isEmpty()) {
        panel_->setGuardFeedback(InspectorPanel::GuardFeedback::None, QString());
        return;
    }
    const bool hasContext = !doc_->machine().context.empty();
    const QString guardParamStr = hasContext ? QStringLiteral("(const Context&)") : QStringLiteral("()");

    // A bare identifier such as `locked` is always a named hook, never a context
    // variable, and never consults the schema (adding a variable must not change
    // an existing guard's meaning). The note names the escape.
    if (expr::isBareIdentifier(guard)) {
        panel_->setGuardFeedback(InspectorPanel::GuardFeedback::Note,
                                  QStringLiteral("named hook → C++: %1%2 — write %3 == true to test a context variable instead")
                                      .arg(sanitizeIdentifier(trimmed, false), guardParamStr, trimmed));
        return;
    }
    int position = 0;
    const QString error = expr::validateGuardSource(guard, doc_->machine().context, &position);
    if (!error.isEmpty()) {
        panel_->setGuardFeedback(InspectorPanel::GuardFeedback::Error,
                                  QStringLiteral("at %1: %2").arg(position).arg(error));
        return;
    }
    // A valid expression is silent, like a blank guard.
    panel_->setGuardFeedback(InspectorPanel::GuardFeedback::None, QString());
}

void InspectorAdapter::onActionTextChanged(QString action) {
    if (doc_ == nullptr) {
        return;
    }
    const QString trimmed = action.trimmed();
    if (trimmed.isEmpty()) {
        panel_->setActionFeedback(InspectorPanel::ActionFeedback::None, QString());
        return;
    }
    const bool hasContext = !doc_->machine().context.empty();
    const QString actionParamStr = hasContext ? QStringLiteral("(Context&)") : QStringLiteral("()");

    const expr::SendToForm sendTo = expr::parseSendToForm(trimmed);
    if (sendTo.ok) {
        panel_->setActionFeedback(InspectorPanel::ActionFeedback::Note,
                                  QStringLiteral("sendTo actor '%1' event '%2' → C++: sendTo_%1_%2%3")
                                      .arg(sendTo.target, sendTo.event, actionParamStr));
        return;
    }
    if (!sendTo.message.isEmpty()) {
        panel_->setActionFeedback(InspectorPanel::ActionFeedback::Error,
                                  QStringLiteral("sendTo: %1").arg(sendTo.message));
        return;
    }
    const expr::SendParentForm sendParent = expr::parseSendParentForm(trimmed);
    if (sendParent.ok) {
        panel_->setActionFeedback(InspectorPanel::ActionFeedback::Note,
                                  QStringLiteral("sendParent event '%1' → C++: sendParent_%1%2")
                                      .arg(sendParent.event, actionParamStr));
        return;
    }
    if (!sendParent.message.isEmpty()) {
        panel_->setActionFeedback(InspectorPanel::ActionFeedback::Error,
                                  QStringLiteral("sendParent: %1").arg(sendParent.message));
        return;
    }
    const expr::RaiseForm raise = expr::parseRaiseForm(trimmed);
    if (raise.ok) {
        panel_->setActionFeedback(InspectorPanel::ActionFeedback::Note,
                                  QStringLiteral("raises event '%1' (internal microstep queue)").arg(raise.event));
        return;
    }
    if (!raise.message.isEmpty()) {
        panel_->setActionFeedback(InspectorPanel::ActionFeedback::Error,
                                  QStringLiteral("raise: %1").arg(raise.message));
        return;
    }
    const expr::AssignForm assign = expr::parseAssignForm(trimmed);
    if (assign.ok) {
        panel_->setActionFeedback(InspectorPanel::ActionFeedback::Note,
                                  QStringLiteral("assignment: %1 = %2").arg(assign.target, assign.valueSource));
        return;
    }
    if (!assign.message.isEmpty()) {
        panel_->setActionFeedback(InspectorPanel::ActionFeedback::Error,
                                  QStringLiteral("assign: %1").arg(assign.message));
        return;
    }
    if (expr::isBareIdentifier(trimmed)) {
        panel_->setActionFeedback(InspectorPanel::ActionFeedback::Note,
                                  QStringLiteral("named action hook → C++: %1%2")
                                      .arg(sanitizeIdentifier(trimmed, false), actionParamStr));
        return;
    }
    panel_->setActionFeedback(InspectorPanel::ActionFeedback::None, QString());
}

void InspectorAdapter::refreshMachineTab() {
    if (doc_ == nullptr || sim_ == nullptr) {
        return;
    }
    const Machine& machine = doc_->machine();
    panel_->setMachineFields(machineName_, static_cast<int>(machine.states.size()),
                              static_cast<int>(machine.transitions.size()), sim_->mode() == events::Mode::Simulate);

    const bool simulateRunning = sim_->mode() == events::Mode::Simulate && sim_->running();
    // Same formula as StatusBadgeAdapter::computeText(): the atomic members of the
    // configuration, joined with " | " for a Parallel machine's active regions.
    QString simulationChip = QStringLiteral("idle");
    if (breakpointHitStateId_ != 0) {
        simulationChip = QString(QChar(0x25CF)) + QStringLiteral(" ") + stateName(breakpointHitStateId_) +
                         QStringLiteral(" (Breakpoint Hit)");
    } else if (simulateRunning) {
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
            if (!hasActiveChild) {
                names.push_back(stateName(id));
            }
        }
        if (!names.isEmpty()) {
            simulationChip = QString(QChar(0x25CF)) + QStringLiteral(" ") + names.join(QStringLiteral(" | "));
        }
    }
    panel_->setSimulationChip(simulationChip);

    refreshSimControls();
    rebuildVocabularies();
    refreshLiveInvocations();
}

// One entry per currently-live invocation, independent of selection. Separate
// from refreshMachineTab() so onTraceAppended() can call it on every trace line
// without rebuilding the vocabularies.
void InspectorAdapter::refreshLiveInvocations() {
    if (doc_ == nullptr || sim_ == nullptr) {
        return;
    }
    const bool simulateRunning = sim_->mode() == events::Mode::Simulate && sim_->running();
    panel_->setLiveInvocations(deriveLiveInvocationRows(doc_->machine(), sim_->liveInvocationIds()), simulateRunning);
}

void InspectorAdapter::refreshSimControls() {
    if (sim_ == nullptr) {
        return;
    }
    const bool simulate = sim_->mode() == events::Mode::Simulate;
    const bool running = sim_->running();
    // Run: start or resume (no-op while running). Pause: needs running. Reset: any
    // time in Simulate (keeps running true). Back: needs running and a fired transition.
    panel_->setSimTransportState(simulate && !running, simulate && running, simulate,
                                  simulate && running && !sim_->firedTransitionIds().empty());
}

void InspectorAdapter::refreshCodeTab() {
    if (doc_ == nullptr) {
        return;
    }
    // Pure and cheap over the current machine, so no caching. The domain/ stubs are
    // previewed too; on disk they are written once and never overwritten.
    QVector<GeneratedFile> files = generate(doc_->machine(), QString::fromLatin1(kGeneratedRootNamespace));
    files += generateDomainStubs(doc_->machine(), QString::fromLatin1(kGeneratedRootNamespace));
    panel_->setGeneratedFiles(files);
}

void InspectorAdapter::rebuildVocabularies() {
    if (doc_ == nullptr || sim_ == nullptr) {
        return;
    }
    const bool simulateRunning = sim_->mode() == events::Mode::Simulate && sim_->running();
    const quint64 activeId = sim_->activeStateId();

    QStringList eventNames;
    QVector<QPair<QString, bool>> guards;
    QStringList seenGuards;
    for (const Transition& transition : doc_->machine().transitions) {
        const QString event = transition.event.trimmed();
        if (!event.isEmpty() && !eventNames.contains(event)) {
            eventNames.push_back(event);
        }
        const QString guard = transition.guard.trimmed();
        if (!guard.isEmpty() && !seenGuards.contains(guard)) {
            seenGuards.push_back(guard);
            guards.push_back(qMakePair(guard, sim_->guardResult(guard)));
        }
    }

    // Enabled only when running and the active state has an outgoing transition
    // carrying the event. A false guard does not disable it (what-if testing).
    QVector<InspectorPanel::EventVocabularyItem> events;
    events.reserve(eventNames.size());
    const bool inSimulate = sim_->mode() == events::Mode::Simulate && !sim_->configuration().empty();
    for (const QString& name : eventNames) {
        bool fireable = false;
        QString payloadType;
        for (const Transition& transition : doc_->machine().transitions) {
            if (transition.event.trimmed() == name) {
                if (payloadType.isEmpty() && !transition.payloadType.isEmpty()) {
                    payloadType = transition.payloadType;
                }
                if (inSimulate && activeId != 0 && (transition.from == activeId || transition.from == 0)) {
                    fireable = true;
                }
            }
        }
        events.push_back(InspectorPanel::EventVocabularyItem{
            .name = name,
            .fireable = fireable,
            .payloadType = payloadType,
        });
    }

    panel_->setEventVocabulary(events);
    panel_->setGuardVocabulary(guards);
}

QString InspectorAdapter::stateName(quint64 id) const {
    if (doc_ == nullptr) {
        return QString();
    }
    const State* state = doc_->findState(id);
    return state != nullptr ? state->name : QString();
}

void InspectorAdapter::fallBackToMachineTab() {
    selectionKind_ = SelectionKind::None;
    selectionId_ = 0;
    panel_->clearCodeProjections();
    refreshMachineTab();
    panel_->showTabForSelection(SelectionKind::None);
}

}  // namespace app
