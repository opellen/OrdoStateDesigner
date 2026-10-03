#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace app {

// Which storage tier an edit or query targets.
enum class StoreScope {
    User,       // Global user-level settings (%APPDATA%/state-designer/settings.json)
    Workspace   // Project-level settings (.sd/settings.json)
};

// 2-Tier configuration storage engine.
// Manages reading, writing, and merging user and workspace settings.json files.
// Priority resolution: Workspace > User > Default fallback.
// In-memory reads and mutations are instantaneous; disk persistence is debounced (300ms)
// and executed via atomic temporary file renaming without blocking the GUI thread.
class SettingsStore : public QObject {
    Q_OBJECT

public:
    explicit SettingsStore(QObject* parent = nullptr);
    SettingsStore(QString userPath, QString workspacePath = QString(), QObject* parent = nullptr);
    ~SettingsStore() override;

    // Active application-wide store pointer (managed by MainWindow or test host)
    static SettingsStore* activeStore();
    static void setActiveStore(SettingsStore* store);

signals:
    void settingChanged(const QString& key, const QJsonValue& oldValue, const QJsonValue& newValue, StoreScope scope);
    void flushCompleted(StoreScope scope);

public:
    // Path configuration
    void setUserPath(const QString& path);
    void setWorkspacePath(const QString& path);
    QString userPath() const { return userPath_; }
    QString workspacePath() const { return workspacePath_; }

    // File I/O
    bool loadUser();
    bool saveUser();
    bool loadWorkspace();
    bool saveWorkspace();

    // Debounced and Synchronous Flush
    void scheduleDebouncedSave(StoreScope scope);
    void flushSync();

    // In-memory string serialization (ideal for headless testing)
    bool loadUserFromJson(const QString& jsonString);
    QString saveUserToJson() const;
    bool loadWorkspaceFromJson(const QString& jsonString);
    QString saveWorkspaceToJson() const;

    // Value access with priority: Workspace -> User -> fallback
    QJsonValue get(const QString& key, const QJsonValue& fallback = QJsonValue()) const;

    // Tier-specific access
    QJsonValue getValueInScope(const QString& key, StoreScope scope) const;
    bool has(const QString& key, StoreScope scope) const;
    bool hasAny(const QString& key) const;

    // Mutation (automatically schedules debounced background persistence)
    void set(const QString& key, const QJsonValue& value, StoreScope scope = StoreScope::User);
    void remove(const QString& key, StoreScope scope);
    void clear(StoreScope scope);

    // Metadata queries
    bool isOverriddenInWorkspace(const QString& key) const;
    StoreScope effectiveScope(const QString& key) const;
    QStringList allKeys() const;

    // Raw object access
    const QJsonObject& userObject() const { return userObj_; }
    const QJsonObject& workspaceObject() const { return workspaceObj_; }

    // Helper for default OS location
    static QString defaultUserPath();

    // Atomic disk persistence helper (writes to unique temp file, flushes, then renames)
    static bool atomicWriteJsonFile(const QString& targetPath, const QString& jsonContent);

private:
    void setupDebounceTimer();
    void performDebouncedFlush();

    static QJsonValue lookupKey(const QJsonObject& root, const QString& key);
    static void insertKey(QJsonObject& root, const QString& key, const QJsonValue& value);
    static void removeKey(QJsonObject& root, const QString& key);

    QString userPath_;
    QString workspacePath_;
    QJsonObject userObj_;
    QJsonObject workspaceObj_;

    QTimer debounceTimer_;
    bool pendingUserSave_ = false;
    bool pendingWorkspaceSave_ = false;

    static inline SettingsStore* s_activeStore = nullptr;
};

}  // namespace app
