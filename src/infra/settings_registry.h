#pragma once

#include <QHash>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <vector>

namespace app {

enum class SettingType {
    Bool,
    Int,
    Double,
    String,
    Enum,
    FilePath,
    DirPath,
    Color
};

enum class SettingScope {
    UserOnly,       // Global settings only (e.g. license keys, global editor prefs)
    WorkspaceOnly,  // Workspace-bound only (.sd/settings.json), not project paths;
                    // those are ProjectOnly.
    Overridable,    // Default to user, can be overridden in workspace
    ProjectOnly     // Lives in the .sdp Project manifest, not settings.json --
                    // read/written through ProjectSettingsAccess (settings_view.h).
};

struct SettingDefinition {
    QString key;                  // Dotted unique key, e.g. "canvas.gridStep"
    QString title;                // Human-readable title: "Grid Step Size"
    QString description;          // Description / hint text
    SettingType type = SettingType::String;
    QJsonValue defaultValue;      // Default value
    SettingScope scope = SettingScope::Overridable;
    QString category;             // Category: "Editor", "Canvas", "CodeGen", "DevMode", "Plugins"

    // Optional constraints
    double minimum = 0.0;
    double maximum = 1000.0;
    QStringList enumOptions;      // For SettingType::Enum
    QString fileFilter;           // For FilePath
};

// Schema-driven registry for State Designer settings.
// Holds declarative definitions for core settings and future plugin contributions.
class SettingsRegistry {
public:
    static SettingsRegistry& instance();

    void registerSetting(SettingDefinition def);
    const SettingDefinition* find(const QString& key) const;
    bool has(const QString& key) const;

    const std::vector<SettingDefinition>& allSettings() const { return definitions_; }
    std::vector<SettingDefinition> settingsForCategory(const QString& category) const;
    const QStringList& categories() const { return categories_; }

    void registerCoreSettings();
    void clear();

private:
    SettingsRegistry();

    std::vector<SettingDefinition> definitions_;
    QHash<QString, std::size_t> keyIndexMap_;
    QStringList categories_;
};

}  // namespace app
