#pragma once

#include <QByteArray>
#include <QFlags>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <QVector>
#include <functional>
#include <memory>

#include "model/machine.h"

namespace app {

class IoDispatcher;

enum class IoCapability : uint32_t {
    None = 0,
    Import = 1 << 0,
    Export = 1 << 1,
    ImportExport = Import | Export
};

inline IoCapability operator|(IoCapability a, IoCapability b) {
    return static_cast<IoCapability>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline bool hasCapability(IoCapability caps, IoCapability flag) {
    return (static_cast<uint32_t>(caps) & static_cast<uint32_t>(flag)) == static_cast<uint32_t>(flag);
}

enum class ExportScope : uint32_t {
    SingleMachine = 1 << 0,
    MultipleMachinesSingleFile = 1 << 1,
    MultipleMachinesDirectory = 1 << 2,
};
Q_DECLARE_FLAGS(ExportScopes, ExportScope)
Q_DECLARE_OPERATORS_FOR_FLAGS(ExportScopes)

enum class PayloadKind {
    Text,
    Binary
};

enum class OptionType {
    Bool,
    Enum,
    String
};

struct FormatOption {
    QString id;
    QString label;
    QString description;
    OptionType type = OptionType::Bool;
    QVariant defaultValue;
    QStringList choices;  // Populated when type == OptionType::Enum
};

using FormatOptionMap = QVariantMap;

struct MachineFormatDescriptor {
    QString id;
    QString name;
    QString defaultExtension;
    QStringList extensions;
    QString filterPattern;
    IoCapability capabilities = IoCapability::None;
    ExportScopes supportedScopes = ExportScope::SingleMachine;
    PayloadKind payloadKind = PayloadKind::Text;
    QVector<FormatOption> options;
    QString description;
};

struct MachineSerializeResult {
    QByteArray payload;
    QStringList diagnostics;
    bool ok = true;
    QString error;
};

struct MachineExportResult {
    bool ok = true;
    QString error;
    QStringList diagnostics;
};

struct MachineImportResult {
    Machine machine;
    bool ok = true;
    QString error;
    QStringList diagnostics;
};

class MachineFormatAdapter {
public:
    virtual ~MachineFormatAdapter() = default;
    virtual MachineFormatDescriptor descriptor() const = 0;

    // Pure virtual serialize without default arguments.
    // machines contains:
    // - exactly 1 machine for SingleMachine or per-file MultipleMachinesDirectory calls.
    // - all machines for MultipleMachinesSingleFile.
    virtual MachineSerializeResult serialize(const QList<const Machine*>& machines,
                                             const FormatOptionMap& options) = 0;

    virtual MachineImportResult importFile(const QString& path) {
        MachineImportResult res;
        res.ok = false;
        res.error = QStringLiteral("Import not supported for format: %1").arg(descriptor().name);
        return res;
    }
};

class MachineIoRegistry {
public:
    static MachineIoRegistry& instance();

    void registerAdapter(std::shared_ptr<MachineFormatAdapter> adapter);

    QVector<MachineFormatDescriptor> registeredFormats() const;
    QVector<MachineFormatDescriptor> exportFormats() const;
    QVector<MachineFormatDescriptor> importFormats() const;

    std::shared_ptr<MachineFormatAdapter> findAdapterById(const QString& id) const;
    std::shared_ptr<MachineFormatAdapter> findAdapterByExtension(const QString& ext) const;

    // Bidirectional filter resolution (single authority for filter creation & parsing)
    QString filterStringForFormat(const MachineFormatDescriptor& desc) const;
    std::shared_ptr<MachineFormatAdapter> formatForFilter(const QString& filterString) const;

    QString exportFilterString() const;
    QString importFilterString() const;

private:
    MachineIoRegistry();
    void registerDefaultAdapters();

    QVector<std::shared_ptr<MachineFormatAdapter>> adapters_;
};

// Export Pipeline: executes file writes atomically through QSaveFile or IoDispatcher,
// handles per-machine directory batching, and formats text for clipboard copy.
struct ExportJob {
    std::shared_ptr<MachineFormatAdapter> adapter;
    ExportScope scope = ExportScope::SingleMachine;
    QList<const Machine*> machines;
    FormatOptionMap options;
    QString destinationPath;  // File path (SingleMachine / SingleFile) or directory path (Directory)
};

struct ExportJobResult {
    bool ok = true;
    QString error;
    QStringList diagnostics;
    QString textPayload;  // Populated for Text formats (useful for clipboard copy)
    int filesWritten = 0;
};

// Synchronous execution (used by CLI, smoke tests, or direct callers)
ExportJobResult executeExportJobSync(const ExportJob& job);

// Asynchronous execution via IoDispatcher
quint64 executeExportJobAsync(IoDispatcher& dispatcher,
                              const ExportJob& job,
                              bool isModal,
                              std::function<void(const ExportJobResult& result)> onDone = nullptr);

// Clipboard export helper (pure in-memory text serialization without disk I/O)
ExportJobResult exportMachineToText(const std::shared_ptr<MachineFormatAdapter>& adapter,
                                    const Machine& machine,
                                    const FormatOptionMap& options);

}  // namespace app
