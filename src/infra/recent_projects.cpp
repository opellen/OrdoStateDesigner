#include "infra/recent_projects.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace app {

namespace {
// Process-wide; set once by --smoke/--gui-probe before any MainWindow exists.
QString g_defaultPathOverride;
}  // namespace

RecentProjects::RecentProjects(QString filePath) : filePath_(std::move(filePath)) {}

QString RecentProjects::defaultPath() {
    if (!g_defaultPathOverride.isEmpty()) {
        return g_defaultPathOverride;
    }
    // Same fallback shape as SettingsStore::defaultUserPath() (infra/
    // settings_store.cpp) -- AppConfigLocation can come back empty on a
    // misconfigured environment.
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configDir.isEmpty()) {
        configDir = QDir::homePath() + QStringLiteral("/.state-designer");
    }
    return configDir + QStringLiteral("/recent.json");
}

void RecentProjects::setDefaultPathOverride(const QString& path) {
    g_defaultPathOverride = path;
}

void RecentProjects::setFilePath(const QString& path) {
    filePath_ = path;
}

QString RecentProjects::normalize(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool RecentProjects::pathsEqual(const QString& a, const QString& b) {
#if defined(Q_OS_WIN)
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

void RecentProjects::load() {
    paths_.clear();
    lastActiveProject_.clear();
    if (filePath_.isEmpty()) {
        return;
    }
    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly)) {
        return;  // missing file -> empty list, not an error
    }
    const QByteArray bytes = file.readAll();
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return;  // corrupt file -> empty list, not an error
    }
    const QJsonObject root = doc.object();
    const QJsonValue lastActiveVal = root.value(QStringLiteral("lastActiveProject"));
    if (lastActiveVal.isString() && !lastActiveVal.toString().isEmpty()) {
        lastActiveProject_ = normalize(lastActiveVal.toString());
    }
    const QJsonValue entriesVal = root.value(QStringLiteral("entries"));
    if (!entriesVal.isArray()) {
        return;
    }
    for (const QJsonValue& v : entriesVal.toArray()) {
        if (v.isString() && !v.toString().isEmpty()) {
            paths_.append(v.toString());
        }
    }
}

void RecentProjects::save() const {
    if (filePath_.isEmpty()) {
        return;
    }
    QFileInfo info(filePath_);
    QDir dir = info.dir();
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }

    QJsonArray arr;
    for (const QString& p : paths_) {
        arr.append(p);
    }
    QJsonObject root;
    root.insert(QStringLiteral("lastActiveProject"), lastActiveProject_);
    root.insert(QStringLiteral("entries"), arr);
    const QString json = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));

    // Atomic write -- same idea as SettingsStore::atomicWriteJsonFile
    // (infra/settings_store.cpp): write to a unique temp file, flush,
    // close, then rename over the target, so a crash mid-write never
    // leaves a truncated recent.json.
    const QString tempPath = filePath_ + QStringLiteral(".tmp.") +
                              QString::number(QCoreApplication::applicationPid()) + QStringLiteral(".") +
                              QString::number(QRandomGenerator::global()->generate());
    QFile tempFile(tempPath);
    if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    tempFile.write(json.toUtf8());
    tempFile.flush();
    tempFile.close();

    if (QFile::exists(filePath_)) {
        QFile::remove(filePath_);
    }
    tempFile.rename(filePath_);
}

void RecentProjects::touch(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    const QString normalized = normalize(path);
    for (int i = 0; i < paths_.size(); ++i) {
        if (pathsEqual(paths_.at(i), normalized)) {
            paths_.removeAt(i);
            break;
        }
    }
    paths_.prepend(normalized);
    while (paths_.size() > kMaxEntries) {
        paths_.removeLast();  // paths_ is newest-first -- the oldest sits at the back
    }
    save();
}

void RecentProjects::remove(const QString& path) {
    const QString normalized = normalize(path);
    for (int i = 0; i < paths_.size(); ++i) {
        if (pathsEqual(paths_.at(i), normalized)) {
            paths_.removeAt(i);
            save();
            return;
        }
    }
}

void RecentProjects::clear() {
    if (paths_.isEmpty()) {
        return;
    }
    paths_.clear();
    save();
}

void RecentProjects::setLastActiveProject(const QString& path) {
    const QString normalized = path.isEmpty() ? QString() : normalize(path);
    if (pathsEqual(lastActiveProject_, normalized)) {
        return;
    }
    lastActiveProject_ = normalized;
    save();
}

bool RecentProjects::isLastActiveProject(const QString& path) const {
    if (path.isEmpty() || lastActiveProject_.isEmpty()) {
        return false;
    }
    return pathsEqual(lastActiveProject_, normalize(path));
}

}  // namespace app
