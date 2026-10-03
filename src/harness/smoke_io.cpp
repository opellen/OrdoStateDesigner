#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QThread>
#include <cstdio>
#include <thread>

#include "harness/harness.h"
#include "infra/io_dispatcher.h"
#include "infra/machine_io.h"
#include "infra/plantuml_io.h"
#include "model/io_events.h"
#include "model/machine.h"

int runIoSmoke() {
    std::printf("[SMOKE] Running IoDispatcher asynchronous pipeline smoke...\n");

    // Ensure a QCoreApplication exists if not already present
    int argc = 0;
    char* argv[] = {nullptr};
    std::unique_ptr<QCoreApplication> tempApp;
    if (!QCoreApplication::instance()) {
        tempApp = std::make_unique<QCoreApplication>(argc, argv);
    }

    // ---- 1. DirectSynchronous Mode Tests -------------------------------------
    {
        app::IoDispatcher dispatcher;
        dispatcher.setExecutionPolicy(app::ExecutionPolicy::DirectSynchronous);

        bool startedSeen = false;
        bool completedSeen = false;
        bool failedSeen = false;
        int progressCount = 0;

        QObject::connect(&dispatcher, &app::IoDispatcher::ioStarted, [&](const app::events::IoStarted& ev) {
            if (ev.title == QStringLiteral("Sync Task")) {
                startedSeen = true;
            }
        });
        QObject::connect(&dispatcher, &app::IoDispatcher::ioProgress, [&](const app::events::IoProgress& ev) {
            if (ev.percentage == 50) {
                progressCount++;
            }
        });
        QObject::connect(&dispatcher, &app::IoDispatcher::ioCompleted, [&](const app::events::IoCompleted& ev) {
            completedSeen = true;
        });
        QObject::connect(&dispatcher, &app::IoDispatcher::ioFailed, [&](const app::events::IoFailed& ev) {
            failedSeen = true;
        });

        bool successCallbackCalled = false;
        QString taskResult;

        quint64 opId = dispatcher.submitTask<QString>(
            app::events::IoOperationKind::LoadProject,
            QStringLiteral("/path/to/test.sdp"),
            QStringLiteral("Sync Task"),
            /*isModal=*/true,
            [](const std::atomic<bool>& cancelToken, app::IoDispatcher::ProgressCallback progress, QString* error) -> std::optional<QString> {
                progress(50, QStringLiteral("Halfway there"));
                return QStringLiteral("PayloadLoaded");
            },
            [&](QString result) {
                successCallbackCalled = true;
                taskResult = result;
            },
            [&](const QString& error) {
                std::fprintf(stderr, "FAIL: sync task unexpectedly errored: %s\n", error.toUtf8().constData());
            }
        );

        if (opId == 0 || !startedSeen || !completedSeen || failedSeen || progressCount != 1) {
            std::fprintf(stderr, "FAIL: DirectSynchronous signal emission mismatch\n");
            return 1;
        }
        if (!successCallbackCalled || taskResult != QStringLiteral("PayloadLoaded")) {
            std::fprintf(stderr, "FAIL: DirectSynchronous result mismatch: got %s\n", taskResult.toUtf8().constData());
            return 1;
        }

        // Test error reporting in DirectSynchronous
        bool errorCallbackCalled = false;
        dispatcher.submitAction(
            app::events::IoOperationKind::SaveProject,
            QStringLiteral("/path/to/fail.sdp"),
            QStringLiteral("Failing Task"),
            /*isModal=*/false,
            [](const std::atomic<bool>& cancelToken, app::IoDispatcher::ProgressCallback progress, QString* error) {
                if (error) *error = QStringLiteral("Disk full error");
                return false;
            },
            []() {
                std::fprintf(stderr, "FAIL: failing task unexpectedly succeeded\n");
            },
            [&](const QString& err) {
                if (err == QStringLiteral("Disk full error")) {
                    errorCallbackCalled = true;
                }
            }
        );

        if (!errorCallbackCalled) {
            std::fprintf(stderr, "FAIL: DirectSynchronous error dispatch did not fire\n");
            return 1;
        }
    }

    // ---- 2. Async Mode & Thread Concurrency Tests ----------------------------
    {
        app::IoDispatcher dispatcher;
        dispatcher.setExecutionPolicy(app::ExecutionPolicy::Async);

        const Qt::HANDLE mainThreadId = QThread::currentThreadId();
        Qt::HANDLE workerThreadId = nullptr;
        Qt::HANDLE callbackThreadId = nullptr;
        bool asyncSuccess = false;
        int computedVal = 0;

        quint64 opId = dispatcher.submitTask<int>(
            app::events::IoOperationKind::ImportMachine,
            QStringLiteral("test.json"),
            QStringLiteral("Async Import"),
            /*isModal=*/true,
            [&](const std::atomic<bool>& cancelToken, app::IoDispatcher::ProgressCallback progress, QString* error) -> std::optional<int> {
                workerThreadId = QThread::currentThreadId();
                progress(25, QStringLiteral("Parsing"));
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                progress(75, QStringLiteral("Validating"));
                return 42;
            },
            [&](int val) {
                callbackThreadId = QThread::currentThreadId();
                computedVal = val;
                asyncSuccess = true;
            },
            [](const QString& err) {
                std::fprintf(stderr, "FAIL: async task failed: %s\n", err.toUtf8().constData());
            }
        );

        // Spin Qt event loop until task completes (timeout at 3s)
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!asyncSuccess && std::chrono::steady_clock::now() < deadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        dispatcher.waitForDone();

        if (!asyncSuccess || computedVal != 42) {
            std::fprintf(stderr, "FAIL: async task did not complete with expected value\n");
            return 1;
        }
        if (workerThreadId == mainThreadId) {
            std::fprintf(stderr, "FAIL: async work did not execute in background worker thread\n");
            return 1;
        }
        if (callbackThreadId != mainThreadId) {
            std::fprintf(stderr, "FAIL: async callback did not marshal back to main GUI thread\n");
            return 1;
        }

        // ---- 3. Cancellation Token Test -------------------------------------
        bool cancelHandled = false;
        quint64 cancelOpId = dispatcher.submitAction(
            app::events::IoOperationKind::ExportMachine,
            QStringLiteral("long_export.json"),
            QStringLiteral("Cancelable Task"),
            /*isModal=*/false,
            [](const std::atomic<bool>& cancelToken, app::IoDispatcher::ProgressCallback progress, QString* error) {
                for (int i = 0; i < 50; ++i) {
                    if (cancelToken.load()) {
                        if (error) *error = QStringLiteral("Canceled by user");
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                return true;
            },
            []() {
                std::fprintf(stderr, "FAIL: canceled task should not have succeeded\n");
            },
            [&](const QString& err) {
                cancelHandled = true;
            }
        );

        // Cancel immediately
        dispatcher.cancel(cancelOpId);

        const auto cancelDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!cancelHandled && std::chrono::steady_clock::now() < cancelDeadline) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        dispatcher.waitForDone();

        if (!cancelHandled) {
            std::fprintf(stderr, "FAIL: cancellation token did not trigger error/cancel callback\n");
            return 1;
        }
    }

    // ---- 4. MachineIoRegistry & Multi-Format Tests ---------------------------
    {
        auto& registry = app::MachineIoRegistry::instance();

        // 4.1 Format registrations & capabilities
        auto formats = registry.registeredFormats();
        if (formats.size() < 2) {
            std::fprintf(stderr, "FAIL: MachineIoRegistry should have at least 2 formats registered (got %d)\n",
                         static_cast<int>(formats.size()));
            return 1;
        }

        auto xstateAdapter = registry.findAdapterById(QStringLiteral("xstate-v5"));
        if (!xstateAdapter) {
            std::fprintf(stderr, "FAIL: xstate-v5 adapter not found by ID\n");
            return 1;
        }
        if (!hasCapability(xstateAdapter->descriptor().capabilities, app::IoCapability::Import) ||
            !hasCapability(xstateAdapter->descriptor().capabilities, app::IoCapability::Export)) {
            std::fprintf(stderr, "FAIL: xstate-v5 adapter capability mismatch\n");
            return 1;
        }

        auto pumlAdapter = registry.findAdapterById(QStringLiteral("plantuml"));
        if (!pumlAdapter) {
            std::fprintf(stderr, "FAIL: plantuml adapter not found by ID\n");
            return 1;
        }
        if (!hasCapability(pumlAdapter->descriptor().capabilities, app::IoCapability::Export) ||
            hasCapability(pumlAdapter->descriptor().capabilities, app::IoCapability::Import)) {
            std::fprintf(stderr, "FAIL: plantuml adapter should be Export-only\n");
            return 1;
        }

        // 4.2 Scopes, payload kinds, and option schema
        if (!pumlAdapter->descriptor().supportedScopes.testFlag(app::ExportScope::SingleMachine) ||
            !pumlAdapter->descriptor().supportedScopes.testFlag(app::ExportScope::MultipleMachinesDirectory)) {
            std::fprintf(stderr, "FAIL: plantuml supportedScopes declaration mismatch\n");
            return 1;
        }
        if (pumlAdapter->descriptor().payloadKind != app::PayloadKind::Text) {
            std::fprintf(stderr, "FAIL: plantuml payloadKind should be Text\n");
            return 1;
        }
        if (pumlAdapter->descriptor().options.size() < 2) {
            std::fprintf(stderr, "FAIL: plantuml should expose at least 2 options\n");
            return 1;
        }
        if (pumlAdapter->descriptor().options.first().type != app::OptionType::Bool) {
            std::fprintf(stderr, "FAIL: plantuml option type should be Bool\n");
            return 1;
        }

        // 4.3 Extension and filter resolution
        if (registry.findAdapterByExtension(QStringLiteral("json")) != xstateAdapter ||
            registry.findAdapterByExtension(QStringLiteral(".json")) != xstateAdapter) {
            std::fprintf(stderr, "FAIL: lookup by .json extension mismatch\n");
            return 1;
        }
        if (registry.findAdapterByExtension(QStringLiteral("puml")) != pumlAdapter ||
            registry.findAdapterByExtension(QStringLiteral(".plantuml")) != pumlAdapter) {
            std::fprintf(stderr, "FAIL: lookup by .puml / .plantuml extension mismatch\n");
            return 1;
        }
        if (registry.formatForFilter(registry.filterStringForFormat(pumlAdapter->descriptor())) != pumlAdapter) {
            std::fprintf(stderr, "FAIL: formatForFilter resolution failed for PlantUML\n");
            return 1;
        }
        if (registry.formatForFilter(registry.filterStringForFormat(xstateAdapter->descriptor())) != xstateAdapter) {
            std::fprintf(stderr, "FAIL: formatForFilter resolution failed for XState\n");
            return 1;
        }

        // 4.4 Filter strings
        const QString exportFilters = registry.exportFilterString();
        if (!exportFilters.contains(QStringLiteral("PlantUML")) ||
            !exportFilters.contains(QStringLiteral("XState v5"))) {
            std::fprintf(stderr, "FAIL: exportFilterString missing expected formats\n");
            return 1;
        }
        const QString importFilters = registry.importFilterString();
        if (importFilters.contains(QStringLiteral("PlantUML")) ||
            !importFilters.contains(QStringLiteral("XState v5"))) {
            std::fprintf(stderr, "FAIL: importFilterString unexpected filter format\n");
            return 1;
        }

        // 4.5 Round-trip and format export tests
        app::Machine machine;
        machine.name = QStringLiteral("IoSmokeMachine");
        machine.initialStateId = 1;

        app::State s1;
        s1.id = 1;
        s1.name = QStringLiteral("Off");
        app::State s2;
        s2.id = 2;
        s2.name = QStringLiteral("On");
        machine.states = {s1, s2};

        app::Transition t1;
        t1.id = 10;
        t1.from = 1;
        t1.to = 2;
        t1.event = QStringLiteral("TOGGLE");
        machine.transitions = {t1};

        const QString tempPuml = QDir::tempPath() + QStringLiteral("/sd_smoke_io_%1.puml").arg(QCoreApplication::applicationPid());
        const QString tempJson = QDir::tempPath() + QStringLiteral("/sd_smoke_io_%1.json").arg(QCoreApplication::applicationPid());

        // In-memory text export (for clipboard copy)
        auto textRes = app::exportMachineToText(pumlAdapter, machine, {});
        if (!textRes.ok || !textRes.textPayload.contains(QStringLiteral("@startuml")) ||
            !textRes.textPayload.contains(QStringLiteral("TOGGLE"))) {
            std::fprintf(stderr, "FAIL: exportMachineToText failed or missing tags\n");
            return 1;
        }

        // Pipeline SingleMachine Export (PlantUML)
        app::ExportJob pumlJob;
        pumlJob.adapter = pumlAdapter;
        pumlJob.scope = app::ExportScope::SingleMachine;
        pumlJob.machines = {&machine};
        pumlJob.destinationPath = tempPuml;
        auto pumlRes = app::executeExportJobSync(pumlJob);
        if (!pumlRes.ok || pumlRes.filesWritten != 1) {
            std::fprintf(stderr, "FAIL: PlantUML pipeline export failed: %s\n", pumlRes.error.toUtf8().constData());
            QFile::remove(tempPuml);
            return 1;
        }

        QFile pumlFile(tempPuml);
        if (!pumlFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::fprintf(stderr, "FAIL: could not read back exported PlantUML file\n");
            QFile::remove(tempPuml);
            return 1;
        }
        QString pumlText = QString::fromUtf8(pumlFile.readAll());
        pumlFile.close();
        QFile::remove(tempPuml);

        if (!pumlText.contains(QStringLiteral("@startuml")) ||
            !pumlText.contains(QStringLiteral("@enduml")) ||
            !pumlText.contains(QStringLiteral("TOGGLE"))) {
            std::fprintf(stderr, "FAIL: exported PlantUML missing required tags or events\n");
            return 1;
        }

        // Pipeline MultipleMachinesDirectory Export (PlantUML)
        const QString tempDir = QDir::tempPath() + QStringLiteral("/sd_smoke_io_dir_%1").arg(QCoreApplication::applicationPid());
        app::ExportJob dirJob;
        dirJob.adapter = pumlAdapter;
        dirJob.scope = app::ExportScope::MultipleMachinesDirectory;
        dirJob.machines = {&machine};
        dirJob.destinationPath = tempDir;
        auto dirRes = app::executeExportJobSync(dirJob);
        if (!dirRes.ok || dirRes.filesWritten != 1) {
            std::fprintf(stderr, "FAIL: MultipleMachinesDirectory export failed: %s\n", dirRes.error.toUtf8().constData());
            QDir(tempDir).removeRecursively();
            return 1;
        }
        QDir(tempDir).removeRecursively();

        // Pipeline SingleMachine Export (XState JSON)
        app::ExportJob xstateJob;
        xstateJob.adapter = xstateAdapter;
        xstateJob.scope = app::ExportScope::SingleMachine;
        xstateJob.machines = {&machine};
        xstateJob.destinationPath = tempJson;
        auto xstateExpRes = app::executeExportJobSync(xstateJob);
        if (!xstateExpRes.ok) {
            std::fprintf(stderr, "FAIL: XState pipeline export failed: %s\n", xstateExpRes.error.toUtf8().constData());
            QFile::remove(tempJson);
            return 1;
        }

        // Import XState JSON
        auto xstateImpRes = xstateAdapter->importFile(tempJson);
        QFile::remove(tempJson);
        if (!xstateImpRes.ok) {
            std::fprintf(stderr, "FAIL: XState import failed: %s\n", xstateImpRes.error.toUtf8().constData());
            return 1;
        }
        if (xstateImpRes.machine.states.size() != 2 || xstateImpRes.machine.transitions.size() != 1) {
            std::fprintf(stderr, "FAIL: XState imported machine mismatch: states=%d transitions=%d\n",
                         static_cast<int>(xstateImpRes.machine.states.size()),
                         static_cast<int>(xstateImpRes.machine.transitions.size()));
            return 1;
        }

        // Test-only MockBinaryFormatAdapter for Binary payloadKind and MultipleMachinesSingleFile bundle scope
        class MockBinaryFormatAdapter : public app::MachineFormatAdapter {
        public:
            app::MachineFormatDescriptor descriptor() const override {
                app::MachineFormatDescriptor d;
                d.id = QStringLiteral("mock-binary");
                d.name = QStringLiteral("Mock Binary");
                d.defaultExtension = QStringLiteral("bin");
                d.extensions = {QStringLiteral("bin")};
                d.capabilities = app::IoCapability::Export;
                d.supportedScopes = app::ExportScope::SingleMachine | app::ExportScope::MultipleMachinesSingleFile;
                d.payloadKind = app::PayloadKind::Binary;
                return d;
            }

            app::MachineSerializeResult serialize(const QList<const app::Machine*>& machines,
                                                  const app::FormatOptionMap& options) override {
                Q_UNUSED(options);
                app::MachineSerializeResult res;
                res.payload = QByteArray("\x00\x01\x02\xFF", 4);
                res.ok = true;
                return res;
            }
        };

        auto binaryAdapter = std::make_shared<MockBinaryFormatAdapter>();

        // Assert: Binary payloadKind format must refuse exportMachineToText
        auto binTextRes = app::exportMachineToText(binaryAdapter, machine, {});
        if (binTextRes.ok) {
            std::fprintf(stderr, "FAIL: exportMachineToText should refuse binary payloadKind\n");
            return 1;
        }

        // Assert: MultipleMachinesSingleFile bundling works on formats declaring it
        const QString tempBundle = QDir::tempPath() + QStringLiteral("/sd_smoke_bundle_%1.bin").arg(QCoreApplication::applicationPid());
        app::ExportJob bundleJob;
        bundleJob.adapter = binaryAdapter;
        bundleJob.scope = app::ExportScope::MultipleMachinesSingleFile;
        bundleJob.machines = {&machine, &machine};
        bundleJob.destinationPath = tempBundle;
        auto bundleRes = app::executeExportJobSync(bundleJob);
        if (!bundleRes.ok || bundleRes.filesWritten != 1) {
            std::fprintf(stderr, "FAIL: MultipleMachinesSingleFile export failed: %s\n", bundleRes.error.toUtf8().constData());
            QFile::remove(tempBundle);
            return 1;
        }
        QFile bundleFile(tempBundle);
        if (!bundleFile.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "FAIL: could not read bundle file\n");
            QFile::remove(tempBundle);
            return 1;
        }
        QByteArray bundleData = bundleFile.readAll();
        bundleFile.close();
        QFile::remove(tempBundle);
        if (bundleData.size() != 4 || bundleData != QByteArray("\x00\x01\x02\xFF", 4)) {
            std::fprintf(stderr, "FAIL: bundle payload mismatch\n");
            return 1;
        }

        // Assert: format NOT declaring MultipleMachinesSingleFile is rejected
        app::ExportJob invalidBundleJob;
        invalidBundleJob.adapter = pumlAdapter;
        invalidBundleJob.scope = app::ExportScope::MultipleMachinesSingleFile;
        invalidBundleJob.machines = {&machine};
        invalidBundleJob.destinationPath = tempBundle;
        auto invalidRes = app::executeExportJobSync(invalidBundleJob);
        if (invalidRes.ok) {
            std::fprintf(stderr, "FAIL: PlantUML should reject MultipleMachinesSingleFile scope\n");
            return 1;
        }

        // Assert: Bidirectional filter resolution in MachineIoRegistry
        const QString pumlFilter = app::MachineIoRegistry::instance().filterStringForFormat(pumlAdapter->descriptor());
        auto resolvedPuml = app::MachineIoRegistry::instance().formatForFilter(pumlFilter);
        if (!resolvedPuml || resolvedPuml->descriptor().id != QStringLiteral("plantuml")) {
            std::fprintf(stderr, "FAIL: filter resolution failed for PlantUML filter '%s'\n", pumlFilter.toUtf8().constData());
            return 1;
        }

        const QString xstateFilter = app::MachineIoRegistry::instance().filterStringForFormat(xstateAdapter->descriptor());
        auto resolvedXState = app::MachineIoRegistry::instance().formatForFilter(xstateFilter);
        if (!resolvedXState || resolvedXState->descriptor().id != QStringLiteral("xstate-v5")) {
            std::fprintf(stderr, "FAIL: filter resolution failed for XState filter '%s'\n", xstateFilter.toUtf8().constData());
            return 1;
        }
    }

    std::printf("[SMOKE] IoDispatcher and MachineIo smoke PASS\n");
    return 0;
}
