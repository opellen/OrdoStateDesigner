#pragma once

#include <QDialog>
#include <QList>
#include <QString>
#include <QVariant>
#include <QVector>
#include <memory>

#include "infra/machine_io.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QRadioButton;
class QVBoxLayout;

namespace app {

class Machine;

struct ExportDialogConfig {
    const Machine* activeMachine = nullptr;
    QString activeMachineName;
    QList<const Machine*> allMachines;
    QString defaultDirectory;
};

class ExportDialog : public QDialog {
    Q_OBJECT

public:
    explicit ExportDialog(const ExportDialogConfig& config, QWidget* parent = nullptr);
    ~ExportDialog() override;

    // Configured export job to be executed by caller
    ExportJob exportJob() const;

    // Programmatic / test inspection and manipulation
    void selectFormat(const QString& formatId);
    QString selectedFormatId() const;

    void setScope(ExportScope scope);
    ExportScope currentScope() const;

    void setOptionValue(const QString& optionId, const QVariant& value);
    QVariant optionValue(const QString& optionId) const;
    FormatOptionMap currentOptions() const;

    void setDestinationPath(const QString& path);
    QString destinationPath() const;

    bool isCopyToClipboardEnabled() const;
    bool isExportEnabled() const;

    void clickCopyToClipboard();
    void clickExport();
    void clickCancel();

    QString feedbackMessage() const;

private slots:
    void onFormatSelected(QListWidgetItem* current, QListWidgetItem* previous);
    void onScopeChanged(int id);
    void onBrowseClicked();
    void onCopyToClipboardClicked();
    void onDestinationTextChanged(const QString& text);

private:
    void setupUi();
    void populateFormats();
    void updateForSelectedFormat();
    void rebuildOptions(const MachineFormatDescriptor& desc);
    void updateDefaultDestination();
    void updateButtonsState();
    std::shared_ptr<MachineFormatAdapter> currentAdapter() const;

    ExportDialogConfig config_;
    QVector<MachineFormatDescriptor> formats_;

    // UI elements
    QListWidget* formatList_ = nullptr;
    QLabel* formatTitleLabel_ = nullptr;
    QLabel* formatExtBadge_ = nullptr;
    QLabel* formatDescLabel_ = nullptr;

    QRadioButton* scopeSingleRadio_ = nullptr;
    QRadioButton* scopeDirectoryRadio_ = nullptr;
    QRadioButton* scopeBundleRadio_ = nullptr;
    QButtonGroup* scopeGroup_ = nullptr;

    QWidget* optionsContainer_ = nullptr;
    QVBoxLayout* optionsLayout_ = nullptr;
    struct OptionWidgetBinding {
        QString id;
        OptionType type;
        QWidget* widget;
    };
    QVector<OptionWidgetBinding> optionBindings_;

    QLabel* destinationLabel_ = nullptr;
    QLineEdit* destinationEdit_ = nullptr;
    QPushButton* browseButton_ = nullptr;

    QLabel* feedbackLabel_ = nullptr;

    QPushButton* copyClipboardButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;

    bool userEditedDestination_ = false;
};

}  // namespace app
