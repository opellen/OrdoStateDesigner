#include "controller/setting_commands.h"

#include <ordo/core/kernel.h>

#include "infra/settings_registry.h"
#include "infra/settings_store.h"

namespace app {

void SetSettingCommand::execute(const events::SetSettingRequested& event, ordo::core::CommandContext& context) {
    auto* store = SettingsStore::activeStore();
    if (!store) {
        return;
    }
    const QJsonValue oldValue = store->get(event.key);

    // Write to store (automatically emits SettingsStore::settingChanged and schedules debounced save)
    store->set(event.key, event.value, event.scope);

    const QJsonValue newValue = store->get(event.key);
    context.send(events::SettingChanged{
        .key = event.key,
        .oldValue = oldValue,
        .newValue = newValue,
        .effectiveScope = store->effectiveScope(event.key)
    });
}

void ResetSettingCommand::execute(const events::ResetSettingRequested& event, ordo::core::CommandContext& context) {
    auto* store = SettingsStore::activeStore();
    if (!store) {
        return;
    }
    const QJsonValue oldValue = store->get(event.key);

    // Remove from store (automatically schedules debounced save)
    store->remove(event.key, event.scope);

    const auto* def = SettingsRegistry::instance().find(event.key);
    const QJsonValue fallback = def ? def->defaultValue : QJsonValue();
    const QJsonValue newValue = store->get(event.key, fallback);

    context.send(events::SettingChanged{
        .key = event.key,
        .oldValue = oldValue,
        .newValue = newValue,
        .effectiveScope = store->effectiveScope(event.key)
    });
}

void registerSettingCommands(ordo::core::Kernel& kernel) {
    kernel.registerCommand<events::SetSettingRequested, SetSettingCommand>();
    kernel.registerCommand<events::ResetSettingRequested, ResetSettingCommand>();
}

}  // namespace app
