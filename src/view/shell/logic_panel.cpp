#include "view/shell/logic_panel.h"

#include <QBoxLayout>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <QIntValidator>
#include "view/shell/icons.h"
#include "view/shell/struct_editor_dialog.h"

namespace app {

using namespace inspector_detail;  // CollapsibleSection/buildCollapsibleSection()

namespace {

class IntEditEventFilter : public QObject {
public:
    IntEditEventFilter(QLineEdit* edit, std::function<void(const QString&)> onCommit, QObject* parent = nullptr)
        : QObject(parent), edit_(edit), onCommit_(std::move(onCommit)) {}

    void setEnabled(bool enabled) { enabled_ = enabled; }
    bool isEnabled() const { return enabled_; }

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (enabled_ && watched == edit_ && event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            int delta = 0;
            if (ke->key() == Qt::Key_Up) {
                delta = 1;
            } else if (ke->key() == Qt::Key_Down) {
                delta = -1;
            } else if (ke->key() == Qt::Key_PageUp) {
                delta = 10;
            } else if (ke->key() == Qt::Key_PageDown) {
                delta = -10;
            }
            if (delta != 0) {
                bool ok = false;
                qint64 val = edit_->text().trimmed().toLongLong(&ok);
                if (!ok) {
                    val = 0;
                }
                val += delta;
                edit_->setText(QString::number(val));
                edit_->selectAll();
                if (onCommit_) {
                    onCommit_(edit_->text());
                }
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QLineEdit* edit_ = nullptr;
    std::function<void(const QString&)> onCommit_;
    bool enabled_ = true;
};


// Row button style: left-aligned, quiet, highlights on hover. A QToolButton
// because it has no border/frame by default under Fusion.
const QString kRowButtonStyle = resolveRoles(QStringLiteral(
    "QToolButton { color: {text-secondary}; font-size: 11px; padding: 3px 6px; "
    "text-align: left; border: none; background: {surface-2}; border-radius: 2px; }"
    "QToolButton:hover { background: {surface-hover}; color: {text-primary}; }"));
// Editable cells (QLineEdit/QComboBox); tight padding keeps four cells per row
// from raising the row's minimum width.
const QString kRowFieldStyle = resolveRoles(QStringLiteral(
    "QLineEdit, QComboBox { background: {surface-2}; color: {text-secondary}; font-size: 11px; "
    "border: 1px solid {outline}; border-radius: 2px; padding: 1px 3px; min-height: 18px; }"
    "QLineEdit:focus, QComboBox:focus { border-color: {outline-focus}; }"));

// Live overlay: thin monospace chips for run-time values, dimmed when
// undecided ("?") and colored on a decided boolean.
const QString kLiveValueStyle = resolveRoles(QStringLiteral(
    "QLabel { color: {success}; font-size: 10px; font-family: monospace; "
    "padding: 1px 4px; background: {success-surface}; border-radius: 2px; }"));

// "Logic -- <suffix>", built from QChar(0x2014) rather than a literal em dash
// so source-encoding drift cannot corrupt it.
QString logicHeaderText(const QString& suffix) {
    return QStringLiteral("Logic ") + QChar(0x2014) + QStringLiteral(" ") + suffix;
}

// Floor width the panel requests from its parent, so it cannot collapse to
// zero when the splitter handle is dragged left.
constexpr int kMinimumPanelWidth = 120;

// Row display names longer than this are elided, so a long name never widens
// the side bar. Character truncation, not elidedText(): headless probes have no
// reliable font metrics.
constexpr int kMaxRowNameChars = 24;

QString elideRowName(const QString& name) {
    if (name.length() <= kMaxRowNameChars) {
        return name;
    }
    return name.left(kMaxRowNameChars - 1) + QChar(0x2026);  // ellipsis codepoint, not a literal "..."
}

icons::IconId rowClassToIconId(LogicRowClass rowClass) {
    switch (rowClass) {
        case LogicRowClass::Hook:
            return icons::IconId::Hook;
        case LogicRowClass::InlineExpression:
            return icons::IconId::Expr;
        case LogicRowClass::Assign:
            return icons::IconId::Assign;
        case LogicRowClass::Raise:
            return icons::IconId::Raise;
        case LogicRowClass::SendTo:
            return icons::IconId::SendTo;
        case LogicRowClass::SendParent:
            return icons::IconId::SendParent;
    }
    return icons::IconId::Hook;
}

QString classBadge(LogicRowClass rowClass) {
    switch (rowClass) {
        case LogicRowClass::Hook:
            return QStringLiteral("Hook");
        case LogicRowClass::InlineExpression:
            return QStringLiteral("Expr");
        case LogicRowClass::Assign:
            return QStringLiteral("Assign");
        case LogicRowClass::Raise:
            return QStringLiteral("Raise");
        case LogicRowClass::SendTo:
            return QStringLiteral("SendTo");
        case LogicRowClass::SendParent:
            return QStringLiteral("SendParent");
    }
    return QString();
}

QString classTooltip(LogicRowClass rowClass, const QString& machineName) {
    switch (rowClass) {
        case LogicRowClass::Hook:
            return QStringLiteral("Hook -- emits into ") + (machineName.isEmpty() ? QStringLiteral("<Machine>") : machineName) + QStringLiteral("Hooks.h");
        case LogicRowClass::InlineExpression:
            return QStringLiteral("Inline expression -- compiles directly, no hook");
        case LogicRowClass::Assign:
            return QStringLiteral("Assignment -- sets context directly");
        case LogicRowClass::Raise:
            return QStringLiteral("Raise -- enqueues event onto internal microstep queue");
        case LogicRowClass::SendTo:
            return QStringLiteral("SendTo -- sends event to an invoked actor");
        case LogicRowClass::SendParent:
            return QStringLiteral("SendParent -- sends event to the parent machine");
    }
    return QString();
}

QString logicRowLabel(const LogicRow& row) {
    const int refCount = row.transitionIds.size() + row.stateIds.size();
    return QStringLiteral("%1  ").arg(elideRowName(row.name)) + QChar(0x00D7) +
           QString::number(refCount);
}

// The Actors row badge: the declared output type its onDone payload resolves
// to. No default branch, so a new ContextType enumerator fails to compile
// rather than mislabel a row.
QString outputTypeBadge(ContextType type) {
    switch (type) {
        case ContextType::Bool:
            return QStringLiteral("Bool");
        case ContextType::Int:
            return QStringLiteral("Int");
        case ContextType::Double:
            return QStringLiteral("Double");
        case ContextType::String:
            return QStringLiteral("String");
        case ContextType::Object:
            return QStringLiteral("Object");
    }
    return QString();  // unreachable -- every enumerator handled above
}

// One Actors row's button text: output-type badge, elided service name, the
// effective id only when it differs from src, then the owning state's name.
// No reference count: an invoke belongs to exactly one state.
QString actorRowLabel(const ActorRow& row) {
    const QString idSuffix = (!row.effectiveId.isEmpty() && row.effectiveId != row.src)
                                  ? QStringLiteral(" #") + elideRowName(row.effectiveId)
                                  : QString();
    return QStringLiteral("[%1] %2%3  in %4")
        .arg(outputTypeBadge(row.outputType), elideRowName(row.src), idSuffix, elideRowName(row.stateName));
}

// The badge's tooltip: the onDone/onError event names a referencing transition
// would use, kept off the row text.
QString actorRowTooltip(const ActorRow& row) {
    return QStringLiteral("Invoked by ") + row.stateName + QStringLiteral(" as \"") + row.effectiveId +
           QStringLiteral("\" -- done.invoke.") + row.effectiveId + QStringLiteral(" / error.platform.") +
           row.effectiveId;
}

}  // namespace

LogicPanel::LogicPanel(QWidget* parent) : QWidget(parent) {
    // A side-bar section must not dictate the side bar's width: unconstrained
    // labels reported a ~216px minimum that widened the whole side bar. Word wrap
    // plus this floor keep the panel a passenger of the width it is given.
    setMinimumWidth(kMinimumPanelWidth);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // No section-to-section spacing: each section carries its own gap, which
    // centers a collapsed header between its hairline and the next.
    layout->setSpacing(0);

    // Fixed section order. Only Context has a "+": it is the only stored
    // section; guards, actions and actors are derived views, and the panel
    // renders without owning.
    sections_.push_back(buildContextSection());
    sections_.push_back(buildLogicListSection(QStringLiteral("Guards"),
                                               QStringLiteral("No guards yet. Set a Guard on a transition."),
                                               &guardPlaceholder_, &guardRowsContainer_, &guardRowsLayout_,
                                               &guardRowsScroll_, /*defaultExpanded=*/false, /*iconSlug=*/"expr"));
    sections_.push_back(buildLogicListSection(QStringLiteral("Actions"),
                                               QStringLiteral("No actions yet. Set an Action on a transition, "
                                                              "or an entry/exit action on a state."),
                                               &actionPlaceholder_, &actionRowsContainer_, &actionRowsLayout_,
                                               &actionRowsScroll_, /*defaultExpanded=*/false, /*iconSlug=*/"raise"));
    sections_.push_back(buildLogicListSection(QStringLiteral("Actors"),
                                               QStringLiteral("No actors yet. Set an Invoke on a state."),
                                               &actorPlaceholder_, &actorRowsContainer_, &actorRowsLayout_,
                                               &actorRowsScroll_, /*defaultExpanded=*/false, /*iconSlug=*/"send-to"));
    sections_.push_back(buildTypesSection());
    for (const Section& section : sections_) {
        layout->addWidget(section.container);
    }
    layout->addStretch(1);

    clearMachine();  // boot state -- no LogicAdapter attaches until MainWindow focuses a pane
}

LogicPanel::Section LogicPanel::buildContextSection() {
    Section section = buildCollapsibleSection(this, QStringLiteral("Context"),
                                              QStringLiteral("No context variables yet."), /*enabled=*/true,
                                              /*defaultExpanded=*/true, /*iconSlug=*/"assign");

    // The "+" affordance: a child of the header, left of its chevron.
    contextAddButton_ = makeSectionHeaderAction(section, "add", QStringLiteral("Add context variable"));
    connect(contextAddButton_, &QToolButton::clicked, this, &LogicPanel::contextAddClicked);

    // Reuse the placeholder label buildCollapsibleSection() added, then append
    // the rows table; setContextVariables() toggles which of the two is visible.
    auto* bodyLayout = qobject_cast<QVBoxLayout*>(section.body->layout());
    contextPlaceholder_ = qobject_cast<QLabel*>(bodyLayout->itemAt(0)->widget());

    contextRowsContainer_ = new QWidget();
    contextRowsLayout_ = new QGridLayout(contextRowsContainer_);
    // Tight margins/spacing: four editable cells per row cannot fit under the
    // 120px floor anyway, so contextRowsScroll_ below is the real fix.
    contextRowsLayout_->setContentsMargins(4, 0, 4, 4);
    contextRowsLayout_->setHorizontalSpacing(4);
    contextRowsLayout_->setVerticalSpacing(2);
    // Only name (0) and value (2) grow, splitting the spare width, so the name
    // field gets a guaranteed share instead of being truncated.
    contextRowsLayout_->setColumnStretch(0, 1);  // name
    contextRowsLayout_->setColumnStretch(1, 0);  // type combo
    contextRowsLayout_->setColumnStretch(2, 1);  // initial value
    contextRowsLayout_->setColumnStretch(3, 0);  // highlight icon gutter
    contextRowsLayout_->setColumnStretch(4, 0);  // delete icon gutter
    contextRowsLayout_->setColumnStretch(5, 0);  // live value chip

    // Wrapped in a QScrollArea so the grid's ever-growing natural width never
    // becomes the panel's minimumSizeHint (a scroll area reports only its
    // frame/scrollbar floor).
    contextRowsScroll_ = new QScrollArea(section.body);
    contextRowsScroll_->setWidget(contextRowsContainer_);
    contextRowsScroll_->setWidgetResizable(true);
    contextRowsScroll_->setFrameShape(QFrame::NoFrame);
    contextRowsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    contextRowsScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    bodyLayout->addWidget(contextRowsScroll_);
    contextRowsScroll_->setVisible(false);  // setContextVariables() flips this once a row exists

    return section;
}

LogicPanel::Section LogicPanel::buildLogicListSection(const QString& title, const QString& placeholderText,
                                                        QLabel** placeholderOut, QWidget** rowsContainerOut,
                                                        QVBoxLayout** rowsLayoutOut, QScrollArea** rowsScrollOut,
                                                        bool defaultExpanded, const char* iconSlug) {
    Section section = buildCollapsibleSection(this, title, placeholderText, /*enabled=*/true, defaultExpanded, iconSlug);

    // Reuse the placeholder label buildCollapsibleSection() added.
    auto* bodyLayout = qobject_cast<QVBoxLayout*>(section.body->layout());
    *placeholderOut = qobject_cast<QLabel*>(bodyLayout->itemAt(0)->widget());

    QWidget* rowsContainer = new QWidget();
    auto* rowsLayout = new QVBoxLayout(rowsContainer);
    rowsLayout->setContentsMargins(4, 0, 4, 4);
    rowsLayout->setSpacing(2);

    // Scroll area so a long row list or row never raises the panel's minimum width.
    QScrollArea* scroll = new QScrollArea(section.body);
    scroll->setWidget(rowsContainer);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    bodyLayout->addWidget(scroll);
    scroll->setVisible(false);  // setGuardRows()/setActionRows() flips this once a row exists

    *rowsContainerOut = rowsContainer;
    *rowsLayoutOut = rowsLayout;
    *rowsScrollOut = scroll;
    return section;
}

void LogicPanel::setMachineName(const QString& machineName) {
    machineName_ = machineName;
}

void LogicPanel::clearMachine() { machineName_.clear(); }

// Probe lever: the header string computed from machineName_ (empty exactly
// when clearMachine() ran last), since no header widget is rendered.
QString LogicPanel::debugHeaderText() const {
    return logicHeaderText(machineName_.isEmpty() ? QStringLiteral("no machine focused") : machineName_);
}

QStringList LogicPanel::debugSectionTitles() const {
    QStringList titles;
    for (const Section& section : sections_) {
        titles.push_back(section.header->text());
    }
    return titles;
}

void LogicPanel::debugClickSectionHeader(int index) {
    if (index < 0 || index >= static_cast<int>(sections_.size())) {
        return;
    }
    sections_[static_cast<std::size_t>(index)].header->click();
}

bool LogicPanel::debugSectionBodyVisible(int index) const {
    if (index < 0 || index >= static_cast<int>(sections_.size())) {
        return false;
    }
    return sections_[static_cast<std::size_t>(index)].body->isVisible();
}

QWidget* LogicPanel::debugSectionWidget(int index) const {
    if (index < 0 || index >= static_cast<int>(sections_.size())) {
        return nullptr;
    }
    return sections_[static_cast<std::size_t>(index)].container;
}

void LogicPanel::debugClickContextAdd() { contextAddButton_->click(); }

int LogicPanel::debugContextRowCount() const { return static_cast<int>(contextRows_.size()); }

ContextVariable LogicPanel::debugContextRow(int index) const {
    if (index < 0 || index >= static_cast<int>(contextRows_.size())) {
        return ContextVariable{};
    }
    const ContextRowWidgets& row = contextRows_[static_cast<std::size_t>(index)];
    ContextVariable variable;
    variable.id = row.id;
    variable.name = row.nameEdit->text();
    variable.type = static_cast<ContextType>(row.typeCombo->currentData().toInt());
    variable.initialValue = row.valueEdit->text();
    return variable;
}

void LogicPanel::debugCommitContextName(int index, const QString& name) {
    if (index < 0 || index >= static_cast<int>(contextRows_.size())) {
        return;
    }
    const ContextRowWidgets& row = contextRows_[static_cast<std::size_t>(index)];
    row.nameEdit->setText(name);  // QLineEdit::setText never emits editingFinished -- no loop
    emit contextNameEditingFinished(row.id, name);
}

void LogicPanel::debugSetContextType(int index, ContextType type) {
    if (index < 0 || index >= static_cast<int>(contextRows_.size())) {
        return;
    }
    const ContextRowWidgets& row = contextRows_[static_cast<std::size_t>(index)];
    row.typeCombo->setCurrentIndex(row.typeCombo->findData(static_cast<int>(type)));  // activated() not emitted
    emit contextTypeActivated(row.id, type);
}

void LogicPanel::debugSetContextCustomTypeName(int index, const QString& customTypeName) {
    if (index < 0 || index >= static_cast<int>(contextRows_.size())) {
        return;
    }
    const ContextRowWidgets& row = contextRows_[static_cast<std::size_t>(index)];
    int idx = row.typeCombo->findData(customTypeName);
    if (idx >= 0) {
        row.typeCombo->setCurrentIndex(idx);
    }
    emit contextTypeActivated(row.id, ContextType::Object);
    emit contextCustomTypeNameActivated(row.id, customTypeName);
}

void LogicPanel::debugCommitContextInitialValue(int index, const QString& initialValue) {
    if (index < 0 || index >= static_cast<int>(contextRows_.size())) {
        return;
    }
    const ContextRowWidgets& row = contextRows_[static_cast<std::size_t>(index)];
    row.valueEdit->setText(initialValue);
    emit contextInitialValueEditingFinished(row.id, initialValue);
}

void LogicPanel::debugClickContextDelete(int index) {
    if (index < 0 || index >= static_cast<int>(contextRows_.size())) {
        return;
    }
    contextRows_[static_cast<std::size_t>(index)].deleteButton->click();
}

void LogicPanel::setHighlightedVariable(const QString& varName) {
    for (ContextRowWidgets& row : contextRows_) {
        if (row.traceButton != nullptr) {
            row.traceButton->setChecked(!varName.isEmpty() && row.name == varName);
        }
    }
}

void LogicPanel::debugClickContextTrace(int index) {
    if (index < 0 || index >= static_cast<int>(contextRows_.size())) {
        return;
    }
    if (contextRows_[static_cast<std::size_t>(index)].traceButton != nullptr) {
        contextRows_[static_cast<std::size_t>(index)].traceButton->click();
    }
}

bool LogicPanel::debugContextTraceActive(int index) const {
    if (index < 0 || index >= static_cast<int>(contextRows_.size())) {
        return false;
    }
    return contextRows_[static_cast<std::size_t>(index)].traceButton != nullptr &&
           contextRows_[static_cast<std::size_t>(index)].traceButton->isChecked();
}


bool LogicPanel::debugContextPlaceholderVisible() const {
    return contextPlaceholder_ != nullptr && !contextPlaceholder_->isHidden();
}

void LogicPanel::setContextVariablesNow(const QVector<ContextVariable>& variables) {
    // Wholesale rebuild every call: cheap at this size, and rows mirror
    // Machine::context's document order 1:1 with no diffing.
    while (QLayoutItem* item = contextRowsLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    contextRows_.clear();

    contextPlaceholder_->setVisible(variables.isEmpty());
    contextRowsScroll_->setVisible(!variables.isEmpty());

    int row = 0;
    for (const ContextVariable& variable : variables) {
        ContextRowWidgets widgets;
        widgets.id = variable.id;
        const quint64 id = variable.id;  // captured by value: the lambdas outlive this iteration

        widgets.nameEdit = new QLineEdit(contextRowsContainer_);
        widgets.nameEdit->setText(variable.name);
        widgets.nameEdit->setStyleSheet(kRowFieldStyle);
        QLineEdit* nameEdit = widgets.nameEdit;
        connect(nameEdit, &QLineEdit::editingFinished, this,
                [this, id, nameEdit] { emit contextNameEditingFinished(id, nameEdit->text()); });
        contextRowsLayout_->addWidget(widgets.nameEdit, row, 0);

        widgets.typeCombo = new QComboBox(contextRowsContainer_);
        // The default size policy follows the widest item ever added (e.g. a long
        // custom struct name), inflating every row's width. This policy sizes off
        // the minimum or the selected item instead; 6 chars keeps "String" and
        // "Double" from clipping.
        widgets.typeCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        widgets.typeCombo->setMinimumContentsLength(6);
        widgets.typeCombo->setStyleSheet(kRowFieldStyle);
        widgets.typeCombo->addItem(QStringLiteral("Bool"), static_cast<int>(ContextType::Bool));
        widgets.typeCombo->addItem(QStringLiteral("Int"), static_cast<int>(ContextType::Int));
        widgets.typeCombo->addItem(QStringLiteral("Double"), static_cast<int>(ContextType::Double));
        widgets.typeCombo->addItem(QStringLiteral("String"), static_cast<int>(ContextType::String));
        widgets.typeCombo->addItem(QStringLiteral("Object"), static_cast<int>(ContextType::Object));
        for (const StructDefinition& def : structDefinitions_) {
            if (!def.name.isEmpty()) {
                widgets.typeCombo->addItem(def.name, QVariant::fromValue(QString(def.name)));
            }
        }
        if (variable.type == ContextType::Object && !variable.customTypeName.isEmpty()) {
            const int customIdx = widgets.typeCombo->findData(QVariant::fromValue(variable.customTypeName));
            if (customIdx >= 0) {
                widgets.typeCombo->setCurrentIndex(customIdx);
            } else {
                widgets.typeCombo->setCurrentIndex(widgets.typeCombo->findData(static_cast<int>(ContextType::Object)));
            }
        } else {
            widgets.typeCombo->setCurrentIndex(widgets.typeCombo->findData(static_cast<int>(variable.type)));
        }

        widgets.valueEdit = new QLineEdit(contextRowsContainer_);
        widgets.valueEdit->setText(variable.initialValue);
        widgets.valueEdit->setStyleSheet(kRowFieldStyle);
        QLineEdit* valueEdit = widgets.valueEdit;

        auto* intValidator = new QIntValidator(valueEdit);
        auto* intFilter = new IntEditEventFilter(valueEdit, [this, id](const QString& text) {
            emit contextInitialValueEditingFinished(id, text);
        }, valueEdit);
        valueEdit->installEventFilter(intFilter);

        const auto applyTypeControls = [valueEdit, intValidator, intFilter](ContextType type) {
            const bool isInt = (type == ContextType::Int);
            intFilter->setEnabled(isInt);
            valueEdit->setValidator(isInt ? intValidator : nullptr);
        };
        applyTypeControls(variable.type);

        const auto updateValueEditStyle = [valueEdit](ContextType type, const QString& text) {
            if (type == ContextType::Object) {
                QJsonParseError err;
                QJsonDocument::fromJson(text.trimmed().toUtf8(), &err);
                if (err.error != QJsonParseError::NoError && !text.trimmed().isEmpty()) {
                    valueEdit->setStyleSheet(resolveRoles(QStringLiteral(
                        "QLineEdit { background: {danger-surface}; color: {danger}; font-size: 11px; "
                        "border: 1px solid {danger}; border-radius: 2px; padding: 1px 3px; min-height: 18px; }")));
                    valueEdit->setToolTip(QStringLiteral("JSON parse error: %1").arg(err.errorString()));
                    return;
                }
            }
            valueEdit->setStyleSheet(kRowFieldStyle);
            valueEdit->setToolTip(QString());
        };
        updateValueEditStyle(variable.type, variable.initialValue);

        QComboBox* typeCombo = widgets.typeCombo;
        connect(typeCombo, &QComboBox::activated, this, [this, id, typeCombo, valueEdit, updateValueEditStyle, applyTypeControls](int index) {
            const QVariant data = typeCombo->itemData(index);
            ContextType newType = ContextType::Object;
            QString customTypeName;
            if (data.typeId() == QMetaType::QString) {
                newType = ContextType::Object;
                customTypeName = data.toString();
            } else {
                newType = static_cast<ContextType>(data.toInt());
            }
            applyTypeControls(newType);
            updateValueEditStyle(newType, valueEdit->text());
            emit contextTypeActivated(id, newType);
            if (!customTypeName.isEmpty() || newType != ContextType::Object) {
                emit contextCustomTypeNameActivated(id, customTypeName);
            }
        });
        contextRowsLayout_->addWidget(widgets.typeCombo, row, 1);

        connect(valueEdit, &QLineEdit::textChanged, this, [updateValueEditStyle, typeCombo, valueEdit](const QString& text) {
            const QVariant data = typeCombo->currentData();
            const ContextType ct = (data.typeId() == QMetaType::QString) ? ContextType::Object : static_cast<ContextType>(data.toInt());
            updateValueEditStyle(ct, text);
        });
        connect(valueEdit, &QLineEdit::editingFinished, this,
                [this, id, valueEdit] { emit contextInitialValueEditingFinished(id, valueEdit->text()); });
        contextRowsLayout_->addWidget(widgets.valueEdit, row, 2);

        // Row-gutter icons: tinted SVGs in a fixed slot. "fit" stands for
        // "bring these on-canvas elements into focus"; "send-to" is already the
        // Actions rows' icon.
        widgets.traceButton = new QToolButton(contextRowsContainer_);
        widgets.traceButton->setIcon(icons::asset("fit"));
        widgets.traceButton->setIconSize(QSize(kRowIconGlyphSize, kRowIconGlyphSize));
        widgets.traceButton->setFixedSize(kRowIconButtonSize, kRowIconButtonSize);
        widgets.traceButton->setCheckable(true);
        widgets.traceButton->setToolTip(QStringLiteral("Highlight transitions referencing '%1'").arg(variable.name));
        widgets.traceButton->setStyleSheet(kRowIconButtonStyle);
        const QString varName = variable.name;
        connect(widgets.traceButton, &QToolButton::clicked, this,
                [this, varName] { emit contextVariableHighlightRequested(varName); });
        contextRowsLayout_->addWidget(widgets.traceButton, row, 3);

        widgets.deleteButton = new QToolButton(contextRowsContainer_);
        widgets.deleteButton->setIcon(icons::asset("remove"));
        widgets.deleteButton->setIconSize(QSize(kRowIconGlyphSize, kRowIconGlyphSize));
        widgets.deleteButton->setFixedSize(kRowIconButtonSize, kRowIconButtonSize);
        widgets.deleteButton->setToolTip(QStringLiteral("Delete"));
        widgets.deleteButton->setStyleSheet(kRowIconButtonStyle);
        connect(widgets.deleteButton, &QToolButton::clicked, this, [this, id] { emit contextDeleteClicked(id); });
        contextRowsLayout_->addWidget(widgets.deleteButton, row, 4);

        // Live value in its own column; hidden until a run pushes values, so an
        // empty column is not read as "no value".
        widgets.liveValueLabel = new QLabel(contextRowsContainer_);
        widgets.liveValueLabel->setStyleSheet(kLiveValueStyle);
        widgets.liveValueLabel->setVisible(false);
        contextRowsLayout_->addWidget(widgets.liveValueLabel, row, 5);

        widgets.name = variable.name;
        contextRows_.push_back(widgets);
        ++row;
    }

    // The rebuild discarded the row widgets; re-apply the run overlay so a
    // mid-run edit does not blank live values.
    applyLiveOverlay();
}

void LogicPanel::setLiveContextValues(const QVariantMap& values, bool running) {
    liveContextValues_ = values;
    liveRunning_ = running;
    applyLiveOverlay();
}

void LogicPanel::setLiveGuardResults(const QHash<QString, LiveGuardResult>& results, bool running) {
    liveGuardResults_ = results;
    liveRunning_ = running;
    applyLiveOverlay();
}

void LogicPanel::setLiveActorStates(const QSet<QString>& liveInvokeIds, bool running) {
    liveActorInvokeIds_ = liveInvokeIds;
    liveRunning_ = running;
    applyLiveOverlay();
}

// ---- Deferred rebuild --------------------------------------------------------
// The setters only record what to draw; drawing runs on a zero-timer, outside
// the caller's event handler (required for correctness, not an optimisation).

void LogicPanel::setContextVariables(const QVector<ContextVariable>& variables) {
    pendingContextVariables_ = variables;
    contextDirty_ = true;
    scheduleFlush();
}

void LogicPanel::setGuardRows(const QVector<LogicRow>& rows) {
    pendingGuardRows_ = rows;
    guardRowsDirty_ = true;
    scheduleFlush();
}

void LogicPanel::setActionRows(const QVector<LogicRow>& rows) {
    pendingActionRows_ = rows;
    actionRowsDirty_ = true;
    scheduleFlush();
}

void LogicPanel::setActorRows(const QVector<ActorRow>& rows) {
    pendingActorRows_ = rows;
    actorRowsDirty_ = true;
    scheduleFlush();
}

void LogicPanel::scheduleFlush() {
    if (flushScheduled_) {
        return;  // one flush per event-loop turn, however many facts arrive
    }
    flushScheduled_ = true;
    QTimer::singleShot(0, this, &LogicPanel::flushPendingRebuilds);
}

void LogicPanel::flushPendingRebuilds() {
    flushScheduled_ = false;
    // A flush would delete an open rename editor mid-typing; re-arm until its
    // commit or abort ends.
    if (renameEditor_ != nullptr) {
        scheduleFlush();
        return;
    }
    if (contextDirty_) {
        contextDirty_ = false;
        setContextVariablesNow(pendingContextVariables_);
    }
    if (guardRowsDirty_) {
        guardRowsDirty_ = false;
        setGuardRowsNow(pendingGuardRows_);
    }
    if (actionRowsDirty_) {
        actionRowsDirty_ = false;
        setActionRowsNow(pendingActionRows_);
    }
    if (actorRowsDirty_) {
        actorRowsDirty_ = false;
        setActorRowsNow(pendingActorRows_);
    }
    if (typesDirty_) {
        typesDirty_ = false;
        setTypesNow(pendingStructDefinitions_, pendingExternalHeaders_);
    }
}


void LogicPanel::applyLiveOverlay() {
    for (ContextRowWidgets& widgets : contextRows_) {
        if (widgets.liveValueLabel == nullptr) {
            continue;
        }
        // Keyed by name, as SimulationAgent::contextValues() is; a missing key
        // shows nothing rather than a stale pre-rename value.
        const bool hasValue = liveRunning_ && liveContextValues_.contains(widgets.name);
        widgets.liveValueLabel->setVisible(hasValue);
        if (hasValue) {
            const QVariant val = liveContextValues_.value(widgets.name);
            if (val.userType() == QMetaType::QVariantMap) {
                const QJsonObject obj = QJsonObject::fromVariantMap(val.toMap());
                const QString jsonStr = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
                widgets.liveValueLabel->setText(jsonStr);
                widgets.liveValueLabel->setToolTip(jsonStr);
            } else {
                widgets.liveValueLabel->setText(val.toString());
                widgets.liveValueLabel->setToolTip(QString());
            }
        } else {
            widgets.liveValueLabel->setText(QString());
            widgets.liveValueLabel->setToolTip(QString());
        }
    }
    for (LogicListRow& listRow : guardRows_) {
        if (listRow.liveResultLabel == nullptr) {
            continue;
        }
        // Keyed by guard source text, as the merged rows are.
        const auto found = liveGuardResults_.constFind(listRow.row.name);
        const bool has = liveRunning_ && found != liveGuardResults_.constEnd();
        listRow.liveResultLabel->setVisible(has);
        if (!has) {
            listRow.liveResultLabel->setText(QString());
            continue;
        }
        // "?" for an undecided guard, never "false": an undecidable guard
        // refuses the transition without the condition being false.
        listRow.liveResultLabel->setText(!found->decided ? QStringLiteral("?")
                                         : found->result ? QStringLiteral("true")
                                                         : QStringLiteral("false"));
    }
    for (ActorListRow& listRow : actorRows_) {
        if (listRow.liveLabel == nullptr) {
            continue;
        }
        // Keyed by effective invoke id; membership is the whole answer (no "?").
        const bool live = liveRunning_ && liveActorInvokeIds_.contains(listRow.row.effectiveId);
        listRow.liveLabel->setVisible(live);
        listRow.liveLabel->setText(live ? QStringLiteral("live") : QString());
    }
}

int LogicPanel::debugGuardRowCount() const { return static_cast<int>(guardRows_.size()); }

LogicRow LogicPanel::debugGuardRow(int index) const {
    if (index < 0 || index >= static_cast<int>(guardRows_.size())) {
        return LogicRow{};
    }
    return guardRows_[static_cast<std::size_t>(index)].row;
}

bool LogicPanel::debugGuardPlaceholderVisible() const {
    return guardPlaceholder_ != nullptr && !guardPlaceholder_->isHidden();
}

void LogicPanel::debugClickGuardRow(int index) {
    if (index < 0 || index >= static_cast<int>(guardRows_.size())) {
        return;
    }
    guardRows_[static_cast<std::size_t>(index)].button->click();
}

int LogicPanel::debugActionRowCount() const { return static_cast<int>(actionRows_.size()); }

LogicRow LogicPanel::debugActionRow(int index) const {
    if (index < 0 || index >= static_cast<int>(actionRows_.size())) {
        return LogicRow{};
    }
    return actionRows_[static_cast<std::size_t>(index)].row;
}

bool LogicPanel::debugActionPlaceholderVisible() const {
    return actionPlaceholder_ != nullptr && !actionPlaceholder_->isHidden();
}

void LogicPanel::debugClickActionRow(int index) {
    if (index < 0 || index >= static_cast<int>(actionRows_.size())) {
        return;
    }
    actionRows_[static_cast<std::size_t>(index)].button->click();
}

int LogicPanel::debugActorRowCount() const { return static_cast<int>(actorRows_.size()); }

ActorRow LogicPanel::debugActorRow(int index) const {
    if (index < 0 || index >= static_cast<int>(actorRows_.size())) {
        return ActorRow{};
    }
    return actorRows_[static_cast<std::size_t>(index)].row;
}

bool LogicPanel::debugActorPlaceholderVisible() const {
    return actorPlaceholder_ != nullptr && !actorPlaceholder_->isHidden();
}

void LogicPanel::debugClickActorRow(int index) {
    if (index < 0 || index >= static_cast<int>(actorRows_.size())) {
        return;
    }
    actorRows_[static_cast<std::size_t>(index)].button->click();
}

QString LogicPanel::debugLiveActorState(int index) const {
    if (index < 0 || index >= static_cast<int>(actorRows_.size())) {
        return QString();
    }
    const QLabel* label = actorRows_[static_cast<std::size_t>(index)].liveLabel;
    return (label != nullptr && !label->isHidden()) ? label->text() : QString();
}

void LogicPanel::setGuardRowsNow(const QVector<LogicRow>& rows) {
    rebuildLogicRows(rows, guardRows_, guardRowsLayout_, guardPlaceholder_, guardRowsScroll_);
}

void LogicPanel::setActionRowsNow(const QVector<LogicRow>& rows) {
    rebuildLogicRows(rows, actionRows_, actionRowsLayout_, actionPlaceholder_, actionRowsScroll_);
}

void LogicPanel::setActorRowsNow(const QVector<ActorRow>& rows) { rebuildActorRows(rows); }

void LogicPanel::rebuildLogicRows(const QVector<LogicRow>& rows, std::vector<LogicListRow>& target,
                                   QVBoxLayout* rowsLayout, QLabel* placeholder, QScrollArea* scroll) {
    // Wholesale rebuild every call: cheap, and undo/redo shows the restored
    // document with no diffing.
    while (QLayoutItem* item = rowsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    target.clear();

    placeholder->setVisible(rows.isEmpty());
    scroll->setVisible(!rows.isEmpty());

    QWidget* container = rowsLayout->parentWidget();
    for (const LogicRow& row : rows) {
        LogicListRow entry;
        entry.row = row;

        QToolButton* button = new QToolButton(container);
        button->setIcon(icons::icon(rowClassToIconId(row.rowClass)));
        button->setIconSize(QSize(kRowIconGlyphSize, kRowIconGlyphSize));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setText(logicRowLabel(row));
        button->setToolTip(classTooltip(row.rowClass, machineName_));
        button->setStyleSheet(kRowButtonStyle);

        // Click reveals the first reference in document order: the first
        // transition if any, else the first state (the inventory visits all
        // transitions before states). Ids are captured by value.
        const quint64 firstTransitionId = row.transitionIds.isEmpty() ? 0 : row.transitionIds.first();
        const quint64 firstStateId = (row.transitionIds.isEmpty() && !row.stateIds.isEmpty()) ? row.stateIds.first() : 0;
        connect(button, &QToolButton::clicked, this, [this, firstTransitionId, firstStateId] {
            if (firstTransitionId != 0) {
                emit transitionRowActivated(firstTransitionId);
            } else if (firstStateId != 0) {
                emit stateRowActivated(firstStateId);
            }
        });

        // Rename lives on a context menu to save horizontal room. Only Hook rows
        // are renameable; expression rows get the menu with the entry disabled.
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        const QString rowName = row.name;
        const bool renameable = row.rowClass == LogicRowClass::Hook;
        const bool isGuardRow = (&target == &guardRows_);
        connect(button, &QToolButton::customContextMenuRequested, this,
                [this, button, rowName, renameable, isGuardRow](const QPoint& where) {
                    QMenu menu(this);
                    QAction* renameAction = menu.addAction(renameable ? QStringLiteral("Rename Hook...")
                                                                      : QStringLiteral("Rename (hooks only)"));
                    renameAction->setEnabled(renameable);
                    if (menu.exec(button->mapToGlobal(where)) == renameAction && renameable) {
                        beginRowRename(button, rowName, isGuardRow);
                    }
                });

        // The live result shares the button's slot in a thin horizontal pair, so
        // row height and panel width are unchanged when no run is live.
        auto* rowLine = new QWidget(container);
        auto* rowLineLayout = new QHBoxLayout(rowLine);
        rowLineLayout->setContentsMargins(0, 0, 0, 0);
        rowLineLayout->setSpacing(0);
        rowLineLayout->addWidget(button, 1);
        entry.liveResultLabel = new QLabel(rowLine);
        entry.liveResultLabel->setStyleSheet(kLiveValueStyle);
        entry.liveResultLabel->setVisible(false);
        rowLineLayout->addWidget(entry.liveResultLabel);

        rowsLayout->addWidget(rowLine);
        entry.button = button;
        target.push_back(entry);
    }
    applyLiveOverlay();  // a rebuild discards the live labels; re-apply
}

void LogicPanel::rebuildActorRows(const QVector<ActorRow>& rows) {
    // Wholesale rebuild every call. ActorRow is not a LogicRow, so this works on
    // the Actors members directly.
    while (QLayoutItem* item = actorRowsLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    actorRows_.clear();

    actorPlaceholder_->setVisible(rows.isEmpty());
    actorRowsScroll_->setVisible(!rows.isEmpty());

    QWidget* container = actorRowsLayout_->parentWidget();
    for (const ActorRow& row : rows) {
        ActorListRow entry;
        entry.row = row;

        QToolButton* button = new QToolButton(container);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setText(actorRowLabel(row));
        button->setToolTip(actorRowTooltip(row));
        button->setStyleSheet(kRowButtonStyle);

        // An Actors row belongs to exactly one state, so a click reveals it via
        // stateRowActivated.
        const quint64 stateId = row.stateId;
        connect(button, &QToolButton::clicked, this, [this, stateId] { emit stateRowActivated(stateId); });

        // The live label shares the button's slot, as in rebuildLogicRows().
        auto* rowLine = new QWidget(container);
        auto* rowLineLayout = new QHBoxLayout(rowLine);
        rowLineLayout->setContentsMargins(0, 0, 0, 0);
        rowLineLayout->setSpacing(0);
        rowLineLayout->addWidget(button, 1);
        entry.liveLabel = new QLabel(rowLine);
        entry.liveLabel->setStyleSheet(kLiveValueStyle);
        entry.liveLabel->setVisible(false);
        rowLineLayout->addWidget(entry.liveLabel);

        actorRowsLayout_->addWidget(rowLine);
        entry.button = button;
        actorRows_.push_back(entry);
    }
    applyLiveOverlay();  // a rebuild discards the live labels; re-apply
}

void LogicPanel::beginRowRename(QToolButton* rowButton, const QString& currentName, bool isGuardRow) {
    // In-place editor in the row's layout slot: the button hides and a QLineEdit
    // takes its position. Enter commits, Esc aborts.
    // Cast to QBoxLayout, not QVBoxLayout: rows are wrapped in a per-row
    // QHBoxLayout, and a QVBoxLayout cast returns nullptr so the editor never opens.
    auto* layout = qobject_cast<QBoxLayout*>(rowButton->parentWidget()->layout());
    if (layout == nullptr) {
        return;
    }
    const int index = layout->indexOf(rowButton);
    if (index < 0) {
        return;
    }
    auto* editor = new QLineEdit(rowButton->parentWidget());
    editor->setText(currentName);
    editor->setStyleSheet(kRowFieldStyle);
    editor->selectAll();
    rowButton->hide();
    layout->insertWidget(index, editor);
    editor->setFocus();
    editor->installEventFilter(this);  // Esc -> abort, via eventFilter() below
    renameEditor_ = editor;

    // Commit on Enter only: editingFinished also fires on focus-out, and this
    // rename rewrites every element referencing the hook.
    connect(editor, &QLineEdit::returnPressed, this, [this, editor, currentName, isGuardRow] {
        const QString typed = editor->text().trimmed();
        finishRowRename();
        if (!typed.isEmpty() && typed != currentName) {
            if (isGuardRow) {
                emit guardRenameRequested(currentName, typed);
            } else {
                emit actionRenameRequested(currentName, typed);
            }
        }
    });
}

void LogicPanel::finishRowRename() {
    if (renameEditor_ == nullptr) {
        return;
    }
    QLineEdit* editor = renameEditor_;
    renameEditor_ = nullptr;
    QWidget* rowHost = editor->parentWidget();  // read before the orphaning below

    // Order matters (wrong order segfaults): commit runs inside the editor's own
    // returnPressed emission and triggers a rebuild that deletes every widget in
    // the row layout, so take the editor out of the layout first.
    if (auto* layout = qobject_cast<QBoxLayout*>(editor->parentWidget() != nullptr
                                                      ? editor->parentWidget()->layout()
                                                      : nullptr)) {
        layout->removeWidget(editor);
    }
    // removeWidget() keeps the parent-child link, and the rebuild deletes the
    // per-row wrapper with all its children; orphan the editor so it survives
    // until deleteLater().
    editor->setParent(nullptr);
    editor->hide();
    editor->deleteLater();

    // Abort triggers no rebuild, so re-show the hidden row button; harmless on commit.
    if (rowHost != nullptr) {
        for (QToolButton* button : rowHost->findChildren<QToolButton*>()) {
            button->show();
        }
    }
}

bool LogicPanel::eventFilterRenameEscape(QKeyEvent* keyEvent) {
    if (renameEditor_ == nullptr || keyEvent->key() != Qt::Key_Escape) {
        return false;
    }
    finishRowRename();  // abort: nothing is sent, the row reappears unchanged
    return true;
}

bool LogicPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == renameEditor_ && event->type() == QEvent::KeyPress &&
        eventFilterRenameEscape(static_cast<QKeyEvent*>(event))) {
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

QLineEdit* LogicPanel::debugContextNameEdit(int index) const {
    if (index < 0 || index >= static_cast<int>(contextRows_.size())) {
        return nullptr;
    }
    return contextRows_[index].nameEdit;
}

QString LogicPanel::debugLiveContextValue(int index) const {
    if (index < 0 || index >= static_cast<int>(contextRows_.size())) {
        return QString();
    }
    const QLabel* label = contextRows_[index].liveValueLabel;
    return (label != nullptr && !label->isHidden()) ? label->text() : QString();
}

QString LogicPanel::debugLiveGuardResult(int index) const {
    if (index < 0 || index >= static_cast<int>(guardRows_.size())) {
        return QString();
    }
    const QLabel* label = guardRows_[index].liveResultLabel;
    return (label != nullptr && !label->isHidden()) ? label->text() : QString();
}

// Both levers apply the context menu's Hook-only gate rather than bypass it.
bool LogicPanel::debugBeginGuardRowRename(int index) {
    if (index < 0 || index >= static_cast<int>(guardRows_.size()) ||
        guardRows_[index].row.rowClass != LogicRowClass::Hook) {
        return false;
    }
    beginRowRename(guardRows_[index].button, guardRows_[index].row.name, /*isGuardRow=*/true);
    return renameEditor_ != nullptr;
}

bool LogicPanel::debugBeginActionRowRename(int index) {
    if (index < 0 || index >= static_cast<int>(actionRows_.size()) ||
        actionRows_[index].row.rowClass != LogicRowClass::Hook) {
        return false;
    }
    beginRowRename(actionRows_[index].button, actionRows_[index].row.name, /*isGuardRow=*/false);
    return renameEditor_ != nullptr;
}

}  // namespace app

