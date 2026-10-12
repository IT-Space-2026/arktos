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

#include "crashreporter.h"

#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>

#if defined(Q_OS_UNIX)

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cstdlib>
#include <exception>

namespace {

// Everything the raw handler touches lives in POD storage it can
// read without locking.
char s_logPath[4096] = {0};

const char *signalName(int signalNumber)
{
    switch (signalNumber) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    case SIGABRT:
        return "SIGABRT";
    case SIGTRAP:
        return "SIGTRAP";
    default:
        return "signal";
    }
}

void writeAll(int fd, const char *data, size_t length)
{
    while (length > 0) {
        const ssize_t written = ::write(fd, data, length);
        if (written <= 0)
            return;
        data += written;
        length -= size_t(written);
    }
}

void dumpReport(int fd, const char *header, size_t headerLength)
{
    writeAll(fd, header, headerLength);
    void *frames[64];
    const int count = ::backtrace(frames, 64);
    if (count > 0)
        ::backtrace_symbols_fd(frames, count, fd);
    writeAll(fd, "\n", 1);
}

// Signal-context code: snprintf into a stack buffer (no allocation
// for the formats used), write() to stderr and the log fd, nothing
// else.  backtrace_symbols_fd may allocate on glibc — the unwinder
// is warmed up at install time so this path is hot.
void emitReport(int signalNumber, const char *reason, const void *addr)
{
    char header[768];
    int length = ::snprintf(
        header, sizeof(header),
        "ARORA CRASH pid=%d signal=%d (%s)%s%s addr=%p\n"
        "ARORA CRASH log: %s\n",
        int(::getpid()), signalNumber, signalName(signalNumber),
        reason ? " reason=" : "", reason ? reason : "", addr,
        s_logPath[0] ? s_logPath : "(unavailable)");
    if (length < 0)
        length = 0;
    if (size_t(length) >= sizeof(header))
        length = sizeof(header) - 1;

    dumpReport(STDERR_FILENO, header, size_t(length));

    if (s_logPath[0]) {
        const int fd = ::open(s_logPath,
                              O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            dumpReport(fd, header, size_t(length));
            ::close(fd);
        }
    }
}

extern "C" void crashSignalHandler(int signalNumber, siginfo_t *info,
                                   void *)
{
    emitReport(signalNumber, nullptr,
               info ? info->si_addr : nullptr);
    // SA_RESETHAND restored the default disposition — die by the same
    // signal so the parent observes WIFSIGNALED.
    ::kill(::getpid(), signalNumber);
    ::_exit(128 + signalNumber);
}

void terminateHandler()
{
    // what() points into the exception object, which dies with the
    // catch block — copy it into stack storage before reporting.
    char reason[512];
    ::strncpy(reason, "std::terminate", sizeof(reason) - 1);
    reason[sizeof(reason) - 1] = 0;
    if (std::current_exception()) {
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception &exception) {
            ::snprintf(reason, sizeof(reason), "std::terminate: %s",
                       exception.what());
        } catch (...) {
            ::strncpy(reason, "non-std exception",
                      sizeof(reason) - 1);
        }
    }
    emitReport(SIGABRT, reason, nullptr);
    // Die by a real SIGABRT (WIFSIGNALED for the parent) without
    // re-entering the armed handler — a second emitReport would
    // truncate the log over this one.
    ::signal(SIGABRT, SIG_DFL);
    ::abort();
}

void installOne(int signalNumber)
{
    struct sigaction action;
    ::memset(&action, 0, sizeof(action));
    ::sigemptyset(&action.sa_mask);
    action.sa_sigaction = &crashSignalHandler;
    // SA_RESETHAND disarms on the way in — a fault inside the handler
    // dies immediately instead of recursing; SA_NODEFER keeps the
    // re-raised delivery unblocked.
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER;
    ::sigaction(signalNumber, &action, nullptr);
}

} // namespace

QString CrashReporter::install(const QString &logFilePath)
{
    QString path = QFile::decodeName(qgetenv("ARORA_CRASH_LOG"));
    if (path.isEmpty())
        path = logFilePath;
    const QString directory =
        QFileInfo(path).absoluteDir().absolutePath();
    QDir().mkpath(directory);

    const QByteArray encoded = QFile::encodeName(path);
    ::strncpy(s_logPath, encoded.constData(), sizeof(s_logPath) - 1);
    s_logPath[sizeof(s_logPath) - 1] = 0;

    // Warm the unwinder: the first backtrace() call initializes
    // libgcc internals via malloc — inside a signal handler that
    // allocation could re-enter a lock the crashed thread held.
    // Doing it now makes the crash-time call allocation-free on
    // glibc.
    void *warmup[8];
    (void)::backtrace(warmup, 8);

    const int fatalSignals[] = {SIGSEGV, SIGBUS, SIGILL,
                                SIGFPE, SIGABRT, SIGTRAP};
    for (int signalNumber : fatalSignals)
        installOne(signalNumber);

    std::set_terminate(&terminateHandler);
    return path;
}

QString CrashReporter::logPath()
{
    return QString::fromUtf8(s_logPath);
}

#else // !Q_OS_UNIX — the WIN01 port supplies its own crash-dump
      // equivalent; install is a no-op elsewhere.

QString CrashReporter::install(const QString &)
{
    return QString();
}

QString CrashReporter::logPath()
{
    return QString();
}

#endif // Q_OS_UNIX
