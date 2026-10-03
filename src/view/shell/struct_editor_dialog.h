#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <optional>

#include "model/machine.h"

namespace app {

class StructEditorDialog : public QDialog {
    Q_OBJECT

public:
    explicit StructEditorDialog(const StructDefinition& initial, QWidget* parent = nullptr);

    StructDefinition resultDefinition() const;

    static std::optional<StructDefinition> editStruct(const StructDefinition& initial, QWidget* parent = nullptr);

private slots:
    void onAddField();
    void onRemoveField();

private:
    void populateFields();
    void addFieldRow(const StructField& field);

    StructDefinition def_;
    QLineEdit* nameEdit_ = nullptr;
    QCheckBox* externalCheck_ = nullptr;
    QLineEdit* headerPathEdit_ = nullptr;
    QTableWidget* fieldsTable_ = nullptr;
    QPushButton* addFieldButton_ = nullptr;
    QPushButton* removeFieldButton_ = nullptr;
    QDialogButtonBox* buttonBox_ = nullptr;
};

}  // namespace app
