#pragma once

// Last-resort crash handler: appends a stack trace to the file-sink log before
// the process dies. Frames are logged as module + file virtual address plus a
// ready-made addr2line command, not names: dbghelp resolves only export tables
// and MinGW emits DWARF. Requires debug info (-g); without it addr2line prints "??".

class QString;

namespace app {

// Installs the unhandled-exception filter, a std::terminate handler, and a
// SIGABRT handler. Call as early in main() as possible -- before QApplication,
// so a crash during construction is covered too. `logPath` is the file the
// trace is appended to (the file sink's own path); stderr if it cannot be opened.
void installCrashHandler(const QString& logPath);

// Crashes on purpose through three named frames (`--crash-selftest`) to check
// that the trace resolves.
[[noreturn]] void crashSelfTest();

// Stack-overflow twin (`--crash-selftest-stackoverflow`): checks that the
// reserved handler stack (SetThreadStackGuarantee) suffices to walk a blown stack.
[[noreturn]] void crashSelfTestStackOverflow();

}  // namespace app
