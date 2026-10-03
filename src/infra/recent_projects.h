#pragma once

#include <QString>
#include <QStringList>

namespace app {

// Backing store for File > Open Recent. Application state, kept in its own file
// (defaultPath()), never in settings.json. Pure list logic: no widgets, no signals.
//
// Entries are absolute cleaned paths (normalize()), newest first, de-duplicated
// case-insensitively on Windows, capped at kMaxEntries. Every mutation saves
// immediately.
class RecentProjects {
public:
    explicit RecentProjects(QString filePath);

    // AppConfigLocation + "/recent.json" (or the ~/.state-designer fallback
    // when AppConfigLocation is empty), unless a process-wide override is
    // active -- see setDefaultPathOverride() below.
    static QString defaultPath();

    // Probe/smoke only: redirects every later defaultPath() call to `path`, so
    // test runs never touch the real list. Call before constructing any
    // MainWindow. An empty string clears the override.
    static void setDefaultPathOverride(const QString& path);

    // Redirects this instance to a different file. Does not reload; call load().
    void setFilePath(const QString& path);
    QString filePath() const { return filePath_; }

    // Reads filePath_ and replaces the in-memory list. A missing or corrupt file
    // yields an empty list.
    void load();
    QStringList paths() const { return paths_; }

    // Normalizes `path` (QDir::cleanPath(QFileInfo(path).absoluteFilePath())),
    // removes any existing entry that normalizes to the same path
    // (case-insensitive on Windows), then inserts it at the front. Drops
    // the oldest entry once the list exceeds kMaxEntries. Always saves.
    void touch(const QString& path);
    // Normalizes and removes a matching entry (case-insensitive on
    // Windows), if present. Saves only when an entry was actually removed.
    void remove(const QString& path);
    // Empties the list. Saves only when it was non-empty.
    void clear();

    // Canonical path of the project open when the application was last active.
    // Empty when none was open or it was closed explicitly; clear with
    // setLastActiveProject(QString()).
    QString lastActiveProject() const { return lastActiveProject_; }
    void setLastActiveProject(const QString& path);
    // Same normalization and comparison as the list, so a raw dialog path can be
    // checked directly.
    bool isLastActiveProject(const QString& path) const;

    static constexpr int kMaxEntries = 10;

private:
    void save() const;
    static QString normalize(const QString& path);
    static bool pathsEqual(const QString& a, const QString& b);

    QString filePath_;
    QStringList paths_;  // newest first, normalized, de-duped, capped
    QString lastActiveProject_;
};

}  // namespace app
