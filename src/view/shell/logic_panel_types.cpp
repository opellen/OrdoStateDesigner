#include "view/shell/logic_panel.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "view/shell/icons.h"
#include "view/shell/struct_editor_dialog.h"

namespace app {

using namespace inspector_detail;  // kRowIconButtonStyle

namespace {

const QString kRowFieldStyle = resolveRoles(QStringLiteral(
    "QLineEdit { background: {surface-2}; color: {text-primary}; border: 1px solid {outline}; "
    "border-radius: 3px; padding: 2px 6px; font-size: 12px; }"
    "QLineEdit:focus { border: 1px solid {outline-focus}; }"));
const QString kRowButtonStyle = resolveRoles(QStringLiteral(
    "QToolButton { color: {text-secondary}; font-size: 11px; padding: 2px 6px; "
    "border: 1px solid {outline-strong}; border-radius: 3px; background: {surface-2}; }"
    "QToolButton:hover { background: {surface-hover}; color: {text-primary}; }"));

inline QStringList splitHeadersList(const QString& text) {
    QStringList result;
    for (const QString& raw : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed = raw.trimmed();
        if (!trimmed.isEmpty()) {
            result.push_back(trimmed);
        }
    }
    return result;
}

}  // namespace

void LogicPanel::debugClickTypesAdd() {
    if (typesAddButton_) {
        typesAddButton_->click();
    }
}

int LogicPanel::debugTypesRowCount() const {
    return static_cast<int>(typeRows_.size());
}

QString LogicPanel::debugTypesRowName(int index) const {
    if (index < 0 || index >= static_cast<int>(typeRows_.size())) {
        return QString();
    }
    return typeRows_[static_cast<size_t>(index)].def.name;
}

void LogicPanel::debugClickTypesDelete(int index) {
    if (index >= 0 && index < static_cast<int>(typeRows_.size())) {
        typeRows_[static_cast<size_t>(index)].deleteButton->click();
    }
}

void LogicPanel::debugClickTypesEdit(int index) {
    if (index >= 0 && index < static_cast<int>(typeRows_.size())) {
        typeRows_[static_cast<size_t>(index)].editButton->click();
    }
}

void LogicPanel::debugCommitExternalHeaders(const QString& commaSeparated) {
    if (externalHeadersEdit_) {
        externalHeadersEdit_->setText(commaSeparated);
        emit externalHeadersEditingFinished(splitHeadersList(commaSeparated));
    }
}

QString LogicPanel::debugExternalHeaders() const {
    return externalHeadersEdit_ ? externalHeadersEdit_->text() : QString();
}

bool LogicPanel::debugTypesPlaceholderVisible() const {
    return typesPlaceholder_ != nullptr && !typesPlaceholder_->isHidden();
}

LogicPanel::Section LogicPanel::buildTypesSection() {
    Section section = inspector_detail::buildCollapsibleSection(
        this, QStringLiteral("Types"), QStringLiteral("No struct definitions yet."), /*enabled=*/true,
        /*defaultExpanded=*/false, /*iconSlug=*/"generate-cpp");

    typesAddButton_ = makeSectionHeaderAction(section, "add", QStringLiteral("Add struct definition"));
    connect(typesAddButton_, &QToolButton::clicked, this, &LogicPanel::structAddClicked);

    auto* bodyLayout = qobject_cast<QVBoxLayout*>(section.body->layout());
    typesPlaceholder_ = qobject_cast<QLabel*>(bodyLayout->itemAt(0)->widget());

    auto* extRow = new QWidget(section.body);
    auto* extLayout = new QHBoxLayout(extRow);
    extLayout->setContentsMargins(4, 2, 4, 2);
    extLayout->setSpacing(4);
    auto* extLabel = new QLabel(QStringLiteral("Headers:"), extRow);
    extLabel->setStyleSheet(resolveRoles(QStringLiteral("color: {text-secondary}; font-size: 11px;")));
    extLayout->addWidget(extLabel, 0);

    externalHeadersEdit_ = new QLineEdit(extRow);
    externalHeadersEdit_->setPlaceholderText(QStringLiteral("\"types.h\", <can.h>"));
    externalHeadersEdit_->setStyleSheet(kRowFieldStyle);
    connect(externalHeadersEdit_, &QLineEdit::editingFinished, this, [this] {
        emit externalHeadersEditingFinished(splitHeadersList(externalHeadersEdit_->text()));
    });
    extLayout->addWidget(externalHeadersEdit_, 1);
    bodyLayout->addWidget(extRow);

    typesRowsContainer_ = new QWidget();
    typesRowsLayout_ = new QVBoxLayout(typesRowsContainer_);
    typesRowsLayout_->setContentsMargins(4, 0, 4, 4);
    typesRowsLayout_->setSpacing(2);

    typesRowsScroll_ = new QScrollArea(section.body);
    typesRowsScroll_->setWidget(typesRowsContainer_);
    typesRowsScroll_->setWidgetResizable(true);
    typesRowsScroll_->setFrameShape(QFrame::NoFrame);
    typesRowsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    typesRowsScroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    bodyLayout->addWidget(typesRowsScroll_);
    typesRowsScroll_->setVisible(false);

    return section;
}

void LogicPanel::setTypes(const QVector<StructDefinition>& structs, const QStringList& externalHeaders) {
    pendingStructDefinitions_ = structs;
    pendingExternalHeaders_ = externalHeaders;
    typesDirty_ = true;
    scheduleFlush();
}

void LogicPanel::setTypesNow(const QVector<StructDefinition>& structs, const QStringList& externalHeaders) {
    structDefinitions_ = structs;
    externalHeaders_ = externalHeaders;

    if (externalHeadersEdit_ != nullptr) {
        const QString joined = externalHeaders.join(QStringLiteral(", "));
        if (externalHeadersEdit_->text() != joined) {
            externalHeadersEdit_->setText(joined);
        }
    }

    while (QLayoutItem* item = typesRowsLayout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    typeRows_.clear();

    typesPlaceholder_->setVisible(structs.isEmpty());
    typesRowsScroll_->setVisible(!structs.isEmpty());

    for (const StructDefinition& def : structs) {
        TypeRowWidgets widgets;
        widgets.id = def.id;
        widgets.def = def;

        widgets.container = new QWidget(typesRowsContainer_);
        auto* rowLayout = new QHBoxLayout(widgets.container);
        rowLayout->setContentsMargins(2, 2, 2, 2);
        rowLayout->setSpacing(4);

        QString fieldsSummary;
        if (def.fields.isEmpty()) {
            fieldsSummary = QStringLiteral("(empty)");
        } else {
            QStringList fieldNames;
            for (const StructField& f : def.fields) {
                fieldNames.push_back(f.name);
            }
            fieldsSummary = QStringLiteral("(%1 fields: %2)").arg(QString::number(def.fields.size()), fieldNames.join(QStringLiteral(", ")));
        }

        widgets.nameLabel = new QLabel(QStringLiteral("[%1] %2 %3")
            .arg(def.external ? QStringLiteral("ext") : QStringLiteral("struct"), def.name, fieldsSummary),
            widgets.container);
        widgets.nameLabel->setStyleSheet(resolveRoles(QStringLiteral("color: {text-primary}; font-size: 11px;")));
        rowLayout->addWidget(widgets.nameLabel, 1);

        widgets.editButton = new QToolButton(widgets.container);
        widgets.editButton->setText(QStringLiteral("Edit"));
        widgets.editButton->setStyleSheet(kRowButtonStyle);
        connect(widgets.editButton, &QToolButton::clicked, this, [this, def] {
            auto result = StructEditorDialog::editStruct(def, this);
            if (result.has_value()) {
                emit structChanged(*result);
            }
        });
        rowLayout->addWidget(widgets.editButton, 0);

        widgets.deleteButton = new QToolButton(widgets.container);
        widgets.deleteButton->setIcon(icons::asset("remove"));
        widgets.deleteButton->setIconSize(QSize(kRowIconGlyphSize, kRowIconGlyphSize));
        widgets.deleteButton->setFixedSize(kRowIconButtonSize, kRowIconButtonSize);
        widgets.deleteButton->setStyleSheet(kRowIconButtonStyle);
        const quint64 id = def.id;
        connect(widgets.deleteButton, &QToolButton::clicked, this, [this, id] {
            emit structDeleteClicked(id);
        });
        rowLayout->addWidget(widgets.deleteButton, 0);

        typesRowsLayout_->addWidget(widgets.container);
        typeRows_.push_back(widgets);
    }
}

}  // namespace app
