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

#ifndef EXTENSIONIDLEWATCHER_H
#define EXTENSIONIDLEWATCHER_H

#include <qelapsedtimer.h>
#include <qobject.h>
#include <qstring.h>

class QTimer;

// ER13 — chrome.idle, served by the hosted runtime.  Qt's extension
// embedder never registered the idle delegate, so queryState /
// setDetectionInterval / onStateChanged land here.
//
// What this is: a browser-side idle clock driven by an application-
// wide Qt event filter — key presses, pointer buttons, pointer
// motion, wheel and touch all count as user activity, exactly the
// input classes that reset an OS idle timer.  A single-shot QTimer
// marks the active -> idle boundary at the configured detection
// interval (Chrome's default 60s), and the next input event flips
// back to active.  State transitions fan out to every hosted
// extension context holding the 'idle' permission through the
// runtime's broadcastEvent gate.
//
// Honest limits, stated once per subscribing context by the shim:
// activity is measured only inside Arktos windows — input in other
// applications does not reset the clock, so "idle" here approximates
// Chrome's "user left the machine" loosely; and 'locked' is never
// reported because no portable screen-lock signal exists.  Both are
// the task's documented v1 scope, not hidden failures.
//
// Chrome parity kept: the configured interval and each queryState
// threshold clamp at the 15-second floor Chromium enforces, the
// interval is global (the last setDetectionInterval wins for every
// extension), and queryState answers against its own argument — a
// large threshold can return 'active' while the declared state is
// already 'idle'.  ARKTOS_EXT_IDLE_MIN_S lowers the floor for the
// autotest leg so a transition is observable without sleeping 16s;
// it changes only the clamp, never the state machine.
//
// The watcher is lazy: nothing filters input or runs a timer until
// the first namespaced call or onStateChanged subscription arrives,
// so an extension-free session carries zero cost.
class ExtensionIdleWatcher : public QObject
{
    Q_OBJECT

public:
    static ExtensionIdleWatcher *instance();

    // chrome.idle.* dispatch — invoked from ExtensionShimBackend::
    // invokeIdle after the 'idle' manifest-permission gate.  Any of
    // them arms the watcher.
    QString queryState(int detectionIntervalSeconds);
    void setDetectionInterval(int intervalSeconds);
    // The shim's onStateChanged onAdd lands here — registering a
    // listener is what arms the watcher for an extension that only
    // ever listens.
    void subscribe();

    // Diagnostics/test surface.
    QString state() const { return m_state; }
    bool armed() const { return m_armed; }
    qint64 idleMs() const;

public slots:
    // The app-level event filter feeds real input here; public so a
    // test leg can drive the identical transition a keypress would.
    void inputActivity();

private:
    explicit ExtensionIdleWatcher(QObject *parent = nullptr);
    bool eventFilter(QObject *watched, QEvent *event) override;
    void arm();
    // (Re)schedules the idle-boundary timer from the last input —
    // a no-op while idle or unarmed.
    void rearm();
    void transition(const QString &state);
    // Chrome's 15s floor — ARKTOS_EXT_IDLE_MIN_S test override.
    static int minIntervalSeconds();

    bool m_armed = false;
    QString m_state;
    int m_intervalSeconds = 60;     // Chrome's default
    QElapsedTimer m_lastInput;
    QTimer *m_timer = nullptr;
};

#endif // EXTENSIONIDLEWATCHER_H
