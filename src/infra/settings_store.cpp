#include "infra/settings_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QThreadPool>

namespace app {

SettingsStore::SettingsStore(QObject* parent)
    : QObject(parent), userPath_(defaultUserPath()) {
    setupDebounceTimer();
}

SettingsStore::SettingsStore(QString userPath, QString workspacePath, QObject* parent)
    : QObject(parent), userPath_(std::move(userPath)), workspacePath_(std::move(workspacePath)) {
    setupDebounceTimer();
}

SettingsStore::~SettingsStore() {
    flushSync();
    if (s_activeStore == this) {
        s_activeStore = nullptr;
    }
}

SettingsStore* SettingsStore::activeStore() {
    return s_activeStore;
}

void SettingsStore::setActiveStore(SettingsStore* store) {
    s_activeStore = store;
}

void SettingsStore::setupDebounceTimer() {
    debounceTimer_.setSingleShot(true);
    connect(&debounceTimer_, &QTimer::timeout, this, &SettingsStore::performDebouncedFlush);
}

void SettingsStore::setUserPath(const QString& path) {
    userPath_ = path;
}

void SettingsStore::setWorkspacePath(const QString& path) {
    workspacePath_ = path;
}

QString SettingsStore::defaultUserPath() {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configDir.isEmpty()) {
        configDir = QDir::homePath() + QStringLiteral("/.state-designer");
    }
    return configDir + QStringLiteral("/settings.json");
}

bool SettingsStore::atomicWriteJsonFile(const QString& targetPath, const QString& jsonContent) {
    if (targetPath.isEmpty()) {
        return false;
    }
    QFileInfo info(targetPath);
    QDir dir = info.dir();
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }

    const QString tempPath = targetPath + QStringLiteral(".tmp.") + QString::number(QCoreApplication::applicationPid())
                             + QStringLiteral(".") + QString::number(QRandomGenerator::global()->generate());

    QFile tempFile(tempPath);
    if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    tempFile.write(jsonContent.toUtf8());
    tempFile.flush();
    tempFile.close();

    if (QFile::exists(targetPath)) {
        QFile::remove(targetPath);
    }
    return tempFile.rename(targetPath);
}

bool SettingsStore::loadUser() {
    if (userPath_.isEmpty()) {
        return false;
    }
    QFile file(userPath_);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray bytes = file.readAll();
    return loadUserFromJson(QString::fromUtf8(bytes));
}

bool SettingsStore::saveUser() {
    pendingUserSave_ = false;
    bool ok = atomicWriteJsonFile(userPath_, saveUserToJson());
    if (ok) {
        emit flushCompleted(StoreScope::User);
    }
    return ok;
}

bool SettingsStore::loadWorkspace() {
    if (workspacePath_.isEmpty()) {
        return false;
    }
    QFile file(workspacePath_);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QByteArray bytes = file.readAll();
    return loadWorkspaceFromJson(QString::fromUtf8(bytes));
}

bool SettingsStore::saveWorkspace() {
    pendingWorkspaceSave_ = false;
    bool ok = atomicWriteJsonFile(workspacePath_, saveWorkspaceToJson());
    if (ok) {
        emit flushCompleted(StoreScope::Workspace);
    }
    return ok;
}

void SettingsStore::scheduleDebouncedSave(StoreScope scope) {
    if (scope == StoreScope::Workspace) {
        pendingWorkspaceSave_ = true;
    } else {
        pendingUserSave_ = true;
    }
    debounceTimer_.start(300);
}

void SettingsStore::performDebouncedFlush() {
    const bool saveUser = pendingUserSave_;
    const bool saveWs = pendingWorkspaceSave_;
    pendingUserSave_ = false;
    pendingWorkspaceSave_ = false;

    const QString userP = userPath_;
    const QString userJson = saveUserToJson();
    const QString wsP = workspacePath_;
    const QString wsJson = saveWorkspaceToJson();

    QPointer<SettingsStore> self(this);
    QThreadPool::globalInstance()->start([self, saveUser, userP, userJson, saveWs, wsP, wsJson]() {
        if (saveUser && !userP.isEmpty()) {
            atomicWriteJsonFile(userP, userJson);
            if (self) {
                QMetaObject::invokeMethod(self.data(), [self]() {
                    if (self) emit self->flushCompleted(StoreScope::User);
                }, Qt::QueuedConnection);
            }
        }
        if (saveWs && !wsP.isEmpty()) {
            atomicWriteJsonFile(wsP, wsJson);
            if (self) {
                QMetaObject::invokeMethod(self.data(), [self]() {
                    if (self) emit self->flushCompleted(StoreScope::Workspace);
                }, Qt::QueuedConnection);
            }
        }
    });
}

void SettingsStore::flushSync() {
    if (debounceTimer_.isActive()) {
        debounceTimer_.stop();
    }
    if (pendingUserSave_ && !userPath_.isEmpty()) {
        atomicWriteJsonFile(userPath_, saveUserToJson());
        pendingUserSave_ = false;
        emit flushCompleted(StoreScope::User);
    }
    if (pendingWorkspaceSave_ && !workspacePath_.isEmpty()) {
        atomicWriteJsonFile(workspacePath_, saveWorkspaceToJson());
        pendingWorkspaceSave_ = false;
        emit flushCompleted(StoreScope::Workspace);
    }
}

bool SettingsStore::loadUserFromJson(const QString& jsonString) {
    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(jsonString.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    userObj_ = doc.object();
    return true;
}

QString SettingsStore::saveUserToJson() const {
    QJsonDocument doc(userObj_);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

bool SettingsStore::loadWorkspaceFromJson(const QString& jsonString) {
    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(jsonString.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    workspaceObj_ = doc.object();
    return true;
}

QString SettingsStore::saveWorkspaceToJson() const {
    QJsonDocument doc(workspaceObj_);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

QJsonValue SettingsStore::lookupKey(const QJsonObject& root, const QString& key) {
    // 1. Direct flat match (standard VS Code representation, e.g. "canvas.gridStep")
    if (root.contains(key)) {
        return root.value(key);
    }

    // 2. Nested dot notation fallback (e.g. root["canvas"]["gridStep"])
    if (!key.contains(QLatin1Char('.'))) {
        return QJsonValue(QJsonValue::Undefined);
    }

    QStringList parts = key.split(QLatin1Char('.'));
    QJsonObject current = root;
    for (int i = 0; i < parts.size() - 1; ++i) {
        if (!current.contains(parts[i]) || !current.value(parts[i]).isObject()) {
            return QJsonValue(QJsonValue::Undefined);
        }
        current = current.value(parts[i]).toObject();
    }
    const QString& leaf = parts.last();
    if (current.contains(leaf)) {
        return current.value(leaf);
    }

    return QJsonValue(QJsonValue::Undefined);
}

void SettingsStore::insertKey(QJsonObject& root, const QString& key, const QJsonValue& value) {
    // We store using flat dot-delimited keys directly at root level
    root.insert(key, value);
}

void SettingsStore::removeKey(QJsonObject& root, const QString& key) {
    if (root.contains(key)) {
        root.remove(key);
        return;
    }
    // Also remove from nested if present
    if (key.contains(QLatin1Char('.'))) {
        QStringList parts = key.split(QLatin1Char('.'));
        auto removeNested = [](auto& self, QJsonObject& obj, const QStringList& path, int index) -> bool {
            if (index == path.size() - 1) {
                obj.remove(path[index]);
                return true;
            }
            const QString& seg = path[index];
            if (obj.contains(seg) && obj.value(seg).isObject()) {
                QJsonObject child = obj.value(seg).toObject();
                if (self(self, child, path, index + 1)) {
                    if (child.isEmpty()) {
                        obj.remove(seg);
                    } else {
                        obj[seg] = child;
                    }
                    return true;
                }
            }
            return false;
        };
        removeNested(removeNested, root, parts, 0);
    }
}

QJsonValue SettingsStore::get(const QString& key, const QJsonValue& fallback) const {
    // 1. Workspace tier (highest priority)
    QJsonValue wsVal = lookupKey(workspaceObj_, key);
    if (!wsVal.isUndefined()) {
        return wsVal;
    }

    // 2. User tier
    QJsonValue userVal = lookupKey(userObj_, key);
    if (!userVal.isUndefined()) {
        return userVal;
    }

    // 3. Default fallback
    return fallback;
}

QJsonValue SettingsStore::getValueInScope(const QString& key, StoreScope scope) const {
    const QJsonObject& obj = (scope == StoreScope::Workspace) ? workspaceObj_ : userObj_;
    return lookupKey(obj, key);
}

bool SettingsStore::has(const QString& key, StoreScope scope) const {
    const QJsonObject& obj = (scope == StoreScope::Workspace) ? workspaceObj_ : userObj_;
    return !lookupKey(obj, key).isUndefined();
}

bool SettingsStore::hasAny(const QString& key) const {
    return has(key, StoreScope::Workspace) || has(key, StoreScope::User);
}

void SettingsStore::set(const QString& key, const QJsonValue& value, StoreScope scope) {
    const QJsonValue oldVal = get(key);
    if (scope == StoreScope::Workspace) {
        insertKey(workspaceObj_, key, value);
    } else {
        insertKey(userObj_, key, value);
    }
    const QJsonValue newVal = get(key);
    if (oldVal != newVal) {
        scheduleDebouncedSave(scope);
        emit settingChanged(key, oldVal, newVal, scope);
    }
}

void SettingsStore::remove(const QString& key, StoreScope scope) {
    const QJsonValue oldVal = get(key);
    if (scope == StoreScope::Workspace) {
        removeKey(workspaceObj_, key);
    } else {
        removeKey(userObj_, key);
    }
    const QJsonValue newVal = get(key);
    if (oldVal != newVal) {
        scheduleDebouncedSave(scope);
        emit settingChanged(key, oldVal, newVal, scope);
    }
}

void SettingsStore::clear(StoreScope scope) {
    if (scope == StoreScope::Workspace) {
        workspaceObj_ = QJsonObject();
    } else {
        userObj_ = QJsonObject();
    }
    scheduleDebouncedSave(scope);
}

bool SettingsStore::isOverriddenInWorkspace(const QString& key) const {
    return has(key, StoreScope::Workspace);
}

StoreScope SettingsStore::effectiveScope(const QString& key) const {
    if (has(key, StoreScope::Workspace)) {
        return StoreScope::Workspace;
    }
    return StoreScope::User;
}

QStringList SettingsStore::allKeys() const {
    QStringList keys;
    auto harvest = [&keys](const QJsonObject& obj) {
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!keys.contains(it.key())) {
                keys.append(it.key());
            }
        }
    };
    harvest(userObj_);
    harvest(workspaceObj_);
    keys.sort();
    return keys;
}

}  // namespace app
