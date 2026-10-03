#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "model/machine.h"

namespace app {

struct ScxmlExportOptions {
    bool includeLayout = true;   // Include <metadata> with visual coordinates (pos)
    bool includeActions = true;  // Include <onentry>, <onexit>, and transition actions
};

struct ScxmlExportResult {
    QByteArray xml;
    QStringList diagnostics;
    bool ok = true;
    QString error;
};

struct ScxmlImportResult {
    Machine machine;
    QStringList diagnostics;
    bool ok = true;
    QString error;
};

// Pure serialization: Machine -> W3C SCXML XML
ScxmlExportResult machineToScxml(const Machine& machine, const ScxmlExportOptions& options = {});

// Pure deserialization: W3C SCXML XML -> Machine
ScxmlImportResult scxmlToMachine(const QByteArray& xmlData);

// Read and deserialize from file
ScxmlImportResult importScxmlFile(const QString& filePath);

}  // namespace app
