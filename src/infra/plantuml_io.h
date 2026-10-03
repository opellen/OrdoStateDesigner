#pragma once

#include <QString>
#include <QStringList>

#include "model/machine.h"

namespace app {

struct PlantUmlOptions {
    bool includeActionsGuards = true;
    bool includeSkinparam = true;
};

struct PlantUmlExportResult {
    QString plantUml;
    QStringList diagnostics;
    bool ok = true;
    QString error;
};

// Generates PlantUML state diagram specification text from a Machine model.
PlantUmlExportResult machineToPlantUml(const Machine& machine, const PlantUmlOptions& options = {});

// Writes machineToPlantUml result as a .puml file.
bool exportPlantUmlFile(const Machine& machine, const QString& path, const PlantUmlOptions& options = {},
                        QStringList* diagnostics = nullptr, QString* error = nullptr);

}  // namespace app
