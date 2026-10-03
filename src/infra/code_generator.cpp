#include "infra/code_generator.h"
#include "infra/code_generator_internal.h"

namespace app {

QVector<GeneratedFile> generate(const Machine& machineIn, const QString& rootNamespace) {
    // A `#machine` self-target resolves here to the initial state, so every
    // emitter sees an ordinary targeted root transition (same result as the
    // simulator's fire-time resolution).
    Machine machine = machineIn;
    for (Transition& transition : machine.transitions) {
        if (transition.from == 0 && transition.machineSelf && machine.initialStateId != 0) {
            transition.to = machine.initialStateId;
            transition.machineSelf = false;
        }
    }
    const GenModel model = buildModel(machine, rootNamespace);
    // Hierarchical-only builders run only when isHierarchical(machine).
    const bool hierarchical = isHierarchical(machine);

    QVector<GeneratedFile> files;
    if (model.hasTypes) {
        files.push_back(buildTypesFile(machine, model));
    }
    files.push_back(buildStateFile(machine, model, hierarchical));
    files.push_back(buildEventsFile(model));
    files.push_back(buildHooksFile(model, hierarchical));
    // Dependency order: core, agent (owns the core), commands, bootstrap.
    files.push_back(hierarchical ? buildHierarchicalCoreFile(machine, model) : buildCoreFile(machine, model));
    files.push_back(buildAgentFile(model, hierarchical));
    files.push_back(buildCommandsFile(model, hierarchical));
    files.push_back(buildBootstrapFile(model, hierarchical));
    return files;
}


QVector<GeneratedFile> generateDomainStubs(const Machine& machine, const QString& rootNamespace) {
    const GenModel model = buildModel(machine, rootNamespace);
    return buildDomainStubFiles(machine, model);
}

bool writeDomainStubsIfAbsent(const QString& outputDir, const QVector<GeneratedFile>& stubs, int* writtenOut,
                               QString* error) {
    if (writtenOut != nullptr) {
        *writtenOut = 0;
    }
    for (const GeneratedFile& stub : stubs) {
        const QString path = outputDir + QStringLiteral("/") + stub.relativePath;
        if (QFile::exists(path)) {
            continue;  // the developer's file now -- never overwritten
        }
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            if (error != nullptr) {
                *error = QStringLiteral("could not create directory for %1").arg(stub.relativePath);
            }
            return false;
        }
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error != nullptr) {
                *error = QStringLiteral("%1: %2").arg(stub.relativePath, out.errorString());
            }
            return false;
        }
        const QByteArray bytes = stub.content.toUtf8();
        if (out.write(bytes) != bytes.size()) {
            if (error != nullptr) {
                *error = QStringLiteral("%1: %2").arg(stub.relativePath, out.errorString());
            }
            return false;
        }
        if (writtenOut != nullptr) {
            ++*writtenOut;
        }
    }
    return true;
}

bool writeGeneratedFiles(const QString& outputDir, const QVector<GeneratedFile>& files, QString* error) {
    QDir dir;
    if (!dir.mkpath(outputDir)) {
        if (error != nullptr) {
            *error = QStringLiteral("could not create directory: %1").arg(outputDir);
        }
        return false;
    }
    for (const GeneratedFile& file : files) {
        QFile out(outputDir + QStringLiteral("/") + file.relativePath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error != nullptr) {
                *error = QStringLiteral("%1: %2").arg(file.relativePath, out.errorString());
            }
            return false;
        }
        const QByteArray bytes = file.content.toUtf8();
        if (out.write(bytes) != bytes.size()) {
            if (error != nullptr) {
                *error = QStringLiteral("%1: %2").arg(file.relativePath, out.errorString());
            }
            return false;
        }
    }
    return true;
}

}  // namespace app
