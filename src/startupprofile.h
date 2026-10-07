/*
 * Copyright (c) 2026, The Arora Authors
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

#ifndef STARTUPPROFILE_H
#define STARTUPPROFILE_H

#include <qelapsedtimer.h>
#include <qdebug.h>
#include <qstring.h>

// PERF03: opt-in startup / menu-population timing.  Enabled by the
// --profile-startup command-line flag (see main.cpp) or the
// ARORA_PROFILE_STARTUP=1 environment variable.  Every mark() prints
// the milliseconds elapsed since start() plus a stage label, so a
// cold-start log reads directly as a timeline; Scope additionally
// reports the wall time spent inside a single function.  All output
// goes to stderr via qInfo so it is trivially greppable and carries
// no cost unless enabled (the enabled() check is the only work done
// per call when off).
namespace StartupProfile {

inline bool &flag()
{
    static bool enabled = qEnvironmentVariableIsSet("ARORA_PROFILE_STARTUP");
    return enabled;
}

inline bool enabled()
{
    return flag();
}

inline void enable()
{
    flag() = true;
}

inline QElapsedTimer &timer()
{
    static QElapsedTimer timer;
    if (!timer.isValid())
        timer.start();
    return timer;
}

inline void start()
{
    timer().start();
}

inline void mark(const QString &stage)
{
    if (!enabled())
        return;
    qInfo().noquote() << QStringLiteral("profile-startup: %1 ms  %2")
        .arg(timer().elapsed(), 6).arg(stage);
}

inline void mark(const char *stage)
{
    mark(QString::fromLatin1(stage));
}

// RAII helper: logs "<stage> (+<block> ms)" on destruction.
class Scope
{
public:
    explicit Scope(const char *stage)
        : m_stage(stage)
    {
        if (enabled())
            m_timer.start();
    }

    ~Scope()
    {
        if (m_timer.isValid())
            mark(QString::fromLatin1("%1 (+%2 ms)")
                     .arg(QLatin1String(m_stage)).arg(m_timer.elapsed()));
    }

private:
    const char *m_stage;
    QElapsedTimer m_timer;
};

} // namespace StartupProfile

#endif // STARTUPPROFILE_H
