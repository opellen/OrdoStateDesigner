#pragma once

#include <QString>
#include <QVector>

#include "model/machine.h"

namespace app {

// A path relative to the output directory (may nest, e.g. "domain/...") plus
// its finished text.
struct GeneratedFile {
    QString relativePath;
    QString content;

    bool operator==(const GeneratedFile&) const = default;
};

// Legal C++ identifier from `raw`: non-alphanumeric runs split words and are
// dropped; an all-caps word is lowercased, mixed case is kept ("HTTPRequest"
// -> "hTTPRequest"). Joined PascalCase (capitalizeFirst) or camelCase; a
// leading digit gets '_'; no alphanumerics gives "" (the validator flags it).
// The validator's collision check calls this exact function.
QString sanitizeIdentifier(const QString& raw, bool capitalizeFirst);

// Same word split, lower_snake_case ("Login Flow" -> "login_flow"): the machine
// namespace and generated-file prefix.
QString sanitizeSnakeCase(const QString& raw);

// Pure: machine in, generated file set out, no IO; cheap enough for a live
// preview. Namespace: `rootNamespace`::sanitizeSnakeCase(machine.name). Does
// not reject an invalid machine; the validator gates writing to disk.
// Output is Qt-free: <machine>_state.h, _events.h, _hooks.h, _core.h (the
// standalone std-only transition core, flat or hierarchical), and the ordo
// wrappers _agent.h, _commands.h, _bootstrap.h.
QVector<GeneratedFile> generate(const Machine& machine, const QString& rootNamespace);

// Developer-owned starting points under domain/: hook implementations with TODO
// bodies (guards default true) and a bootstrap .cpp. writeDomainStubsIfAbsent()
// skips any file that already exists; once written, a stub is never regenerated.
QVector<GeneratedFile> generateDomainStubs(const Machine& machine, const QString& rootNamespace);
bool writeDomainStubsIfAbsent(const QString& outputDir, const QVector<GeneratedFile>& stubs,
                               int* writtenOut = nullptr, QString* error = nullptr);

// Writes each file at outputDir/relativePath (creating outputDir if missing,
// truncating existing files). Returns false and sets *error on any failure.
bool writeGeneratedFiles(const QString& outputDir, const QVector<GeneratedFile>& files, QString* error = nullptr);

}  // namespace app
