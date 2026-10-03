#include "infra/machine_io.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStringConverter>
#include <QTextStream>

#include "infra/io_dispatcher.h"
#include "infra/plantuml_io.h"
#include "infra/scxml_io.h"
#include "infra/xstate_v5_io.h"
#include "model/io_events.h"

namespace app {
namespace {

QString sanitizeFileName(const QString& name) {
    QString out;
    out.reserve(name.size());
    for (const QChar ch : name) {
        if (ch.isLetterOrNumber() || ch == '_' || ch == '-') {
            out.append(ch);
        } else if (ch.isSpace()) {
            out.append('_');
        }
    }
    return out.isEmpty() ? QStringLiteral("machine") : out;
}

class XStateV5FormatAdapter : public MachineFormatAdapter {
public:
    MachineFormatDescriptor descriptor() const override {
        MachineFormatDescriptor d;
        d.id = QStringLiteral("xstate-v5");
        d.name = QStringLiteral("XState v5 JSON");
        d.defaultExtension = QStringLiteral("json");
        d.extensions = {QStringLiteral("json")};
        d.filterPattern = QStringLiteral("XState v5 JSON (*.json)");
        d.capabilities = IoCapability::ImportExport;
        d.supportedScopes = ExportScope::SingleMachine;
        d.payloadKind = PayloadKind::Text;
        d.description = QStringLiteral("Bidirectional XState v5 / Stately Studio machine configuration JSON");
        return d;
    }

    MachineSerializeResult serialize(const QList<const Machine*>& machines,
                                     const FormatOptionMap& options) override {
        Q_UNUSED(options);
        MachineSerializeResult res;
        if (machines.isEmpty() || !machines.first()) {
            res.ok = false;
            res.error = QStringLiteral("No machine provided for XState serialization");
            return res;
        }

        XStateExportResult xres = machineToXStateJson(*machines.first());
        if (!xres.ok) {
            res.ok = false;
            res.error = xres.error;
            res.diagnostics = xres.diagnostics;
            return res;
        }

        QJsonDocument doc(xres.json);
        res.payload = doc.toJson(QJsonDocument::Indented);
        res.diagnostics = xres.diagnostics;
        res.ok = true;
        return res;
    }

    MachineImportResult importFile(const QString& path) override {
        MachineImportResult res;
        XStateImportResult importRes = importXStateFile(path);
        res.ok = importRes.ok;
        res.error = importRes.error;
        res.diagnostics = importRes.diagnostics;
        res.machine = std::move(importRes.machine);
        return res;
    }
};

class PlantUmlFormatAdapter : public MachineFormatAdapter {
public:
    MachineFormatDescriptor descriptor() const override {
        MachineFormatDescriptor d;
        d.id = QStringLiteral("plantuml");
        d.name = QStringLiteral("PlantUML State Diagram");
        d.defaultExtension = QStringLiteral("puml");
        d.extensions = {QStringLiteral("puml"), QStringLiteral("plantuml")};
        d.filterPattern = QStringLiteral("PlantUML State Diagram (*.puml *.plantuml)");
        d.capabilities = IoCapability::Export;
        d.supportedScopes = ExportScope::SingleMachine | ExportScope::MultipleMachinesDirectory;
        d.payloadKind = PayloadKind::Text;
        d.description = QStringLiteral("PlantUML text-based state diagram for documentation and review");

        FormatOption optActions;
        optActions.id = QStringLiteral("includeActionsGuards");
        optActions.label = QStringLiteral("Include actions and guards");
        optActions.description = QStringLiteral("Emit transition guard expressions and entry/exit actions");
        optActions.type = OptionType::Bool;
        optActions.defaultValue = true;

        FormatOption optSkin;
        optSkin.id = QStringLiteral("includeSkinparam");
        optSkin.label = QStringLiteral("Include skinparam styling");
        optSkin.description = QStringLiteral("Emit PlantUML default state font and skinparam styling block");
        optSkin.type = OptionType::Bool;
        optSkin.defaultValue = true;

        d.options = {optActions, optSkin};
        return d;
    }

    MachineSerializeResult serialize(const QList<const Machine*>& machines,
                                     const FormatOptionMap& options) override {
        MachineSerializeResult res;
        if (machines.isEmpty() || !machines.first()) {
            res.ok = false;
            res.error = QStringLiteral("No machine provided for PlantUML serialization");
            return res;
        }

        PlantUmlOptions pumlOpts;
        if (options.contains(QStringLiteral("includeActionsGuards"))) {
            pumlOpts.includeActionsGuards = options.value(QStringLiteral("includeActionsGuards")).toBool();
        }
        if (options.contains(QStringLiteral("includeSkinparam"))) {
            pumlOpts.includeSkinparam = options.value(QStringLiteral("includeSkinparam")).toBool();
        }

        QStringList fullDiagrams;
        for (const Machine* m : machines) {
            if (!m) continue;
            PlantUmlExportResult pumlRes = machineToPlantUml(*m, pumlOpts);
            if (!pumlRes.ok) {
                res.ok = false;
                res.error = pumlRes.error;
                return res;
            }
            fullDiagrams.append(pumlRes.plantUml);
            res.diagnostics.append(pumlRes.diagnostics);
        }

        res.payload = fullDiagrams.join(QStringLiteral("\n\n")).toUtf8();
        res.ok = true;
        return res;
    }
};

class ScxmlFormatAdapter : public MachineFormatAdapter {
public:
    MachineFormatDescriptor descriptor() const override {
        MachineFormatDescriptor d;
        d.id = QStringLiteral("scxml");
        d.name = QStringLiteral("W3C SCXML");
        d.defaultExtension = QStringLiteral("scxml");
        d.extensions = {QStringLiteral("scxml"), QStringLiteral("xml")};
        d.filterPattern = QStringLiteral("W3C SCXML (*.scxml *.xml)");
        d.capabilities = IoCapability::Import | IoCapability::Export;
        d.supportedScopes = ExportScope::SingleMachine | ExportScope::MultipleMachinesDirectory;
        d.payloadKind = PayloadKind::Text;
        d.description = QStringLiteral("W3C State Chart XML for Qt SCXML and industrial tools");

        FormatOption optLayout;
        optLayout.id = QStringLiteral("includeLayout");
        optLayout.label = QStringLiteral("Include visual layout coordinates");
        optLayout.description = QStringLiteral("Embed non-destructive layout metadata for pixel-perfect round trip");
        optLayout.type = OptionType::Bool;
        optLayout.defaultValue = true;

        FormatOption optActions;
        optActions.id = QStringLiteral("includeActions");
        optActions.label = QStringLiteral("Include actions and executable content");
        optActions.description = QStringLiteral("Emit onentry, onexit, and transition actions");
        optActions.type = OptionType::Bool;
        optActions.defaultValue = true;

        d.options = {optLayout, optActions};
        return d;
    }

    MachineSerializeResult serialize(const QList<const Machine*>& machines,
                                     const FormatOptionMap& options) override {
        MachineSerializeResult res;
        if (machines.isEmpty() || !machines.first()) {
            res.ok = false;
            res.error = QStringLiteral("No machine provided for SCXML serialization");
            return res;
        }

        ScxmlExportOptions scxmlOpts;
        if (options.contains(QStringLiteral("includeLayout"))) {
            scxmlOpts.includeLayout = options.value(QStringLiteral("includeLayout")).toBool();
        }
        if (options.contains(QStringLiteral("includeActions"))) {
            scxmlOpts.includeActions = options.value(QStringLiteral("includeActions")).toBool();
        }

        ScxmlExportResult scxmlRes = machineToScxml(*machines.first(), scxmlOpts);
        if (!scxmlRes.ok) {
            res.ok = false;
            res.error = scxmlRes.error;
            return res;
        }

        res.payload = scxmlRes.xml;
        res.diagnostics = scxmlRes.diagnostics;
        res.ok = true;
        return res;
    }

    MachineImportResult importFile(const QString& path) override {
        MachineImportResult res;
        ScxmlImportResult scxmlRes = importScxmlFile(path);
        if (!scxmlRes.ok) {
            res.ok = false;
            res.error = scxmlRes.error;
            return res;
        }
        res.machine = std::move(scxmlRes.machine);
        res.diagnostics = scxmlRes.diagnostics;
        res.ok = true;
        return res;
    }
};

}  // namespace

MachineIoRegistry& MachineIoRegistry::instance() {
    static MachineIoRegistry s_instance;
    return s_instance;
}

MachineIoRegistry::MachineIoRegistry() {
    registerDefaultAdapters();
}

void MachineIoRegistry::registerDefaultAdapters() {
    registerAdapter(std::make_shared<ScxmlFormatAdapter>());
    registerAdapter(std::make_shared<PlantUmlFormatAdapter>());
    registerAdapter(std::make_shared<XStateV5FormatAdapter>());
}

void MachineIoRegistry::registerAdapter(std::shared_ptr<MachineFormatAdapter> adapter) {
    if (!adapter) {
        return;
    }
    const QString id = adapter->descriptor().id;
    for (int i = 0; i < adapters_.size(); ++i) {
        if (adapters_[i]->descriptor().id == id) {
            adapters_[i] = adapter;
            return;
        }
    }
    adapters_.append(adapter);
}

QVector<MachineFormatDescriptor> MachineIoRegistry::registeredFormats() const {
    QVector<MachineFormatDescriptor> list;
    list.reserve(adapters_.size());
    for (const auto& a : adapters_) {
        list.append(a->descriptor());
    }
    return list;
}

QVector<MachineFormatDescriptor> MachineIoRegistry::exportFormats() const {
    QVector<MachineFormatDescriptor> list;
    for (const auto& a : adapters_) {
        if (hasCapability(a->descriptor().capabilities, IoCapability::Export)) {
            list.append(a->descriptor());
        }
    }
    return list;
}

QVector<MachineFormatDescriptor> MachineIoRegistry::importFormats() const {
    QVector<MachineFormatDescriptor> list;
    for (const auto& a : adapters_) {
        if (hasCapability(a->descriptor().capabilities, IoCapability::Import)) {
            list.append(a->descriptor());
        }
    }
    return list;
}

std::shared_ptr<MachineFormatAdapter> MachineIoRegistry::findAdapterById(const QString& id) const {
    for (const auto& a : adapters_) {
        if (a->descriptor().id.compare(id, Qt::CaseInsensitive) == 0) {
            return a;
        }
    }
    return nullptr;
}

std::shared_ptr<MachineFormatAdapter> MachineIoRegistry::findAdapterByExtension(const QString& ext) const {
    const QString cleanExt = ext.startsWith('.') ? ext.mid(1) : ext;
    for (const auto& a : adapters_) {
        for (const auto& e : a->descriptor().extensions) {
            if (e.compare(cleanExt, Qt::CaseInsensitive) == 0) {
                return a;
            }
        }
    }
    return nullptr;
}

QString MachineIoRegistry::filterStringForFormat(const MachineFormatDescriptor& desc) const {
    if (!desc.filterPattern.isEmpty()) {
        return desc.filterPattern;
    }
    QStringList globs;
    for (const QString& ext : desc.extensions) {
        globs.append(QStringLiteral("*.%1").arg(ext));
    }
    return QStringLiteral("%1 (%2)").arg(desc.name, globs.join(' '));
}

std::shared_ptr<MachineFormatAdapter> MachineIoRegistry::formatForFilter(const QString& filterString) const {
    for (const auto& a : adapters_) {
        const QString pattern = filterStringForFormat(a->descriptor());
        if (pattern.compare(filterString, Qt::CaseInsensitive) == 0) {
            return a;
        }
    }
    for (const auto& a : adapters_) {
        if (!a->descriptor().filterPattern.isEmpty() &&
            a->descriptor().filterPattern.compare(filterString, Qt::CaseInsensitive) == 0) {
            return a;
        }
        if (filterString.startsWith(a->descriptor().name, Qt::CaseInsensitive)) {
            return a;
        }
    }
    return nullptr;
}

QString MachineIoRegistry::exportFilterString() const {
    QStringList filters;
    for (const auto& desc : exportFormats()) {
        filters.append(filterStringForFormat(desc));
    }
    filters.append(QStringLiteral("All Files (*.*)"));
    return filters.join(QStringLiteral(";;"));
}

QString MachineIoRegistry::importFilterString() const {
    QStringList filters;
    for (const auto& desc : importFormats()) {
        filters.append(filterStringForFormat(desc));
    }
    filters.append(QStringLiteral("All Files (*.*)"));
    return filters.join(QStringLiteral(";;"));
}

// -----------------------------------------------------------------------------
// Export Pipeline
// -----------------------------------------------------------------------------

ExportJobResult executeExportJobSync(const ExportJob& job) {
    ExportJobResult result;
    if (!job.adapter) {
        result.ok = false;
        result.error = QStringLiteral("No export adapter specified");
        return result;
    }

    if (job.machines.isEmpty()) {
        result.ok = false;
        result.error = QStringLiteral("No machines specified for export");
        return result;
    }

    const MachineFormatDescriptor desc = job.adapter->descriptor();

    if (job.scope == ExportScope::SingleMachine) {
        if (job.destinationPath.isEmpty()) {
            result.ok = false;
            result.error = QStringLiteral("Destination path is empty");
            return result;
        }

        MachineSerializeResult ser = job.adapter->serialize({job.machines.first()}, job.options);
        if (!ser.ok) {
            result.ok = false;
            result.error = ser.error;
            result.diagnostics = ser.diagnostics;
            return result;
        }

        QSaveFile file(job.destinationPath);
        if (!file.open(QIODevice::WriteOnly)) {
            result.ok = false;
            result.error = QStringLiteral("Could not write file %1: %2").arg(job.destinationPath, file.errorString());
            return result;
        }
        file.write(ser.payload);
        if (!file.commit()) {
            result.ok = false;
            result.error = QStringLiteral("Could not commit atomic save to %1: %2").arg(job.destinationPath, file.errorString());
            return result;
        }

        result.filesWritten = 1;
        result.diagnostics = ser.diagnostics;
        if (desc.payloadKind == PayloadKind::Text) {
            result.textPayload = QString::fromUtf8(ser.payload);
        }
        return result;
    }

    if (job.scope == ExportScope::MultipleMachinesDirectory) {
        if (job.destinationPath.isEmpty()) {
            result.ok = false;
            result.error = QStringLiteral("Destination directory path is empty");
            return result;
        }

        QDir dir(job.destinationPath);
        if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
            result.ok = false;
            result.error = QStringLiteral("Could not create destination directory %1").arg(job.destinationPath);
            return result;
        }

        int written = 0;
        for (const Machine* m : job.machines) {
            if (!m) continue;
            MachineSerializeResult ser = job.adapter->serialize({m}, job.options);
            if (!ser.ok) {
                result.ok = false;
                result.error = ser.error;
                result.diagnostics = ser.diagnostics;
                return result;
            }

            const QString fileName = QStringLiteral("%1.%2").arg(sanitizeFileName(m->name), desc.defaultExtension);
            const QString targetFilePath = dir.filePath(fileName);

            QSaveFile file(targetFilePath);
            if (!file.open(QIODevice::WriteOnly)) {
                result.ok = false;
                result.error = QStringLiteral("Could not write file %1: %2").arg(targetFilePath, file.errorString());
                return result;
            }
            file.write(ser.payload);
            if (!file.commit()) {
                result.ok = false;
                result.error = QStringLiteral("Could not commit save to %1: %2").arg(targetFilePath, file.errorString());
                return result;
            }
            written++;
            result.diagnostics.append(ser.diagnostics);
        }

        result.filesWritten = written;
        result.ok = true;
        return result;
    }

    if (job.scope == ExportScope::MultipleMachinesSingleFile) {
        if (!desc.supportedScopes.testFlag(ExportScope::MultipleMachinesSingleFile)) {
            result.ok = false;
            result.error = QStringLiteral("Format %1 does not support bundling multiple machines into a single file").arg(desc.name);
            return result;
        }

        MachineSerializeResult ser = job.adapter->serialize(job.machines, job.options);
        if (!ser.ok) {
            result.ok = false;
            result.error = ser.error;
            result.diagnostics = ser.diagnostics;
            return result;
        }

        QSaveFile file(job.destinationPath);
        if (!file.open(QIODevice::WriteOnly)) {
            result.ok = false;
            result.error = QStringLiteral("Could not write file %1: %2").arg(job.destinationPath, file.errorString());
            return result;
        }
        file.write(ser.payload);
        if (!file.commit()) {
            result.ok = false;
            result.error = QStringLiteral("Could not commit save to %1: %2").arg(job.destinationPath, file.errorString());
            return result;
        }

        result.filesWritten = 1;
        result.diagnostics = ser.diagnostics;
        if (desc.payloadKind == PayloadKind::Text) {
            result.textPayload = QString::fromUtf8(ser.payload);
        }
        return result;
    }

    result.ok = false;
    result.error = QStringLiteral("Unsupported export scope");
    return result;
}

quint64 executeExportJobAsync(IoDispatcher& dispatcher,
                              const ExportJob& job,
                              bool isModal,
                              std::function<void(const ExportJobResult& result)> onDone) {
    const QString target = job.destinationPath;
    const QString title = QStringLiteral("Exporting %1").arg(job.adapter ? job.adapter->descriptor().name : QStringLiteral("Machine"));

    auto ownedMachines = std::make_shared<QList<Machine>>();
    for (const Machine* m : job.machines) {
        if (m) {
            ownedMachines->append(*m);
        }
    }

    return dispatcher.submitTask<ExportJobResult>(
        events::IoOperationKind::ExportMachine,
        target,
        title,
        isModal,
        [job, ownedMachines](const std::atomic<bool>& cancelToken, IoDispatcher::ProgressCallback progress, QString* error) -> std::optional<ExportJobResult> {
            Q_UNUSED(cancelToken);
            if (progress) progress(10, QStringLiteral("Serializing"));
            ExportJob jobCopy = job;
            QList<const Machine*> ptrs;
            ptrs.reserve(ownedMachines->size());
            for (const Machine& m : *ownedMachines) {
                ptrs.append(&m);
            }
            jobCopy.machines = ptrs;

            ExportJobResult res = executeExportJobSync(jobCopy);
            if (!res.ok) {
                if (error) *error = res.error;
                return std::nullopt;
            }
            if (progress) progress(100, QStringLiteral("Completed"));
            return res;
        },
        [onDone](ExportJobResult res) {
            if (onDone) {
                onDone(res);
            }
        },
        [onDone](const QString& err) {
            if (onDone) {
                ExportJobResult failed;
                failed.ok = false;
                failed.error = err;
                onDone(failed);
            }
        }
    );
}

ExportJobResult exportMachineToText(const std::shared_ptr<MachineFormatAdapter>& adapter,
                                    const Machine& machine,
                                    const FormatOptionMap& options) {
    ExportJobResult result;
    if (!adapter) {
        result.ok = false;
        result.error = QStringLiteral("No export adapter specified");
        return result;
    }

    if (adapter->descriptor().payloadKind != PayloadKind::Text) {
        result.ok = false;
        result.error = QStringLiteral("Format %1 is not a text format").arg(adapter->descriptor().name);
        return result;
    }

    MachineSerializeResult ser = adapter->serialize({&machine}, options);
    result.ok = ser.ok;
    result.error = ser.error;
    result.diagnostics = ser.diagnostics;
    if (ser.ok) {
        result.textPayload = QString::fromUtf8(ser.payload);
    }
    return result;
}

}  // namespace app
