#include "view/shell/struct_editor_dialog.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QKeyEvent>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

namespace app {

namespace {

class StructIntFilter : public QObject {
public:
    explicit StructIntFilter(QLineEdit* edit, QObject* parent = nullptr)
        : QObject(parent), edit_(edit) {}

    void setEnabled(bool enabled) { enabled_ = enabled; }

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
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QLineEdit* edit_ = nullptr;
    bool enabled_ = true;
};

}  // namespace

StructEditorDialog::StructEditorDialog(const StructDefinition& initial, QWidget* parent)
    : QDialog(parent), def_(initial) {
    setWindowTitle(initial.name.isEmpty() ? QStringLiteral("New Struct Definition")
                                          : QStringLiteral("Edit Struct: %1").arg(initial.name));
    setMinimumSize(540, 380);

    auto* mainLayout = new QVBoxLayout(this);

    auto* formLayout = new QFormLayout();
    nameEdit_ = new QLineEdit(this);
    nameEdit_->setText(def_.name);
    nameEdit_->setPlaceholderText(QStringLiteral("e.g. CanMessage"));
    formLayout->addRow(QStringLiteral("Struct Name:"), nameEdit_);

    externalCheck_ = new QCheckBox(QStringLiteral("External C++ type (not synthesized in types header)"), this);
    externalCheck_->setChecked(def_.external);
    formLayout->addRow(QStringLiteral("External:"), externalCheck_);

    headerPathEdit_ = new QLineEdit(this);
    headerPathEdit_->setText(def_.headerPath);
    headerPathEdit_->setPlaceholderText(QStringLiteral("e.g. \"custom_types.h\" or <can.h>"));
    formLayout->addRow(QStringLiteral("Header Path:"), headerPathEdit_);

    connect(externalCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        headerPathEdit_->setEnabled(checked);
    });
    headerPathEdit_->setEnabled(def_.external);

    mainLayout->addLayout(formLayout);

    auto* fieldsGroup = new QGroupBox(QStringLiteral("Fields"), this);
    auto* fieldsLayout = new QVBoxLayout(fieldsGroup);

    fieldsTable_ = new QTableWidget(this);
    fieldsTable_->setColumnCount(6);
    fieldsTable_->setHorizontalHeaderLabels({
        QStringLiteral("Field Name"),
        QStringLiteral("Type"),
        QStringLiteral("Custom Type"),
        QStringLiteral("Array"),
        QStringLiteral("Size"),
        QStringLiteral("Initial Value")
    });
    fieldsTable_->horizontalHeader()->setStretchLastSection(true);
    fieldsTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    fieldsTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    fieldsTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    fieldsTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    fieldsTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    fieldsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    fieldsLayout->addWidget(fieldsTable_);

    auto* btnLayout = new QHBoxLayout();
    addFieldButton_ = new QPushButton(QStringLiteral("+ Add Field"), this);
    connect(addFieldButton_, &QPushButton::clicked, this, &StructEditorDialog::onAddField);
    btnLayout->addWidget(addFieldButton_);

    removeFieldButton_ = new QPushButton(QStringLiteral("- Remove Field"), this);
    connect(removeFieldButton_, &QPushButton::clicked, this, &StructEditorDialog::onRemoveField);
    btnLayout->addWidget(removeFieldButton_);
    btnLayout->addStretch(1);
    fieldsLayout->addLayout(btnLayout);

    mainLayout->addWidget(fieldsGroup);

    buttonBox_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttonBox_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox_);

    populateFields();
}

void StructEditorDialog::populateFields() {
    fieldsTable_->setRowCount(0);
    for (const StructField& field : def_.fields) {
        addFieldRow(field);
    }
}

void StructEditorDialog::addFieldRow(const StructField& field) {
    const int row = fieldsTable_->rowCount();
    fieldsTable_->insertRow(row);

    auto* nameEdit = new QLineEdit(field.name);
    fieldsTable_->setCellWidget(row, 0, nameEdit);

    auto* typeCombo = new QComboBox();
    typeCombo->addItem(QStringLiteral("Int"), static_cast<int>(FieldType::Int));
    typeCombo->addItem(QStringLiteral("Bool"), static_cast<int>(FieldType::Bool));
    typeCombo->addItem(QStringLiteral("Double"), static_cast<int>(FieldType::Double));
    typeCombo->addItem(QStringLiteral("String"), static_cast<int>(FieldType::String));
    typeCombo->addItem(QStringLiteral("Custom"), static_cast<int>(FieldType::Custom));
    typeCombo->setCurrentIndex(typeCombo->findData(static_cast<int>(field.type)));
    fieldsTable_->setCellWidget(row, 1, typeCombo);

    auto* customTypeEdit = new QLineEdit(field.customTypeName);
    customTypeEdit->setEnabled(field.type == FieldType::Custom);
    fieldsTable_->setCellWidget(row, 2, customTypeEdit);

    auto* arrayCheck = new QCheckBox();
    arrayCheck->setChecked(field.isArray);
    fieldsTable_->setCellWidget(row, 3, arrayCheck);

    auto* sizeSpin = new QSpinBox();
    sizeSpin->setRange(0, 1024);
    sizeSpin->setValue(field.arraySize);
    sizeSpin->setEnabled(field.isArray);
    fieldsTable_->setCellWidget(row, 4, sizeSpin);

    auto* initEdit = new QLineEdit(field.initialValue);
    auto* intValidator = new QIntValidator(initEdit);
    auto* intFilter = new StructIntFilter(initEdit, initEdit);
    initEdit->installEventFilter(intFilter);

    const auto applyTypeControls = [initEdit, intValidator, intFilter](FieldType ft) {
        const bool isInt = (ft == FieldType::Int);
        intFilter->setEnabled(isInt);
        initEdit->setValidator(isInt ? intValidator : nullptr);
    };
    applyTypeControls(field.type);
    fieldsTable_->setCellWidget(row, 5, initEdit);

    connect(typeCombo, &QComboBox::currentIndexChanged, this, [customTypeEdit, typeCombo, applyTypeControls] {
        const auto ft = static_cast<FieldType>(typeCombo->currentData().toInt());
        customTypeEdit->setEnabled(ft == FieldType::Custom);
        applyTypeControls(ft);
    });

    connect(arrayCheck, &QCheckBox::toggled, this, [sizeSpin](bool checked) {
        sizeSpin->setEnabled(checked);
    });
}

void StructEditorDialog::onAddField() {
    addFieldRow(StructField{
        .name = QStringLiteral("field%1").arg(fieldsTable_->rowCount() + 1),
        .type = FieldType::Int,
        .customTypeName = QString(),
        .isArray = false,
        .arraySize = 0,
        .initialValue = QStringLiteral("0"),
    });
}

void StructEditorDialog::onRemoveField() {
    const int currentRow = fieldsTable_->currentRow();
    if (currentRow >= 0 && currentRow < fieldsTable_->rowCount()) {
        fieldsTable_->removeRow(currentRow);
    } else if (fieldsTable_->rowCount() > 0) {
        fieldsTable_->removeRow(fieldsTable_->rowCount() - 1);
    }
}

StructDefinition StructEditorDialog::resultDefinition() const {
    StructDefinition out = def_;
    out.name = nameEdit_->text().trimmed();
    out.external = externalCheck_->isChecked();
    out.headerPath = headerPathEdit_->text().trimmed();
    out.fields.clear();

    for (int row = 0; row < fieldsTable_->rowCount(); ++row) {
        auto* nameWidget = qobject_cast<QLineEdit*>(fieldsTable_->cellWidget(row, 0));
        auto* typeWidget = qobject_cast<QComboBox*>(fieldsTable_->cellWidget(row, 1));
        auto* customTypeWidget = qobject_cast<QLineEdit*>(fieldsTable_->cellWidget(row, 2));
        auto* arrayWidget = qobject_cast<QCheckBox*>(fieldsTable_->cellWidget(row, 3));
        auto* sizeWidget = qobject_cast<QSpinBox*>(fieldsTable_->cellWidget(row, 4));
        auto* initWidget = qobject_cast<QLineEdit*>(fieldsTable_->cellWidget(row, 5));

        if (!nameWidget || !typeWidget || !customTypeWidget || !arrayWidget || !sizeWidget || !initWidget) {
            continue;
        }

        StructField field;
        field.name = nameWidget->text().trimmed();
        field.type = static_cast<FieldType>(typeWidget->currentData().toInt());
        field.customTypeName = customTypeWidget->text().trimmed();
        field.isArray = arrayWidget->isChecked();
        field.arraySize = sizeWidget->value();
        field.initialValue = initWidget->text().trimmed();
        out.fields.push_back(field);
    }

    return out;
}

std::optional<StructDefinition> StructEditorDialog::editStruct(const StructDefinition& initial, QWidget* parent) {
    StructEditorDialog dialog(initial, parent);
    if (dialog.exec() == QDialog::Accepted) {
        return dialog.resultDefinition();
    }
    return std::nullopt;
}

}  // namespace app
