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

#include "terminationsignalhandler.h"

#if defined(Q_OS_UNIX)

#include <qdebug.h>
#include <qsocketnotifier.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// The only state the raw handler may touch: a sig_atomic_t slot and
// the write end of the bounce socketpair (async-signal-safety).
int s_signalFd[2] = {-1, -1};
volatile sig_atomic_t s_pendingSignal = 0;

// The event loop gets this long to run the flush before the watchdog
// _exit()s anyway — a wedged loop cannot stall a termination request.
const unsigned int kFlushDeadlineSeconds = 8;

extern "C" void terminationHandler(int signalNumber)
{
    s_pendingSignal = signalNumber;
    const unsigned char byte =
        static_cast<unsigned char>(signalNumber & 0xff);
    if (s_signalFd[1] >= 0) {
        const ssize_t written = ::write(s_signalFd[1], &byte, 1);
        (void)written;  // a full buffer still leaves the alarm armed
    }
    ::alarm(kFlushDeadlineSeconds);
}

extern "C" void terminationWatchdog(int)
{
    // A wedged event loop never ran the flush — die by the pending
    // signal anyway so the parent still sees WIFSIGNALED.
    const int signalNumber = s_pendingSignal ? int(s_pendingSignal)
                                             : SIGTERM;
    struct sigaction action;
    ::sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    ::sigaction(signalNumber, &action, nullptr);
    ::kill(::getpid(), signalNumber);
    ::_exit(128 + signalNumber);
}

void installHandler(int signalNumber, void (*handler)(int))
{
    struct sigaction action;
    ::sigemptyset(&action.sa_mask);
    action.sa_handler = handler;
    action.sa_flags = 0;
    ::sigaction(signalNumber, &action, nullptr);
}

} // namespace

TerminationSignalHandler::TerminationSignalHandler(QObject *parent)
    : QObject(parent)
{
    if (s_signalFd[0] >= 0) {
        qWarning("TerminationSignalHandler: already installed");
        return;
    }
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, s_signalFd) != 0) {
        qWarning("TerminationSignalHandler: socketpair failed: %s",
                 ::strerror(errno));
        s_signalFd[0] = s_signalFd[1] = -1;
        return;
    }
    // SOCK_NONBLOCK/SOCK_CLOEXEC in the socket type is Linux-only —
    // fcntl is the portable spelling for both flags.
    for (int i = 0; i < 2; ++i) {
        ::fcntl(s_signalFd[i], F_SETFL, O_NONBLOCK);
        ::fcntl(s_signalFd[i], F_SETFD, FD_CLOEXEC);
    }

    installHandler(SIGTERM, terminationHandler);
    installHandler(SIGINT, terminationHandler);
    installHandler(SIGHUP, terminationHandler);
    installHandler(SIGALRM, terminationWatchdog);

    QSocketNotifier *notifier =
        new QSocketNotifier(s_signalFd[0], QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated,
            this, &TerminationSignalHandler::drainAndExit);
    m_installed = true;
}

TerminationSignalHandler::~TerminationSignalHandler()
{
    if (!m_installed)
        return;
    // Restoring the defaults lets a late signal during teardown die
    // instantly instead of flushing mid-destruction.
    ::signal(SIGTERM, SIG_DFL);
    ::signal(SIGINT, SIG_DFL);
    ::signal(SIGHUP, SIG_DFL);
    ::signal(SIGALRM, SIG_DFL);
    ::close(s_signalFd[0]);
    ::close(s_signalFd[1]);
    s_signalFd[0] = s_signalFd[1] = -1;
    s_pendingSignal = 0;
}

void TerminationSignalHandler::drainAndExit()
{
    // The first queued signal decides — the process dies before a
    // second read would matter.
    unsigned char bytes[64];
    int signalNumber = 0;
    ssize_t count;
    while ((count = ::read(s_signalFd[0], bytes, sizeof(bytes))) > 0) {
        for (ssize_t i = 0; i < count; ++i) {
            if (bytes[i] && !signalNumber)
                signalNumber = bytes[i];
        }
    }
    if (!signalNumber)
        signalNumber = s_pendingSignal ? int(s_pendingSignal) : SIGTERM;

    // Slots run synchronously on this thread — the flush must finish
    // before the process dies on the next line.
    emit terminateRequested(signalNumber);

    // Die by the same signal so a waiting parent sees WIFSIGNALED and
    // the shell reports e.g. 143, not a clean exit code.
    ::signal(signalNumber, SIG_DFL);
    ::kill(::getpid(), signalNumber);
    ::_exit(128 + signalNumber);
}

#else // !Q_OS_UNIX — see the class comment; WIN01 supplies the
      // SetConsoleCtrlHandler equivalent when the port lands.

TerminationSignalHandler::TerminationSignalHandler(QObject *parent)
    : QObject(parent)
{
}

TerminationSignalHandler::~TerminationSignalHandler()
{
}

void TerminationSignalHandler::drainAndExit()
{
}

#endif // Q_OS_UNIX
