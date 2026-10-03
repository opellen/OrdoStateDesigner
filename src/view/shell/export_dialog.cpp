#include "view/shell/export_dialog.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMargins>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSize>
#include <QSpacerItem>
#include <QVBoxLayout>

#include "constants/design_tokens.h"
#include "model/machine.h"

namespace app {

namespace {

// Screen-specific geometry, kept local rather than as shared tokens: this
// 2-pane picker needs more room than a single-column dialog.
constexpr int kDefaultWidth = 680;
constexpr int kDefaultHeight = 480;
constexpr int kMinWidth = 580;
constexpr int kMinHeight = 400;
constexpr int kFormatListWidth = 230;
// Item size hint's width: narrower than kFormatListWidth so no horizontal
// scrollbar appears. QListView::setSpacing() reserves its margin on every side
// of each item's cell, so this subtracts room for that too.
constexpr int kFormatItemWidth = kFormatListWidth - design::kSpace4 - design::kSpace4;

// Renders a token as a QSS pixel length (4 becomes "4px"), built at runtime
// so a px-literal scan of the template finds no off-grid magic numbers.
QString px(int value) { return QString::number(value) + QStringLiteral("px"); }

QString sanitizeFileName(const QString& name) {
    QString out;
    out.reserve(name.size());
    for (const QChar ch : name) {
        if (ch.isLetterOrNumber() || ch == '_' || ch == '-') {
            out.append(ch);
        } else if (ch.isSpace()) {
            out.append('_');
        }
    }
    return out.isEmpty() ? QStringLiteral("machine") : out;
}

// Feedback labels (clipboard copy result): kTypeValue with the color
// overridden to success/danger. QSS declarations are last-wins within one
// rule, so appending "color: {...};" replaces only the color.
const QString kFeedbackDangerStyle =
    design::kTypeValue + design::resolveRoles(QStringLiteral("color: {danger};"));
const QString kFeedbackSuccessStyle =
    design::kTypeValue + design::resolveRoles(QStringLiteral("color: {success};"));

}  // namespace

ExportDialog::ExportDialog(const ExportDialogConfig& config, QWidget* parent)
    : QDialog(parent), config_(config) {
    setWindowTitle(QStringLiteral("Export"));
    setModal(true);
    resize(kDefaultWidth, kDefaultHeight);
    setMinimumSize(kMinWidth, kMinHeight);

    setupUi();
    populateFormats();
}

ExportDialog::~ExportDialog() = default;

void ExportDialog::setupUi() {
    // Selected list item: surface-hover plus a kSelectionBarWidth left bar in
    // accent-interactive. The bar is transparent but the same width on the
    // other states so selecting a row never shifts its text.
    setStyleSheet(design::resolveRoles(QStringLiteral(
                                            "QDialog {"
                                            "  background-color: {surface-1};"
                                            "  color: {text-primary};"
                                            "}"
                                            "QListWidget {"
                                            "  background-color: {surface-2};"
                                            "  border: %1 solid {outline};"
                                            "  border-radius: %2;"
                                            "  color: {text-primary};"
                                            "  outline: none;"
                                            "  padding: %3;"
                                            "}"
                                            "QListWidget::item {"
                                            "  padding: %4 %5;"
                                            "  border-radius: %2;"
                                            "  color: {text-secondary};"
                                            "  border-left: %6 solid transparent;"
                                            "}"
                                            "QListWidget::item:selected {"
                                            "  background-color: {surface-hover};"
                                            "  color: {text-primary};"
                                            "  border-left: %6 solid {accent-interactive};"
                                            "}"
                                            "QListWidget::item:hover:!selected {"
                                            "  background-color: {surface-hover};"
                                            "  color: {text-primary};"
                                            "}"
                                            "QLineEdit, QComboBox {"
                                            "  background-color: {surface-2};"
                                            "  border: %1 solid {outline};"
                                            "  border-radius: %2;"
                                            "  padding: %3 %4;"
                                            "  color: {text-primary};"
                                            "}"
                                            "QLineEdit:focus, QComboBox:focus {"
                                            "  border: %1 solid {outline-focus};"
                                            "}"
                                            "QRadioButton {"
                                            "  color: {text-primary};"
                                            "  spacing: %4;"
                                            "}"
                                            "QRadioButton:disabled {"
                                            "  color: {text-disabled};"
                                            "}"
                                            "QCheckBox {"
                                            "  color: {text-primary};"
                                            "  spacing: %4;"
                                            "}"
                                            "QCheckBox:disabled {"
                                            "  color: {text-disabled};"
                                            "}"))
                       .arg(px(design::kHairline))
                       .arg(px(design::kRadius))
                       .arg(px(design::kSpace1))
                       .arg(px(design::kSpace2))
                       .arg(px(design::kSpace3))
                       .arg(px(design::kSelectionBarWidth)));

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(design::kSpace4, design::kSpace4, design::kSpace4, design::kSpace4);
    rootLayout->setSpacing(design::kSpace3);

    // Main 2-Pane content area
    auto* contentLayout = new QHBoxLayout();
    contentLayout->setSpacing(design::kSpace4);

    // Left pane: Format list
    auto* leftPane = new QVBoxLayout();
    leftPane->setSpacing(design::kSpace2);
    auto* formatsLabel = new QLabel(QStringLiteral("Formats"), this);
    formatsLabel->setStyleSheet(design::kTypeSection);
    leftPane->addWidget(formatsLabel);

    formatList_ = new QListWidget(this);
    formatList_->setFixedWidth(kFormatListWidth);
    // Grid floor (kSpace1); the item's own kSpace2/kSpace3 padding supplies
    // most of the row spacing.
    formatList_->setSpacing(design::kSpace1);
    formatList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(formatList_, &QListWidget::currentItemChanged, this, &ExportDialog::onFormatSelected);
    leftPane->addWidget(formatList_, 1);
    contentLayout->addLayout(leftPane);

    // Right pane: Details, Scope, Options, Destination
    auto* rightPane = new QVBoxLayout();
    rightPane->setSpacing(design::kSpace3);

    // Header: Name + extension badge. The format name reuses kTypeSection.
    auto* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(design::kSpace2);
    formatTitleLabel_ = new QLabel(this);
    formatTitleLabel_->setStyleSheet(design::kTypeSection);
    headerLayout->addWidget(formatTitleLabel_);

    // Extension badge: a kTypeMono chip on a kRadius/kHairline outline card.
    formatExtBadge_ = new QLabel(this);
    formatExtBadge_->setStyleSheet(
        design::resolveRoles(QStringLiteral("background-color: {surface-2}; border: %1 solid {outline}; "
                                             "border-radius: %2; padding: %3 %4; %5"))
            .arg(px(design::kHairline))
            .arg(px(design::kRadius))
            .arg(px(design::kSpace1))
            .arg(px(design::kSpace2))
            .arg(design::kTypeMono));
    headerLayout->addWidget(formatExtBadge_);
    headerLayout->addStretch(1);
    rightPane->addLayout(headerLayout);

    formatDescLabel_ = new QLabel(this);
    formatDescLabel_->setWordWrap(true);
    formatDescLabel_->setStyleSheet(design::kTypeLabel);
    rightPane->addWidget(formatDescLabel_);

    auto* div1 = new QFrame(this);
    div1->setFrameShape(QFrame::HLine);
    div1->setFrameShadow(QFrame::Plain);
    div1->setLineWidth(design::kHairline);
    div1->setStyleSheet(design::resolveRoles(QStringLiteral("color: {outline-strong};")));
    rightPane->addWidget(div1);

    // Scope Section
    auto* scopeTitle = new QLabel(QStringLiteral("Scope"), this);
    scopeTitle->setStyleSheet(design::kTypeSection);
    rightPane->addWidget(scopeTitle);

    const QString activeName = !config_.activeMachineName.isEmpty() ? config_.activeMachineName : QStringLiteral("Active machine");
    scopeSingleRadio_ = new QRadioButton(QStringLiteral("Active machine (%1)").arg(activeName), this);
    scopeDirectoryRadio_ = new QRadioButton(QStringLiteral("All machines in project (separate files)"), this);
    scopeBundleRadio_ = new QRadioButton(QStringLiteral("Bundle all machines into a single file"), this);

    scopeGroup_ = new QButtonGroup(this);
    scopeGroup_->addButton(scopeSingleRadio_, static_cast<int>(ExportScope::SingleMachine));
    scopeGroup_->addButton(scopeDirectoryRadio_, static_cast<int>(ExportScope::MultipleMachinesDirectory));
    scopeGroup_->addButton(scopeBundleRadio_, static_cast<int>(ExportScope::MultipleMachinesSingleFile));
    scopeSingleRadio_->setChecked(true);

    connect(scopeGroup_, &QButtonGroup::idClicked, this, &ExportDialog::onScopeChanged);

    rightPane->addWidget(scopeSingleRadio_);
    rightPane->addWidget(scopeDirectoryRadio_);
    rightPane->addWidget(scopeBundleRadio_);

    auto* div2 = new QFrame(this);
    div2->setFrameShape(QFrame::HLine);
    div2->setFrameShadow(QFrame::Plain);
    div2->setLineWidth(design::kHairline);
    div2->setStyleSheet(design::resolveRoles(QStringLiteral("color: {outline-strong};")));
    rightPane->addWidget(div2);

    // Options Section
    auto* optionsTitle = new QLabel(QStringLiteral("Options"), this);
    optionsTitle->setStyleSheet(design::kTypeSection);
    rightPane->addWidget(optionsTitle);

    optionsContainer_ = new QWidget(this);
    optionsLayout_ = new QVBoxLayout(optionsContainer_);
    optionsLayout_->setContentsMargins(QMargins());
    // Grid floor (kSpace1), matching the collapsible section's row rhythm.
    optionsLayout_->setSpacing(design::kSpace1);
    rightPane->addWidget(optionsContainer_);

    auto* div3 = new QFrame(this);
    div3->setFrameShape(QFrame::HLine);
    div3->setFrameShadow(QFrame::Plain);
    div3->setLineWidth(design::kHairline);
    div3->setStyleSheet(design::resolveRoles(QStringLiteral("color: {outline-strong};")));
    rightPane->addWidget(div3);

    // Destination Section
    auto* destLayout = new QVBoxLayout();
    // kSpace2, matching the Formats pane's title-to-content gap.
    destLayout->setSpacing(design::kSpace2);

    // No trailing colon -- consistent with the other section titles
    // (Formats/Scope/Options). updateDefaultDestination() below swaps this
    // text to "File"/"Directory" per scope, same convention.
    destinationLabel_ = new QLabel(QStringLiteral("Destination"), this);
    destinationLabel_->setStyleSheet(design::kTypeSection);
    destLayout->addWidget(destinationLabel_);

    auto* pathEditLayout = new QHBoxLayout();
    pathEditLayout->setSpacing(design::kSpace2);

    destinationEdit_ = new QLineEdit(this);
    connect(destinationEdit_, &QLineEdit::textEdited, this, [this](const QString&) {
        userEditedDestination_ = true;
    });
    connect(destinationEdit_, &QLineEdit::textChanged, this, &ExportDialog::onDestinationTextChanged);
    pathEditLayout->addWidget(destinationEdit_, 1);

    browseButton_ = new QPushButton(QStringLiteral("Browse..."), this);
    browseButton_->setStyleSheet(design::kSecondaryButtonStyle);
    connect(browseButton_, &QPushButton::clicked, this, &ExportDialog::onBrowseClicked);
    pathEditLayout->addWidget(browseButton_);

    destLayout->addLayout(pathEditLayout);
    rightPane->addLayout(destLayout);

    // Status / feedback label (for clipboard copy feedback)
    feedbackLabel_ = new QLabel(this);
    feedbackLabel_->setVisible(false);
    rightPane->addWidget(feedbackLabel_);

    rightPane->addStretch(1);
    contentLayout->addLayout(rightPane, 1);
    rootLayout->addLayout(contentLayout, 1);

    // Bottom Button Bar
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(design::kSpace3);

    copyClipboardButton_ = new QPushButton(QStringLiteral("Copy to Clipboard"), this);
    copyClipboardButton_->setStyleSheet(design::kSecondaryButtonStyle);
    connect(copyClipboardButton_, &QPushButton::clicked, this, &ExportDialog::onCopyToClipboardClicked);
    buttonLayout->addWidget(copyClipboardButton_);

    buttonLayout->addStretch(1);

    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancelButton_->setStyleSheet(design::kSecondaryButtonStyle);
    connect(cancelButton_, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelButton_);

    // Primary Button: Exactly one primary button in dialog
    exportButton_ = new QPushButton(QStringLiteral("Export"), this);
    exportButton_->setDefault(true);
    exportButton_->setStyleSheet(design::kPrimaryButtonStyle);
    connect(exportButton_, &QPushButton::clicked, this, &QDialog::accept);
    buttonLayout->addWidget(exportButton_);

    rootLayout->addLayout(buttonLayout);
}

void ExportDialog::populateFormats() {
    formats_ = MachineIoRegistry::instance().exportFormats();
    formatList_->clear();

    for (const auto& fmt : formats_) {
        auto* item = new QListWidgetItem(fmt.name, formatList_);
        item->setData(Qt::UserRole, fmt.id);
        item->setSizeHint(QSize(kFormatItemWidth, design::kRowHeight));
    }

    if (formatList_->count() > 0) {
        formatList_->setCurrentRow(0);
    }
}

std::shared_ptr<MachineFormatAdapter> ExportDialog::currentAdapter() const {
    QListWidgetItem* item = formatList_->currentItem();
    if (!item) return nullptr;
    const QString id = item->data(Qt::UserRole).toString();
    return MachineIoRegistry::instance().findAdapterById(id);
}

void ExportDialog::onFormatSelected(QListWidgetItem* current, QListWidgetItem* previous) {
    Q_UNUSED(current);
    Q_UNUSED(previous);
    updateForSelectedFormat();
}

void ExportDialog::updateForSelectedFormat() {
    auto adapter = currentAdapter();
    if (!adapter) return;

    const MachineFormatDescriptor desc = adapter->descriptor();
    formatTitleLabel_->setText(desc.name);
    formatExtBadge_->setText(QStringLiteral(".%1").arg(desc.defaultExtension));
    formatDescLabel_->setText(desc.description);

    // Update Scope radio availability
    const bool supportsSingle = desc.supportedScopes.testFlag(ExportScope::SingleMachine);
    const bool supportsDir = desc.supportedScopes.testFlag(ExportScope::MultipleMachinesDirectory);
    const bool supportsBundle = desc.supportedScopes.testFlag(ExportScope::MultipleMachinesSingleFile);

    const int totalMachines = config_.allMachines.size();

    scopeSingleRadio_->setEnabled(supportsSingle && config_.activeMachine != nullptr);

    const bool canDir = supportsDir && (totalMachines > 1);
    scopeDirectoryRadio_->setEnabled(canDir);
    if (!supportsDir) {
        scopeDirectoryRadio_->setToolTip(QStringLiteral("Format does not support directory export"));
    } else if (totalMachines <= 1) {
        scopeDirectoryRadio_->setToolTip(QStringLiteral("Project contains only one machine"));
    } else {
        scopeDirectoryRadio_->setToolTip(QString());
    }

    const bool canBundle = supportsBundle && (totalMachines > 1);
    scopeBundleRadio_->setEnabled(canBundle);
    if (!supportsBundle) {
        scopeBundleRadio_->setToolTip(QStringLiteral("Format does not support bundling multiple machines"));
    } else if (totalMachines <= 1) {
        scopeBundleRadio_->setToolTip(QStringLiteral("Project contains only one machine"));
    } else {
        scopeBundleRadio_->setToolTip(QString());
    }

    // If current selected scope is disabled, fall back to single machine
    QAbstractButton* checkedRadio = scopeGroup_->checkedButton();
    if (checkedRadio && !checkedRadio->isEnabled()) {
        scopeSingleRadio_->setChecked(true);
    }

    rebuildOptions(desc);
    updateDefaultDestination();
    updateButtonsState();
}

void ExportDialog::rebuildOptions(const MachineFormatDescriptor& desc) {
    // Clear old options
    optionBindings_.clear();
    while (QLayoutItem* item = optionsLayout_->takeAt(0)) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }

    if (desc.options.isEmpty()) {
        // kTypeLabel with the text-disabled color override.
        auto* noneLabel = new QLabel(QStringLiteral("No additional options for this format."), optionsContainer_);
        noneLabel->setStyleSheet(design::kTypeLabel +
                                  design::resolveRoles(QStringLiteral("color: {text-disabled};")));
        optionsLayout_->addWidget(noneLabel);
        return;
    }

    for (const FormatOption& opt : desc.options) {
        switch (opt.type) {
            case OptionType::Bool: {
                auto* cb = new QCheckBox(opt.label, optionsContainer_);
                cb->setToolTip(opt.description);
                cb->setChecked(opt.defaultValue.toBool());
                optionsLayout_->addWidget(cb);
                optionBindings_.append({opt.id, OptionType::Bool, cb});
                break;
            }
            case OptionType::Enum: {
                auto* row = new QWidget(optionsContainer_);
                auto* rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(QMargins());
                rowLayout->setSpacing(design::kSpace2);

                auto* lbl = new QLabel(opt.label, row);
                lbl->setStyleSheet(design::kTypeLabel);
                rowLayout->addWidget(lbl);

                auto* combo = new QComboBox(row);
                combo->addItems(opt.choices);
                combo->setCurrentText(opt.defaultValue.toString());
                combo->setToolTip(opt.description);
                rowLayout->addWidget(combo, 1);

                optionsLayout_->addWidget(row);
                optionBindings_.append({opt.id, OptionType::Enum, combo});
                break;
            }
            case OptionType::String: {
                auto* row = new QWidget(optionsContainer_);
                auto* rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(QMargins());
                rowLayout->setSpacing(design::kSpace2);

                auto* lbl = new QLabel(opt.label, row);
                lbl->setStyleSheet(design::kTypeLabel);
                rowLayout->addWidget(lbl);

                auto* edit = new QLineEdit(row);
                edit->setText(opt.defaultValue.toString());
                edit->setToolTip(opt.description);
                rowLayout->addWidget(edit, 1);

                optionsLayout_->addWidget(row);
                optionBindings_.append({opt.id, OptionType::String, edit});
                break;
            }
        }
    }
}

void ExportDialog::updateDefaultDestination() {
    auto adapter = currentAdapter();
    if (!adapter) return;

    const MachineFormatDescriptor desc = adapter->descriptor();
    const QString baseDir = !config_.defaultDirectory.isEmpty() ? config_.defaultDirectory : QDir::currentPath();
    const ExportScope scope = currentScope();

    if (scope == ExportScope::SingleMachine) {
        destinationLabel_->setText(QStringLiteral("File"));
        if (!userEditedDestination_) {
            QString name = !config_.activeMachineName.isEmpty() ? config_.activeMachineName : QStringLiteral("machine");
            if (name.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) ||
                name.endsWith(QStringLiteral(".sdm"), Qt::CaseInsensitive) ||
                name.endsWith(QStringLiteral(".scxml"), Qt::CaseInsensitive) ||
                name.endsWith(QStringLiteral(".puml"), Qt::CaseInsensitive) ||
                name.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)) {
                name = QFileInfo(name).completeBaseName();
            }
            const QString safeName = sanitizeFileName(name);
            const QString fileName = QStringLiteral("%1.%2").arg(safeName, desc.defaultExtension);
            destinationEdit_->setText(QDir(baseDir).filePath(fileName));
        } else {
            // Keep user-edited directory and base filename, but synchronize extension with selected format
            const QString cur = destinationEdit_->text().trimmed();
            if (!cur.isEmpty()) {
                QFileInfo fi(cur);
                const QString base = fi.completeBaseName();
                if (!base.isEmpty() && fi.suffix().compare(desc.defaultExtension, Qt::CaseInsensitive) != 0) {
                    const QString updated = fi.dir().filePath(QStringLiteral("%1.%2").arg(base, desc.defaultExtension));
                    destinationEdit_->setText(updated);
                }
            }
        }
    } else if (scope == ExportScope::MultipleMachinesDirectory) {
        destinationLabel_->setText(QStringLiteral("Directory"));
        if (!userEditedDestination_) {
            const QString dirName = QStringLiteral("export_%1").arg(desc.id);
            destinationEdit_->setText(QDir(baseDir).filePath(dirName));
        }
    } else if (scope == ExportScope::MultipleMachinesSingleFile) {
        destinationLabel_->setText(QStringLiteral("File"));
        if (!userEditedDestination_) {
            const QString fileName = QStringLiteral("project_bundle.%1").arg(desc.defaultExtension);
            destinationEdit_->setText(QDir(baseDir).filePath(fileName));
        } else {
            const QString cur = destinationEdit_->text().trimmed();
            if (!cur.isEmpty()) {
                QFileInfo fi(cur);
                const QString base = fi.completeBaseName();
                if (!base.isEmpty() && fi.suffix().compare(desc.defaultExtension, Qt::CaseInsensitive) != 0) {
                    const QString updated = fi.dir().filePath(QStringLiteral("%1.%2").arg(base, desc.defaultExtension));
                    destinationEdit_->setText(updated);
                }
            }
        }
    }
}

void ExportDialog::updateButtonsState() {
    auto adapter = currentAdapter();
    const bool hasAdapter = (adapter != nullptr);
    const bool hasDestination = !destinationEdit_->text().trimmed().isEmpty();

    exportButton_->setEnabled(hasAdapter && hasDestination);

    if (hasAdapter) {
        const MachineFormatDescriptor desc = adapter->descriptor();
        const bool isText = (desc.payloadKind == PayloadKind::Text);
        const bool notDir = (currentScope() != ExportScope::MultipleMachinesDirectory);
        copyClipboardButton_->setEnabled(isText && notDir && (config_.activeMachine != nullptr || currentScope() == ExportScope::MultipleMachinesSingleFile));
    } else {
        copyClipboardButton_->setEnabled(false);
    }
}

void ExportDialog::onScopeChanged(int id) {
    Q_UNUSED(id);
    userEditedDestination_ = false;
    updateDefaultDestination();
    updateButtonsState();
}

void ExportDialog::onDestinationTextChanged(const QString& text) {
    Q_UNUSED(text);
    updateButtonsState();
}

void ExportDialog::onBrowseClicked() {
    auto adapter = currentAdapter();
    if (!adapter) return;

    const MachineFormatDescriptor desc = adapter->descriptor();
    const ExportScope scope = currentScope();
    const QString currentPath = destinationEdit_->text().trimmed();
    const QString startDir = currentPath.isEmpty() ? config_.defaultDirectory : QFileInfo(currentPath).absolutePath();

    if (scope == ExportScope::MultipleMachinesDirectory) {
        const QString chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Select Destination Directory"), startDir);
        if (!chosen.isEmpty()) {
            destinationEdit_->setText(chosen);
            userEditedDestination_ = true;
        }
    } else {
        const QString filter = MachineIoRegistry::instance().filterStringForFormat(desc);
        const QString chosen = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export %1").arg(desc.name), currentPath, filter);
        if (!chosen.isEmpty()) {
            destinationEdit_->setText(chosen);
            userEditedDestination_ = true;
        }
    }
}

void ExportDialog::onCopyToClipboardClicked() {
    auto adapter = currentAdapter();
    if (!adapter) return;

    const MachineFormatDescriptor desc = adapter->descriptor();
    if (desc.payloadKind != PayloadKind::Text) return;

    const ExportScope scope = currentScope();
    const FormatOptionMap opts = currentOptions();

    if (scope == ExportScope::SingleMachine) {
        if (!config_.activeMachine) {
            feedbackLabel_->setText(QStringLiteral("No active machine to export"));
            feedbackLabel_->setStyleSheet(kFeedbackDangerStyle);
            feedbackLabel_->setVisible(true);
            return;
        }

        ExportJobResult res = exportMachineToText(adapter, *config_.activeMachine, opts);
        if (res.ok) {
            QGuiApplication::clipboard()->setText(res.textPayload);
            feedbackLabel_->setText(QStringLiteral("Copied to clipboard"));
            feedbackLabel_->setStyleSheet(kFeedbackSuccessStyle);
            feedbackLabel_->setVisible(true);
        } else {
            feedbackLabel_->setText(QStringLiteral("Copy failed: %1").arg(res.error));
            feedbackLabel_->setStyleSheet(kFeedbackDangerStyle);
            feedbackLabel_->setVisible(true);
        }
    } else if (scope == ExportScope::MultipleMachinesSingleFile) {
        MachineSerializeResult ser = adapter->serialize(config_.allMachines, opts);
        if (ser.ok) {
            QGuiApplication::clipboard()->setText(QString::fromUtf8(ser.payload));
            feedbackLabel_->setText(QStringLiteral("Copied bundle to clipboard"));
            feedbackLabel_->setStyleSheet(kFeedbackSuccessStyle);
            feedbackLabel_->setVisible(true);
        } else {
            feedbackLabel_->setText(QStringLiteral("Copy failed: %1").arg(ser.error));
            feedbackLabel_->setStyleSheet(kFeedbackDangerStyle);
            feedbackLabel_->setVisible(true);
        }
    }
}

ExportJob ExportDialog::exportJob() const {
    ExportJob job;
    job.adapter = currentAdapter();
    job.scope = currentScope();
    job.options = currentOptions();
    job.destinationPath = destinationPath();

    if (job.scope == ExportScope::SingleMachine) {
        if (config_.activeMachine) {
            job.machines.append(config_.activeMachine);
        }
    } else {
        job.machines = config_.allMachines;
    }

    return job;
}

void ExportDialog::selectFormat(const QString& formatId) {
    for (int i = 0; i < formatList_->count(); ++i) {
        QListWidgetItem* item = formatList_->item(i);
        if (item && item->data(Qt::UserRole).toString() == formatId) {
            formatList_->setCurrentItem(item);
            return;
        }
    }
}

QString ExportDialog::selectedFormatId() const {
    QListWidgetItem* item = formatList_->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void ExportDialog::setScope(ExportScope scope) {
    switch (scope) {
        case ExportScope::SingleMachine:
            if (scopeSingleRadio_->isEnabled()) scopeSingleRadio_->setChecked(true);
            break;
        case ExportScope::MultipleMachinesDirectory:
            if (scopeDirectoryRadio_->isEnabled()) scopeDirectoryRadio_->setChecked(true);
            break;
        case ExportScope::MultipleMachinesSingleFile:
            if (scopeBundleRadio_->isEnabled()) scopeBundleRadio_->setChecked(true);
            break;
    }
    userEditedDestination_ = false;
    updateDefaultDestination();
    updateButtonsState();
}

ExportScope ExportDialog::currentScope() const {
    const int id = scopeGroup_->checkedId();
    return static_cast<ExportScope>(id);
}

void ExportDialog::setOptionValue(const QString& optionId, const QVariant& value) {
    for (const auto& binding : optionBindings_) {
        if (binding.id == optionId) {
            switch (binding.type) {
                case OptionType::Bool: {
                    if (auto* cb = qobject_cast<QCheckBox*>(binding.widget)) {
                        cb->setChecked(value.toBool());
                    }
                    break;
                }
                case OptionType::Enum: {
                    if (auto* combo = qobject_cast<QComboBox*>(binding.widget)) {
                        combo->setCurrentText(value.toString());
                    }
                    break;
                }
                case OptionType::String: {
                    if (auto* edit = qobject_cast<QLineEdit*>(binding.widget)) {
                        edit->setText(value.toString());
                    }
                    break;
                }
            }
            return;
        }
    }
}

QVariant ExportDialog::optionValue(const QString& optionId) const {
    for (const auto& binding : optionBindings_) {
        if (binding.id == optionId) {
            switch (binding.type) {
                case OptionType::Bool: {
                    if (auto* cb = qobject_cast<QCheckBox*>(binding.widget)) {
                        return cb->isChecked();
                    }
                    break;
                }
                case OptionType::Enum: {
                    if (auto* combo = qobject_cast<QComboBox*>(binding.widget)) {
                        return combo->currentText();
                    }
                    break;
                }
                case OptionType::String: {
                    if (auto* edit = qobject_cast<QLineEdit*>(binding.widget)) {
                        return edit->text();
                    }
                    break;
                }
            }
        }
    }
    return QVariant();
}

FormatOptionMap ExportDialog::currentOptions() const {
    FormatOptionMap map;
    for (const auto& binding : optionBindings_) {
        switch (binding.type) {
            case OptionType::Bool: {
                if (auto* cb = qobject_cast<QCheckBox*>(binding.widget)) {
                    map.insert(binding.id, cb->isChecked());
                }
                break;
            }
            case OptionType::Enum: {
                if (auto* combo = qobject_cast<QComboBox*>(binding.widget)) {
                    map.insert(binding.id, combo->currentText());
                }
                break;
            }
            case OptionType::String: {
                if (auto* edit = qobject_cast<QLineEdit*>(binding.widget)) {
                    map.insert(binding.id, edit->text());
                }
                break;
            }
        }
    }
    return map;
}

void ExportDialog::setDestinationPath(const QString& path) {
    destinationEdit_->setText(path);
    userEditedDestination_ = true;
    updateButtonsState();
}

QString ExportDialog::destinationPath() const {
    return destinationEdit_->text().trimmed();
}

bool ExportDialog::isCopyToClipboardEnabled() const {
    return copyClipboardButton_->isEnabled();
}

bool ExportDialog::isExportEnabled() const {
    return exportButton_->isEnabled();
}

void ExportDialog::clickCopyToClipboard() {
    if (copyClipboardButton_->isEnabled()) {
        copyClipboardButton_->click();
    }
}

void ExportDialog::clickExport() {
    if (exportButton_->isEnabled()) {
        exportButton_->click();
    }
}

void ExportDialog::clickCancel() {
    cancelButton_->click();
}

QString ExportDialog::feedbackMessage() const {
    return feedbackLabel_->text();
}

}  // namespace app
