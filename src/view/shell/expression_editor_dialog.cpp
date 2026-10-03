#include "view/shell/expression_editor_dialog.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTextCursor>
#include <QVBoxLayout>

#include "constants/design_tokens.h"
#include "view/shell/icons.h"

namespace app {

namespace {

QString contextTypeToString(ContextType type, const QString& customTypeName = QString()) {
    if (!customTypeName.isEmpty()) {
        return customTypeName;
    }
    switch (type) {
        case ContextType::Bool: return QStringLiteral("Bool");
        case ContextType::Int: return QStringLiteral("Int");
        case ContextType::Double: return QStringLiteral("Double");
        case ContextType::String: return QStringLiteral("String");
        case ContextType::Object: return QStringLiteral("Object");
    }
    return QStringLiteral("Unknown");
}

}  // namespace

ExpressionEditorDialog::ExpressionEditorDialog(Kind kind,
                                               const QString& initialText,
                                               const QVector<ContextVariable>& contextVars,
                                               const QVector<StructDefinition>& structDefs,
                                               const expr::PayloadBinding& payload,
                                               QWidget* parent)
    : QDialog(parent),
      kind_(kind),
      initialText_(initialText),
      contextVars_(contextVars),
      structDefs_(structDefs),
      payload_(payload) {
    setWindowTitle(kind_ == Kind::Guard ? QStringLiteral("Expression Editor — Transition Guard")
                                        : QStringLiteral("Expression Editor — Transition Action"));
    setMinimumSize(580, 430);
    resize(620, 460);

    setupUi();
    setExpressionText(initialText_);
}

void ExpressionEditorDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(14, 14, 14, 14);
    mainLayout->setSpacing(10);

    // Subtitle / guide banner
    auto* guideLabel = new QLabel(this);
    guideLabel->setWordWrap(true);
    guideLabel->setStyleSheet(design::resolveRoles(QStringLiteral("color: {text-secondary}; font-size: 11px;")));
    if (kind_ == Kind::Guard) {
        guideLabel->setText(QStringLiteral(
            "Enter a boolean condition evaluated against context variables, or a bare identifier for a named hook. "
            "(Note: a bare identifier like 'active' calls a virtual hook; write 'active == true' to test a context boolean)"));
    } else {
        guideLabel->setText(QStringLiteral(
            "Enter an assignment (e.g. 'counter = counter + 1'), raise('EVENT'), sendTo('actor', 'EVENT'), "
            "sendParent('EVENT'), or a bare identifier for a named action hook."));
    }
    mainLayout->addWidget(guideLabel);

    // Context variables bar
    auto* varsGroup = new QGroupBox(QStringLiteral("Context Variables (click to insert)"), this);
    varsGroup->setStyleSheet(design::resolveRoles(QStringLiteral(
        "QGroupBox { font-size: 11px; font-weight: bold; color: {text-secondary}; border: 1px solid {outline-strong}; border-radius: 6px; margin-top: 6px; padding-top: 10px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }")));
    buildContextChips(varsGroup);
    mainLayout->addWidget(varsGroup);

    // Operator and snippet bar
    auto* opGroup = new QGroupBox(QStringLiteral("Operators & Snippets"), this);
    opGroup->setStyleSheet(design::resolveRoles(QStringLiteral(
        "QGroupBox { font-size: 11px; font-weight: bold; color: {text-secondary}; border: 1px solid {outline-strong}; border-radius: 6px; margin-top: 6px; padding-top: 10px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }")));
    buildOperatorButtons(opGroup);
    mainLayout->addWidget(opGroup);

    // Text editor
    editor_ = new QPlainTextEdit(this);
    QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    monoFont.setPointSize(11);
    editor_->setFont(monoFont);
    editor_->setPlaceholderText(kind_ == Kind::Guard
                                    ? QStringLiteral("e.g. count >= 10 && status == 'active'")
                                    : QStringLiteral("e.g. count = count + 1"));
    editor_->setStyleSheet(design::resolveRoles(QStringLiteral(
        "QPlainTextEdit { background-color: {surface-2}; color: {text-primary}; border: 1px solid {outline}; border-radius: 6px; padding: 8px; font-family: monospace; }"
        "QPlainTextEdit:focus { border: 1px solid {outline-focus}; }")));
    editor_->installEventFilter(this);
    connect(editor_, &QPlainTextEdit::textChanged, this, &ExpressionEditorDialog::onTextChanged);
    mainLayout->addWidget(editor_, 1);

    // Validation feedback banner: state icon (found by object name in
    // setValidationStatus) beside the text label.
    auto* validationRow = new QHBoxLayout();
    validationRow->setContentsMargins(0, 0, 0, 0);
    validationRow->setSpacing(6);

    auto* validationIcon = new QLabel(this);
    validationIcon->setObjectName(QStringLiteral("validationStatusIcon"));
    validationIcon->setFixedSize(14, 14);
    validationRow->addWidget(validationIcon);
    validationRow->setAlignment(validationIcon, Qt::AlignVCenter);

    validationBanner_ = new QLabel(this);
    validationBanner_->setWordWrap(true);
    validationBanner_->setMinimumHeight(32);
    validationRow->addWidget(validationBanner_, 1);

    mainLayout->addLayout(validationRow);

    // Button box & shortcut help
    auto* bottomRow = new QHBoxLayout();
    auto* hintLabel = new QLabel(QStringLiteral("Shortcut: Ctrl+Enter to apply, Esc to cancel"), this);
    hintLabel->setStyleSheet(design::resolveRoles(QStringLiteral("color: {text-disabled}; font-size: 11px;")));
    bottomRow->addWidget(hintLabel);
    bottomRow->addStretch();

    buttonBox_ = new QDialogButtonBox(this);
    applyButton_ = buttonBox_->addButton(QStringLiteral("Apply"), QDialogButtonBox::AcceptRole);
    applyButton_->setDefault(true);
    buttonBox_->addButton(QDialogButtonBox::Cancel);

    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    bottomRow->addWidget(buttonBox_);

    mainLayout->addLayout(bottomRow);
}

void ExpressionEditorDialog::buildContextChips(QWidget* container) {
    auto* groupLayout = new QVBoxLayout(container);
    groupLayout->setContentsMargins(8, 8, 8, 8);

    auto* scroll = new QScrollArea(container);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet(QStringLiteral("background: transparent;"));
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedHeight(36);

    auto* chipsWidget = new QWidget(scroll);
    chipsWidget->setStyleSheet(QStringLiteral("background: transparent;"));
    auto* chipsLayout = new QHBoxLayout(chipsWidget);
    chipsLayout->setContentsMargins(0, 0, 0, 0);
    chipsLayout->setSpacing(6);

    const QString chipStyle = design::resolveRoles(QStringLiteral(
        "QPushButton { background-color: {surface-2}; color: {logic-variable}; border: 1px solid {outline-strong}; border-radius: 4px; padding: 2px 8px; font-size: 11px; }"
        "QPushButton:hover { background-color: {surface-hover}; color: {text-primary}; border-color: {outline-hover}; }"));

    bool hasAny = false;
    for (const ContextVariable& var : contextVars_) {
        hasAny = true;
        const QString labelText = QStringLiteral("%1 : %2").arg(var.name, contextTypeToString(var.type, var.customTypeName));
        auto* chip = new QPushButton(labelText, chipsWidget);
        chip->setStyleSheet(chipStyle);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(QStringLiteral("Click to insert '%1'").arg(var.name));
        connect(chip, &QPushButton::clicked, this, [this, name = var.name] { insertSnippet(name); });
        chipsLayout->addWidget(chip);
        variableChips_.push_back(chip);
    }

    if (payload_.kind != expr::PayloadKind::None && !payload_.name.isEmpty()) {
        hasAny = true;
        const QString labelText = QStringLiteral("%1 (Payload)").arg(payload_.name);
        auto* chip = new QPushButton(labelText, chipsWidget);
        chip->setStyleSheet(design::resolveRoles(QStringLiteral(
            "QPushButton { background-color: {surface-2}; color: {logic-send}; border: 1px solid {outline-strong}; border-radius: 4px; padding: 2px 8px; font-size: 11px; }"
            "QPushButton:hover { background-color: {surface-hover}; color: {text-primary}; }")));
        chip->setCursor(Qt::PointingHandCursor);
        chip->setToolTip(QStringLiteral("Click to insert '%1'").arg(payload_.name));
        connect(chip, &QPushButton::clicked, this, [this, name = payload_.name] { insertSnippet(name); });
        chipsLayout->addWidget(chip);
        variableChips_.push_back(chip);
    }

    if (!hasAny) {
        auto* emptyLabel = new QLabel(QStringLiteral("No context variables defined in machine"), chipsWidget);
        emptyLabel->setStyleSheet(design::resolveRoles(QStringLiteral("color: {text-disabled}; font-size: 11px; font-style: italic;")));
        chipsLayout->addWidget(emptyLabel);
    }

    chipsLayout->addStretch();
    scroll->setWidget(chipsWidget);
    groupLayout->addWidget(scroll);
}

void ExpressionEditorDialog::buildOperatorButtons(QWidget* container) {
    auto* groupLayout = new QVBoxLayout(container);
    groupLayout->setContentsMargins(8, 8, 8, 8);

    auto* rowLayout = new QHBoxLayout();
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);

    const QString opBtnStyle = design::resolveRoles(QStringLiteral(
        "QPushButton { background-color: {surface-2}; color: {text-secondary}; border: 1px solid {outline-strong}; border-radius: 4px; padding: 3px 7px; font-size: 11px; font-weight: bold; }"
        "QPushButton:hover { background-color: {surface-hover}; color: {text-primary}; border-color: {outline-hover}; }"));

    auto addOp = [&](const QString& label, const QString& snippet, int offset = 0, const QString& tooltip = QString()) {
        auto* btn = new QPushButton(label, container);
        btn->setStyleSheet(opBtnStyle);
        btn->setCursor(Qt::PointingHandCursor);
        if (!tooltip.isEmpty()) {
            btn->setToolTip(tooltip);
        }
        connect(btn, &QPushButton::clicked, this, [this, snippet, offset] { insertSnippet(snippet, offset); });
        rowLayout->addWidget(btn);
        operatorButtons_.push_back(btn);
    };

    if (kind_ == Kind::Guard) {
        addOp(QStringLiteral("=="), QStringLiteral(" == "));
        addOp(QStringLiteral("!="), QStringLiteral(" != "));
        addOp(QStringLiteral("<"), QStringLiteral(" < "));
        addOp(QStringLiteral("<="), QStringLiteral(" <= "));
        addOp(QStringLiteral(">"), QStringLiteral(" > "));
        addOp(QStringLiteral(">="), QStringLiteral(" >= "));
        addOp(QStringLiteral("&&"), QStringLiteral(" && "));
        addOp(QStringLiteral("||"), QStringLiteral(" || "));
        addOp(QStringLiteral("!"), QStringLiteral("!"));
        addOp(QStringLiteral("( )"), QStringLiteral("()"), -1, QStringLiteral("Enclose in parentheses"));
        addOp(QStringLiteral("true"), QStringLiteral("true"));
        addOp(QStringLiteral("false"), QStringLiteral("false"));
    } else {
        addOp(QStringLiteral("="), QStringLiteral(" = "));
        addOp(QStringLiteral("+"), QStringLiteral(" + "));
        addOp(QStringLiteral("-"), QStringLiteral(" - "));
        addOp(QStringLiteral("*"), QStringLiteral(" * "));
        addOp(QStringLiteral("/"), QStringLiteral(" / "));
        addOp(QStringLiteral("raise()"), QStringLiteral("raise()"), -1, QStringLiteral("Raise an event to internal microstep queue"));
        addOp(QStringLiteral("sendTo()"), QStringLiteral("sendTo('', '')"), -5, QStringLiteral("Send event to invoked actor"));
        addOp(QStringLiteral("sendParent()"), QStringLiteral("sendParent('')"), -2, QStringLiteral("Send event to parent machine"));
        addOp(QStringLiteral("true"), QStringLiteral("true"));
        addOp(QStringLiteral("false"), QStringLiteral("false"));
        addOp(QStringLiteral("0"), QStringLiteral("0"));
    }

    rowLayout->addStretch();
    groupLayout->addLayout(rowLayout);
}

void ExpressionEditorDialog::insertSnippet(const QString& snippet, int cursorOffset) {
    if (editor_ == nullptr) {
        return;
    }
    QTextCursor cursor = editor_->textCursor();
    if (snippet == QStringLiteral("()") && cursor.hasSelection()) {
        const QString selected = cursor.selectedText();
        cursor.insertText(QStringLiteral("(%1)").arg(selected));
    } else {
        cursor.insertText(snippet);
        if (cursorOffset != 0) {
            cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, -cursorOffset);
        }
    }
    editor_->setTextCursor(cursor);
    editor_->setFocus();
}

QString ExpressionEditorDialog::expressionText() const {
    return editor_ ? editor_->toPlainText().trimmed() : QString();
}

void ExpressionEditorDialog::setExpressionText(const QString& text) {
    if (editor_ != nullptr) {
        editor_->setPlainText(text);
        onTextChanged();
    }
}

void ExpressionEditorDialog::onTextChanged() {
    updateValidation();
}

void ExpressionEditorDialog::updateValidation() {
    const QString text = editor_->toPlainText().trimmed();
    if (text.isEmpty()) {
        setValidationStatus(ValidationStatus::Note,
                            kind_ == Kind::Guard ? QStringLiteral("Empty (no guard condition — transition fires unconditionally)")
                                                 : QStringLiteral("Empty (no action executed on transition)"));
        return;
    }

    if (kind_ == Kind::Guard) {
        if (expr::isBareIdentifier(text)) {
            setValidationStatus(ValidationStatus::Valid,
                                QStringLiteral("Named guard hook: will call virtual method '%1' on machine hooks").arg(text));
            return;
        }

        int position = 0;
        const QString error = expr::validateGuardSource(text, contextVars_, &position, payload_, structDefs_);
        if (!error.isEmpty()) {
            setValidationStatus(ValidationStatus::Error,
                                QStringLiteral("Syntax / Type error at position %1: %2").arg(position).arg(error));
            return;
        }

        setValidationStatus(ValidationStatus::Valid, QStringLiteral("Valid boolean expression"));
    } else {
        // Action validation
        const expr::SendToForm sendTo = expr::parseSendToForm(text);
        if (sendTo.ok) {
            setValidationStatus(ValidationStatus::Valid,
                                QStringLiteral("sendTo actor '%1' event '%2'").arg(sendTo.target, sendTo.event));
            return;
        }
        if (!sendTo.message.isEmpty()) {
            setValidationStatus(ValidationStatus::Error, QStringLiteral("sendTo error: %1").arg(sendTo.message));
            return;
        }

        const expr::SendParentForm sendParent = expr::parseSendParentForm(text);
        if (sendParent.ok) {
            setValidationStatus(ValidationStatus::Valid,
                                QStringLiteral("sendParent event '%1' to parent machine").arg(sendParent.event));
            return;
        }
        if (!sendParent.message.isEmpty()) {
            setValidationStatus(ValidationStatus::Error, QStringLiteral("sendParent error: %1").arg(sendParent.message));
            return;
        }

        const expr::RaiseForm raise = expr::parseRaiseForm(text);
        if (raise.ok) {
            setValidationStatus(ValidationStatus::Valid,
                                QStringLiteral("raise event '%1' (internal microstep queue)").arg(raise.event));
            return;
        }
        if (!raise.message.isEmpty()) {
            setValidationStatus(ValidationStatus::Error, QStringLiteral("raise error: %1").arg(raise.message));
            return;
        }

        const expr::AssignForm assign = expr::parseAssignForm(text);
        if (assign.ok) {
            const QString root = assign.rootTarget();
            bool targetFound = false;
            for (const ContextVariable& v : contextVars_) {
                if (v.name == root) {
                    targetFound = true;
                    break;
                }
            }
            if (!targetFound) {
                setValidationStatus(ValidationStatus::Error,
                                    QStringLiteral("Assign error: target '%1' is not a declared context variable").arg(root));
                return;
            }

            const expr::ParseResult rhsParse = expr::parse(assign.valueSource);
            if (!rhsParse.ok) {
                setValidationStatus(ValidationStatus::Error,
                                    QStringLiteral("Assign RHS error: %1").arg(rhsParse.message));
                return;
            }

            setValidationStatus(ValidationStatus::Valid,
                                QStringLiteral("Assignment: %1 = %2").arg(assign.target, assign.valueSource));
            return;
        }
        if (!assign.message.isEmpty()) {
            setValidationStatus(ValidationStatus::Error, QStringLiteral("Assign error: %1").arg(assign.message));
            return;
        }

        if (expr::isBareIdentifier(text)) {
            setValidationStatus(ValidationStatus::Valid,
                                QStringLiteral("Named action hook: will call virtual method '%1' on machine hooks").arg(text));
            return;
        }

        setValidationStatus(ValidationStatus::Error,
                            QStringLiteral("Unrecognized action syntax (expected assignment, raise, sendTo, sendParent, or named hook)"));
    }
}

void ExpressionEditorDialog::setValidationStatus(ValidationStatus status, const QString& message) {
    validationStatus_ = status;
    feedbackMessage_ = message;
    if (validationBanner_ == nullptr) {
        return;
    }
    validationBanner_->setText(message);
    // The icon label is not a member; found by object name.
    QLabel* icon = findChild<QLabel*>(QStringLiteral("validationStatusIcon"));
    switch (status) {
        case ValidationStatus::Valid:
            validationBanner_->setStyleSheet(design::resolveRoles(QStringLiteral(
                "background-color: {success-surface}; color: {success}; border: 1px solid {success}; border-radius: 6px; padding: 6px 10px; font-size: 11px;")));
            if (icon != nullptr) {
                icon->setPixmap(icons::assetPixmap("check", 14, design::color(design::kSuccess), icon->devicePixelRatioF()));
                icon->setVisible(true);
            }
            break;
        case ValidationStatus::Error:
            validationBanner_->setStyleSheet(design::resolveRoles(QStringLiteral(
                "background-color: {danger-surface}; color: {danger}; border: 1px solid {danger}; border-radius: 6px; padding: 6px 10px; font-size: 11px; font-weight: bold;")));
            if (icon != nullptr) {
                icon->setPixmap(icons::assetPixmap("warning", 14, design::color(design::kDanger), icon->devicePixelRatioF()));
                icon->setVisible(true);
            }
            break;
        case ValidationStatus::Note:
            validationBanner_->setStyleSheet(design::resolveRoles(QStringLiteral(
                "background-color: {surface-2}; color: {text-secondary}; border: 1px solid {outline-hover}; border-radius: 6px; padding: 6px 10px; font-size: 11px;")));
            if (icon != nullptr) {
                icon->clear();
                icon->setVisible(false);
            }
            break;
    }

    if (applyButton_ != nullptr) {
        const bool canApply = (status != ValidationStatus::Error);
        applyButton_->setEnabled(canApply);
        applyButton_->setToolTip(canApply ? QStringLiteral("Apply changes (Ctrl+Enter)")
                                          : QStringLiteral("Fix errors before applying"));
    }
}

bool ExpressionEditorDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == editor_ && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            if (ke->modifiers() & Qt::ControlModifier) {
                if (isValid()) {
                    accept();
                    return true;
                }
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void ExpressionEditorDialog::clickVariableChip(const QString& varName) {
    for (QPushButton* chip : variableChips_) {
        if (chip != nullptr && chip->text().startsWith(varName)) {
            chip->click();
            return;
        }
    }
}

void ExpressionEditorDialog::clickOperatorButton(const QString& opText) {
    for (QPushButton* btn : operatorButtons_) {
        if (btn != nullptr && btn->text() == opText) {
            btn->click();
            return;
        }
    }
}

void ExpressionEditorDialog::clickApply() {
    if (applyButton_ != nullptr && applyButton_->isEnabled()) {
        accept();
    }
}

void ExpressionEditorDialog::clickCancel() {
    reject();
}

std::optional<QString> ExpressionEditorDialog::editExpression(Kind kind,
                                                             const QString& initialText,
                                                             const QVector<ContextVariable>& contextVars,
                                                             const QVector<StructDefinition>& structDefs,
                                                             const expr::PayloadBinding& payload,
                                                             QWidget* parent) {
    ExpressionEditorDialog dialog(kind, initialText, contextVars, structDefs, payload, parent);
    if (dialog.exec() == QDialog::Accepted) {
        return dialog.expressionText();
    }
    return std::nullopt;
}

}  // namespace app
