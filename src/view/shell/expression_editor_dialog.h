#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QVector>
#include <optional>

#include "infra/expression.h"
#include "model/machine.h"

namespace app {

class ExpressionEditorDialog : public QDialog {
    Q_OBJECT

public:
    enum class Kind {
        Guard,
        Action
    };

    enum class ValidationStatus {
        Valid,
        Error,
        Note
    };

    explicit ExpressionEditorDialog(Kind kind,
                                   const QString& initialText,
                                   const QVector<ContextVariable>& contextVars,
                                   const QVector<StructDefinition>& structDefs = {},
                                   const expr::PayloadBinding& payload = expr::PayloadBinding(),
                                   QWidget* parent = nullptr);

    [[nodiscard]] Kind kind() const { return kind_; }
    [[nodiscard]] QString expressionText() const;
    void setExpressionText(const QString& text);

    [[nodiscard]] bool isValid() const { return validationStatus_ != ValidationStatus::Error; }
    [[nodiscard]] ValidationStatus validationStatus() const { return validationStatus_; }
    [[nodiscard]] QString feedbackMessage() const { return feedbackMessage_; }

    // Test & probe hooks
    void clickVariableChip(const QString& varName);
    void clickOperatorButton(const QString& opText);
    void clickApply();
    void clickCancel();

    static std::optional<QString> editExpression(Kind kind,
                                                const QString& initialText,
                                                const QVector<ContextVariable>& contextVars,
                                                const QVector<StructDefinition>& structDefs = {},
                                                const expr::PayloadBinding& payload = expr::PayloadBinding(),
                                                QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void onTextChanged();

private:
    void setupUi();
    void buildContextChips(QWidget* container);
    void buildOperatorButtons(QWidget* container);
    void insertSnippet(const QString& snippet, int cursorOffset = 0);
    void updateValidation();
    void setValidationStatus(ValidationStatus status, const QString& message);

    Kind kind_;
    QString initialText_;
    QVector<ContextVariable> contextVars_;
    QVector<StructDefinition> structDefs_;
    expr::PayloadBinding payload_;

    QPlainTextEdit* editor_ = nullptr;
    QLabel* validationBanner_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
    QPushButton* applyButton_ = nullptr;

    ValidationStatus validationStatus_ = ValidationStatus::Valid;
    QString feedbackMessage_;
    QVector<QPushButton*> variableChips_;
    QVector<QPushButton*> operatorButtons_;
};

}  // namespace app
