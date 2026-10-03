#pragma once

#include <QString>
#include <any>
#include <string_view>

namespace app::events {

enum class IoOperationKind {
    LoadProject,
    SaveProject,
    ImportMachine,
    ExportMachine,
    GenerateCode
};

struct IoStarted {
    static constexpr std::string_view eventName = "IoStarted";
    quint64 opId = 0;
    IoOperationKind kind = IoOperationKind::LoadProject;
    QString targetPath;
    QString title;
    bool isModal = false;
};

struct IoProgress {
    static constexpr std::string_view eventName = "IoProgress";
    quint64 opId = 0;
    int percentage = -1;  // -1 indicates indeterminate progress
    QString statusMessage;
};

struct IoCompleted {
    static constexpr std::string_view eventName = "IoCompleted";
    quint64 opId = 0;
    IoOperationKind kind = IoOperationKind::LoadProject;
    QString targetPath;
    std::any resultPayload;
};

struct IoFailed {
    static constexpr std::string_view eventName = "IoFailed";
    quint64 opId = 0;
    IoOperationKind kind = IoOperationKind::LoadProject;
    QString targetPath;
    QString errorMessage;
};

}  // namespace app::events
