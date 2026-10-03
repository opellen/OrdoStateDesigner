#pragma once

#include <memory>
#include <vector>

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QWidget>
#include <QtGlobal>

#include <ordo/qt/presenter.h>

#include "infra/logic_inventory.h"  // LogicRow/LogicRowClass/LogicInventory
#include "model/machine.h"         // ContextType, ContextVariable
#include "model/machine_doc.h"     // MachineDocAgent (LogicAdapter's doc_ member)
#include "model/machine_events.h"  // events::ContextVariable* facts this header's method signatures name
#include "model/sim_agent.h"      // SimulationAgent: contextValues()/lastGuardEvaluations()
#include "model/sim_events.h"    // run-lifecycle facts the live overlay refreshes on
#include "view/shell/inspector_panel_internal.h"  // CollapsibleSection (the Section alias)

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace app {

// Logic side-bar sections (Context, Guards, Actions, Actors, Types) for the
// focused pane's bound machine. Render-only: values arrive via the setters
// (from LogicAdapter, or MainWindow when nothing is focused); user edits
// leave as Qt signals and only LogicAdapter turns them into kernel intents.

// One Actors row: a state's invoke declaration. Not a LogicRow, since an
// invoke has no guard/action string to classify. `effectiveId` is captured
// once so readers (row label, live overlay) do not re-derive it.
struct ActorRow {
    quint64 stateId = 0;
    QString stateName;
    QString src;                                  // State::invokeSrc -- the service name
    QString effectiveId;                          // model/machine.h's effectiveInvokeId(state)
    ContextType outputType = ContextType::Int;    // State::invokeOutputType
    bool operator==(const ActorRow&) const = default;
};

class LogicPanel : public QWidget {
    Q_OBJECT

public:
    explicit LogicPanel(QWidget* parent = nullptr);

    // Records the focused pane's bound machine name (not painted; used for
    // the Hook-badge tooltip and debugHeaderText()).
    void setMachineName(const QString& machineName);
    // Clears the machine name; MainWindow calls it directly when the focused
    // pane has no bound session.
    void clearMachine();

    // ---- Context section ---------------------------------------------------
    // Replaces the Context table with `variables`, in the caller's order
    // (Machine::context document order). Fact-driven only: call it from fact
    // handlers, never from an edit handler, or a rejected command would show
    // a lie. The rows are drawn later (see scheduleFlush()), so callers that
    // assert on them must let the event loop turn once.
    void setContextVariables(const QVector<ContextVariable>& variables);
    void setContextVariablesNow(const QVector<ContextVariable>& variables);

    // ---- Types section -----------------------------------------------------
    void setTypes(const QVector<StructDefinition>& structs, const QStringList& externalHeaders);
    void setTypesNow(const QVector<StructDefinition>& structs, const QStringList& externalHeaders);

    void debugClickTypesAdd();
    int debugTypesRowCount() const;
    QString debugTypesRowName(int index) const;
    void debugClickTypesDelete(int index);
    void debugClickTypesEdit(int index);
    void debugCommitExternalHeaders(const QString& commaSeparated);
    QString debugExternalHeaders() const;
    bool debugTypesPlaceholderVisible() const;

    // ---- probe levers ------------------------------------------------------
    // The banner text ("Logic -- <name>") computed from machineName_.
    QString debugHeaderText() const;
    // One title per section, Context/Guards/Actions/Actors order.
    QStringList debugSectionTitles() const;
    // Clicks section `index`'s header button; a no-op on the disabled Actors
    // section (index 3), like a real click.
    void debugClickSectionHeader(int index);
    // Whether section `index`'s body is currently shown.
    bool debugSectionBodyVisible(int index) const;
    // The single widget spanning section `index`'s header row and body.
    QWidget* debugSectionWidget(int index) const;

    // Clicks the "+" button on the Context section's header row.
    void debugClickContextAdd();
    int debugContextRowCount() const;
    // Row `index` as displayed; ContextVariable{} (id == 0) if out of range.
    ContextVariable debugContextRow(int index) const;
    // Sets row `index`'s name text, then emits contextNameEditingFinished
    // directly (a headless probe cannot force a real focus-out).
    void debugCommitContextName(int index, const QString& name);
    // Sets row `index`'s type combo and emits contextTypeActivated directly
    // (activated() never fires programmatically).
    void debugSetContextType(int index, ContextType type);
    void debugSetContextCustomTypeName(int index, const QString& customTypeName);
    // Commits row `index`'s initial-value field, like debugCommitContextName().
    void debugCommitContextInitialValue(int index, const QString& initialValue);
    void debugClickContextDelete(int index);
    // Cross-highlighting trace button:
    void setHighlightedVariable(const QString& varName);
    void debugClickContextTrace(int index);
    bool debugContextTraceActive(int index) const;
    // True whenever the Context table has zero rows.
    bool debugContextPlaceholderVisible() const;

    // ---- Guards/Actions sections -------------------------------------------
    // Replace the Guards (or Actions) row list with the inventory's rows, in
    // its first-appearance order (never re-sorted). Each row shows truncated
    // source text, a class badge (Hook/Expr/Assign) and its reference count
    // (transitionIds.size() + stateIds.size()).
    void setGuardRows(const QVector<LogicRow>& rows);
    void setActionRows(const QVector<LogicRow>& rows);
    void setGuardRowsNow(const QVector<LogicRow>& rows);
    void setActionRowsNow(const QVector<LogicRow>& rows);

    // ---- Actors section ----------------------------------------------------
    // Replaces the Actors row list; deferred like every other setter here.
    void setActorRows(const QVector<ActorRow>& rows);
    void setActorRowsNow(const QVector<ActorRow>& rows);

    // ---- Live run overlay --------------------------------------------------
    // Overlays on the existing rows, shown only while a run is live and
    // cleared when it is not; the authoring rows never depend on the simulator.
    //
    // One value per context variable, keyed by name; a name missing from the
    // map shows nothing rather than a stale value.
    void setLiveContextValues(const QVariantMap& values, bool running);
    // Last macrostep's evaluations, keyed by trimmed guard source (how the
    // Guards rows are keyed). `decided == false` renders "?", not false.
    struct LiveGuardResult {
        bool decided = true;
        bool result = false;
    };
    void setLiveGuardResults(const QHash<QString, LiveGuardResult>& results, bool running);
    // Currently live invocations, keyed by effective invoke id
    // (ActorRow::effectiveId); membership is the whole answer.
    void setLiveActorStates(const QSet<QString>& liveInvokeIds, bool running);

    // ---- probe levers ------------------------------------------------------
    int debugGuardRowCount() const;
    // Row `index` as last handed to setGuardRows(); LogicRow{} if out of range.
    LogicRow debugGuardRow(int index) const;
    bool debugGuardPlaceholderVisible() const;
    // Clicks row `index`'s button, firing transitionRowActivated()/
    // stateRowActivated(); a no-op if out of range.
    void debugClickGuardRow(int index);

    int debugActionRowCount() const;
    LogicRow debugActionRow(int index) const;
    bool debugActionPlaceholderVisible() const;
    void debugClickActionRow(int index);

    int debugActorRowCount() const;
    // ActorRow{} (id 0) if out of range.
    ActorRow debugActorRow(int index) const;
    bool debugActorPlaceholderVisible() const;
    // Fires stateRowActivated(row.stateId).
    void debugClickActorRow(int index);
    // The overlay's displayed text ("live", or blank when hidden).
    QString debugLiveActorState(int index) const;

    // The real QLineEdit behind a Context row's name field, for probes that
    // drive commit through focus-out instead of the direct-signal levers.
    QLineEdit* debugContextNameEdit(int index) const;

    // The live overlay's displayed text, blank when the label is hidden.
    QString debugLiveContextValue(int index) const;
    QString debugLiveGuardResult(int index) const;

    // Open the in-place rename editor as the context menu's Rename entry does.
    bool debugBeginGuardRowRename(int index);
    bool debugBeginActionRowRename(int index);
    QLineEdit* debugRenameEditor() const { return renameEditor_; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    // ---- Context section ---------------------------------------------------
    // The "+" button on the Context section header.
    void contextAddClicked();
    // Fires on QLineEdit::editingFinished, not per keystroke, to keep one
    // undo entry per edit.
    void contextNameEditingFinished(quint64 id, QString name);
    // The user clicked a row's highlight button to trace the transitions
    // referencing this context variable.
    void contextVariableHighlightRequested(QString varName);
    // ---- Hook rename -------------------------------------------------------
    // Keyed by text, since derived Guards/Actions rows have no id. Emitted
    // only on Enter in the in-place editor, never on focus-out: the rename
    // rewrites every element referencing the hook.
    void guardRenameRequested(QString before, QString after);
    void actionRenameRequested(QString before, QString after);
    // Fires on QComboBox::activated: user interaction only, never this
    // class's own programmatic setCurrentIndex() calls.
    void contextTypeActivated(quint64 id, ContextType type);
    void contextCustomTypeNameActivated(quint64 id, QString customTypeName);
    void contextInitialValueEditingFinished(quint64 id, QString initialValue);
    // Fires on the per-row delete QToolButton's clicked().
    void contextDeleteClicked(quint64 id);

    // ---- Types section -----------------------------------------------------
    void structAddClicked();
    void structDeleteClicked(quint64 id);
    void structChanged(StructDefinition def);
    void externalHeadersEditingFinished(QStringList headers);

    // ---- Guards/Actions sections -------------------------------------------
    // Click-to-reveal a row's first reference (ids only; the panel shows one
    // machine): transitionIds.first() when non-empty, else stateIds.first().
    void transitionRowActivated(quint64 transitionId);
    void stateRowActivated(quint64 stateId);

private:
    using Section = inspector_detail::CollapsibleSection;

    // One Context row's widgets plus the ContextVariable::id they were built
    // for; rows are rebuilt wholesale on every update.
    struct ContextRowWidgets {
        quint64 id = 0;
        QString name;                    // key into the live-value map
        QLineEdit* nameEdit = nullptr;
        QComboBox* typeCombo = nullptr;
        QLineEdit* valueEdit = nullptr;
        QLabel* liveValueLabel = nullptr;  // hidden outside a run
        QToolButton* traceButton = nullptr;
        QToolButton* deleteButton = nullptr;
    };

    // One Guards/Actions row: its LogicRow plus the button that displays it
    // and fires the row-activated signals on click.
    struct LogicListRow {
        LogicRow row;
        QLabel* liveResultLabel = nullptr;  // hidden outside a run
        QToolButton* button = nullptr;
    };

    // Actors counterpart of LogicListRow. No rename affordance: an invoke id
    // is renamed in the Inspector.
    struct ActorListRow {
        ActorRow row;
        QLabel* liveLabel = nullptr;  // hidden outside a run, or while not live
        QToolButton* button = nullptr;
    };

    // In-place hook rename: swaps the row button for a QLineEdit in the same
    // layout slot. Enter commits (emits *RenameRequested), Esc aborts via
    // eventFilter(); finishRowRename tears the editor down on either path.
    void beginRowRename(QToolButton* rowButton, const QString& currentName, bool isGuardRow);
    void finishRowRename();
    bool eventFilterRenameEscape(QKeyEvent* keyEvent);
    // Re-applies the remembered run overlay to the current rows; also called
    // after each wholesale rebuild, which discards the labels.
    void applyLiveOverlay();

    // Row rebuilds are deferred to a zero-timer, never run inside the call
    // that asked for one: a commit arrives from the committing widget's own
    // focusOutEvent, and destroying it there crashes. Also coalesces the
    // facts of one macrostep into a single rebuild.
    void scheduleFlush();
    void flushPendingRebuilds();

    Section buildContextSection();
    Section buildTypesSection();
    // Shared Guards/Actions builder: a scroll-wrapped column of row buttons.
    // The QScrollArea keeps arbitrarily wide row text from raising the
    // panel's minimum width. Out-params let one function serve both sections.
    Section buildLogicListSection(const QString& title, const QString& placeholderText, QLabel** placeholderOut,
                                   QWidget** rowsContainerOut, QVBoxLayout** rowsLayoutOut,
                                   QScrollArea** rowsScrollOut, bool defaultExpanded = true,
                                   const char* iconSlug = nullptr);
    // Shared Guards/Actions body: wholesale rebuild into `target`, toggling
    // `placeholder`/`scroll` visibility.
    void rebuildLogicRows(const QVector<LogicRow>& rows, std::vector<LogicListRow>& target, QVBoxLayout* rowsLayout,
                           QLabel* placeholder, QScrollArea* scroll);
    void rebuildActorRows(const QVector<ActorRow>& rows);

    std::vector<Section> sections_;  // Context, Guards, Actions, Actors -- fixed order

    QToolButton* contextAddButton_ = nullptr;
    QLabel* contextPlaceholder_ = nullptr;
    // The row grid lives inside a QScrollArea because its minimumSizeHint
    // ignores the content: the per-row widgets alone would push the panel
    // past its 120px minimum width. Toggle the scroll area's visibility, not
    // the container's.
    QWidget* contextRowsContainer_ = nullptr;
    QScrollArea* contextRowsScroll_ = nullptr;
    QGridLayout* contextRowsLayout_ = nullptr;
    std::vector<ContextRowWidgets> contextRows_;  // mirrors the last setContextVariables() call, in its own order

    // Machine display name; the Hook-badge tooltip names its generated header.
    QString machineName_;

    // Guards and Actions keep separate storage even when their text collides.
    QLabel* guardPlaceholder_ = nullptr;
    QWidget* guardRowsContainer_ = nullptr;
    QScrollArea* guardRowsScroll_ = nullptr;
    QVBoxLayout* guardRowsLayout_ = nullptr;
    std::vector<LogicListRow> guardRows_;  // mirrors the last setGuardRows() call, in its own order

    QLabel* actionPlaceholder_ = nullptr;
    QWidget* actionRowsContainer_ = nullptr;
    QScrollArea* actionRowsScroll_ = nullptr;
    QVBoxLayout* actionRowsLayout_ = nullptr;
    std::vector<LogicListRow> actionRows_;  // mirrors the last setActionRows() call, in its own order

    QLabel* actorPlaceholder_ = nullptr;
    QWidget* actorRowsContainer_ = nullptr;
    QScrollArea* actorRowsScroll_ = nullptr;
    QVBoxLayout* actorRowsLayout_ = nullptr;
    std::vector<ActorListRow> actorRows_;  // mirrors the last setActorRows() call, in its own order

    // At most one rename editor is open; opening a second replaces it without
    // committing.
    QLineEdit* renameEditor_ = nullptr;

    // Deferred-rebuild inputs (see scheduleFlush): last write wins per section.
    bool flushScheduled_ = false;
    bool contextDirty_ = false;
    bool guardRowsDirty_ = false;
    bool actionRowsDirty_ = false;
    bool actorRowsDirty_ = false;
    bool typesDirty_ = false;

    QToolButton* typesAddButton_ = nullptr;
    QLabel* typesPlaceholder_ = nullptr;
    QWidget* typesRowsContainer_ = nullptr;
    QVBoxLayout* typesRowsLayout_ = nullptr;
    QScrollArea* typesRowsScroll_ = nullptr;
    QLineEdit* externalHeadersEdit_ = nullptr;
    QVector<StructDefinition> structDefinitions_;
    QStringList externalHeaders_;

    struct TypeRowWidgets {
        quint64 id = 0;
        StructDefinition def;
        QWidget* container = nullptr;
        QLabel* nameLabel = nullptr;
        QLabel* summaryLabel = nullptr;
        QToolButton* editButton = nullptr;
        QToolButton* deleteButton = nullptr;
    };
    QVector<TypeRowWidgets> typeRows_;

    QVector<ContextVariable> pendingContextVariables_;
    QVector<LogicRow> pendingGuardRows_;
    QVector<LogicRow> pendingActionRows_;
    QVector<ActorRow> pendingActorRows_;
    QVector<StructDefinition> pendingStructDefinitions_;
    QStringList pendingExternalHeaders_;

    // Live overlay, remembered so a mid-run rebuild can re-apply it.
    QVariantMap liveContextValues_;
    QHash<QString, LiveGuardResult> liveGuardResults_;
    QSet<QString> liveActorInvokeIds_;  // keyed by ActorRow::effectiveId
    bool liveRunning_ = false;
};

// Focus-bound Presenter over LogicPanel, constructed fresh by MainWindow each
// time the focused pane's bound machine changes. `machineName` is captured
// once (DocumentSession::machineName(), not Machine::name). Panel updates are
// fact-driven only; see refreshContext().
class LogicAdapter : public ordo::qt::Presenter {
    Q_OBJECT

public:
    LogicAdapter(LogicPanel* panel, QString machineName);

    void onRegister() override;

private:
    void onContextVariableAdded(const events::ContextVariableAdded&);
    void onContextVariableRenamed(const events::ContextVariableRenamed&);
    void onContextTypeChanged(const events::ContextTypeChanged&);
    void onContextInitialValueChanged(const events::ContextInitialValueChanged&);
    void onContextVariableDeleted(const events::ContextVariableDeleted&);

    void onContextNameEditingFinished(quint64 id, QString name);
    void onContextTypeActivated(quint64 id, ContextType type);
    void onContextInitialValueEditingFinished(quint64 id, QString initialValue);
    void onContextDeleteClicked(quint64 id);

    // ---- Guards/Actions sections -------------------------------------------
    // The nine facts that can move the inventory: guard/action string edits,
    // transition/state add or delete, and MachineSnapshotPublished (undo/redo).
    // Other facts never affect inventory(); each handler calls refreshLogic().
    // Live overlay: the run's facts, re-read wholesale. TraceAppended fires on
    // every assign, which is when a context value changes.
    void onTraceAppended(const events::TraceAppended&);
    void onSimulationStarted(const events::SimulationStarted&);
    void onSimulationReset(const events::SimulationReset&);
    void onTransitionFired(const events::TransitionFired&);
    void onModeChanged(const events::ModeChanged&);
    void onGuardResultChanged(const events::GuardResultChanged&);
    void refreshLive();

    void onTransitionGuardChanged(const events::TransitionGuardChanged&);
    void onTransitionActionChanged(const events::TransitionActionChanged&);
    void onEntryActionsChanged(const events::EntryActionsChanged&);
    void onExitActionsChanged(const events::ExitActionsChanged&);
    void onTransitionAdded(const events::TransitionAdded&);
    void onTransitionDeleted(const events::TransitionDeleted&);
    void onStateAdded(const events::StateAdded&);
    void onStateDeleted(const events::StateDeleted&);
    void onMachineSnapshotPublished(const events::MachineSnapshotPublished&);

    // ---- Actors section ----------------------------------------------------
    // The nine facts above already cover invoke declarations appearing or
    // disappearing; these cover a state's invoke fields changing in place.
    void onInvokeSrcChanged(const events::InvokeSrcChanged&);
    void onInvokeIdChanged(const events::InvokeIdChanged&);
    void onInvokeOutputTypeChanged(const events::InvokeOutputTypeChanged&);
    void onInvocationsChanged(const events::InvocationsChanged&);

    // Re-derives all Guards/Actions/Actors rows from the document and pushes
    // them into the panel. Called only from onRegister() and fact handlers.
    void refreshLogic();

    // Re-pushes the whole Context table from doc_->machine().context. Called
    // from onRegister() and fact handlers, never from the edit handlers: the
    // panel must show what the document holds, and a rejected command fires
    // no fact.
    void refreshContext();
    void refreshTypes();

    void onStructAddClicked();
    void onStructDeleteClicked(quint64 id);
    void onStructChanged(StructDefinition def);
    void onExternalHeadersEditingFinished(QStringList headers);
    void onContextCustomTypeNameActivated(quint64 id, QString customTypeName);

    void onExternalHeadersChanged(const events::ExternalHeadersChanged&);
    void onStructDefinitionAdded(const events::StructDefinitionAdded&);
    void onStructDefinitionChanged(const events::StructDefinitionChanged&);
    void onStructDefinitionDeleted(const events::StructDefinitionDeleted&);
    void onContextCustomTypeNameChanged(const events::ContextCustomTypeNameChanged&);

    LogicPanel* panel_;
    QString machineName_;
    std::shared_ptr<MachineDocAgent> doc_;
    std::shared_ptr<SimulationAgent> sim_;  // contextValues() + lastGuardEvaluations()
};

}  // namespace app
