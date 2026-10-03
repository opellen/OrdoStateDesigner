#pragma once

#include <memory>

#include <QPair>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <QtGlobal>

#include <ordo/qt/presenter.h>

#include "infra/code_generator.h"  // GeneratedFile
#include "infra/node_code_projector.h"
#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/sim_agent.h"
#include "model/sim_events.h"
#include "view/canvas/canvas_presenter.h"  // SelectionKind
#include "view/shell/expression_editor_dialog.h"

class QTabWidget;
class QLineEdit;
class QCheckBox;
class QComboBox;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QStackedWidget;
class QGroupBox;
class QCompleter;
class QStringListModel;
class QVBoxLayout;
class QFormLayout;
class QEvent;
class QObject;
class QToolButton;

namespace app {

class LogicPanel;
class NodeCodePreviewWidget;
namespace inspector_detail {
class ColorPaletteWidget;
class SectionHeaderButton;
}

// The right inspector panel: tabs that always show what the focused pane's session facts/agents
// say; nothing here is locally authoritative. InspectorPanel is the dumb view (owns the field
// widgets, reports raw edits via signals); InspectorAdapter, below, is its Presenter.
// MainWindow owns one panel and rebuilds a dedicated ViewHost + adapter on every focus or
// machine rebind, since an adapter can never be rebound to a different kernel.
class InspectorPanel : public QWidget {
    Q_OBJECT

public:
    explicit InspectorPanel(QWidget* parent = nullptr);

    // ---- State tab ----------------------------------------------------------
    // `isInitial` mirrors Machine::initialStateId == this state (the "Initial state" checkbox).
    // historyDeep matters only for History kind; other kinds hide the Deep-history row.
    // Empty invokeSrc = no invocation: the Id/Output-type rows are disabled, not hidden.
    // Empty invokeId means the effective id derives from invokeSrc, shown as the Id placeholder.
    void setStateFields(const QString& name, StateKind kind, bool isInitial, const QStringList& entryActions,
                        const QStringList& exitActions, const QString& description, const QStringList& tags,
                        bool historyDeep, QPointF pos, const QString& invokeSrc, const QString& invokeId,
                        ContextType invokeOutputType,
                        const QVector<Invocation>& invocations = {},
                        ElementColor color = ElementColor::Default);
    void setStateCodeProjection(const NodeCodeProjection& projection);

    // ---- Transition tab -------------------------------------------------------
    void setTransitionFields(const QString& event, const QString& guard, const QString& action, int delayMs,
                              const QString& sourceName, const QString& targetName,
                              bool reenter = true, bool always = false, const QString& payloadType = QString(),
                              ElementColor color = ElementColor::Default);
    void setTransitionCodeProjection(const NodeCodeProjection& projection);
    void clearCodeProjections();
    void setPayloadTypeSuggestions(const QStringList& suggestions);

    // ---- Note tab -------------------------------------------------------------
    void setNoteFields(const QString& text, ElementColor color = ElementColor::Default);

    // Inline event feedback (wildcard descriptor hints / syntax errors)
    enum class EventFeedback { None, Note, Error };
    void setEventFeedback(EventFeedback severity, const QString& message);

    // Inline guard feedback under the Guard field: Error must be fixed, Note is orientation.
    // A blank message hides the row. Display only; the adapter decides what counts as an error.
    enum class GuardFeedback { None, Note, Error };
    void setGuardFeedback(GuardFeedback severity, const QString& message);
    // Inline note/error for raise(...) and assign forms under the Action field.
    enum class ActionFeedback { None, Note, Error };
    void setActionFeedback(ActionFeedback severity, const QString& message);
    enum class TargetFeedback { None, Note, Error };
    void setTargetFeedback(TargetFeedback severity, const QString& message);
    // Type-ahead over guards already used in this machine; a completer so it never blocks typing a new one.
    void setGuardSuggestions(const QStringList& guards);

    // ---- Machine tab ----------------------------------------------------------
    void setMachineFields(const QString& machineName, int stateCount, int transitionCount, bool simulateMode);
    void setSimulationChip(const QString& text);
    void setTimescale(double scale);
    void debugSetTimescale(double scale);
    double debugTimescale() const;
    // Per-button gating for the sim transport: buttons must look unpressable when the kernel would
    // ignore them. The adapter derives each flag; this class only displays them.
    void setSimTransportState(bool runEnabled, bool pauseEnabled, bool resetEnabled, bool backEnabled);
    struct EventVocabularyItem {
        QString name;
        bool fireable = false;
        QString payloadType;
    };
    void setEventVocabulary(const QVector<EventVocabularyItem>& events);
    void setEventVocabulary(const QVector<QPair<QString, bool>>& events);
    // One checkbox per (name, checked) pair; default-checked semantics are resolved by the caller.
    void setGuardVocabulary(const QVector<QPair<QString, bool>>& guards);

    // ---- Live invocation completions (one entry per currently-live invocation) ----
    // A state with a non-empty invokeSrc whose effective invoke id is currently live.
    struct LiveInvocationRow {
        quint64 stateId = 0;
        QString stateName;
        QString src;                                // State::invokeSrc
        QString effectiveId;                         // model/machine.h's effectiveInvokeId(state)
        ContextType outputType = ContextType::Int;   // State::invokeOutputType
    };
    // Records rows/running and schedules a zero-timer rebuild; entries are not rebuilt in this call.
    // A Done/Error click can shrink the live set from inside its own clicked(), and a synchronous
    // rebuild would delete that button mid-signal. Tests must let the event loop turn once first.
    void setLiveInvocations(const QVector<LiveInvocationRow>& rows, bool running);

    // ---- Probe levers: user-equivalent, never a bypass ----
    int debugInvocationEntryCount() const;
    // Entry `index`'s group-box title, "<src> #<effectiveId> - <owning state name>"; empty if out of range.
    QString debugInvocationEntryLabel(int index) const;
    // Fill entry `index`'s typed payload editor; Done reads back through the same widgets.
    void debugSetInvocationPayloadInt(int index, int value);
    void debugSetInvocationPayloadDouble(int index, double value);
    void debugSetInvocationPayloadBool(int index, bool value);
    void debugSetInvocationPayloadString(int index, const QString& text);
    // Raw widget access so a scenario can send real key events at the spin box under test.
    // nullptr if `index` is out of range or the payload type is not Int/Double respectively.
    QSpinBox* debugInvocationPayloadIntSpinBox(int index) const;
    QDoubleSpinBox* debugInvocationPayloadDoubleSpinBox(int index) const;
    void debugSetInvocationErrorMessage(int index, const QString& text);
    void debugClickInvocationDone(int index);
    void debugClickInvocationError(int index);
    // Entry `index`'s rect in this panel's coordinates, for minimal captures.
    QRect debugInvocationEntryRegion(int index) const;
    // The guidance label's text when no entry exists (empty otherwise); differs by whether a run is live.
    QString debugInvocationsGuidanceText() const;

    // ---- Machine tab section order + Events/Guard Results disclosure ----
    // The section widgets buildMachineTab() places in on-screen order (Invocations heading,
    // Events, Guard Results); they share a parent, so raw y() values compare directly.
    QWidget* debugInvocationsSectionWidget() const;
    QWidget* debugEventsSectionWidget() const;
    QWidget* debugGuardsSectionWidget() const;
    // Clicks the header via QAbstractButton::click(), never bypassing the real toggle.
    void debugClickEventsHeader();
    void debugClickGuardsHeader();
    // Whether the section body is shown (the header's isChecked()).
    bool debugEventsBodyVisible() const;
    bool debugGuardsBodyVisible() const;
    // The header text, e.g. "Events (14)" / "Guard Results (2)".
    QString debugEventsHeaderText() const;
    QString debugGuardsHeaderText() const;

    // ---- Structural probes ----
    bool debugEventsHeaderLeftAligned() const;
    // Widths of the Machine form's "Machine" label and the Simulation "Current state:" label;
    // they must agree (shared label/value axis).
    int debugMachineFormLabelWidth() const;
    int debugSimulationLabelWidth() const;

    // Primary vs secondary is read from the stylesheet: the accent hex appears only in kPrimaryButtonStyle.
    bool debugRunButtonIsPrimary() const;
    bool debugPauseButtonIsPrimary() const;
    bool debugResetButtonIsPrimary() const;
    bool debugBackButtonIsPrimary() const;

    // ---- Code tab (live code preview) ----
    // Rebuilds the file combo from `files`, keeping the selected relativePath if still present
    // (else index 0), and refreshes the preview. Selection is local UI state; no round trip to the adapter.
    void setGeneratedFiles(const QVector<GeneratedFile>& files);
    // Probe hook: switches to the Code tab and selects the first file whose relativePath ends with `fileNameSuffix`.
    void showCodeTabForFile(const QString& fileNameSuffix);

    // Probe levers, user-equivalent: click levers run the full click chain; debugSetKind() sets the
    // combo and emits stateKindEdited (activated never fires programmatically); debugCommit*()
    // fill a metadata widget and emit its editing-finished signal.
    void debugClickInitialCheckbox();
    // debugHistoryDeepRowVisible() is true only while the row is shown (History kind only).
    void debugClickHistoryDeepCheckbox();
    bool debugHistoryDeepRowVisible() const;
    // Typing goes through the real QLineEdit, so the textChanged path is the one a keystroke takes.
    void debugTypeGuard(const QString& text);
    QString debugGuardFeedbackText() const;
    bool debugGuardFeedbackIsError() const;
    void debugTypeAction(const QString& text);
    QString debugActionFeedbackText() const;
    bool debugActionFeedbackIsError() const;
    void debugTypeEvent(const QString& text);
    QString debugEventFeedbackText() const;
    bool debugEventFeedbackIsError() const;
    void debugTypeTarget(const QString& text);
    QString debugTargetFeedbackText() const;
    bool debugTargetFeedbackIsError() const;
    void debugCommitTarget(const QString& text);
    QString debugTargetText() const;
    QStringList debugGuardSuggestions() const;
    void debugClickTransitionReenterCheckbox();
    void debugClickTransitionAlwaysCheckbox();
    bool debugTransitionReenterChecked() const;
    bool debugTransitionAlwaysChecked() const;
    void debugClickGuardExpand();
    void debugClickActionExpand();
    void debugTypePayloadType(const QString& text);
    QString debugPayloadTypeText() const;
    void debugSetEventPayload(const QString& name, const QString& payloadText);
    void debugClickSendEvent(const QString& name);
    QString debugEventPayloadText(const QString& name) const;
    void debugSetKind(StateKind kind);
    void debugCommitDescription(const QString& text);
    void debugCommitTags(const QString& commaSeparated);
    void debugCommitExitActions(const QString& text);
    // Invoke declaration levers commit on Enter only (both fields cascade onto done.invoke./
    // error.platform. transitions), so they emit what returnPressed would, not a focus-out.
    void debugCommitInvokeSrc(const QString& text);
    void debugCommitInvokeId(const QString& text);
    void debugSetInvokeOutputType(ContextType type);
    // Whether the Id / Output-type rows are enabled (only while Service is non-empty).
    bool debugInvokeIdEnabled() const;
    bool debugInvokeOutputTypeEnabled() const;
    // The Id field's placeholder: the effective id derived while the field is empty.
    QString debugInvokeIdPlaceholder() const;
    // Union rect of the three Invoke rows in this panel's coordinates, for minimal captures.
    QRect debugInvokeFieldsRegion() const;
    // The Code tab combo's current relativePath.
    QString debugCurrentCodeFile() const;

    // Multi-invocation levers. The card Commit/SetOutputType levers drive the card's own
    // returnPressed()/activated() emission for index > 0, so the card's signal is the one on the
    // stack (see rebuildInvocationCardsNow()).
    void debugClickAddInvocation();
    void debugRemoveInvocation(int index);
    int debugInvocationCardCount() const;
    void debugCommitInvocationCardSrc(int index, const QString& src);
    void debugCommitInvocationCardId(int index, const QString& id);
    void debugSetInvocationCardOutputType(int index, ContextType type);
    // Raw widget access for driving card `index`'s Output-type combo with real events;
    // nullptr for index 0 (the permanent combo) or out of range.
    QComboBox* debugInvocationCardOutputTypeCombo(int index) const;

    // Color levers
    void debugSetStateColor(ElementColor color);
    void debugSetTransitionColor(ElementColor color);
    void debugSetNoteColor(ElementColor color);
    ElementColor debugStateColor() const;
    ElementColor debugTransitionColor() const;
    ElementColor debugNoteColor() const;
    void debugCommitNoteText(const QString& text);
    QString debugNoteText() const;
    NodeCodePreviewWidget* debugStateCodePreview() const { return stateCodePreview_; }
    NodeCodePreviewWidget* debugTransitionCodePreview() const { return transitionCodePreview_; }

    // Shows the "No machine open" page; MainWindow calls it when no session is focused. Any adapter
    // bind ends in showTabForSelection(), which leaves this page on its own.
    void clearMachine();
    // True while the empty-state page is showing.
    bool debugIsShowingEmptyState() const;

    // Switches the visible tab to match `kind` and enables only the tab(s)
    // that make sense for it -- State and Transition are each enabled only
    // while that kind is selected; Machine and Code are always enabled.
    void showTabForSelection(SelectionKind kind);

    // Context / Logic inventory panel embedded in MachineInspectorPage
    LogicPanel* logicPanel() const { return logicPanel_; }

    // Puts keyboard focus into the named field; the caller has already selected the state/transition.
    void focusField(InspectorField field);

signals:
    // ---- State tab ----------------------------------------------------------
    void stateNameEditingFinished(QString name);
    void stateKindEdited(StateKind kind);
    void stateColorEdited(ElementColor color);
    // Checked = make this state the machine's initial; unchecked = clear
    // initialStateId (the validator then reports "no initial state").
    void stateInitialToggled(bool checked);
    // History kind only.
    void historyDeepToggled(bool checked);
    // Entry actions display one per line; input accepts newline or comma separators, trimmed,
    // empties dropped (splitEntryActions()/joinEntryActions()).
    void entryActionsEditingFinished(QStringList actions);
    // Exit actions share that convention; tags are comma-separated on a single line.
    void exitActionsEditingFinished(QStringList actions);
    void descriptionEditingFinished(QString description);
    void tagsEditingFinished(QStringList tags);
    // Service/Id fire on Enter only (a focus-out commit would cascade onto done.invoke. transitions);
    // Output type fires on a combo pick.
    void invokeSrcEditingFinished(QString src);
    void invokeIdEditingFinished(QString invokeId);
    void invokeOutputTypeEdited(ContextType type);
    void invocationsEdited(QVector<Invocation> invocations);

    // ---- Transition tab -------------------------------------------------------
    void transitionEventEditingFinished(QString event);
    void transitionEventTextChanged(QString event);
    void transitionGuardEditingFinished(QString guard);
    void transitionColorEdited(ElementColor color);
    // Per keystroke, unlike every other signal here, for live validation. Never sends an intent;
    // only transitionGuardEditingFinished commits.
    void transitionGuardTextChanged(QString guard);
    void transitionGuardExpandRequested();
    void transitionActionEditingFinished(QString action);
    void transitionActionTextChanged(QString action);
    void transitionActionExpandRequested();
    void transitionDelayEdited(int delayMs);
    void transitionReenterToggled(bool checked);
    void transitionAlwaysToggled(bool checked);
    void transitionTargetEditingFinished(QString targetText);
    void transitionTargetTextChanged(QString targetText);
    void transitionPayloadTypeEditingFinished(QString payloadType);

    // ---- Note tab -------------------------------------------------------------
    void noteTextEditingFinished(QString text);
    void noteColorEdited(ElementColor color);

    // ---- Machine tab / Simulation section --------------------------------------
    void runClicked();
    void pauseClicked();
    void resetClicked();
    void backClicked();
    void timescaleChanged(double scale);
    void sendEventClicked(QString name, QVariant payload = QVariant());
    void guardToggled(QString name, bool checked);
    void invocationDoneClicked(QString invokeId, QVariant payload);
    void invocationErrorClicked(QString invokeId, QString message);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* buildStateTab();
    QWidget* buildTransitionTab();
    QWidget* buildNoteTab();
    QWidget* buildMachineTab();
    QWidget* buildCodeTab();
    // The "No machine open" page, clearMachine()'s target; top-left aligned.
    QWidget* buildEmptyTab();
    void updateCodePreview();
    static void clearLayoutWidgets(QVBoxLayout* layout);

    // One live invocation's widgets. `container` is the group box (deleting it deletes every child);
    // effectiveId/outputType are captured at build time and closed over by the Done/Error handlers.
    struct InvocationEntryWidgets {
        QString effectiveId;
        QWidget* container = nullptr;
        QStackedWidget* payloadStack = nullptr;
        QCheckBox* payloadBoolCheck = nullptr;
        QSpinBox* payloadIntSpin = nullptr;
        QDoubleSpinBox* payloadDoubleSpin = nullptr;
        QLineEdit* payloadStringEdit = nullptr;
        ContextType outputType = ContextType::Int;
        QPushButton* doneButton = nullptr;
        QLineEdit* errorMessageEdit = nullptr;
        QPushButton* errorButton = nullptr;
    };
    // Reads the entry's payload widget, chosen by outputType.
    static QVariant entryPayload(const InvocationEntryWidgets& widgets);

    // ---- Live invocation completions: deferred rebuild (see setLiveInvocations()) ----
    void scheduleInvocationFlush();
    void flushPendingInvocationRebuild();
    // Wholesale rebuild: one group box per row, or the guidance label when `rows` is empty.
    // Only called from flushPendingInvocationRebuild().
    void rebuildInvocationEntriesNow(const QVector<LiveInvocationRow>& rows, bool running);

    QStackedWidget* stackedWidget_ = nullptr;
    LogicPanel* logicPanel_ = nullptr;

    // State tab
    QLineEdit* stateNameEdit_ = nullptr;
    QComboBox* stateKindCombo_ = nullptr;
    inspector_detail::ColorPaletteWidget* stateColorRow_ = nullptr;
    QCheckBox* stateInitialCheck_ = nullptr;
    // Kept so setStateFields() can call setRowVisible() on the Deep-history row.
    QFormLayout* stateForm_ = nullptr;
    QCheckBox* historyDeepCheck_ = nullptr;
    QPlainTextEdit* entryActionsEdit_ = nullptr;
    QPlainTextEdit* exitActionsEdit_ = nullptr;
    QPlainTextEdit* descriptionEdit_ = nullptr;
    QLineEdit* tagsEdit_ = nullptr;
    // invokeSrcCurrent_/invokeIdCurrent_ cache the last values setStateFields() pushed; Escape
    // (eventFilter()) reverts to these, not to empty.
    QLineEdit* invokeSrcEdit_ = nullptr;
    QLineEdit* invokeIdEdit_ = nullptr;
    QComboBox* invokeOutputTypeCombo_ = nullptr;
    QString invokeSrcCurrent_;
    QString invokeIdCurrent_;
    QLabel* statePositionValue_ = nullptr;
    NodeCodePreviewWidget* stateCodePreview_ = nullptr;

    // Multi-invoke cards
    struct InvocationCardWidgets {
        QWidget* card = nullptr;
        QLineEdit* srcEdit = nullptr;
        QLineEdit* idEdit = nullptr;
        QComboBox* outputTypeCombo = nullptr;
        QPushButton* removeBtn = nullptr;
    };
    QWidget* invocationsContainer_ = nullptr;
    QVBoxLayout* invocationsCardsLayout_ = nullptr;
    QPushButton* addInvocationBtn_ = nullptr;
    QVector<Invocation> currentInvocations_;
    QVector<InvocationCardWidgets> invocationCards_;
    bool invocationCardsFlushScheduled_ = false;
    bool invocationCardsDirty_ = false;
    void onAddInvocationClicked();
    // ---- Deferred card rebuild ----
    // rebuildInvocationCards() only records that a rebuild is owed: the signal that reached it can
    // be a card's own combo/edit/button, so a synchronous rebuild would delete that widget mid-emission.
    void rebuildInvocationCards();
    void scheduleInvocationCardsFlush();
    void flushPendingInvocationCardsRebuild();
    void rebuildInvocationCardsNow();

    // Transition tab
    QLineEdit* transitionEventEdit_ = nullptr;
    QLabel* eventFeedbackLabel_ = nullptr;
    QLineEdit* transitionGuardEdit_ = nullptr;
    QToolButton* transitionGuardExpandBtn_ = nullptr;
    inspector_detail::ColorPaletteWidget* transitionColorRow_ = nullptr;
    QLabel* guardFeedbackLabel_ = nullptr;
    QCompleter* guardCompleter_ = nullptr;
    QStringListModel* guardSuggestionModel_ = nullptr;
    QLineEdit* transitionActionEdit_ = nullptr;
    QToolButton* transitionActionExpandBtn_ = nullptr;
    QLabel* actionFeedbackLabel_ = nullptr;
    QSpinBox* transitionDelaySpin_ = nullptr;
    QCheckBox* transitionReenterCheck_ = nullptr;
    QCheckBox* transitionAlwaysCheck_ = nullptr;
    QLabel* transitionSourceValue_ = nullptr;
    QLineEdit* transitionTargetEdit_ = nullptr;
    QLabel* targetFeedbackLabel_ = nullptr;
    QLineEdit* transitionPayloadTypeEdit_ = nullptr;
    QCompleter* payloadTypeCompleter_ = nullptr;
    QStringListModel* payloadTypeSuggestionModel_ = nullptr;
    NodeCodePreviewWidget* transitionCodePreview_ = nullptr;

    // Note tab
    QPlainTextEdit* noteTextEdit_ = nullptr;
    inspector_detail::ColorPaletteWidget* noteColorRow_ = nullptr;

    // Machine tab
    QLabel* machineNameValue_ = nullptr;
    // The "Machine" row's label, debugMachineFormLabelWidth()'s anchor.
    QLabel* machineNameLabel_ = nullptr;
    QLabel* machineCountsValue_ = nullptr;
    QLabel* machineModeValue_ = nullptr;
    // Simulation's "Current state:" label, debugSimulationLabelWidth()'s anchor (not a QFormLayout row).
    QLabel* simCurrentStateLabel_ = nullptr;
    QLabel* simulationChip_ = nullptr;
    QComboBox* timescaleCombo_ = nullptr;
    QPushButton* runButton_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* backButton_ = nullptr;
    // Events / Guard Results: collapsible sections from buildCollapsibleSection(). Only the header
    // and container are kept; body visibility is owned by the header's toggled() binding.
    QToolButton* eventsSectionHeader_ = nullptr;
    QWidget* eventsSectionContainer_ = nullptr;
    QVBoxLayout* eventVocabularyLayout_ = nullptr;
    QMap<QString, QLineEdit*> eventPayloadEdits_;
    QToolButton* guardsSectionHeader_ = nullptr;
    QWidget* guardsSectionContainer_ = nullptr;
    QVBoxLayout* guardVocabularyLayout_ = nullptr;
    // invocationEntriesLayout_ holds one group box per LiveInvocationRow; invocationsGuidance_ shows instead when there are none.
    // The pending*/*Dirty_ fields record what setLiveInvocations() last received; flushScheduled_ prevents a second zero-timer.
    // invocationsHeading_ collapses by toggling entries/guidance visibility in place (applyInvocationsVisibility()),
    // and is the section-order probe's anchor.
    inspector_detail::SectionHeaderButton* invocationsHeading_ = nullptr;
    // Entries + guidance as a layout (no extra widget depth). Its margins are zeroed while collapsed:
    // an all-hidden nested box layout still reports its margins.
    QVBoxLayout* invocationsBodyLayout_ = nullptr;
    QVBoxLayout* invocationEntriesLayout_ = nullptr;
    QLabel* invocationsGuidance_ = nullptr;
    // Whether the guidance label would show (empty live set), independent of collapse; the two compose
    // in applyInvocationsVisibility().
    bool invocationsGuidanceApplicable_ = false;
    void applyInvocationsVisibility();
    QVector<InvocationEntryWidgets> invocationEntries_;
    bool invocationFlushScheduled_ = false;
    bool invocationEntriesDirty_ = false;
    QVector<LiveInvocationRow> pendingLiveInvocations_;
    bool pendingLiveInvocationsRunning_ = false;

    // Code tab
    QComboBox* codeFileCombo_ = nullptr;
    QPlainTextEdit* codePreviewEdit_ = nullptr;
    QVector<GeneratedFile> generatedFiles_;  // mirrors whatever setGeneratedFiles() last received
};

// Presenter over InspectorPanel; one instance per focused-session binding (MainWindow destroys the
// previous ViewHost + adapter and builds a fresh pair on each focus or machine change).
class InspectorAdapter : public ordo::qt::Presenter {
    Q_OBJECT

public:
    // Does not take ownership of `panel`; MainWindow keeps it alive past any one binding.
    // `machineName` is the session's display name, captured once since a session switch rebuilds the adapter.
    InspectorAdapter(InspectorPanel* panel, QString machineName);

    void onRegister() override;

    // Pushed by MainWindow on attach and on every canvas selection change (selection is a Qt signal, not a kernel fact).
    void setSelection(SelectionKind kind, quint64 id);

    using ExpressionDialogRunner = std::function<std::optional<QString>(
        ExpressionEditorDialog::Kind,
        const QString&,
        const QVector<ContextVariable>&,
        const QVector<StructDefinition>&,
        const expr::PayloadBinding&,
        QWidget*)>;
    void setExpressionDialogRunner(ExpressionDialogRunner runner) { expressionDialogRunner_ = std::move(runner); }

private:
    void onStateAdded(const events::StateAdded&);
    void onStateRenamed(const events::StateRenamed&);
    void onStateKindChanged(const events::StateKindChanged&);
    void onInitialStateChanged(const events::InitialStateChanged&);
    void onStateMoved(const events::StateMoved&);
    void onEntryActionsChanged(const events::EntryActionsChanged&);
    void onExitActionsChanged(const events::ExitActionsChanged&);
    void onDescriptionChanged(const events::DescriptionChanged&);
    void onTagsChanged(const events::TagsChanged&);
    void onHistoryDeepChanged(const events::HistoryDeepChanged&);
    // Invoke declaration facts: re-sync the fields on any change (undo/redo, snapshot load), not only via the edit path.
    void onInvokeSrcChanged(const events::InvokeSrcChanged&);
    void onInvokeIdChanged(const events::InvokeIdChanged&);
    void onInvokeOutputTypeChanged(const events::InvokeOutputTypeChanged&);
    void onInvocationsChanged(const events::InvocationsChanged&);
    void onStateDeleted(const events::StateDeleted&);
    void onTransitionAdded(const events::TransitionAdded&);
    void onTransitionEventChanged(const events::TransitionEventChanged&);
    void onTransitionGuardChanged(const events::TransitionGuardChanged&);
    void onTransitionActionChanged(const events::TransitionActionChanged&);
    void onTransitionDelayChanged(const events::TransitionDelayChanged&);
    void onTransitionReenterChanged(const events::TransitionReenterChanged&);
    void onTransitionAlwaysChanged(const events::TransitionAlwaysChanged&);
    void onTransitionRetargeted(const events::TransitionRetargeted&);
    void onTransitionDeleted(const events::TransitionDeleted&);
    void onSnapshotPublished(const events::MachineSnapshotPublished&);
    void onModeChanged(const events::ModeChanged&);
    void onActiveStateChanged(const events::ActiveStateChanged&);
    void onSimulationStarted(const events::SimulationStarted&);
    void onSimulationPaused(const events::SimulationPaused&);
    void onSimulationReset(const events::SimulationReset&);
    void onTimescaleChanged(const events::TimescaleChanged&);
    void onBreakpointHit(const events::BreakpointHit&);
    void onTimescaleSelected(double scale);
    void onGuardResultChanged(const events::GuardResultChanged&);
    // TraceAppended is the first fact that reflects a just-armed invocation: ActiveStateChanged fires
    // before armStateInvocation(), so a refresh keyed to it alone reads the live set too early.
    // Calls refreshLiveInvocations(), never refreshMachineTab(): TraceAppended is high-frequency and
    // refreshMachineTab() synchronously rebuilds the vocabulary buttons, which could delete a button mid-click.
    void onTraceAppended(const events::TraceAppended&);

    void onNameEditingFinished(QString name);
    void onKindEdited(StateKind kind);
    void onInitialToggled(bool checked);
    void onEntryActionsEditingFinished(QStringList actions);
    void onExitActionsEditingFinished(QStringList actions);
    void onDescriptionEditingFinished(QString description);
    void onTagsEditingFinished(QStringList tags);
    void onHistoryDeepToggled(bool checked);
    // Invoke declaration edit handlers (Enter-only commit; see the panel's signals).
    void onInvokeSrcEditingFinished(QString src);
    void onInvokeIdEditingFinished(QString invokeId);
    void onInvokeOutputTypeEdited(ContextType type);
    void onInvocationsEdited(QVector<Invocation> invocations);
    // Completion click handlers; `invokeId` arrives already resolved (the entry's captured effectiveId).
    void onInvocationDoneClicked(QString invokeId, QVariant payload);
    void onInvocationErrorClicked(QString invokeId, QString message);
    void onEventEditingFinished(QString event);
    void onEventTextChanged(QString event);
    void onGuardEditingFinished(QString guard);
    // Resolves one guard string's feedback through infra/expression.h and pushes it to the panel. Sends nothing.
    void onGuardTextChanged(QString guard);
    void onGuardExpandRequested();
    void onActionEditingFinished(QString action);
    void onActionTextChanged(QString action);
    void onActionExpandRequested();
    void onDelayEdited(int delayMs);
    void onReenterToggled(bool checked);
    void onAlwaysToggled(bool checked);
    void onTargetEditingFinished(QString targetText);
    void onTargetTextChanged(QString targetText);
    void onPayloadTypeEditingFinished(QString payloadType);
    void onSendEventClicked(QString name, QVariant payload);
    void onGuardToggled(QString name, bool result);
    void onTransitionPayloadTypeChanged(const events::TransitionPayloadTypeChanged&);
    void onStructDefinitionAdded(const events::StructDefinitionAdded&);
    void onStructDefinitionChanged(const events::StructDefinitionChanged&);
    void onStructDefinitionDeleted(const events::StructDefinitionDeleted&);
    void onStateColorChanged(const events::StateColorChanged&);
    void onTransitionColorChanged(const events::TransitionColorChanged&);
    void onNoteColorChanged(const events::NoteColorChanged&);
    void onNoteTextChanged(const events::NoteTextChanged&);
    void onNoteDeleted(const events::NoteDeleted&);

    void refreshStateTab();
    void refreshTransitionTab();
    void refreshNoteTab();
    void refreshMachineTab();
    // Regenerates the whole machine's files into the Code tab; called on attach and from every
    // handler that can change generated output (not onStateMoved: geometry is never emitted).
    void refreshCodeTab();
    // Derives the transport buttons' enabled flags (Run no-ops while running, Pause needs running,
    // Back needs running + a fired history, everything needs Simulate mode).
    void refreshSimControls();
    // Pushes only the live-invocation entries; kept apart from refreshMachineTab() (see
    // onTraceAppended()), which also calls it.
    void refreshLiveInvocations();
    void rebuildVocabularies();
    QString stateName(quint64 id) const;
    // Falls back to the Machine tab when the currently-inspected state/
    // transition id no longer resolves (deleted out from under this
    // adapter, or a project-load snapshot that dropped it).
    void fallBackToMachineTab();

    InspectorPanel* panel_ = nullptr;
    QString machineName_;
    std::shared_ptr<MachineDocAgent> doc_;
    std::shared_ptr<SimulationAgent> sim_;

    SelectionKind selectionKind_ = SelectionKind::None;
    quint64 selectionId_ = 0;
    quint64 breakpointHitStateId_ = 0;
    bool programmaticTextChange_ = false;
    ExpressionDialogRunner expressionDialogRunner_;
};

}  // namespace app
