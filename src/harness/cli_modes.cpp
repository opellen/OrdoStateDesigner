// CLI mode runners: --codegen-check, --import-xstate, --project/--generate,
// --machine-drift-check, --machine-doc-check, and --resave-machine.

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHash>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPair>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointF>
#include <QRect>
#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <ordo/core/kernel.h>
#include <ordo/qt/view_host.h>

#include "controller/edit_commands.h"
#include "controller/sim_commands.h"
#include "controller/undo_capture.h"
#include "controller/undo_commands.h"
#include "infra/code_generator.h"
#include "infra/machine_io.h"
#include "infra/machine_validator.h"
#include "infra/project_io.h"
#include "infra/sim_clock.h"
#include "infra/xstate_v5_io.h"
#include "model/machine.h"
#include "model/machine_doc.h"
#include "model/machine_events.h"
#include "model/sim_agent.h"
#include "model/sim_events.h"
#include "model/undo_events.h"
#include "model/undo_store.h"
#include "view/canvas/canvas_presenter.h"
#include "view/canvas/canvas_view.h"
#include "view/generated/canvas_interaction_core.h"  // phase 5b drives the machine directly
#include "view/geometry/auto_layout.h"  // machineHasNoGeometry() for the no-fonts CLI import note
#include "view/shell/document_session.h"
#include "view/shell/editor_view.h"
#include "view/shell/inspector_panel.h"
#include "view/shell/main_window.h"
#include "view/items/machine_frame_item.h"
#include "view/shell/minimap_view.h"
#include "view/items/note_item.h"
#include "view/geometry/pill_port_resolver.h"  // phase 8 asserts the rule table 1:1
#include "view/items/state_item.h"
#include "view/shell/theme.h"
#include "view/shell/trace_panel.h"
#include "view/items/transition_item.h"

#include "harness/harness.h"

// Headless drift-detection proof for the code generator; needs no QApplication.
// Builds a named login-flow machine with a guarded Login transition, validate()s
// it (must be clean), generate()s it, and writes the result into `outputDir`,
// printing every written path.
int runCodegenCheck(const QString& outputDir) {
    ordo::core::Kernel kernel;
    kernel.registerAgent(std::make_shared<app::MachineDocAgent>());
    auto doc = kernel.agentAs<app::MachineDocAgent>(app::MachineDocAgent::kName);
    if (!doc) {
        std::fprintf(stderr, "FAIL: codegen-check's MachineDocAgent did not register under kName\n");
        return 1;
    }
    registerEditCommands(kernel);

    kernel.send(app::events::AddStateRequested{.pos = QPointF(80, 200)});     // id 1: LoggedOut
    kernel.send(app::events::AddStateRequested{.pos = QPointF(320, 180)});    // id 2: Authenticating
    kernel.send(app::events::AddStateRequested{.pos = QPointF(560, 120)});    // id 3: LoggedIn
    kernel.send(app::events::AddStateRequested{.pos = QPointF(320, 360)});    // id 4: Error
    kernel.send(app::events::RenameStateRequested{.id = 1, .name = QStringLiteral("LoggedOut")});
    kernel.send(app::events::RenameStateRequested{.id = 2, .name = QStringLiteral("Authenticating")});
    kernel.send(app::events::RenameStateRequested{.id = 3, .name = QStringLiteral("LoggedIn")});
    kernel.send(app::events::RenameStateRequested{.id = 4, .name = QStringLiteral("Error")});
    kernel.send(app::events::SetInitialStateRequested{.id = 1});
    kernel.send(app::events::SetStateKindRequested{.id = 3, .kind = app::StateKind::Final});
    kernel.send(
        app::events::SetEntryActionsRequested{.id = 2, .entryActions = QStringList{QStringLiteral("beginLogin()")}});
    kernel.send(
        app::events::SetEntryActionsRequested{.id = 4, .entryActions = QStringList{QStringLiteral("showError()")}});
    kernel.send(app::events::AddTransitionRequested{.from = 1, .to = 2});  // id 5: Login
    kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 3});  // id 6: Success
    kernel.send(app::events::AddTransitionRequested{.from = 2, .to = 4});  // id 7: Failure
    kernel.send(app::events::AddTransitionRequested{.from = 4, .to = 1});  // id 8: Reset
    kernel.send(app::events::SetTransitionEventRequested{.id = 5, .event = QStringLiteral("Login")});
    kernel.send(app::events::SetTransitionEventRequested{.id = 6, .event = QStringLiteral("Success")});
    kernel.send(app::events::SetTransitionEventRequested{.id = 7, .event = QStringLiteral("Failure")});
    kernel.send(app::events::SetTransitionEventRequested{.id = 8, .event = QStringLiteral("Reset")});
    kernel.send(app::events::SetTransitionGuardRequested{.id = 5, .guard = QStringLiteral("canLogin")});

    // Machine::name is set via restore(); there is no edit intent for it.
    app::Machine machine = doc->machine();
    machine.name = QStringLiteral("Login Flow");
    doc->restore(machine);

    removeEditCommands(kernel);

    const QVector<app::Problem> problems = app::validate(doc->machine());
    bool hasError = false;
    for (const app::Problem& problem : problems) {
        std::fprintf(stderr, "%s: %s (state=%llu transition=%llu)\n",
                      problem.severity == app::ProblemSeverity::Error ? "ERROR" : "WARNING",
                      qUtf8Printable(problem.text), static_cast<unsigned long long>(problem.stateId),
                      static_cast<unsigned long long>(problem.transitionId));
        hasError = hasError || problem.severity == app::ProblemSeverity::Error;
    }
    if (hasError) {
        std::fprintf(stderr, "FAIL: codegen-check's login-flow topology failed validate()\n");
        return 1;
    }
    const QVector<app::GeneratedFile> files = app::generate(doc->machine(), QStringLiteral("app::generated"));
    QString error;
    if (!app::writeGeneratedFiles(outputDir, files, &error)) {
        std::fprintf(stderr, "FAIL: writeGeneratedFiles failed: %s\n", qUtf8Printable(error));
        return 1;
    }
    for (const app::GeneratedFile& file : files) {
        std::printf("wrote %s\n", qUtf8Printable(outputDir + QStringLiteral("/") + file.relativePath));
    }

    // The explicit file list must include the standalone core: the GUI writes
    // whatever generate() returns, so only this list catches a dropped file.
    static const char* const kExpectedSuffixes[] = {"_state.h",    "_events.h",   "_hooks.h", "_core.h",
                                                     "_agent.h",    "_commands.h", "_bootstrap.h"};
    for (const char* suffix : kExpectedSuffixes) {
        bool present = false;
        for (const app::GeneratedFile& file : files) {
            present = present || file.relativePath.endsWith(QLatin1String(suffix));
        }
        if (!present) {
            std::fprintf(stderr, "FAIL: generate() emitted no <machine>%s\n", suffix);
            return 1;
        }
    }

    // The domain/ stubs write once and never overwrite: proven by tampering
    // with a written stub and re-running the writer.
    const QVector<app::GeneratedFile> stubs =
        app::generateDomainStubs(doc->machine(), QStringLiteral("app::generated"));
    int stubsWritten = 0;
    if (!app::writeDomainStubsIfAbsent(outputDir, stubs, &stubsWritten, &error)) {
        std::fprintf(stderr, "FAIL: writeDomainStubsIfAbsent failed: %s\n", qUtf8Printable(error));
        return 1;
    }
    if (stubsWritten != stubs.size() || stubs.isEmpty()) {
        std::fprintf(stderr, "FAIL: first stub write wrote %d of %d domain/ file(s)\n", stubsWritten,
                     static_cast<int>(stubs.size()));
        return 1;
    }
    const QString tamperedPath = outputDir + QStringLiteral("/") + stubs.first().relativePath;
    {
        QFile tampered(tamperedPath);
        if (!tampered.open(QIODevice::Append)) {
            std::fprintf(stderr, "FAIL: could not append the tamper sentinel to %s\n", qUtf8Printable(tamperedPath));
            return 1;
        }
        tampered.write("// DEVELOPER-EDIT SENTINEL\n");
    }
    if (!app::writeDomainStubsIfAbsent(outputDir, stubs, &stubsWritten, &error) || stubsWritten != 0) {
        std::fprintf(stderr, "FAIL: a second stub write ran again (wrote %d) -- stubs must never overwrite\n",
                     stubsWritten);
        return 1;
    }
    QFile reread(tamperedPath);
    if (!reread.open(QIODevice::ReadOnly) || !reread.readAll().contains("DEVELOPER-EDIT SENTINEL")) {
        std::fprintf(stderr, "FAIL: the developer-edit sentinel did not survive a stub re-write pass\n");
        return 1;
    }
    for (const app::GeneratedFile& stub : stubs) {
        std::printf("wrote %s (once)\n", qUtf8Printable(outputDir + QStringLiteral("/") + stub.relativePath));
    }

    std::printf("PASS: state-designer codegen-check (login-flow validate + generate + write, %d file(s) + %d "
                 "domain stub(s), stubs never overwritten)\n",
                 static_cast<int>(files.size()), static_cast<int>(stubs.size()));
    return 0;
}

int runExportMachine(const QString& formatId, const QString& machinePath, const QString& outputPath) {
    app::Machine machine;
    QString error;
    if (!app::loadMachine(machinePath, &machine, &error)) {
        std::fprintf(stderr, "FAIL: could not load %s: %s\n", qUtf8Printable(machinePath), qUtf8Printable(error));
        return 1;
    }
    auto adapter = app::MachineIoRegistry::instance().findAdapterById(formatId);
    if (!adapter) {
        adapter = app::MachineIoRegistry::instance().findAdapterByExtension(QFileInfo(outputPath).suffix());
    }
    if (!adapter) {
        std::fprintf(stderr, "FAIL: unknown export format '%s'\n", qUtf8Printable(formatId));
        return 1;
    }
    app::ExportJob job;
    job.adapter = adapter;
    job.scope = app::ExportScope::SingleMachine;
    job.machines = {&machine};
    job.destinationPath = outputPath;
    app::ExportJobResult res = app::executeExportJobSync(job);
    if (!res.ok) {
        std::fprintf(stderr, "FAIL: could not export %s: %s\n", qUtf8Printable(outputPath), qUtf8Printable(res.error));
        return 1;
    }
    for (const QString& d : res.diagnostics) {
        std::fprintf(stderr, "note: %s\n", qUtf8Printable(d));
    }
    std::printf("exported %s -> %s via %s (%d state(s), %d transition(s))\n",
                qUtf8Printable(machine.name), qUtf8Printable(outputPath),
                qUtf8Printable(adapter->descriptor().name),
                static_cast<int>(machine.states.size()), static_cast<int>(machine.transitions.size()));
    return 0;
}

int runExportXState(const QString& machinePath, const QString& outputPath) {
    return runExportMachine(QStringLiteral("xstate-v5"), machinePath, outputPath);
}

// Headless CI pipeline: import -> validate -> [save] -> [generate] -> write.
// Import diagnostics go to stderr as notes and are never fatal; the exit code
// is non-zero only for a structural import failure or a validator Error.
// savePath/outputDir are each optional (empty = skip); main() requires at
// least one. The save happens before generate. No QApplication.
int runImportMachine(const QString& formatId, const QString& inputPath, const QString& outputDir, const QString& savePath) {
    auto adapter = app::MachineIoRegistry::instance().findAdapterById(formatId);
    if (!adapter) {
        adapter = app::MachineIoRegistry::instance().findAdapterByExtension(QFileInfo(inputPath).suffix());
    }
    if (!adapter) {
        std::fprintf(stderr, "FAIL: unknown import format '%s'\n", qUtf8Printable(formatId));
        return 1;
    }
    app::MachineImportResult imported = adapter->importFile(inputPath);
    for (const QString& line : imported.diagnostics) {
        std::fprintf(stderr, "note: %s\n", qUtf8Printable(line));
    }
    if (!imported.ok) {
        std::fprintf(stderr, "FAIL: could not import %s: %s\n", qUtf8Printable(inputPath),
                     qUtf8Printable(imported.error));
        return 1;
    }
    const app::Machine& machine = imported.machine;
    std::printf("imported %s (%s): %d state(s), %d transition(s)\n", qUtf8Printable(machine.name),
                qUtf8Printable(adapter->descriptor().name),
                static_cast<int>(machine.states.size()), static_cast<int>(machine.transitions.size()));

    const QVector<app::Problem> problems = app::validate(machine);
    bool hasError = false;
    for (const app::Problem& problem : problems) {
        std::fprintf(stderr, "%s: %s (state=%llu transition=%llu)\n",
                      problem.severity == app::ProblemSeverity::Error ? "ERROR" : "WARNING",
                      qUtf8Printable(problem.text), static_cast<unsigned long long>(problem.stateId),
                      static_cast<unsigned long long>(problem.transitionId));
        hasError = hasError || problem.severity == app::ProblemSeverity::Error;
    }
    if (hasError) {
        std::fprintf(stderr, "FAIL: the imported machine failed validate() -- nothing generated\n");
        return 1;
    }

    QString error;
    if (!savePath.isEmpty()) {
        if (!QDir().mkpath(QFileInfo(savePath).absolutePath())) {
            std::fprintf(stderr, "FAIL: could not create parent directory for %s\n", qUtf8Printable(savePath));
            return 1;
        }
        if (!app::saveMachine(machine, savePath, &error)) {
            std::fprintf(stderr, "FAIL: saveMachine failed: %s\n", qUtf8Printable(error));
            return 1;
        }
        std::printf("wrote %s (project)\n", qUtf8Printable(savePath));
    }

    int filesWritten = 0;
    int stubsWritten = 0;
    if (!outputDir.isEmpty()) {
        const QVector<app::GeneratedFile> files = app::generate(machine, QStringLiteral("app::generated"));
        if (!app::writeGeneratedFiles(outputDir, files, &error)) {
            std::fprintf(stderr, "FAIL: writeGeneratedFiles failed: %s\n", qUtf8Printable(error));
            return 1;
        }
        for (const app::GeneratedFile& file : files) {
            std::printf("wrote %s\n", qUtf8Printable(outputDir + QStringLiteral("/") + file.relativePath));
        }
        filesWritten = static_cast<int>(files.size());

        const QVector<app::GeneratedFile> stubs = app::generateDomainStubs(machine, QStringLiteral("app::generated"));
        if (!app::writeDomainStubsIfAbsent(outputDir, stubs, &stubsWritten, &error)) {
            std::fprintf(stderr, "FAIL: writeDomainStubsIfAbsent failed: %s\n", qUtf8Printable(error));
            return 1;
        }
    }

    // No QGuiApplication, so no fonts to lay out a coordinate-free machine:
    // the saved file keeps every state at the origin and the GUI lays it out
    // on first open.
    if (app::machineHasNoGeometry(machine)) {
        std::printf(
            "note: imported machine carries no geometry -- it will be laid out when first opened in the GUI\n");
    }

    std::printf("PASS: state-designer import (%s -> validate -> %d generated file(s), %d domain stub(s) "
                 "written)\n",
                 qUtf8Printable(machine.name), filesWritten, stubsWritten);
    return 0;
}

int runImportXState(const QString& inputPath, const QString& outputDir, const QString& savePath) {
    return runImportMachine(QStringLiteral("xstate-v5"), inputPath, outputDir, savePath);
}

// Loads a saved .sdm (app::loadMachine, not the .sdp project manifest) and
// regenerates ("--project <file.sdm> --generate <dir>"). generate() only, no
// generateDomainStubs(): domain/ stubs belong to the developer once written.
int runProjectGenerate(const QString& machinePath, const QString& outputDir) {
    app::Machine machine;
    QString error;
    if (!app::loadMachine(machinePath, &machine, &error)) {
        std::fprintf(stderr, "FAIL: --project could not read %s: %s\n", qUtf8Printable(machinePath),
                     qUtf8Printable(error));
        return 1;
    }
    std::printf("loaded %s: %d state(s), %d transition(s)\n", qUtf8Printable(machine.name),
                static_cast<int>(machine.states.size()), static_cast<int>(machine.transitions.size()));

    const QVector<app::Problem> problems = app::validate(machine);
    bool hasError = false;
    for (const app::Problem& problem : problems) {
        std::fprintf(stderr, "%s: %s (state=%llu transition=%llu)\n",
                      problem.severity == app::ProblemSeverity::Error ? "ERROR" : "WARNING",
                      qUtf8Printable(problem.text), static_cast<unsigned long long>(problem.stateId),
                      static_cast<unsigned long long>(problem.transitionId));
        hasError = hasError || problem.severity == app::ProblemSeverity::Error;
    }
    if (hasError) {
        std::fprintf(stderr, "FAIL: the loaded machine failed validate() -- nothing generated\n");
        return 1;
    }
    const QVector<app::GeneratedFile> files = app::generate(machine, QStringLiteral("app::generated"));
    if (!app::writeGeneratedFiles(outputDir, files, &error)) {
        std::fprintf(stderr, "FAIL: writeGeneratedFiles failed: %s\n", qUtf8Printable(error));
        return 1;
    }
    for (const app::GeneratedFile& file : files) {
        std::printf("wrote %s\n", qUtf8Printable(outputDir + QStringLiteral("/") + file.relativePath));
    }

    std::printf("PASS: state-designer project-generate (%s -> validate -> %d generated file(s) written)\n",
                 qUtf8Printable(machine.name), static_cast<int>(files.size()));
    return 0;
}

// CI drift gate: loads `machinePath`, regenerates into a scratch dir (never the
// committed tree), and byte-compares the file set app::generate() reports
// against `committedDir`, top-level only, so a committed domain/ subdirectory
// (developer-owned stubs) is not compared. Any missing/extra/differing name is
// a FAIL, listed by name; exit nonzero on any mismatch.
int runMachineDriftCheck(const QString& machinePath, const QString& committedDir) {
    app::Machine machine;
    QString error;
    if (!app::loadMachine(machinePath, &machine, &error)) {
        std::fprintf(stderr, "FAIL: machine-drift-check could not read %s: %s\n", qUtf8Printable(machinePath),
                     qUtf8Printable(error));
        return 1;
    }

    const QVector<app::Problem> problems = app::validate(machine);
    bool hasError = false;
    for (const app::Problem& problem : problems) {
        hasError = hasError || problem.severity == app::ProblemSeverity::Error;
    }
    if (hasError) {
        std::fprintf(stderr, "FAIL: machine-drift-check: %s failed validate()\n", qUtf8Printable(machinePath));
        return 1;
    }

    // Fresh scratch dir every run; no write-once state lives here.
    const QString scratchDir =
        QStringLiteral("temp/code/machine-drift-check/") + app::sanitizeSnakeCase(machine.name);
    QDir(scratchDir).removeRecursively();
    if (!QDir().mkpath(scratchDir)) {
        std::fprintf(stderr, "FAIL: machine-drift-check could not create scratch dir %s\n",
                     qUtf8Printable(scratchDir));
        return 1;
    }

    const QVector<app::GeneratedFile> files = app::generate(machine, QStringLiteral("app::generated"));
    if (!app::writeGeneratedFiles(scratchDir, files, &error)) {
        std::fprintf(stderr, "FAIL: machine-drift-check writeGeneratedFiles failed: %s\n", qUtf8Printable(error));
        return 1;
    }

    QStringList missing;
    QStringList differing;
    QStringList expectedNames;
    for (const app::GeneratedFile& file : files) {
        expectedNames.push_back(file.relativePath);
        QFile committedFile(committedDir + QStringLiteral("/") + file.relativePath);
        if (!committedFile.open(QIODevice::ReadOnly)) {
            missing.push_back(file.relativePath);
            continue;
        }
        const QByteArray committedBytes = committedFile.readAll();
        QFile scratchFile(scratchDir + QStringLiteral("/") + file.relativePath);
        // This path was just written, so a failed open is itself a mismatch.
        if (!scratchFile.open(QIODevice::ReadOnly) || committedBytes != scratchFile.readAll()) {
            differing.push_back(file.relativePath);
        }
    }
    QStringList extra;
    // The committed dir may host several machines' generated families side by
    // side; staleness is judged only within this machine's file-name prefix.
    const QString filePrefix = app::sanitizeSnakeCase(machine.name) + QStringLiteral("_");
    const QFileInfoList committedEntries = QDir(committedDir).entryInfoList(QDir::Files);
    for (const QFileInfo& info : committedEntries) {
        if (!info.fileName().startsWith(filePrefix)) {
            continue;
        }
        if (!expectedNames.contains(info.fileName())) {
            extra.push_back(info.fileName());
        }
    }

    if (!missing.isEmpty() || !differing.isEmpty() || !extra.isEmpty()) {
        for (const QString& name : missing) {
            std::fprintf(stderr, "FAIL: missing from %s: %s\n", qUtf8Printable(committedDir), qUtf8Printable(name));
        }
        for (const QString& name : extra) {
            std::fprintf(stderr, "FAIL: extra in %s (not produced by generate()): %s\n", qUtf8Printable(committedDir),
                         qUtf8Printable(name));
        }
        for (const QString& name : differing) {
            std::fprintf(stderr, "FAIL: differs from %s: %s\n", qUtf8Printable(committedDir), qUtf8Printable(name));
        }
        std::fprintf(stderr,
                     "FAIL: state-designer machine-drift-check (%d missing, %d extra, %d differing against %s)\n",
                     static_cast<int>(missing.size()), static_cast<int>(extra.size()),
                     static_cast<int>(differing.size()), qUtf8Printable(committedDir));
        return 1;
    }

    std::printf("PASS: state-designer machine-drift-check (%s -> %d generated file(s) byte-identical to %s)\n",
                qUtf8Printable(machine.name), static_cast<int>(files.size()), qUtf8Printable(committedDir));
    return 0;
}

// Migrate a native .sdm to the current format by load -> save, no validate
// gate (a resave must not refuse a machine the loader accepts). The version
// numbers are read off the raw files for the report only; 0 = unreadable.
int runResaveMachine(const QString& inputPath, const QString& outputPath) {
    const auto rawFormatVersion = [](const QString& path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return 0;
        }
        return QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("formatVersion")).toInt();
    };
    const int oldFormatVersion = rawFormatVersion(inputPath);

    app::Machine machine;
    QString error;
    if (!app::loadMachine(inputPath, &machine, &error)) {
        std::fprintf(stderr, "FAIL: resave-machine could not read %s: %s\n", qUtf8Printable(inputPath),
                     qUtf8Printable(error));
        return 1;
    }
    if (!app::saveMachine(machine, outputPath, &error)) {
        std::fprintf(stderr, "FAIL: resave-machine could not write %s: %s\n", qUtf8Printable(outputPath),
                     qUtf8Printable(error));
        return 1;
    }
    const int newFormatVersion = rawFormatVersion(outputPath);

    std::printf("resaved %s: formatVersion %d -> %d (%s -> %s)\n",
                qUtf8Printable(machine.name), oldFormatVersion, newFormatVersion,
                qUtf8Printable(inputPath), qUtf8Printable(outputPath));
    return 0;
}

// ---- --machine-doc-check helpers (doc-table <-> .sdm cross-check) --------
// The doc's transition table is canonical. Its markers derive from the
// machine's NAME as a kebab slug: "Canvas Interaction" ->
// machine:canvas-interaction:table:begin, "Note Editor" ->
// machine:note-editor:table:begin.
QString docTableMarkerSlug(const QString& machineName) {
    QString slug = machineName.toLower();
    slug.replace(QLatin1Char(' '), QLatin1Char('-'));
    return slug;
}

// One data row of the doc table: cells trimmed, em-dash cells (no payload / no
// guard / no action / TARGETLESS) normalized to "" to match Transition's
// empty-string convention and its 0 = TARGETLESS sentinel. payload is "" for
// every row of a legacy 6-column table.
struct DocTableRow {
    QString from;
    QString event;
    QString payload;
    QString guard;
    QString action;
    QString to;
};

// Locates the <beginMarker>...-->...<endMarker> block in `text` and returns
// its pipe ('|'-prefixed, trimmed) lines in physical order. Returns false and
// fills *error on a missing/duplicated marker or an unterminated begin comment.
static bool extractMarkedPipeLines(const QString& text, const QString& beginMarker, const QString& endMarker,
                                    QStringList* pipeLines, QString* error) {
    const qsizetype beginCount = text.count(beginMarker);
    const qsizetype endCount = text.count(endMarker);
    if (beginCount == 0 || endCount == 0) {
        if (error) {
            *error = QStringLiteral("missing marker (begin=%1, end=%2)").arg(beginCount).arg(endCount);
        }
        return false;
    }
    if (beginCount > 1 || endCount > 1) {
        if (error) {
            *error = QStringLiteral("duplicated marker (begin=%1, end=%2)").arg(beginCount).arg(endCount);
        }
        return false;
    }

    const qsizetype beginIndex = text.indexOf(beginMarker);
    const qsizetype commentClose = text.indexOf(QStringLiteral("-->"), beginIndex);
    if (commentClose < 0) {
        if (error) {
            *error = QStringLiteral("begin marker's comment is never closed with -->");
        }
        return false;
    }
    const qsizetype contentStart = commentClose + 3;
    const qsizetype endIndex = text.indexOf(endMarker, contentStart);
    if (endIndex < contentStart) {
        if (error) {
            *error = QStringLiteral("end marker not found after the begin marker's comment");
        }
        return false;
    }

    const QString content = text.mid(contentStart, endIndex - contentStart);
    for (const QString& rawLine : content.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(QLatin1Char('|'))) {
            pipeLines->push_back(line);
        }
    }
    return true;
}

// Reads `docPath` and parses the one marked table into `*rows`, in document
// order. Returns false and fills *error on an unreadable file, a
// missing/duplicated marker, an unterminated begin comment, or a row whose
// cell count differs from the header's. 6 columns is the legacy table
// (#, From, Event, Guard, Action, To); 7 inserts Payload after Event.
static bool parseDocTable(const QString& docPath, const QString& beginMarker, const QString& endMarker,
                          QVector<DocTableRow>* rows, QString* error) {
    QFile file(docPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    const QString text = QString::fromUtf8(file.readAll());

    QStringList pipeLines;
    if (!extractMarkedPipeLines(text, beginMarker, endMarker, &pipeLines, error)) {
        return false;
    }
    // Markdown preamble (header row, then `---` alignment row) is skipped by
    // position only; the header's cell count fixes the table shape below.
    if (pipeLines.size() < 2) {
        if (error) {
            *error = QStringLiteral("no header/separator rows found between the markers");
        }
        return false;
    }

    QStringList headerCells = pipeLines[0].split(QLatin1Char('|'));
    if (!headerCells.isEmpty() && headerCells.first().trimmed().isEmpty()) {
        headerCells.removeFirst();
    }
    if (!headerCells.isEmpty() && headerCells.last().trimmed().isEmpty()) {
        headerCells.removeLast();
    }
    const qsizetype columnCount = headerCells.size();
    if (columnCount != 6 && columnCount != 7) {
        if (error) {
            *error = QStringLiteral("header row has %1 column(s), expected 6 (#, From, Event, Guard, Action, To) "
                                     "or 7 (#, From, Event, Payload, Guard, Action, To): %2")
                         .arg(columnCount)
                         .arg(pipeLines[0]);
        }
        return false;
    }

    for (qsizetype i = 2; i < pipeLines.size(); ++i) {
        QStringList cells = pipeLines[i].split(QLatin1Char('|'));
        // "| a | b | c |" splits into empty leading/trailing strings plus the
        // real cells; drop both. A row without outer pipes fails the count check.
        if (!cells.isEmpty() && cells.first().trimmed().isEmpty()) {
            cells.removeFirst();
        }
        if (!cells.isEmpty() && cells.last().trimmed().isEmpty()) {
            cells.removeLast();
        }
        for (QString& cell : cells) {
            cell = cell.trimmed();
        }
        if (cells.size() != columnCount) {
            if (error) {
                *error = QStringLiteral("row %1 has %2 cell(s), expected %3 (matching the header row): %4")
                             .arg(rows->size() + 1)
                             .arg(cells.size())
                             .arg(columnCount)
                             .arg(pipeLines[i]);
            }
            return false;
        }
        // cells[0] is "#": presentational, physical order is what counts.
        const auto normalize = [](const QString& cell) {
            // U+2014 EM DASH marks "no payload"/"no guard"/"no action"/
            // TARGETLESS. Unicode-escaped so it never depends on source encoding.
            return cell == QString(QChar(0x2014)) ? QString() : cell;
        };
        DocTableRow row;
        row.from = cells[1];
        row.event = cells[2];
        if (columnCount == 7) {
            row.payload = normalize(cells[3]);
            row.guard = normalize(cells[4]);
            row.action = normalize(cells[5]);
            row.to = normalize(cells[6]);
        } else {
            // Legacy 6-column table: no Payload column, row.payload stays "".
            row.guard = normalize(cells[3]);
            row.action = normalize(cells[4]);
            row.to = normalize(cells[5]);
        }
        rows->push_back(row);
    }
    return true;
}

// One data row of the doc's optional context table: Variable/Type/Initial,
// cells trimmed, Initial's em-dash normalized to "". Remaining cells (Meaning)
// are ignored.
struct DocContextRow {
    QString name;
    QString type;
    QString initialValue;
};

// Parses the optional `<slug>:context:begin`/`:end` block into `*rows`,
// physical order preserved. *present reports whether the markers were found;
// when false, *rows is left untouched and true is returned (absence is not a
// parse error). Returns false and fills *error on duplicated markers, an
// incomplete begin/end pair, an unterminated begin comment, or a row with
// fewer than 3 cells (Variable, Type, Initial).
static bool parseDocContextTable(const QString& docPath, const QString& beginMarker, const QString& endMarker,
                                  bool* present, QVector<DocContextRow>* rows, QString* error) {
    QFile file(docPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    const QString text = QString::fromUtf8(file.readAll());

    if (text.count(beginMarker) == 0 && text.count(endMarker) == 0) {
        *present = false;
        return true;
    }
    *present = true;

    QStringList pipeLines;
    if (!extractMarkedPipeLines(text, beginMarker, endMarker, &pipeLines, error)) {
        return false;
    }
    // Markdown preamble (header + `---` row) skipped by position, as in parseDocTable.
    if (pipeLines.size() < 2) {
        if (error) {
            *error = QStringLiteral("no header/separator rows found between the context markers");
        }
        return false;
    }
    for (qsizetype i = 2; i < pipeLines.size(); ++i) {
        QStringList cells = pipeLines[i].split(QLatin1Char('|'));
        if (!cells.isEmpty() && cells.first().trimmed().isEmpty()) {
            cells.removeFirst();
        }
        if (!cells.isEmpty() && cells.last().trimmed().isEmpty()) {
            cells.removeLast();
        }
        for (QString& cell : cells) {
            cell = cell.trimmed();
        }
        if (cells.size() < 3) {
            if (error) {
                *error = QStringLiteral("context row %1 has %2 cell(s), expected at least 3 (Variable, Type, "
                                         "Initial): %3")
                             .arg(rows->size() + 1)
                             .arg(cells.size())
                             .arg(pipeLines[i]);
            }
            return false;
        }
        DocContextRow row;
        row.name = cells[0];
        row.type = cells[1];
        // U+2014 EM DASH means "no value", as in the transition table.
        row.initialValue = cells[2] == QString(QChar(0x2014)) ? QString() : cells[2];
        rows->push_back(row);
    }
    return true;
}

// One (payload, guard, action, to) tuple inside a (From, Event) group, in
// document order. All fields are empty-normalized on both sides (to also maps
// "#machine" for the root self-transition and "" for TARGETLESS), so
// comparison is plain memberwise ==.
struct DocCheckGroupItem {
    QString payload;
    QString guard;
    QString action;
    QString to;
    bool operator==(const DocCheckGroupItem&) const = default;
};
using DocCheckGroupKey = QPair<QString, QString>;  // (From, Event)

// Groups table rows by (From, Event), preserving first-seen key order in
// `*order` and each group's physical row order in `*groups` (v5 evaluation
// order: top-down within a group; ordering across groups is not compared).
static void groupDocRows(const QVector<DocTableRow>& rows, QVector<DocCheckGroupKey>* order,
                          QHash<DocCheckGroupKey, QVector<DocCheckGroupItem>>* groups) {
    for (const DocTableRow& row : rows) {
        const DocCheckGroupKey key(row.from, row.event);
        if (!groups->contains(key)) {
            order->push_back(key);
        }
        (*groups)[key].push_back(DocCheckGroupItem{row.payload, row.guard, row.action, row.to});
    }
}

// Same grouping over a loaded Machine's transitions, by (from-state-name,
// event), in the .sdm array's own order. from == 0 is the ROOT sentinel (the
// doc's "#machine"). to == 0 is TARGETLESS (""), unless this is the root
// self-transition (from == 0 && machineSelf), which targets #machine. An id
// absent from `machine.states` falls back to `<state#id>`.
static void groupMachineTransitions(const app::Machine& machine, QVector<DocCheckGroupKey>* order,
                                     QHash<DocCheckGroupKey, QVector<DocCheckGroupItem>>* groups) {
    QHash<quint64, QString> stateNames;
    for (const app::State& state : machine.states) {
        stateNames.insert(state.id, state.name);
    }
    const auto stateNameOf = [&stateNames](quint64 id) {
        const auto it = stateNames.constFind(id);
        return it == stateNames.constEnd() ? QStringLiteral("<state#%1>").arg(id) : it.value();
    };
    for (const app::Transition& transition : machine.transitions) {
        const QString from = transition.from == 0 ? QStringLiteral("#machine") : stateNameOf(transition.from);
        const DocCheckGroupKey key(from, transition.event);
        if (!groups->contains(key)) {
            order->push_back(key);
        }
        QString to;
        if (transition.to == 0) {
            to = (transition.from == 0 && transition.machineSelf) ? QStringLiteral("#machine") : QString();
        } else {
            to = stateNameOf(transition.to);
        }
        (*groups)[key].push_back(DocCheckGroupItem{transition.payloadType, transition.guard, transition.action, to});
    }
}

// The doc-table<->.sdm cross-check: parses `docPath`'s marked transition table
// (6 legacy columns, or 7 with Payload) and its optional context table, loads
// `machinePath`, and reports every state-set, per-group transition, and
// context-variable mismatch; exit nonzero on any. Not covered: state
// kinds/delays/descriptions/tags, the machine-level initial state, and
// struct/custom field shapes (only a variable's declared type name is compared).
int runMachineDocCheck(const QString& docPath, const QString& machinePath) {
    // Machine first: its name derives the doc's marker slug.
    app::Machine machine;
    QString error;
    if (!app::loadMachine(machinePath, &machine, &error)) {
        std::fprintf(stderr, "FAIL: machine-doc-check could not read %s: %s\n", qUtf8Printable(machinePath),
                     qUtf8Printable(error));
        return 1;
    }
    const QString slug = docTableMarkerSlug(machine.name);
    const QString beginMarker = QStringLiteral("<!-- machine:") + slug + QStringLiteral(":table:begin");
    const QString endMarker = QStringLiteral("<!-- machine:") + slug + QStringLiteral(":table:end -->");

    QVector<DocTableRow> docRows;
    if (!parseDocTable(docPath, beginMarker, endMarker, &docRows, &error)) {
        std::fprintf(stderr, "FAIL: machine-doc-check could not parse %s: %s\n", qUtf8Printable(docPath),
                     qUtf8Printable(error));
        return 1;
    }

    bool ok = true;

    // State-name set: the table's From-union-To vs the machine's names. The
    // doc is canonical: "missing" = doc has it, machine doesn't; "extra" = the
    // reverse. "#machine" and "" (TARGETLESS) are not state names.
    QSet<QString> docStates;
    for (const DocTableRow& row : docRows) {
        docStates.insert(row.from);
        docStates.insert(row.to);
    }
    docStates.remove(QStringLiteral("#machine"));
    docStates.remove(QString());
    QSet<QString> machineStates;
    for (const app::State& state : machine.states) {
        machineStates.insert(state.name);
    }
    QSet<QString> missingStateSet = docStates;
    missingStateSet.subtract(machineStates);
    QSet<QString> extraStateSet = machineStates;
    extraStateSet.subtract(docStates);
    QStringList missingStates(missingStateSet.begin(), missingStateSet.end());
    missingStates.sort();
    QStringList extraStates(extraStateSet.begin(), extraStateSet.end());
    extraStates.sort();
    if (!missingStates.isEmpty()) {
        ok = false;
        std::fprintf(stderr, "FAIL: machine-doc-check state(s) in doc table but not in machine: %s\n",
                     qUtf8Printable(missingStates.join(QStringLiteral(", "))));
    }
    if (!extraStates.isEmpty()) {
        ok = false;
        std::fprintf(stderr, "FAIL: machine-doc-check state(s) in machine but not in doc table: %s\n",
                     qUtf8Printable(extraStates.join(QStringLiteral(", "))));
    }

    // (From, Event) groups, order semantics matching v5 evaluation.
    QVector<DocCheckGroupKey> docOrder;
    QHash<DocCheckGroupKey, QVector<DocCheckGroupItem>> docGroups;
    groupDocRows(docRows, &docOrder, &docGroups);
    QVector<DocCheckGroupKey> machineOrder;
    QHash<DocCheckGroupKey, QVector<DocCheckGroupItem>> machineGroups;
    groupMachineTransitions(machine, &machineOrder, &machineGroups);

    int missingGroups = 0;
    int extraGroups = 0;
    int mismatchedGroups = 0;
    for (const DocCheckGroupKey& key : docOrder) {
        if (!machineGroups.contains(key)) {
            ok = false;
            ++missingGroups;
            std::fprintf(stderr,
                         "FAIL: machine-doc-check group (%s, %s) is in the doc table but has no machine "
                         "transition\n",
                         qUtf8Printable(key.first), qUtf8Printable(key.second));
        }
    }
    for (const DocCheckGroupKey& key : machineOrder) {
        if (!docGroups.contains(key)) {
            ok = false;
            ++extraGroups;
            std::fprintf(stderr,
                         "FAIL: machine-doc-check group (%s, %s) has machine transition(s) undocumented in the "
                         "doc table\n",
                         qUtf8Printable(key.first), qUtf8Printable(key.second));
        }
    }
    for (const DocCheckGroupKey& key : docOrder) {
        if (!machineGroups.contains(key)) {
            continue;  // already reported above as missingGroups
        }
        const QVector<DocCheckGroupItem>& docSeq = docGroups.value(key);
        const QVector<DocCheckGroupItem>& machineSeq = machineGroups.value(key);
        const int docLen = static_cast<int>(docSeq.size());
        const int machineLen = static_cast<int>(machineSeq.size());
        const int maxLen = docLen > machineLen ? docLen : machineLen;
        QStringList positionDiffs;
        for (int i = 0; i < maxLen; ++i) {
            if (i >= docLen) {
                positionDiffs.push_back(
                    QStringLiteral("position %1: machine has payload=\"%2\" guard=\"%3\" action=\"%4\" to=\"%5\", "
                                   "doc has no row here")
                        .arg(i)
                        .arg(machineSeq[i].payload)
                        .arg(machineSeq[i].guard)
                        .arg(machineSeq[i].action)
                        .arg(machineSeq[i].to));
            } else if (i >= machineLen) {
                positionDiffs.push_back(
                    QStringLiteral("position %1: doc has payload=\"%2\" guard=\"%3\" action=\"%4\" to=\"%5\", "
                                   "machine has no transition here")
                        .arg(i)
                        .arg(docSeq[i].payload)
                        .arg(docSeq[i].guard)
                        .arg(docSeq[i].action)
                        .arg(docSeq[i].to));
            } else if (!(docSeq[i] == machineSeq[i])) {
                positionDiffs.push_back(
                    QStringLiteral("position %1: doc payload=\"%2\" guard=\"%3\" action=\"%4\" to=\"%5\" vs machine "
                                   "payload=\"%6\" guard=\"%7\" action=\"%8\" to=\"%9\"")
                        .arg(i)
                        .arg(docSeq[i].payload)
                        .arg(docSeq[i].guard)
                        .arg(docSeq[i].action)
                        .arg(docSeq[i].to)
                        .arg(machineSeq[i].payload)
                        .arg(machineSeq[i].guard)
                        .arg(machineSeq[i].action)
                        .arg(machineSeq[i].to));
            }
        }
        if (!positionDiffs.isEmpty()) {
            ok = false;
            ++mismatchedGroups;
            std::fprintf(stderr,
                         "FAIL: machine-doc-check group (%s, %s) sequence mismatch (doc has %d row(s), machine "
                         "has %d transition(s)):\n",
                         qUtf8Printable(key.first), qUtf8Printable(key.second), docLen, machineLen);
            for (const QString& diff : positionDiffs) {
                std::fprintf(stderr, "FAIL: machine-doc-check   %s\n", qUtf8Printable(diff));
            }
        }
    }

    // Context table (optional): absent markers with an empty Machine::context
    // means nothing to check, not a failure.
    const QString contextBeginMarker = QStringLiteral("<!-- machine:") + slug + QStringLiteral(":context:begin");
    const QString contextEndMarker = QStringLiteral("<!-- machine:") + slug + QStringLiteral(":context:end -->");
    bool contextTablePresent = false;
    QVector<DocContextRow> contextRows;
    if (!parseDocContextTable(docPath, contextBeginMarker, contextEndMarker, &contextTablePresent, &contextRows,
                               &error)) {
        std::fprintf(stderr, "FAIL: machine-doc-check could not parse %s context table: %s\n",
                     qUtf8Printable(docPath), qUtf8Printable(error));
        return 1;
    }

    int contextChecked = 0;
    if (!contextTablePresent) {
        if (!machine.context.isEmpty()) {
            ok = false;
            std::fprintf(stderr,
                         "FAIL: machine-doc-check machine has %d context variable(s) but %s has no context "
                         "table\n",
                         static_cast<int>(machine.context.size()), qUtf8Printable(docPath));
        }
    } else {
        QHash<QString, DocContextRow> docContextByName;
        for (const DocContextRow& row : contextRows) {
            docContextByName.insert(row.name, row);
        }
        QSet<QString> docContextNames;
        for (const DocContextRow& row : contextRows) {
            docContextNames.insert(row.name);
        }
        QSet<QString> machineContextNames;
        for (const app::ContextVariable& variable : machine.context) {
            machineContextNames.insert(variable.name);
        }
        QSet<QString> missingContextSet = docContextNames;
        missingContextSet.subtract(machineContextNames);
        QSet<QString> extraContextSet = machineContextNames;
        extraContextSet.subtract(docContextNames);
        QStringList missingContext(missingContextSet.begin(), missingContextSet.end());
        missingContext.sort();
        QStringList extraContext(extraContextSet.begin(), extraContextSet.end());
        extraContext.sort();
        if (!missingContext.isEmpty()) {
            ok = false;
            std::fprintf(stderr, "FAIL: machine-doc-check context variable(s) in doc table but not in machine: %s\n",
                         qUtf8Printable(missingContext.join(QStringLiteral(", "))));
        }
        if (!extraContext.isEmpty()) {
            ok = false;
            std::fprintf(stderr, "FAIL: machine-doc-check context variable(s) in machine but not in doc table: %s\n",
                         qUtf8Printable(extraContext.join(QStringLiteral(", "))));
        }

        // Per shared name: Type (vs contextTypeToString(type), or
        // customTypeName when non-empty) and Initial (exact) must match.
        for (const app::ContextVariable& variable : machine.context) {
            const auto docIt = docContextByName.constFind(variable.name);
            if (docIt == docContextByName.constEnd()) {
                continue;  // already reported above as missing/extra
            }
            ++contextChecked;
            const QString expectedType = !variable.customTypeName.isEmpty() ? variable.customTypeName
                                                                              : app::contextTypeToString(variable.type);
            if (docIt.value().type != expectedType) {
                ok = false;
                std::fprintf(stderr,
                             "FAIL: machine-doc-check context variable %s: doc Type=\"%s\" vs machine Type=\"%s\"\n",
                             qUtf8Printable(variable.name), qUtf8Printable(docIt.value().type),
                             qUtf8Printable(expectedType));
            }
            if (docIt.value().initialValue != variable.initialValue) {
                ok = false;
                std::fprintf(stderr,
                             "FAIL: machine-doc-check context variable %s: doc Initial=\"%s\" vs machine "
                             "Initial=\"%s\"\n",
                             qUtf8Printable(variable.name), qUtf8Printable(docIt.value().initialValue),
                             qUtf8Printable(variable.initialValue));
            }
        }
    }

    if (!ok) {
        std::fprintf(stderr,
                     "FAIL: state-designer machine-doc-check (%d missing group(s), %d extra group(s), %d "
                     "mismatched group(s) between %s and %s)\n",
                     missingGroups, extraGroups, mismatchedGroups, qUtf8Printable(docPath),
                     qUtf8Printable(machinePath));
        return 1;
    }

    std::printf(
        "PASS: state-designer machine-doc-check (%s -> %d table rows == %d machine transitions, %d context "
        "variable(s) checked, doc is canonical)\n",
        qUtf8Printable(machine.name), static_cast<int>(docRows.size()), static_cast<int>(machine.transitions.size()),
        contextChecked);
    return 0;
}

