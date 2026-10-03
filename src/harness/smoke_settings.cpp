#include <cstdio>

#include <QCoreApplication>
#include <QDir>
#include <QFile>

#include "harness/harness.h"
#include "infra/settings_registry.h"
#include "infra/settings_store.h"
#include "view/geometry/auto_layout.h"
#include "view/shell/auto_layout_dialog.h"  // loadOptions/storeOptions only -- widget-free, no dialog built here

int runSettingsSmoke() {
    std::printf("[SMOKE] Running settings store and registry smoke...\n");

    // ---- 1. SettingsRegistry Tests ------------------------------------------
    auto& registry = app::SettingsRegistry::instance();
    if (!registry.has(QStringLiteral("canvas.gridStep"))) {
        std::fprintf(stderr, "FAIL: registry missing canvas.gridStep\n");
        return 1;
    }
    if (!registry.has(QStringLiteral("editor.restoreLastProject"))) {
        std::fprintf(stderr, "FAIL: registry missing editor.restoreLastProject\n");
        return 1;
    }
    const auto* restoreDef = registry.find(QStringLiteral("editor.restoreLastProject"));
    if (!restoreDef || restoreDef->defaultValue.toBool() != true ||
        restoreDef->type != app::SettingType::Bool ||
        restoreDef->scope != app::SettingScope::UserOnly ||
        restoreDef->category != QStringLiteral("Editor")) {
        std::fprintf(stderr, "FAIL: editor.restoreLastProject definition corrupted\n");
        return 1;
    }
    if (!registry.has(QStringLiteral("devmode.codeLensEnabled"))) {
        std::fprintf(stderr, "FAIL: registry missing devmode.codeLensEnabled\n");
        return 1;
    }

    const auto* gridDef = registry.find(QStringLiteral("canvas.gridStep"));
    if (!gridDef || gridDef->defaultValue.toInt() != 24 || gridDef->type != app::SettingType::Int) {
        std::fprintf(stderr, "FAIL: canvas.gridStep definition corrupted\n");
        return 1;
    }

    const auto canvasDefs = registry.settingsForCategory(QStringLiteral("Canvas"));
    if (canvasDefs.size() < 3) {
        std::fprintf(stderr, "FAIL: expected at least 3 Canvas settings, got %zu\n", canvasDefs.size());
        return 1;
    }

    // Auto Layout keys: the defaults are the layout engine's own, the ranges
    // the dialog's spin boxes.
    {
        const auto* directionDef = registry.find(QStringLiteral("autoLayout.direction"));
        const auto* layerGapDef = registry.find(QStringLiteral("autoLayout.layerGap"));
        const auto* nodeGapDef = registry.find(QStringLiteral("autoLayout.nodeGap"));
        const QStringList directionNames = {QStringLiteral("LeftToRight"), QStringLiteral("RightToLeft"),
                                            QStringLiteral("TopToBottom"), QStringLiteral("BottomToTop")};
        if (!directionDef || directionDef->type != app::SettingType::Enum ||
            directionDef->defaultValue.toString() != QStringLiteral("LeftToRight") ||
            directionDef->enumOptions != directionNames || directionDef->category != QStringLiteral("Canvas") ||
            directionDef->scope != app::SettingScope::Overridable) {
            std::fprintf(stderr, "FAIL: autoLayout.direction missing or not {Enum, LeftToRight, 4 names, Canvas, Overridable}\n");
            return 1;
        }
        if (!layerGapDef || layerGapDef->type != app::SettingType::Int || layerGapDef->defaultValue.toInt() != 96 ||
            layerGapDef->minimum != 48.0 || layerGapDef->maximum != 480.0 ||
            layerGapDef->category != QStringLiteral("Canvas") || layerGapDef->scope != app::SettingScope::Overridable) {
            std::fprintf(stderr, "FAIL: autoLayout.layerGap missing or not {Int, 96, 48..480, Canvas, Overridable}\n");
            return 1;
        }
        if (!nodeGapDef || nodeGapDef->type != app::SettingType::Int || nodeGapDef->defaultValue.toInt() != 48 ||
            nodeGapDef->minimum != 24.0 || nodeGapDef->maximum != 240.0 ||
            nodeGapDef->category != QStringLiteral("Canvas") || nodeGapDef->scope != app::SettingScope::Overridable) {
            std::fprintf(stderr, "FAIL: autoLayout.nodeGap missing or not {Int, 48, 24..240, Canvas, Overridable}\n");
            return 1;
        }

        app::SettingsStore layoutStore;
        layoutStore.set(QStringLiteral("autoLayout.direction"), QStringLiteral("RightToLeft"), app::StoreScope::User);
        layoutStore.set(QStringLiteral("autoLayout.layerGap"), 144, app::StoreScope::User);
        if (layoutStore.get(QStringLiteral("autoLayout.direction")).toString() != QStringLiteral("RightToLeft") ||
            layoutStore.get(QStringLiteral("autoLayout.layerGap")).toInt() != 144) {
            std::fprintf(stderr, "FAIL: autoLayout.* set/get round trip failed\n");
            return 1;
        }

        // A null store and an empty one both read as the engine defaults.
        const app::AutoLayoutOptions defaults = app::AutoLayoutDialog::loadOptions(nullptr);
        app::SettingsStore emptyStore;
        const app::AutoLayoutOptions fromEmpty = app::AutoLayoutDialog::loadOptions(&emptyStore);
        if (defaults.direction != app::LayoutDirection::LeftToRight || defaults.layerGap != app::kDefaultLayerGap ||
            defaults.nodeGap != app::kDefaultNodeGap || fromEmpty.direction != defaults.direction ||
            fromEmpty.layerGap != defaults.layerGap || fromEmpty.nodeGap != defaults.nodeGap) {
            std::fprintf(stderr, "FAIL: AutoLayoutDialog::loadOptions without stored values is not the engine default\n");
            return 1;
        }

        app::AutoLayoutOptions stored;
        stored.direction = app::LayoutDirection::TopToBottom;
        stored.layerGap = 120.0;
        stored.nodeGap = 72.0;
        app::AutoLayoutDialog::storeOptions(&layoutStore, stored);
        const app::AutoLayoutOptions loaded = app::AutoLayoutDialog::loadOptions(&layoutStore);
        if (loaded.direction != app::LayoutDirection::TopToBottom || loaded.layerGap != 120.0 || loaded.nodeGap != 72.0 ||
            layoutStore.get(QStringLiteral("autoLayout.direction")).toString() != QStringLiteral("TopToBottom")) {
            std::fprintf(stderr, "FAIL: AutoLayoutDialog storeOptions/loadOptions did not round-trip {TopToBottom, 120, 72}\n");
            return 1;
        }

        // Out-of-range and unknown values degrade instead of reaching the layout.
        layoutStore.set(QStringLiteral("autoLayout.direction"), QStringLiteral("Diagonal"), app::StoreScope::User);
        layoutStore.set(QStringLiteral("autoLayout.layerGap"), 9999, app::StoreScope::User);
        layoutStore.set(QStringLiteral("autoLayout.nodeGap"), 1, app::StoreScope::User);
        const app::AutoLayoutOptions degraded = app::AutoLayoutDialog::loadOptions(&layoutStore);
        if (degraded.direction != app::LayoutDirection::LeftToRight || degraded.layerGap != 480.0 ||
            degraded.nodeGap != 24.0) {
            std::fprintf(stderr, "FAIL: AutoLayoutDialog::loadOptions did not fall back / clamp an unusable stored value\n");
            return 1;
        }
        std::printf("PASS: autoLayout.* settings -- three keys with engine defaults, set/get, dialog "
                    "storeOptions/loadOptions round trip {TopToBottom, 120, 72}, unusable values fall back / clamp\n");
    }

    // Dynamic plugin registration test
    app::SettingDefinition pluginDef;
    pluginDef.key = QStringLiteral("plugins.xstate.exportTypes");
    pluginDef.title = QStringLiteral("Export Type Declarations");
    pluginDef.category = QStringLiteral("Plugins");
    pluginDef.type = app::SettingType::Bool;
    pluginDef.defaultValue = true;
    registry.registerSetting(pluginDef);

    if (!registry.has(QStringLiteral("plugins.xstate.exportTypes"))) {
        std::fprintf(stderr, "FAIL: dynamic plugin setting registration failed\n");
        return 1;
    }

    // ---- 2. SettingsStore Tests ---------------------------------------------
    app::SettingsStore store;

    // 2.1 Fallback to default
    const auto* freshGridDef = registry.find(QStringLiteral("canvas.gridStep"));
    if (!freshGridDef) {
        std::fprintf(stderr, "FAIL: could not find canvas.gridStep after plugin registration\n");
        return 1;
    }
    QJsonValue val1 = store.get(QStringLiteral("canvas.gridStep"), freshGridDef->defaultValue);
    if (val1.toInt() != 24) {
        std::fprintf(stderr, "FAIL: expected fallback default 24, got %d\n", val1.toInt());
        return 1;
    }

    // 2.2 User tier override
    store.set(QStringLiteral("canvas.gridStep"), 32, app::StoreScope::User);
    if (store.get(QStringLiteral("canvas.gridStep"), 24).toInt() != 32) {
        std::fprintf(stderr, "FAIL: user setting override failed\n");
        return 1;
    }
    if (store.isOverriddenInWorkspace(QStringLiteral("canvas.gridStep"))) {
        std::fprintf(stderr, "FAIL: should not be marked as workspace overridden\n");
        return 1;
    }

    // 2.3 Workspace tier override (priority over User)
    store.set(QStringLiteral("canvas.gridStep"), 48, app::StoreScope::Workspace);
    if (store.get(QStringLiteral("canvas.gridStep"), 24).toInt() != 48) {
        std::fprintf(stderr, "FAIL: workspace setting did not override user setting (got %d)\n",
                     store.get(QStringLiteral("canvas.gridStep"), 24).toInt());
        return 1;
    }
    if (!store.isOverriddenInWorkspace(QStringLiteral("canvas.gridStep"))) {
        std::fprintf(stderr, "FAIL: should be marked as workspace overridden\n");
        return 1;
    }

    // 2.4 Reverting workspace override returns to User tier
    store.remove(QStringLiteral("canvas.gridStep"), app::StoreScope::Workspace);
    if (store.get(QStringLiteral("canvas.gridStep"), 24).toInt() != 32) {
        std::fprintf(stderr, "FAIL: removing workspace override should revert to user setting (got %d)\n",
                     store.get(QStringLiteral("canvas.gridStep"), 24).toInt());
        return 1;
    }

    // 2.5 Reverting User override returns to fallback
    store.remove(QStringLiteral("canvas.gridStep"), app::StoreScope::User);
    if (store.get(QStringLiteral("canvas.gridStep"), 24).toInt() != 24) {
        std::fprintf(stderr, "FAIL: removing user override should revert to fallback default (got %d)\n",
                     store.get(QStringLiteral("canvas.gridStep"), 24).toInt());
        return 1;
    }

    // 2.6 JSON serialization round-trip
    store.set(QStringLiteral("editor.autoSaveIntervalMs"), 5000, app::StoreScope::User);
    store.set(QStringLiteral("canvas.edgeStyle"), QStringLiteral("Straight"), app::StoreScope::User);
    QString userJson = store.saveUserToJson();

    app::SettingsStore store2;
    if (!store2.loadUserFromJson(userJson)) {
        std::fprintf(stderr, "FAIL: loadUserFromJson failed to parse serialized JSON\n");
        return 1;
    }
    if (store2.get(QStringLiteral("editor.autoSaveIntervalMs")).toInt() != 5000 ||
        store2.get(QStringLiteral("canvas.edgeStyle")).toString() != QStringLiteral("Straight")) {
        std::fprintf(stderr, "FAIL: serialized JSON round-trip data mismatch\n");
        return 1;
    }

    // 2.7 Nested JSON lookup fallback
    QString nestedJson = QStringLiteral("{\"devmode\": {\"codeLensEnabled\": false}}");
    app::SettingsStore store3;
    store3.loadUserFromJson(nestedJson);
    if (store3.get(QStringLiteral("devmode.codeLensEnabled"), true).toBool() != false) {
        std::fprintf(stderr, "FAIL: nested dot lookup fallback failed\n");
        return 1;
    }

    // 2.8 Injected service pointer test
    app::SettingsStore::setActiveStore(&store);
    if (app::SettingsStore::activeStore() != &store) {
        std::fprintf(stderr, "FAIL: activeStore pointer mismatch\n");
        return 1;
    }
    app::SettingsStore::setActiveStore(nullptr);
    if (app::SettingsStore::activeStore() != nullptr) {
        std::fprintf(stderr, "FAIL: activeStore reset failed\n");
        return 1;
    }

    // 2.9 Debounced flush & synchronous flushSync() disk persistence test
    const QString tempDir = QDir::tempPath() + QStringLiteral("/sd_settings_smoke_") + QString::number(QCoreApplication::applicationPid());
    const QString tempUserFile = tempDir + QStringLiteral("/user_settings.json");
    const QString tempWsFile = tempDir + QStringLiteral("/workspace_settings.json");

    {
        app::SettingsStore diskStore(tempUserFile, tempWsFile);
        diskStore.set(QStringLiteral("canvas.gridStep"), 42, app::StoreScope::User);
        diskStore.set(QStringLiteral("canvas.edgeStyle"), QStringLiteral("LargeFillet"), app::StoreScope::Workspace);
        diskStore.flushSync();
    }

    if (!QFile::exists(tempUserFile) || !QFile::exists(tempWsFile)) {
        std::fprintf(stderr, "FAIL: flushSync did not produce setting files on disk\n");
        return 1;
    }

    // 2.10 Reload from disk and verify values
    {
        app::SettingsStore loadedStore(tempUserFile, tempWsFile);
        if (!loadedStore.loadUser() || !loadedStore.loadWorkspace()) {
            std::fprintf(stderr, "FAIL: failed to reload settings from disk\n");
            return 1;
        }
        if (loadedStore.get(QStringLiteral("canvas.gridStep")).toInt() != 42) {
            std::fprintf(stderr, "FAIL: reloaded user setting mismatch (expected 42, got %d)\n",
                         loadedStore.get(QStringLiteral("canvas.gridStep")).toInt());
            return 1;
        }
        if (loadedStore.get(QStringLiteral("canvas.edgeStyle")).toString() != QStringLiteral("LargeFillet")) {
            std::fprintf(stderr, "FAIL: reloaded workspace setting mismatch\n");
            return 1;
        }
    }

    // Cleanup temp files
    QFile::remove(tempUserFile);
    QFile::remove(tempWsFile);
    QDir(tempDir).rmdir(tempDir);

    std::printf("[SMOKE] Settings smoke passed successfully.\n");
    return 0;
}
