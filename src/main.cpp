// Entry point: flag parsing, the file-sink Qt message handler, and the QApplication/
// MainWindow bootstrap. The --smoke, --gui-probe and CLI mode runners live under harness/.
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QStandardPaths>
#include <QStringList>

#include <cstdio>
#include <cstring>

#include "harness/harness.h"
#include "infra/crash_handler.h"
#include "infra/recent_projects.h"
#include "view/shell/icons.h"
#include "view/shell/main_window.h"
#include "view/shell/theme.h"

// The system temp directory (%TEMP% on Windows, /tmp elsewhere): it always exists and sits
// outside Windows Controlled Folder Access, which blocks unsigned-exe writes under Documents.
static QString tempDir() {
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation);
}

// Shared with the crash handler so a trace lands in the same log as the run's last qDebug lines.
static QString logPath() {
    return tempDir() + QStringLiteral("/state-designer.log");
}

// Appends every Qt message (categories like "sd.undo" included) to the log, timestamped,
// and still echoes to stderr.
static void fileSinkMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    static QFile logFile(logPath());
    if (!logFile.isOpen()) {
        logFile.open(QIODevice::Append | QIODevice::Text);
    }
    const char* level = type == QtDebugMsg      ? "DBG"
                        : type == QtInfoMsg     ? "INF"
                        : type == QtWarningMsg  ? "WRN"
                        : type == QtCriticalMsg ? "CRT"
                                                 : "FTL";
    const QString line = QStringLiteral("%1 [%2] %3: %4\n")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                                  QString::fromLatin1(level),
                                  QString::fromLatin1(context.category != nullptr ? context.category : "default"),
                                  message);
    if (logFile.isOpen()) {
        logFile.write(line.toUtf8());
        logFile.flush();
    }
    std::fprintf(stderr, "%s", line.toUtf8().constData());
}

int main(int argc, char** argv) {
#if !defined(SD_ENABLE_LOGGING) || (SD_ENABLE_LOGGING != 0)
    qInstallMessageHandler(fileSinkMessageHandler);
#endif
    // Installed first, so a crash in flag parsing, QApplication construction or any mode is traced.
    app::installCrashHandler(logPath());
    // Flags are parsed before QApplication exists. --smoke returns immediately without
    // constructing one; --gui-probe needs a QApplication, so it is recorded as a flag and
    // acted on below.
    bool guiProbe = false;
    bool guiProbeVisible = false;
    QStringList probeFilter;
    bool noRestore = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) {
            return runSmoke();
        }
        if (std::strcmp(argv[i], "--gui-probe") == 0) {
            guiProbe = true;
        }
        if (std::strcmp(argv[i], "--no-restore") == 0) {
            noRestore = true;
        }
        // Crash handler self-test: dereferences null through three named frames so the
        // log shows whether a trace resolves to real names and lines.
        if (std::strcmp(argv[i], "--crash-selftest") == 0) {
            app::crashSelfTest();
        }
        if (std::strcmp(argv[i], "--crash-selftest-stackoverflow") == 0) {
            app::crashSelfTestStackOverflow();
        }
        // --gui-probe runs offscreen by default; --gui-probe-visible shows the real window
        // for manual inspection and implies --gui-probe.
        if (std::strcmp(argv[i], "--gui-probe-visible") == 0) {
            guiProbe = true;
            guiProbeVisible = true;
        }
        // --gui-probe-only runs just the scenarios whose function name contains a
        // comma-separated term (case-insensitive) and implies --gui-probe. Skipping changes
        // later scenarios' baselines, so only the unfiltered run is a verification gate.
        if (std::strcmp(argv[i], "--gui-probe-only") == 0) {
            if (i + 1 >= argc || std::strncmp(argv[i + 1], "--", 2) == 0) {
                std::fprintf(stderr, "usage: state-designer --gui-probe-only <name>[,<name>...]\n");
                return 2;
            }
            guiProbe = true;
            probeFilter = QString::fromLocal8Bit(argv[++i]).split(QLatin1Char(','), Qt::SkipEmptyParts);
        }
        if (std::strcmp(argv[i], "--codegen-check") == 0) {
            QString outDir = tempDir() + QStringLiteral("/state-designer-codegen-check");
            if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) {
                outDir = QString::fromLocal8Bit(argv[++i]);
            }
            return runCodegenCheck(outDir);
        }
        if (std::strcmp(argv[i], "--export") == 0) {
            if (i + 3 >= argc || std::strncmp(argv[i + 1], "--", 2) == 0 ||
                std::strncmp(argv[i + 2], "--", 2) == 0 || std::strncmp(argv[i + 3], "--", 2) == 0) {
                std::fprintf(stderr, "usage: state-designer --export <format> <in.sdm> <out-file>\n");
                return 2;
            }
            return runExportMachine(QString::fromLocal8Bit(argv[i + 1]),
                                    QString::fromLocal8Bit(argv[i + 2]),
                                    QString::fromLocal8Bit(argv[i + 3]));
        }
        if (std::strcmp(argv[i], "--import") == 0) {
            QString generateDir;
            QString savePath;
            for (int j = 1; j + 1 < argc; ++j) {
                if (std::strcmp(argv[j], "--generate") == 0 && std::strncmp(argv[j + 1], "--", 2) != 0) {
                    generateDir = QString::fromLocal8Bit(argv[j + 1]);
                }
                if (std::strcmp(argv[j], "--save-project") == 0 && std::strncmp(argv[j + 1], "--", 2) != 0) {
                    savePath = QString::fromLocal8Bit(argv[j + 1]);
                }
            }
            if (i + 2 >= argc || std::strncmp(argv[i + 1], "--", 2) == 0 || std::strncmp(argv[i + 2], "--", 2) == 0 ||
                (generateDir.isEmpty() && savePath.isEmpty())) {
                std::fprintf(stderr,
                              "usage: state-designer --import <format> <in-file> [--save-project <out.sdm>] "
                              "[--generate <dir>]\n");
                return 2;
            }
            return runImportMachine(QString::fromLocal8Bit(argv[i + 1]),
                                    QString::fromLocal8Bit(argv[i + 2]),
                                    generateDir, savePath);
        }
        if (std::strcmp(argv[i], "--export-xstate") == 0) {
            if (i + 2 >= argc || std::strncmp(argv[i + 1], "--", 2) == 0 || std::strncmp(argv[i + 2], "--", 2) == 0) {
                std::fprintf(stderr, "usage: state-designer --export-xstate <in.sdm> <out.json>\n");
                return 2;
            }
            return runExportXState(QString::fromLocal8Bit(argv[i + 1]), QString::fromLocal8Bit(argv[i + 2]));
        }
        if (std::strcmp(argv[i], "--import-xstate") == 0) {
            // --generate/--save-project may come before or after, so scan the whole argv.
            // At least one must be present, or the import has nothing to do with its result.
            QString generateDir;
            QString savePath;
            for (int j = 1; j + 1 < argc; ++j) {
                if (std::strcmp(argv[j], "--generate") == 0 && std::strncmp(argv[j + 1], "--", 2) != 0) {
                    generateDir = QString::fromLocal8Bit(argv[j + 1]);
                }
                if (std::strcmp(argv[j], "--save-project") == 0 && std::strncmp(argv[j + 1], "--", 2) != 0) {
                    savePath = QString::fromLocal8Bit(argv[j + 1]);
                }
            }
            if (i + 1 >= argc || std::strncmp(argv[i + 1], "--", 2) == 0 ||
                (generateDir.isEmpty() && savePath.isEmpty())) {
                std::fprintf(stderr,
                              "usage: state-designer --import-xstate <in.json> [--save-project <out.sdm>] "
                              "[--generate <dir>]\n");
                return 2;
            }
            return runImportXState(QString::fromLocal8Bit(argv[i + 1]), generateDir, savePath);
        }
        if (std::strcmp(argv[i], "--project") == 0) {
            // Loads a saved project and regenerates; --generate is mandatory.
            QString generateDir;
            for (int j = 1; j + 1 < argc; ++j) {
                if (std::strcmp(argv[j], "--generate") == 0 && std::strncmp(argv[j + 1], "--", 2) != 0) {
                    generateDir = QString::fromLocal8Bit(argv[j + 1]);
                }
            }
            if (i + 1 >= argc || std::strncmp(argv[i + 1], "--", 2) == 0 || generateDir.isEmpty()) {
                std::fprintf(stderr, "usage: state-designer --project <file.sdm> --generate <dir>\n");
                return 2;
            }
            return runProjectGenerate(QString::fromLocal8Bit(argv[i + 1]), generateDir);
        }
        if (std::strcmp(argv[i], "--machine-drift-check") == 0) {
            // Positional: <file.sdm> then <committed-dir>; nonzero exit on any mismatch.
            if (i + 2 >= argc || std::strncmp(argv[i + 1], "--", 2) == 0 || std::strncmp(argv[i + 2], "--", 2) == 0) {
                std::fprintf(stderr, "usage: state-designer --machine-drift-check <file.sdm> <committed-dir>\n");
                return 2;
            }
            return runMachineDriftCheck(QString::fromLocal8Bit(argv[i + 1]), QString::fromLocal8Bit(argv[i + 2]));
        }
        if (std::strcmp(argv[i], "--machine-doc-check") == 0) {
            // Positional: <doc.md> then <machine.sdm>; nonzero exit on any mismatch.
            if (i + 2 >= argc || std::strncmp(argv[i + 1], "--", 2) == 0 || std::strncmp(argv[i + 2], "--", 2) == 0) {
                std::fprintf(stderr, "usage: state-designer --machine-doc-check <doc.md> <machine.sdm>\n");
                return 2;
            }
            return runMachineDocCheck(QString::fromLocal8Bit(argv[i + 1]), QString::fromLocal8Bit(argv[i + 2]));
        }
        if (std::strcmp(argv[i], "--resave-machine") == 0) {
            // One required positional arg (input), one optional (output).
            // When omitted, output = input (in-place resave).
            if (i + 1 >= argc || std::strncmp(argv[i + 1], "--", 2) == 0) {
                std::fprintf(stderr, "usage: state-designer --resave-machine <in.sdm> [<out.sdm>]\n");
                return 2;
            }
            const QString inputPath = QString::fromLocal8Bit(argv[i + 1]);
            QString outputPath = inputPath;
            if (i + 2 < argc && std::strncmp(argv[i + 2], "--", 2) != 0) {
                outputPath = QString::fromLocal8Bit(argv[i + 2]);
            }
            return runResaveMachine(inputPath, outputPath);
        }
    }

    // Default --gui-probe to the offscreen platform. It is read at QApplication construction,
    // so it must be set before the constructor. An explicit QT_QPA_PLATFORM wins.
    if (guiProbe && !guiProbeVisible && !qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }

    QApplication application(argc, argv);
    // Brand mark for the title bar, taskbar and Alt+Tab.
    application.setWindowIcon(app::icons::appIcon());
    if (guiProbe && !guiProbeVisible) {
        // The offscreen platform registers no font families, so text metrics come out about
        // 2x too wide and break geometry assertions. Load Segoe UI (regular + bold) directly.
        const int regularId = QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeuib.ttf"));
        if (regularId >= 0) {
            const QStringList families = QFontDatabase::applicationFontFamilies(regularId);
            if (!families.isEmpty()) {
                application.setFont(QFont(families.first()));
            }
        }
    }

    // Applied before any widget exists, on the GUI and probe paths alike.
    app::theme::apply(application);

    // MainWindow is the whole composition root: it owns the kernels, agents and command
    // registrations.
    if (guiProbe) {
        // A probe must never touch the real recent-projects list: redirect it BEFORE
        // MainWindow constructs its RecentProjects. Anchored on the exe, so the probe
        // stays cwd-independent.
        const QString probeRecentPath = QDir::cleanPath(
            QCoreApplication::applicationDirPath() + QStringLiteral("/../temp/code/open-recent/recent-probe.json"));
        app::RecentProjects::setDefaultPathOverride(probeRecentPath);
        // Plant a non-empty lastActiveProject before MainWindow is constructed, proving that
        // --gui-probe never takes the restore path.
        {
            app::RecentProjects probeRecent(probeRecentPath);
            probeRecent.load();  // keep the file's existing entries -- setLastActiveProject() saves the whole file
            probeRecent.setLastActiveProject(QStringLiteral("C:/bogus/planted_probe_project.sdp"));
        }
    }

    app::MainWindow window;

    int exitCode = 0;
    if (guiProbe) {
        exitCode = runGuiProbe(window, probeFilter);
    } else {
        window.show();
        window.tryRestoreLastProject(noRestore);
        exitCode = application.exec();
    }
    return exitCode;
}
