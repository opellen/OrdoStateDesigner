#include "infra/crash_handler.h"

#include <QString>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>

#include <windows.h>
// dbghelp.h must follow windows.h -- it depends on its types.
#include <dbghelp.h>

namespace app {
namespace {

// Everything below runs in a dying process: no allocation, no Qt. The log path
// is copied into a static buffer at install time and the trace is written with
// raw Win32 file calls.
char g_logPath[MAX_PATH] = {0};
char g_exePath[MAX_PATH] = {0};
// The directory g_logPath lives in, split off once at install time; exe
// snapshots land next to the log.
char g_logDir[MAX_PATH] = {0};

void writeAll(HANDLE file, const char* text) {
    const DWORD length = static_cast<DWORD>(std::strlen(text));
    DWORD written = 0;
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, text, length, &written, nullptr);
    }
    std::fprintf(stderr, "%s", text);
}

HANDLE openLog() {
    if (g_logPath[0] == '\0') {
        return INVALID_HANDLE_VALUE;
    }
    HANDLE file = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    return file;
}

const char* exceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:
            return "STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:
            return "IN_PAGE_ERROR";
        case EXCEPTION_PRIV_INSTRUCTION:
            return "PRIV_INSTRUCTION";
        case EXCEPTION_BREAKPOINT:
            return "BREAKPOINT";
        default:
            return "UNKNOWN";
    }
}

// The exe's preferred ImageBase and its actual load address, captured at
// install time; a frame resolves as preferredBase + (pc - loadedBase).
//
// The preferred base must be read from the file on disk: Windows patches
// ImageBase in the mapped image to the load address, which would cancel the
// correction.
DWORD64 g_preferredImageBase = 0;
DWORD64 g_exeLoadedBase = 0;

DWORD64 readPreferredImageBaseFromDisk(const char* exePath) {
    HANDLE file = CreateFileA(exePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD64 base = 0;
    IMAGE_DOS_HEADER dos = {};
    DWORD read = 0;
    if (ReadFile(file, &dos, sizeof(dos), &read, nullptr) != FALSE && read == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE) {
        if (SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
            IMAGE_NT_HEADERS64 nt = {};
            if (ReadFile(file, &nt, sizeof(nt), &read, nullptr) != FALSE && read == sizeof(nt) &&
                nt.Signature == IMAGE_NT_SIGNATURE) {
                base = static_cast<DWORD64>(nt.OptionalHeader.ImageBase);
            }
        }
    }
    CloseHandle(file);
    return base;
}

// Frames collected by writeStackTrace()'s walk, read back afterward by
// writeSymbolizeCommands().
constexpr int kMaxOwnFrames = 64;
DWORD64 g_ownFrames[kMaxOwnFrames];
int g_ownFrameCount = 0;

// Non-own-exe frames grouped by module (one addr2line call per DLL). Fixed-size
// (8 modules x 64 frames) so the crash path never allocates.
constexpr int kMaxModules = 8;
constexpr int kMaxModuleFrames = 64;
struct ModuleFrameGroup {
    char path[MAX_PATH];
    DWORD64 loadedBase;
    DWORD64 pcs[kMaxModuleFrames];
    int frameCount;
};
ModuleFrameGroup g_moduleGroups[kMaxModules];
int g_moduleGroupCount = 0;

DWORD g_lastCopyError = 0;

void writeStackTrace(HANDLE log, CONTEXT* context) {
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);

    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    // Reset the collections for this walk.
    g_ownFrameCount = 0;
    g_moduleGroupCount = 0;

    constexpr int kMaxDepth = 64;
    char line[1024];
    for (int depth = 0; depth < kMaxDepth; ++depth) {
        if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, context, nullptr, SymFunctionTableAccess64,
                        SymGetModuleBase64, nullptr) == FALSE) {
            break;
        }
        const DWORD64 pc = frame.AddrPC.Offset;
        if (pc == 0) {
            break;
        }

        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(pc), &module);
        char moduleFile[MAX_PATH] = {0};
        if (module != nullptr) {
            GetModuleFileNameA(module, moduleFile, MAX_PATH);
        }
        const char* moduleName = moduleFile;
        if (const char* slash = std::strrchr(moduleFile, '\\')) {
            moduleName = slash + 1;
        }
        const DWORD64 loadedBase = reinterpret_cast<DWORD64>(module);
        // Only our exe gets a file address; a DLL frame prints module+offset
        // and relies on dbghelp's export lookup.
        const bool isOwnExe = loadedBase != 0 && loadedBase == g_exeLoadedBase;
        const DWORD64 fileAddress = isOwnExe ? g_preferredImageBase + (pc - loadedBase) : 0;

        // DLL frames get a name from dbghelp; our own come back nameless.
        char symbolBuffer[sizeof(SYMBOL_INFO) + 512] = {0};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 500;
        DWORD64 displacement = 0;
        const char* symbolName = SymFromAddr(process, pc, &displacement, symbol) != FALSE ? symbol->Name : "-";

        if (isOwnExe) {
            std::snprintf(line, sizeof(line), "  #%02d  %s+0x%llx  addr2line=0x%llx\n", depth, moduleName,
                          static_cast<unsigned long long>(pc - loadedBase),
                          static_cast<unsigned long long>(fileAddress));
        } else {
            std::snprintf(line, sizeof(line), "  #%02d  %s+0x%llx  (%s)\n", depth, moduleName,
                          static_cast<unsigned long long>(loadedBase != 0 ? pc - loadedBase : 0), symbolName);
        }
        writeAll(log, line);

        if (isOwnExe) {
            if (fileAddress != 0 && g_ownFrameCount < kMaxOwnFrames) {
                g_ownFrames[g_ownFrameCount++] = fileAddress;
            }
        } else if (loadedBase != 0 && moduleFile[0] != '\0') {
            // Group under its module (by loaded base).
            ModuleFrameGroup* group = nullptr;
            for (int g = 0; g < g_moduleGroupCount; ++g) {
                if (g_moduleGroups[g].loadedBase == loadedBase) {
                    group = &g_moduleGroups[g];
                    break;
                }
            }
            if (group == nullptr && g_moduleGroupCount < kMaxModules) {
                group = &g_moduleGroups[g_moduleGroupCount++];
                std::snprintf(group->path, sizeof(group->path), "%s", moduleFile);
                group->loadedBase = loadedBase;
                group->frameCount = 0;
            }
            if (group != nullptr && group->frameCount < kMaxModuleFrames) {
                group->pcs[group->frameCount++] = pc;
            }
        }
    }
}

// A module's debug-info sibling: ".../Qt6Widgets.dll" -> ".../Qt6Widgets.debug".
// The dot search starts after the last path separator, so a '.' in a directory
// name cannot truncate the path.
bool hasDebugSibling(const char* modulePath, char* debugPathOut, size_t outSize) {
    std::snprintf(debugPathOut, outSize, "%s", modulePath);
    const char* lastSlash = nullptr;
    for (const char* p = debugPathOut; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') {
            lastSlash = p;
        }
    }
    char* dot = std::strrchr(debugPathOut, '.');
    if (dot == nullptr || (lastSlash != nullptr && dot < lastSlash)) {
        return false;
    }
    const size_t prefixLen = static_cast<size_t>(dot - debugPathOut);
    if (prefixLen + 7 >= outSize) {  // ".debug" + NUL
        return false;
    }
    std::snprintf(dot, outSize - prefixLen, ".debug");
    const DWORD attrs = GetFileAttributesA(debugPathOut);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

constexpr int kKeepSnapshots = 3;
constexpr int kMaxSnapshotScan = 64;

// Snapshot names (zero-padded YYYYMMDD-HHMMSS) sort lexically == chronologically,
// so keeping the newest kKeepSnapshots is a plain string sort.
void pruneOldSnapshots() {
    if (g_logDir[0] == '\0') {
        return;
    }
    char pattern[MAX_PATH];
    std::snprintf(pattern, sizeof(pattern), "%s\\sd-crash-*.exe", g_logDir);

    static char names[kMaxSnapshotScan][MAX_PATH];
    int count = 0;
    WIN32_FIND_DATAA findData;
    HANDLE find = FindFirstFileA(pattern, &findData);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && count < kMaxSnapshotScan) {
                std::snprintf(names[count], MAX_PATH, "%s", findData.cFileName);
                ++count;
            }
        } while (FindNextFileA(find, &findData) != FALSE);
        FindClose(find);
    }

    // Insertion sort, oldest first; count is capped at kMaxSnapshotScan.
    for (int i = 1; i < count; ++i) {
        char key[MAX_PATH];
        std::snprintf(key, sizeof(key), "%s", names[i]);
        int j = i - 1;
        while (j >= 0 && std::strcmp(names[j], key) > 0) {
            std::snprintf(names[j + 1], MAX_PATH, "%s", names[j]);
            --j;
        }
        std::snprintf(names[j + 1], MAX_PATH, "%s", key);
    }

    for (int i = 0; i < count - kKeepSnapshots; ++i) {
        char fullPath[MAX_PATH];
        std::snprintf(fullPath, sizeof(fullPath), "%s\\%s", g_logDir, names[i]);
        DeleteFileA(fullPath);
    }
}

// A byte copy, never a hard link: the linker rewrites the exe in place on the
// next build, and a link would follow that rewrite.
bool copyExeSnapshot(char* outPath, size_t outSize) {
    if (g_logDir[0] == '\0') {
        outPath[0] = '\0';
        return false;
    }
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char name[96];
    std::snprintf(name, sizeof(name), "sd-crash-%04d%02d%02d-%02d%02d%02d-%lu.exe", now.wYear, now.wMonth, now.wDay,
                  now.wHour, now.wMinute, now.wSecond, static_cast<unsigned long>(GetCurrentProcessId()));
    std::snprintf(outPath, outSize, "%s/%s", g_logDir, name);
    if (CopyFileA(g_exePath, outPath, FALSE) == FALSE) {
        g_lastCopyError = GetLastError();
        outPath[0] = '\0';
        return false;
    }
    pruneOldSnapshots();
    return true;
}

// Emitted after the walk and the exe-copy attempt, on the frames the walk
// collected. allowExeCopy is false on the STACK_OVERFLOW path: the 64KB
// SetThreadStackGuarantee slice is reserved for the walk, not for file calls.
void writeSymbolizeCommands(HANDLE log, bool allowExeCopy) {
    if (g_ownFrameCount > 0) {
        char snapshotPath[MAX_PATH] = {0};
        bool haveSnapshot = false;
        if (allowExeCopy) {
            haveSnapshot = copyExeSnapshot(snapshotPath, sizeof(snapshotPath));
        } else {
            writeAll(log, "  exe snapshot copy skipped (stack-overflow path -- reserved handler stack only)\n");
        }

        const char* exeForSymbols = haveSnapshot ? snapshotPath : g_exePath;
        writeAll(log, "  symbolize (paste into a shell):\n    addr2line -e ");
        writeAll(log, exeForSymbols);
        writeAll(log, " -f -C -p");
        char line[64];
        for (int i = 0; i < g_ownFrameCount; ++i) {
            std::snprintf(line, sizeof(line), " 0x%llx", static_cast<unsigned long long>(g_ownFrames[i]));
            writeAll(log, line);
        }
        writeAll(log, "\n");

        if (allowExeCopy && !haveSnapshot) {
            char note[192];
            std::snprintf(note, sizeof(note),
                          "  (exe snapshot copy failed, GetLastError=%lu -- the line above points at the LIVE exe "
                          "and stops resolving after the next build)\n",
                          static_cast<unsigned long>(g_lastCopyError));
            writeAll(log, note);
        }
    }

    for (int g = 0; g < g_moduleGroupCount; ++g) {
        const ModuleFrameGroup& group = g_moduleGroups[g];
        if (group.frameCount == 0) {
            continue;
        }
        char debugPath[MAX_PATH];
        if (!hasDebugSibling(group.path, debugPath, sizeof(debugPath))) {
            continue;
        }
        const DWORD64 preferredBase = readPreferredImageBaseFromDisk(group.path);
        if (preferredBase == 0) {
            continue;
        }
        writeAll(log, "  symbolize Qt frames (paste into a shell):\n    addr2line -e ");
        writeAll(log, group.path);
        writeAll(log, " -f -C -i -p");
        char line[64];
        for (int i = 0; i < group.frameCount; ++i) {
            const DWORD64 fileAddress = preferredBase + (group.pcs[i] - group.loadedBase);
            std::snprintf(line, sizeof(line), " 0x%llx", static_cast<unsigned long long>(fileAddress));
            writeAll(log, line);
        }
        writeAll(log, "\n");
    }
}

void writeHeader(HANDLE log, const char* reason, const void* address) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    char line[512];
    std::snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [FTL] sd.crash: %s at 0x%llx (thread %lu)\n", now.wHour,
                  now.wMinute, now.wSecond, now.wMilliseconds, reason,
                  static_cast<unsigned long long>(reinterpret_cast<DWORD64>(address)), GetCurrentThreadId());
    writeAll(log, line);
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* pointers) {
    HANDLE log = openLog();
    char reason[128];
    const DWORD code = pointers->ExceptionRecord->ExceptionCode;
    std::snprintf(reason, sizeof(reason), "%s (0x%08lx)", exceptionName(code), code);
    writeHeader(log, reason, pointers->ExceptionRecord->ExceptionAddress);
    // StackWalk64 mutates the context it walks, so it gets a copy.
    CONTEXT context = *pointers->ContextRecord;
    writeStackTrace(log, &context);
    writeSymbolizeCommands(log, code != EXCEPTION_STACK_OVERFLOW);
    if (log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(log);
        CloseHandle(log);
    }
    // The process still dies; this only records the crash.
    return EXCEPTION_EXECUTE_HANDLER;
}

// abort() and an escaped exception never reach the SEH filter, so both walk a
// context captured in this frame; the trace continues into the caller.
void writeTraceFromHere(const char* reason) {
    HANDLE log = openLog();
    writeHeader(log, reason, nullptr);
    CONTEXT context = {};
    RtlCaptureContext(&context);
    writeStackTrace(log, &context);
    writeSymbolizeCommands(log, true);
    if (log != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(log);
        CloseHandle(log);
    }
}

void abortHandler(int) {
    writeTraceFromHere("SIGABRT");
    _exit(3);
}

void terminateHandler() {
    const char* what = "std::terminate";
    if (std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& error) {
            static char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "std::terminate (uncaught %s)", error.what());
            what = buffer;
        } catch (...) {
            what = "std::terminate (uncaught non-std exception)";
        }
    }
    writeTraceFromHere(what);
    _exit(3);
}

}  // namespace

void installCrashHandler(const QString& logPath) {
    const QByteArray localPath = logPath.toLocal8Bit();
    std::snprintf(g_logPath, sizeof(g_logPath), "%s", localPath.constData());
    // Split off the log's directory; the file sink's path uses forward
    // slashes, so both separators are checked.
    std::snprintf(g_logDir, sizeof(g_logDir), "%s", g_logPath);
    char* lastSlash = nullptr;
    for (char* p = g_logDir; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') {
            lastSlash = p;
        }
    }
    if (lastSlash != nullptr) {
        *lastSlash = '\0';
    } else {
        g_logDir[0] = '\0';
    }

    GetModuleFileNameA(nullptr, g_exePath, MAX_PATH);
    g_exeLoadedBase = reinterpret_cast<DWORD64>(GetModuleHandleA(nullptr));
    g_preferredImageBase = readPreferredImageBaseFromDisk(g_exePath);

    // Reserve stack for the handler itself: after a STACK_OVERFLOW the filter
    // has almost no stack left and the walk dies frameless.
    ULONG guaranteedBytes = 64 * 1024;
    SetThreadStackGuarantee(&guaranteedBytes);

    SetUnhandledExceptionFilter(unhandledExceptionFilter);
    std::signal(SIGABRT, abortHandler);
    std::set_terminate(terminateHandler);
}

// Three named frames: a trace resolving to exactly this chain shows the walk,
// the ASLR correction and the DWARF lookup line up.
[[noreturn]] void crashSelfTestInner(int* target) {
    *target = 42;  // deliberate null dereference -- the whole point of the flag
    std::abort();  // unreachable; keeps the [[noreturn]] contract honest
}

[[noreturn]] void crashSelfTestMiddle(int* target) { crashSelfTestInner(target); }

[[noreturn]] void crashSelfTest() { crashSelfTestMiddle(nullptr); }

// Runaway recursion; volatile padding keeps the frame real so the optimizer
// cannot turn this into a loop.
int crashSelfTestRecurse(int depth) {
    volatile char padding[512];
    padding[0] = static_cast<char>(depth);
    return padding[0] + crashSelfTestRecurse(depth + 1);
}

[[noreturn]] void crashSelfTestStackOverflow() {
    crashSelfTestRecurse(0);
    std::abort();  // unreachable
}

}  // namespace app
