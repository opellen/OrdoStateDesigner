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
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringListModel>
#include <QSyntaxHighlighter>
#include <QTabWidget>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "infra/code_generator.h"
#include "infra/expression.h"        // single-string guard validation
#include "infra/logic_inventory.h"   // guard suggestions, derived not stored

#include "view/shell/icons.h"
#include "view/shell/inspector_panel_internal.h"
#include "view/shell/logic_panel.h"
#include "view/shell/node_code_preview_widget.h"

namespace app {

using namespace inspector_detail;

// ---- InspectorPanel ---------------------------------------------------------

namespace {

constexpr int kMachinePageIndex = 0;
constexpr int kStatePageIndex = 1;
constexpr int kTransitionPageIndex = 2;
constexpr int kCodePageIndex = 3;
constexpr int kNotePageIndex = 4;
// No-machine empty state: reachable only via clearMachine(); any adapter bind leaves it.
constexpr int kEmptyPageIndex = 5;

QWidget* wrapInScrollArea(QWidget* content) {
    auto* scroll = new QScrollArea();
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; } "
                                         "QScrollArea > QWidget > QWidget { background: transparent; }"));
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    return scroll;
}
}  // namespace

InspectorPanel::InspectorPanel(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QStringLiteral("background: %1;").arg(QString::fromLatin1(kSurface1)));

    stackedWidget_ = new QStackedWidget(this);
    stackedWidget_->addWidget(wrapInScrollArea(buildMachineTab()));     // 0: Machine
    stackedWidget_->addWidget(wrapInScrollArea(buildStateTab()));       // 1: State
    stackedWidget_->addWidget(wrapInScrollArea(buildTransitionTab()));  // 2: Transition
    stackedWidget_->addWidget(buildCodeTab());                          // 3: Code
    stackedWidget_->addWidget(wrapInScrollArea(buildNoteTab()));        // 4: Note
    stackedWidget_->addWidget(buildEmptyTab());                         // 5: No machine open (clearMachine())

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(stackedWidget_);

    showTabForSelection(SelectionKind::None);
}

QWidget* InspectorPanel::buildStateTab() {
    auto* tab = new QWidget();
    auto* form = new QFormLayout(tab);

    stateNameEdit_ = new QLineEdit(tab);
    connect(stateNameEdit_, &QLineEdit::editingFinished, this,
            [this] { emit stateNameEditingFinished(stateNameEdit_->text()); });
    form->addRow(QStringLiteral("Name"), stateNameEdit_);

    stateKindCombo_ = new QComboBox(tab);
    stateKindCombo_->addItem(QStringLiteral("Normal"), static_cast<int>(StateKind::Normal));
    stateKindCombo_->addItem(QStringLiteral("Parallel"), static_cast<int>(StateKind::Parallel));
    stateKindCombo_->addItem(QStringLiteral("Final"), static_cast<int>(StateKind::Final));
    stateKindCombo_->addItem(QStringLiteral("History"), static_cast<int>(StateKind::History));
    // activated(), not currentIndexChanged(): fires only on user interaction, so
    // programmatic setCurrentIndex() needs no QSignalBlocker.
    connect(stateKindCombo_, &QComboBox::activated, this, [this](int index) {
        emit stateKindEdited(static_cast<StateKind>(stateKindCombo_->itemData(index).toInt()));
    });
    form->addRow(QStringLiteral("Kind"), stateKindCombo_);

    stateColorRow_ = new ColorPaletteWidget([this](ElementColor c) { emit stateColorEdited(c); }, tab);
    form->addRow(QStringLiteral("Color"), stateColorRow_);

    // Initial is machine-level (Machine::initialStateId), not a kind. clicked(),
    // not toggled(): setStateFields' setChecked() never loops back.
    stateInitialCheck_ = new QCheckBox(QStringLiteral("machine starts here"), tab);
    connect(stateInitialCheck_, &QCheckBox::clicked, this,
            [this](bool checked) { emit stateInitialToggled(checked); });
    form->addRow(QStringLiteral("Initial state"), stateInitialCheck_);

    // Deep-history row: meaningful only for History states; setStateFields()
    // hides the whole row for every other kind. clicked(), as above.
    historyDeepCheck_ = new QCheckBox(QStringLiteral("deep (all levels)"), tab);
    connect(historyDeepCheck_, &QCheckBox::clicked, this,
            [this](bool checked) { emit historyDeepToggled(checked); });
    form->addRow(QStringLiteral("Deep history"), historyDeepCheck_);
    stateForm_ = form;

    entryActionsEdit_ = new QPlainTextEdit(tab);
    entryActionsEdit_->setPlaceholderText(QStringLiteral("one per line, or comma-separated"));
    entryActionsEdit_->setFixedHeight(64);
    entryActionsEdit_->installEventFilter(this);  // FocusOut == "editing finished" (see eventFilter())
    form->addRow(QStringLiteral("Entry actions"), entryActionsEdit_);

    // Exit actions share the entry-actions join/split convention; tags are one comma-separated line.
    exitActionsEdit_ = new QPlainTextEdit(tab);
    exitActionsEdit_->setPlaceholderText(QStringLiteral("one per line, or comma-separated"));
    exitActionsEdit_->setFixedHeight(48);
    exitActionsEdit_->installEventFilter(this);
    form->addRow(QStringLiteral("Exit actions"), exitActionsEdit_);

    descriptionEdit_ = new QPlainTextEdit(tab);
    descriptionEdit_->setPlaceholderText(QStringLiteral("what this state means"));
    descriptionEdit_->setFixedHeight(48);
    descriptionEdit_->installEventFilter(this);
    form->addRow(QStringLiteral("Description"), descriptionEdit_);

    tagsEdit_ = new QLineEdit(tab);
    tagsEdit_->setPlaceholderText(QStringLiteral("comma-separated"));
    connect(tagsEdit_, &QLineEdit::editingFinished, this,
            [this] { emit tagsEditingFinished(splitTags(tagsEdit_->text())); });
    form->addRow(QStringLiteral("Tags"), tagsEdit_);

    // Invoke Service: empty means the state invokes nothing; setStateFields() then
    // disables (not hides, so the row layout never shifts) the Id/Output-type rows.
    // Service and Id commit on Enter only: both cascade to the state's done.invoke/
    // error.platform transition events, too much to trigger by clicking away.
    // Escape reverts to invokeSrcCurrent_ (eventFilter()).
    invokeSrcEdit_ = new QLineEdit(tab);
    invokeSrcEdit_->setPlaceholderText(QStringLiteral("(invokes nothing)"));
    connect(invokeSrcEdit_, &QLineEdit::returnPressed, this, [this] {
        if (currentInvocations_.size() > 1) {
            currentInvocations_[0].src = invokeSrcEdit_->text();
            emit invocationsEdited(currentInvocations_);
        } else {
            emit invokeSrcEditingFinished(invokeSrcEdit_->text());
        }
    });
    invokeSrcEdit_->installEventFilter(this);  // Esc -> revert, see eventFilter() below
    form->addRow(QStringLiteral("Service"), invokeSrcEdit_);

    // Id: empty means "derive from Service"; setStateFields() mirrors the live
    // Service into the placeholder. Commit-on-Enter only, like Service.
    invokeIdEdit_ = new QLineEdit(tab);
    connect(invokeIdEdit_, &QLineEdit::returnPressed, this, [this] {
        if (currentInvocations_.size() > 1) {
            currentInvocations_[0].id = invokeIdEdit_->text();
            emit invocationsEdited(currentInvocations_);
        } else {
            emit invokeIdEditingFinished(invokeIdEdit_->text());
        }
    });
    invokeIdEdit_->installEventFilter(this);
    form->addRow(QStringLiteral("Id"), invokeIdEdit_);

    // Output type: declared type of the invoke's onDone payload. A combo with
    // activated(), so it needs no focus-out or Escape handling.
    invokeOutputTypeCombo_ = new QComboBox(tab);
    invokeOutputTypeCombo_->addItem(QStringLiteral("Bool"), static_cast<int>(ContextType::Bool));
    invokeOutputTypeCombo_->addItem(QStringLiteral("Int"), static_cast<int>(ContextType::Int));
    invokeOutputTypeCombo_->addItem(QStringLiteral("Double"), static_cast<int>(ContextType::Double));
    invokeOutputTypeCombo_->addItem(QStringLiteral("String"), static_cast<int>(ContextType::String));
    connect(invokeOutputTypeCombo_, &QComboBox::activated, this, [this](int index) {
        const auto type = static_cast<ContextType>(invokeOutputTypeCombo_->itemData(index).toInt());
        if (currentInvocations_.size() > 1) {
            currentInvocations_[0].outputType = type;
            emit invocationsEdited(currentInvocations_);
        } else {
            emit invokeOutputTypeEdited(type);
        }
    });
    form->addRow(QStringLiteral("Output type"), invokeOutputTypeCombo_);

    // Multi-invocation cards container and "+ Add Service" button
    invocationsContainer_ = new QWidget(tab);
    invocationsCardsLayout_ = new QVBoxLayout(invocationsContainer_);
    invocationsCardsLayout_->setContentsMargins(0, 4, 0, 4);
    invocationsCardsLayout_->setSpacing(6);
    invocationsContainer_->setVisible(false);
    form->addRow(QString(), invocationsContainer_);

    addInvocationBtn_ = new QPushButton(QStringLiteral("+ Add Service"), tab);
    addInvocationBtn_->setStyleSheet(QStringLiteral("QPushButton { text-align: center; padding: 3px 8px; }"));
    connect(addInvocationBtn_, &QPushButton::clicked, this, &InspectorPanel::onAddInvocationClicked);
    form->addRow(QString(), addInvocationBtn_);

    statePositionValue_ = new QLabel(tab);
    statePositionValue_->setStyleSheet(kTypeValue);
    form->addRow(QStringLiteral("Position"), statePositionValue_);

    stateCodePreview_ = new NodeCodePreviewWidget(/*isTransition=*/false, tab);
    form->addRow(stateCodePreview_);

    styleInspectorForm(form);  // AFTER every addRow() -- see its own comment on why
    return tab;
}

QWidget* InspectorPanel::buildTransitionTab() {
    auto* tab = new QWidget();
    auto* form = new QFormLayout(tab);

    transitionEventEdit_ = new QLineEdit(tab);
    connect(transitionEventEdit_, &QLineEdit::editingFinished, this,
            [this] { emit transitionEventEditingFinished(transitionEventEdit_->text()); });
    connect(transitionEventEdit_, &QLineEdit::textChanged, this,
            [this](const QString& text) { emit transitionEventTextChanged(text); });
    form->addRow(QStringLiteral("Event"), transitionEventEdit_);

    eventFeedbackLabel_ = new QLabel(tab);
    eventFeedbackLabel_->setWordWrap(true);
    eventFeedbackLabel_->setVisible(false);
    form->addRow(eventFeedbackLabel_);

    transitionPayloadTypeEdit_ = new QLineEdit(tab);
    transitionPayloadTypeEdit_->setPlaceholderText(QStringLiteral("e.g. CanMessage or int"));
    payloadTypeSuggestionModel_ = new QStringListModel(this);
    payloadTypeCompleter_ = new QCompleter(payloadTypeSuggestionModel_, this);
    payloadTypeCompleter_->setCaseSensitivity(Qt::CaseSensitive);
    payloadTypeCompleter_->setCompletionMode(QCompleter::PopupCompletion);
    transitionPayloadTypeEdit_->setCompleter(payloadTypeCompleter_);
    connect(transitionPayloadTypeEdit_, &QLineEdit::editingFinished, this,
            [this] { emit transitionPayloadTypeEditingFinished(transitionPayloadTypeEdit_->text().trimmed()); });
    form->addRow(QStringLiteral("Payload type"), transitionPayloadTypeEdit_);

    transitionGuardEdit_ = new QLineEdit(tab);
    connect(transitionGuardEdit_, &QLineEdit::editingFinished, this,
            [this] { emit transitionGuardEditingFinished(transitionGuardEdit_->text()); });
    // textChanged fires per keystroke for live feedback only; it never sends an
    // intent, so it cannot flood the undo stack.
    connect(transitionGuardEdit_, &QLineEdit::textChanged, this,
            [this](const QString& text) { emit transitionGuardTextChanged(text); });

    // Type-ahead over guards already used in this machine. Popup mode (does not
    // touch the typed text) and case-sensitive: guard names are C++ identifiers.
    guardSuggestionModel_ = new QStringListModel(this);
    guardCompleter_ = new QCompleter(guardSuggestionModel_, this);
    guardCompleter_->setCaseSensitivity(Qt::CaseSensitive);
    guardCompleter_->setCompletionMode(QCompleter::PopupCompletion);
    transitionGuardEdit_->setCompleter(guardCompleter_);

    auto* guardLayout = new QHBoxLayout();
    guardLayout->setContentsMargins(0, 0, 0, 0);
    guardLayout->setSpacing(4);
    guardLayout->addWidget(transitionGuardEdit_);

    transitionGuardExpandBtn_ = new QToolButton(tab);
    transitionGuardExpandBtn_->setText(QStringLiteral("..."));
    transitionGuardExpandBtn_->setToolTip(QStringLiteral("Open Expression Editor (Ctrl+E)"));
    transitionGuardExpandBtn_->setCursor(Qt::PointingHandCursor);
    connect(transitionGuardExpandBtn_, &QToolButton::clicked, this,
            [this] { emit transitionGuardExpandRequested(); });
    guardLayout->addWidget(transitionGuardExpandBtn_);

    auto* guardShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_E), transitionGuardEdit_);
    guardShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(guardShortcut, &QShortcut::activated, this,
            [this] { emit transitionGuardExpandRequested(); });

    form->addRow(QStringLiteral("Guard"), guardLayout);

    // Feedback line under the field, full form width; hidden when there is nothing to say.
    guardFeedbackLabel_ = new QLabel(tab);
    guardFeedbackLabel_->setWordWrap(true);
    guardFeedbackLabel_->setVisible(false);
    form->addRow(guardFeedbackLabel_);

    transitionActionEdit_ = new QLineEdit(tab);
    connect(transitionActionEdit_, &QLineEdit::editingFinished, this,
            [this] { emit transitionActionEditingFinished(transitionActionEdit_->text()); });
    connect(transitionActionEdit_, &QLineEdit::textChanged, this,
            [this](const QString& text) { emit transitionActionTextChanged(text); });

    auto* actionLayout = new QHBoxLayout();
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(4);
    actionLayout->addWidget(transitionActionEdit_);

    transitionActionExpandBtn_ = new QToolButton(tab);
    transitionActionExpandBtn_->setText(QStringLiteral("..."));
    transitionActionExpandBtn_->setToolTip(QStringLiteral("Open Expression Editor (Ctrl+E)"));
    transitionActionExpandBtn_->setCursor(Qt::PointingHandCursor);
    connect(transitionActionExpandBtn_, &QToolButton::clicked, this,
            [this] { emit transitionActionExpandRequested(); });
    actionLayout->addWidget(transitionActionExpandBtn_);

    auto* actionShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_E), transitionActionEdit_);
    actionShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(actionShortcut, &QShortcut::activated, this,
            [this] { emit transitionActionExpandRequested(); });

    form->addRow(QStringLiteral("Action"), actionLayout);

    actionFeedbackLabel_ = new QLabel(tab);
    actionFeedbackLabel_->setWordWrap(true);
    actionFeedbackLabel_->setVisible(false);
    form->addRow(actionFeedbackLabel_);

    transitionDelaySpin_ = new QSpinBox(tab);
    transitionDelaySpin_->setRange(0, 60000);
    transitionDelaySpin_->setSuffix(QStringLiteral(" ms"));
    // editingFinished(), not valueChanged(): fires once on Enter/focus-out, and
    // never on programmatic setValue().
    connect(transitionDelaySpin_, &QSpinBox::editingFinished, this,
            [this] { emit transitionDelayEdited(transitionDelaySpin_->value()); });
    form->addRow(QStringLiteral("Delay (ms)"), transitionDelaySpin_);

    transitionReenterCheck_ = new QCheckBox(tab);
    connect(transitionReenterCheck_, &QCheckBox::toggled, this,
            [this](bool checked) { emit transitionReenterToggled(checked); });
    form->addRow(QStringLiteral("Re-enter"), transitionReenterCheck_);

    transitionAlwaysCheck_ = new QCheckBox(tab);
    connect(transitionAlwaysCheck_, &QCheckBox::toggled, this,
            [this](bool checked) { emit transitionAlwaysToggled(checked); });
    form->addRow(QStringLiteral("Always"), transitionAlwaysCheck_);

    transitionColorRow_ = new ColorPaletteWidget([this](ElementColor c) { emit transitionColorEdited(c); }, tab);
    form->addRow(QStringLiteral("Color"), transitionColorRow_);

    transitionSourceValue_ = new QLabel(tab);
    transitionSourceValue_->setStyleSheet(kTypeValue);
    form->addRow(QStringLiteral("Source"), transitionSourceValue_);

    transitionTargetEdit_ = new QLineEdit(tab);
    connect(transitionTargetEdit_, &QLineEdit::editingFinished, this,
            [this] { emit transitionTargetEditingFinished(transitionTargetEdit_->text()); });
    connect(transitionTargetEdit_, &QLineEdit::textChanged, this,
            [this](const QString& text) { emit transitionTargetTextChanged(text); });
    form->addRow(QStringLiteral("Target"), transitionTargetEdit_);

    targetFeedbackLabel_ = new QLabel(tab);
    targetFeedbackLabel_->setWordWrap(true);
    targetFeedbackLabel_->setVisible(false);
    form->addRow(targetFeedbackLabel_);

    transitionCodePreview_ = new NodeCodePreviewWidget(/*isTransition=*/true, tab);
    form->addRow(transitionCodePreview_);

    styleInspectorForm(form);  // AFTER every addRow() -- see its own comment on why
    return tab;
}

QWidget* InspectorPanel::buildNoteTab() {
    auto* tab = new QWidget();
    auto* form = new QFormLayout(tab);

    noteTextEdit_ = new QPlainTextEdit(tab);
    noteTextEdit_->setPlaceholderText(QStringLiteral("Note content..."));
    noteTextEdit_->setFixedHeight(90);
    noteTextEdit_->installEventFilter(this);
    form->addRow(QStringLiteral("Text"), noteTextEdit_);

    noteColorRow_ = new ColorPaletteWidget([this](ElementColor c) { emit noteColorEdited(c); }, tab);
    form->addRow(QStringLiteral("Color"), noteColorRow_);

    styleInspectorForm(form);  // AFTER every addRow() -- see its own comment on why
    return tab;
}

QWidget* InspectorPanel::buildMachineTab() {
    auto* tab = new QWidget();
    auto* outer = new QVBoxLayout(tab);
    // No implicit spacing: every gap on `outer` is an explicit addSpacing() from
    // the kSpace* scale, so a blanket value can't land inside a section (e.g.
    // between the INVOCATIONS heading and its entries).
    outer->setSpacing(0);

    auto* form = new QFormLayout();
    machineNameValue_ = new QLabel(tab);
    machineNameValue_->setStyleSheet(kTypeValue);
    form->addRow(QStringLiteral("Machine"), machineNameValue_);
    // Anchor for debugMachineFormLabelWidth(); grabbed before styleInspectorForm() resizes it.
    machineNameLabel_ = qobject_cast<QLabel*>(form->itemAt(form->rowCount() - 1, QFormLayout::LabelRole)->widget());
    machineCountsValue_ = new QLabel(tab);
    machineCountsValue_->setStyleSheet(kTypeValue);
    form->addRow(QStringLiteral("States / Transitions"), machineCountsValue_);
    machineModeValue_ = new QLabel(tab);
    machineModeValue_->setStyleSheet(kTypeValue);
    form->addRow(QStringLiteral("Mode"), machineModeValue_);
    styleInspectorForm(form);  // AFTER every addRow() -- see its own comment on why
    outer->addLayout(form);
    outer->addSpacing(kSpace4);  // section gap: meta form -> SIMULATION

    // SIMULATION: collapsible, expanded by default (its controls are used immediately).
    // buildCollapsibleSection() nests rows at tab -> container -> body -> row,
    // the same depth as the EVENTS and GUARD RESULTS sections below.
    inspector_detail::CollapsibleSection simSection = inspector_detail::buildCollapsibleSection(
        tab, QStringLiteral("Simulation"), QString(), /*enabled=*/true, /*defaultExpanded=*/true,
        /*iconSlug=*/"simulate");
    auto* simBodyLayout = qobject_cast<QVBoxLayout*>(simSection.body->layout());
    QWidget* simParent = simSection.body;  // every row widget below is parented here

    // These two label/value rows are not a QFormLayout, so they take the shared
    // kRowLabelColumnWidth/kRowHeight/kTypeLabel directly to keep the value
    // column on the same x as the meta form above.
    auto* chipRow = new QHBoxLayout();
    simCurrentStateLabel_ = new QLabel(QStringLiteral("Current state:"), simParent);
    simCurrentStateLabel_->setStyleSheet(kTypeLabel);
    simCurrentStateLabel_->setFixedWidth(kRowLabelColumnWidth);
    simCurrentStateLabel_->setMinimumHeight(kRowHeight);
    simulationChip_ = new QLabel(QStringLiteral("idle"), simParent);
    simulationChip_->setStyleSheet(kChipStyle);
    chipRow->addWidget(simCurrentStateLabel_);
    chipRow->addWidget(simulationChip_);
    chipRow->addStretch(1);
    simBodyLayout->addLayout(chipRow);

    auto* timescaleRow = new QHBoxLayout();
    auto* timescaleLabel = new QLabel(QStringLiteral("Speed:"), simParent);
    timescaleLabel->setStyleSheet(kTypeLabel);
    timescaleLabel->setFixedWidth(kRowLabelColumnWidth);
    timescaleLabel->setMinimumHeight(kRowHeight);
    timescaleCombo_ = new QComboBox(simParent);
    timescaleCombo_->setStyleSheet(kTypeValue);
    timescaleCombo_->setMinimumHeight(kRowHeight);
    timescaleCombo_->addItem(QStringLiteral("0.1x"), 0.1);
    timescaleCombo_->addItem(QStringLiteral("0.25x"), 0.25);
    timescaleCombo_->addItem(QStringLiteral("0.5x"), 0.5);
    timescaleCombo_->addItem(QStringLiteral("1.0x (Normal)"), 1.0);
    timescaleCombo_->addItem(QStringLiteral("2.0x"), 2.0);
    timescaleCombo_->addItem(QStringLiteral("5.0x"), 5.0);
    timescaleCombo_->addItem(QStringLiteral("10.0x"), 10.0);
    timescaleCombo_->setCurrentIndex(3);
    connect(timescaleCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0) {
            emit timescaleChanged(timescaleCombo_->itemData(index).toDouble());
        }
    });
    timescaleRow->addWidget(timescaleLabel);
    timescaleRow->addWidget(timescaleCombo_, 1);
    simBodyLayout->addLayout(timescaleRow);

    // Run is the one primary button (accent fill); Pause/Reset/Back are secondary
    // (outline only). Each style carries its own :disabled rule.
    auto* controlsRow = new QHBoxLayout();
    runButton_ = new QPushButton(QStringLiteral("Run"), simParent);
    runButton_->setStyleSheet(kPrimaryButtonStyle);
    connect(runButton_, &QPushButton::clicked, this, &InspectorPanel::runClicked);
    pauseButton_ = new QPushButton(QStringLiteral("Pause"), simParent);
    pauseButton_->setStyleSheet(kSecondaryButtonStyle);
    connect(pauseButton_, &QPushButton::clicked, this, &InspectorPanel::pauseClicked);
    resetButton_ = new QPushButton(QStringLiteral("Reset"), simParent);
    resetButton_->setStyleSheet(kSecondaryButtonStyle);
    connect(resetButton_, &QPushButton::clicked, this, &InspectorPanel::resetClicked);
    backButton_ = new QPushButton(QStringLiteral("Back"), simParent);
    backButton_->setStyleSheet(kSecondaryButtonStyle);
    connect(backButton_, &QPushButton::clicked, this, &InspectorPanel::backClicked);
    // Everything starts disabled: the boot mode is Design, and the adapter's
    // first refreshMachineTab() pushes the real gating immediately.
    for (QPushButton* button : {runButton_, pauseButton_, resetButton_, backButton_}) {
        button->setEnabled(false);
        controlsRow->addWidget(button);
    }
    simBodyLayout->addLayout(controlsRow);

    // No outer gap between sections from here down: each header carries its
    // own symmetric pad and each expanded body its own bottom gap
    // (buildCollapsibleSection()), so the header row centers between hairlines.
    outer->addWidget(simSection.container);

    // INVOCATIONS is deliberately not built with buildCollapsibleSection(): its
    // extra nesting would push each live-invocation group box two levels deeper
    // and risk the offscreen paint-recursion stack overflow. The header instead
    // toggles the entries' visibility in place (applyInvocationsVisibility()).
    // One group box per live invocation (rebuildInvocationEntriesNow());
    // invocationsGuidance_ replaces them when none are live.
    invocationsHeading_ = new inspector_detail::SectionHeaderButton(tab);
    invocationsHeading_->setCheckable(true);
    invocationsHeading_->setChecked(true);  // expanded by default
    invocationsHeading_->setIconSlug("hook");  // an invocation generates a start<Service> hook
    setSectionHeaderTitle(invocationsHeading_, QStringLiteral("Invocations (0)"));
    applyTypeSectionFont(invocationsHeading_);
    connect(invocationsHeading_, &QToolButton::toggled, this, [this](bool) { applyInvocationsVisibility(); });
    outer->addWidget(invocationsHeading_);
    invocationsBodyLayout_ = new QVBoxLayout();
    invocationsBodyLayout_->setSpacing(0);
    outer->addLayout(invocationsBodyLayout_);
    invocationEntriesLayout_ = new QVBoxLayout();
    invocationsBodyLayout_->addLayout(invocationEntriesLayout_);
    invocationsGuidance_ = new QLabel(tab);
    invocationsGuidance_->setWordWrap(true);
    invocationsGuidance_->setStyleSheet(QStringLiteral("color: %1; padding: 0px 2px 4px 2px;")
                                             .arg(QString::fromLatin1(inspector_detail::kTextDisabled)));
    invocationsGuidance_->setVisible(false);
    invocationsBodyLayout_->addWidget(invocationsGuidance_);
    applyInvocationsVisibility();  // sets the body margins for the initial expanded state

    // EVENTS: expanded by default. Placeholder is empty because
    // setEventVocabulary() builds its own "No transition events" row.
    inspector_detail::CollapsibleSection eventsSection = inspector_detail::buildCollapsibleSection(
        tab, QStringLiteral("Events"), QString(), /*enabled=*/true, /*defaultExpanded=*/true,
        /*iconSlug=*/"play");
    eventsSectionHeader_ = eventsSection.header;
    eventsSectionContainer_ = eventsSection.container;
    eventVocabularyLayout_ = new QVBoxLayout();
    qobject_cast<QVBoxLayout*>(eventsSection.body->layout())->addLayout(eventVocabularyLayout_);
    outer->addWidget(eventsSection.container);

    // GUARD RESULTS: collapsed by default. Named apart from the Logic panel's
    // "Guards" declaration list on the same tab; this is the runtime evaluation.
    inspector_detail::CollapsibleSection guardsSection = inspector_detail::buildCollapsibleSection(
        tab, QStringLiteral("Guard Results"), QString(), /*enabled=*/true, /*defaultExpanded=*/false,
        /*iconSlug=*/"check");
    guardsSectionHeader_ = guardsSection.header;
    guardsSectionContainer_ = guardsSection.container;
    guardVocabularyLayout_ = new QVBoxLayout();
    qobject_cast<QVBoxLayout*>(guardsSection.body->layout())->addLayout(guardVocabularyLayout_);
    outer->addWidget(guardsSection.container);

    logicPanel_ = new LogicPanel(tab);
    outer->addWidget(logicPanel_);

    outer->addStretch(1);

    return tab;
}

QWidget* InspectorPanel::buildCodeTab() {
    auto* tab = new QWidget();
    auto* layout = new QVBoxLayout(tab);

    codeFileCombo_ = new QComboBox(tab);
    connect(codeFileCombo_, &QComboBox::activated, this, [this](int) { updateCodePreview(); });
    layout->addWidget(codeFileCombo_);

    codePreviewEdit_ = new QPlainTextEdit(tab);
    codePreviewEdit_->setReadOnly(true);
    codePreviewEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont monoFont(QStringLiteral("Consolas"));
    monoFont.setStyleHint(QFont::Monospace);
    codePreviewEdit_->setFont(monoFont);
    // Color only: kTypeValue's font-size would override the monospace font above.
    codePreviewEdit_->setStyleSheet(QStringLiteral("color: %1;").arg(QString::fromLatin1(kTextPrimary)));
    // Owned by the document (QObject parent) -- no member needed.
    new CppHighlighter(codePreviewEdit_->document());
    layout->addWidget(codePreviewEdit_, 1);

    return tab;
}

// Empty state: static copy, no member widgets. Top-left aligned with the form
// padding (14/14/14/10), not centered, so it doesn't read as a loading state.
QWidget* InspectorPanel::buildEmptyTab() {
    auto* tab = new QWidget();
    tab->setStyleSheet(design::resolveRoles(QStringLiteral("background: {surface-1};")));

    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(14, 14, 14, 10);
    layout->setSpacing(kSpace1);
    layout->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    const QString textStyle = design::resolveRoles(QStringLiteral("color: {text-disabled}; font-size: 12px;"));

    auto* title = new QLabel(QStringLiteral("No machine open"), tab);
    title->setStyleSheet(textStyle);
    layout->addWidget(title);

    auto* subtitle = new QLabel(QStringLiteral("Open a project or create a machine to inspect it."), tab);
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(textStyle);
    layout->addWidget(subtitle);

    layout->addStretch(1);
    return tab;
}

void InspectorPanel::debugClickInitialCheckbox() {
    stateInitialCheck_->click();
}

void InspectorPanel::debugClickHistoryDeepCheckbox() {
    historyDeepCheck_->click();
}

bool InspectorPanel::debugHistoryDeepRowVisible() const { return historyDeepCheck_->isVisible(); }

void InspectorPanel::debugClickTransitionReenterCheckbox() {
    if (transitionReenterCheck_) {
        transitionReenterCheck_->click();
    }
}

void InspectorPanel::debugClickTransitionAlwaysCheckbox() {
    if (transitionAlwaysCheck_) {
        transitionAlwaysCheck_->click();
    }
}

bool InspectorPanel::debugTransitionReenterChecked() const {
    return transitionReenterCheck_ ? transitionReenterCheck_->isChecked() : false;
}

bool InspectorPanel::debugTransitionAlwaysChecked() const {
    return transitionAlwaysCheck_ ? transitionAlwaysCheck_->isChecked() : false;
}

void InspectorPanel::debugClickGuardExpand() {
    if (transitionGuardExpandBtn_) {
        transitionGuardExpandBtn_->click();
    }
}

void InspectorPanel::debugClickActionExpand() {
    if (transitionActionExpandBtn_) {
        transitionActionExpandBtn_->click();
    }
}

void InspectorPanel::debugSetKind(StateKind kind) {
    stateKindCombo_->setCurrentIndex(stateKindCombo_->findData(static_cast<int>(kind)));
    emit stateKindEdited(kind);
}

void InspectorPanel::debugCommitDescription(const QString& text) {
    descriptionEdit_->setPlainText(text);
    emit descriptionEditingFinished(text);
}

void InspectorPanel::debugCommitTags(const QString& commaSeparated) {
    tagsEdit_->setText(commaSeparated);
    emit tagsEditingFinished(splitTags(commaSeparated));
}

void InspectorPanel::debugCommitExitActions(const QString& text) {
    exitActionsEdit_->setPlainText(text);
    emit exitActionsEditingFinished(splitEntryActions(text));
}

void InspectorPanel::debugCommitInvokeSrc(const QString& text) {
    invokeSrcEdit_->setText(text);
    emit invokeSrcEditingFinished(text);  // mirrors returnPressed, NOT editingFinished -- see the signal's own comment
}

void InspectorPanel::debugCommitInvokeId(const QString& text) {
    invokeIdEdit_->setText(text);
    emit invokeIdEditingFinished(text);
}

void InspectorPanel::debugSetInvokeOutputType(ContextType type) {
    invokeOutputTypeCombo_->setCurrentIndex(invokeOutputTypeCombo_->findData(static_cast<int>(type)));
    emit invokeOutputTypeEdited(type);
}

bool InspectorPanel::debugInvokeIdEnabled() const { return invokeIdEdit_->isEnabled(); }

bool InspectorPanel::debugInvokeOutputTypeEnabled() const { return invokeOutputTypeCombo_->isEnabled(); }

QString InspectorPanel::debugInvokeIdPlaceholder() const { return invokeIdEdit_->placeholderText(); }

void InspectorPanel::debugClickAddInvocation() {
    if (addInvocationBtn_) {
        addInvocationBtn_->click();
    }
}

void InspectorPanel::debugRemoveInvocation(int index) {
    if (index > 0 && index - 1 < invocationCards_.size() && invocationCards_[index - 1].removeBtn) {
        invocationCards_[index - 1].removeBtn->click();
    }
}

int InspectorPanel::debugInvocationCardCount() const {
    return currentInvocations_.size();
}

void InspectorPanel::debugCommitInvocationCardSrc(int index, const QString& src) {
    if (index == 0) {
        debugCommitInvokeSrc(src);
    } else if (index - 1 < invocationCards_.size() && invocationCards_[index - 1].srcEdit) {
        QLineEdit* edit = invocationCards_[index - 1].srcEdit;
        edit->setText(src);
        // Fires the card's own returnPressed() so the real handler runs, as Enter would.
        emit edit->returnPressed();
    }
}

void InspectorPanel::debugCommitInvocationCardId(int index, const QString& id) {
    if (index == 0) {
        debugCommitInvokeId(id);
    } else if (index - 1 < invocationCards_.size() && invocationCards_[index - 1].idEdit) {
        QLineEdit* edit = invocationCards_[index - 1].idEdit;
        edit->setText(id);
        emit edit->returnPressed();
    }
}

void InspectorPanel::debugSetInvocationCardOutputType(int index, ContextType type) {
    if (index == 0) {
        debugSetInvokeOutputType(type);
    } else if (index - 1 < invocationCards_.size() && invocationCards_[index - 1].outputTypeCombo) {
        QComboBox* combo = invocationCards_[index - 1].outputTypeCombo;
        const int newIndex = combo->findData(static_cast<int>(type));
        combo->setCurrentIndex(newIndex);
        // activated() doesn't fire from setCurrentIndex(); emit it so the real handler runs.
        emit combo->activated(newIndex);
    }
}

QComboBox* InspectorPanel::debugInvocationCardOutputTypeCombo(int index) const {
    if (index <= 0 || index - 1 >= invocationCards_.size()) {
        return nullptr;
    }
    return invocationCards_[index - 1].outputTypeCombo;
}

QRect InspectorPanel::debugInvokeFieldsRegion() const {
    // Rects are mapped into this panel's frame, ready for QWidget::grab().
    auto rectInPanel = [this](const QWidget* field) {
        const QPoint topLeft = field->mapTo(const_cast<InspectorPanel*>(this), QPoint(0, 0));
        return QRect(topLeft, field->size());
    };
    // Each row's label joins its field's rect so the capture is self-explanatory.
    auto rowRect = [this, &rectInPanel](QWidget* field) {
        QRect rect = rectInPanel(field);
        int row = -1;
        QFormLayout::ItemRole role = QFormLayout::LabelRole;
        stateForm_->getWidgetPosition(field, &row, &role);  // void; leaves row == -1 if not found
        if (row >= 0) {
            if (QLayoutItem* labelItem = stateForm_->itemAt(row, QFormLayout::LabelRole)) {
                if (QWidget* label = labelItem->widget()) {
                    rect = rect.united(rectInPanel(label));
                }
            }
        }
        return rect;
    };
    QRect region = rowRect(invokeSrcEdit_);
    region = region.united(rowRect(invokeIdEdit_));
    region = region.united(rowRect(invokeOutputTypeCombo_));
    return region.adjusted(-8, -8, 8, 8);
}

// ---- Invocation completions (Machine tab) -----------------------------------

QVariant InspectorPanel::entryPayload(const InvocationEntryWidgets& widgets) {
    switch (widgets.outputType) {
        case ContextType::Bool:
            return widgets.payloadBoolCheck->isChecked();
        case ContextType::Int:
            return widgets.payloadIntSpin->value();
        case ContextType::Double:
            return widgets.payloadDoubleSpin->value();
        case ContextType::String:
            return widgets.payloadStringEdit->text();
    }
    return QVariant();  // unreachable -- ContextType has exactly these four values
}

void InspectorPanel::setLiveInvocations(const QVector<LiveInvocationRow>& rows, bool running) {
    pendingLiveInvocations_ = rows;
    pendingLiveInvocationsRunning_ = running;
    invocationEntriesDirty_ = true;
    scheduleInvocationFlush();
}

void InspectorPanel::scheduleInvocationFlush() {
    if (invocationFlushScheduled_) {
        return;  // one flush per event-loop turn, however many facts arrive
    }
    invocationFlushScheduled_ = true;
    QTimer::singleShot(0, this, &InspectorPanel::flushPendingInvocationRebuild);
}

void InspectorPanel::flushPendingInvocationRebuild() {
    invocationFlushScheduled_ = false;
    if (!invocationEntriesDirty_) {
        return;
    }
    invocationEntriesDirty_ = false;
    rebuildInvocationEntriesNow(pendingLiveInvocations_, pendingLiveInvocationsRunning_);
}

void InspectorPanel::rebuildInvocationEntriesNow(const QVector<LiveInvocationRow>& rows, bool running) {
    clearLayoutWidgets(invocationEntriesLayout_);  // deletes every entry's group box, and everything inside it
    invocationEntries_.clear();

    // The live count lives in the header text, refreshed here only.
    setSectionHeaderTitle(invocationsHeading_, QStringLiteral("Invocations (%1)").arg(rows.size()));

    if (rows.isEmpty()) {
        // Two messages, one per fix: start the simulation, or get an invocation live.
        invocationsGuidance_->setText(running ? QStringLiteral("Running, but no invocation is currently live — "
                                                                "enter an invoking state to arm one")
                                               : QStringLiteral("Fires only while running, with a state's "
                                                                "invocation live"));
        invocationsGuidanceApplicable_ = true;
        applyInvocationsVisibility();
        return;
    }
    invocationsGuidance_->clear();
    invocationsGuidanceApplicable_ = false;

    for (const LiveInvocationRow& row : rows) {
        // Title: "<src> #<effectiveId> — <owning state>", so same-src entries stay distinguishable.
        const QString label = QStringLiteral("%1 #%2 %3 %4")
                                   .arg(row.src, row.effectiveId, QString(QChar(0x2014)), row.stateName);
        auto* group = new QGroupBox(label, invocationEntriesLayout_->parentWidget());
        group->setStyleSheet(kGroupBoxTitleStyle);
        auto* entryForm = new QFormLayout(group);

        InvocationEntryWidgets widgets;
        widgets.effectiveId = row.effectiveId;
        widgets.container = group;
        widgets.outputType = row.outputType;

        // Payload editor: one page per ContextType, inserted in enum order so the
        // enum value is the page index.
        widgets.payloadStack = new QStackedWidget(group);
        widgets.payloadBoolCheck = new QCheckBox(QStringLiteral("true"), group);
        widgets.payloadStack->insertWidget(static_cast<int>(ContextType::Bool), widgets.payloadBoolCheck);
        widgets.payloadIntSpin = new QSpinBox(group);
        widgets.payloadIntSpin->setRange(-1000000, 1000000);  // arrows/PgUp-PgDn come free from QAbstractSpinBox
        widgets.payloadStack->insertWidget(static_cast<int>(ContextType::Int), widgets.payloadIntSpin);
        widgets.payloadDoubleSpin = new QDoubleSpinBox(group);
        widgets.payloadDoubleSpin->setRange(-1.0e9, 1.0e9);
        widgets.payloadDoubleSpin->setDecimals(6);
        widgets.payloadStack->insertWidget(static_cast<int>(ContextType::Double), widgets.payloadDoubleSpin);
        widgets.payloadStringEdit = new QLineEdit(group);
        widgets.payloadStack->insertWidget(static_cast<int>(ContextType::String), widgets.payloadStringEdit);
        widgets.payloadStack->setCurrentIndex(static_cast<int>(row.outputType));
        // A QStackedWidget's minimum width is its widest page's, hidden pages included;
        // the wide double spin box otherwise pushed the Machine tab past the Inspector's width.
        widgets.payloadStack->setSizePolicy(QSizePolicy::Ignored, widgets.payloadStack->sizePolicy().verticalPolicy());
        entryForm->addRow(QStringLiteral("Payload (output)"), widgets.payloadStack);

        // Done reads the payload at click time from this entry's own widgets only.
        widgets.doneButton = new QPushButton(QStringLiteral("Done"), group);
        const QString effectiveId = row.effectiveId;  // captured by value -- entryPayload() needs `widgets` too
        QCheckBox* boolCheck = widgets.payloadBoolCheck;
        QSpinBox* intSpin = widgets.payloadIntSpin;
        QDoubleSpinBox* doubleSpin = widgets.payloadDoubleSpin;
        QLineEdit* stringEdit = widgets.payloadStringEdit;
        const ContextType outputType = row.outputType;
        connect(widgets.doneButton, &QPushButton::clicked, this,
                [this, effectiveId, outputType, boolCheck, intSpin, doubleSpin, stringEdit] {
                    InvocationEntryWidgets snapshot;
                    snapshot.outputType = outputType;
                    snapshot.payloadBoolCheck = boolCheck;
                    snapshot.payloadIntSpin = intSpin;
                    snapshot.payloadDoubleSpin = doubleSpin;
                    snapshot.payloadStringEdit = stringEdit;
                    emit invocationDoneClicked(effectiveId, entryPayload(snapshot));
                });
        entryForm->addRow(widgets.doneButton);

        // Error's payload is always a string regardless of outputType, so it has its own line edit.
        widgets.errorMessageEdit = new QLineEdit(group);
        widgets.errorMessageEdit->setPlaceholderText(QStringLiteral("error message"));
        entryForm->addRow(QStringLiteral("Error message"), widgets.errorMessageEdit);

        widgets.errorButton = new QPushButton(QStringLiteral("Error"), group);
        QLineEdit* errorEdit = widgets.errorMessageEdit;
        connect(widgets.errorButton, &QPushButton::clicked, this,
                [this, effectiveId, errorEdit] { emit invocationErrorClicked(effectiveId, errorEdit->text()); });
        entryForm->addRow(widgets.errorButton);

        styleInspectorForm(entryForm);  // AFTER every addRow() -- see its own comment on why
        invocationEntriesLayout_->addWidget(group);
        invocationEntries_.push_back(widgets);
    }
    applyInvocationsVisibility();  // a rebuild while collapsed must not resurface the entries
}

// Entries and guidance show only while the header is expanded; guidance also
// requires invocationsGuidanceApplicable_ (no live invocations).
void InspectorPanel::applyInvocationsVisibility() {
    const bool expanded = invocationsHeading_ == nullptr || invocationsHeading_->isChecked();
    for (int i = 0; i < invocationEntriesLayout_->count(); ++i) {
        if (QWidget* widget = invocationEntriesLayout_->itemAt(i)->widget()) {
            widget->setVisible(expanded);
        }
    }
    invocationsGuidance_->setVisible(expanded && invocationsGuidanceApplicable_);
    // Same rhythm as the builder-made sections: header-to-row kSpace3 (pad included), section gap kSpace4.
    if (expanded) {
        invocationsBodyLayout_->setContentsMargins(0, kSpace3 - kSectionHeaderPad, 0, kSpace4);
    } else {
        invocationsBodyLayout_->setContentsMargins(0, 0, 0, 0);
    }
}

int InspectorPanel::debugInvocationEntryCount() const { return invocationEntries_.size(); }

QString InspectorPanel::debugInvocationEntryLabel(int index) const {
    if (index < 0 || index >= invocationEntries_.size()) {
        return QString();
    }
    return qobject_cast<QGroupBox*>(invocationEntries_.at(index).container)->title();
}

void InspectorPanel::debugSetInvocationPayloadInt(int index, int value) {
    if (index >= 0 && index < invocationEntries_.size()) {
        invocationEntries_.at(index).payloadIntSpin->setValue(value);
    }
}

void InspectorPanel::debugSetInvocationPayloadDouble(int index, double value) {
    if (index >= 0 && index < invocationEntries_.size()) {
        invocationEntries_.at(index).payloadDoubleSpin->setValue(value);
    }
}

void InspectorPanel::debugSetInvocationPayloadBool(int index, bool value) {
    if (index >= 0 && index < invocationEntries_.size()) {
        invocationEntries_.at(index).payloadBoolCheck->setChecked(value);
    }
}

void InspectorPanel::debugSetInvocationPayloadString(int index, const QString& text) {
    if (index >= 0 && index < invocationEntries_.size()) {
        invocationEntries_.at(index).payloadStringEdit->setText(text);
    }
}

QSpinBox* InspectorPanel::debugInvocationPayloadIntSpinBox(int index) const {
    return (index >= 0 && index < invocationEntries_.size()) ? invocationEntries_.at(index).payloadIntSpin : nullptr;
}

QDoubleSpinBox* InspectorPanel::debugInvocationPayloadDoubleSpinBox(int index) const {
    return (index >= 0 && index < invocationEntries_.size()) ? invocationEntries_.at(index).payloadDoubleSpin
                                                               : nullptr;
}

void InspectorPanel::debugSetInvocationErrorMessage(int index, const QString& text) {
    if (index >= 0 && index < invocationEntries_.size()) {
        invocationEntries_.at(index).errorMessageEdit->setText(text);
    }
}

void InspectorPanel::debugClickInvocationDone(int index) {
    if (index >= 0 && index < invocationEntries_.size()) {
        invocationEntries_.at(index).doneButton->click();
    }
}

void InspectorPanel::debugClickInvocationError(int index) {
    if (index >= 0 && index < invocationEntries_.size()) {
        invocationEntries_.at(index).errorButton->click();
    }
}

QRect InspectorPanel::debugInvocationEntryRegion(int index) const {
    if (index < 0 || index >= invocationEntries_.size()) {
        return QRect();
    }
    // The group box's title is its own label, so no label union is needed.
    QWidget* container = invocationEntries_.at(index).container;
    const QPoint topLeft = container->mapTo(const_cast<InspectorPanel*>(this), QPoint(0, 0));
    return QRect(topLeft, container->size()).adjusted(-8, -8, 8, 8);
}

QString InspectorPanel::debugInvocationsGuidanceText() const {
    return invocationsGuidance_->isVisible() ? invocationsGuidance_->text() : QString();
}

QWidget* InspectorPanel::debugInvocationsSectionWidget() const { return invocationsHeading_; }

QWidget* InspectorPanel::debugEventsSectionWidget() const { return eventsSectionContainer_; }

QWidget* InspectorPanel::debugGuardsSectionWidget() const { return guardsSectionContainer_; }

void InspectorPanel::debugClickEventsHeader() {
    if (eventsSectionHeader_ != nullptr) {
        eventsSectionHeader_->click();
    }
}

void InspectorPanel::debugClickGuardsHeader() {
    if (guardsSectionHeader_ != nullptr) {
        guardsSectionHeader_->click();
    }
}

bool InspectorPanel::debugEventsBodyVisible() const {
    return eventsSectionHeader_ != nullptr && eventsSectionHeader_->isChecked();
}

bool InspectorPanel::debugGuardsBodyVisible() const {
    return guardsSectionHeader_ != nullptr && guardsSectionHeader_->isChecked();
}

QString InspectorPanel::debugEventsHeaderText() const {
    return eventsSectionHeader_ != nullptr ? eventsSectionHeader_->text() : QString();
}

QString InspectorPanel::debugGuardsHeaderText() const {
    return guardsSectionHeader_ != nullptr ? guardsSectionHeader_->text() : QString();
}

bool InspectorPanel::debugEventsHeaderLeftAligned() const {
    // SectionHeaderButton is what achieves left-alignment; a plain dynamic_cast proves the header is one.
    return eventsSectionHeader_ != nullptr &&
           dynamic_cast<inspector_detail::SectionHeaderButton*>(eventsSectionHeader_) != nullptr;
}

int InspectorPanel::debugMachineFormLabelWidth() const {
    return machineNameLabel_ != nullptr ? machineNameLabel_->width() : -1;
}

int InspectorPanel::debugSimulationLabelWidth() const {
    return simCurrentStateLabel_ != nullptr ? simCurrentStateLabel_->width() : -1;
}

// Primary-ness is read from the stylesheet: kAccentBrand appears only in kPrimaryButtonStyle.
bool InspectorPanel::debugRunButtonIsPrimary() const {
    return runButton_ != nullptr && runButton_->styleSheet().contains(QString::fromLatin1(kAccentBrand));
}

bool InspectorPanel::debugPauseButtonIsPrimary() const {
    return pauseButton_ != nullptr && pauseButton_->styleSheet().contains(QString::fromLatin1(kAccentBrand));
}

bool InspectorPanel::debugResetButtonIsPrimary() const {
    return resetButton_ != nullptr && resetButton_->styleSheet().contains(QString::fromLatin1(kAccentBrand));
}

bool InspectorPanel::debugBackButtonIsPrimary() const {
    return backButton_ != nullptr && backButton_->styleSheet().contains(QString::fromLatin1(kAccentBrand));
}

QString InspectorPanel::debugCurrentCodeFile() const { return codeFileCombo_->currentData().toString(); }

void InspectorPanel::setStateFields(const QString& name, StateKind kind, bool isInitial,
                                     const QStringList& entryActions, const QStringList& exitActions,
                                     const QString& description, const QStringList& tags, bool historyDeep,
                                     QPointF pos, const QString& invokeSrc, const QString& invokeId,
                                     ContextType invokeOutputType,
                                     const QVector<Invocation>& invocations,
                                     ElementColor color) {
    stateNameEdit_->setText(name);  // QLineEdit::setText never emits editingFinished -- no loop
    stateKindCombo_->setCurrentIndex(stateKindCombo_->findData(static_cast<int>(kind)));  // activated() not emitted
    stateInitialCheck_->setChecked(isInitial);  // clicked() not emitted programmatically -- no loop
    // Row visible only for History; the check is set regardless so it is current when shown.
    historyDeepCheck_->setChecked(historyDeep);  // clicked() not emitted programmatically -- no loop
    stateForm_->setRowVisible(historyDeepCheck_, kind == StateKind::History);
    {
        const QSignalBlocker blocker(entryActionsEdit_);  // textChanged listeners (none today) stay quiet regardless
        entryActionsEdit_->setPlainText(joinEntryActions(entryActions));
    }
    {
        const QSignalBlocker blocker(exitActionsEdit_);
        exitActionsEdit_->setPlainText(joinEntryActions(exitActions));
    }
    {
        const QSignalBlocker blocker(descriptionEdit_);
        descriptionEdit_->setPlainText(description);
    }
    tagsEdit_->setText(joinTags(tags));  // setText never emits editingFinished -- no loop

    // Multi-invocations sync
    currentInvocations_ = invocations;
    if (currentInvocations_.isEmpty() && !invokeSrc.isEmpty()) {
        currentInvocations_ = {Invocation{.src = invokeSrc, .id = invokeId, .outputType = invokeOutputType}};
    }

    if (currentInvocations_.isEmpty()) {
        invokeSrcCurrent_.clear();
        invokeIdCurrent_.clear();
        invokeSrcEdit_->setText(QString());
        invokeIdEdit_->setText(QString());
        invokeIdEdit_->setPlaceholderText(QStringLiteral("(no service)"));
        invokeOutputTypeCombo_->setCurrentIndex(invokeOutputTypeCombo_->findData(static_cast<int>(invokeOutputType)));
        invokeIdEdit_->setEnabled(false);
        invokeOutputTypeCombo_->setEnabled(false);
    } else {
        invokeSrcCurrent_ = currentInvocations_[0].src;
        invokeIdCurrent_ = currentInvocations_[0].id;
        invokeSrcEdit_->setText(currentInvocations_[0].src);
        invokeIdEdit_->setText(currentInvocations_[0].id);
        invokeIdEdit_->setPlaceholderText(currentInvocations_[0].src.isEmpty() ? QStringLiteral("(no service)") : currentInvocations_[0].src);
        invokeOutputTypeCombo_->setCurrentIndex(invokeOutputTypeCombo_->findData(static_cast<int>(currentInvocations_[0].outputType)));
        const bool hasService = !currentInvocations_[0].src.isEmpty();
        invokeIdEdit_->setEnabled(hasService);
        invokeOutputTypeCombo_->setEnabled(hasService);
    }
    rebuildInvocationCards();

    statePositionValue_->setText(formatPosition(pos));
    if (stateColorRow_) {
        stateColorRow_->setColor(color);
    }
}

void InspectorPanel::rebuildInvocationCards() {
    invocationCardsDirty_ = true;
    scheduleInvocationCardsFlush();
}

void InspectorPanel::scheduleInvocationCardsFlush() {
    if (invocationCardsFlushScheduled_) {
        return;  // one flush per event-loop turn, however many facts arrive
    }
    invocationCardsFlushScheduled_ = true;
    QTimer::singleShot(0, this, &InspectorPanel::flushPendingInvocationCardsRebuild);
}

void InspectorPanel::flushPendingInvocationCardsRebuild() {
    invocationCardsFlushScheduled_ = false;
    if (!invocationCardsDirty_) {
        return;
    }
    invocationCardsDirty_ = false;
    rebuildInvocationCardsNow();
}

void InspectorPanel::rebuildInvocationCardsNow() {
    // currentInvocations_ is read at flush time, so it always draws the latest.
    clearLayoutWidgets(invocationsCardsLayout_);
    invocationCards_.clear();

    if (currentInvocations_.size() <= 1) {
        if (invocationsContainer_) {
            invocationsContainer_->setVisible(false);
        }
        return;
    }

    if (invocationsContainer_) {
        invocationsContainer_->setVisible(true);
    }

    for (int i = 1; i < currentInvocations_.size(); ++i) {
        auto* card = new QWidget(invocationsContainer_);
        card->setStyleSheet(resolveRoles(QStringLiteral("background-color: {surface-2}; border: 1px solid {outline-strong}; border-radius: 4px; padding: 4px;")));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(6, 6, 6, 6);
        cardLayout->setSpacing(4);

        auto* headerRow = new QHBoxLayout();
        auto* title = new QLabel(QStringLiteral("Service #%1").arg(i + 1), card);
        title->setStyleSheet(resolveRoles(QStringLiteral("font-weight: bold; color: {text-secondary};")));
        auto* removeBtn = new QPushButton(card);
        removeBtn->setIcon(icons::asset("remove"));
        removeBtn->setIconSize(QSize(12, 12));
        removeBtn->setFixedSize(20, 20);
        removeBtn->setStyleSheet(resolveRoles(QStringLiteral("QPushButton { color: {danger}; font-weight: bold; border: none; } QPushButton:hover { background-color: {danger-surface}; border-radius: 2px; }")));
        headerRow->addWidget(title);
        headerRow->addStretch();
        headerRow->addWidget(removeBtn);
        cardLayout->addLayout(headerRow);

        auto* cardForm = new QFormLayout();
        cardForm->setContentsMargins(0, 0, 0, 0);
        cardForm->setSpacing(4);

        auto* srcEdit = new QLineEdit(card);
        srcEdit->setText(currentInvocations_[i].src);
        srcEdit->setPlaceholderText(QStringLiteral("serviceName"));
        cardForm->addRow(QStringLiteral("Service"), srcEdit);

        auto* idEdit = new QLineEdit(card);
        idEdit->setText(currentInvocations_[i].id);
        idEdit->setPlaceholderText(currentInvocations_[i].src.isEmpty() ? QStringLiteral("(derive)") : currentInvocations_[i].src);
        cardForm->addRow(QStringLiteral("Id"), idEdit);

        auto* typeCombo = new QComboBox(card);
        typeCombo->addItem(QStringLiteral("Bool"), static_cast<int>(ContextType::Bool));
        typeCombo->addItem(QStringLiteral("Int"), static_cast<int>(ContextType::Int));
        typeCombo->addItem(QStringLiteral("Double"), static_cast<int>(ContextType::Double));
        typeCombo->addItem(QStringLiteral("String"), static_cast<int>(ContextType::String));
        typeCombo->setCurrentIndex(typeCombo->findData(static_cast<int>(currentInvocations_[i].outputType)));
        cardForm->addRow(QStringLiteral("Output type"), typeCombo);

        cardLayout->addLayout(cardForm);
        invocationsCardsLayout_->addWidget(card);

        InvocationCardWidgets cw;
        cw.card = card;
        cw.srcEdit = srcEdit;
        cw.idEdit = idEdit;
        cw.outputTypeCombo = typeCombo;
        cw.removeBtn = removeBtn;
        invocationCards_.push_back(cw);

        connect(removeBtn, &QPushButton::clicked, this, [this, i] {
            if (i < currentInvocations_.size()) {
                currentInvocations_.removeAt(i);
                emit invocationsEdited(currentInvocations_);
            }
        });
        connect(srcEdit, &QLineEdit::returnPressed, this, [this, i, srcEdit] {
            if (i < currentInvocations_.size()) {
                currentInvocations_[i].src = srcEdit->text();
                emit invocationsEdited(currentInvocations_);
            }
        });
        connect(idEdit, &QLineEdit::returnPressed, this, [this, i, idEdit] {
            if (i < currentInvocations_.size()) {
                currentInvocations_[i].id = idEdit->text();
                emit invocationsEdited(currentInvocations_);
            }
        });
        connect(typeCombo, &QComboBox::activated, this, [this, i, typeCombo](int idx) {
            if (i < currentInvocations_.size()) {
                currentInvocations_[i].outputType = static_cast<ContextType>(typeCombo->itemData(idx).toInt());
                emit invocationsEdited(currentInvocations_);
            }
        });
    }
}

void InspectorPanel::onAddInvocationClicked() {
    if (currentInvocations_.isEmpty()) {
        if (!invokeSrcEdit_->text().isEmpty()) {
            currentInvocations_.push_back(Invocation{
                .src = invokeSrcEdit_->text(),
                .id = invokeIdEdit_->text(),
                .outputType = static_cast<ContextType>(invokeOutputTypeCombo_->currentData().toInt())
            });
        } else {
            currentInvocations_.push_back(Invocation{
                .src = QStringLiteral("service1"),
                .id = QString(),
                .outputType = ContextType::Int
            });
        }
    }
    const int nextIdx = currentInvocations_.size() + 1;
    currentInvocations_.push_back(Invocation{
        .src = QStringLiteral("service%1").arg(nextIdx),
        .id = QString(),
        .outputType = ContextType::Int
    });
    emit invocationsEdited(currentInvocations_);
}

void InspectorPanel::setTransitionFields(const QString& event, const QString& guard, const QString& action,
                                          int delayMs, const QString& sourceName, const QString& targetName,
                                          bool reenter, bool always, const QString& payloadType,
                                          ElementColor color) {
    transitionEventEdit_->setText(event);
    if (transitionPayloadTypeEdit_) {
        transitionPayloadTypeEdit_->setText(payloadType);
    }
    transitionGuardEdit_->setText(guard);
    transitionActionEdit_->setText(action);
    transitionDelaySpin_->setValue(delayMs);  // setValue() never emits editingFinished -- no loop
    transitionSourceValue_->setText(sourceName);
    transitionTargetEdit_->setText(targetName);
    if (transitionReenterCheck_) {
        const QSignalBlocker blocker(transitionReenterCheck_);
        transitionReenterCheck_->setChecked(reenter);
    }
    if (transitionAlwaysCheck_) {
        const QSignalBlocker blocker(transitionAlwaysCheck_);
        transitionAlwaysCheck_->setChecked(always);
    }
    if (transitionColorRow_) {
        transitionColorRow_->setColor(color);
    }
}

void InspectorPanel::setStateCodeProjection(const NodeCodeProjection& projection) {
    if (stateCodePreview_) {
        stateCodePreview_->setProjection(projection);
    }
}

void InspectorPanel::setTransitionCodeProjection(const NodeCodeProjection& projection) {
    if (transitionCodePreview_) {
        transitionCodePreview_->setProjection(projection);
    }
}

void InspectorPanel::clearCodeProjections() {
    if (stateCodePreview_) {
        stateCodePreview_->clear();
    }
    if (transitionCodePreview_) {
        transitionCodePreview_->clear();
    }
}

void InspectorPanel::setNoteFields(const QString& text, ElementColor color) {
    if (noteTextEdit_) {
        const QSignalBlocker blocker(noteTextEdit_);
        noteTextEdit_->setPlainText(text);
    }
    if (noteColorRow_) {
        noteColorRow_->setColor(color);
    }
}

void InspectorPanel::setPayloadTypeSuggestions(const QStringList& suggestions) {
    if (payloadTypeSuggestionModel_) {
        payloadTypeSuggestionModel_->setStringList(suggestions);
    }
}

void InspectorPanel::debugTypePayloadType(const QString& text) {
    if (transitionPayloadTypeEdit_) {
        transitionPayloadTypeEdit_->setText(text);
        emit transitionPayloadTypeEditingFinished(text);
    }
}

QString InspectorPanel::debugPayloadTypeText() const {
    return transitionPayloadTypeEdit_ ? transitionPayloadTypeEdit_->text() : QString();
}

void InspectorPanel::setEventFeedback(EventFeedback severity, const QString& message) {
    if (eventFeedbackLabel_ == nullptr) {
        return;
    }
    if (severity == EventFeedback::None || message.isEmpty()) {
        eventFeedbackLabel_->clear();
        eventFeedbackLabel_->setVisible(false);
        return;
    }
    eventFeedbackLabel_->setStyleSheet(severity == EventFeedback::Error
                                            ? resolveRoles(QStringLiteral("color: {danger}; padding: 0px 2px 4px 2px;"))
                                            : resolveRoles(QStringLiteral("color: {text-secondary}; padding: 0px 2px 4px 2px;")));
    eventFeedbackLabel_->setText(message);
    eventFeedbackLabel_->setVisible(true);
}

void InspectorPanel::setGuardFeedback(GuardFeedback severity, const QString& message) {
    if (severity == GuardFeedback::None || message.isEmpty()) {
        guardFeedbackLabel_->clear();
        guardFeedbackLabel_->setVisible(false);
        return;
    }
    // Error vs Note differ by colour only, so a Note never reads as an alarm.
    guardFeedbackLabel_->setStyleSheet(severity == GuardFeedback::Error
                                            ? resolveRoles(QStringLiteral("color: {danger}; padding: 0px 2px 4px 2px;"))
                                            : resolveRoles(QStringLiteral("color: {text-secondary}; padding: 0px 2px 4px 2px;")));
    guardFeedbackLabel_->setText(message);
    guardFeedbackLabel_->setVisible(true);
}

void InspectorPanel::setActionFeedback(ActionFeedback severity, const QString& message) {
    if (severity == ActionFeedback::None || message.isEmpty()) {
        actionFeedbackLabel_->clear();
        actionFeedbackLabel_->setVisible(false);
        return;
    }
    actionFeedbackLabel_->setStyleSheet(severity == ActionFeedback::Error
                                            ? resolveRoles(QStringLiteral("color: {danger}; padding: 0px 2px 4px 2px;"))
                                            : resolveRoles(QStringLiteral("color: {text-secondary}; padding: 0px 2px 4px 2px;")));
    actionFeedbackLabel_->setText(message);
    actionFeedbackLabel_->setVisible(true);
}

void InspectorPanel::setTargetFeedback(TargetFeedback severity, const QString& message) {
    if (targetFeedbackLabel_ == nullptr) {
        return;
    }
    if (severity == TargetFeedback::None || message.isEmpty()) {
        targetFeedbackLabel_->clear();
        targetFeedbackLabel_->setVisible(false);
        return;
    }
    targetFeedbackLabel_->setStyleSheet(severity == TargetFeedback::Error
                                            ? resolveRoles(QStringLiteral("color: {danger}; padding: 0px 2px 4px 2px;"))
                                            : resolveRoles(QStringLiteral("color: {text-secondary}; padding: 0px 2px 4px 2px;")));
    targetFeedbackLabel_->setText(message);
    targetFeedbackLabel_->setVisible(true);
}

void InspectorPanel::setGuardSuggestions(const QStringList& guards) {
    if (guardSuggestionModel_->stringList() == guards) {
        return;  // rebuilding an identical model would close an open popup mid-typing
    }
    guardSuggestionModel_->setStringList(guards);
}

void InspectorPanel::debugTypeEvent(const QString& text) {
    transitionEventEdit_->setText(text);  // emits textChanged
}

QString InspectorPanel::debugEventFeedbackText() const {
    return eventFeedbackLabel_ && eventFeedbackLabel_->isVisible() ? eventFeedbackLabel_->text() : QString();
}

bool InspectorPanel::debugEventFeedbackIsError() const {
    return eventFeedbackLabel_ && eventFeedbackLabel_->isVisible() &&
           eventFeedbackLabel_->styleSheet().contains(QString::fromLatin1(kDanger));
}

void InspectorPanel::debugTypeTarget(const QString& text) {
    transitionTargetEdit_->setText(text);  // emits textChanged
}

QString InspectorPanel::debugTargetFeedbackText() const {
    return targetFeedbackLabel_ && targetFeedbackLabel_->isVisible() ? targetFeedbackLabel_->text() : QString();
}

bool InspectorPanel::debugTargetFeedbackIsError() const {
    return targetFeedbackLabel_ && targetFeedbackLabel_->isVisible() &&
           targetFeedbackLabel_->styleSheet().contains(QString::fromLatin1(kDanger));
}

void InspectorPanel::debugCommitTarget(const QString& text) {
    transitionTargetEdit_->setText(text);
    emit transitionTargetEditingFinished(text);
}

QString InspectorPanel::debugTargetText() const {
    return transitionTargetEdit_ ? transitionTargetEdit_->text() : QString();
}

void InspectorPanel::debugTypeGuard(const QString& text) {
    transitionGuardEdit_->setText(text);  // emits textChanged -- the real feedback path
}

QString InspectorPanel::debugGuardFeedbackText() const {
    return guardFeedbackLabel_->isVisible() ? guardFeedbackLabel_->text() : QString();
}

bool InspectorPanel::debugGuardFeedbackIsError() const {
    // Severity is not stored separately; the stylesheet is the state.
    return guardFeedbackLabel_->isVisible() && guardFeedbackLabel_->styleSheet().contains(QString::fromLatin1(kDanger));
}

void InspectorPanel::debugTypeAction(const QString& text) {
    transitionActionEdit_->setText(text);  // emits textChanged
}

QString InspectorPanel::debugActionFeedbackText() const {
    return actionFeedbackLabel_->isVisible() ? actionFeedbackLabel_->text() : QString();
}

bool InspectorPanel::debugActionFeedbackIsError() const {
    return actionFeedbackLabel_->isVisible() && actionFeedbackLabel_->styleSheet().contains(QString::fromLatin1(kDanger));
}

QStringList InspectorPanel::debugGuardSuggestions() const { return guardSuggestionModel_->stringList(); }

void InspectorPanel::setMachineFields(const QString& machineName, int stateCount, int transitionCount,
                                       bool simulateMode) {
    machineNameValue_->setText(machineName);
    machineCountsValue_->setText(QStringLiteral("%1 / %2").arg(stateCount).arg(transitionCount));
    machineModeValue_->setText(simulateMode ? QStringLiteral("Simulate") : QStringLiteral("Design"));
}

void InspectorPanel::setSimulationChip(const QString& text) { simulationChip_->setText(text); }

void InspectorPanel::setTimescale(double scale) {
    if (!timescaleCombo_) {
        return;
    }
    for (int i = 0; i < timescaleCombo_->count(); ++i) {
        if (qFuzzyCompare(timescaleCombo_->itemData(i).toDouble(), scale)) {
            const QSignalBlocker blocker(timescaleCombo_);
            timescaleCombo_->setCurrentIndex(i);
            return;
        }
    }
}

void InspectorPanel::debugSetTimescale(double scale) {
    setTimescale(scale);
    emit timescaleChanged(scale);
}

double InspectorPanel::debugTimescale() const {
    return timescaleCombo_ ? timescaleCombo_->currentData().toDouble() : 1.0;
}

void InspectorPanel::setSimTransportState(bool runEnabled, bool pauseEnabled, bool resetEnabled, bool backEnabled) {
    runButton_->setEnabled(runEnabled);
    pauseButton_->setEnabled(pauseEnabled);
    resetButton_->setEnabled(resetEnabled);
    backButton_->setEnabled(backEnabled);
}

void InspectorPanel::setEventVocabulary(const QVector<QPair<QString, bool>>& events) {
    QVector<EventVocabularyItem> items;
    items.reserve(events.size());
    for (const auto& pair : events) {
        items.push_back(EventVocabularyItem{.name = pair.first, .fireable = pair.second});
    }
    setEventVocabulary(items);
}

void InspectorPanel::setEventVocabulary(const QVector<EventVocabularyItem>& events) {
    clearLayoutWidgets(eventVocabularyLayout_);
    eventPayloadEdits_.clear();
    // The count lives in the header text, refreshed only here. Use
    // setSectionHeaderTitle(), not setText(): it owns the chevron prefix.
    if (eventsSectionHeader_ != nullptr) {
        setSectionHeaderTitle(eventsSectionHeader_, QStringLiteral("Events (%1)").arg(events.size()));
    }
    if (events.isEmpty()) {
        auto* emptyLabel = new QLabel(QStringLiteral("No transition events"), this);
        emptyLabel->setStyleSheet(resolveRoles(QStringLiteral("color: {text-disabled}; font-size: 11px; font-style: italic;")));
        eventVocabularyLayout_->addWidget(emptyLabel);
        return;
    }
    for (const auto& item : events) {
        const QString name = item.name;
        auto* rowWidget = new QWidget();
        auto* rowLayout = new QVBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 2);
        rowLayout->setSpacing(2);

        auto* btnRow = new QHBoxLayout();
        btnRow->setContentsMargins(0, 0, 0, 0);
        btnRow->setSpacing(4);

        auto* button = new QPushButton(name);
        button->setIcon(icons::asset("play"));
        button->setEnabled(item.fireable);
        button->setMinimumHeight(26);
        button->setCursor(item.fireable ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (item.fireable) {
            button->setStyleSheet(resolveRoles(QStringLiteral(
                "QPushButton { background: {surface-2}; border: 1px solid {accent-interactive}; color: {text-primary}; border-radius: 4px; padding: 4px 8px; text-align: left; font-weight: 500; }"
                "QPushButton:hover { background: {accent-interactive}; color: {text-on-accent}; }"
                "QPushButton:pressed { background: {accent-brand}; }")));
        } else {
            button->setStyleSheet(resolveRoles(QStringLiteral(
                "QPushButton { background: {surface-2}; border: 1px solid {outline-strong}; color: {text-disabled}; border-radius: 4px; padding: 4px 8px; text-align: left; }")));
        }
        if (!item.fireable) {
            button->setToolTip(
                QStringLiteral("Fires from a state with an outgoing '%1' transition").arg(name));
        }
        btnRow->addWidget(button, 1);

        if (!item.payloadType.isEmpty()) {
            auto* badge = new QLabel(QStringLiteral("[%1]").arg(item.payloadType));
            badge->setStyleSheet(resolveRoles(QStringLiteral("color: {code-type}; font-size: 11px; font-weight: 600; padding: 0 4px;")));
            btnRow->addWidget(badge, 0);
        }
        rowLayout->addLayout(btnRow);

        auto* payloadEdit = new QLineEdit();
        payloadEdit->setPlaceholderText(item.payloadType.isEmpty()
            ? QStringLiteral("payload (optional)")
            : QStringLiteral("payload: %1 (JSON or scalar)").arg(item.payloadType));
        payloadEdit->setStyleSheet(resolveRoles(QStringLiteral(
            "QLineEdit { background: {surface-2}; border: 1px solid {outline}; color: {text-primary}; border-radius: 3px; font-size: 11px; padding: 2px 4px; }")));
        rowLayout->addWidget(payloadEdit);
        eventPayloadEdits_[name] = payloadEdit;

        connect(button, &QPushButton::clicked, this, [this, name, payloadEdit] {
            const QString text = payloadEdit->text().trimmed();
            QVariant payload;
            if (!text.isEmpty()) {
                QJsonParseError err;
                const QJsonDocument jsonDoc = QJsonDocument::fromJson(text.toUtf8(), &err);
                if (err.error == QJsonParseError::NoError) {
                    payload = jsonDoc.toVariant();
                } else if (text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) {
                    payload = true;
                } else if (text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) {
                    payload = false;
                } else {
                    bool okInt = false;
                    qlonglong iVal = text.toLongLong(&okInt, 0);
                    if (okInt) {
                        payload = iVal;
                    } else {
                        bool okDbl = false;
                        double dVal = text.toDouble(&okDbl);
                        if (okDbl) {
                            payload = dVal;
                        } else {
                            payload = text;
                        }
                    }
                }
            }
            emit sendEventClicked(name, payload);
        });
        eventVocabularyLayout_->addWidget(rowWidget);
    }
}

void InspectorPanel::debugSetEventPayload(const QString& name, const QString& payloadText) {
    if (eventPayloadEdits_.contains(name) && eventPayloadEdits_[name] != nullptr) {
        eventPayloadEdits_[name]->setText(payloadText);
    }
}

void InspectorPanel::debugClickSendEvent(const QString& name) {
    if (eventPayloadEdits_.contains(name)) {
        const QString text = eventPayloadEdits_[name]->text().trimmed();
        QVariant payload;
        if (!text.isEmpty()) {
            QJsonParseError err;
            const QJsonDocument jsonDoc = QJsonDocument::fromJson(text.toUtf8(), &err);
            if (err.error == QJsonParseError::NoError) {
                payload = jsonDoc.toVariant();
            } else if (text.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) {
                payload = true;
            } else if (text.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) {
                payload = false;
            } else {
                bool okInt = false;
                qlonglong iVal = text.toLongLong(&okInt, 0);
                if (okInt) {
                    payload = iVal;
                } else {
                    bool okDbl = false;
                    double dVal = text.toDouble(&okDbl);
                    if (okDbl) {
                        payload = dVal;
                    } else {
                        payload = text;
                    }
                }
            }
        }
        emit sendEventClicked(name, payload);
    }
}

QString InspectorPanel::debugEventPayloadText(const QString& name) const {
    return eventPayloadEdits_.contains(name) && eventPayloadEdits_[name] != nullptr
               ? eventPayloadEdits_[name]->text()
               : QString();
}

namespace {
constexpr int kMaxGuardDisplayChars = 32;
QString elideGuardDisplay(const QString& text) {
    if (text.size() <= kMaxGuardDisplayChars) {
        return text;
    }
    return text.left(kMaxGuardDisplayChars - 1) + QChar(0x2026);
}
}  // namespace

void InspectorPanel::setGuardVocabulary(const QVector<QPair<QString, bool>>& guards) {
    clearLayoutWidgets(guardVocabularyLayout_);
    // Count in the header text, as in setEventVocabulary().
    if (guardsSectionHeader_ != nullptr) {
        setSectionHeaderTitle(guardsSectionHeader_, QStringLiteral("Guard Results (%1)").arg(guards.size()));
    }
    for (const auto& guard : guards) {
        const QString name = guard.first;
        auto* checkbox = new QCheckBox(elideGuardDisplay(name));
        checkbox->setChecked(guard.second);  // before connecting -- see header comment on why this can't loop
        checkbox->setMinimumHeight(20);
        checkbox->setToolTip(name);
        connect(checkbox, &QCheckBox::toggled, this, [this, name](bool checked) { emit guardToggled(name, checked); });
        guardVocabularyLayout_->addWidget(checkbox);
    }
}

void InspectorPanel::setGeneratedFiles(const QVector<GeneratedFile>& files) {
    const QString previousPath = codeFileCombo_->currentIndex() >= 0 ? codeFileCombo_->currentData().toString() : QString();

    generatedFiles_ = files;
    {
        const QSignalBlocker blocker(codeFileCombo_);
        codeFileCombo_->clear();
        for (const GeneratedFile& file : generatedFiles_) {
            codeFileCombo_->addItem(file.relativePath, file.relativePath);
        }
        if (codeFileCombo_->count() > 0) {
            const int previousIndex = codeFileCombo_->findData(previousPath);
            if (previousIndex >= 0) {
                codeFileCombo_->setCurrentIndex(previousIndex);
            } else {
                // First open: default to the commands file, which lists states,
                // events, guards, actions and targets in one place.
                int commandsIndex = 0;
                for (int i = 0; i < codeFileCombo_->count(); ++i) {
                    if (codeFileCombo_->itemData(i).toString().endsWith(QStringLiteral("_commands.h"))) {
                        commandsIndex = i;
                        break;
                    }
                }
                codeFileCombo_->setCurrentIndex(commandsIndex);
            }
        }
    }
    updateCodePreview();
}

void InspectorPanel::showCodeTabForFile(const QString& fileNameSuffix) {
    stackedWidget_->setCurrentIndex(kCodePageIndex);
    for (int i = 0; i < generatedFiles_.size(); ++i) {
        if (generatedFiles_.at(i).relativePath.endsWith(fileNameSuffix)) {
            codeFileCombo_->setCurrentIndex(i);
            break;
        }
    }
    updateCodePreview();
}

void InspectorPanel::updateCodePreview() {
    const int index = codeFileCombo_->currentIndex();
    codePreviewEdit_->setPlainText(index >= 0 && index < generatedFiles_.size() ? generatedFiles_.at(index).content
                                                                                 : QString());
}

// Shows the empty page so a closed machine's stale fields aren't left visible.
// Nothing else is reset: the next adapter bind repopulates every field.
void InspectorPanel::clearMachine() {
    stackedWidget_->setCurrentIndex(kEmptyPageIndex);
}

bool InspectorPanel::debugIsShowingEmptyState() const {
    return stackedWidget_->currentIndex() == kEmptyPageIndex;
}

void InspectorPanel::showTabForSelection(SelectionKind kind) {
    switch (kind) {
        case SelectionKind::State:
            stackedWidget_->setCurrentIndex(kStatePageIndex);
            break;
        case SelectionKind::Transition:
            stackedWidget_->setCurrentIndex(kTransitionPageIndex);
            break;
        case SelectionKind::Note:
            stackedWidget_->setCurrentIndex(kNotePageIndex);
            break;
        case SelectionKind::None:
        case SelectionKind::Frame:
        case SelectionKind::Multi:
            stackedWidget_->setCurrentIndex(kMachinePageIndex);
            break;
    }
}

void InspectorPanel::focusField(InspectorField field) {
    switch (field) {
        case InspectorField::EntryActions:
            entryActionsEdit_->setFocus();
            break;
        case InspectorField::ExitActions:
            exitActionsEdit_->setFocus();
            break;
        case InspectorField::Description:
            descriptionEdit_->setFocus();
            break;
        case InspectorField::Tags:
            tagsEdit_->setFocus();
            break;
        case InspectorField::TransitionGuard:
            transitionGuardEdit_->setFocus();
            break;
        case InspectorField::TransitionAction:
            transitionActionEdit_->setFocus();
            break;
        case InspectorField::TransitionDelay:
            transitionDelaySpin_->setFocus();
            break;
    }
}

bool InspectorPanel::eventFilter(QObject* watched, QEvent* event) {
    // QPlainTextEdit has no editingFinished() signal (QLineEdit/
    // QAbstractSpinBox/QComboBox-only) -- FocusOut is these widgets'
    // equivalent "the user is done typing" moment.
    if (event->type() == QEvent::FocusOut) {
        if (watched == entryActionsEdit_) {
            emit entryActionsEditingFinished(splitEntryActions(entryActionsEdit_->toPlainText()));
        } else if (watched == exitActionsEdit_) {
            emit exitActionsEditingFinished(splitEntryActions(exitActionsEdit_->toPlainText()));
        } else if (watched == descriptionEdit_) {
            emit descriptionEditingFinished(descriptionEdit_->toPlainText());
        } else if (watched == noteTextEdit_) {
            emit noteTextEditingFinished(noteTextEdit_->toPlainText());
        }
        // invokeSrcEdit_/invokeIdEdit_ commit on Enter only, not here.
    } else if (event->type() == QEvent::KeyPress) {
        // Escape reverts Service/Id to the last displayed value.
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            if (watched == invokeSrcEdit_) {
                invokeSrcEdit_->setText(invokeSrcCurrent_);
                return true;
            }
            if (watched == invokeIdEdit_) {
                invokeIdEdit_->setText(invokeIdCurrent_);
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void InspectorPanel::clearLayoutWidgets(QVBoxLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void InspectorPanel::debugSetStateColor(ElementColor color) {
    if (stateColorRow_) {
        stateColorRow_->setColor(color);
        emit stateColorEdited(color);
    }
}

void InspectorPanel::debugSetTransitionColor(ElementColor color) {
    if (transitionColorRow_) {
        transitionColorRow_->setColor(color);
        emit transitionColorEdited(color);
    }
}

void InspectorPanel::debugSetNoteColor(ElementColor color) {
    if (noteColorRow_) {
        noteColorRow_->setColor(color);
        emit noteColorEdited(color);
    }
}

ElementColor InspectorPanel::debugStateColor() const {
    return stateColorRow_ ? stateColorRow_->color() : ElementColor::Default;
}

ElementColor InspectorPanel::debugTransitionColor() const {
    return transitionColorRow_ ? transitionColorRow_->color() : ElementColor::Default;
}

ElementColor InspectorPanel::debugNoteColor() const {
    return noteColorRow_ ? noteColorRow_->color() : ElementColor::Default;
}

void InspectorPanel::debugCommitNoteText(const QString& text) {
    if (noteTextEdit_) {
        noteTextEdit_->setPlainText(text);
        emit noteTextEditingFinished(text);
    }
}

QString InspectorPanel::debugNoteText() const {
    return noteTextEdit_ ? noteTextEdit_->toPlainText() : QString();
}

}  // namespace app
