#include "infra/settings_registry.h"

namespace app {

SettingsRegistry::SettingsRegistry() {
    registerCoreSettings();
}

SettingsRegistry& SettingsRegistry::instance() {
    static SettingsRegistry registry;
    return registry;
}

void SettingsRegistry::registerSetting(SettingDefinition def) {
    if (def.key.isEmpty()) {
        return;
    }

    if (keyIndexMap_.contains(def.key)) {
        // Update existing definition
        std::size_t idx = keyIndexMap_.value(def.key);
        definitions_[idx] = std::move(def);
        return;
    }

    if (!def.category.isEmpty() && !categories_.contains(def.category)) {
        categories_.append(def.category);
    }

    std::size_t idx = definitions_.size();
    keyIndexMap_.insert(def.key, idx);
    definitions_.push_back(std::move(def));
}

const SettingDefinition* SettingsRegistry::find(const QString& key) const {
    auto it = keyIndexMap_.constFind(key);
    if (it == keyIndexMap_.constEnd()) {
        return nullptr;
    }
    return &definitions_[it.value()];
}

bool SettingsRegistry::has(const QString& key) const {
    return keyIndexMap_.contains(key);
}

std::vector<SettingDefinition> SettingsRegistry::settingsForCategory(const QString& category) const {
    std::vector<SettingDefinition> result;
    for (const auto& def : definitions_) {
        if (def.category.compare(category, Qt::CaseInsensitive) == 0) {
            result.push_back(def);
        }
    }
    return result;
}

void SettingsRegistry::clear() {
    definitions_.clear();
    keyIndexMap_.clear();
    categories_.clear();
}

void SettingsRegistry::registerCoreSettings() {
    if (!definitions_.empty()) {
        return;
    }
    definitions_.reserve(32);

    // 1. Editor
    {
        SettingDefinition def;
        def.key = QStringLiteral("editor.autoSaveIntervalMs");
        def.title = QStringLiteral("Auto Save Interval (ms)");
        def.description = QStringLiteral("Interval in milliseconds between automatic project saves. 0 disables auto save.");
        def.type = SettingType::Int;
        def.defaultValue = 0;
        def.minimum = 0.0;
        def.maximum = 300000.0;
        def.category = QStringLiteral("Editor");
        def.scope = SettingScope::Overridable;
        registerSetting(def);
    }
    {
        SettingDefinition def;
        def.key = QStringLiteral("editor.confirmOnDelete");
        def.title = QStringLiteral("Confirm On Delete");
        def.description = QStringLiteral("Controls whether a confirmation dialog is prompted before deleting canvas items.");
        def.type = SettingType::Bool;
        def.defaultValue = false;
        def.category = QStringLiteral("Editor");
        def.scope = SettingScope::Overridable;
        registerSetting(def);
    }
    {
        SettingDefinition def;
        def.key = QStringLiteral("editor.restoreLastProject");
        def.title = QStringLiteral("Restore Last Project");
        def.description = QStringLiteral("Controls whether the previously active project is automatically reopened on startup.");
        def.type = SettingType::Bool;
        def.defaultValue = true;
        def.scope = SettingScope::UserOnly;
        def.category = QStringLiteral("Editor");
        registerSetting(def);
    }

    // 2. Canvas
    {
        SettingDefinition def;
        def.key = QStringLiteral("canvas.gridStep");
        def.title = QStringLiteral("Grid Step Size (px)");
        def.description = QStringLiteral("Distance in pixels between canvas grid snap points.");
        def.type = SettingType::Int;
        def.defaultValue = 24;
        def.minimum = 8.0;
        def.maximum = 64.0;
        def.category = QStringLiteral("Canvas");
        def.scope = SettingScope::Overridable;
        registerSetting(def);
    }
    {
        SettingDefinition def;
        def.key = QStringLiteral("canvas.edgeStyle");
        def.title = QStringLiteral("Edge Fillet Style");
        def.description = QStringLiteral("Corner routing appearance for orthogonal transition wires.");
        def.type = SettingType::Enum;
        def.defaultValue = QStringLiteral("SmallFillet");
        def.enumOptions = {QStringLiteral("SmallFillet"), QStringLiteral("Chamfer"), QStringLiteral("Straight")};
        def.category = QStringLiteral("Canvas");
        def.scope = SettingScope::Overridable;
        registerSetting(def);
    }
    {
        SettingDefinition def;
        def.key = QStringLiteral("canvas.showMinimap");
        def.title = QStringLiteral("Show Minimap");
        def.description = QStringLiteral("Controls whether the navigation minimap overview is visible in canvas panes.");
        def.type = SettingType::Bool;
        def.defaultValue = true;
        def.category = QStringLiteral("Canvas");
        def.scope = SettingScope::Overridable;
        registerSetting(def);
    }
    // Auto Layout dialog's last choice: enum options are the LayoutDirection
    // enumerator names; defaults are the layout engine's own (kDefaultLayerGap /
    // kDefaultNodeGap in view/geometry/auto_layout.h).
    {
        SettingDefinition def;
        def.key = QStringLiteral("autoLayout.direction");
        def.title = QStringLiteral("Auto Layout Direction");
        def.description = QStringLiteral("Direction in which Auto Layout advances the ranks of states, starting from the initial state.");
        def.type = SettingType::Enum;
        def.defaultValue = QStringLiteral("LeftToRight");
        def.enumOptions = {QStringLiteral("LeftToRight"), QStringLiteral("RightToLeft"), QStringLiteral("TopToBottom"),
                           QStringLiteral("BottomToTop")};
        def.category = QStringLiteral("Canvas");
        def.scope = SettingScope::Overridable;
        registerSetting(def);
    }
    {
        SettingDefinition def;
        def.key = QStringLiteral("autoLayout.layerGap");
        def.title = QStringLiteral("Auto Layout Layer Gap (px)");
        def.description = QStringLiteral("Minimum distance in pixels between two ranks of states laid out by Auto Layout.");
        def.type = SettingType::Int;
        def.defaultValue = 96;
        def.minimum = 48.0;
        def.maximum = 480.0;
        def.category = QStringLiteral("Canvas");
        def.scope = SettingScope::Overridable;
        registerSetting(def);
    }
    {
        SettingDefinition def;
        def.key = QStringLiteral("autoLayout.nodeGap");
        def.title = QStringLiteral("Auto Layout Node Gap (px)");
        def.description = QStringLiteral("Distance in pixels between two states of the same rank laid out by Auto Layout.");
        def.type = SettingType::Int;
        def.defaultValue = 48;
        def.minimum = 24.0;
        def.maximum = 240.0;
        def.category = QStringLiteral("Canvas");
        def.scope = SettingScope::Overridable;
        registerSetting(def);
    }

    // 3. CodeGen
    {
        SettingDefinition def;
        def.key = QStringLiteral("codegen.targetLanguage");
        def.title = QStringLiteral("Target Language");
        def.description = QStringLiteral("Primary programming language emitted by code generators.");
        def.type = SettingType::Enum;
        def.defaultValue = QStringLiteral("cpp20");
        def.enumOptions = {QStringLiteral("cpp20")};
        def.category = QStringLiteral("CodeGen");
        def.scope = SettingScope::Overridable;
        registerSetting(def);
    }
    {
        SettingDefinition def;
        def.key = QStringLiteral("codegen.indentSize");
        def.title = QStringLiteral("Indent Size");
        def.description = QStringLiteral("Number of spaces per indentation level in generated C++ code.");
        def.type = SettingType::Int;
        def.defaultValue = 4;
        def.minimum = 2.0;
        def.maximum = 8.0;
        def.category = QStringLiteral("CodeGen");
        def.scope = SettingScope::Overridable;
        registerSetting(def);
    }
    {
        SettingDefinition def;
        def.key = QStringLiteral("codegen.outputDir");
        def.title = QStringLiteral("Output Directory");
        def.description = QStringLiteral(
            "Where Generate C++ writes files, resolved relative to the .sdp project file's own directory. "
            "Empty asks for a directory every time generation runs.");
        def.type = SettingType::DirPath;
        def.defaultValue = QString();
        def.category = QStringLiteral("CodeGen");
        def.scope = SettingScope::ProjectOnly;
        registerSetting(def);
    }
    {
        SettingDefinition def;
        def.key = QStringLiteral("codegen.rootNamespace");
        def.title = QStringLiteral("Root Namespace");
        def.description = QStringLiteral("C++ namespace generated code is emitted under.");
        def.type = SettingType::String;
        def.defaultValue = QStringLiteral("app::generated");
        def.category = QStringLiteral("CodeGen");
        def.scope = SettingScope::ProjectOnly;
        registerSetting(def);
    }

    // 4. DevMode
    {
        SettingDefinition def;
        def.key = QStringLiteral("devmode.codeLensEnabled");
        def.title = QStringLiteral("Enable In-Canvas Code Lens");
        def.description = QStringLiteral("Enables spacebar gesture to peek generated C++ node code directly over canvas.");
        def.type = SettingType::Bool;
        def.defaultValue = true;
        def.category = QStringLiteral("DevMode");
        def.scope = SettingScope::Overridable;
        registerSetting(def);
    }
}

}  // namespace app
