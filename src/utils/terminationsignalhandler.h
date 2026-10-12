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

#ifndef TERMINATIONSIGNALHANDLER_H
#define TERMINATIONSIGNALHANDLER_H

#include <qobject.h>

/*
    SESS02 — session durability on termination signals.

    POSIX signal handlers cannot run Qt code (not async-signal-safe),
    so the raw handler only writes the signal number into a socketpair
    and arms a watchdog alarm.  A QSocketNotifier on the read end
    bounces the request onto the event loop, where terminateRequested
    fires and connected slots flush pending state synchronously; the
    process then re-raises the signal so a waiting parent observes the
    real cause of death (WIFSIGNALED — e.g. shell status 143 for
    SIGTERM).  If the event loop is wedged and the notifier never
    runs, the SIGALRM watchdog _exit()s on the deadline so the
    application cannot stall a termination request indefinitely.

    Windows has no POSIX signals — the WIN01 port's equivalent is a
    SetConsoleCtrlHandler callback; on non-UNIX builds this class
    compiles to an inert stub and terminateRequested never fires.
*/
class TerminationSignalHandler : public QObject
{
    Q_OBJECT

public:
    explicit TerminationSignalHandler(QObject *parent = nullptr);
    ~TerminationSignalHandler();

signals:
    void terminateRequested(int signalNumber);

private slots:
    void drainAndExit();

private:
    // Only the instance that installed the handlers may disarm them.
    bool m_installed = false;

};

#endif // TERMINATIONSIGNALHANDLER_H
