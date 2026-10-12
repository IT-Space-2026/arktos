/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef CRASHREPORTER_H
#define CRASHREPORTER_H

#include <qstring.h>

/*
    CRASH02 — minimal crash reporter.

    A silent exit leaves no kernel trap line, no coredump and no Qt
    output, so a one-shot failure (the failed-print death) is
    undiagnosable after the fact.  install() registers handlers for
    the fatal signals (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT/SIGTRAP —
    the last covers Chromium's int3 CHECKs) plus a std::terminate
    hook; the handler writes a marker line and a backtrace to stderr
    AND to the crash log, then re-raises so a waiting parent still
    observes WIFSIGNALED.

    The handler is deliberately small: a marker line via write(),
    backtrace() + backtrace_symbols_fd() (the unwinder is warmed up
    at install time so the crash-time call does not take its
    initializing malloc path), then re-raise.  Handlers are
    installed with SA_RESETHAND|SA_NODEFER so a fault inside the
    handler itself still dies by the original signal instead of
    looping.

    Only the application process is covered — exec()d children
    (the WebEngine helper process, the download worker, the tor
    daemon) get default dispositions and their own diagnostics.
*/
namespace CrashReporter {

// Installs the handlers.  logFilePath is where the marker +
// backtrace land (its parent directory is created); the
// ARORA_CRASH_LOG environment variable overrides the path entirely
// (test/support hook).  Returns the resolved log path, or an empty
// string on platforms without POSIX signal support.
QString install(const QString &logFilePath);

// The path reports are written to (empty before install()).
QString logPath();

} // namespace CrashReporter

#endif // CRASHREPORTER_H
