#pragma once

#include <ordo/core/command.h>

#include "model/setting_events.h"

namespace ordo::core {
class Kernel;
}

namespace app {

class SetSettingCommand : public ordo::core::Command<events::SetSettingRequested> {
public:
    void execute(const events::SetSettingRequested& event, ordo::core::CommandContext& context) override;
};

class ResetSettingCommand : public ordo::core::Command<events::ResetSettingRequested> {
public:
    void execute(const events::ResetSettingRequested& event, ordo::core::CommandContext& context) override;
};

void registerSettingCommands(ordo::core::Kernel& kernel);

}  // namespace app
