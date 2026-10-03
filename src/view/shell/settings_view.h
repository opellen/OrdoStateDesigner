#pragma once

#include <functional>

#include <QFrame>
#include <QHash>
#include <QJsonValue>
#include <QWidget>
#include <vector>

#include "infra/settings_registry.h"
#include "infra/settings_store.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QVBoxLayout;

namespace app {

// Window onto ProjectOnly settings, which live in the project manifest rather
// than settings.json; MainWindow supplies it. `read` returns a null
// QJsonValue for an unknown key; `write` is a no-op with no project open.
struct ProjectSettingsAccess {
    std::function<bool()> isProjectOpen;
    std::function<QJsonValue(const QString& key)> read;
    std::function<void(const QString& key, const QJsonValue& value)> write;
};

// Single setting item card widget in the right scroll area
class SettingItemCard : public QFrame {
    Q_OBJECT

public:
    explicit SettingItemCard(const SettingDefinition& def, SettingsStore& store,
                              ProjectSettingsAccess projectAccess, QWidget* parent = nullptr);

    const SettingDefinition& definition() const { return def_; }
    void refreshValue(StoreScope currentScope);
    bool matchesFilter(const QString& filterText, const QString& selectedCategory) const;

signals:
    void valueChanged(const QString& key, const QJsonValue& val);
    void resetRequested(const QString& key);

private:
    void setupUi();
    void updateModifiedState(StoreScope currentScope);
    // ProjectOnly value lookup; an empty/absent field reads as the registry default.
    QJsonValue projectValueOrDefault() const;

    SettingDefinition def_;
    SettingsStore& store_;
    ProjectSettingsAccess projectAccess_;
    QCheckBox* boolEditor_ = nullptr;
    QSpinBox* intEditor_ = nullptr;
    QComboBox* enumEditor_ = nullptr;
    QLineEdit* stringEditor_ = nullptr;

    QLabel* modifiedIndicator_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    // ProjectOnly-only guidance shown in place of the editor when no project
    // is open (disabled and explained, never hidden).
    QLabel* projectClosedHint_ = nullptr;
    bool updatingProgrammatically_ = false;
};

// VS Code-style Settings View tab editor.
// Schema-driven, supports 2-tier scope switching (User vs Workspace),
// instant search filtering, category tree, and direct settings.json access.
// Consumes an injected SettingsStore application service.
class SettingsView : public QWidget {
    Q_OBJECT

public:
    explicit SettingsView(SettingsStore& store, ProjectSettingsAccess projectAccess, QWidget* parent = nullptr);

    StoreScope activeScope() const { return currentScope_; }
    void setActiveScope(StoreScope scope);
    void setSearchQuery(const QString& query);

signals:
    void openJsonRequested(StoreScope scope);

private slots:
    void handleSettingChangedInStore(const QString& key, const QJsonValue& oldVal,
                                     const QJsonValue& newVal, StoreScope scope);
    void applyFilter();
    void handleOpenJson();

private:
    void setupUi();
    void populateCategories();
    void populateCards();

    SettingsStore& store_;
    ProjectSettingsAccess projectAccess_;
    StoreScope currentScope_ = StoreScope::User;
    QLineEdit* searchEdit_ = nullptr;
    QPushButton* userScopeBtn_ = nullptr;
    QPushButton* workspaceScopeBtn_ = nullptr;
    QPushButton* openJsonBtn_ = nullptr;

    QListWidget* categoryList_ = nullptr;
    QScrollArea* scrollArea_ = nullptr;
    QWidget* cardsContainer_ = nullptr;
    QVBoxLayout* cardsLayout_ = nullptr;
    QLabel* emptySearchLabel_ = nullptr;

    std::vector<SettingItemCard*> cards_;
    QHash<QString, SettingItemCard*> cardMap_;
};

}  // namespace app
