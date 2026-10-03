// --smoke phase for infra/recent_projects: list rules (touch order, case/`..`
// de-dup, 10-entry cap, remove, clear), the save/load round trip, and a
// missing or corrupt file loading as empty. Pure unit test over a scratch
// temp file; no QApplication.

#include <cstdio>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "harness/harness.h"
#include "infra/recent_projects.h"

namespace {
// Independent copy of RecentProjects::normalize() so the stored string shape is
// checked. No QFileInfo::operator==: its equality is unreliable for paths that
// do not exist on disk.
QString normalized(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}
}  // namespace

int runRecentProjectsSmoke() {
    std::printf("[SMOKE] Running recent-projects smoke...\n");

    const QString tempDir = QDir::tempPath() + QStringLiteral("/sd_recent_smoke_") +
                             QString::number(QCoreApplication::applicationPid());
    QDir().mkpath(tempDir);
    const QString listPath = tempDir + QStringLiteral("/recent.json");
    QFile::remove(listPath);

    // ---- 1. Missing file -> empty list --------------------------------------
    {
        app::RecentProjects recent(listPath);
        recent.load();
        if (!recent.paths().isEmpty()) {
            std::fprintf(stderr, "FAIL: a missing recent.json should load as an empty list\n");
            return 1;
        }
    }

    // ---- 2. Corrupt file -> empty list ---------------------------------------
    {
        QFile corrupt(listPath);
        if (!corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::fprintf(stderr, "FAIL: could not write a corrupt recent.json fixture\n");
            return 1;
        }
        corrupt.write("{not valid json");
        corrupt.close();

        app::RecentProjects recent(listPath);
        recent.load();
        if (!recent.paths().isEmpty()) {
            std::fprintf(stderr, "FAIL: a corrupt recent.json should load as an empty list\n");
            return 1;
        }
        QFile::remove(listPath);
    }

    // ---- 3. Touch order (newest first) ---------------------------------------
    const QString fileA = tempDir + QStringLiteral("/a.sdp");
    const QString fileB = tempDir + QStringLiteral("/b.sdp");
    const QString fileC = tempDir + QStringLiteral("/c.sdp");
    // touch() only normalizes the string; the file need not exist.
    app::RecentProjects recent(listPath);
    recent.touch(fileA);
    recent.touch(fileB);
    recent.touch(fileC);
    if (recent.paths().size() != 3 || recent.paths().at(0) != normalized(fileC) ||
        recent.paths().at(1) != normalized(fileB) || recent.paths().at(2) != normalized(fileA)) {
        std::fprintf(stderr, "FAIL: touch order should be newest-first (C, B, A), got %d entries\n",
                     static_cast<int>(recent.paths().size()));
        return 1;
    }

    // Re-touching an already-listed entry moves it to the front instead of
    // duplicating it.
    recent.touch(fileA);
    if (recent.paths().size() != 3 || recent.paths().at(0) != normalized(fileA)) {
        std::fprintf(stderr, "FAIL: re-touching an existing entry should move it to front without duplicating\n");
        return 1;
    }

    // ---- 4. De-dup: different case and a ".." spelling -----------------------
#if defined(Q_OS_WIN)
    const QString fileAUpper = fileA.toUpper();
    recent.touch(fileAUpper);
    if (recent.paths().size() != 3) {
        std::fprintf(stderr, "FAIL: touching the same path in a different CASE should not duplicate (Windows)\n");
        return 1;
    }
#endif
    const QString fileBViaDotDot = tempDir + QStringLiteral("/sub/../b.sdp");
    recent.touch(fileBViaDotDot);
    if (recent.paths().size() != 3) {
        std::fprintf(stderr, "FAIL: a '..'-spelled path to an already-listed entry should not duplicate\n");
        return 1;
    }
    if (recent.paths().at(0) != normalized(fileB)) {
        std::fprintf(stderr, "FAIL: the '..'-spelled touch should have moved b.sdp to the front\n");
        return 1;
    }

    // ---- 5. Cap at kMaxEntries -- drops the oldest ----------------------------
    app::RecentProjects capped(listPath);
    capped.clear();
    for (int i = 0; i < app::RecentProjects::kMaxEntries + 2; ++i) {
        const QString path = tempDir + QStringLiteral("/cap_%1.sdp").arg(i);
        capped.touch(path);
    }
    if (capped.paths().size() != app::RecentProjects::kMaxEntries) {
        std::fprintf(stderr, "FAIL: cap should hold list at kMaxEntries=%d, got %d\n", app::RecentProjects::kMaxEntries,
                     static_cast<int>(capped.paths().size()));
        return 1;
    }
    // Newest entry (index kMaxEntries+1, i.e. "cap_11") is at the front; the
    // two oldest (cap_0, cap_1) were dropped.
    const QString newestCapPath = tempDir + QStringLiteral("/cap_%1.sdp").arg(app::RecentProjects::kMaxEntries + 1);
    const QString droppedOldestPath = normalized(tempDir + QStringLiteral("/cap_0.sdp"));
    if (capped.paths().at(0) != normalized(newestCapPath)) {
        std::fprintf(stderr, "FAIL: cap should keep the newest entry at the front\n");
        return 1;
    }
    if (capped.paths().contains(droppedOldestPath)) {
        std::fprintf(stderr, "FAIL: cap should have dropped the oldest entry (cap_0), it is still present\n");
        return 1;
    }

    // ---- 6. remove() ------------------------------------------------------------
    const int beforeRemove = capped.paths().size();
    capped.remove(newestCapPath);
    if (capped.paths().size() != beforeRemove - 1) {
        std::fprintf(stderr, "FAIL: remove() of a listed entry should shrink the list by exactly one\n");
        return 1;
    }
    if (capped.paths().contains(normalized(newestCapPath))) {
        std::fprintf(stderr, "FAIL: remove() left the removed entry in the list\n");
        return 1;
    }
    // Removing an entry that was never listed is a harmless no-op.
    const int beforeNoopRemove = capped.paths().size();
    capped.remove(tempDir + QStringLiteral("/never-listed.sdp"));
    if (capped.paths().size() != beforeNoopRemove) {
        std::fprintf(stderr, "FAIL: remove() of an absent entry should not change the list\n");
        return 1;
    }

    // ---- 7. clear() ---------------------------------------------------------
    capped.clear();
    if (!capped.paths().isEmpty()) {
        std::fprintf(stderr, "FAIL: clear() should empty the list\n");
        return 1;
    }

    // ---- 8. save -> new instance -> load round trip --------------------------
    app::RecentProjects roundTripSource(listPath);
    roundTripSource.clear();  // saves an empty list, establishing a clean baseline file
    roundTripSource.touch(fileA);
    roundTripSource.touch(fileB);
    roundTripSource.setLastActiveProject(fileA);
    if (!QFile::exists(listPath)) {
        std::fprintf(stderr, "FAIL: touch() should save immediately -- recent.json was not created on disk\n");
        return 1;
    }

    app::RecentProjects roundTripTarget(listPath);
    roundTripTarget.load();
    if (roundTripTarget.paths().size() != 2 || roundTripTarget.paths().at(0) != normalized(fileB) ||
        roundTripTarget.paths().at(1) != normalized(fileA)) {
        std::fprintf(stderr, "FAIL: a fresh instance's load() did not reproduce the saved list (got %d entries)\n",
                     static_cast<int>(roundTripTarget.paths().size()));
        return 1;
    }
    if (roundTripTarget.lastActiveProject() != normalized(fileA)) {
        std::fprintf(stderr, "FAIL: lastActiveProject did not round-trip correctly (expected %s, got %s)\n",
                     normalized(fileA).toUtf8().constData(),
                     roundTripTarget.lastActiveProject().toUtf8().constData());
        return 1;
    }

    // ---- 9. lastActiveProject clearing via setLastActiveProject(QString()) ---
    roundTripTarget.setLastActiveProject(QString());
    app::RecentProjects roundTripCleared(listPath);
    roundTripCleared.load();
    if (!roundTripCleared.lastActiveProject().isEmpty()) {
        std::fprintf(stderr, "FAIL: setLastActiveProject(QString()) did not persist empty string to disk\n");
        return 1;
    }

    // ---- 10. Legacy format compatibility (no lastActiveProject key) ---------
    {
        QFile legacyFile(listPath);
        if (!legacyFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::fprintf(stderr, "FAIL: could not write legacy recent.json fixture\n");
            return 1;
        }
        legacyFile.write("{\"entries\": [\"" + normalized(fileA).toUtf8() + "\"]}");
        legacyFile.close();

        app::RecentProjects legacyRecent(listPath);
        legacyRecent.load();
        if (legacyRecent.paths().size() != 1 || legacyRecent.paths().at(0) != normalized(fileA)) {
            std::fprintf(stderr, "FAIL: legacy recent.json without lastActiveProject did not load entries\n");
            return 1;
        }
        if (!legacyRecent.lastActiveProject().isEmpty()) {
            std::fprintf(stderr, "FAIL: legacy recent.json should deserialize lastActiveProject as empty\n");
            return 1;
        }
    }

    // ---- Cleanup --------------------------------------------------------------
    QFile::remove(listPath);
    QDir(tempDir).removeRecursively();

    std::printf(
        "PASS: state-designer recent-projects smoke (missing/corrupt file -> empty; touch order; case/'..' "
        "de-dup; cap drops oldest; remove; clear; save -> new instance -> load round trip; lastActiveProject "
        "persistence/clearing; legacy backward compatibility)\n");
    return 0;
}
