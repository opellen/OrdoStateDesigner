#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "model/machine.h"

namespace app {

// One project's manifest (.sdp): the machine documents (one .sdm file each)
// that belong together, plus where generated code lands. Opening a project
// spawns one DocumentSession per entry in machineFiles.
struct Project {
    QString name;
    QStringList machineFiles;
    QString outputDir;
    QString rootNamespace;

    bool operator==(const Project&) const = default;
};

// Pure functions plus thin file read/write helpers; no dialogs or widgets.
// Serialized arrays keep their insertion order, so output is byte-stable
// without sorting (no unordered container is serialized).

// ---- Machine (.sdm) <-> JSON ------------------------------------------------

// Context variable type -> name; exported for callers outside this TU
// (harness/cli_modes.cpp).
QString contextTypeToString(ContextType type);

QJsonObject machineToJson(const Machine& machine);
// Callers handling untrusted JSON must pass `error`: on an unknown enum string
// it is set to a diagnostic (machine/element/field/value/supported list) and
// the returned Machine is incomplete. Detect failure by *error being non-empty.
Machine machineFromJson(const QJsonObject& json, QString* error = nullptr);

// Writes/reads one .sdm file. Returns false and fills *error (when
// non-null) on failure: a missing directory, an unreadable file, malformed or
// non-object JSON, or (loadMachine only) an unrecognized enum string.
bool saveMachine(const Machine& machine, const QString& path, QString* error = nullptr);
bool loadMachine(const QString& path, Machine* machine, QString* error = nullptr);

// ---- Project (.sdp) <-> JSON ------------------------------------------------

QJsonObject projectToJson(const Project& project);
Project projectFromJson(const QJsonObject& json);

bool saveProject(const Project& project, const QString& path, QString* error = nullptr);
bool loadProject(const QString& path, Project* project, QString* error = nullptr);

// ---- Asynchronous / Thread-Safe Batch Persistence --------------------------

struct MachineFileEntry {
    QString relativePath;
    QString name;
    Machine machine;
};

struct LoadedProjectData {
    Project project;
    std::vector<MachineFileEntry> machines;
};

// Thread-safe pure file I/O & parsing (NO QObject, NO DocumentSession).
// Safe to execute in background worker threads.
std::optional<LoadedProjectData> readProjectFiles(
    const QString& sdpPath,
    std::function<void(int percentage, const QString& message)> progress = nullptr,
    QString* error = nullptr
);

// Thread-safe pure batch disk writer.
// Safe to execute in background worker threads.
bool writeProjectFiles(
    const QString& sdpPath,
    const Project& project,
    const std::vector<MachineFileEntry>& machines,
    std::function<void(int percentage, const QString& message)> progress = nullptr,
    QString* error = nullptr
);

}  // namespace app
