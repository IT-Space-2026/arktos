/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#include "extensionidlewatcher.h"

#include "extensionbackgroundhost.h"

#include <qapplication.h>
#include <qevent.h>
#include <qjsonarray.h>
#include <qjsonobject.h>
#include <qtimer.h>

ExtensionIdleWatcher *ExtensionIdleWatcher::instance()
{
    static ExtensionIdleWatcher *watcher = new ExtensionIdleWatcher;
    return watcher;
}

ExtensionIdleWatcher::ExtensionIdleWatcher(QObject *parent)
    : QObject(parent)
    , m_state(QStringLiteral("active"))
{
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    // A coarse timer may fire up to 5% early — the declared state
    // would race ahead of the elapsed clock queryState() measures,
    // answering 'active' right after an 'idle' transition.  Precise
    // timing keeps the state machine honest against the clock.
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        if (m_lastInput.elapsed() >= qint64(m_intervalSeconds) * 1000)
            transition(QStringLiteral("idle"));
        else
            rearm();   // fired early — reschedule the real boundary
    });
}

int ExtensionIdleWatcher::minIntervalSeconds()
{
    // Chromium clamps both queryState's threshold and
    // setDetectionInterval's argument at a 15-second floor.  The env
    // override exists so the autotest leg can watch a real
    // transition without sleeping past it — production keeps 15.
    static const int floor =
        qEnvironmentVariableIsSet("ARKTOS_EXT_IDLE_MIN_S")
        ? qMax(0, qEnvironmentVariableIntValue("ARKTOS_EXT_IDLE_MIN_S"))
        : 15;
    return floor;
}

void ExtensionIdleWatcher::arm()
{
    if (m_armed)
        return;
    m_armed = true;
    m_lastInput.start();
    // A filter on the application object sees every event Qt
    // delivers (sendThroughApplicationEventFilters) — including the
    // synthetic ones QTest posts, which is what the test leg drives.
    if (qApp)
        qApp->installEventFilter(this);
    rearm();
}

void ExtensionIdleWatcher::subscribe()
{
    arm();
}

QString ExtensionIdleWatcher::queryState(int detectionIntervalSeconds)
{
    arm();
    const int threshold =
        qMax(detectionIntervalSeconds, minIntervalSeconds());
    // 'locked' is never answered — there is no honest source for it.
    return m_lastInput.elapsed() >= qint64(threshold) * 1000
        ? QStringLiteral("idle") : QStringLiteral("active");
}

void ExtensionIdleWatcher::setDetectionInterval(int intervalSeconds)
{
    arm();
    m_intervalSeconds = qMax(intervalSeconds, minIntervalSeconds());
    rearm();
}

qint64 ExtensionIdleWatcher::idleMs() const
{
    return m_lastInput.isValid() ? m_lastInput.elapsed() : 0;
}

void ExtensionIdleWatcher::inputActivity()
{
    if (!m_armed)
        return;
    m_lastInput.start();
    if (m_state == QLatin1String("idle"))
        transition(QStringLiteral("active"));
    else
        rearm();
}

bool ExtensionIdleWatcher::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);
    if (!m_armed)
        return false;
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::HoverMove:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TabletMove:
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
        inputActivity();
        break;
    default:
        break;
    }
    return false;
}

void ExtensionIdleWatcher::rearm()
{
    if (!m_armed || m_state != QLatin1String("active"))
        return;
    const qint64 remaining =
        qint64(m_intervalSeconds) * 1000 - m_lastInput.elapsed();
    m_timer->start(int(qMax<qint64>(0, remaining)));
}

void ExtensionIdleWatcher::transition(const QString &state)
{
    if (m_state == state)
        return;
    m_state = state;
    // Chrome-shaped dispatch: the listener's single argument is the
    // bare state string — the __args envelope carries it as one arg.
    const QJsonObject payload{
        {QStringLiteral("__args"), QJsonArray{state}}};
    ExtensionBackgroundRuntime::instance()->broadcastEvent(
        QStringLiteral("idle.onStateChanged"), payload);
    rearm();
}
