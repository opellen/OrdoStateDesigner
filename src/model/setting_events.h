#pragma once

#include <QJsonValue>
#include <QString>
#include <string_view>

#include "infra/settings_store.h"

namespace app::events {

// Published whenever a setting value changes, so views can respond without a restart.
struct SettingChanged {
    static constexpr std::string_view eventName = "SettingChanged";
    QString key;
    QJsonValue oldValue;
    QJsonValue newValue;
    StoreScope effectiveScope = StoreScope::User;
};

// Intent to modify a setting in the store
struct SetSettingRequested {
    static constexpr std::string_view eventName = "SetSettingRequested";
    QString key;
    QJsonValue value;
    StoreScope scope = StoreScope::User;
};

// Intent to remove an override and restore inherited/default value
struct ResetSettingRequested {
    static constexpr std::string_view eventName = "ResetSettingRequested";
    QString key;
    StoreScope scope = StoreScope::User;
};

}  // namespace app::events
